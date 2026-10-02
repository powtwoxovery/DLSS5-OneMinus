#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DLSS5OneMinusEditorUserSettings.generated.h"

class UDLSS5OneMinusProfile;

UCLASS(Config = EditorPerProjectUserSettings)
class UDLSS5OneMinusEditorUserSettings : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(Config)
    bool bHasSavedEditorState = false;

    UPROPERTY(Config)
    TSoftObjectPtr<UDLSS5OneMinusProfile> LastProfile;

    UPROPERTY(Config)
    bool bLastNRRequested = false;

    UPROPERTY(Config)
    int32 LastEditorScreenPercentage = 100;
};
