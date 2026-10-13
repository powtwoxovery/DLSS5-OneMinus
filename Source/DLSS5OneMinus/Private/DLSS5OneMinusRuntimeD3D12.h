#pragma once

#include "CoreMinimal.h"
#include "DLSS5OneMinusSettings.h"
#include "RenderGraphFwd.h"

class FSceneView;

struct FDLSS5OneMinusModelPassInputs
{
    FRDGTextureRef Color = nullptr;
    FRDGTextureRef Depth = nullptr;
    FRDGTextureRef MotionVectors = nullptr;
    FIntRect ColorRect;
    FIntRect DepthRect;
    FIntRect MotionVectorRect;
    bool bReset = false;
    uint64 ViewId = 0;
    int32 Route = 1;
    FDLSS5OneMinusRenderSettings Settings;
};

namespace DLSS5OneMinus
{
    bool IsNGXSnippetBootstrapReady();
    FRDGTextureRef AddModelPass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FDLSS5OneMinusModelPassInputs& Inputs);

    // Runs NR once on a standalone image (already encoded for the model, sRGB 0..1), with no
    // history, zero motion and flat depth. Game thread; blocks until the result is read back.
    bool EvaluateStandaloneModel(
        const TArray<FFloat16Color>& EncodedInput,
        FIntPoint Size,
        const FDLSS5OneMinusRenderSettings& Settings,
        int32 Route,
        TArray<FFloat16Color>& OutModel,
        float ConstantDepth = 0.0f);
}
