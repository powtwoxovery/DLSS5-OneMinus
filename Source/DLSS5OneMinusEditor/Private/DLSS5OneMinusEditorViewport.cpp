#include "DLSS5OneMinusEditorViewport.h"

#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusStatus.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "SLevelViewport.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusEditorViewport, Log, All);

namespace
{
    const FText RealtimeOverrideName = FText::FromString(TEXT("DLSS5-OneMinus Neural Rendering"));

    // Viewport ownership: AA/TAA/throttling overrides are captured and restored once per ownership, and NR control
    // follows the editor world's effective state. The preview screen percentage is user intent owned by the Lab panel
    // and is never snapshotted or restored here.
    struct FOwnedViewportState
    {
        FEditorViewportClient* ViewportClient = nullptr;
        int32 LastAppliedPercentage = 100;
        bool bPreviousAntiAliasing = true;
        bool bPreviousTemporalAA = true;
        int32 PreviousAntiAliasingMethod = 0;
        int32 PreviousTemporalUpsampling = 0;
        int32 PreviousTemporalUpscaler = 0;
        int32 PreviousSlateThrottling = 1;
        bool bOwnsOverrides = false;
    };

    FOwnedViewportState GOwnedState;

    FLevelEditorViewportClient* GetActiveViewportClient()
    {
        FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (LevelEditor != nullptr)
        {
            const TSharedPtr<SLevelViewport> Viewport = LevelEditor->GetFirstActiveLevelViewport();
            if (Viewport.IsValid())
            {
                FLevelEditorViewportClient& ViewportClient = Viewport->GetLevelViewportClient();
                if (ViewportClient.Viewport != nullptr && ViewportClient.IsVisible())
                {
                    return &ViewportClient;
                }
            }
        }

        if (GCurrentLevelEditingViewportClient != nullptr
            && GCurrentLevelEditingViewportClient->Viewport != nullptr
            && GCurrentLevelEditingViewportClient->IsVisible())
        {
            return GCurrentLevelEditingViewportClient;
        }

        return GCurrentLevelEditingViewportClient;
    }

    IConsoleVariable* FindCVar(const TCHAR* Name)
    {
        return IConsoleManager::Get().FindConsoleVariable(Name);
    }

    IConsoleVariable* GetEditorScreenPercentageCVar()
    {
        static IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
            TEXT("r.NGX.DLSSNR.EditorScreenPercentage"));
        return Variable;
    }

    bool IsLiveViewportClient(FEditorViewportClient* ViewportClient)
    {
        return ViewportClient != nullptr
            && GEditor != nullptr
            && GEditor->GetAllViewportClients().Contains(ViewportClient);
    }

    void RestoreCVarIfOwned(const TCHAR* Name, const int32 AppliedValue, const int32 PreviousValue)
    {
        if (IConsoleVariable* Variable = FindCVar(Name);
            Variable != nullptr && Variable->GetInt() == AppliedValue)
        {
            Variable->Set(PreviousValue, ECVF_SetByConsole);
        }
    }

    void ReleaseOwnedState()
    {
        FEditorViewportClient* ViewportClient = GOwnedState.ViewportClient;
        if (GOwnedState.bOwnsOverrides && IsLiveViewportClient(ViewportClient))
        {
            ViewportClient->RemoveRealtimeOverride(RealtimeOverrideName, false);
            if (ViewportClient->EngineShowFlags.AntiAliasing)
            {
                ViewportClient->EngineShowFlags.SetAntiAliasing(GOwnedState.bPreviousAntiAliasing);
            }
            if (ViewportClient->EngineShowFlags.TemporalAA)
            {
                ViewportClient->EngineShowFlags.SetTemporalAA(GOwnedState.bPreviousTemporalAA);
            }
            ViewportClient->Invalidate();
        }

        if (GOwnedState.bOwnsOverrides)
        {
            RestoreCVarIfOwned(TEXT("r.AntiAliasingMethod"), 4, GOwnedState.PreviousAntiAliasingMethod);
            RestoreCVarIfOwned(TEXT("r.TemporalAA.Upsampling"), 1, GOwnedState.PreviousTemporalUpsampling);
            RestoreCVarIfOwned(TEXT("r.TemporalAA.Upscaler"), 1, GOwnedState.PreviousTemporalUpscaler);
            RestoreCVarIfOwned(TEXT("Slate.bAllowThrottling"), 0, GOwnedState.PreviousSlateThrottling);
            if (IConsoleVariable* Variable = GetEditorScreenPercentageCVar();
                Variable != nullptr && Variable->GetInt() == GOwnedState.LastAppliedPercentage)
            {
                Variable->Set(0, ECVF_SetByConsole);
            }
            IConsoleManager::Get().CallAllConsoleVariableSinks();
        }

        GOwnedState = FOwnedViewportState();
    }

    void AcquireOwnedState(FEditorViewportClient* ViewportClient, const int32 Percentage)
    {
        if (GOwnedState.bOwnsOverrides && GOwnedState.ViewportClient != ViewportClient)
        {
            ReleaseOwnedState();
        }

        if (!GOwnedState.bOwnsOverrides)
        {
            GOwnedState.ViewportClient = ViewportClient;
            GOwnedState.bPreviousAntiAliasing = ViewportClient->EngineShowFlags.AntiAliasing;
            GOwnedState.bPreviousTemporalAA = ViewportClient->EngineShowFlags.TemporalAA;
            GOwnedState.PreviousAntiAliasingMethod = FindCVar(TEXT("r.AntiAliasingMethod")) != nullptr
                ? FindCVar(TEXT("r.AntiAliasingMethod"))->GetInt() : 0;
            GOwnedState.PreviousTemporalUpsampling = FindCVar(TEXT("r.TemporalAA.Upsampling")) != nullptr
                ? FindCVar(TEXT("r.TemporalAA.Upsampling"))->GetInt() : 0;
            GOwnedState.PreviousTemporalUpscaler = FindCVar(TEXT("r.TemporalAA.Upscaler")) != nullptr
                ? FindCVar(TEXT("r.TemporalAA.Upscaler"))->GetInt() : 0;
            GOwnedState.PreviousSlateThrottling = FindCVar(TEXT("Slate.bAllowThrottling")) != nullptr
                ? FindCVar(TEXT("Slate.bAllowThrottling"))->GetInt() : 1;
            GOwnedState.bOwnsOverrides = true;
            if (IConsoleVariable* Variable = FindCVar(TEXT("r.AntiAliasingMethod"))) { Variable->Set(4, ECVF_SetByConsole); }
            if (IConsoleVariable* Variable = FindCVar(TEXT("r.TemporalAA.Upsampling"))) { Variable->Set(1, ECVF_SetByConsole); }
            if (IConsoleVariable* Variable = FindCVar(TEXT("r.TemporalAA.Upscaler"))) { Variable->Set(1, ECVF_SetByConsole); }
            if (IConsoleVariable* Variable = FindCVar(TEXT("Slate.bAllowThrottling"))) { Variable->Set(0, ECVF_SetByConsole); }
        }

        GOwnedState.LastAppliedPercentage = Percentage;
        if (!ViewportClient->HasRealtimeOverride(RealtimeOverrideName))
        {
            ViewportClient->AddRealtimeOverride(true, RealtimeOverrideName);
        }
        ViewportClient->EngineShowFlags.SetAntiAliasing(true);
        ViewportClient->EngineShowFlags.SetTemporalAA(true);
    }

    bool IsEditorWorldNREnabled()
    {
        const UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
        return World != nullptr
            && World->Scene != nullptr
            && DLSS5OneMinus::GetRenderSettingsForScene(World->Scene).bEnabled;
    }
}

// Switch the active Level Editor viewport's actor lock to the selected camera.
// Copying a camera transform for one frame is immediately overwritten when the viewport is already piloting another camera.
bool DLSS5OneMinusEditorViewport::SetViewFromCamera(ACameraActor* CameraActor)
{
    FLevelEditorViewportClient* ViewportClient = GetActiveViewportClient();
    if (!IsValid(CameraActor) || ViewportClient == nullptr)
    {
        return false;
    }

    FMinimalViewInfo CameraView;
    CameraActor->GetCameraComponent()->GetCameraView(0.0f, CameraView);
    if (!ViewportClient->IsPerspective())
    {
        ViewportClient->SetViewportType(LVT_Perspective);
    }
    ViewportClient->SetActorLock(CameraActor);
    ViewportClient->MoveCameraToLockedActor();
    ViewportClient->Invalidate();

    UE_LOG(LogDLSS5OneMinusEditorViewport, Display,
        TEXT("Switched and locked editor viewport to camera '%s' at %s / %s with FOV %.2f."),
        *CameraActor->GetActorLabel(),
        *CameraView.Location.ToCompactString(),
        *CameraView.Rotation.ToCompactString(),
        CameraView.FOV);
    return true;
}

int32 DLSS5OneMinusEditorViewport::NormalizeScreenPercentage(const int32 Percentage, const int32 Route, const bool bNeuralRendering)
{
    // route limits apply only while NR is requested; without NR the
    // editor preview floor (25%) is the only constraint, because the sub-25% driver is NR-owned.
    if (!bNeuralRendering)
    {
        return FMath::Clamp(Percentage, 25, 200);
    }
    const int32 Clamped = FMath::Clamp(Percentage, 10, 200);
    if (Route == 2)
    {
        return 100;
    }
    if (Route == 0)
    {
        return FMath::Clamp(Clamped, 33, 99);
    }
    return Clamped;
}

bool DLSS5OneMinusEditorViewport::SetScreenPercentage(int32 Percentage)
{
    FEditorViewportClient* ViewportClient = GetActiveViewportClient();
    if (ViewportClient == nullptr)
    {
        return false;
    }

    Percentage = FMath::Clamp(Percentage, 10, 200);
    const TOptional<int32> PreviousPercentage = GetScreenPercentage();
    if (!PreviousPercentage.IsSet() || PreviousPercentage.GetValue() != Percentage)
    {
        // DLSS Ultra Performance is a fixed-resolution mode. A Slate slider can
        // otherwise queue a new view size while the RHI thread is still
        // evaluating the previous fixed-size NGX feature, which NVIDIA rejects
        // as an invalid dynamic-resolution input. Drain the preceding frame
        // before publishing the new percentage.
        FlushRenderingCommands();
    }
    const bool bNeuralControlActive = IsEditorWorldNREnabled(); // per-world state, not the global CVar
    if (bNeuralControlActive)
    {
        AcquireOwnedState(ViewportClient, Percentage);
    }
    else if (GOwnedState.bOwnsOverrides)
    {
        ReleaseOwnedState(); // NR off must not leave AA/realtime overrides owned
    }

    ViewportClient->SetPreviewScreenPercentage(Percentage);
    ViewportClient->SetPreviewingScreenPercentage(true);
    if (IConsoleVariable* Variable = GetEditorScreenPercentageCVar())
    {
        Variable->Set(Percentage, ECVF_SetByConsole);
    }
    IConsoleManager::Get().CallAllConsoleVariableSinks();

    const uint64 TargetViewId = bNeuralControlActive
        ? reinterpret_cast<uint64>(ViewportClient->ViewState.GetReference())
        : 0;
    const uint64 TargetRenderTargetId = bNeuralControlActive
        ? reinterpret_cast<uint64>(ViewportClient->Viewport)
        : 0;
    DLSS5OneMinus::SetTargetViewId(TargetViewId);
    DLSS5OneMinus::SetTargetRenderTargetId(TargetRenderTargetId);
    ViewportClient->Invalidate();
    UE_LOG(LogDLSS5OneMinusEditorViewport, Display,
        TEXT("Configured current viewport client=%p renderTarget=%p visible=%s realtime=%s override=%s neuralControl=%s target=%llu percentage=%d."),
        ViewportClient,
        ViewportClient->Viewport,
        ViewportClient->IsVisible() ? TEXT("true") : TEXT("false"),
        ViewportClient->IsRealtime() ? TEXT("true") : TEXT("false"),
        ViewportClient->HasRealtimeOverride(RealtimeOverrideName) ? TEXT("true") : TEXT("false"),
        bNeuralControlActive ? TEXT("true") : TEXT("false"),
        TargetViewId,
        Percentage);
    return true;
}

TOptional<int32> DLSS5OneMinusEditorViewport::GetScreenPercentage()
{
    FEditorViewportClient* ViewportClient = GetActiveViewportClient();
    if (ViewportClient == nullptr || !ViewportClient->IsPreviewingScreenPercentage())
    {
        return {};
    }
    if (GOwnedState.bOwnsOverrides && GOwnedState.ViewportClient == ViewportClient)
    {
        if (const IConsoleVariable* Variable = GetEditorScreenPercentageCVar();
            Variable != nullptr && Variable->GetInt() > 0)
        {
            return Variable->GetInt();
        }
    }
    return ViewportClient->GetPreviewScreenPercentage();
}

void DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(const TCHAR* Reason)
{
    ReleaseOwnedState();
    DLSS5OneMinus::SetTargetRenderTargetId(0);
    DLSS5OneMinus::SetTargetViewId(0);
    DLSS5OneMinus::InvalidateOutputStatus(Reason);
    UE_LOG(LogDLSS5OneMinusEditorViewport, Display, TEXT("Released neural-rendering viewport control: %s"),
        Reason != nullptr ? Reason : TEXT("unspecified"));
}

bool DLSS5OneMinusEditorViewport::Invalidate()
{
    if (FEditorViewportClient* ViewportClient = GetActiveViewportClient())
    {
        ViewportClient->Invalidate();
        if (IsInGameThread() && ViewportClient->Viewport != nullptr)
        {
            // Slate can defer even a realtime editor viewport while the
            // control panel owns focus, and IsVisible() can report false for
            // that still-displayed level viewport. FViewport::Draw is the
            // same bounded path used by UE's screenshot and click handlers.
            ViewportClient->Viewport->Draw();
        }
        return true;
    }
    return false;
}
