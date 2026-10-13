#include "DLSS5OneMinusConsistency.h"

#include "DLSS5OneMinusInternal.h"
#include "DLSS5OneMinusRuntimeD3D12.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusConsistencyRef, Log, All);

namespace
{
    FCriticalSection GConsistencyMutex;
    DLSS5OneMinus::FConsistencyContext GConsistencyContext;
    bool GConsistencyMissingReferenceWarned = false;
    bool GConsistencyReferenceUseLogged = false;

    // Game-thread state for the active pipeline.
    int32 GLoadedFrameKey = INDEX_NONE;
    uint32 GSettingsHash = 0;
    FTextureRHIRef GMapTexture_RenderThread;

    // Render-thread copy; only touched on the render thread so it stays ordered with the frames it belongs to.
    DLSS5OneMinus::FConsistencyFrameReference GReference_RenderThread;

    TAutoConsoleVariable<int32> CVarConsistencyMode(
        TEXT("r.NGX.DLSSNR.ConsistencyMode"),
        -1,
        TEXT("Multi-view consistency override. -1 = use the Movie Render Queue setting (default), 0 = off, 1 = reference tone, 2 = detail only (NR tone removed)."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarConsistencyToneBlur(
        TEXT("r.NGX.DLSSNR.ConsistencyToneBlur"),
        0.011f,
        TEXT("Gaussian sigma of the per-view tone/detail split as a fraction of the view width. Default 0.011."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarConsistencyReferenceBlur(
        TEXT("r.NGX.DLSSNR.ConsistencyReferenceBlur"),
        0.02f,
        TEXT("Gaussian sigma of the reference tone field as a fraction of the reference width. Default 0.02."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarConsistencyDebug(
        TEXT("r.NGX.DLSSNR.ConsistencyDebug"),
        0,
        TEXT("Consistency debug view: 0 off, 1 view tone, 2 detail, 3 reference tone, 4 reference validity."),
        ECVF_RenderThreadSafe);

    constexpr float MaxLogRatio = 1.386294f;
    // Bump whenever the tone-cache algorithm changes, so caches written by an older producer are not reused.
    constexpr int32 ProducerVersion = 2;
    constexpr float RatioFloor = 0.002f;

    float LinearToSrgb(float Value)
    {
        Value = FMath::Clamp(Value, 0.0f, 1.0f);
        return Value < 0.0031308f ? Value * 12.92f : 1.055f * FMath::Pow(FMath::Max(Value, 1.0e-8f), 1.0f / 2.4f) - 0.055f;
    }

    float SrgbToLinear(float Value)
    {
        Value = FMath::Clamp(Value, 0.0f, 1.0f);
        return Value < 0.04045f ? Value / 12.92f : FMath::Pow((Value + 0.055f) / 1.055f, 2.4f);
    }

    // Same soft knee as the HDR encode shader.
    FVector3f SoftKnee(FVector3f Value)
    {
        const float Luminance = Value.X * 0.2126f + Value.Y * 0.7152f + Value.Z * 0.0722f;
        if (Luminance > 0.75f)
        {
            const float Rolled = 0.75f + 0.25f * (1.0f - FMath::Exp(-(Luminance - 0.75f) / 0.25f));
            Value *= Rolled / FMath::Max(Luminance, 1.0e-6f);
        }
        const float Peak = FMath::Max3(Value.X, Value.Y, Value.Z);
        if (Peak > 1.0f)
        {
            Value /= Peak;
        }
        return Value;
    }

    // Linear-light value the consumers compare NR's output against, for the given route domain.
    FVector3f LinearOriginal(const FLinearColor& Pixel, bool bDisplayDomain, float WhitePoint)
    {
        const FVector3f Value(FMath::Max(Pixel.R, 0.0f), FMath::Max(Pixel.G, 0.0f), FMath::Max(Pixel.B, 0.0f));
        return bDisplayDomain
            ? FVector3f(FMath::Min(Value.X, 1.0f), FMath::Min(Value.Y, 1.0f), FMath::Min(Value.Z, 1.0f))
            : SoftKnee(Value / FMath::Max(WhitePoint, 0.001f));
    }

    void GaussianBlurMasked(TArray<FVector4f>& Data, int32 Width, int32 Height, float Sigma)
    {
        // xyz = value * weight, w = weight; blurring both and dividing keeps masked edges unbiased.
        const int32 Radius = FMath::Clamp(FMath::CeilToInt(Sigma * 3.0f), 1, 256);
        TArray<float> Kernel;
        Kernel.SetNum(Radius * 2 + 1);
        for (int32 Offset = -Radius; Offset <= Radius; ++Offset)
        {
            Kernel[Offset + Radius] = FMath::Exp(-(Offset * Offset) / (2.0f * Sigma * Sigma));
        }
        TArray<FVector4f> Temp;
        Temp.SetNumZeroed(Data.Num());
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            const TArray<FVector4f>& Source = Pass == 0 ? Data : Temp;
            TArray<FVector4f>& Target = Pass == 0 ? Temp : Data;
            TArray<FVector4f> Result;
            Result.SetNumZeroed(Source.Num());
            for (int32 Y = 0; Y < Height; ++Y)
            {
                for (int32 X = 0; X < Width; ++X)
                {
                    FVector4f Sum(0, 0, 0, 0);
                    for (int32 Offset = -Radius; Offset <= Radius; ++Offset)
                    {
                        const int32 SX = Pass == 0 ? FMath::Clamp(X + Offset, 0, Width - 1) : X;
                        const int32 SY = Pass == 1 ? FMath::Clamp(Y + Offset, 0, Height - 1) : Y;
                        Sum += Source[SY * Width + SX] * Kernel[Offset + Radius];
                    }
                    Result[Y * Width + X] = Sum;
                }
            }
            Target = MoveTemp(Result);
        }
    }

    // Masked Gaussian on a box-downsampled grid, bilinearly upsampled back. The fields blurred here are
    // smooth by construction, so this keeps large sigmas affordable at full reference resolution.
    void GaussianBlurMaskedFast(TArray<FVector4f>& Data, int32 Width, int32 Height, float Sigma)
    {
        const int32 Factor = FMath::Clamp(FMath::FloorToInt(Sigma / 4.0f), 1, 16);
        if (Factor == 1)
        {
            GaussianBlurMasked(Data, Width, Height, Sigma);
            return;
        }
        const int32 SmallW = FMath::DivideAndRoundUp(Width, Factor);
        const int32 SmallH = FMath::DivideAndRoundUp(Height, Factor);
        TArray<FVector4f> Small;
        Small.SetNumZeroed(SmallW * SmallH);
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                Small[(Y / Factor) * SmallW + X / Factor] += Data[Y * Width + X];
            }
        }
        GaussianBlurMasked(Small, SmallW, SmallH, Sigma / Factor);
        for (int32 Y = 0; Y < Height; ++Y)
        {
            const float SY = FMath::Clamp((Y + 0.5f) / Factor - 0.5f, 0.0f, SmallH - 1.0f);
            const int32 Y0 = FMath::FloorToInt(SY);
            const int32 Y1 = FMath::Min(Y0 + 1, SmallH - 1);
            const float FY = SY - Y0;
            for (int32 X = 0; X < Width; ++X)
            {
                const float SX = FMath::Clamp((X + 0.5f) / Factor - 0.5f, 0.0f, SmallW - 1.0f);
                const int32 X0 = FMath::FloorToInt(SX);
                const int32 X1 = FMath::Min(X0 + 1, SmallW - 1);
                const float FX = SX - X0;
                const FVector4f Top = Small[Y0 * SmallW + X0] * (1.0f - FX) + Small[Y0 * SmallW + X1] * FX;
                const FVector4f Bottom = Small[Y1 * SmallW + X0] * (1.0f - FX) + Small[Y1 * SmallW + X1] * FX;
                // Sums of Factor² samples: the ratio xyz / w is what matters, so no rescale is needed.
                Data[Y * Width + X] = Top * (1.0f - FY) + Bottom * FY;
            }
        }
    }

    // Progressive masked blurs: each invalid pixel takes the nearest scale's average of valid content.
    TArray<FLinearColor> FillInvalidArea(const TArrayView64<FLinearColor>& Pixels, int32 Width, int32 Height)
    {
        TArray<FLinearColor> Result;
        Result.SetNumUninitialized(Width * Height);
        TArray<bool> Done;
        Done.SetNumUninitialized(Width * Height);
        TArray<FVector4f> Source;
        Source.SetNumUninitialized(Width * Height);
        int32 Remaining = 0;
        for (int32 Index = 0; Index < Width * Height; ++Index)
        {
            const bool bValid = Pixels[Index].A > 0.5f;
            Result[Index] = bValid ? Pixels[Index] : FLinearColor::Black;
            Done[Index] = bValid;
            Remaining += bValid ? 0 : 1;
            Source[Index] = bValid ? FVector4f(Pixels[Index].R, Pixels[Index].G, Pixels[Index].B, 1.0f) : FVector4f(0, 0, 0, 0);
        }
        for (const float Fraction : { 0.004f, 0.016f, 0.06f, 0.25f })
        {
            if (Remaining == 0)
            {
                break;
            }
            TArray<FVector4f> Blurred = Source;
            GaussianBlurMaskedFast(Blurred, Width, Height, FMath::Max(Fraction * Width, 1.0f));
            for (int32 Index = 0; Index < Width * Height; ++Index)
            {
                if (!Done[Index] && Blurred[Index].W > 1.0e-3f)
                {
                    const FVector4f& Value = Blurred[Index];
                    Result[Index] = FLinearColor(Value.X / Value.W, Value.Y / Value.W, Value.Z / Value.W, Pixels[Index].A);
                    Done[Index] = true;
                    --Remaining;
                }
            }
        }
        return Result;
    }

    FString FramePath(const FString& Pattern, int32 FrameKey, int32 Digits)
    {
        return Pattern.Replace(TEXT("{frame}"), *FString::Printf(TEXT("%0*d"), Digits, FrameKey));
    }

    FString TonePathFor(const FString& CompositePath, uint32 SettingsHash)
    {
        return FPaths::ChangeExtension(CompositePath, TEXT("")) + FString::Printf(TEXT(".tone.%08x.exr"), SettingsHash);
    }

    uint32 HashSettings(const FDLSS5OneMinusRenderSettings& Settings, float ReferenceBlur)
    {
        uint32 Hash = 0;
        auto Mix = [&Hash](const auto& Value) { Hash = FCrc::MemCrc32(&Value, sizeof(Value), Hash); };
        Mix(Settings.Route);
        Mix(Settings.Style);
        Mix(Settings.Intensity);
        Mix(Settings.LocalToneStrength);
        Mix(Settings.LocalStructureStrength);
        Mix(Settings.SkinStructureStrength);
        Mix(Settings.bAutoMask);
        Mix(Settings.HDRModelWhite);
        Mix(ReferenceBlur);
        Mix(DLSS5OneMinus::ConsistencyContractVersion);
        Mix(ProducerVersion);
        return Hash;
    }

    bool LoadLinearImage(const FString& Path, FImage& OutImage)
    {
        FImage Loaded;
        if (!FImageUtils::LoadImage(*Path, Loaded))
        {
            return false;
        }
        Loaded.CopyTo(OutImage, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
        return OutImage.SizeX > 0 && OutImage.SizeY > 0;
    }

    // Evaluates NR on one reference composite and writes its blurred per-channel log-ratio tone (A = validity).
    bool ProduceToneCache(
        const FString& CompositePath,
        const FString& TonePath,
        const FDLSS5OneMinusRenderSettings& Settings,
        float ReferenceBlur)
    {
        FImage Composite;
        if (!LoadLinearImage(CompositePath, Composite))
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("Could not read reference composite %s."), *CompositePath);
            return false;
        }
        const int32 Width = Composite.SizeX;
        const int32 Height = Composite.SizeY;
        const bool bDisplayDomain = Settings.Route == 1;
        const TArrayView64<FLinearColor> Pixels = Composite.AsRGBA32F();

        // Outside the dome the composite is empty. The model judges tone from the whole image, so a large
        // black border would bias it; fill that area with the surrounding content before evaluation.
        TArray<FLinearColor> Filled = FillInvalidArea(Pixels, Width, Height);

        TArray<FFloat16Color> Encoded;
        Encoded.SetNumUninitialized(Width * Height);
        for (int32 Index = 0; Index < Width * Height; ++Index)
        {
            const FVector3f Linear = LinearOriginal(Filled[Index], bDisplayDomain, Settings.HDRModelWhite);
            Encoded[Index] = FFloat16Color(FLinearColor(LinearToSrgb(Linear.X), LinearToSrgb(Linear.Y), LinearToSrgb(Linear.Z), 1.0f));
        }

        TArray<FFloat16Color> Model;
        if (!DLSS5OneMinus::EvaluateStandaloneModel(Encoded, FIntPoint(Width, Height), Settings, Settings.Route, Model))
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("NR evaluation of reference composite %s failed."), *CompositePath);
            return false;
        }

        TArray<FVector4f> Field;
        Field.SetNumUninitialized(Width * Height);
        for (int32 Index = 0; Index < Width * Height; ++Index)
        {
            const FVector3f Original = LinearOriginal(Filled[Index], bDisplayDomain, Settings.HDRModelWhite);
            const FLinearColor Out = Model[Index].GetFloats();
            const float Weight = Pixels[Index].A > 0.5f ? 1.0f : 0.0f;
            FVector3f LogRatio(
                FMath::Loge((SrgbToLinear(Out.R) + RatioFloor) / (Original.X + RatioFloor)),
                FMath::Loge((SrgbToLinear(Out.G) + RatioFloor) / (Original.Y + RatioFloor)),
                FMath::Loge((SrgbToLinear(Out.B) + RatioFloor) / (Original.Z + RatioFloor)));
            LogRatio = FVector3f(
                FMath::Clamp(LogRatio.X, -MaxLogRatio, MaxLogRatio),
                FMath::Clamp(LogRatio.Y, -MaxLogRatio, MaxLogRatio),
                FMath::Clamp(LogRatio.Z, -MaxLogRatio, MaxLogRatio));
            Field[Index] = FVector4f(LogRatio * Weight, Weight);
        }
        GaussianBlurMaskedFast(Field, Width, Height, FMath::Max(ReferenceBlur * Width, 0.5f));

        FImage Tone(Width, Height, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
        const TArrayView64<FLinearColor> TonePixels = Tone.AsRGBA32F();
        for (int32 Index = 0; Index < Width * Height; ++Index)
        {
            const FVector4f& Value = Field[Index];
            const float Valid = Pixels[Index].A > 0.5f ? 1.0f : 0.0f;
            TonePixels[Index] = Value.W > 1.0e-3f
                ? FLinearColor(Value.X / Value.W, Value.Y / Value.W, Value.Z / Value.W, Valid)
                : FLinearColor(0, 0, 0, 0);
        }

        // Written to a temporary name and renamed, so concurrent consumers never read a partial file.
        const FString TempPath = TonePath + FString::Printf(TEXT(".%u.tmp.exr"), FPlatformProcess::GetCurrentProcessId());
        if (!FImageUtils::SaveImageByExtension(*TempPath, Tone))
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("Could not write tone cache %s."), *TempPath);
            return false;
        }
        if (!IFileManager::Get().Move(*TonePath, *TempPath, true, true))
        {
            IFileManager::Get().Delete(*TempPath);
            return IFileManager::Get().FileExists(*TonePath);
        }
        UE_LOG(LogDLSS5OneMinusConsistencyRef, Display, TEXT("Wrote reference tone %s (%dx%d)."), *TonePath, Width, Height);
        return true;
    }

    FTextureRHIRef CreateFloatTexture(FRHICommandListImmediate& RHICmdList, const FImage& Image, const TCHAR* Name)
    {
        const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Image.SizeX, Image.SizeY, PF_A32B32G32R32F)
            .SetFlags(ETextureCreateFlags::ShaderResource)
            .SetInitialState(ERHIAccess::SRVMask);
        FTextureRHIRef Texture = RHICreateTexture(Desc);
        const FUpdateTextureRegion2D Region(0, 0, 0, 0, Image.SizeX, Image.SizeY);
        RHICmdList.UpdateTexture2D(Texture, 0, Region, Image.SizeX * sizeof(FLinearColor), Image.RawData.GetData());
        return Texture;
    }

    // The NR runtime bootstrap is queued on the render/RHI threads. The producer runs on the game thread
    // before the first rendered frame, so wait for the bootstrap here instead of failing on a race.
    bool EnsureNRReadyForProducer()
    {
        if (DLSS5OneMinus::IsNGXSnippetBootstrapReady())
        {
            return true;
        }
        const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        if (Status.State == EDLSS5OneMinusRuntimeState::DependenciesReady && !Status.bNGXOwnershipProven)
        {
            DLSS5OneMinus::RunNGXSnippetBootstrap();
        }
        ENQUEUE_RENDER_COMMAND(DLSS5OneMinusProducerBootstrapFlush)(
            [](FRHICommandListImmediate& RHICmdList)
            {
                RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
            });
        FlushRenderingCommands();
        return DLSS5OneMinus::IsNGXSnippetBootstrapReady();
    }

    void ResetReferenceState()
    {
        GLoadedFrameKey = INDEX_NONE;
        ENQUEUE_RENDER_COMMAND(DLSS5OneMinusConsistencyReset)(
            [](FRHICommandListImmediate&)
            {
                GReference_RenderThread = DLSS5OneMinus::FConsistencyFrameReference();
                GMapTexture_RenderThread.SafeRelease();
            });
    }
}

void DLSS5OneMinus::SetConsistencyContext(const FConsistencyContext& Context)
{
    {
        FScopeLock Lock(&GConsistencyMutex);
        GConsistencyContext = Context;
        GConsistencyMissingReferenceWarned = false;
        GConsistencyReferenceUseLogged = false;
    }
    ResetReferenceState();
}

void DLSS5OneMinus::ClearConsistencyContext()
{
    {
        FScopeLock Lock(&GConsistencyMutex);
        GConsistencyContext = FConsistencyContext();
    }
    ResetReferenceState();
}

DLSS5OneMinus::FConsistencyContext DLSS5OneMinus::GetConsistencyContext()
{
    FScopeLock Lock(&GConsistencyMutex);
    return GConsistencyContext;
}

bool DLSS5OneMinus::PrepareConsistencyReferences(const FDLSS5OneMinusRenderSettings& Settings)
{
    check(IsInGameThread());
    const FConsistencyContext Context = GetConsistencyContext();
    if (Context.Mode != EConsistencyMode::Consumer)
    {
        return true;
    }
    const float ReferenceBlur = FMath::Clamp(CVarConsistencyReferenceBlur.GetValueOnGameThread(), 0.001f, 0.2f);
    GSettingsHash = HashSettings(Settings, ReferenceBlur);
    const bool bNRReady = EnsureNRReadyForProducer();

    // Every composite matching the pattern gets a tone cache; existing caches for these settings are reused.
    const FString Directory = FPaths::GetPath(Context.ReferenceCompositePattern);
    const FString Wildcard = FPaths::GetCleanFilename(Context.ReferenceCompositePattern).Replace(TEXT("{frame}"), TEXT("*"));
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, Wildcard), true, false);
    int32 Produced = 0;
    int32 Reused = 0;
    int32 Failed = 0;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT(".tone.")))
        {
            continue;
        }
        const FString CompositePath = FPaths::Combine(Directory, File);
        const FString TonePath = TonePathFor(CompositePath, GSettingsHash);
        if (IFileManager::Get().FileExists(*TonePath))
        {
            ++Reused;
        }
        else if (bNRReady && ProduceToneCache(CompositePath, TonePath, Settings, ReferenceBlur))
        {
            ++Produced;
        }
        else
        {
            ++Failed;
        }
    }
    UE_LOG(LogDLSS5OneMinusConsistencyRef, Display,
        TEXT("Reference tone for viewport '%s': %d produced, %d reused, %d failed (settings %08x, %s)."),
        *Context.ReferenceViewportId, Produced, Reused, Failed, GSettingsHash, *Context.ReferenceCompositePattern);
    return Failed == 0 && Produced + Reused > 0;
}

void DLSS5OneMinus::UpdateConsistencyFrame(int32 OutputFrameNumber)
{
    check(IsInGameThread());
    const FConsistencyContext Context = GetConsistencyContext();
    if (Context.Mode != EConsistencyMode::Consumer)
    {
        return;
    }
    // Warm-up frames report output frame -1; they lead into output frame 0, so they use its reference.
    const int32 FrameKey = FMath::Max(OutputFrameNumber, 0) + Context.ReferenceFrameOffset;
    if (FrameKey == GLoadedFrameKey)
    {
        return;
    }
    const bool bLoadMap = GLoadedFrameKey == INDEX_NONE;
    if (bLoadMap)
    {
        UE_LOG(LogDLSS5OneMinusConsistencyRef, Display, TEXT("Viewport '%s': reference frame key = output frame %d + offset %d = %d."),
            *Context.ReferenceViewportId, FMath::Max(OutputFrameNumber, 0), Context.ReferenceFrameOffset, FrameKey);
    }
    GLoadedFrameKey = FrameKey;

    const FString CompositePath = FramePath(Context.ReferenceCompositePattern, FrameKey, Context.ReferenceFrameDigits);
    TSharedRef<FImage, ESPMode::ThreadSafe> Tone = MakeShared<FImage, ESPMode::ThreadSafe>();
    TSharedRef<FImage, ESPMode::ThreadSafe> Map = MakeShared<FImage, ESPMode::ThreadSafe>();
    const bool bToneLoaded = LoadLinearImage(TonePathFor(CompositePath, GSettingsHash), *Tone);
    bool bMapLoaded = !bLoadMap;
    if (bLoadMap)
    {
        bMapLoaded = LoadLinearImage(FPaths::Combine(Context.ReferenceMapDirectory, Context.ReferenceViewportId + TEXT(".exr")), *Map);
        if (!bMapLoaded)
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("No view-to-reference map for viewport '%s' in %s."),
                *Context.ReferenceViewportId, *Context.ReferenceMapDirectory);
        }
    }
    if (!bToneLoaded)
    {
        // Warm-up and handle frames usually have no reference; report the first miss per render, then quietly.
        if (ShouldWarnMissingConsistencyReference())
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("No reference tone for frame %d (%s); frames without one keep the view's own tone."),
                FrameKey, *CompositePath);
        }
        else
        {
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Verbose, TEXT("No reference tone for frame %d (%s)."), FrameKey, *CompositePath);
        }
    }
    else
    {
        UE_LOG(LogDLSS5OneMinusConsistencyRef, Log, TEXT("Reference tone loaded for frame %d."), FrameKey);
    }

    ENQUEUE_RENDER_COMMAND(DLSS5OneMinusConsistencyFrame)(
        [Tone, Map, bToneLoaded, bLoadMap, bMapLoaded, FrameKey](FRHICommandListImmediate& RHICmdList)
        {
            if (bLoadMap)
            {
                GMapTexture_RenderThread = bMapLoaded ? CreateFloatTexture(RHICmdList, *Map, TEXT("DLSS5OneMinus.ConsistencyMap")) : nullptr;
            }
            GReference_RenderThread.FrameKey = FrameKey;
            GReference_RenderThread.MapTexture = GMapTexture_RenderThread;
            GReference_RenderThread.ToneTexture = bToneLoaded ? CreateFloatTexture(RHICmdList, *Tone, TEXT("DLSS5OneMinus.ConsistencyTone")) : nullptr;
        });
}

DLSS5OneMinus::EConsistencyMode DLSS5OneMinus::GetEffectiveConsistencyMode()
{
    const int32 Override = CVarConsistencyMode.GetValueOnAnyThread();
    if (Override >= 0 && Override <= 2)
    {
        return static_cast<EConsistencyMode>(Override);
    }
    FScopeLock Lock(&GConsistencyMutex);
    return GConsistencyContext.Mode;
}

DLSS5OneMinus::FConsistencyFrameReference DLSS5OneMinus::GetConsistencyFrameReference()
{
    check(IsInRenderingThread());
    return GReference_RenderThread;
}

bool DLSS5OneMinus::ShouldWarnMissingConsistencyReference()
{
    FScopeLock Lock(&GConsistencyMutex);
    const bool bWarn = !GConsistencyMissingReferenceWarned;
    GConsistencyMissingReferenceWarned = true;
    return bWarn;
}

bool DLSS5OneMinus::ShouldLogConsistencyReferenceUse()
{
    FScopeLock Lock(&GConsistencyMutex);
    const bool bLog = !GConsistencyReferenceUseLogged;
    GConsistencyReferenceUseLogged = true;
    return bLog;
}

float DLSS5OneMinus::GetConsistencyToneBlur()
{
    return FMath::Clamp(CVarConsistencyToneBlur.GetValueOnAnyThread(), 0.001f, 0.1f);
}

int32 DLSS5OneMinus::GetConsistencyDebug()
{
    return FMath::Clamp(CVarConsistencyDebug.GetValueOnAnyThread(), 0, 4);
}

namespace
{
    // Developer test of the consumer path without a Movie Render Queue job:
    //   DLSSNR.ConsistencyTest <compositePattern> <mapDirectory> <viewportId> <frame>   (consumer mode)
    //   DLSSNR.ConsistencyTest off
    FAutoConsoleCommandWithWorldAndArgs GConsistencyTestCommand(
        TEXT("DLSSNR.ConsistencyTest"),
        TEXT("Consumer-mode test: <compositePattern with {frame}> <mapDirectory> <viewportId> <frame>, or 'off'."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
        {
            if (Args.Num() == 1 && Args[0].Equals(TEXT("off"), ESearchCase::IgnoreCase))
            {
                DLSS5OneMinus::ClearConsistencyContext();
                UE_LOG(LogDLSS5OneMinusConsistencyRef, Display, TEXT("Consistency test context cleared."));
                return;
            }
            if (Args.Num() < 4 || World == nullptr)
            {
                UE_LOG(LogDLSS5OneMinusConsistencyRef, Warning, TEXT("Usage: DLSSNR.ConsistencyTest <compositePattern> <mapDirectory> <viewportId> <frame> | off"));
                return;
            }
            DLSS5OneMinus::FConsistencyContext Context;
            Context.Mode = DLSS5OneMinus::EConsistencyMode::Consumer;
            Context.ReferenceCompositePattern = Args[0];
            Context.ReferenceMapDirectory = Args[1];
            Context.ReferenceViewportId = Args[2];
            DLSS5OneMinus::SetConsistencyContext(Context);
            const bool bPrepared = DLSS5OneMinus::PrepareConsistencyReferences(
                DLSS5OneMinus::GetRenderSettingsForScene(World->Scene));
            DLSS5OneMinus::UpdateConsistencyFrame(FCString::Atoi(*Args[3]));
            UE_LOG(LogDLSS5OneMinusConsistencyRef, Display, TEXT("Consistency test context set (prepared=%s)."),
                bPrepared ? TEXT("true") : TEXT("false"));
        }));
}
