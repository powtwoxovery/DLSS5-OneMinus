#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DLSS5OneMinusSettings.h"
#include "DLSS5OneMinusProfile.generated.h"

UCLASS(BlueprintType)
class DLSS5ONEMINUS_API UDLSS5OneMinusProfile : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profile")
    FText DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profile", meta = (MultiLine = "true"))
    FText Description;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Neural Rendering", meta = (ShowOnlyInnerProperties))
    FDLSS5OneMinusSettings Settings;
};
