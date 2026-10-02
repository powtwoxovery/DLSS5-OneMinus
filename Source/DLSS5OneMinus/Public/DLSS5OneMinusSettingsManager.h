#pragma once

#include "CoreMinimal.h"
#include "DLSS5OneMinusSettings.h"

class FSceneView;
class FSceneInterface;
class UWorld;

namespace DLSS5OneMinus
{
    DLSS5ONEMINUS_API FDLSS5OneMinusApplyResult ApplySettings(
        UWorld* World,
        const FDLSS5OneMinusSettings& Settings,
        bool bEnable,
        bool bAllowSceneCaptures,
        FName Source);

    DLSS5ONEMINUS_API FDLSS5OneMinusApplyResult ApplyProjectDefaults(UWorld* World, FName Source = TEXT("Project Default"));
    DLSS5ONEMINUS_API bool GetEffectiveSettings(const UWorld* World, FDLSS5OneMinusEffectiveState& OutState);
    DLSS5ONEMINUS_API FDLSS5OneMinusRenderSettings GetRenderSettings(const FSceneView& View);
    DLSS5ONEMINUS_API FDLSS5OneMinusRenderSettings GetRenderSettingsForScene(const FSceneInterface* Scene);
    DLSS5ONEMINUS_API void RemoveWorldSettings(const UWorld* World);
    DLSS5ONEMINUS_API void ResetAllSettingsState();
    DLSS5ONEMINUS_API FString GetConsoleOverrideSummary();
}
