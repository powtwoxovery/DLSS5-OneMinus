#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DLSS5OneMinusSettings.h"
#include "DLSS5OneMinusProjectSettings.generated.h"

class UDLSS5OneMinusProfile;
class UWorld;

UCLASS(Config = Engine, DefaultConfig, meta = (DisplayName = "DLSS5-OneMinus Neural Rendering"))
class DLSS5ONEMINUS_API UDLSS5OneMinusProjectSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UDLSS5OneMinusProjectSettings();

    UPROPERTY(Config, EditAnywhere, Category = "Project Default", meta = (AllowedClasses = "/Script/DLSS5OneMinus.DLSS5OneMinusProfile"))
    TSoftObjectPtr<UDLSS5OneMinusProfile> DefaultProfile;

    UPROPERTY(Config, EditAnywhere, Category = "Project Default", meta = (ShowOnlyInnerProperties))
    FDLSS5OneMinusSettings DefaultSettings;

    UPROPERTY(Config, EditAnywhere, Category = "Activation")
    bool bEnableInGameWorlds = false;

    UPROPERTY(Config, EditAnywhere, Category = "Activation")
    bool bEnableInEditorWorlds = false;

    UPROPERTY(Config, EditAnywhere, Category = "Activation")
    bool bAllowSceneCapturesByDefault = false;

    UPROPERTY(Config, EditAnywhere, Category = "Editor Preview", meta = (ClampMin = "10", ClampMax = "200", UIMin = "10", UIMax = "200"))
    int32 EditorPreviewScreenPercentage = 100;

    UPROPERTY(Config, EditAnywhere, Category = "Editor Preview")
    bool bApplyLastEditorProfileWhenPanelOpens = true;

    FDLSS5OneMinusSettings ResolveDefaultSettings() const;
    bool ShouldEnableForWorld(const UWorld* World) const;

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("DLSS5-OneMinus"); }
};
