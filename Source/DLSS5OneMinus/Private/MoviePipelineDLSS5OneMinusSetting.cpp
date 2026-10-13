#include "MoviePipelineDLSS5OneMinusSetting.h"

#include "DLSS5OneMinusConsistency.h"
#include "DLSS5OneMinusInternal.h"
#include "DLSS5OneMinusProfile.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusStatus.h"
#include "MoviePipeline.h"
#include "MovieRenderPipelineDataTypes.h"
#include "SceneView.h"

#define LOCTEXT_NAMESPACE "MoviePipelineDLSS5OneMinusSetting"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusMoviePipeline, Log, All);

void UMoviePipelineDLSS5OneMinusSetting::SetupForPipelineImpl(UMoviePipeline* InPipeline)
{
    UWorld* World = InPipeline != nullptr ? InPipeline->GetWorld() : nullptr;
    bHadPreviousState = DLSS5OneMinus::GetEffectiveSettings(World, PreviousState);

    FDLSS5OneMinusSettings Settings = GetDefault<UDLSS5OneMinusProjectSettings>()->ResolveDefaultSettings();
    FName Source(TEXT("MRQ / Project Default"));
    if (const UDLSS5OneMinusProfile* LoadedProfile = Profile.LoadSynchronous())
    {
        Settings = LoadedProfile->Settings;
        Source = FName(*FString::Printf(TEXT("MRQ Profile: %s"), *LoadedProfile->GetName()));
    }
    DLSS5OneMinus::ApplySettings(World, Settings, bEnableNeuralRendering, true, Source);

    DLSS5OneMinus::ClearConsistencyContext();
    if (bEnableNeuralRendering && ConsistencyMode != 0)
    {
        if (ConsistencyContractVersion != DLSS5OneMinus::ConsistencyContractVersion)
        {
            UE_LOG(LogDLSS5OneMinusMoviePipeline, Warning,
                TEXT("DLSS5-OneMinus: multi-view consistency contract version %d is not supported (expected %d); consistency is off for this render."),
                ConsistencyContractVersion, DLSS5OneMinus::ConsistencyContractVersion);
        }
        else
        {
            DLSS5OneMinus::FConsistencyContext Context;
            Context.Mode = static_cast<DLSS5OneMinus::EConsistencyMode>(FMath::Clamp(ConsistencyMode, 0, 2));
            Context.ReferenceCompositePattern = ReferenceCompositePattern;
            Context.ReferenceFrameDigits = FMath::Clamp(ReferenceFrameDigits, 1, 12);
            Context.ReferenceFrameOffset = ReferenceFrameOffset;
            Context.ReferenceMapDirectory = ReferenceMapDirectory;
            Context.ReferenceViewportId = ReferenceViewportId;
            DLSS5OneMinus::SetConsistencyContext(Context);
            UE_LOG(LogDLSS5OneMinusMoviePipeline, Display,
                TEXT("DLSS5-OneMinus: multi-view consistency mode %d for viewport '%s'."),
                ConsistencyMode, *ReferenceViewportId);
            if (Context.Mode == DLSS5OneMinus::EConsistencyMode::Consumer && World != nullptr)
            {
                DLSS5OneMinus::PrepareConsistencyReferences(DLSS5OneMinus::GetRenderSettingsForScene(World->Scene));
            }
        }
    }
}

void UMoviePipelineDLSS5OneMinusSetting::TeardownForPipelineImpl(UMoviePipeline* InPipeline)
{
    UWorld* World = InPipeline != nullptr ? InPipeline->GetWorld() : nullptr;
    if (bHadPreviousState)
    {
        DLSS5OneMinus::ApplySettings(
            World,
            PreviousState.Settings,
            PreviousState.bEnabled,
            PreviousState.bAllowSceneCaptures,
            PreviousState.Source);
    }
    else
    {
        DLSS5OneMinus::ApplyProjectDefaults(World, TEXT("MRQ Restore / Project Default"));
    }
    DLSS5OneMinus::ClearConsistencyContext();
    bHadPreviousState = false;
}

void UMoviePipelineDLSS5OneMinusSetting::SetupViewFamily(FSceneViewFamily& ViewFamily)
{
    if (const UMoviePipeline* Pipeline = GetPipeline())
    {
        // Keyed by the output frame index plus ReferenceFrameOffset (the renderer stamps the offset that maps
        // output index 0 to the first reference frame); the sequence frame number is not yet set here.
        DLSS5OneMinus::UpdateConsistencyFrame(Pipeline->GetOutputState().OutputFrameNumber);
    }
    if (bEnableNeuralRendering)
    {
        ViewFamily.bRealtimeUpdate = true;
        ViewFamily.EngineShowFlags.SetAntiAliasing(true);
        ViewFamily.EngineShowFlags.SetTemporalAA(true);
    }
}

void UMoviePipelineDLSS5OneMinusSetting::ValidateStateImpl()
{
    Super::ValidateStateImpl();
    const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
    if (!Status.bVendorPluginAvailable || !Status.bUnofficialRuntimeAvailable)
    {
        ValidationState = EMoviePipelineValidationState::Warnings;
        ValidationResults.Add(FText::FromString(Status.Detail));
    }
}

void UMoviePipelineDLSS5OneMinusSetting::GetFormatArguments(FMoviePipelineFormatArgs& InOutFormatArgs) const
{
    Super::GetFormatArguments(InOutFormatArgs);
    const UDLSS5OneMinusProfile* LoadedProfile = Profile.Get();
    const FString ProfileName = LoadedProfile != nullptr ? LoadedProfile->GetName() : TEXT("ProjectDefault");
    InOutFormatArgs.FileMetadata.Add(TEXT("unreal/dlss5oneminusProfile"), ProfileName);
    InOutFormatArgs.FilenameArguments.Add(TEXT("dlss5oneminus_profile"), ProfileName);
}

#if WITH_EDITOR
FText UMoviePipelineDLSS5OneMinusSetting::GetDisplayText() const
{
    return LOCTEXT("DisplayName", "DLSS5-OneMinus Neural Rendering");
}

FText UMoviePipelineDLSS5OneMinusSetting::GetCategoryText() const
{
    return LOCTEXT("Category", "Rendering");
}
#endif

#undef LOCTEXT_NAMESPACE
