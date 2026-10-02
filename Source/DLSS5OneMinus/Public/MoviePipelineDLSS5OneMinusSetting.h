#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineViewFamilySetting.h"
#include "DLSS5OneMinusSettings.h"
#include "MoviePipelineDLSS5OneMinusSetting.generated.h"

class UDLSS5OneMinusProfile;

UCLASS(BlueprintType, meta = (DisplayName = "DLSS5-OneMinus Neural Rendering"))
class DLSS5ONEMINUS_API UMoviePipelineDLSS5OneMinusSetting : public UMoviePipelineViewFamilySetting
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neural Rendering")
    TSoftObjectPtr<UDLSS5OneMinusProfile> Profile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neural Rendering")
    bool bEnableNeuralRendering = true;

    virtual void SetupViewFamily(FSceneViewFamily& ViewFamily) override;
    virtual void ValidateStateImpl() override;
    virtual void GetFormatArguments(FMoviePipelineFormatArgs& InOutFormatArgs) const override;
#if WITH_EDITOR
    virtual FText GetDisplayText() const override;
    virtual FText GetCategoryText() const override;
#endif

protected:
    virtual void SetupForPipelineImpl(UMoviePipeline* InPipeline) override;
    virtual void TeardownForPipelineImpl(UMoviePipeline* InPipeline) override;

private:
    UPROPERTY(Transient)
    FDLSS5OneMinusEffectiveState PreviousState;

    UPROPERTY(Transient)
    bool bHadPreviousState = false;
};
