#include "DLSS5OneMinusBlueprintLibrary.h"

#include "DLSS5OneMinusProfile.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
    UWorld* ResolveWorld(const UObject* WorldContextObject)
    {
        return GEngine != nullptr
            ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
            : nullptr;
    }
}

FDLSS5OneMinusApplyResult UDLSS5OneMinusBlueprintLibrary::ApplyProfile(
    const UObject* WorldContextObject,
    UDLSS5OneMinusProfile* Profile,
    const bool bEnableNeuralRendering,
    const bool bAllowSceneCaptures)
{
    if (Profile == nullptr)
    {
        FDLSS5OneMinusApplyResult Result;
        Result.Reason = NSLOCTEXT("DLSS5OneMinus", "NullProfile", "No DLSS5-OneMinus profile was supplied.");
        return Result;
    }
    return DLSS5OneMinus::ApplySettings(
        ResolveWorld(WorldContextObject),
        Profile->Settings,
        bEnableNeuralRendering,
        bAllowSceneCaptures,
        FName(*FString::Printf(TEXT("Blueprint Profile: %s"), *Profile->GetName())));
}

FDLSS5OneMinusApplyResult UDLSS5OneMinusBlueprintLibrary::SetNeuralRenderingEnabled(
    const UObject* WorldContextObject,
    const bool bEnabled)
{
    UWorld* World = ResolveWorld(WorldContextObject);
    FDLSS5OneMinusEffectiveState Current;
    if (!DLSS5OneMinus::GetEffectiveSettings(World, Current))
    {
        // Honour bEnabled when the world has no effective state yet: use the project default settings
        // with the caller's enable state.
        const UDLSS5OneMinusProjectSettings* ProjectSettings = GetDefault<UDLSS5OneMinusProjectSettings>();
        return DLSS5OneMinus::ApplySettings(
            World,
            ProjectSettings->ResolveDefaultSettings(),
            bEnabled,
            ProjectSettings->bAllowSceneCapturesByDefault,
            TEXT("Blueprint Enable Override / Project Default"));
    }
    return DLSS5OneMinus::ApplySettings(
        World, Current.Settings, bEnabled, Current.bAllowSceneCaptures, TEXT("Blueprint Enable Override"));
}

FDLSS5OneMinusApplyResult UDLSS5OneMinusBlueprintLibrary::RestoreProjectDefaults(const UObject* WorldContextObject)
{
    return DLSS5OneMinus::ApplyProjectDefaults(ResolveWorld(WorldContextObject), TEXT("Blueprint Restore"));
}

bool UDLSS5OneMinusBlueprintLibrary::GetEffectiveSettings(
    const UObject* WorldContextObject,
    FDLSS5OneMinusEffectiveState& OutState)
{
    return DLSS5OneMinus::GetEffectiveSettings(ResolveWorld(WorldContextObject), OutState);
}
