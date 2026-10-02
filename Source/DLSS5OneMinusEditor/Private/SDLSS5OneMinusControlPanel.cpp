#include "SDLSS5OneMinusControlPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "Camera/CameraActor.h"
#include "DLSS5OneMinusEditorUserSettings.h"
#include "DLSS5OneMinusEditorViewport.h"
#include "DLSS5OneMinusProfile.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusStatus.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Factories/DataAssetFactory.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SDLSS5OneMinusControlPanel"

namespace
{
    constexpr int32 GDLSS5OneMinusPreDLSSPath = 0;
    constexpr int32 GDLSS5OneMinusPostTonePath = 1;
    constexpr int32 GDLSS5OneMinusPostDLAAPath = 2;

    void FlushCVarsAndViewport()
    {
        IConsoleManager::Get().CallAllConsoleVariableSinks();
        DLSS5OneMinusEditorViewport::Invalidate();
    }
}

namespace DLSS5OneMinusControlPanelStyle
{
    // Instrument palette: a quiet hierarchy that sits below the surrounding editor chrome.
    static const FLinearColor Background(0.0108f, 0.0138f, 0.0180f, 1.0f);
    static const FLinearColor Surface(0.0210f, 0.0258f, 0.0330f, 1.0f);
    static const FLinearColor SurfaceRaised(0.0330f, 0.0402f, 0.0492f, 1.0f);
    static const FLinearColor Cyan(0.00f, 0.62f, 0.68f, 1.0f);
    static const FLinearColor Amber(0.624f, 0.256f, 0.056f, 1.0f);
    // selected-but-unavailable keeps the current choice readable instead of hiding it.
    static const FLinearColor AmberDim(0.190f, 0.098f, 0.040f, 1.0f);
    static const FLinearColor Green(0.31f, 0.88f, 0.56f, 1.0f);
    static const FLinearColor Muted(0.43f, 0.49f, 0.56f, 1.0f);
    static const FLinearColor Text(0.90f, 0.94f, 0.98f, 1.0f);
    static const FLinearColor IdleButton(0.300f, 0.314f, 0.328f, 1.0f);
    static const FLinearColor UnavailableButton(0.108f, 0.114f, 0.122f, 1.0f);
    static const FLinearColor IdleButtonText(0.015f, 0.018f, 0.020f, 1.0f);
    static const FLinearColor UnavailableButtonText(0.31f, 0.33f, 0.35f, 1.0f);
    static const FLinearColor ActiveButtonText(1.0f, 1.0f, 1.0f, 1.0f);

    static const FButtonStyle FlatButtonStyle = FButtonStyle()
        .SetNormal(FSlateNoResource())
        .SetHovered(FSlateNoResource())
        .SetPressed(FSlateNoResource())
        .SetDisabled(FSlateNoResource())
        .SetNormalPadding(FMargin(0.0f))
        .SetPressedPadding(FMargin(0.0f));

    static const FButtonStyle ResetButtonStyle = FButtonStyle()
        .SetNormal(FSlateRoundedBoxBrush(IdleButton, 3.0f))
        .SetHovered(FSlateRoundedBoxBrush(IdleButton, 3.0f))
        .SetPressed(FSlateRoundedBoxBrush(Cyan, 3.0f))
        .SetDisabled(FSlateRoundedBoxBrush(UnavailableButton, 3.0f))
        .SetNormalPadding(FMargin(0.0f))
        .SetPressedPadding(FMargin(0.0f));

    // Slider style with a 10 px thumb and 5 px bar for larger drag targets, and 18 px buttons
    // for a compact Neural Rendering Lab layout.
    static constexpr float ButtonHeight = 18.0f;

    static const FSliderStyle& LargeSliderStyle()
    {
        static const FSliderStyle Style = []()
        {
            FSliderStyle Result = FCoreStyle::Get().GetWidgetStyle<FSliderStyle>("Slider");
            constexpr float Scale = 1.25f;
            Result.NormalThumbImage.ImageSize *= Scale;
            Result.HoveredThumbImage.ImageSize *= Scale;
            Result.DisabledThumbImage.ImageSize *= Scale;
            Result.SetBarThickness(Result.BarThickness * Scale);
            return Result;
        }();
        return Style;
    }
}

// Neural Rendering Lab Slate panel: header, profile row, and the camera, composition and model sections.
// One dockable instrument that exposes every Neural Rendering control in a consistent hierarchy and palette.
void SDLSS5OneMinusControlPanel::Construct(const FArguments& InArgs)
{
    LoadInitialState();

    const TSharedRef<SWidget> PipelineControls =
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeNeuralRenderingToggleButton()
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
            [
                MakeDebugButton(
                    1,
                    LOCTEXT("NRDifferenceButton", "NR DIFFERENCE"),
                    LOCTEXT("NRDifferenceTip",
                        "Show the absolute RGB difference between the original and composed NR image.\n"
                        "Difference is amplified 4x for inspection.\n"
                        "Click again to return to the normal image."))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
            [
                MakeDebugButton(
                    2,
                    LOCTEXT("NRSliceButton", "NR SLICE"),
                    LOCTEXT("NRSliceTip",
                        "Show Original / NR / Difference in equal-width thirds.\n"
                        "The two slice dividers lean 20 degrees clockwise and use black separators.\n"
                        "Difference is amplified 4x for inspection; click again to return to normal."))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.SliceOffsetPercent"),
                LOCTEXT("NRSliceOffsetLabel", "NR slice offset (%)"),
                LOCTEXT("NRSliceOffsetDescription",
                    "Move the angled Original / NR / Difference slices horizontally.\n"
                    "0% is the default composition; 100% wraps back to the same position."),
                0.0f,
                100.0f,
                1.0f,
                0.0f)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("RouteLabel", "PIPELINE ROUTE"))
                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Path"),
                        GDLSS5OneMinusPreDLSSPath,
                        LOCTEXT("PreDLSSRoute", "PRE DLSS"),
                        LOCTEXT("PreDLSSRouteTip",
                            "Runs NR before DLSS Super Resolution.\n"
                            "Scaled frames only: while NR is on the viewport is held within 33-99%.\n"
                            "Your requested screen percentage is restored when NR is turned off."))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Path"),
                        GDLSS5OneMinusPostDLAAPath,
                        LOCTEXT("PostDLAARoute", "POST DLAA"),
                        LOCTEXT("PostDLAARouteTip",
                            "Runs NR immediately after native-resolution DLAA.\n"
                            "Native frames only: while NR is on the viewport is held at 100%.\n"
                            "Your requested screen percentage is restored when NR is turned off."))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Path"),
                        GDLSS5OneMinusPostTonePath,
                        LOCTEXT("PostToneRoute", "POST TONE"),
                        LOCTEXT("PostToneRouteTip",
                            "Runs NR after UE tonemapping and before UI.\n"
                            "Works at 10-200% with native DLAA or scaled DLSS Super Resolution.\n"
                            "Requires SDR sRGB output and the tonemap pass. HDR composition settings do not apply.\n"
                            "Default route."))
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.Strength"),
                LOCTEXT("CompositionLabel", "Composition strength"),
                LOCTEXT("CompositionDescription",
                    "Blend of the neural edit into the selected pipeline route.\n"
                    "0 = original, 1 = full NR edit (default), up to 2 = exaggerated edit."),
                0.0f,
                2.0f, // range 0..2
                0.01f,
                1.0f)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("HDRCompositionLabel", "HDR COMPOSITION / PRE DLSS + POST DLAA ONLY"))
                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.HDRComposition"),
                        0,
                        LOCTEXT("HDRAdditive", "ADDITIVE"),
                        LOCTEXT("HDRAdditiveTip", "Validated absolute HDR delta.\nDefault pre-tonemap composition."))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.HDRComposition"),
                        1,
                        LOCTEXT("HDRRatio", "GUARDED RATIO"),
                        LOCTEXT("HDRRatioTip",
                            "Applies a linear NR/proxy gain bounded to 0.5-2x.\n"
                            "Near-black channels fall back to additive composition.\n"
                            "Experimental."))
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.WhitePoint"),
                LOCTEXT("WhitePointLabel", "HDR model white"),
                LOCTEXT("WhitePointDescription",
                    "Linear HDR value mapped to model-domain white.\n"
                    "Used by Pre DLSS and Post DLAA; Post Tone works in display space and ignores it."),
                0.25f,
                4.0f,
                0.05f,
                1.0f)
        ];

    const TSharedRef<SWidget> ModelControls =
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("StyleLabel", "MODEL CHARACTER"))
                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Style"),
                        0,
                        LOCTEXT("StyleDefault", "DEFAULT"),
                        LOCTEXT("StyleDefaultTip", "Runtime style 0.\nVerified default."))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Style"),
                        1,
                        LOCTEXT("StyleNatural", "NATURAL"),
                        LOCTEXT("StyleNaturalTip", "Runtime style 1.\nStays closer to source tonality."))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
                [
                    MakeChoiceButton(
                        TEXT("r.NGX.DLSSNR.Style"),
                        2,
                        LOCTEXT("StyleCinematic", "CINEMATIC"),
                        LOCTEXT("StyleCinematicTip", "Runtime style 2.\nStronger contrast and grade."))
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.Intensity"),
                LOCTEXT("IntensityLabel", "Neural intensity"),
                LOCTEXT("IntensityDescription", "Overall model influence."),
                0.0f,
                2.0f,
                0.01f,
                1.0f)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.LocalToneStrength"),
                LOCTEXT("ToneLabel", "Local tone"),
                LOCTEXT("ToneDescription", "Low-frequency local contrast and lighting response."),
                0.0f,
                2.0f,
                0.01f,
                1.0f)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.LocalStructureStrength"),
                LOCTEXT("StructureLabel", "Local structure"),
                LOCTEXT("StructureDescription", "High-frequency detail and material reconstruction."),
                0.0f,
                2.0f,
                0.01f,
                1.0f)
        ]
        // Skin structure: an AUTO toggle (-1, the model decides) and an explicit 0-2 strength slider.
        // 0 removes skin detail; values above 0 set an explicit strength.
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            MakeToggleControl(
                TEXT("DLSS5OneMinus.SkinStructureAuto"),
                LOCTEXT("SkinAutoLabel", "Skin structure: AUTO (model decides)"),
                LOCTEXT("SkinAutoDescription",
                    "On: the model chooses skin reconstruction itself.\n"
                    "Off: use the Skin structure slider below for an explicit strength.\n"
                    "Requires Automatic subject mask."),
                true)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MakeScalarControl(
                TEXT("r.NGX.DLSSNR.SkinStructureStrength"),
                LOCTEXT("SkinLabel", "Skin structure"),
                LOCTEXT("SkinDescription",
                    "Explicit skin reconstruction strength: 0 = none (skin detail removed), 1 = normal, 2 = maximum.\n"
                    "Available when Skin structure AUTO is off and Automatic subject mask is on."),
                0.0f,
                2.0f,
                0.01f,
                1.0f)
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            MakeToggleControl(
                TEXT("r.NGX.DLSSNR.AutoMask"),
                LOCTEXT("AutoMaskLabel", "Automatic subject mask"),
                LOCTEXT("AutoMaskDescription",
                    "Let the model locate skin and subject regions. Required for explicit skin tuning."),
                true)
        ];

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(DLSS5OneMinusControlPanelStyle::Background)
        .Padding(0.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(DLSS5OneMinusControlPanelStyle::SurfaceRaised)
                .Padding(FMargin(12.0f, 8.0f))
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ResearchEyebrow", "DLSS 5 / UNOFFICIAL RESEARCH INSTRUMENT"))
                                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Amber)
                            ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("PanelTitle", "NEURAL RENDERING LAB"))
                                .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
                                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Text)
                            ]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text(this, &SDLSS5OneMinusControlPanel::GetRequestStatusText)
                            .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                            .ColorAndOpacity(this, &SDLSS5OneMinusControlPanel::GetRequestStatusColor)
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text(this, &SDLSS5OneMinusControlPanel::GetRouteStatusText)
                            .Font(FAppStyle::GetFontStyle("SmallFont"))
                            .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Cyan)
                        ]
                        + SHorizontalBox::Slot().AutoWidth()
                        [
                            SNew(STextBlock)
                            .Text(this, &SDLSS5OneMinusControlPanel::GetControlBusStatusText)
                            .Font(FAppStyle::GetFontStyle("SmallFont"))
                            .ColorAndOpacity(this, &SDLSS5OneMinusControlPanel::GetControlBusStatusColor)
                            .ToolTipText(this, &SDLSS5OneMinusControlPanel::GetRuntimeDetailText)
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(10.0f, 6.0f, 10.0f, 0.0f)
            [
                MakeProfileBar()
            ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot().Padding(FMargin(10.0f, 8.0f, 10.0f, 6.0f))
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        MakeSection(
                            LOCTEXT("CameraEyebrow", "00 / CAMERA"),
                            LOCTEXT("CameraTitle", "Viewport"),
                            LOCTEXT("CameraDescription",
                                "Switch the active editor viewport to a scene camera and control its render scale."),
                            MakeCameraControls())
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        MakeSection(
                            LOCTEXT("PipelineEyebrow", "01 / COMPOSITION"),
                            LOCTEXT("PipelineTitle", "Pipeline"),
                            LOCTEXT("PipelineDescription",
                                "Choose where the neural pass enters the frame and how strongly its edit is composed."),
                            PipelineControls)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        MakeSection(
                            LOCTEXT("ModelEyebrow", "02 / MODEL SIGNAL"),
                            LOCTEXT("ModelTitle", "Neural character"),
                            LOCTEXT("ModelDescription",
                                "These unofficial parameters are sent to the model on every evaluation; "
                                "all changes, including model character, apply on the next frame."),
                            ModelControls)
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(DLSS5OneMinusControlPanelStyle::SurfaceRaised)
                .Padding(FMargin(10.0f, 6.0f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("FooterCopy",
                            "EDITOR VIEWPORT  /  POST TONE works at any scale  /  NR-off keeps DLSS on"))
                        .Font(FAppStyle::GetFontStyle("SmallFont"))
                        .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SBorder)
                        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                        .BorderBackgroundColor_Lambda([this]()
                        {
                            return IsNRControlAvailable()
                                ? DLSS5OneMinusControlPanelStyle::IdleButton
                                : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                        })
                        .Padding(0.0f)
                        [
                            SNew(SButton)
                            .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                            .IsEnabled(this, &SDLSS5OneMinusControlPanel::IsNRControlAvailable)
                            .ToolTipText(LOCTEXT("ResetButtonTip",
                                "Reset route, composition, HDR and model parameters to the verified defaults,\n"
                                "and clear the Difference/Slice view. Available while NR is on."))
                            .ContentPadding(FMargin(6.0f, 2.0f))
                            .OnClicked(this, &SDLSS5OneMinusControlPanel::ResetVerifiedDefaults)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ResetButton", "RESET VERIFIED DEFAULTS"))
                                .Font(FAppStyle::GetFontStyle("SmallFont"))
                                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::IdleButtonText)
                            ]
                        ]
                    ]
                ]
            ]
        ]
    ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeProfileBar() const
{
    // profile buttons take an availability rule and show it.
    auto MakeProfileButton = [](
        const FText& Label,
        const FText& ToolTip,
        const FOnClicked& OnClicked,
        TFunction<bool()> IsAvailable = [] { return true; })
    {
        return SNew(SBox)
            .HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor_Lambda([IsAvailable]()
                {
                    return IsAvailable()
                        ? DLSS5OneMinusControlPanelStyle::IdleButton
                        : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                })
                .Padding(0.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                    .HAlign(HAlign_Center)
                    .VAlign(VAlign_Center)
                    .IsEnabled_Lambda([IsAvailable]() { return IsAvailable(); })
                    .ToolTipText(ToolTip)
                    .ContentPadding(FMargin(6.0f, 1.0f))
                    .OnClicked(OnClicked)
                    [
                        SNew(STextBlock)
                        .Text(Label)
                        .Font(FAppStyle::GetFontStyle("SmallFont"))
                        .ColorAndOpacity_Lambda([IsAvailable]()
                        {
                            return IsAvailable()
                                ? FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText)
                                : FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                        })
                    ]
                ]
            ];
    };

    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(DLSS5OneMinusControlPanelStyle::Surface)
        .Padding(FMargin(8.0f, 5.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox).WidthOverride(48.0f)
                    [
                        MakeProfileButton(
                            LOCTEXT("OpenHelp", "HELP"),
                            LOCTEXT("OpenHelpTip", "Open the packaged DLSS5-OneMinus Neural Rendering Lab guide in your default browser."),
                            FOnClicked::CreateSP(this, &SDLSS5OneMinusControlPanel::OpenHelp))
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ProfileLabel", "PROFILE"))
                    .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                    .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                [
                    SNew(SObjectPropertyEntryBox)
                    .IsEnabled_Lambda([this]() { return !IsPIEActive(); })
                    .AllowedClass(UDLSS5OneMinusProfile::StaticClass())
                    .ObjectPath(this, &SDLSS5OneMinusControlPanel::GetProfileObjectPath)
                    .OnObjectChanged(this, &SDLSS5OneMinusControlPanel::HandleProfileChanged)
                    .AllowClear(true)
                    .DisplayUseSelected(true)
                    .DisplayBrowse(true)
                    .ToolTipText(LOCTEXT("ProfileSelectorTip",
                        "Choose a DLSS5-OneMinus profile for this editor preview.\n"
                        "The selection survives editor restarts; PIE and packaged games use the project default or a level settings actor."))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(this, &SDLSS5OneMinusControlPanel::GetProfileStatusText)
                    .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                    .ColorAndOpacity(this, &SDLSS5OneMinusControlPanel::GetProfileStatusColor)
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                [
                    MakeProfileButton(
                        LOCTEXT("ClearProfile", "CLEAR"),
                        LOCTEXT("ClearProfileTip", "Clear the selected profile and reload the shared Project Settings defaults.\nAsks before discarding unsaved edits."),
                        FOnClicked::CreateSP(this, &SDLSS5OneMinusControlPanel::ClearProfileSelection),
                        [this]() { return CanClearProfile(); })
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                [
                    SNew(SBox).HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
                    [
                        SNew(SBorder)
                        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                        .BorderBackgroundColor_Lambda([this]()
                        {
                            return CanSaveProfile()
                                ? DLSS5OneMinusControlPanelStyle::IdleButton
                                : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                        })
                        .Padding(0.0f)
                        [
                            SNew(SButton)
                            .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                            .HAlign(HAlign_Center)
                            .VAlign(VAlign_Center)
                            .IsEnabled(this, &SDLSS5OneMinusControlPanel::CanSaveProfile)
                            .ToolTipText(LOCTEXT("SaveProfileTip", "Save the working image settings into the selected project profile."))
                            .ContentPadding(FMargin(6.0f, 1.0f))
                            .OnClicked(this, &SDLSS5OneMinusControlPanel::SaveProfile)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("SaveProfile", "SAVE"))
                                .Font(FAppStyle::GetFontStyle("SmallFont"))
                                .ColorAndOpacity_Lambda([this]()
                                {
                                    return CanSaveProfile()
                                        ? FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText)
                                        : FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                                })
                            ]
                        ]
                    ]
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f, 0.0f, 0.0f)
                [
                    MakeProfileButton(
                        LOCTEXT("SaveProfileAs", "SAVE AS"),
                        LOCTEXT("SaveProfileAsTip",
                            "Create a profile from the working image settings.\n"
                            "When a profile is selected, the dialog starts with its folder and name."),
                        FOnClicked::CreateSP(this, &SDLSS5OneMinusControlPanel::SaveProfileAs),
                        [this]() { return !IsPIEActive(); })
                ]
            ]
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeCameraControls() const
{
    RefreshCameraOptions();

    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SAssignNew(CameraComboBox, SComboBox<TSharedPtr<FCameraOption>>)
            .IsEnabled_Lambda([this]() { return !IsPIEActive(); })
            .OptionsSource(&CameraOptions)
            .OnComboBoxOpening(this, &SDLSS5OneMinusControlPanel::RefreshCameraOptions)
            .OnGenerateWidget(this, &SDLSS5OneMinusControlPanel::GenerateCameraOptionWidget)
            .OnSelectionChanged(this, &SDLSS5OneMinusControlPanel::HandleCameraSelected)
            .ToolTipText(LOCTEXT("CameraSelectorTip",
                "Choose a Camera Actor or Cine Camera Actor and switch the active editor viewport to pilot that camera."))
            [
                SNew(STextBlock)
                .Text(this, &SDLSS5OneMinusControlPanel::GetSelectedCameraText)
                .Font(FAppStyle::GetFontStyle("SmallFont"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Text)
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            MakeScalarControl(
                TEXT("r.Editor.Viewport.ScreenPercentage"),
                LOCTEXT("ViewportScreenPercentageLabel", "Viewport screen percentage"),
                LOCTEXT("ViewportScreenPercentageDescription",
                    "Manual Level Editor viewport resolution percentage. 100% is native; values below 100% scale down and values above 100% supersample up to 200%.\n"
                    "Your value is kept when NR or the route changes. While NR is on: Post Tone renders 10-200%, Pre DLSS is held within 33-99%, and Post DLAA is locked at 100%.\n"
                    "With NR off the editor renders 25-200%.\n"
                    "The dragged value is applied on release so fixed-resolution DLSS modes can transition safely."),
                10.0f,
                200.0f,
                1.0f,
                100.0f)
        ];
}

// Open the plugin-packaged Neural Rendering Lab HTML guide from the profile row.
// Operators need concise workflow and feature help at the point of use, packaged with the plugin.
FReply SDLSS5OneMinusControlPanel::OpenHelp() const
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DLSS5OneMinus"));
    if (!Plugin.IsValid())
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            LOCTEXT("HelpPluginMissing", "DLSS5-OneMinus could not resolve its plugin directory, so local help cannot be opened."));
        return FReply::Handled();
    }

    const FString HelpPath = FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Resources/Help/DLSS5OneMinusHelp.html"));
    if (!IFileManager::Get().FileExists(*HelpPath))
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("HelpFileMissing", "DLSS5-OneMinus local help is missing:\n{0}"),
                FText::FromString(HelpPath)));
        return FReply::Handled();
    }

    FPlatformProcess::LaunchFileInDefaultExternalApplication(*HelpPath);
    return FReply::Handled();
}

void SDLSS5OneMinusControlPanel::RefreshCameraOptions() const
{
    const TWeakObjectPtr<ACameraActor> PreviouslySelected = SelectedCameraOption.IsValid()
        ? SelectedCameraOption->Camera
        : TWeakObjectPtr<ACameraActor>();

    CameraOptions.Reset();
    if (UWorld* World = GetEditorWorld())
    {
        for (TActorIterator<ACameraActor> It(World); It; ++It)
        {
            ACameraActor* Camera = *It;
            if (!IsValid(Camera) || Camera->IsActorBeingDestroyed())
            {
                continue;
            }

            TSharedPtr<FCameraOption> Option = MakeShared<FCameraOption>();
            Option->Camera = Camera;
            Option->Label = FText::FromString(Camera->GetActorLabel());
            CameraOptions.Add(Option);
        }
    }

    CameraOptions.Sort([](const TSharedPtr<FCameraOption>& Left, const TSharedPtr<FCameraOption>& Right)
    {
        return Left.IsValid() && Right.IsValid()
            ? Left->Label.ToString() < Right->Label.ToString()
            : Left.IsValid();
    });

    SelectedCameraOption.Reset();
    if (PreviouslySelected.IsValid())
    {
        for (const TSharedPtr<FCameraOption>& Option : CameraOptions)
        {
            if (Option.IsValid() && Option->Camera == PreviouslySelected)
            {
                SelectedCameraOption = Option;
                break;
            }
        }
    }

    if (CameraComboBox.IsValid())
    {
        CameraComboBox->RefreshOptions();
        CameraComboBox->SetSelectedItem(SelectedCameraOption);
    }
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::GenerateCameraOptionWidget(
    const TSharedPtr<FCameraOption> Option) const
{
    return SNew(STextBlock)
        .Text(Option.IsValid() ? Option->Label : LOCTEXT("InvalidCameraOption", "Unavailable camera"))
        .Font(FAppStyle::GetFontStyle("SmallFont"));
}

void SDLSS5OneMinusControlPanel::HandleCameraSelected(
    const TSharedPtr<FCameraOption> Option,
    const ESelectInfo::Type SelectionType) const
{
    if (SelectionType == ESelectInfo::Direct)
    {
        return;
    }
    if (!Option.IsValid() || !Option->Camera.IsValid())
    {
        return;
    }

    if (DLSS5OneMinusEditorViewport::SetViewFromCamera(Option->Camera.Get()))
    {
        SelectedCameraOption = Option;
    }
}

FText SDLSS5OneMinusControlPanel::GetSelectedCameraText() const
{
    return SelectedCameraOption.IsValid() && SelectedCameraOption->Camera.IsValid()
        ? SelectedCameraOption->Label
        : LOCTEXT("SelectCamera", "SELECT CAMERA");
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeSection(
    const FText& Eyebrow,
    const FText& Title,
    const FText& Description,
    TSharedRef<SWidget> Content) const
{
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(DLSS5OneMinusControlPanelStyle::Surface)
        .Padding(FMargin(10.0f, 8.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(Eyebrow)
                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Cyan)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(Title)
                .Font(FAppStyle::GetFontStyle("NormalFontBold"))
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Text)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 7.0f)
            [
                SNew(STextBlock)
                .Text(Description)
                .Font(FAppStyle::GetFontStyle("SmallFont"))
                .AutoWrapText(true)
                .ColorAndOpacity(DLSS5OneMinusControlPanelStyle::Muted)
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                Content
            ]
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeScalarControl(
    const TCHAR* CVarName,
    const FText& Label,
    const FText& Description,
    const float Minimum,
    const float Maximum,
    const float Step,
    const float DefaultValue) const
{
    const bool bDeferredScreenPercentage =
        FCString::Strcmp(CVarName, TEXT("r.Editor.Viewport.ScreenPercentage")) == 0;
    // the whole row (label, value, R, slider) follows the control's context rule.
    return SNew(SVerticalBox)
        .ToolTipText(Description)
        .IsEnabled_Lambda([this, CVarName]() { return IsScalarAvailable(CVarName); })
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                .ColorAndOpacity_Lambda([this, CVarName]()
                {
                    return IsScalarAvailable(CVarName)
                        ? FSlateColor(DLSS5OneMinusControlPanelStyle::Text)
                        : FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(64.0f)
                [
                    SNew(SNumericEntryBox<float>)
                    .Value_Lambda([this, CVarName, DefaultValue, bDeferredScreenPercentage]()
                    {
                        if (bDeferredScreenPercentage && PendingScreenPercentage.IsSet())
                        {
                            return PendingScreenPercentage;
                        }
                        return TOptional<float>(GetFloatCVar(CVarName, DefaultValue));
                    })
                    .MinValue(Minimum)
                    .MaxValue(Maximum)
                    .MinSliderValue(Minimum)
                    .MaxSliderValue(Maximum)
                    .Delta(Step)
                    .AllowSpin(true)
                    .OnValueChanged_Lambda([this, CVarName, Minimum, Maximum, bDeferredScreenPercentage](const float Value)
                    {
                        const float ClampedValue = FMath::Clamp(Value, Minimum, Maximum);
                        if (bDeferredScreenPercentage)
                        {
                            // Only a spin drag is pending; a typed value arrives via commit,
                            // so the display always reflects the applied value and any route lock.
                            if (FSlateApplication::Get().HasAnyMouseCaptor())
                            {
                                PendingScreenPercentage = ClampedValue;
                            }
                            return;
                        }
                        SetFloatCVar(CVarName, ClampedValue);
                    })
                    .OnValueCommitted_Lambda([this, CVarName, Minimum, Maximum, bDeferredScreenPercentage](
                        const float Value,
                        ETextCommit::Type CommitType)
                    {
                        // leave text mode on Enter; a focused spin box keeps
                        // showing typed text and would not reflect a later route lock (e.g. Post DLAA -> 100).
                        if (CommitType == ETextCommit::OnEnter)
                        {
                            FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
                        }
                        if (!bDeferredScreenPercentage)
                        {
                            return;
                        }
                        const float ClampedValue = FMath::Clamp(Value, Minimum, Maximum);
                        PendingScreenPercentage.Reset();
                        SetFloatCVar(CVarName, ClampedValue);
                    })
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SBox)
                .WidthOverride(11.0f)
                .HeightOverride(20.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&DLSS5OneMinusControlPanelStyle::ResetButtonStyle)
                    .HAlign(HAlign_Center)
                    .VAlign(VAlign_Center)
                    .IsEnabled_Lambda([this, CVarName, DefaultValue]()
                    {
                        const float Baseline = GetBaselineFloat(CVarName, DefaultValue);
                        return !FMath::IsNearlyEqual(GetFloatCVar(CVarName, DefaultValue), Baseline);
                    })
                    .ToolTipText_Lambda([this, CVarName, DefaultValue, Label]()
                    {
                        return FText::Format(
                            LOCTEXT("ResetScalarTip", "Reset {0} to its baseline (selected profile, project setting or default).\nBaseline: {1}"),
                            Label,
                            FText::AsNumber(GetBaselineFloat(CVarName, DefaultValue)));
                    })
                    .ContentPadding(0.0f)
                    .OnClicked_Lambda([this, CVarName, DefaultValue, bDeferredScreenPercentage]()
                    {
                        if (bDeferredScreenPercentage)
                        {
                            PendingScreenPercentage.Reset();
                        }
                        SetFloatCVar(CVarName, GetBaselineFloat(CVarName, DefaultValue));
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ResetScalarGlyph", "R"))
                        .Font(FAppStyle::GetFontStyle("SmallFontBold"))
                        .ColorAndOpacity_Lambda([this, CVarName, DefaultValue]()
                        {
                            const float Baseline = GetBaselineFloat(CVarName, DefaultValue);
                            return FMath::IsNearlyEqual(GetFloatCVar(CVarName, DefaultValue), Baseline)
                                ? FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText)
                                : FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText);
                        })
                    ]
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
        [
            SNew(SSlider)
            .Style(&DLSS5OneMinusControlPanelStyle::LargeSliderStyle())
            .Value_Lambda([this, CVarName, Minimum, Maximum, DefaultValue, bDeferredScreenPercentage]()
            {
                const float DisplayValue =
                    bDeferredScreenPercentage && PendingScreenPercentage.IsSet()
                        ? PendingScreenPercentage.GetValue()
                        : GetFloatCVar(CVarName, DefaultValue);
                return (DisplayValue - Minimum) / (Maximum - Minimum);
            })
            .StepSize(Step / (Maximum - Minimum))
            .SliderBarColor(DLSS5OneMinusControlPanelStyle::SurfaceRaised)
            .SliderHandleColor(DLSS5OneMinusControlPanelStyle::Cyan)
            .OnValueChanged_Lambda([this, CVarName, Minimum, Maximum, bDeferredScreenPercentage](const float Value)
            {
                const float ScalarValue = Minimum + Value * (Maximum - Minimum);
                if (bDeferredScreenPercentage && FSlateApplication::Get().HasAnyMouseCaptor())
                {
                    PendingScreenPercentage = ScalarValue;
                    return;
                }
                if (bDeferredScreenPercentage)
                {
                    PendingScreenPercentage.Reset();
                }
                SetFloatCVar(CVarName, ScalarValue);
            })
            .OnMouseCaptureEnd_Lambda([this, CVarName, Minimum, Maximum, bDeferredScreenPercentage]()
            {
                if (!bDeferredScreenPercentage || !PendingScreenPercentage.IsSet())
                {
                    return;
                }
                const float CommittedValue = FMath::Clamp(
                    PendingScreenPercentage.GetValue(),
                    Minimum,
                    Maximum);
                PendingScreenPercentage.Reset();
                SetFloatCVar(CVarName, CommittedValue);
            })
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeChoiceButton(
    const TCHAR* CVarName,
    const int32 Choice,
    const FText& Label,
    const FText& ToolTip,
    const bool bAllowDeselect) const
{
    return SNew(SBox)
        .HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this, CVarName, Choice]()
            {
                if (!IsChoiceAvailable(CVarName, Choice))
                {
                    return GetIntCVar(CVarName, 0) == Choice
                        ? DLSS5OneMinusControlPanelStyle::AmberDim
                        : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                }
                return GetIntCVar(CVarName, 0) == Choice
                    ? DLSS5OneMinusControlPanelStyle::Amber
                    : DLSS5OneMinusControlPanelStyle::IdleButton;
            })
            .Padding(0.0f)
            [
                SNew(SButton)
                .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .IsEnabled_Lambda([this, CVarName, Choice]()
                {
                    return IsChoiceAvailable(CVarName, Choice);
                })
                .ToolTipText(ToolTip)
                .ContentPadding(FMargin(6.0f, 1.0f))
                .OnClicked_Lambda([this, CVarName, Choice, bAllowDeselect]()
                {
                    if (!IsChoiceAvailable(CVarName, Choice))
                    {
                        return FReply::Handled();
                    }
                    const int32 NewChoice =
                        bAllowDeselect && GetIntCVar(CVarName, 0) == Choice ? 0 : Choice;
                    SetIntCVar(CVarName, NewChoice);
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(Label)
                    .Justification(ETextJustify::Center)
                    .Font_Lambda([this, CVarName, Choice]()
                    {
                        const bool bSelected = GetIntCVar(CVarName, 0) == Choice;
                        return FAppStyle::GetFontStyle(bSelected ? "SmallFontBold" : "SmallFont");
                    })
                    .ColorAndOpacity_Lambda([this, CVarName, Choice]()
                    {
                        if (!IsChoiceAvailable(CVarName, Choice))
                        {
                            return FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                        }
                        return GetIntCVar(CVarName, 0) == Choice
                            ? FSlateColor(DLSS5OneMinusControlPanelStyle::ActiveButtonText)
                            : FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText);
                    })
                ]
            ]
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeDebugButton(
    const int32 DebugMode,
    const FText& Label,
    const FText& ToolTip) const
{
    return SNew(SBox)
        .HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this, DebugMode]()
            {
                if (!IsChoiceAvailable(TEXT("r.NGX.DLSSNR.DebugView"), DebugMode))
                {
                    return GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0) == DebugMode
                        ? DLSS5OneMinusControlPanelStyle::AmberDim
                        : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                }
                return GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0) == DebugMode
                    ? DLSS5OneMinusControlPanelStyle::Amber
                    : DLSS5OneMinusControlPanelStyle::IdleButton;
            })
            .Padding(0.0f)
            [
                SNew(SButton)
                .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .IsEnabled_Lambda([this, DebugMode]()
                {
                    return IsChoiceAvailable(TEXT("r.NGX.DLSSNR.DebugView"), DebugMode);
                })
                .ToolTipText(ToolTip)
                .ContentPadding(FMargin(6.0f, 1.0f))
                .OnClicked_Lambda([this, DebugMode]()
                {
                    const int32 Current = GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
                    SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), Current == DebugMode ? 0 : DebugMode);
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(Label)
                    .Justification(ETextJustify::Center)
                    .Font_Lambda([this, DebugMode]()
                    {
                        return FAppStyle::GetFontStyle(
                            GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0) == DebugMode
                                ? "SmallFontBold"
                                : "SmallFont");
                    })
                    .ColorAndOpacity_Lambda([this, DebugMode]()
                    {
                        if (!IsChoiceAvailable(TEXT("r.NGX.DLSSNR.DebugView"), DebugMode))
                        {
                            return FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                        }
                        return GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0) == DebugMode
                            ? FSlateColor(DLSS5OneMinusControlPanelStyle::ActiveButtonText)
                            : FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText);
                    })
                ]
            ]
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeNeuralRenderingToggleButton() const
{
    return SNew(SBox)
        .HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this]()
            {
                if (!CanToggleNeuralRendering())
                {
                    return DLSS5OneMinusControlPanelStyle::UnavailableButton;
                }
                return IsNRActive()
                    ? DLSS5OneMinusControlPanelStyle::Amber
                    : DLSS5OneMinusControlPanelStyle::IdleButton;
            })
            .Padding(0.0f)
            [
                SNew(SButton)
                .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .IsEnabled(this, &SDLSS5OneMinusControlPanel::CanToggleNeuralRendering)
                .ToolTipText(LOCTEXT("EnableDescription",
                    "Enable or disable Neural Rendering for the editor viewport.\n"
                    "Orange means the isolated feature completed on the selected route.\n"
                    "A requested but unavailable or pending route remains gray; hover DEPENDENCIES status for details.\n"
                    "Every control below is available only while NR is on. Turning NR off is always possible; turning it on needs dependencies and no PIE session."))
                .ContentPadding(FMargin(6.0f, 1.0f))
                .OnClicked_Lambda([this]()
                {
                    SetNeuralRenderingRequest(!IsNRRequested());
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        if (IsNRActive())
                        {
                            return LOCTEXT("NREnabledButton", "NR ENABLED");
                        }
                        if (IsNRRequested())
                        {
                            return LOCTEXT("NRPendingButton", "NR REQUESTED / NOT ACTIVE");
                        }
                        return LOCTEXT("NRDisabledButton", "NR DISABLED");
                    })
                    .Font_Lambda([this]()
                    {
                        return FAppStyle::GetFontStyle(IsNRActive() ? "SmallFontBold" : "SmallFont");
                    })
                    .ColorAndOpacity_Lambda([this]()
                    {
                        if (!CanToggleNeuralRendering())
                        {
                            return FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                        }
                        return IsNRActive()
                            ? FSlateColor(DLSS5OneMinusControlPanelStyle::ActiveButtonText)
                            : FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText);
                    })
                ]
            ]
        ];
}

TSharedRef<SWidget> SDLSS5OneMinusControlPanel::MakeToggleControl(
    const TCHAR* CVarName,
    const FText& Label,
    const FText& Description,
    const bool bDefaultValue) const
{
    return SNew(SBox)
        .HeightOverride(DLSS5OneMinusControlPanelStyle::ButtonHeight)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor_Lambda([this, CVarName, bDefaultValue]()
            {
                if (!IsChoiceAvailable(CVarName, 1))
                {
                    return GetIntCVar(CVarName, bDefaultValue ? 1 : 0) != 0
                        ? DLSS5OneMinusControlPanelStyle::AmberDim
                        : DLSS5OneMinusControlPanelStyle::UnavailableButton;
                }
                return GetIntCVar(CVarName, bDefaultValue ? 1 : 0) != 0
                    ? DLSS5OneMinusControlPanelStyle::Amber
                    : DLSS5OneMinusControlPanelStyle::IdleButton;
            })
            .Padding(0.0f)
            [
                SNew(SButton)
                .ButtonStyle(&DLSS5OneMinusControlPanelStyle::FlatButtonStyle)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .IsEnabled_Lambda([this, CVarName]()
                {
                    return IsChoiceAvailable(CVarName, 1);
                })
                .ToolTipText(Description)
                .ContentPadding(FMargin(6.0f, 1.0f))
                .OnClicked_Lambda([this, CVarName, bDefaultValue]()
                {
                    const bool bEnabled = GetIntCVar(CVarName, bDefaultValue ? 1 : 0) != 0;
                    SetIntCVar(CVarName, bEnabled ? 0 : 1);
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(Label)
                    .Font_Lambda([this, CVarName, bDefaultValue]()
                    {
                        return FAppStyle::GetFontStyle(
                            GetIntCVar(CVarName, bDefaultValue ? 1 : 0) != 0
                                ? "SmallFontBold"
                                : "SmallFont");
                    })
                    .ColorAndOpacity_Lambda([this, CVarName, bDefaultValue]()
                    {
                        if (!IsChoiceAvailable(CVarName, 1))
                        {
                            return FSlateColor(DLSS5OneMinusControlPanelStyle::UnavailableButtonText);
                        }
                        return GetIntCVar(CVarName, bDefaultValue ? 1 : 0) != 0
                            ? FSlateColor(DLSS5OneMinusControlPanelStyle::ActiveButtonText)
                            : FSlateColor(DLSS5OneMinusControlPanelStyle::IdleButtonText);
                    })
                ]
            ]
        ];
}

void SDLSS5OneMinusControlPanel::LoadInitialState() const
{
    const UDLSS5OneMinusProjectSettings* ProjectSettings = GetDefault<UDLSS5OneMinusProjectSettings>();
    UDLSS5OneMinusEditorUserSettings* UserSettings = GetMutableDefault<UDLSS5OneMinusEditorUserSettings>();

    SelectedProfile = UserSettings->LastProfile.LoadSynchronous();
    BaselineSettings = SelectedProfile.IsValid()
        ? SelectedProfile->Settings.Normalized()
        : ProjectSettings->ResolveDefaultSettings();
    WorkingSettings = BaselineSettings;
    bWorkingEnabled = UserSettings->bHasSavedEditorState
        ? UserSettings->bLastNRRequested
        : ProjectSettings->bEnableInEditorWorlds;
    // Seed the requested screen % from saved state, else from the live viewport, and apply settings before deriving
    // the viewport scale, so a fresh panel keeps the user's live viewport value.
    if (UserSettings->bHasSavedEditorState)
    {
        RequestedScreenPercentage = UserSettings->LastEditorScreenPercentage;
    }
    else if (const TOptional<int32> Live = DLSS5OneMinusEditorViewport::GetScreenPercentage(); Live.IsSet())
    {
        RequestedScreenPercentage = Live.GetValue();
    }
    else
    {
        RequestedScreenPercentage = ProjectSettings->EditorPreviewScreenPercentage;
    }

    if (ProjectSettings->bApplyLastEditorProfileWhenPanelOpens && UserSettings->bHasSavedEditorState)
    {
        ApplyWorkingSettings(TEXT("Editor Restored Profile"));
        ApplyViewportScale();
    }
    else if (UWorld* World = GetEditorWorld())
    {
        FDLSS5OneMinusEffectiveState Effective;
        if (DLSS5OneMinus::GetEffectiveSettings(World, Effective))
        {
            WorkingSettings = Effective.Settings;
            bWorkingEnabled = Effective.bEnabled;
            LastAppliedRevision = Effective.Revision;
            LastAppliedWorld = World;
        }
        else
        {
            const FDLSS5OneMinusApplyResult Result = DLSS5OneMinus::ApplyProjectDefaults(World, TEXT("Editor Project Default"));
            WorkingSettings = Result.Effective.Settings;
            bWorkingEnabled = Result.Effective.bEnabled;
            LastAppliedRevision = Result.Effective.Revision;
            LastAppliedWorld = World;
        }
        if (bWorkingEnabled)
        {
            ApplyViewportScale();
        }
    }
}

UWorld* SDLSS5OneMinusControlPanel::GetEditorWorld() const
{
    return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void SDLSS5OneMinusControlPanel::ApplyWorkingSettings(const FName Source) const
{
    UWorld* World = GetEditorWorld();
    if (World == nullptr)
    {
        return;
    }

    WorkingSettings = WorkingSettings.Normalized();
    // honour the project scene-capture default,
    // and remember the revision so Tick can tell our own applies from external ones.
    const FDLSS5OneMinusApplyResult Result = DLSS5OneMinus::ApplySettings(
        World,
        WorkingSettings,
        bWorkingEnabled,
        GetDefault<UDLSS5OneMinusProjectSettings>()->bAllowSceneCapturesByDefault,
        Source);
    if (Result.State != EDLSS5OneMinusApplyState::Rejected)
    {
        LastAppliedRevision = Result.Effective.Revision;
        LastAppliedWorld = World;
    }

    UDLSS5OneMinusEditorUserSettings* UserSettings = GetMutableDefault<UDLSS5OneMinusEditorUserSettings>();
    UserSettings->bHasSavedEditorState = true;
    UserSettings->LastProfile = SelectedProfile.Get();
    UserSettings->bLastNRRequested = bWorkingEnabled;
    if (RequestedScreenPercentage.IsSet())
    {
        UserSettings->LastEditorScreenPercentage = RequestedScreenPercentage.GetValue();
    }
    UserSettings->SaveConfig();

    FlushCVarsAndViewport();
}

void SDLSS5OneMinusControlPanel::HandleProfileChanged(const FAssetData& AssetData) const
{
    // confirm before discarding edits; re-derive scale for the profile's route.
    if (!ConfirmDiscardEdits())
    {
        return;
    }
    SelectedProfile = Cast<UDLSS5OneMinusProfile>(AssetData.GetAsset());
    BaselineSettings = SelectedProfile.IsValid()
        ? SelectedProfile->Settings.Normalized()
        : GetDefault<UDLSS5OneMinusProjectSettings>()->ResolveDefaultSettings();
    WorkingSettings = BaselineSettings;
    ApplyWorkingSettings(SelectedProfile.IsValid() ? TEXT("Editor Selected Profile") : TEXT("Editor Project Default"));
    ApplyViewportScale();
}

FReply SDLSS5OneMinusControlPanel::SaveProfile() const
{
    UDLSS5OneMinusProfile* Profile = SelectedProfile.Get();
    if (Profile == nullptr)
    {
        return FReply::Handled();
    }

    const FScopedTransaction Transaction(LOCTEXT("SaveProfileTransaction", "Save DLSS5-OneMinus Profile"));
    Profile->Modify();
    Profile->Settings = WorkingSettings.Normalized();
    Profile->MarkPackageDirty();
    // only report SAVED when the asset actually saved.
    const bool bSaved = GEditor != nullptr
        && GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()->SaveLoadedAsset(Profile, true);
    if (!bSaved)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::Format(
                LOCTEXT("SaveProfileFailed", "The profile '{0}' could not be saved (checkout or write failed).\nYour edits are kept and remain MODIFIED."),
                FText::FromString(Profile->GetName())));
        return FReply::Handled();
    }
    BaselineSettings = Profile->Settings;
    return FReply::Handled();
}

FReply SDLSS5OneMinusControlPanel::SaveProfileAs() const
{
    FString InitialAssetName = TEXT("DLSS5OneMinus_Profile");
    FString InitialPackagePath = TEXT("/Game/DLSS5OneMinus/Profiles");
    if (const UDLSS5OneMinusProfile* Profile = SelectedProfile.Get())
    {
        InitialAssetName = Profile->GetName();
        InitialPackagePath = FPackageName::GetLongPackagePath(Profile->GetOutermost()->GetName());
    }

    UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
    Factory->DataAssetClass = UDLSS5OneMinusProfile::StaticClass();
    Factory->SupportedClass = UDLSS5OneMinusProfile::StaticClass();
    UObject* Created = FAssetToolsModule::GetModule().Get().CreateAssetWithDialog(
        InitialAssetName,
        InitialPackagePath,
        UDLSS5OneMinusProfile::StaticClass(),
        Factory,
        TEXT("DLSS5OneMinus"),
        false);
    if (UDLSS5OneMinusProfile* Profile = Cast<UDLSS5OneMinusProfile>(Created))
    {
        Profile->Modify();
        Profile->Settings = WorkingSettings.Normalized();
        Profile->MarkPackageDirty();
        if (GEditor != nullptr)
        {
            GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()->SaveLoadedAsset(Profile, true);
        }
        SelectedProfile = Profile;
        BaselineSettings = Profile->Settings;
        ApplyWorkingSettings(TEXT("Editor Saved Profile As"));
        ApplyViewportScale();
    }
    return FReply::Handled();
}

FReply SDLSS5OneMinusControlPanel::ClearProfileSelection() const
{
    if (!ConfirmDiscardEdits())
    {
        return FReply::Handled();
    }
    SelectedProfile.Reset();
    BaselineSettings = GetDefault<UDLSS5OneMinusProjectSettings>()->ResolveDefaultSettings();
    WorkingSettings = BaselineSettings;
    ApplyWorkingSettings(TEXT("Editor Project Default"));
    ApplyViewportScale();
    return FReply::Handled();
}

FString SDLSS5OneMinusControlPanel::GetProfileObjectPath() const
{
    return SelectedProfile.IsValid() ? SelectedProfile->GetPathName() : FString();
}

FText SDLSS5OneMinusControlPanel::GetProfileStatusText() const
{
    if (!DLSS5OneMinus::GetConsoleOverrideSummary().IsEmpty())
    {
        // do not hide unsaved edits behind the override badge.
        return IsProfileDirty()
            ? LOCTEXT("ProfileCVarOverrideModified", "CVAR OVERRIDE / MODIFIED")
            : LOCTEXT("ProfileCVarOverride", "CVAR OVERRIDE");
    }
    if (IsProfileDirty())
    {
        return LOCTEXT("ProfileModified", "MODIFIED");
    }
    return SelectedProfile.IsValid()
        ? LOCTEXT("ProfileSaved", "SAVED")
        : LOCTEXT("ProfileProjectDefault", "PROJECT DEFAULT");
}

FSlateColor SDLSS5OneMinusControlPanel::GetProfileStatusColor() const
{
    if (!DLSS5OneMinus::GetConsoleOverrideSummary().IsEmpty() || IsProfileDirty())
    {
        return DLSS5OneMinusControlPanelStyle::Amber;
    }
    return SelectedProfile.IsValid()
        ? FSlateColor(DLSS5OneMinusControlPanelStyle::Green)
        : FSlateColor(DLSS5OneMinusControlPanelStyle::Muted);
}

bool SDLSS5OneMinusControlPanel::IsProfileDirty() const
{
    return !WorkingSettings.NearlyEquals(BaselineSettings);
}

bool SDLSS5OneMinusControlPanel::CanSaveProfile() const
{
    return SelectedProfile.IsValid() && IsProfileDirty();
}

float SDLSS5OneMinusControlPanel::GetBaselineFloat(const TCHAR* Name, const float Fallback) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Strength")) == 0) return BaselineSettings.CompositionStrength;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.WhitePoint")) == 0) return BaselineSettings.HDRModelWhite;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Intensity")) == 0) return BaselineSettings.Intensity;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalToneStrength")) == 0) return BaselineSettings.LocalToneStrength;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalStructureStrength")) == 0) return BaselineSettings.LocalStructureStrength;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.SkinStructureStrength")) == 0) return BaselineSettings.SkinStructureStrength;
    if (FCString::Strcmp(Name, TEXT("r.Editor.Viewport.ScreenPercentage")) == 0)
    {
        return static_cast<float>(GetDefault<UDLSS5OneMinusProjectSettings>()->EditorPreviewScreenPercentage);
    }
    return Fallback;
}

int32 SDLSS5OneMinusControlPanel::GetBaselineInt(const TCHAR* Name, const int32 Fallback) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Path")) == 0) return static_cast<int32>(BaselineSettings.Route);
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.HDRComposition")) == 0) return static_cast<int32>(BaselineSettings.HDRComposition);
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Style")) == 0) return BaselineSettings.Style;
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.AutoMask")) == 0) return BaselineSettings.bAutoMask ? 1 : 0;
    return Fallback;
}

bool SDLSS5OneMinusControlPanel::TryGetWorkingFloat(const TCHAR* Name, float& OutValue) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Strength")) == 0) OutValue = WorkingSettings.CompositionStrength;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.WhitePoint")) == 0) OutValue = WorkingSettings.HDRModelWhite;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Intensity")) == 0) OutValue = WorkingSettings.Intensity;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalToneStrength")) == 0) OutValue = WorkingSettings.LocalToneStrength;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalStructureStrength")) == 0) OutValue = WorkingSettings.LocalStructureStrength;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.SkinStructureStrength")) == 0)
    {
        // in AUTO (-1) the greyed slider shows the explicit value it will return to.
        if (WorkingSettings.SkinStructureStrength >= 0.0f)
        {
            LastExplicitSkinStructure = WorkingSettings.SkinStructureStrength;
        }
        OutValue = WorkingSettings.SkinStructureStrength >= 0.0f ? WorkingSettings.SkinStructureStrength : LastExplicitSkinStructure;
    }
    else return false;
    return true;
}

bool SDLSS5OneMinusControlPanel::TryGetWorkingInt(const TCHAR* Name, int32& OutValue) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Path")) == 0) OutValue = static_cast<int32>(WorkingSettings.Route);
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.HDRComposition")) == 0) OutValue = static_cast<int32>(WorkingSettings.HDRComposition);
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Style")) == 0) OutValue = WorkingSettings.Style;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.AutoMask")) == 0) OutValue = WorkingSettings.bAutoMask ? 1 : 0;
    else if (FCString::Strcmp(Name, TEXT("DLSS5OneMinus.SkinStructureAuto")) == 0) OutValue = WorkingSettings.SkinStructureStrength < 0.0f ? 1 : 0;
    else return false;
    return true;
}

bool SDLSS5OneMinusControlPanel::TrySetWorkingFloat(const TCHAR* Name, const float Value) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Strength")) == 0) WorkingSettings.CompositionStrength = Value;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.WhitePoint")) == 0) WorkingSettings.HDRModelWhite = Value;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Intensity")) == 0) WorkingSettings.Intensity = Value;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalToneStrength")) == 0) WorkingSettings.LocalToneStrength = Value;
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.LocalStructureStrength")) == 0) WorkingSettings.LocalStructureStrength = Value;
    // the slider is explicit 0..2; a negative value (R reset to an AUTO baseline) selects AUTO.
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.SkinStructureStrength")) == 0)
    {
        WorkingSettings.SkinStructureStrength = Value < 0.0f ? -1.0f : FMath::Clamp(Value, 0.0f, 2.0f);
        if (Value >= 0.0f)
        {
            LastExplicitSkinStructure = WorkingSettings.SkinStructureStrength;
        }
    }
    else return false;
    ApplyWorkingSettings();
    return true;
}

bool SDLSS5OneMinusControlPanel::TrySetWorkingInt(const TCHAR* Name, const int32 Value) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Path")) == 0) WorkingSettings.Route = static_cast<EDLSS5OneMinusRoute>(FMath::Clamp(Value, 0, 2));
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.HDRComposition")) == 0) WorkingSettings.HDRComposition = static_cast<EDLSS5OneMinusHDRComposition>(FMath::Clamp(Value, 0, 1));
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Style")) == 0) WorkingSettings.Style = FMath::Clamp(Value, 0, 2);
    else if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.AutoMask")) == 0) WorkingSettings.bAutoMask = Value != 0;
    else if (FCString::Strcmp(Name, TEXT("DLSS5OneMinus.SkinStructureAuto")) == 0)
    {
        WorkingSettings.SkinStructureStrength = Value != 0 ? -1.0f : LastExplicitSkinStructure;
    }
    else return false;
    ApplyWorkingSettings();
    return true;
}

float SDLSS5OneMinusControlPanel::GetFloatCVar(const TCHAR* Name, const float DefaultValue) const
{
    if (FCString::Strcmp(Name, TEXT("r.Editor.Viewport.ScreenPercentage")) == 0)
    {
        // the control shows the user's requested value; it shows the
        // rendered value only while a route locks the scale. The header shows REQ / EFFECTIVE when they differ.
        if (IsScreenPercentageLocked())
        {
            const TOptional<int32> Value = DLSS5OneMinusEditorViewport::GetScreenPercentage();
            return Value.IsSet() ? static_cast<float>(Value.GetValue()) : 100.0f;
        }
        return static_cast<float>(GetRequestedScreenPercentage());
    }
    float WorkingValue = 0.0f;
    if (TryGetWorkingFloat(Name, WorkingValue))
    {
        return WorkingValue;
    }
    if (const IConsoleVariable* Variable = ResolveCVar(Name))
    {
        return Variable->GetFloat();
    }
    return DefaultValue;
}

int32 SDLSS5OneMinusControlPanel::GetIntCVar(const TCHAR* Name, const int32 DefaultValue) const
{
    int32 WorkingValue = 0;
    if (TryGetWorkingInt(Name, WorkingValue))
    {
        return WorkingValue;
    }
    if (const IConsoleVariable* Variable = ResolveCVar(Name))
    {
        return Variable->GetInt();
    }
    return DefaultValue;
}

void SDLSS5OneMinusControlPanel::SetFloatCVar(const TCHAR* Name, const float Value) const
{
    if (FCString::Strcmp(Name, TEXT("r.Editor.Viewport.ScreenPercentage")) == 0)
    {
        const int32 RequestedPercentage = FMath::Clamp(FMath::RoundToInt(Value), 10, 200);
        RequestedScreenPercentage = RequestedPercentage;
        ApplyViewportScale();
        UDLSS5OneMinusEditorUserSettings* UserSettings = GetMutableDefault<UDLSS5OneMinusEditorUserSettings>();
        UserSettings->bHasSavedEditorState = true;
        UserSettings->LastEditorScreenPercentage = RequestedPercentage;
        UserSettings->SaveConfig();
        FlushCVarsAndViewport();
        return;
    }
    if (TrySetWorkingFloat(Name, Value))
    {
        return;
    }
    if (IConsoleVariable* Variable = ResolveCVar(Name))
    {
        Variable->Set(Value, ECVF_SetByConsole);
        FlushCVarsAndViewport();
    }
}

void SDLSS5OneMinusControlPanel::SetIntCVar(const TCHAR* Name, const int32 Value) const
{
    if (FCString::Strcmp(Name, TEXT("r.NGX.DLSSNR.Path")) == 0)
    {
        // apply first, then derive scale from the requested value.
        const int32 Path = FMath::Clamp(Value, GDLSS5OneMinusPreDLSSPath, GDLSS5OneMinusPostDLAAPath);
        WorkingSettings.Route = static_cast<EDLSS5OneMinusRoute>(Path);
        ApplyWorkingSettings(TEXT("Editor Route Changed"));
        ApplyViewportScale();
        return;
    }

    if (TrySetWorkingInt(Name, Value))
    {
        return;
    }
    if (IConsoleVariable* Variable = ResolveCVar(Name))
    {
        Variable->Set(Value, ECVF_SetByConsole);
        FlushCVarsAndViewport();
    }
}

void SDLSS5OneMinusControlPanel::SetNeuralRenderingRequest(const bool bEnable) const
{
    bWorkingEnabled = bEnable;
    if (!bEnable)
    {
        if (IConsoleVariable* DebugView = ResolveCVar(TEXT("r.NGX.DLSSNR.DebugView")))
        {
            DebugView->Set(0, ECVF_SetByConsole);
        }
    }
    // Apply the enable state first, then derive the viewport scale from the user's requested value,
    // so enabling takes viewport ownership immediately and disabling keeps the requested scale.
    if (!bEnable)
    {
        ApplyWorkingSettings(TEXT("Editor NR Disabled"));
        DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(
            TEXT("NR disabled; viewport overrides released."));
        ApplyViewportScale();
        FlushCVarsAndViewport();
        return;
    }

    ApplyWorkingSettings(TEXT("Editor NR Enabled"));
    ApplyViewportScale();
}

bool SDLSS5OneMinusControlPanel::HasDependencies() const
{
    const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
    return Status.bVendorPluginAvailable
        && Status.bUnofficialRuntimeAvailable
        && Status.State != EDLSS5OneMinusRuntimeState::MissingVendor
        && Status.State != EDLSS5OneMinusRuntimeState::MissingUnofficialRuntime
        && Status.State != EDLSS5OneMinusRuntimeState::FaultedQuarantined;
}

bool SDLSS5OneMinusControlPanel::IsNRRequested() const
{
    return bWorkingEnabled;
}

bool SDLSS5OneMinusControlPanel::IsNRActive() const
{
    const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
    const int32 Path = GetIntCVar(TEXT("r.NGX.DLSSNR.Path"), GDLSS5OneMinusPostTonePath);
    const FString ExpectedRoute =
        Path == GDLSS5OneMinusPreDLSSPath
            ? TEXT("Pre DLSS")
            : Path == GDLSS5OneMinusPostDLAAPath ? TEXT("Post DLAA") : TEXT("Post Tone");
    const uint64 TargetViewId = DLSS5OneMinus::GetTargetViewId();
    return IsNRRequested()
        && Status.State == EDLSS5OneMinusRuntimeState::Ready
        && Status.bOutputCompleted
        && (TargetViewId == 0 || Status.ViewId == TargetViewId)
        && Status.EffectiveRoute == ExpectedRoute;
}

// One availability rule per control, derived from the context the control actually depends on:
// controls below the NR toggle need NR on, HDR settings need a pre-tonemap route, slice offset needs the Slice view.
bool SDLSS5OneMinusControlPanel::IsChoiceAvailable(const TCHAR* CVarName, const int32 Choice) const
{
    if (!IsNRControlAvailable())
    {
        return false;
    }
    if (FCString::Strcmp(CVarName, TEXT("r.NGX.DLSSNR.HDRComposition")) == 0)
    {
        return GetEffectiveRoute() != GDLSS5OneMinusPostTonePath;
    }
    if (FCString::Strcmp(CVarName, TEXT("r.NGX.DLSSNR.Path")) == 0)
    {
        return Choice >= GDLSS5OneMinusPreDLSSPath && Choice <= GDLSS5OneMinusPostDLAAPath;
    }
    if (FCString::Strcmp(CVarName, TEXT("DLSS5OneMinus.SkinStructureAuto")) == 0)
    {
        return WorkingSettings.bAutoMask;
    }
    return true;
}

bool SDLSS5OneMinusControlPanel::IsScalarAvailable(const TCHAR* CVarName) const
{
    if (FCString::Strcmp(CVarName, TEXT("r.Editor.Viewport.ScreenPercentage")) == 0)
    {
        return !IsPIEActive() && !IsScreenPercentageLocked();
    }
    if (!IsNRControlAvailable())
    {
        return false;
    }
    if (FCString::Strcmp(CVarName, TEXT("r.NGX.DLSSNR.SliceOffsetPercent")) == 0)
    {
        return GetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0) == 2;
    }
    if (FCString::Strcmp(CVarName, TEXT("r.NGX.DLSSNR.WhitePoint")) == 0)
    {
        return GetEffectiveRoute() != GDLSS5OneMinusPostTonePath;
    }
    if (FCString::Strcmp(CVarName, TEXT("r.NGX.DLSSNR.SkinStructureStrength")) == 0)
    {
        return WorkingSettings.bAutoMask && WorkingSettings.SkinStructureStrength >= 0.0f;
    }
    return true;
}

bool SDLSS5OneMinusControlPanel::IsPIEActive() const
{
    return GEditor != nullptr && GEditor->PlayWorld != nullptr;
}

bool SDLSS5OneMinusControlPanel::IsNRControlAvailable() const
{
    return HasDependencies() && IsNRRequested() && !IsPIEActive();
}

bool SDLSS5OneMinusControlPanel::CanToggleNeuralRendering() const
{
    // Turning NR off is always allowed (fail-closed); turning it on needs dependencies and no PIE session.
    return IsNRRequested() || (HasDependencies() && !IsPIEActive());
}

bool SDLSS5OneMinusControlPanel::IsScreenPercentageLocked() const
{
    return IsNRRequested() && GetEffectiveRoute() == GDLSS5OneMinusPostDLAAPath;
}

bool SDLSS5OneMinusControlPanel::CanClearProfile() const
{
    return !IsPIEActive() && (SelectedProfile.IsValid() || IsProfileDirty());
}

bool SDLSS5OneMinusControlPanel::ConfirmDiscardEdits() const
{
    if (!IsProfileDirty())
    {
        return true;
    }
    return FMessageDialog::Open(
        EAppMsgType::YesNo,
        LOCTEXT("DiscardEditsPrompt", "The working settings have unsaved edits.\nDiscard them and load the new settings?"))
        == EAppReturnType::Yes;
}

int32 SDLSS5OneMinusControlPanel::GetRequestedScreenPercentage() const
{
    if (RequestedScreenPercentage.IsSet())
    {
        return RequestedScreenPercentage.GetValue();
    }
    const TOptional<int32> Live = DLSS5OneMinusEditorViewport::GetScreenPercentage();
    return Live.IsSet() ? Live.GetValue() : 100;
}

void SDLSS5OneMinusControlPanel::ApplyViewportScale() const
{
    // The single place the viewport scale is derived: user intent, constrained by the route only while NR is requested.
    PendingScreenPercentage.Reset(); // any applied scale supersedes an in-flight drag value
    const int32 Effective = DLSS5OneMinusEditorViewport::NormalizeScreenPercentage(
        GetRequestedScreenPercentage(),
        GetEffectiveRoute(),
        IsNRRequested());
    // The level viewport may not exist yet (panel restored during editor startup); Tick retries until it applies.
    bViewportScalePending = !DLSS5OneMinusEditorViewport::SetScreenPercentage(Effective);
}

bool SDLSS5OneMinusControlPanel::IsRouteUnavailable() const
{
    return IsNRRequested() && DLSS5OneMinus::GetStatus().EffectiveRoute.StartsWith(TEXT("Unavailable"));
}

void SDLSS5OneMinusControlPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
    if (IsPIEActive())
    {
        return;
    }
    UWorld* World = GetEditorWorld();
    if (World == nullptr || World->Scene == nullptr)
    {
        return;
    }
    if (bViewportScalePending)
    {
        ApplyViewportScale();
    }

    FDLSS5OneMinusEffectiveState Effective;
    if (!DLSS5OneMinus::GetEffectiveSettings(World, Effective))
    {
        // A newly opened map has no state yet; the panel is the editor preview authority.
        ApplyWorkingSettings(TEXT("Editor Map Changed"));
        if (bWorkingEnabled)
        {
            ApplyViewportScale();
        }
        return;
    }

    if (Effective.Revision != LastAppliedRevision || LastAppliedWorld.Get() != World)
    {
        // Another source (Blueprint, settings actor, console command, PIE restore) changed the editor world.
        const bool bWasEnabled = bWorkingEnabled;
        WorkingSettings = Effective.Settings;
        bWorkingEnabled = Effective.bEnabled;
        LastAppliedRevision = Effective.Revision;
        LastAppliedWorld = World;
        if (bWasEnabled && !bWorkingEnabled)
        {
            DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(TEXT("NR disabled outside the panel; viewport overrides released."));
            ApplyViewportScale();
        }
        else if (!bWasEnabled && bWorkingEnabled)
        {
            ApplyViewportScale();
        }
    }
}

int32 SDLSS5OneMinusControlPanel::GetEffectiveRoute() const
{
    return FMath::Clamp(static_cast<int32>(WorkingSettings.Route),
        GDLSS5OneMinusPreDLSSPath,
        GDLSS5OneMinusPostDLAAPath);
}

IConsoleVariable* SDLSS5OneMinusControlPanel::ResolveCVar(const TCHAR* Name) const
{
    if (IConsoleVariable* const* CachedVariable = ConsoleVariables.Find(Name))
    {
        return *CachedVariable;
    }
    if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
    {
        ConsoleVariables.Add(Name, Variable);
        return Variable;
    }
    return nullptr;
}

FReply SDLSS5OneMinusControlPanel::ResetVerifiedDefaults() const
{
    WorkingSettings = FDLSS5OneMinusSettings::VerifiedDefaults();
    SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
    SetFloatCVar(TEXT("r.NGX.DLSSNR.SliceOffsetPercent"), 0.0f);
    ApplyWorkingSettings(TEXT("Editor Verified Defaults"));
    ApplyViewportScale(); // the default route may have a different scale range
    return FReply::Handled();
}

FText SDLSS5OneMinusControlPanel::GetRequestStatusText() const
{
    if (IsNRActive())
    {
        return LOCTEXT("RequestActive", "OUTPUT / ACTIVE");
    }
    if (IsRouteUnavailable())
    {
        return LOCTEXT("RequestRouteUnavailable", "OUTPUT / ROUTE UNAVAILABLE");
    }
    if (IsNRRequested())
    {
        return LOCTEXT("RequestPending", "OUTPUT / PENDING");
    }
    return LOCTEXT("RequestOff", "OUTPUT / OFF");
}

FSlateColor SDLSS5OneMinusControlPanel::GetRequestStatusColor() const
{
    if (IsNRActive())
    {
        return DLSS5OneMinusControlPanelStyle::Green;
    }
    if (IsNRRequested())
    {
        return DLSS5OneMinusControlPanelStyle::Amber;
    }
    return DLSS5OneMinusControlPanelStyle::Muted;
}

FText SDLSS5OneMinusControlPanel::GetRouteStatusText() const
{
    // effective = what the viewport renders; requested = user intent.
    const TOptional<int32> LiveScale = DLSS5OneMinusEditorViewport::GetScreenPercentage();
    const int32 EffectivePercentage = LiveScale.IsSet() ? LiveScale.GetValue() : 100;
    const int32 RequestedPercentage = GetRequestedScreenPercentage();
    const FText ScaleText = RequestedPercentage == EffectivePercentage
        ? FText::Format(LOCTEXT("EffectiveScaleOnly", "{0}%"), FText::AsNumber(EffectivePercentage))
        : FText::Format(
            LOCTEXT("RequestedEffectiveScale", "REQ {0}% / EFFECTIVE {1}%"),
            FText::AsNumber(RequestedPercentage),
            FText::AsNumber(EffectivePercentage));
    switch (GetEffectiveRoute())
    {
    case GDLSS5OneMinusPreDLSSPath:
        return FText::Format(LOCTEXT("RoutePreDLSSStatus", "ROUTE / PRE DLSS / {0}"), ScaleText);
    case GDLSS5OneMinusPostDLAAPath:
        return FText::Format(LOCTEXT("RoutePostDLAAStatus", "ROUTE / POST DLAA / {0}"), ScaleText);
    default:
        return FText::Format(LOCTEXT("RoutePostToneStatus", "ROUTE / POST TONE / {0}"), ScaleText);
    }
}

FText SDLSS5OneMinusControlPanel::GetControlBusStatusText() const
{
    return HasDependencies()
        ? LOCTEXT("EditorViewportReady", "DEPENDENCIES / READY")
        : LOCTEXT("EditorViewportWaiting", "DEPENDENCIES / WAITING");
}

FSlateColor SDLSS5OneMinusControlPanel::GetControlBusStatusColor() const
{
    return HasDependencies()
        ? DLSS5OneMinusControlPanelStyle::Green
        : DLSS5OneMinusControlPanelStyle::Amber;
}

FText SDLSS5OneMinusControlPanel::GetRuntimeDetailText() const
{
    FString Detail = DLSS5OneMinus::GetStatus().Detail;
    FDLSS5OneMinusEffectiveState Effective;
    if (DLSS5OneMinus::GetEffectiveSettings(GetEditorWorld(), Effective))
    {
        Detail += FString::Printf(TEXT("\nSettings source: %s (revision %lld)."),
            *Effective.Source.ToString(),
            Effective.Revision);
    }
    const FString Overrides = DLSS5OneMinus::GetConsoleOverrideSummary();
    if (!Overrides.IsEmpty())
    {
        Detail += FString::Printf(TEXT("\nHigher-priority CVar override: %s"), *Overrides);
    }
    return FText::FromString(Detail);
}

#undef LOCTEXT_NAMESPACE
