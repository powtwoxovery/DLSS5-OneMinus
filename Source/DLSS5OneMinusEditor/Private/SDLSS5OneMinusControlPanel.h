#pragma once

#include "CoreMinimal.h"
#include "DLSS5OneMinusSettings.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

struct FAssetData;
class ACameraActor;
class IConsoleVariable;
class UDLSS5OneMinusProfile;
class UWorld;

class SDLSS5OneMinusControlPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDLSS5OneMinusControlPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
    struct FCameraOption
    {
        TWeakObjectPtr<ACameraActor> Camera;
        FText Label;
    };

    TSharedRef<SWidget> MakeProfileBar() const;
    TSharedRef<SWidget> MakeCameraControls() const;
    TSharedRef<SWidget> MakeSection(
        const FText& Eyebrow,
        const FText& Title,
        const FText& Description,
        TSharedRef<SWidget> Content) const;
    TSharedRef<SWidget> MakeScalarControl(
        const TCHAR* CVarName,
        const FText& Label,
        const FText& Description,
        float Minimum,
        float Maximum,
        float Step,
        float DefaultValue) const;
    TSharedRef<SWidget> MakeChoiceButton(
        const TCHAR* CVarName,
        int32 Choice,
        const FText& Label,
        const FText& ToolTip,
        bool bAllowDeselect = false) const;
    TSharedRef<SWidget> MakeDebugButton(
        int32 DebugMode,
        const FText& Label,
        const FText& ToolTip) const;
    TSharedRef<SWidget> MakeNeuralRenderingToggleButton() const;
    TSharedRef<SWidget> MakeToggleControl(
        const TCHAR* CVarName,
        const FText& Label,
        const FText& Description,
        bool bDefaultValue) const;

    float GetFloatCVar(const TCHAR* Name, float DefaultValue) const;
    int32 GetIntCVar(const TCHAR* Name, int32 DefaultValue) const;
    void SetFloatCVar(const TCHAR* Name, float Value) const;
    void SetIntCVar(const TCHAR* Name, int32 Value) const;
    void SetNeuralRenderingRequest(bool bEnable) const;
    void LoadInitialState() const;
    void ApplyWorkingSettings(FName Source = TEXT("Editor Working Copy")) const;
    UWorld* GetEditorWorld() const;
    void HandleProfileChanged(const FAssetData& AssetData) const;
    FReply SaveProfile() const;
    FReply SaveProfileAs() const;
    FReply ClearProfileSelection() const;
    FReply OpenHelp() const;
    void RefreshCameraOptions() const;
    TSharedRef<SWidget> GenerateCameraOptionWidget(TSharedPtr<FCameraOption> Option) const;
    void HandleCameraSelected(TSharedPtr<FCameraOption> Option, ESelectInfo::Type SelectionType) const;
    FText GetSelectedCameraText() const;
    FString GetProfileObjectPath() const;
    FText GetProfileStatusText() const;
    FSlateColor GetProfileStatusColor() const;
    bool IsProfileDirty() const;
    bool CanSaveProfile() const;
    float GetBaselineFloat(const TCHAR* Name, float Fallback) const;
    int32 GetBaselineInt(const TCHAR* Name, int32 Fallback) const;
    bool TryGetWorkingFloat(const TCHAR* Name, float& OutValue) const;
    bool TryGetWorkingInt(const TCHAR* Name, int32& OutValue) const;
    bool TrySetWorkingFloat(const TCHAR* Name, float Value) const;
    bool TrySetWorkingInt(const TCHAR* Name, int32 Value) const;
    bool HasDependencies() const;
    bool IsNRRequested() const;
    bool IsNRActive() const;
    bool IsChoiceAvailable(const TCHAR* CVarName, int32 Choice) const;
    int32 GetEffectiveRoute() const;
    IConsoleVariable* ResolveCVar(const TCHAR* Name) const;
    FReply ResetVerifiedDefaults() const;

    // Context gates, a single screen-percentage derivation path and effective-state resync:
    // each control is available only where it has an effect, and the viewport scale is derived in one place.
    void ApplyViewportScale() const;
    int32 GetRequestedScreenPercentage() const;
    bool IsPIEActive() const;
    bool IsNRControlAvailable() const;
    bool IsScalarAvailable(const TCHAR* CVarName) const;
    bool IsScreenPercentageLocked() const;
    bool CanToggleNeuralRendering() const;
    bool CanClearProfile() const;
    bool ConfirmDiscardEdits() const;
    bool IsRouteUnavailable() const;
    mutable int64 LastAppliedRevision = -1;
    mutable bool bViewportScalePending = false;
    mutable float LastExplicitSkinStructure = 1.0f;
    mutable TWeakObjectPtr<UWorld> LastAppliedWorld;

    FText GetRequestStatusText() const;
    FSlateColor GetRequestStatusColor() const;
    FText GetRouteStatusText() const;
    FText GetControlBusStatusText() const;
    FSlateColor GetControlBusStatusColor() const;
    FText GetRuntimeDetailText() const;

    mutable TMap<FString, IConsoleVariable*> ConsoleVariables;
    mutable TOptional<float> PendingScreenPercentage;
    mutable TOptional<int32> RequestedScreenPercentage;
    mutable TWeakObjectPtr<UDLSS5OneMinusProfile> SelectedProfile;
    mutable TArray<TSharedPtr<FCameraOption>> CameraOptions;
    mutable TSharedPtr<SComboBox<TSharedPtr<FCameraOption>>> CameraComboBox;
    mutable TSharedPtr<FCameraOption> SelectedCameraOption;
    mutable FDLSS5OneMinusSettings BaselineSettings;
    mutable FDLSS5OneMinusSettings WorkingSettings;
    mutable bool bWorkingEnabled = false;
};
