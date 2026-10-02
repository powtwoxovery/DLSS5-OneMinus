#pragma once

#include "CoreMinimal.h"

class ACameraActor;

namespace DLSS5OneMinusEditorViewport
{
    bool SetViewFromCamera(ACameraActor* CameraActor);
    bool SetScreenPercentage(int32 Percentage);
    TOptional<int32> GetScreenPercentage();
    int32 NormalizeScreenPercentage(int32 Percentage, int32 Route, bool bNeuralRendering = true);
    void ReleaseNeuralRenderingControl(const TCHAR* Reason);
    bool Invalidate();
}
