#pragma once

#include "CoreMinimal.h"

enum class EDLSS5OneMinusRuntimeState : uint8
{
    Disabled,
    MissingVendor,
    MissingUnofficialRuntime,
    DependenciesReady,
    Probing,
    Initializing,
    WaitingForCreateFence,
    Ready,
    Retiring,
    FaultedQuarantined
};

struct DLSS5ONEMINUS_API FDLSS5OneMinusStatusSnapshot
{
    EDLSS5OneMinusRuntimeState State = EDLSS5OneMinusRuntimeState::Disabled;
    FString Detail;
    FString EffectiveRoute = TEXT("None");
    uint64 FrameNumber = 0;
    uint64 ViewId = 0;
    uint64 ConfigGeneration = 0;
    uint64 SubmittedFrameNumber = 0;
    uint64 SubmittedViewId = 0;
    uint64 SubmittedConfigGeneration = 0;
    uint64 LastSubmittedFence = 0;
    int32 LastEvaluateResult = 0;
    uint64 LastCompletedFence = 0;
    bool bVendorPluginAvailable = false;
    bool bUnofficialRuntimeAvailable = false;
    bool bNGXOwnershipProven = false;
    bool bOutputSubmitted = false;
    bool bOutputCompleted = false;
};

namespace DLSS5OneMinus
{
    DLSS5ONEMINUS_API FDLSS5OneMinusStatusSnapshot GetStatus();
    DLSS5ONEMINUS_API void SetTargetViewId(uint64 ViewId);
    DLSS5ONEMINUS_API uint64 GetTargetViewId();
    DLSS5ONEMINUS_API void SetTargetRenderTargetId(uint64 RenderTargetId);
    DLSS5ONEMINUS_API uint64 GetTargetRenderTargetId();
    DLSS5ONEMINUS_API void InvalidateOutputStatus(const TCHAR* Reason);
    DLSS5ONEMINUS_API const TCHAR* LexToString(EDLSS5OneMinusRuntimeState State);
}
