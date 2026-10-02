#include "HAL/IConsoleManager.h"

namespace
{
    TAutoConsoleVariable<int32> CVarDLSS5OneMinusEnable(
        TEXT("r.NGX.DLSSNR.Enable"),
        0,
        TEXT("Request the independent DLSS5-OneMinus Neural Rendering path. Default: 0."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusPath(
        TEXT("r.NGX.DLSSNR.Path"),
        1,
        TEXT("Route: 0 Pre DLSS, 1 Post Tone, 2 Post DLAA."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusHDRComposition(
        TEXT("r.NGX.DLSSNR.HDRComposition"),
        0,
        TEXT("HDR composition: 0 additive, 1 guarded ratio."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusWhitePoint(
        TEXT("r.NGX.DLSSNR.WhitePoint"),
        1.0f,
        TEXT("Linear HDR value mapped to model-domain white."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusStrength(
        TEXT("r.NGX.DLSSNR.Strength"),
        1.0f,
        TEXT("Composition strength in the range 0..2 (1 = full NR edit; above 1 exaggerates it)."), // composition strength 0..2 (default 1); >1 extrapolates the NR edit
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusDebugView(
        TEXT("r.NGX.DLSSNR.DebugView"),
        0,
        TEXT("Debug view: 0 normal, 1 difference, 2 angled slices."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusSliceOffsetPercent(
        TEXT("r.NGX.DLSSNR.SliceOffsetPercent"),
        0.0f,
        TEXT("Horizontal phase offset for the angled Slice debug view in percent. Range: 0..100; 100 wraps to 0."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusStyle(
        TEXT("r.NGX.DLSSNR.Style"),
        0,
        TEXT("Model style: 0 default, 1 natural, 2 cinematic."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusIntensity(
        TEXT("r.NGX.DLSSNR.Intensity"),
        1.0f,
        TEXT("Model intensity in the range 0..2."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusLocalToneStrength(
        TEXT("r.NGX.DLSSNR.LocalToneStrength"),
        1.0f,
        TEXT("Local tone strength in the range 0..2."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusLocalStructureStrength(
        TEXT("r.NGX.DLSSNR.LocalStructureStrength"),
        1.0f,
        TEXT("Local structure strength in the range 0..2."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<float> CVarDLSS5OneMinusSkinStructureStrength(
        TEXT("r.NGX.DLSSNR.SkinStructureStrength"),
        -1.0f,
        TEXT("Skin structure strength: -1 automatic, otherwise 0..2."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusAutoMask(
        TEXT("r.NGX.DLSSNR.AutoMask"),
        1,
        TEXT("Enable automatic subject and skin masking."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusAllowSceneCaptures(
        TEXT("r.NGX.DLSSNR.AllowSceneCaptures"),
        0,
        TEXT("Allow NR on scene-capture views such as Movie Render Queue. Default: 0."),
        ECVF_RenderThreadSafe);

    TAutoConsoleVariable<int32> CVarDLSS5OneMinusEditorScreenPercentage(
        TEXT("r.NGX.DLSSNR.EditorScreenPercentage"),
        0,
        TEXT("Exact owned Level Editor viewport percentage. Values below UE's 25% preview floor use a bounded per-view legacy screen-percentage interface."),
        ECVF_Default);
}
