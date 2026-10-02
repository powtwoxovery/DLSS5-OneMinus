#pragma once

#include "CoreMinimal.h"
#include "DLSS5OneMinusSettings.generated.h"

// Define one reflected settings schema and explicit application result for Neural Rendering.
// CVars are a useful compatibility surface but are not serialized and cannot express saved/profile/effective state.

UENUM(BlueprintType)
enum class EDLSS5OneMinusRoute : uint8
{
    PreDLSS = 0 UMETA(DisplayName = "Pre DLSS"),
    PostTone = 1 UMETA(DisplayName = "Post Tone"),
    PostDLAA = 2 UMETA(DisplayName = "Post DLAA")
};

UENUM(BlueprintType)
enum class EDLSS5OneMinusHDRComposition : uint8
{
    Additive = 0 UMETA(DisplayName = "Additive"),
    GuardedRatio = 1 UMETA(DisplayName = "Guarded Ratio")
};

UENUM(BlueprintType)
enum class EDLSS5OneMinusApplyState : uint8
{
    Applied,
    Pending,
    Rejected
};

USTRUCT(BlueprintType)
struct DLSS5ONEMINUS_API FDLSS5OneMinusSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
    EDLSS5OneMinusRoute Route = EDLSS5OneMinusRoute::PostTone;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0")) // composition strength 0..2 (default 1); >1 extrapolates the NR edit
    float CompositionStrength = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
    EDLSS5OneMinusHDRComposition HDRComposition = EDLSS5OneMinusHDRComposition::Additive;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.25", UIMax = "4.0"))
    float HDRModelWhite = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model", meta = (ClampMin = "0", ClampMax = "2", UIMin = "0", UIMax = "2"))
    int32 Style = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
    float Intensity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
    float LocalToneStrength = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
    float LocalStructureStrength = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model", meta = (ClampMin = "-1.0", ClampMax = "2.0", UIMin = "-1.0", UIMax = "2.0"))
    float SkinStructureStrength = -1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model")
    bool bAutoMask = true;

    static FDLSS5OneMinusSettings VerifiedDefaults();
    FDLSS5OneMinusSettings Normalized() const;
    bool NearlyEquals(const FDLSS5OneMinusSettings& Other, float Tolerance = KINDA_SMALL_NUMBER) const;
};

USTRUCT(BlueprintType)
struct DLSS5ONEMINUS_API FDLSS5OneMinusEffectiveState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    FDLSS5OneMinusSettings Settings;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    bool bEnabled = false;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    bool bAllowSceneCaptures = false;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    FName Source = NAME_None;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    int64 Revision = 0;
};

USTRUCT(BlueprintType)
struct DLSS5ONEMINUS_API FDLSS5OneMinusApplyResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    EDLSS5OneMinusApplyState State = EDLSS5OneMinusApplyState::Rejected;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    FName Source = NAME_None;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    FText Reason;

    UPROPERTY(BlueprintReadOnly, Category = "DLSS5-OneMinus")
    FDLSS5OneMinusEffectiveState Effective;

    bool WasAccepted() const { return State != EDLSS5OneMinusApplyState::Rejected; }
};

// Render-safe, UObject-free snapshot selected per scene/view.
struct DLSS5ONEMINUS_API FDLSS5OneMinusRenderSettings
{
    bool bEnabled = false;
    bool bAllowSceneCaptures = false;
    int32 Route = 1;
    float CompositionStrength = 1.0f;
    int32 HDRComposition = 0;
    float HDRModelWhite = 1.0f;
    int32 Style = 0;
    float Intensity = 1.0f;
    float LocalToneStrength = 1.0f;
    float LocalStructureStrength = 1.0f;
    float SkinStructureStrength = -1.0f;
    bool bAutoMask = true;
    int32 DebugView = 0;
    float SliceOffsetPercent = 0.0f;
    uint64 Revision = 0;
};

