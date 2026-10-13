#pragma once

#include "CoreMinimal.h"
#include "DLSS5OneMinusSettings.h"
#include "RHIFwd.h"

// Multi-view consistency: replaces each view's own NR tone decision with a shared
// reference tone (or removes it), keeping the view's NR detail. See the MRQ setting's
// "Multi-View Consistency" properties for the external contract.
namespace DLSS5OneMinus
{
    constexpr int32 ConsistencyContractVersion = 1;

    enum class EConsistencyMode : int32
    {
        Off = 0,
        Consumer = 1,
        DetailOnly = 2,
    };

    struct FConsistencyContext
    {
        EConsistencyMode Mode = EConsistencyMode::Off;
        FString ReferenceCompositePattern;
        int32 ReferenceFrameDigits = 8;
        int32 ReferenceFrameOffset = 0;
        FString ReferenceMapDirectory;
        FString ReferenceViewportId;
    };

    // Per-frame reference data resolved on the game thread and read by the render thread.
    struct FConsistencyFrameReference
    {
        FTextureRHIRef ToneTexture;   // log-ratio tone in dome UV
        FTextureRHIRef MapTexture;    // view UV -> dome UV (RG), validity (A)
        int32 FrameKey = INDEX_NONE;
    };

    void SetConsistencyContext(const FConsistencyContext& Context);
    void ClearConsistencyContext();
    FConsistencyContext GetConsistencyContext();
    // Game thread, at pipeline setup: makes sure a tone cache exists for every reference composite
    // (evaluating NR on the composites that lack one). Returns false if any composite failed.
    bool PrepareConsistencyReferences(const FDLSS5OneMinusRenderSettings& Settings);
    // Game thread, every rendered frame: loads that frame's reference tone and hands it to the render thread.
    // The frame is the MRQ output frame index (warm-up -1 counts as 0); ReferenceFrameOffset is added to it.
    void UpdateConsistencyFrame(int32 OutputFrameNumber);

    // Effective mode for rendering: the r.NGX.DLSSNR.ConsistencyMode override when set, else the pipeline context.
    EConsistencyMode GetEffectiveConsistencyMode();
    // Render thread.
    FConsistencyFrameReference GetConsistencyFrameReference();
    // True once per pipeline context, so a missing reference is reported once per render rather than every frame.
    bool ShouldWarnMissingConsistencyReference();
    // True once per pipeline context, for logging the first frame that actually uses the reference.
    bool ShouldLogConsistencyReferenceUse();
    float GetConsistencyToneBlur();
    int32 GetConsistencyDebug();
}
