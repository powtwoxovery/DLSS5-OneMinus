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
}
