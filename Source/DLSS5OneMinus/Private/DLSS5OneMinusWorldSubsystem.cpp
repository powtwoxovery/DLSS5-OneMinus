#include "DLSS5OneMinusWorldSubsystem.h"

#include "DLSS5OneMinusSettingsActor.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "EngineUtils.h"

bool UDLSS5OneMinusWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const UWorld* World = Cast<UWorld>(Outer);
    if (World == nullptr)
    {
        return false;
    }
    switch (World->WorldType)
    {
    case EWorldType::Editor:
    case EWorldType::PIE:
    case EWorldType::Game:
    case EWorldType::GamePreview:
        return true;
    default:
        return false;
    }
}

void UDLSS5OneMinusWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    RefreshEffectiveSettings();
}

void UDLSS5OneMinusWorldSubsystem::Deinitialize()
{
    DLSS5OneMinus::RemoveWorldSettings(GetWorld());
    Super::Deinitialize();
}

void UDLSS5OneMinusWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    // Call the base UWorldSubsystem::OnWorldBeginPlay.
    // UE 5.7+ tracks bHasCalledBeginPlay in the base and raises an ensure ("check for missing Super::OnWorldBeginPlay call") on every PIE/MRQ start when it is skipped. 5.5/5.6 base is empty, so this is a no-op there.
    Super::OnWorldBeginPlay(InWorld);
    RefreshEffectiveSettings();
}

void UDLSS5OneMinusWorldSubsystem::RefreshEffectiveSettings()
{
    UWorld* World = GetWorld();
    if (World == nullptr || World->Scene == nullptr)
    {
        return;
    }

    const ADLSS5OneMinusSettingsActor* Winner = nullptr;
    for (TActorIterator<ADLSS5OneMinusSettingsActor> It(World); It; ++It)
    {
        const ADLSS5OneMinusSettingsActor* Candidate = *It;
        if (!IsValid(Candidate) || Candidate->IsActorBeingDestroyed() || !Candidate->bApplyOnWorldStart)
        {
            continue;
        }
        if (Winner == nullptr
            || Candidate->Priority > Winner->Priority
            || (Candidate->Priority == Winner->Priority
                && Candidate->GetPathName().Compare(Winner->GetPathName()) < 0))
        {
            Winner = Candidate;
        }
    }

    if (Winner != nullptr)
    {
        Winner->ApplyToWorld();
    }
    else
    {
        DLSS5OneMinus::ApplyProjectDefaults(World);
    }
}
