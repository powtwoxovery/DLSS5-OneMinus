#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DLSS5OneMinusSettings.h"
#include "DLSS5OneMinusBlueprintLibrary.generated.h"

class UDLSS5OneMinusProfile;

UCLASS()
class DLSS5ONEMINUS_API UDLSS5OneMinusBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "DLSS5-OneMinus", meta = (WorldContext = "WorldContextObject"))
    static FDLSS5OneMinusApplyResult ApplyProfile(
        const UObject* WorldContextObject,
        UDLSS5OneMinusProfile* Profile,
        bool bEnableNeuralRendering = true,
        bool bAllowSceneCaptures = false);

    UFUNCTION(BlueprintCallable, Category = "DLSS5-OneMinus", meta = (WorldContext = "WorldContextObject"))
    static FDLSS5OneMinusApplyResult SetNeuralRenderingEnabled(
        const UObject* WorldContextObject,
        bool bEnabled);

    UFUNCTION(BlueprintCallable, Category = "DLSS5-OneMinus", meta = (WorldContext = "WorldContextObject"))
    static FDLSS5OneMinusApplyResult RestoreProjectDefaults(const UObject* WorldContextObject);

    UFUNCTION(BlueprintPure, Category = "DLSS5-OneMinus", meta = (WorldContext = "WorldContextObject"))
    static bool GetEffectiveSettings(
        const UObject* WorldContextObject,
        FDLSS5OneMinusEffectiveState& OutState);
};
