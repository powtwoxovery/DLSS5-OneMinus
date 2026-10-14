#pragma once

#include "SceneViewExtension.h"
#include "DLSS5OneMinusOfflineHistory.h"
#include "RenderTargetPool.h"

class FDLSS5OneMinusViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FDLSS5OneMinusViewExtension(const FAutoRegister& AutoRegister)
        : FSceneViewExtensionBase(AutoRegister)
    {
    }

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void PrePostProcessPass_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const FPostProcessingInputs& Inputs) override;
    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
        const FSceneView& InView,
        FAfterPassCallbackDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

private:
    struct FOfflineFrame
    {
        FViewMatrices Matrices;
        TRefCountPtr<IPooledRenderTarget> Velocity;
    };

    struct FOfflineViewHistory
    {
        DLSS5OneMinus::FOfflineSampleHistory Samples;
        FOfflineFrame Frames[DLSS5OneMinus::FOfflineSampleHistory::Capacity];
        uint64 LastSeen = 0;
    };

    // Stable allocations: RDG texture extraction writes into a frame slot.
    TMap<uint32, TUniquePtr<FOfflineViewHistory>> OfflineHistories;
    uint64 OfflineHistoryClock = 0;

    FScreenPassTexture PostDLAA_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessMaterialInputs& Inputs);

    FScreenPassTexture PostTone_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessMaterialInputs& Inputs);
};
