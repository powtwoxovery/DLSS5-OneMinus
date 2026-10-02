#include "DLSS5OneMinusEditorModule.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "DLSS5OneMinusEditorUserSettings.h"
#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusStatus.h"
#include "DLSS5OneMinusEditorViewport.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "LevelEditor.h"
#include "Misc/Paths.h"
#include "SDLSS5OneMinusControlPanel.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "UnrealClient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "DLSS5OneMinusEditor"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusEditor, Log, All);

namespace
{
    const FName ControlPanelTabName(TEXT("DLSS5OneMinusControlPanel"));
    IConsoleObject* OpenControlPanelCommand = nullptr;
    IConsoleObject* ValidatePreDLSSCommand = nullptr;
    IConsoleObject* ValidatePostDLAACommand = nullptr;
    IConsoleObject* ValidatePostToneCommand = nullptr;
    IConsoleObject* ValidateRouteSwitchCommand = nullptr;
    IConsoleObject* ValidateScreenPercentageCommand = nullptr;
    IConsoleObject* DebugDifferenceCommand = nullptr;
    IConsoleObject* DebugSliceCommand = nullptr;
    FTSTicker::FDelegateHandle ValidationTickerHandle;
    FDelegateHandle PreBeginPIEHandle;
    FDelegateHandle EndPIEHandle;

    void ReleaseViewportForPIE(bool bIsSimulating)
    {
        DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(
            TEXT("Editor viewport control released for PIE/SIE context handoff."));
    }

    // After PIE/SIE ends, re-publish the editor world's NR state and re-acquire the viewport at the user's requested scale.
    // PIE worlds mirror their own state into the shared CVars, and PIE start releases the viewport.
    void RestoreEditorNRAfterPIE()
    {
        UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
        FDLSS5OneMinusEffectiveState Current;
        if (World == nullptr || !DLSS5OneMinus::GetEffectiveSettings(World, Current))
        {
            return;
        }
        DLSS5OneMinus::ApplySettings(
            World, Current.Settings, Current.bEnabled, Current.bAllowSceneCaptures, TEXT("Editor Restored After PIE"));
        if (!Current.bEnabled)
        {
            return;
        }
        const UDLSS5OneMinusEditorUserSettings* UserSettings = GetDefault<UDLSS5OneMinusEditorUserSettings>();
        const int32 Requested = UserSettings->bHasSavedEditorState
            ? UserSettings->LastEditorScreenPercentage
            : GetDefault<UDLSS5OneMinusProjectSettings>()->EditorPreviewScreenPercentage;
        DLSS5OneMinusEditorViewport::SetScreenPercentage(DLSS5OneMinusEditorViewport::NormalizeScreenPercentage(
            Requested, static_cast<int32>(Current.Settings.Route), true));
        UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("Restored editor NR viewport after PIE at requested %d%%."), Requested);
    }

    void ReleaseViewportAfterPIE(bool bWasSimulating)
    {
        DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(
            TEXT("PIE/SIE ended; restoring the editor viewport NR state."));
        // PIE world teardown completes after EndPIE; restore on the next tick.
        FTSTicker::GetCoreTicker().AddTicker(
            TEXT("DLSS5OneMinus.RestoreAfterPIE"),
            0.0f,
            [](float)
            {
                RestoreEditorNRAfterPIE();
                return false;
            });
    }

    struct FRouteValidationState
    {
        int32 Path = 1;
        int32 ScreenPercentage = 100;
        FString Label = TEXT("Post Tone");
        FString FilenameStem = TEXT("PostTone");
        double ElapsedSeconds = 0.0;
        double NextStatusLogSeconds = 0.0;
        double DisabledScreenshotRequestTime = 0.0;
        double EnableTime = 0.0;
        double EnabledScreenshotRequestTime = 0.0;
        bool bDisabledScreenshotRequested = false;
        bool bEnabled = false;
        bool bEnabledScreenshotRequested = false;
    };

    struct FRouteSwitchValidationState
    {
        double ElapsedSeconds = 0.0;
        double SwitchTime = 0.0;
        uint64 PreSwitchFrame = 0;
        uint64 PreSwitchGeneration = 0;
        bool bSwitchedToPostTone = false;
    };

    struct FScreenPercentageValidationState
    {
        double ElapsedSeconds = 0.0;
        double StageStartSeconds = 0.0;
        int32 StageIndex = 0;
        uint64 BaselineFrame = 0;
        uint64 BaselineGeneration = 0;
        bool bHoldingLowValue = false;
        bool bHoldingFixedDLSSBoundary = false;
        bool bHoldingFinalValue = false;
    };

    constexpr int32 ScreenPercentageValidationValues[] = { 10, 33, 31, 33, 100, 150, 200 };

    const TCHAR* ExpectedRouteName(const int32 Path)
    {
        switch (Path)
        {
        case 0: return TEXT("Pre DLSS");
        case 2: return TEXT("Post DLAA");
        default: return TEXT("Post Tone");
        }
    }

    TSharedRef<SDockTab> SpawnControlPanel(const FSpawnTabArgs& SpawnTabArgs)
    {
        UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("Spawning Neural Rendering Lab tab."));
        return SNew(SDockTab)
            .TabRole(ETabRole::NomadTab)
            .Label(LOCTEXT("TabLabel", "Neural Rendering Lab"))
            [
                SNew(SDLSS5OneMinusControlPanel)
            ];
    }

    void InvokeControlPanel()
    {
        AsyncTask(ENamedThreads::GameThread, []
        {
            const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(ControlPanelTabName);
            if (Tab.IsValid())
            {
                Tab->ActivateInParent(ETabActivationCause::SetDirectly);
                if (const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef()))
                {
                    // Match the Details panel's scale
                    // only while detached. A docked tab remains user-sized.
                    if (Window->GetParentWindow().IsValid()
                        || !Window->GetTitle().ToString().Contains(TEXT("Unreal Editor")))
                    {
                        Window->Resize(FVector2D(850.0f, 1400.0f));
                    }
                }
                UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("Neural Rendering Lab tab invoked and activated."));
            }
            else
            {
                UE_LOG(LogDLSS5OneMinusEditor, Error, TEXT("Neural Rendering Lab tab invocation returned no tab."));
            }
        });
    }

    void SetIntCVar(const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            Variable->Set(Value, ECVF_SetByConsole);
        }
    }

    // Route the debug/validation commands' NR enable and route through the editor world's settings state,
    // so the commands and the panel edit the same state and the panel can always turn NR off.
    void ApplyEditorNR(const int32 Path, const bool bEnable)
    {
        UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
        const UDLSS5OneMinusProjectSettings* ProjectSettings = GetDefault<UDLSS5OneMinusProjectSettings>();
        FDLSS5OneMinusEffectiveState Current;
        const bool bHasCurrent = DLSS5OneMinus::GetEffectiveSettings(World, Current);
        FDLSS5OneMinusSettings Settings = bHasCurrent ? Current.Settings : ProjectSettings->ResolveDefaultSettings();
        Settings.Route = static_cast<EDLSS5OneMinusRoute>(FMath::Clamp(Path, 0, 2));
        DLSS5OneMinus::ApplySettings(
            World,
            Settings,
            bEnable,
            bHasCurrent ? Current.bAllowSceneCaptures : ProjectSettings->bAllowSceneCapturesByDefault,
            TEXT("Editor Validation Command"));
    }

    void ActivateDebugView(int32 DebugMode)
    {
        AsyncTask(ENamedThreads::GameThread, [DebugMode]
        {
            ApplyEditorNR(1, true);
            SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), DebugMode);
            SetIntCVar(TEXT("r.NGX.DLSS.Enable"), 1);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
            const bool bScreenPercentageSet = DLSS5OneMinusEditorViewport::SetScreenPercentage(100);
            DLSS5OneMinusEditorViewport::Invalidate();
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Activated bounded Post Tone debug view %d; viewport percentage applied=%s."),
                DebugMode,
                bScreenPercentageSet ? TEXT("true") : TEXT("false"));
        });
    }

    bool TickRouteValidation(float DeltaSeconds, TSharedRef<FRouteValidationState> State)
    {
        State->ElapsedSeconds += DeltaSeconds;

        DLSS5OneMinusEditorViewport::Invalidate();

        if (State->ElapsedSeconds >= State->NextStatusLogSeconds)
        {
            const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("%s validation t=%.1fs state=%s route=%s frame=%llu result=%d completed=%s detail=%s"),
                *State->Label,
                State->ElapsedSeconds,
                DLSS5OneMinus::LexToString(Status.State),
                *Status.EffectiveRoute,
                Status.FrameNumber,
                Status.LastEvaluateResult,
                Status.bOutputCompleted ? TEXT("true") : TEXT("false"),
                *Status.Detail);
            State->NextStatusLogSeconds += 1.0;
        }

        const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DLSS5OneMinus/Validation"));
        // Let the stock temporal upscaler build a stable history before the
        // NR-off baseline. Otherwise the A/B delta includes DLSS convergence.
        if (!State->bDisabledScreenshotRequested && State->ElapsedSeconds >= 8.0)
        {
            State->bDisabledScreenshotRequested = true;
            State->DisabledScreenshotRequestTime = State->ElapsedSeconds;
            FScreenshotRequest::RequestScreenshot(
                FPaths::Combine(OutputDir, State->FilenameStem + TEXT("_NR_Off.png")), false, false);
            UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("Requested disabled baseline screenshot."));
        }

        if (!State->bEnabled
            && State->bDisabledScreenshotRequested
            && (!FScreenshotRequest::IsScreenshotRequested()
                || State->ElapsedSeconds - State->DisabledScreenshotRequestTime >= 2.0)
            && State->ElapsedSeconds - State->DisabledScreenshotRequestTime >= 0.5)
        {
            State->bEnabled = true;
            State->EnableTime = State->ElapsedSeconds;
            ApplyEditorNR(State->Path, true);
            SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
            DLSS5OneMinusEditorViewport::SetScreenPercentage(State->ScreenPercentage);
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Enabled %s NR for bounded viewport validation."), *State->Label);
        }

        if (!State->bEnabledScreenshotRequested
            && State->bEnabled
            && State->ElapsedSeconds - State->EnableTime >= 8.0)
        {
            State->bEnabledScreenshotRequested = true;
            State->EnabledScreenshotRequestTime = State->ElapsedSeconds;
            // Clear any screenshot request still pending from the baseline image
            // before asking for the enabled image.
            if (FScreenshotRequest::IsScreenshotRequested())
            {
                FScreenshotRequest::Reset();
            }
            FScreenshotRequest::RequestScreenshot(
                FPaths::Combine(OutputDir, State->FilenameStem + TEXT("_NR_On.png")), false, false);
            UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("Requested enabled %s screenshot."), *State->Label);
        }

        const bool bEnabledScreenshotFinished = State->bEnabledScreenshotRequested
            && State->ElapsedSeconds - State->EnabledScreenshotRequestTime >= 0.5
            && (!FScreenshotRequest::IsScreenshotRequested()
                || State->ElapsedSeconds - State->EnabledScreenshotRequestTime >= 5.0);
        if (!bEnabledScreenshotFinished && State->ElapsedSeconds < 40.0)
        {
            return true;
        }

        const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        const FString DisabledScreenshotPath = FPaths::Combine(
            OutputDir, State->FilenameStem + TEXT("_NR_Off.png"));
        const FString EnabledScreenshotPath = FPaths::Combine(
            OutputDir, State->FilenameStem + TEXT("_NR_On.png"));
        const bool bDisabledScreenshotWritten = IFileManager::Get().FileSize(*DisabledScreenshotPath) > 0;
        const bool bEnabledScreenshotWritten = IFileManager::Get().FileSize(*EnabledScreenshotPath) > 0;
        const bool bPassed = Status.State == EDLSS5OneMinusRuntimeState::Ready
            && Status.bOutputSubmitted
            && Status.bOutputCompleted
            && Status.EffectiveRoute == ExpectedRouteName(State->Path)
            && bDisabledScreenshotWritten
            && bEnabledScreenshotWritten;
        const FString ValidationSummary = FString::Printf(
            TEXT("%s validation %s: state=%s route=%s frame=%llu result=%d submitted=%s completed=%s offImage=%s onImage=%s detail=%s"),
            *State->Label,
            bPassed ? TEXT("PASSED") : TEXT("FAILED"),
            DLSS5OneMinus::LexToString(Status.State),
            *Status.EffectiveRoute,
            Status.FrameNumber,
            Status.LastEvaluateResult,
            Status.bOutputSubmitted ? TEXT("true") : TEXT("false"),
            Status.bOutputCompleted ? TEXT("true") : TEXT("false"),
            bDisabledScreenshotWritten ? TEXT("written") : TEXT("missing"),
            bEnabledScreenshotWritten ? TEXT("written") : TEXT("missing"),
            *Status.Detail);
        if (bPassed)
        {
            UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("%s"), *ValidationSummary);
        }
        else
        {
            UE_LOG(LogDLSS5OneMinusEditor, Error, TEXT("%s"), *ValidationSummary);
        }
        ValidationTickerHandle.Reset();
        return false;
    }

    void StartRouteValidation(int32 Path, int32 ScreenPercentage, const TCHAR* Label, const TCHAR* FilenameStem)
    {
        AsyncTask(ENamedThreads::GameThread, [Path, ScreenPercentage, Label = FString(Label), FilenameStem = FString(FilenameStem)]
        {
            if (ValidationTickerHandle.IsValid())
            {
                UE_LOG(LogDLSS5OneMinusEditor, Warning, TEXT("A route validation is already running."));
                return;
            }

            const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DLSS5OneMinus/Validation"));
            IFileManager::Get().MakeDirectory(*OutputDir, true);
            IFileManager::Get().Delete(*FPaths::Combine(OutputDir, FilenameStem + TEXT("_NR_Off.png")), false, true);
            IFileManager::Get().Delete(*FPaths::Combine(OutputDir, FilenameStem + TEXT("_NR_On.png")), false, true);
            ApplyEditorNR(Path, false);
            SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
            SetIntCVar(TEXT("r.NGX.DLSS.Enable"), 1);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
            const bool bScreenPercentageSet = DLSS5OneMinusEditorViewport::SetScreenPercentage(ScreenPercentage);
            const TOptional<int32> ScreenPercentageReadback = DLSS5OneMinusEditorViewport::GetScreenPercentage();

            const TSharedRef<FRouteValidationState> State = MakeShared<FRouteValidationState>();
            State->Path = Path;
            State->ScreenPercentage = ScreenPercentage;
            State->Label = Label;
            State->FilenameStem = FilenameStem;
            ValidationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                TEXT("DLSS5OneMinus.RouteValidation"),
                0.0f,
                [State](float DeltaSeconds)
                {
                    return TickRouteValidation(DeltaSeconds, State);
                });
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Started bounded %s viewport validation; viewport screen percentage requested=%d applied=%s readback=%s; output=%s"),
                *Label,
                ScreenPercentage,
                bScreenPercentageSet ? TEXT("true") : TEXT("false"),
                ScreenPercentageReadback.IsSet() ? *FString::FromInt(ScreenPercentageReadback.GetValue()) : TEXT("unavailable"),
                *OutputDir);
        });
    }

    void StartRouteSwitchValidation()
    {
        AsyncTask(ENamedThreads::GameThread, []
        {
            if (ValidationTickerHandle.IsValid())
            {
                UE_LOG(LogDLSS5OneMinusEditor, Warning, TEXT("A route validation is already running."));
                return;
            }

            ApplyEditorNR(0, true);
            SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
            SetIntCVar(TEXT("r.NGX.DLSS.Enable"), 1);
            IConsoleManager::Get().CallAllConsoleVariableSinks();
            const bool bPreScaleApplied = DLSS5OneMinusEditorViewport::SetScreenPercentage(67);
            DLSS5OneMinusEditorViewport::Invalidate();

            const TSharedRef<FRouteSwitchValidationState> State = MakeShared<FRouteSwitchValidationState>();
            ValidationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                TEXT("DLSS5OneMinus.RouteSwitchValidation"),
                0.0f,
                [State](float DeltaSeconds)
                {
                    State->ElapsedSeconds += DeltaSeconds;
                    DLSS5OneMinusEditorViewport::Invalidate();

                    if (!State->bSwitchedToPostTone && State->ElapsedSeconds >= 8.0)
                    {
                        const FDLSS5OneMinusStatusSnapshot PreStatus = DLSS5OneMinus::GetStatus();
                        if (!PreStatus.bOutputCompleted
                            || PreStatus.EffectiveRoute != TEXT("Pre DLSS"))
                        {
                            if (State->ElapsedSeconds < 30.0)
                            {
                                return true;
                            }
                            UE_LOG(LogDLSS5OneMinusEditor, Error,
                                TEXT("Route switch validation timed out waiting for active Pre DLSS output: state=%s route=%s completed=%s detail=%s"),
                                DLSS5OneMinus::LexToString(PreStatus.State),
                                *PreStatus.EffectiveRoute,
                                PreStatus.bOutputCompleted ? TEXT("true") : TEXT("false"),
                                *PreStatus.Detail);
                            ValidationTickerHandle.Reset();
                            return false;
                        }
                        UE_LOG(LogDLSS5OneMinusEditor, Display,
                            TEXT("Route switch pre-check: state=%s route=%s frame=%llu result=%d completed=%s detail=%s"),
                            DLSS5OneMinus::LexToString(PreStatus.State),
                            *PreStatus.EffectiveRoute,
                            PreStatus.FrameNumber,
                            PreStatus.LastEvaluateResult,
                            PreStatus.bOutputCompleted ? TEXT("true") : TEXT("false"),
                            *PreStatus.Detail);
                        State->PreSwitchFrame = PreStatus.FrameNumber;
                        State->PreSwitchGeneration = PreStatus.ConfigGeneration;

                        ApplyEditorNR(1, true);
                        IConsoleManager::Get().CallAllConsoleVariableSinks();
                        const bool bPostScaleApplied = DLSS5OneMinusEditorViewport::SetScreenPercentage(100);
                        State->bSwitchedToPostTone = true;
                        State->SwitchTime = State->ElapsedSeconds;
                        UE_LOG(LogDLSS5OneMinusEditor, Display,
                            TEXT("Route switch applied: Pre DLSS 67%% -> Post Tone 100%%; viewport applied=%s."),
                            bPostScaleApplied ? TEXT("true") : TEXT("false"));
                    }

                    if (!State->bSwitchedToPostTone)
                    {
                        return true;
                    }

                    const FDLSS5OneMinusStatusSnapshot PostStatus = DLSS5OneMinus::GetStatus();
                    const TOptional<int32> ScreenPercentageReadback = DLSS5OneMinusEditorViewport::GetScreenPercentage();
                    const bool bPostOutputFresh = PostStatus.bOutputCompleted
                        && PostStatus.EffectiveRoute == TEXT("Post Tone")
                        && (PostStatus.FrameNumber > State->PreSwitchFrame
                            || PostStatus.ConfigGeneration > State->PreSwitchGeneration);
                    const bool bPassed = PostStatus.State == EDLSS5OneMinusRuntimeState::Ready
                        && PostStatus.bOutputSubmitted
                        && bPostOutputFresh
                        && ScreenPercentageReadback.IsSet()
                        && ScreenPercentageReadback.GetValue() == 100;
                    if (!bPassed && State->ElapsedSeconds - State->SwitchTime < 30.0)
                    {
                        return true;
                    }

                    const FString ValidationSummary = FString::Printf(
                        TEXT("Route switch validation %s: state=%s route=%s frame=%llu generation=%llu result=%d submitted=%s completed=%s screenPercentage=%s detail=%s"),
                        bPassed ? TEXT("PASSED") : TEXT("FAILED"),
                        DLSS5OneMinus::LexToString(PostStatus.State),
                        *PostStatus.EffectiveRoute,
                        PostStatus.FrameNumber,
                        PostStatus.ConfigGeneration,
                        PostStatus.LastEvaluateResult,
                        PostStatus.bOutputSubmitted ? TEXT("true") : TEXT("false"),
                        PostStatus.bOutputCompleted ? TEXT("true") : TEXT("false"),
                        ScreenPercentageReadback.IsSet() ? *FString::FromInt(ScreenPercentageReadback.GetValue()) : TEXT("unavailable"),
                        *PostStatus.Detail);
                    if (bPassed)
                    {
                        UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("%s"), *ValidationSummary);
                    }
                    else
                    {
                        UE_LOG(LogDLSS5OneMinusEditor, Error, TEXT("%s"), *ValidationSummary);
                    }
                    ValidationTickerHandle.Reset();
                    return false;
                });

            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Started route switch validation at Pre DLSS 67%%; viewport applied=%s."),
                bPreScaleApplied ? TEXT("true") : TEXT("false"));
        });
    }

    void ApplyScreenPercentageValidationStage(const TSharedRef<FScreenPercentageValidationState>& State)
    {
        const int32 RequestedPercentage = ScreenPercentageValidationValues[State->StageIndex];
        const FDLSS5OneMinusStatusSnapshot Before = DLSS5OneMinus::GetStatus();
        State->BaselineFrame = Before.FrameNumber;
        State->BaselineGeneration = Before.ConfigGeneration;
        State->StageStartSeconds = State->ElapsedSeconds;

        const bool bApplied = DLSS5OneMinusEditorViewport::SetScreenPercentage(RequestedPercentage);
        const TOptional<int32> Readback = DLSS5OneMinusEditorViewport::GetScreenPercentage();
        DLSS5OneMinusEditorViewport::Invalidate();
        UE_LOG(LogDLSS5OneMinusEditor, Display,
            TEXT("Screen percentage stage applied: requested=%d applied=%s readback=%s baselineFrame=%llu baselineGeneration=%llu."),
            RequestedPercentage,
            bApplied ? TEXT("true") : TEXT("false"),
            Readback.IsSet() ? *FString::FromInt(Readback.GetValue()) : TEXT("unavailable"),
            State->BaselineFrame,
            State->BaselineGeneration);
    }

    bool TickScreenPercentageValidation(float DeltaSeconds, TSharedRef<FScreenPercentageValidationState> State)
    {
        State->ElapsedSeconds += DeltaSeconds;
        DLSS5OneMinusEditorViewport::Invalidate();

        if (State->bHoldingLowValue)
        {
            if (State->ElapsedSeconds - State->StageStartSeconds < 10.0)
            {
                return true;
            }

            State->bHoldingLowValue = false;
            ++State->StageIndex;
            ApplyScreenPercentageValidationStage(State);
            return true;
        }

        if (State->bHoldingFixedDLSSBoundary)
        {
            // Hold the first 33% stage long enough to inspect the fixed
            // Ultra Performance boundary before the scale continues to 31%.
            if (State->ElapsedSeconds - State->StageStartSeconds < 5.0)
            {
                return true;
            }

            State->bHoldingFixedDLSSBoundary = false;
            ++State->StageIndex;
            ApplyScreenPercentageValidationStage(State);
            return true;
        }

        if (State->bHoldingFinalValue)
        {
            // Hold 200% for ten seconds so the panel readback and the viewport
            // proportions can be inspected.
            if (State->ElapsedSeconds - State->StageStartSeconds < 10.0)
            {
                return true;
            }

            const bool bRestored = DLSS5OneMinusEditorViewport::SetScreenPercentage(100);
            const TOptional<int32> RestoreReadback = DLSS5OneMinusEditorViewport::GetScreenPercentage();
            const bool bRestorePassed = bRestored
                && RestoreReadback.IsSet()
                && RestoreReadback.GetValue() == 100;
            if (bRestorePassed)
            {
                UE_LOG(LogDLSS5OneMinusEditor, Display,
                    TEXT("Screen percentage validation PASSED: values=10,33,31,33,100,150,200 restore=100."));
            }
            else
            {
                UE_LOG(LogDLSS5OneMinusEditor, Error,
                    TEXT("Screen percentage validation FAILED while restoring 100%%: applied=%s readback=%s."),
                    bRestored ? TEXT("true") : TEXT("false"),
                    RestoreReadback.IsSet() ? *FString::FromInt(RestoreReadback.GetValue()) : TEXT("unavailable"));
            }
            ValidationTickerHandle.Reset();
            return false;
        }

        const int32 RequestedPercentage = ScreenPercentageValidationValues[State->StageIndex];
        const FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        const TOptional<int32> Readback = DLSS5OneMinusEditorViewport::GetScreenPercentage();
        const bool bFreshOutput = Status.bOutputCompleted
            && (Status.FrameNumber > State->BaselineFrame
                || Status.ConfigGeneration > State->BaselineGeneration);
        const bool bPassed = Status.State == EDLSS5OneMinusRuntimeState::Ready
            && Status.EffectiveRoute == TEXT("Post Tone")
            && Status.bOutputSubmitted
            && bFreshOutput
            && Readback.IsSet()
            && Readback.GetValue() == RequestedPercentage;

        if (bPassed)
        {
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Screen percentage stage PASSED: requested=%d readback=%d frame=%llu generation=%llu result=%d detail=%s"),
                RequestedPercentage,
                Readback.GetValue(),
                Status.FrameNumber,
                Status.ConfigGeneration,
                Status.LastEvaluateResult,
                *Status.Detail);

            if (State->StageIndex == 0)
            {
                State->bHoldingLowValue = true;
                State->StageStartSeconds = State->ElapsedSeconds;
                UE_LOG(LogDLSS5OneMinusEditor, Display,
                    TEXT("Screen percentage validation holding 10%% for 10 seconds for inspection."));
                return true;
            }
            if (State->StageIndex == 1)
            {
                State->bHoldingFixedDLSSBoundary = true;
                State->StageStartSeconds = State->ElapsedSeconds;
                UE_LOG(LogDLSS5OneMinusEditor, Display,
                    TEXT("Screen percentage validation holding the fixed DLSS 33%% boundary for 5 seconds."));
                return true;
            }

            if (State->StageIndex == UE_ARRAY_COUNT(ScreenPercentageValidationValues) - 1)
            {
                State->bHoldingFinalValue = true;
                State->StageStartSeconds = State->ElapsedSeconds;
                UE_LOG(LogDLSS5OneMinusEditor, Display,
                    TEXT("Screen percentage validation holding 200%% for 10 seconds for inspection."));
                return true;
            }

            ++State->StageIndex;
            ApplyScreenPercentageValidationStage(State);
            return true;
        }

        if (State->ElapsedSeconds - State->StageStartSeconds < 20.0)
        {
            return true;
        }

        UE_LOG(LogDLSS5OneMinusEditor, Error,
            TEXT("Screen percentage stage FAILED: requested=%d readback=%s state=%s route=%s frame=%llu generation=%llu submitted=%s completed=%s detail=%s"),
            RequestedPercentage,
            Readback.IsSet() ? *FString::FromInt(Readback.GetValue()) : TEXT("unavailable"),
            DLSS5OneMinus::LexToString(Status.State),
            *Status.EffectiveRoute,
            Status.FrameNumber,
            Status.ConfigGeneration,
            Status.bOutputSubmitted ? TEXT("true") : TEXT("false"),
            Status.bOutputCompleted ? TEXT("true") : TEXT("false"),
            *Status.Detail);
        DLSS5OneMinusEditorViewport::SetScreenPercentage(100);
        ValidationTickerHandle.Reset();
        return false;
    }

    void StartScreenPercentageValidation()
    {
        AsyncTask(ENamedThreads::GameThread, []
        {
            if (ValidationTickerHandle.IsValid())
            {
                UE_LOG(LogDLSS5OneMinusEditor, Warning, TEXT("A route validation is already running."));
                return;
            }

            ApplyEditorNR(1, true);
            SetIntCVar(TEXT("r.NGX.DLSSNR.DebugView"), 0);
            SetIntCVar(TEXT("r.NGX.DLSS.Enable"), 1);
            IConsoleManager::Get().CallAllConsoleVariableSinks();

            const TSharedRef<FScreenPercentageValidationState> State = MakeShared<FScreenPercentageValidationState>();
            ValidationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                TEXT("DLSS5OneMinus.ScreenPercentageValidation"),
                0.0f,
                [State](float DeltaSeconds)
                {
                    return TickScreenPercentageValidation(DeltaSeconds, State);
                });
            ApplyScreenPercentageValidationStage(State);
            UE_LOG(LogDLSS5OneMinusEditor, Display,
                TEXT("Started active editor viewport screen percentage validation at 10%%, 33%%, 31%%, 33%%, 100%%, 150%%, and 200%%."));
        });
    }
}

void FDLSS5OneMinusEditorModule::StartupModule()
{
    UE_LOG(LogDLSS5OneMinusEditor, Display, TEXT("DLSS5-OneMinus editor module starting."));
    PreBeginPIEHandle = FEditorDelegates::PreBeginPIE.AddStatic(&ReleaseViewportForPIE);
    EndPIEHandle = FEditorDelegates::EndPIE.AddStatic(&ReleaseViewportAfterPIE);
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ControlPanelTabName,
        FOnSpawnTab::CreateStatic(&SpawnControlPanel))
        .SetMenuType(ETabSpawnerMenuType::Hidden)
        .SetDisplayName(LOCTEXT("TabDisplayName", "Neural Rendering Lab"))
        .SetTooltipText(LOCTEXT("TabTooltip", "Open the independent Neural Rendering control panel."));

    OpenControlPanelCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.Controls"),
        TEXT("Open the dockable DLSS5-OneMinus Neural Rendering Lab."),
        FConsoleCommandDelegate::CreateStatic(&InvokeControlPanel),
        ECVF_Default);

    ValidatePostToneCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ValidatePostTone"),
        TEXT("Run a bounded editor viewport validation and capture NR-off/on screenshots under Saved/DLSS5OneMinus/Validation."),
        FConsoleCommandDelegate::CreateLambda([]
        {
            StartRouteValidation(1, 100, TEXT("Post Tone"), TEXT("PostTone"));
        }),
        ECVF_Default);

    ValidatePreDLSSCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ValidatePreDLSS"),
        TEXT("Run a bounded Pre DLSS editor viewport validation at 67% and capture NR-off/on screenshots."),
        FConsoleCommandDelegate::CreateLambda([]
        {
            StartRouteValidation(0, 67, TEXT("Pre DLSS"), TEXT("PreDLSS"));
        }),
        ECVF_Default);

    ValidatePostDLAACommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ValidatePostDLAA"),
        TEXT("Run a bounded Post DLAA editor viewport validation at 100% and capture NR-off/on screenshots."),
        FConsoleCommandDelegate::CreateLambda([]
        {
            StartRouteValidation(2, 100, TEXT("Post DLAA"), TEXT("PostDLAA"));
        }),
        ECVF_Default);

    ValidateRouteSwitchCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ValidateRouteSwitch"),
        TEXT("Validate an in-session Pre DLSS 67% to Post Tone 100% transition."),
        FConsoleCommandDelegate::CreateStatic(&StartRouteSwitchValidation),
        ECVF_Default);

    ValidateScreenPercentageCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ValidateScreenPercentage"),
        TEXT("Validate active editor viewport percentages at 10, 33, 31, 33, 100, 150 and 200, then restore 100."),
        FConsoleCommandDelegate::CreateStatic(&StartScreenPercentageValidation),
        ECVF_Default);

    DebugDifferenceCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.DebugDifference"),
        TEXT("Enable the Post Tone NR difference view in the current editor viewport."),
        FConsoleCommandDelegate::CreateLambda([] { ActivateDebugView(1); }),
        ECVF_Default);

    DebugSliceCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.DebugSlice"),
        TEXT("Enable the Post Tone original/NR/difference slice view in the current editor viewport."),
        FConsoleCommandDelegate::CreateLambda([] { ActivateDebugView(2); }),
        ECVF_Default);

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FDLSS5OneMinusEditorModule::RegisterMenus));
}

void FDLSS5OneMinusEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
    FToolMenuSection& Section = ToolsMenu->FindOrAddSection(
        TEXT("DLSS5OneMinus"),
        LOCTEXT("Section", "DLSS5-OneMinus"));

    Section.AddEntry(FToolMenuEntry::InitMenuEntry(
        TEXT("OpenDLSS5OneMinusControlPanel"),
        LOCTEXT("OpenLabel", "Neural Rendering Lab"),
        LOCTEXT("OpenTooltip", "Open the independent DLSS5-OneMinus Neural Rendering Lab."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
        FUIAction(FExecuteAction::CreateStatic(&InvokeControlPanel))));
}

void FDLSS5OneMinusEditorModule::ShutdownModule()
{
    DLSS5OneMinusEditorViewport::ReleaseNeuralRenderingControl(
        TEXT("DLSS5-OneMinus editor module shutting down."));
    if (PreBeginPIEHandle.IsValid())
    {
        FEditorDelegates::PreBeginPIE.Remove(PreBeginPIEHandle);
        PreBeginPIEHandle.Reset();
    }
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }
    if (ValidationTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ValidationTickerHandle);
        ValidationTickerHandle.Reset();
    }

    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);

    if (OpenControlPanelCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(OpenControlPanelCommand);
        OpenControlPanelCommand = nullptr;
    }

    if (ValidatePostToneCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ValidatePostToneCommand);
        ValidatePostToneCommand = nullptr;
    }
    if (ValidatePreDLSSCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ValidatePreDLSSCommand);
        ValidatePreDLSSCommand = nullptr;
    }
    if (ValidatePostDLAACommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ValidatePostDLAACommand);
        ValidatePostDLAACommand = nullptr;
    }
    if (ValidateRouteSwitchCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ValidateRouteSwitchCommand);
        ValidateRouteSwitchCommand = nullptr;
    }
    if (ValidateScreenPercentageCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(ValidateScreenPercentageCommand);
        ValidateScreenPercentageCommand = nullptr;
    }
    if (DebugDifferenceCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(DebugDifferenceCommand);
        DebugDifferenceCommand = nullptr;
    }
    if (DebugSliceCommand)
    {
        IConsoleManager::Get().UnregisterConsoleObject(DebugSliceCommand);
        DebugSliceCommand = nullptr;
    }

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ControlPanelTabName);
}

IMPLEMENT_MODULE(FDLSS5OneMinusEditorModule, DLSS5OneMinusEditor)

#undef LOCTEXT_NAMESPACE
