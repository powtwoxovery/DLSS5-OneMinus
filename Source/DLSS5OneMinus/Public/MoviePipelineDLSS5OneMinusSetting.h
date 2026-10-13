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

    // Multi-view consistency (contract version 1). Plain types so a multi-view renderer can set
    // them by reflection without depending on this plugin. Off by default.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency")
    int32 ConsistencyContractVersion = 1;

    // 0 Off, 1 Consumer (use the shared reference tone), 2 Detail only (remove NR's per-view tone).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency", meta = (ClampMin = "0", ClampMax = "2"))
    int32 ConsistencyMode = 0;

    // Absolute path of the reference composite with a {frame} token.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency")
    FString ReferenceCompositePattern;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency", meta = (ClampMin = "1", ClampMax = "12"))
    int32 ReferenceFrameDigits = 8;

    // Added to this job's output frame index to form the reference frame key.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency")
    int32 ReferenceFrameOffset = 0;

    // Directory holding <viewportId>.exr view-to-reference maps and manifest.json.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency")
    FString ReferenceMapDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Multi-View Consistency")
    FString ReferenceViewportId;

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
