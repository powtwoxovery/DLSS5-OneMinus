#include "DLSS5OneMinusViewExtension.h"

#include "DLSS5OneMinusColorPass.h"
#include "DLSS5OneMinusInternal.h"
#include "DLSS5OneMinusRuntimeD3D12.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "HAL/IConsoleManager.h"
#include "LegacyScreenPercentageDriver.h"
#include "PostProcess/PostProcessInputs.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBlackboard.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Math/TranslationMatrix.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "TemporalUpscaler.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusOffline, Log, All);

struct FDLSS5OneMinusFrameGuides
{
    FDLSS5OneMinusFrameGuides(
        const FSceneView* InView,
        FRDGTextureRef InDepth,
        FRDGTextureRef InMotion,
        const FIntRect& InGuideRect,
        bool bInReset,
        uint64 InViewId,
        const FDLSS5OneMinusRenderSettings& InSettings)
        : View(InView)
        , Depth(InDepth)
        , Motion(InMotion)
        , GuideRect(InGuideRect)
        , bReset(bInReset)
        , ViewId(InViewId)
        , Settings(InSettings)
    {
    }

    const FSceneView* View = nullptr;
    FRDGTextureRef Depth = nullptr;
    FRDGTextureRef Motion = nullptr;
    FIntRect GuideRect;
    bool bReset = false;
    uint64 ViewId = 0;
    FDLSS5OneMinusRenderSettings Settings;
};

RDG_REGISTER_BLACKBOARD_STRUCT(FDLSS5OneMinusFrameGuides);

namespace
{
    TAutoConsoleVariable<int32> CVarOfflineTemporalFix(
        TEXT("r.NGX.DLSSNR.OfflineTemporalFix"), 2,
        TEXT("Offline Path Tracer: 0 legacy NR on every sample; 1 completed samples only; "
             "2 completed samples and NFOR-aligned motion guides (default). NR stays enabled."),
        ECVF_RenderThreadSafe);

    int32 ReadInt(const TCHAR* Name, int32 DefaultValue)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetInt() : DefaultValue;
    }

    float ReadFloat(const TCHAR* Name, float DefaultValue)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetFloat() : DefaultValue;
    }

    int32 GetNFORGuideDelay(const FSceneView& View)
    {
        // Both legacy MRQ and MRG set OverrideOutputFrameIndexValue. That
        // field does not identify the age of NFOR's delivered color/depth:
        // NFOR selects an older image from its own completed-image history.
        if (ReadInt(TEXT("r.PathTracing.SpatialDenoiser.Type"), 0) != 1)
        {
            return 0;
        }
        const int32 Denoiser = ReadInt(TEXT("r.PathTracing.Denoiser"), -1);
        if (Denoiser == 0 || (Denoiser < 0 && !View.FinalPostProcessSettings.PathTracingEnableDenoiser))
        {
            return 0;
        }
        const IConsoleVariable* Name = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.TemporalDenoiser.Name"));
        if (Name == nullptr || !Name->GetString().Equals(TEXT("NFOR"), ESearchCase::IgnoreCase))
        {
            return 0;
        }
        const int32 HalfWindow = FMath::Clamp(ReadInt(TEXT("r.NFOR.FrameCount"), 2), 0, 3);
        const int32 Index = ReadInt(TEXT("r.NFOR.DenoisingFrameIndex"), -1);
        return Index < 0 ? HalfWindow : FMath::Clamp(Index, 0, HalfWindow * 2);
    }

    FMatrix44f MakeClipToPrevious(const FViewMatrices& Current, const FViewMatrices& Previous)
    {
        // Match SceneView.cpp's jitter-free, large-world-safe reprojection,
        // using the cameras of the delivered images, not the newest PT sample.
        const FVector Delta = Previous.GetPreViewTranslation() - Current.GetPreViewTranslation();
        const FMatrix InvViewProj = Current.ComputeInvProjectionNoAAMatrix()
            * Current.GetTranslatedViewMatrix().GetTransposed();
        const FMatrix PrevViewProj = FTranslationMatrix(Delta)
            * Previous.GetTranslatedViewMatrix() * Previous.ComputeProjectionNoAAMatrix();
        return FMatrix44f(InvViewProj * PrevViewProj);
    }

    int32 ReadEditorScreenPercentage()
    {
        static const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
            TEXT("r.NGX.DLSSNR.EditorScreenPercentage"));
        return Variable != nullptr ? Variable->GetInt() : 0;
    }

    bool IsDLSSFamily(const FSceneViewFamily* ViewFamily)
    {
        if (ViewFamily == nullptr)
        {
            return false;
        }
        const auto* TemporalUpscaler = ViewFamily->GetTemporalUpscalerInterface();
        if (TemporalUpscaler == nullptr)
        {
            return false;
        }
        const TCHAR* DebugName = TemporalUpscaler->GetDebugName();
        return DebugName != nullptr
            && (FCString::Strcmp(DebugName, TEXT("FDLSSSceneViewFamilyUpscaler")) == 0
                || FCString::Strcmp(DebugName, TEXT("FDLSSRRSceneViewFamilyUpscaler")) == 0);
    }
}

// Gate the editor-viewport family setup and the sub-25% driver on this scene's effective settings,
// not the global Enable/Path CVars, which are shared by every world (editor, PIE, MRQ).
void FDLSS5OneMinusViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
    const uint64 TargetRenderTargetId = DLSS5OneMinus::GetTargetRenderTargetId();
    if (DLSS5OneMinus::GetRenderSettingsForScene(InViewFamily.Scene).bEnabled
        && TargetRenderTargetId != 0
        && reinterpret_cast<uint64>(InViewFamily.RenderTarget) == TargetRenderTargetId)
    {
        // A focused Slate tool can mark the Level Editor viewport family
        // non-realtime before temporal-upscaler selection. Restrict this
        // correction to the exact FViewport selected by our editor controls;
        // preview, capture, PIE, and MRQ render targets remain untouched.
        InViewFamily.bRealtimeUpdate = true;
        InViewFamily.EngineShowFlags.SetAntiAliasing(true);
        InViewFamily.EngineShowFlags.SetTemporalAA(true);
    }
}

void FDLSS5OneMinusViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
    const uint64 TargetRenderTargetId = DLSS5OneMinus::GetTargetRenderTargetId();
    const int32 RequestedScreenPercentage = ReadEditorScreenPercentage();
    const FDLSS5OneMinusRenderSettings SceneSettings = DLSS5OneMinus::GetRenderSettingsForScene(InViewFamily.Scene);
    if (SceneSettings.bEnabled
        && SceneSettings.Route == 1
        && TargetRenderTargetId != 0
        && reinterpret_cast<uint64>(InViewFamily.RenderTarget) == TargetRenderTargetId
        && RequestedScreenPercentage >= 10
        && RequestedScreenPercentage < 25
        && InViewFamily.SupportsScreenPercentage()
        && InViewFamily.GetScreenPercentageInterface() == nullptr)
    {
        // FEditorViewportClient clamps its normal preview field to TSR's 25%
        // floor. SetupView runs after the editor's alien-driver guard and
        // before its default FLegacyScreenPercentageDriver is installed, so
        // this exact-view replacement can safely request the engine-wide 1%
        // legacy minimum.
        InViewFamily.EngineShowFlags.ScreenPercentage = true;
        InViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
            InViewFamily,
            RequestedScreenPercentage / 100.0f));
    }
}

void FDLSS5OneMinusViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
    if (DLSS5OneMinus::GetRenderSettingsForScene(InViewFamily.Scene).bEnabled)
    {
        const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        if (Status.State == EDLSS5OneMinusRuntimeState::DependenciesReady
            && !Status.bNGXOwnershipProven)
        {
            DLSS5OneMinus::RunNGXSnippetBootstrap();
        }
    }
}

void FDLSS5OneMinusViewExtension::PrePostProcessPass_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const FPostProcessingInputs& Inputs)
{
    const FDLSS5OneMinusRenderSettings Settings = DLSS5OneMinus::GetRenderSettings(InView);
    const int32 Path = Settings.Route;
    const uint64 ViewId = reinterpret_cast<uint64>(InView.State);
    const uint64 TargetViewId = DLSS5OneMinus::GetTargetViewId();
    ++OfflineHistoryClock;
    // Release finished jobs' retained guides during subsequent viewport work.
    for (auto It = OfflineHistories.CreateIterator(); It; ++It)
    {
        if (OfflineHistoryClock - It.Value()->LastSeen > 4096)
        {
            It.RemoveCurrent();
        }
    }
    if (!Settings.bEnabled
        || Path < 0
        || Path > 2
        || !DLSS5OneMinus::IsNGXSnippetBootstrapReady()
        || InView.State == nullptr
        || (TargetViewId != 0 && ViewId != TargetViewId)
        || (InView.bIsSceneCapture && !Settings.bAllowSceneCaptures)
        || GraphBuilder.Blackboard.Get<FDLSS5OneMinusFrameGuides>() != nullptr)
    {
        return;
    }

    if ((Path == 0 || Path == 2) && !IsDLSSFamily(InView.Family))
    {
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.bOutputSubmitted = false;
        Status.bOutputCompleted = false;
        Status.EffectiveRoute = TEXT("Unavailable: DLSS does not own this view");
        Status.Detail = TEXT("The selected Pre DLSS/Post DLAA route requires the active view family to be owned by NVIDIA DLSS. The original scene color is preserved.");
        DLSS5OneMinus::SetStatus(Status);
        return;
    }

    Inputs.Validate();
    FRDGTextureRef Depth = (*Inputs.SceneTextures)->SceneDepthTexture;
    FRDGTextureRef Velocity = (*Inputs.SceneTextures)->GBufferVelocityTexture;
    const FIntRect ViewRect = static_cast<const FViewInfo&>(InView).ViewRect;
    if (Depth == nullptr || Velocity == nullptr || ViewRect.IsEmpty())
    {
        return;
    }

    bool bReset = InView.bCameraCut;
    FMatrix44f ClipToPrevious;
    const FMatrix44f* ClipToPreviousOverride = nullptr;
#if RHI_RAYTRACING
    const int32 OfflineFix = CVarOfflineTemporalFix.GetValueOnRenderThread();
    if (OfflineFix > 0 && InView.bIsOfflineRender && InView.Family
        && InView.Family->EngineShowFlags.PathTracing)
    {
        const uint32 Key = InView.State->GetViewKey();
        TUniquePtr<FOfflineViewHistory>& Entry = OfflineHistories.FindOrAdd(Key);
        if (!Entry)
        {
            Entry = MakeUnique<FOfflineViewHistory>();
        }
        FOfflineViewHistory& History = *Entry;
        History.LastSeen = OfflineHistoryClock;
        const int32 Delay = OfflineFix >= 2 ? GetNFORGuideDelay(InView) : 0;
        const uint32 SampleIndex = InView.State->GetPathTracingSampleIndex();
        const uint32 SampleCount = InView.State->GetPathTracingSampleCount();
        if (!History.Samples.Advance(SampleIndex, SampleCount, InView.bCameraCut, ViewRect, Delay))
        {
            // MRQ discards this partial image. Evaluating NR here would pollute
            // its temporal history with up to hundreds of unconverged images.
            return;
        }

        FOfflineFrame& Current = History.Frames[History.Samples.WriteSlot];
        Current.Matrices = InView.ViewMatrices;
        if (Delay > 0)
        {
            // Retain the *matching frame's* object velocities too. Reusing the
            // current raster buffer with NFOR's older color/depth is incorrect.
            FRDGTextureRef SavedVelocity = GraphBuilder.CreateTexture(Velocity->Desc,
                TEXT("DLSS5OneMinus.OfflineVelocity"));
            AddCopyTexturePass(GraphBuilder, Velocity, SavedVelocity);
            GraphBuilder.QueueTextureExtraction(SavedVelocity, &Current.Velocity);
        }
        else
        {
            Current.Velocity.SafeRelease();
        }

        const FOfflineFrame& Delivered = History.Frames[History.Samples.ReadSlot];
        bReset = History.Samples.bReset;
        if (History.Samples.ReadSlot != History.Samples.WriteSlot)
        {
            if (Delivered.Velocity.IsValid())
            {
                Velocity = GraphBuilder.RegisterExternalTexture(Delivered.Velocity);
            }
            else
            {
                bReset = true;
            }
        }
        ClipToPrevious = MakeClipToPrevious(Delivered.Matrices,
            History.Frames[History.Samples.PreviousSlot].Matrices);
        ClipToPreviousOverride = &ClipToPrevious;
        if (History.Samples.Completed == 1)
        {
            UE_LOG(LogDLSS5OneMinusOffline, Display,
                TEXT("NR offline history: final samples only, samples=%u, NFOR guide delay=%d, view=%u, film grain=%.3f."),
                SampleCount, Delay, Key, InView.FinalPostProcessSettings.FilmGrainIntensity);
        }
        UE_LOG(LogDLSS5OneMinusOffline, Verbose,
            TEXT("NR completed PT sample=%u/%u frame=%lld delay=%d reset=%d."),
            SampleIndex, SampleCount, History.Samples.Completed, Delay, bReset);
    }
#endif

    FRDGTextureRef Motion = DLSS5OneMinus::AddMotionGuidePass(
        GraphBuilder, InView, Depth, Velocity, ViewRect, ClipToPreviousOverride, bReset);
    GraphBuilder.Blackboard.Create<FDLSS5OneMinusFrameGuides>(
        &InView,
        Depth,
        Motion,
        ViewRect,
        bReset,
        ViewId,
        Settings);

    if (Path != 0)
    {
        return;
    }

    const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
    if (ViewInfo.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale
        || ViewRect.Size() == ViewInfo.GetSecondaryViewRectSize())
    {
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.EffectiveRoute = TEXT("Unavailable: Pre DLSS requires temporal scaling below 100%");
        Status.Detail = FString::Printf(
            TEXT("Pre DLSS requires reduced temporal scaling; method=%d aa=%d realtime=%s showAA=%d showTAA=%d primary=%dx%d secondary=%dx%d unscaled=%dx%d. The original scene color is preserved."),
            static_cast<int32>(ViewInfo.PrimaryScreenPercentageMethod),
            static_cast<int32>(ViewInfo.AntiAliasingMethod),
            ViewInfo.Family != nullptr && ViewInfo.Family->bRealtimeUpdate ? TEXT("true") : TEXT("false"),
            ViewInfo.Family != nullptr ? ViewInfo.Family->EngineShowFlags.AntiAliasing : 0,
            ViewInfo.Family != nullptr ? ViewInfo.Family->EngineShowFlags.TemporalAA : 0,
            ViewInfo.ViewRect.Width(),
            ViewInfo.ViewRect.Height(),
            ViewInfo.GetSecondaryViewRectSize().X,
            ViewInfo.GetSecondaryViewRectSize().Y,
            ViewInfo.UnscaledViewRect.Width(),
            ViewInfo.UnscaledViewRect.Height());
        DLSS5OneMinus::SetStatus(Status);
        return;
    }

    FRDGTextureRef SceneColorTexture = (*Inputs.SceneTextures)->SceneColorTexture;
    if (SceneColorTexture == nullptr
        || !SceneColorTexture->Desc.IsTexture2D()
        || ViewRect.Max.X > SceneColorTexture->Desc.Extent.X
        || ViewRect.Max.Y > SceneColorTexture->Desc.Extent.Y)
    {
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.EffectiveRoute = TEXT("Unavailable: Pre DLSS scene color layout");
        Status.Detail = TEXT("Pre DLSS requires an in-bounds 2D primary scene-color slice. The original scene color is preserved.");
        DLSS5OneMinus::SetStatus(Status);
        return;
    }

    const float WhitePoint = Settings.HDRModelWhite;
    FRDGTextureRef ModelInput = DLSS5OneMinus::AddHDREncodePass(
        GraphBuilder, InView, SceneColorTexture, ViewRect, WhitePoint);
    FDLSS5OneMinusModelPassInputs ModelInputs;
    ModelInputs.Color = ModelInput;
    ModelInputs.Depth = Depth;
    ModelInputs.MotionVectors = Motion;
    ModelInputs.ColorRect = FIntRect(FIntPoint::ZeroValue, ViewRect.Size());
    ModelInputs.DepthRect = ViewRect;
    ModelInputs.MotionVectorRect = FIntRect(FIntPoint::ZeroValue, ViewRect.Size());
    ModelInputs.bReset = bReset;
    ModelInputs.ViewId = ViewId;
    ModelInputs.Route = Path;
    ModelInputs.Settings = Settings;
    FRDGTextureRef ModelOutput = DLSS5OneMinus::AddModelPass(GraphBuilder, InView, ModelInputs);

    FRDGTextureDesc ResolveDesc = SceneColorTexture->Desc;
    ResolveDesc.Dimension = ETextureDimension::Texture2D;
    ResolveDesc.Extent = ViewRect.Size();
    ResolveDesc.ArraySize = 1;
    ResolveDesc.NumMips = 1;
    ResolveDesc.Flags |= TexCreate_ShaderResource | TexCreate_RenderTargetable;
    FRDGTextureRef ResolvedColor = GraphBuilder.CreateTexture(ResolveDesc, TEXT("DLSS5OneMinus.PreDLSSResolved"));
    const FIntRect CompactRect(FIntPoint::ZeroValue, ViewRect.Size());
    const FScreenPassTexture OriginalColor(SceneColorTexture, ViewRect);
    const FScreenPassRenderTarget ResolveTarget(ResolvedColor, CompactRect, ERenderTargetLoadAction::ENoAction);
    if (!DLSS5OneMinus::AddConsistencyResolvePass(
        GraphBuilder, InView, OriginalColor, ModelOutput, false,
        Settings.CompositionStrength, WhitePoint, Settings.DebugView, Settings.SliceOffsetPercent, ResolveTarget))
    {
        DLSS5OneMinus::AddHDRResolvePass(
            GraphBuilder,
            InView,
            OriginalColor,
            ModelOutput,
            Settings.CompositionStrength,
            WhitePoint,
            Settings.HDRComposition,
            Settings.DebugView,
            Settings.SliceOffsetPercent,
            ResolveTarget);
    }

    FRHICopyTextureInfo CopyInfo;
    CopyInfo.SourcePosition = FIntVector::ZeroValue;
    CopyInfo.DestPosition = FIntVector(ViewRect.Min.X, ViewRect.Min.Y, 0);
    CopyInfo.Size = FIntVector(ViewRect.Width(), ViewRect.Height(), 1);
    AddCopyTexturePass(GraphBuilder, ResolvedColor, SceneColorTexture, CopyInfo);

}

void FDLSS5OneMinusViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass,
    const FSceneView& InView,
    FAfterPassCallbackDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    const FDLSS5OneMinusRenderSettings Settings = DLSS5OneMinus::GetRenderSettings(InView);
    const int32 Path = Settings.Route;
    if (Pass == EPostProcessingPass::MotionBlur
        && Settings.bEnabled
        && Path == 2)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
            this, &FDLSS5OneMinusViewExtension::PostDLAA_RenderThread));
    }
    else if (Pass == EPostProcessingPass::Tonemap
        && bIsPassEnabled
        && Settings.bEnabled
        && Path == 1)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
            this, &FDLSS5OneMinusViewExtension::PostTone_RenderThread));
    }
}

FScreenPassTexture FDLSS5OneMinusViewExtension::PostDLAA_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs)
{
    const FDLSS5OneMinusFrameGuides* Guides = GraphBuilder.Blackboard.Get<FDLSS5OneMinusFrameGuides>();
    if (Guides == nullptr
        || Guides->View != &View
        || Guides->Depth == nullptr
        || Guides->Motion == nullptr
        || Guides->Settings.Route != 2) // frame snapshot must agree with this route (no double evaluate on a mid-frame route switch)
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
        GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid() || SceneColor.ViewRect.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
    if (!IsDLSSFamily(View.Family)
        || ViewInfo.ViewRect.Size() != ViewInfo.GetSecondaryViewRectSize()
        || SceneColor.ViewRect.Size() != Guides->GuideRect.Size())
    {
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.EffectiveRoute = TEXT("Unavailable: Post DLAA requires native temporal scale");
        Status.Detail = FString::Printf(
            TEXT("Post DLAA requires enabled DLSS/DLAA at native scale; method=%d primary=%dx%d secondary=%dx%d scene=%dx%d guide=%dx%d. The original scene color is preserved."),
            static_cast<int32>(ViewInfo.PrimaryScreenPercentageMethod),
            ViewInfo.ViewRect.Width(),
            ViewInfo.ViewRect.Height(),
            ViewInfo.GetSecondaryViewRectSize().X,
            ViewInfo.GetSecondaryViewRectSize().Y,
            SceneColor.ViewRect.Width(),
            SceneColor.ViewRect.Height(),
            Guides->GuideRect.Width(),
            Guides->GuideRect.Height());
        DLSS5OneMinus::SetStatus(Status);
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    const FDLSS5OneMinusRenderSettings& Settings = Guides->Settings;
    const float WhitePoint = Settings.HDRModelWhite;
    FRDGTextureRef ModelInput = DLSS5OneMinus::AddHDREncodePass(
        GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect, WhitePoint);
    FDLSS5OneMinusModelPassInputs ModelInputs;
    ModelInputs.Color = ModelInput;
    ModelInputs.Depth = Guides->Depth;
    ModelInputs.MotionVectors = Guides->Motion;
    ModelInputs.ColorRect = FIntRect(FIntPoint::ZeroValue, SceneColor.ViewRect.Size());
    ModelInputs.DepthRect = Guides->GuideRect;
    ModelInputs.MotionVectorRect = FIntRect(FIntPoint::ZeroValue, Guides->GuideRect.Size());
    ModelInputs.bReset = Guides->bReset;
    ModelInputs.ViewId = Guides->ViewId;
    ModelInputs.Route = 2;
    ModelInputs.Settings = Settings;
    FRDGTextureRef ModelOutput = DLSS5OneMinus::AddModelPass(GraphBuilder, View, ModelInputs);

    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(
            GraphBuilder,
            SceneColor,
            View.GetOverwriteLoadAction(),
            TEXT("DLSS5OneMinus.PostDLAAOutput"));
    }

    if (!DLSS5OneMinus::AddConsistencyResolvePass(
        GraphBuilder, View, SceneColor, ModelOutput, false,
        Settings.CompositionStrength, WhitePoint, Settings.DebugView, Settings.SliceOffsetPercent, Output))
    {
        DLSS5OneMinus::AddHDRResolvePass(
            GraphBuilder,
            View,
            SceneColor,
            ModelOutput,
            Settings.CompositionStrength,
            WhitePoint,
            Settings.HDRComposition,
            Settings.DebugView,
            Settings.SliceOffsetPercent,
            Output);
    }

    return MoveTemp(Output);
}

FScreenPassTexture FDLSS5OneMinusViewExtension::PostTone_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs)
{
    const FDLSS5OneMinusFrameGuides* Guides = GraphBuilder.Blackboard.Get<FDLSS5OneMinusFrameGuides>();
    if (Guides == nullptr
        || Guides->View != &View
        || Guides->Depth == nullptr
        || Guides->Motion == nullptr
        || Guides->Settings.Route != 1) // frame snapshot must agree with this route
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    if (View.Family == nullptr
        || View.Family->RenderTarget == nullptr
        || View.Family->RenderTarget->GetDisplayOutputFormat() != EDisplayOutputFormat::SDR_sRGB
        || View.Family->RenderTarget->GetSceneHDREnabled())
    {
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.EffectiveRoute = TEXT("Unavailable: Post Tone requires SDR sRGB");
        Status.Detail = TEXT("Post Tone was requested, but the active tonemapper output is not SDR sRGB. The original scene color is preserved.");
        DLSS5OneMinus::SetStatus(Status);
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
        GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid() || SceneColor.ViewRect.IsEmpty())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FRDGTextureRef ModelInput = DLSS5OneMinus::AddDisplayEncodePass(
        GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect);
    FDLSS5OneMinusModelPassInputs ModelInputs;
    ModelInputs.Color = ModelInput;
    ModelInputs.Depth = Guides->Depth;
    ModelInputs.MotionVectors = Guides->Motion;
    ModelInputs.ColorRect = FIntRect(FIntPoint::ZeroValue, SceneColor.ViewRect.Size());
    ModelInputs.DepthRect = Guides->GuideRect;
    ModelInputs.MotionVectorRect = FIntRect(FIntPoint::ZeroValue, Guides->GuideRect.Size());
    ModelInputs.bReset = Guides->bReset;
    ModelInputs.ViewId = Guides->ViewId;
    ModelInputs.Route = 1;
    ModelInputs.Settings = Guides->Settings;
    FRDGTextureRef ModelOutput = DLSS5OneMinus::AddModelPass(GraphBuilder, View, ModelInputs);

    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(
            GraphBuilder,
            SceneColor,
            View.GetOverwriteLoadAction(),
            TEXT("DLSS5OneMinus.PostToneOutput"));
    }

    if (!DLSS5OneMinus::AddConsistencyResolvePass(
        GraphBuilder, View, SceneColor, ModelOutput, true,
        Guides->Settings.CompositionStrength, 1.0f, Guides->Settings.DebugView, Guides->Settings.SliceOffsetPercent, Output))
    {
        DLSS5OneMinus::AddPostToneResolvePass(
            GraphBuilder,
            View,
            SceneColor,
            ModelOutput,
            Guides->Settings.CompositionStrength,
            Guides->Settings.DebugView,
            Guides->Settings.SliceOffsetPercent,
            Output);
    }

    return MoveTemp(Output);
}
