#include "DLSS5OneMinusSettings.h"

FDLSS5OneMinusSettings FDLSS5OneMinusSettings::VerifiedDefaults()
{
    return FDLSS5OneMinusSettings();
}

FDLSS5OneMinusSettings FDLSS5OneMinusSettings::Normalized() const
{
    FDLSS5OneMinusSettings Result = *this;
    const int32 RouteValue = FMath::Clamp(static_cast<int32>(Result.Route), 0, 2);
    Result.Route = static_cast<EDLSS5OneMinusRoute>(RouteValue);
    Result.CompositionStrength = FMath::Clamp(Result.CompositionStrength, 0.0f, 2.0f); // composition strength 0..2 (default 1); >1 extrapolates the NR edit
    Result.HDRComposition = static_cast<EDLSS5OneMinusHDRComposition>(
        FMath::Clamp(static_cast<int32>(Result.HDRComposition), 0, 1));
    Result.HDRModelWhite = FMath::Clamp(Result.HDRModelWhite, 0.25f, 4.0f);
    Result.Style = FMath::Clamp(Result.Style, 0, 2);
    Result.Intensity = FMath::Clamp(Result.Intensity, 0.0f, 2.0f);
    Result.LocalToneStrength = FMath::Clamp(Result.LocalToneStrength, 0.0f, 2.0f);
    Result.LocalStructureStrength = FMath::Clamp(Result.LocalStructureStrength, 0.0f, 2.0f);
    Result.SkinStructureStrength = FMath::Clamp(Result.SkinStructureStrength, -1.0f, 2.0f);
    return Result;
}

bool FDLSS5OneMinusSettings::NearlyEquals(const FDLSS5OneMinusSettings& Other, const float Tolerance) const
{
    return Route == Other.Route
        && HDRComposition == Other.HDRComposition
        && Style == Other.Style
        && bAutoMask == Other.bAutoMask
        && FMath::IsNearlyEqual(CompositionStrength, Other.CompositionStrength, Tolerance)
        && FMath::IsNearlyEqual(HDRModelWhite, Other.HDRModelWhite, Tolerance)
        && FMath::IsNearlyEqual(Intensity, Other.Intensity, Tolerance)
        && FMath::IsNearlyEqual(LocalToneStrength, Other.LocalToneStrength, Tolerance)
        && FMath::IsNearlyEqual(LocalStructureStrength, Other.LocalStructureStrength, Tolerance)
        && FMath::IsNearlyEqual(SkinStructureStrength, Other.SkinStructureStrength, Tolerance);
}
