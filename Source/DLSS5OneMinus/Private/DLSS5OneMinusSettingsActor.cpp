#include "DLSS5OneMinusSettingsActor.h"

#include "DLSS5OneMinusProfile.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusWorldSubsystem.h"

ADLSS5OneMinusSettingsActor::ADLSS5OneMinusSettingsActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bIsSpatiallyLoaded = false;
}

FDLSS5OneMinusApplyResult ADLSS5OneMinusSettingsActor::ApplyToWorld() const
{
    FDLSS5OneMinusSettings Resolved = InlineSettings;
    FName Source = GetFName();
    if (!bUseInlineSettings)
    {
        if (const UDLSS5OneMinusProfile* LoadedProfile = Profile.LoadSynchronous())
        {
            Resolved = LoadedProfile->Settings;
            Source = FName(*FString::Printf(TEXT("Level Profile: %s"), *LoadedProfile->GetName()));
        }
        else
        {
            Resolved = GetDefault<UDLSS5OneMinusProjectSettings>()->ResolveDefaultSettings();
            Source = TEXT("Level Actor / Project Default");
        }
    }
    return DLSS5OneMinus::ApplySettings(
        GetWorld(), Resolved, bEnableNeuralRendering, bAllowSceneCaptures, Source);
}

void ADLSS5OneMinusSettingsActor::BeginPlay()
{
    Super::BeginPlay();
    if (bApplyOnWorldStart)
    {
        if (UDLSS5OneMinusWorldSubsystem* Subsystem = GetWorld()->GetSubsystem<UDLSS5OneMinusWorldSubsystem>())
        {
            Subsystem->RefreshEffectiveSettings();
        }
    }
}

void ADLSS5OneMinusSettingsActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UDLSS5OneMinusWorldSubsystem* Subsystem = GetWorld() != nullptr
        ? GetWorld()->GetSubsystem<UDLSS5OneMinusWorldSubsystem>() : nullptr)
    {
        Subsystem->RefreshEffectiveSettings();
    }
    Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void ADLSS5OneMinusSettingsActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    if (UWorld* World = GetWorld())
    {
        if (UDLSS5OneMinusWorldSubsystem* Subsystem = World->GetSubsystem<UDLSS5OneMinusWorldSubsystem>())
        {
            Subsystem->RefreshEffectiveSettings();
        }
    }
}
#endif
