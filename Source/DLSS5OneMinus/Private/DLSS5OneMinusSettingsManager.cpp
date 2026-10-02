#include "DLSS5OneMinusSettingsManager.h"

#include "DLSS5OneMinusProjectSettings.h"
#include "DLSS5OneMinusStatus.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeRWLock.h"
#include "SceneInterface.h"
#include "SceneView.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusSettings, Log, All);

namespace
{
    FRWLock GSettingsLock;
    TMap<const FSceneInterface*, FDLSS5OneMinusEffectiveState> GSceneSettings;
    TAtomic<uint64> GSettingsRevision { 0 };

    struct FConsoleVariableCache
    {
        FConsoleVariableCache()
        {
            static const TCHAR* Names[] = {
                TEXT("r.NGX.DLSS.Enable"), TEXT("r.NGX.DLSSNR.Enable"),
                TEXT("r.NGX.DLSSNR.Path"), TEXT("r.NGX.DLSSNR.HDRComposition"),
                TEXT("r.NGX.DLSSNR.WhitePoint"), TEXT("r.NGX.DLSSNR.Strength"),
                TEXT("r.NGX.DLSSNR.Style"), TEXT("r.NGX.DLSSNR.Intensity"),
                TEXT("r.NGX.DLSSNR.LocalToneStrength"), TEXT("r.NGX.DLSSNR.LocalStructureStrength"),
                TEXT("r.NGX.DLSSNR.SkinStructureStrength"), TEXT("r.NGX.DLSSNR.AutoMask"),
                TEXT("r.NGX.DLSSNR.AllowSceneCaptures"), TEXT("r.NGX.DLSSNR.DebugView"),
                TEXT("r.NGX.DLSSNR.SliceOffsetPercent")
            };
            Values.Reserve(UE_ARRAY_COUNT(Names));
            for (const TCHAR* Name : Names)
            {
                Values.Emplace(Name, IConsoleManager::Get().FindConsoleVariable(Name));
            }
        }

        IConsoleVariable* Find(const TCHAR* Name) const
        {
            for (const TPair<const TCHAR*, IConsoleVariable*>& Value : Values)
            {
                if (FCString::Strcmp(Value.Key, Name) == 0)
                {
                    return Value.Value;
                }
            }
            return nullptr;
        }

        TArray<TPair<const TCHAR*, IConsoleVariable*>> Values;
    };

    IConsoleVariable* FindCVar(const TCHAR* Name)
    {
        static const FConsoleVariableCache Cache;
        return Cache.Find(Name);
    }

    bool IsExternalOverride(const IConsoleVariable* Variable)
    {
        return Variable != nullptr
            && (Variable->GetFlags() & ECVF_SetByMask) > ECVF_SetByGameOverride;
    }

    void SetInt(const TCHAR* Name, const int32 Value)
    {
        if (IConsoleVariable* Variable = FindCVar(Name))
        {
            Variable->Set(Value, ECVF_SetByGameOverride);
        }
    }

    void SetFloat(const TCHAR* Name, const float Value)
    {
        if (IConsoleVariable* Variable = FindCVar(Name))
        {
            Variable->Set(Value, ECVF_SetByGameOverride);
        }
    }

    int32 ReadExternalInt(const TCHAR* Name, const int32 Value)
    {
        const IConsoleVariable* Variable = FindCVar(Name);
        return IsExternalOverride(Variable) ? Variable->GetInt() : Value;
    }

    float ReadExternalFloat(const TCHAR* Name, const float Value)
    {
        const IConsoleVariable* Variable = FindCVar(Name);
        return IsExternalOverride(Variable) ? Variable->GetFloat() : Value;
    }

    FDLSS5OneMinusRenderSettings ToRenderSettings(const FDLSS5OneMinusEffectiveState& State)
    {
        FDLSS5OneMinusRenderSettings Result;
        Result.bEnabled = ReadExternalInt(TEXT("r.NGX.DLSSNR.Enable"), State.bEnabled ? 1 : 0) != 0;
        Result.bAllowSceneCaptures = ReadExternalInt(
            TEXT("r.NGX.DLSSNR.AllowSceneCaptures"), State.bAllowSceneCaptures ? 1 : 0) != 0;
        Result.Route = FMath::Clamp(ReadExternalInt(
            TEXT("r.NGX.DLSSNR.Path"), static_cast<int32>(State.Settings.Route)), 0, 2);
        Result.CompositionStrength = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.Strength"), State.Settings.CompositionStrength), 0.0f, 2.0f); // composition strength 0..2 (default 1); >1 extrapolates the NR edit
        Result.HDRComposition = FMath::Clamp(ReadExternalInt(
            TEXT("r.NGX.DLSSNR.HDRComposition"), static_cast<int32>(State.Settings.HDRComposition)), 0, 1);
        Result.HDRModelWhite = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.WhitePoint"), State.Settings.HDRModelWhite), 0.25f, 4.0f);
        Result.Style = FMath::Clamp(ReadExternalInt(TEXT("r.NGX.DLSSNR.Style"), State.Settings.Style), 0, 2);
        Result.Intensity = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.Intensity"), State.Settings.Intensity), 0.0f, 2.0f);
        Result.LocalToneStrength = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.LocalToneStrength"), State.Settings.LocalToneStrength), 0.0f, 2.0f);
        Result.LocalStructureStrength = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.LocalStructureStrength"), State.Settings.LocalStructureStrength), 0.0f, 2.0f);
        Result.SkinStructureStrength = FMath::Clamp(ReadExternalFloat(
            TEXT("r.NGX.DLSSNR.SkinStructureStrength"), State.Settings.SkinStructureStrength), -1.0f, 2.0f);
        Result.bAutoMask = ReadExternalInt(
            TEXT("r.NGX.DLSSNR.AutoMask"), State.Settings.bAutoMask ? 1 : 0) != 0;
        const IConsoleVariable* DebugView = FindCVar(TEXT("r.NGX.DLSSNR.DebugView"));
        Result.DebugView = FMath::Clamp(DebugView != nullptr ? DebugView->GetInt() : 0, 0, 2);
        const IConsoleVariable* SliceOffset = FindCVar(TEXT("r.NGX.DLSSNR.SliceOffsetPercent"));
        Result.SliceOffsetPercent = FMath::Clamp(
            SliceOffset != nullptr ? SliceOffset->GetFloat() : 0.0f,
            0.0f,
            100.0f);
        Result.Revision = static_cast<uint64>(FMath::Max<int64>(State.Revision, 0));
        return Result;
    }

    FDLSS5OneMinusEffectiveState MakeFallbackState()
    {
        FDLSS5OneMinusEffectiveState State;
        State.Settings = FDLSS5OneMinusSettings::VerifiedDefaults();
        State.Source = TEXT("Verified Fallback");
        return State;
    }

    void MirrorToCVars(const FDLSS5OneMinusEffectiveState& State)
    {
        SetInt(TEXT("r.NGX.DLSSNR.Enable"), State.bEnabled ? 1 : 0);
        SetInt(TEXT("r.NGX.DLSSNR.Path"), static_cast<int32>(State.Settings.Route));
        SetInt(TEXT("r.NGX.DLSSNR.HDRComposition"), static_cast<int32>(State.Settings.HDRComposition));
        SetFloat(TEXT("r.NGX.DLSSNR.WhitePoint"), State.Settings.HDRModelWhite);
        SetFloat(TEXT("r.NGX.DLSSNR.Strength"), State.Settings.CompositionStrength);
        SetInt(TEXT("r.NGX.DLSSNR.Style"), State.Settings.Style);
        SetFloat(TEXT("r.NGX.DLSSNR.Intensity"), State.Settings.Intensity);
        SetFloat(TEXT("r.NGX.DLSSNR.LocalToneStrength"), State.Settings.LocalToneStrength);
        SetFloat(TEXT("r.NGX.DLSSNR.LocalStructureStrength"), State.Settings.LocalStructureStrength);
        SetFloat(TEXT("r.NGX.DLSSNR.SkinStructureStrength"), State.Settings.SkinStructureStrength);
        SetInt(TEXT("r.NGX.DLSSNR.AutoMask"), State.Settings.bAutoMask ? 1 : 0);
        SetInt(TEXT("r.NGX.DLSSNR.AllowSceneCaptures"), State.bAllowSceneCaptures ? 1 : 0);
        if (State.bEnabled)
        {
            SetInt(TEXT("r.NGX.DLSS.Enable"), 1);
        }
        IConsoleManager::Get().CallAllConsoleVariableSinks();
    }
}

FDLSS5OneMinusApplyResult DLSS5OneMinus::ApplySettings(
    UWorld* World,
    const FDLSS5OneMinusSettings& Settings,
    const bool bEnable,
    const bool bAllowSceneCaptures,
    const FName Source)
{
    FDLSS5OneMinusApplyResult Result;
    Result.Source = Source;
    if (World == nullptr || World->Scene == nullptr)
    {
        Result.State = EDLSS5OneMinusApplyState::Rejected;
        Result.Reason = NSLOCTEXT("DLSS5OneMinus", "ApplyNoWorld", "No initialized rendering world was available.");
        return Result;
    }

    FDLSS5OneMinusEffectiveState State;
    State.Settings = Settings.Normalized();
    State.bEnabled = bEnable;
    State.bAllowSceneCaptures = bAllowSceneCaptures;
    State.Source = Source.IsNone() ? FName(TEXT("Runtime")) : Source;
    State.Revision = static_cast<int64>(GSettingsRevision.IncrementExchange() + 1);

    {
        FRWScopeLock Lock(GSettingsLock, SLT_Write);
        GSceneSettings.Add(World->Scene, State);
    }

    MirrorToCVars(State);
    InvalidateOutputStatus(TEXT("Neural Rendering settings changed; waiting for output from the new effective state."));

    const FDLSS5OneMinusStatusSnapshot RuntimeStatus = GetStatus();
    Result.State = bEnable
        && (!RuntimeStatus.bVendorPluginAvailable || !RuntimeStatus.bUnofficialRuntimeAvailable)
        ? EDLSS5OneMinusApplyState::Pending
        : EDLSS5OneMinusApplyState::Applied;
    Result.Reason = Result.State == EDLSS5OneMinusApplyState::Pending
        ? FText::FromString(RuntimeStatus.Detail)
        : NSLOCTEXT("DLSS5OneMinus", "ApplyAccepted", "Settings were applied to this world.");
    Result.Effective = State;

    UE_LOG(LogDLSS5OneMinusSettings, Display,
        TEXT("Applied source='%s' world='%s' enabled=%s route=%d revision=%lld."),
        *State.Source.ToString(),
        *World->GetName(),
        State.bEnabled ? TEXT("true") : TEXT("false"),
        static_cast<int32>(State.Settings.Route),
        State.Revision);
    return Result;
}

FDLSS5OneMinusApplyResult DLSS5OneMinus::ApplyProjectDefaults(UWorld* World, const FName Source)
{
    const UDLSS5OneMinusProjectSettings* ProjectSettings = GetDefault<UDLSS5OneMinusProjectSettings>();
    return ApplySettings(
        World,
        ProjectSettings->ResolveDefaultSettings(),
        ProjectSettings->ShouldEnableForWorld(World),
        ProjectSettings->bAllowSceneCapturesByDefault,
        Source);
}

bool DLSS5OneMinus::GetEffectiveSettings(const UWorld* World, FDLSS5OneMinusEffectiveState& OutState)
{
    if (World == nullptr || World->Scene == nullptr)
    {
        return false;
    }
    FRWScopeLock Lock(GSettingsLock, SLT_ReadOnly);
    if (const FDLSS5OneMinusEffectiveState* State = GSceneSettings.Find(World->Scene))
    {
        OutState = *State;
        return true;
    }
    return false;
}

FDLSS5OneMinusRenderSettings DLSS5OneMinus::GetRenderSettings(const FSceneView& View)
{
    const FSceneInterface* Scene = View.Family != nullptr ? View.Family->Scene : nullptr;
    // Scope the Difference/Slice debug views to the editor viewport the Lab panel owns.
    // DebugView and SliceOffset are global CVars; PIE, scene captures and MRQ always render the normal image.
    FDLSS5OneMinusRenderSettings Result = GetRenderSettingsForScene(Scene);
    const uint64 TargetViewId = GetTargetViewId();
    if (TargetViewId == 0 || reinterpret_cast<uint64>(View.State) != TargetViewId)
    {
        Result.DebugView = 0;
    }
    return Result;
}

FDLSS5OneMinusRenderSettings DLSS5OneMinus::GetRenderSettingsForScene(const FSceneInterface* Scene)
{
    FDLSS5OneMinusEffectiveState State = MakeFallbackState();
    if (Scene != nullptr)
    {
        FRWScopeLock Lock(GSettingsLock, SLT_ReadOnly);
        if (const FDLSS5OneMinusEffectiveState* Found = GSceneSettings.Find(Scene))
        {
            State = *Found;
        }
    }
    return ToRenderSettings(State);
}

void DLSS5OneMinus::RemoveWorldSettings(const UWorld* World)
{
    if (World != nullptr && World->Scene != nullptr)
    {
        FRWScopeLock Lock(GSettingsLock, SLT_Write);
        GSceneSettings.Remove(World->Scene);
    }
}

void DLSS5OneMinus::ResetAllSettingsState()
{
    FRWScopeLock Lock(GSettingsLock, SLT_Write);
    GSceneSettings.Reset();
}

FString DLSS5OneMinus::GetConsoleOverrideSummary()
{
    static const TCHAR* Names[] = {
        TEXT("r.NGX.DLSSNR.Enable"), TEXT("r.NGX.DLSSNR.Path"),
        TEXT("r.NGX.DLSSNR.HDRComposition"), TEXT("r.NGX.DLSSNR.WhitePoint"),
        TEXT("r.NGX.DLSSNR.Strength"), TEXT("r.NGX.DLSSNR.Style"),
        TEXT("r.NGX.DLSSNR.Intensity"), TEXT("r.NGX.DLSSNR.LocalToneStrength"),
        TEXT("r.NGX.DLSSNR.LocalStructureStrength"), TEXT("r.NGX.DLSSNR.SkinStructureStrength"),
        TEXT("r.NGX.DLSSNR.AutoMask"), TEXT("r.NGX.DLSSNR.AllowSceneCaptures")
    };
    TArray<FString> Overrides;
    for (const TCHAR* Name : Names)
    {
        if (const IConsoleVariable* Variable = FindCVar(Name); IsExternalOverride(Variable))
        {
            Overrides.Add(FString::Printf(TEXT("%s (%s)"), Name,
                GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Variable->GetFlags()))));
        }
    }
    return FString::Join(Overrides, TEXT(", "));
}
