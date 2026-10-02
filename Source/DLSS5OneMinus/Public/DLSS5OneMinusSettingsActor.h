#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DLSS5OneMinusSettings.h"
#include "DLSS5OneMinusSettingsActor.generated.h"

class UDLSS5OneMinusProfile;

UCLASS(BlueprintType, meta = (DisplayName = "DLSS5-OneMinus Settings"))
class DLSS5ONEMINUS_API ADLSS5OneMinusSettingsActor : public AActor
{
    GENERATED_BODY()

public:
    ADLSS5OneMinusSettingsActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Profile")
    TSoftObjectPtr<UDLSS5OneMinusProfile> Profile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Profile")
    bool bUseInlineSettings = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Profile", meta = (EditCondition = "bUseInlineSettings", ShowOnlyInnerProperties))
    FDLSS5OneMinusSettings InlineSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
    bool bEnableNeuralRendering = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
    bool bAllowSceneCaptures = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
    bool bApplyOnWorldStart = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
    int32 Priority = 0;

    UFUNCTION(BlueprintCallable, Category = "DLSS5-OneMinus")
    FDLSS5OneMinusApplyResult ApplyToWorld() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
