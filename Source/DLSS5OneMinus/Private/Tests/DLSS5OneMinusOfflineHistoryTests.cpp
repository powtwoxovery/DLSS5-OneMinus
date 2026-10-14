#include "DLSS5OneMinusOfflineHistory.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDLSS5OfflineHistoryTest,
    "DLSS5OneMinus.OfflineHistory.CadenceAndNFOR",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDLSS5OfflineHistoryTest::RunTest(const FString& Parameters)
{
    using DLSS5OneMinus::FOfflineSampleHistory;
    const FIntRect Rect(0, 0, 2560, 1440);
    FOfflineSampleHistory History;
    int32 Evaluations = 0;
    for (uint32 Sample = 1; Sample <= 256; ++Sample)
    {
        Evaluations += History.Advance(Sample, 256, Sample == 1, Rect, 2) ? 1 : 0;
    }
    TestEqual(TEXT("256 PT samples advance NR exactly once"), Evaluations, 1);
    TestTrue(TEXT("Cut on skipped sample survives until final sample"), History.bReset);
    History.Advance(256, 256, false, Rect, 2);
    TestTrue(TEXT("Second initial noisy frame does not inherit NR history"), History.bReset);
    History.Advance(256, 256, false, Rect, 2);
    TestEqual(TEXT("Third completed frame delivers NFOR frame zero"), History.ReadSlot, 0);
    TestTrue(TEXT("Raw-to-denoised backwards jump resets NR"), History.bReset);
    History.Advance(256, 256, false, Rect, 2);
    TestEqual(TEXT("Fourth completed frame uses frame one's guides"), History.ReadSlot, 1);
    TestEqual(TEXT("Motion starts at previous delivered frame"), History.PreviousSlot, 0);
    TestFalse(TEXT("Consecutive delayed images retain NR history"), History.bReset);

    for (int32 Frame = 4; Frame < 25; ++Frame)
    {
        History.Advance(256, 256, false, Rect, 2);
        TestEqual(TEXT("Delayed frame survives ring wrap"), History.ReadSlot, (Frame - 2) % 8);
        TestEqual(TEXT("Previous frame survives ring wrap"), History.PreviousSlot, (Frame - 3) % 8);
        TestFalse(TEXT("Ring wrap is not a cut"), History.bReset);
    }
    TestFalse(TEXT("Cut on partial frame is skipped"), History.Advance(1, 256, true, Rect, 2));
    History.Advance(256, 256, false, Rect, 2);
    TestTrue(TEXT("Cut invalidates history"), History.bReset);
    TestEqual(TEXT("Cut begins a new NFOR window"), History.Completed, int64(1));

    History.Advance(512, 512, false, Rect, 2);
    TestTrue(TEXT("Warmup/sample count transition resets"), History.bReset);
    TestEqual(TEXT("Sample count transition restarts window"), History.Completed, int64(1));
    History.Advance(512, 512, false, FIntRect(0, 0, 1920, 1080), 2);
    TestTrue(TEXT("Resolution change resets"), History.bReset);
    History.Advance(512, 512, false, Rect, 0);
    TestTrue(TEXT("Denoiser delay change resets"), History.bReset);
    History.Advance(512, 512, false, Rect, 0);
    TestFalse(TEXT("Spatial denoising preserves consecutive NR history"), History.bReset);
    TestEqual(TEXT("No delay uses current guides"), History.ReadSlot, History.WriteSlot);

    FOfflineSampleHistory SingleSample;
    TestTrue(TEXT("One sample is a completed image"), SingleSample.Advance(1, 1, false, Rect, 0));
    TestFalse(TEXT("Missing PT state is not an image"), SingleSample.Advance(0, 0, false, Rect, 0));
    FOfflineSampleHistory OtherView;
    OtherView.Advance(1, 1, false, Rect, 0);
    TestTrue(TEXT("Separate view starts its own history"), OtherView.bReset);
    return true;
}
#endif
