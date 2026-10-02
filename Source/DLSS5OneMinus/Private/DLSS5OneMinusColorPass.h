#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "ScreenPass.h"

class FSceneView;

namespace DLSS5OneMinus
{
    FRDGTextureRef AddDisplayEncodePass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTextureRef InputColor,
        const FIntRect& InputRect);

    FRDGTextureRef AddHDREncodePass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTextureRef InputColor,
        const FIntRect& InputRect,
        float WhitePoint);

    FRDGTextureRef AddMotionGuidePass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTextureRef SceneDepth,
        FRDGTextureRef SceneVelocity,
        const FIntRect& InputRect);

    void AddPostToneResolvePass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& OriginalColor,
        FRDGTextureRef ModelColor,
        float Strength,
        int32 DebugView,
        float SliceOffsetPercent,
        const FScreenPassRenderTarget& Output);

    void AddHDRResolvePass(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FScreenPassTexture& OriginalColor,
        FRDGTextureRef ModelColor,
        float Strength,
        float WhitePoint,
        int32 CompositionMode,
        int32 DebugView,
        float SliceOffsetPercent,
        const FScreenPassRenderTarget& Output);
}
