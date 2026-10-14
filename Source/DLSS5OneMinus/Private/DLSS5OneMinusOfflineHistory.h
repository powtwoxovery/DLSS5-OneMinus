#pragma once

#include "CoreMinimal.h"

namespace DLSS5OneMinus
{
    // NFOR uses at most seven images. Keep one extra slot for the previous
    // delivered frame when the selected image is the oldest in its window.
    struct FOfflineSampleHistory
    {
        static constexpr int32 Capacity = 8;

        bool Advance(uint32 SampleIndex, uint32 SampleCount, bool bCameraCut,
            const FIntRect& InRect, int32 InDelay)
        {
            InDelay = FMath::Clamp(InDelay, 0, Capacity - 2);
            if (bCameraCut || SampleCount != Samples || InRect != Rect || InDelay != Delay)
            {
                Completed = 0;
                LastOutput = INDEX_NONE;
                Samples = SampleCount;
                Rect = InRect;
                Delay = InDelay;
            }

            // The renderer has incremented SampleIndex before post processing.
            // Preserve a cut seen on an early sample until the completed image.
            if (SampleCount == 0 || SampleIndex < SampleCount)
            {
                return false;
            }

            WriteSlot = Completed % Capacity;
            const int64 Output = Completed >= Delay ? Completed - Delay : Completed;
            ReadSlot = Output % Capacity;
            PreviousSlot = Output > 0 ? (Output - 1) % Capacity : ReadSlot;
            // NFOR's initial undenoised images precede its delayed output.
            // Never reproject across that backwards jump (or a cut).
            bReset = LastOutput == INDEX_NONE || Output != LastOutput + 1 || Completed < Delay;
            LastOutput = Output;
            ++Completed;
            return true;
        }

        int32 WriteSlot = 0;
        int32 ReadSlot = 0;
        int32 PreviousSlot = 0;
        bool bReset = true;
        int64 Completed = 0;

    private:
        uint32 Samples = 0;
        FIntRect Rect;
        int32 Delay = 0;
        int64 LastOutput = INDEX_NONE;
    };
}
