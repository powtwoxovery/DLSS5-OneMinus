#include "MoviePipelineDLSS5OneMinusSetting.h"

#include "DLSS5OneMinusProfile.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusStatus.h"
#include "MoviePipeline.h"
#include "MovieRenderPipelineDataTypes.h"
#include "SceneView.h"

#define LOCTEXT_NAMESPACE "MoviePipelineDLSS5OneMinusSetting"

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
    bHadPreviousState = false;
}

void UMoviePipelineDLSS5OneMinusSetting::SetupViewFamily(FSceneViewFamily& ViewFamily)
{
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
