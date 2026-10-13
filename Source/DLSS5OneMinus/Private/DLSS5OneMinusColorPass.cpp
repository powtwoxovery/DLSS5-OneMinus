#include "DLSS5OneMinusColorPass.h"

#include "DLSS5OneMinusConsistency.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "PostProcess/PostProcessTonemap.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusConsistency, Log, All);

namespace
{
    constexpr int32 TileSize = 8;
    constexpr int32 ConsistencyToneFactor = 4;

    float NormalizeSliceOffset(const float SliceOffsetPercent)
    {
        const float ClampedPercent = FMath::Clamp(SliceOffsetPercent, 0.0f, 100.0f);
        return FMath::IsNearlyEqual(ClampedPercent, 100.0f) ? 0.0f : ClampedPercent * 0.01f;
    }

    bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
            && IsPCPlatform(Parameters.Platform)
            && IsD3DPlatform(Parameters.Platform);
    }

    class FDisplayEncodeCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FDisplayEncodeCS);
        SHADER_USE_PARAMETER_STRUCT(FDisplayEncodeCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_ENCODE"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InputColor)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
            SHADER_PARAMETER(FIntPoint, InputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(uint32, DisplayInputIsLinear)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FHDREncodeCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FHDREncodeCS);
        SHADER_USE_PARAMETER_STRUCT(FHDREncodeCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_HDR_ENCODE"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InputColor)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
            SHADER_PARAMETER(FIntPoint, InputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(float, WhitePoint)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FMotionGuideCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FMotionGuideCS);
        SHADER_USE_PARAMETER_STRUCT(FMotionGuideCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_MOTION_GUIDE"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneVelocity)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutputMotion)
            SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
            SHADER_PARAMETER(FIntPoint, InputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(uint32, VelocityIsValid)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FPostToneResolvePS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FPostToneResolvePS);
        SHADER_USE_PARAMETER_STRUCT(FPostToneResolvePS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_POST_TONE_RESOLVE"), 1);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, OriginalColor)
            SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSampler)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ModelColor)
            SHADER_PARAMETER_SAMPLER(SamplerState, ModelSampler)
            SHADER_PARAMETER(float, Strength)
            SHADER_PARAMETER(uint32, DebugView)
            SHADER_PARAMETER(uint32, DisplayInputIsLinear)
            SHADER_PARAMETER(float, SliceOffset)
            SHADER_PARAMETER(FIntPoint, OutputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()
    };

    class FHDRResolvePS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FHDRResolvePS);
        SHADER_USE_PARAMETER_STRUCT(FHDRResolvePS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_HDR_RESOLVE"), 1);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, OriginalColor)
            SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSampler)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ModelColor)
            SHADER_PARAMETER_SAMPLER(SamplerState, ModelSampler)
            SHADER_PARAMETER(float, Strength)
            SHADER_PARAMETER(float, WhitePoint)
            SHADER_PARAMETER(uint32, CompositionMode)
            SHADER_PARAMETER(uint32, DebugView)
            SHADER_PARAMETER(float, SliceOffset)
            SHADER_PARAMETER(FIntPoint, OutputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()
    };
    class FConsistencyLogRatioCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FConsistencyLogRatioCS);
        SHADER_USE_PARAMETER_STRUCT(FConsistencyLogRatioCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_CONSISTENCY_LOG_RATIO"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InputColor)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ConsModelColor)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
            SHADER_PARAMETER(FIntPoint, InputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(uint32, ConsDisplayDomain)
            SHADER_PARAMETER(float, ConsWhitePoint)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FConsistencyDownsampleCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FConsistencyDownsampleCS);
        SHADER_USE_PARAMETER_STRUCT(FConsistencyDownsampleCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_CONSISTENCY_DOWNSAMPLE"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InputColor)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(FIntPoint, ConsSourceSize)
            SHADER_PARAMETER(uint32, ConsFactor)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FConsistencyBlurCS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FConsistencyBlurCS);
        SHADER_USE_PARAMETER_STRUCT(FConsistencyBlurCS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_CONSISTENCY_BLUR"), 1);
            Environment.SetDefine(TEXT("THREADGROUP_SIZE"), TileSize);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InputColor)
            SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(FIntPoint, ConsDirection)
            SHADER_PARAMETER(float, ConsSigma)
            SHADER_PARAMETER(int32, ConsRadius)
        END_SHADER_PARAMETER_STRUCT()
    };

    class FConsistencyResolvePS final : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FConsistencyResolvePS);
        SHADER_USE_PARAMETER_STRUCT(FConsistencyResolvePS, FGlobalShader);

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return ShouldCompile(Parameters);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& Environment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
            Environment.SetDefine(TEXT("DLSS5_ONEMINUS_CONSISTENCY_RESOLVE"), 1);
        }

        BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, OriginalColor)
            SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSampler)
            SHADER_PARAMETER_SAMPLER(SamplerState, ModelSampler)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ConsLogRatio)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ConsViewTone)
            SHADER_PARAMETER_SAMPLER(SamplerState, ConsLinearSampler)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ConsReferenceTone)
            SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ConsReferenceMap)
            SHADER_PARAMETER(float, Strength)
            SHADER_PARAMETER(uint32, DebugView)
            SHADER_PARAMETER(float, SliceOffset)
            SHADER_PARAMETER(FIntPoint, OutputOffset)
            SHADER_PARAMETER(FIntPoint, OutputSize)
            SHADER_PARAMETER(uint32, ConsMode)
            SHADER_PARAMETER(uint32, ConsHasReference)
            SHADER_PARAMETER(uint32, ConsDisplayDomain)
            SHADER_PARAMETER(uint32, ConsDebug)
            SHADER_PARAMETER(float, ConsWhitePoint)
            RENDER_TARGET_BINDING_SLOTS()
        END_SHADER_PARAMETER_STRUCT()
    };
}

IMPLEMENT_GLOBAL_SHADER(FDisplayEncodeCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "DisplayEncodeMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FHDREncodeCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "HDREncodeMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMotionGuideCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "MotionGuideMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPostToneResolvePS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "PostToneResolveMain", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FHDRResolvePS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "HDRResolveMain", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FConsistencyLogRatioCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "ConsistencyLogRatioMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConsistencyDownsampleCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "ConsistencyDownsampleMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConsistencyBlurCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "ConsistencyBlurMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FConsistencyResolvePS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "ConsistencyResolveMain", SF_Pixel);

FRDGTextureRef DLSS5OneMinus::AddDisplayEncodePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTextureRef InputColor,
    const FIntRect& InputRect)
{
    const FIntPoint OutputSize = InputRect.Size();
    const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
        OutputSize,
        PF_FloatRGBA,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV);
    FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("DLSS5OneMinus.ModelInput"));

    FDisplayEncodeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDisplayEncodeCS::FParameters>();
    Parameters->InputColor = InputColor;
    Parameters->OutputColor = GraphBuilder.CreateUAV(Output);
    Parameters->InputOffset = InputRect.Min;
    Parameters->OutputSize = OutputSize;
    Parameters->DisplayInputIsLinear = View.Family && GetTonemapperOutputDeviceParameters(*View.Family).OutputDevice
        == static_cast<uint32>(EDisplayOutputFormat::HDR_LinearWithToneCurve) ? 1u : 0u;

    TShaderMapRef<FDisplayEncodeCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus Prepare Display Input %dx%d", OutputSize.X, OutputSize.Y),
        Shader,
        Parameters,
        FComputeShaderUtils::GetGroupCount(OutputSize, TileSize));
    return Output;
}

FRDGTextureRef DLSS5OneMinus::AddHDREncodePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTextureRef InputColor,
    const FIntRect& InputRect,
    float WhitePoint)
{
    const FIntPoint OutputSize = InputRect.Size();
    const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
        OutputSize,
        PF_FloatRGBA,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV);
    FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("DLSS5OneMinus.HDRModelInput"));

    FHDREncodeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHDREncodeCS::FParameters>();
    Parameters->InputColor = InputColor;
    Parameters->OutputColor = GraphBuilder.CreateUAV(Output);
    Parameters->InputOffset = InputRect.Min;
    Parameters->OutputSize = OutputSize;
    Parameters->WhitePoint = FMath::Max(WhitePoint, 0.001f);

    TShaderMapRef<FHDREncodeCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus Prepare HDR Input %dx%d", OutputSize.X, OutputSize.Y),
        Shader,
        Parameters,
        FComputeShaderUtils::GetGroupCount(OutputSize, TileSize));
    return Output;
}

FRDGTextureRef DLSS5OneMinus::AddMotionGuidePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTextureRef SceneDepth,
    FRDGTextureRef SceneVelocity,
    const FIntRect& InputRect)
{
    const FIntPoint OutputSize = InputRect.Size();
    const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
        OutputSize,
        PF_G16R16F,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV);
    FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("DLSS5OneMinus.MotionGuide"));

    FMotionGuideCS::FParameters* Parameters = GraphBuilder.AllocParameters<FMotionGuideCS::FParameters>();
    Parameters->SceneDepth = SceneDepth;
    Parameters->SceneVelocity = SceneVelocity;
    Parameters->OutputMotion = GraphBuilder.CreateUAV(Output);
    Parameters->View = View.ViewUniformBuffer;
    Parameters->InputOffset = InputRect.Min;
    Parameters->OutputSize = OutputSize;
    Parameters->VelocityIsValid = SceneVelocity->Desc.Extent == SceneDepth->Desc.Extent ? 1u : 0u;

    TShaderMapRef<FMotionGuideCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus Motion Guide %dx%d", OutputSize.X, OutputSize.Y),
        Shader,
        Parameters,
        FComputeShaderUtils::GetGroupCount(OutputSize, TileSize));
    return Output;
}

void DLSS5OneMinus::AddPostToneResolvePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FScreenPassTexture& OriginalColor,
    FRDGTextureRef ModelColor,
    float Strength,
    int32 DebugView,
    float SliceOffsetPercent,
    const FScreenPassRenderTarget& Output)
{
    const FScreenPassTextureViewport InputViewport(OriginalColor);
    const FScreenPassTextureViewport OutputViewport(Output);
    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
    TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
    TShaderMapRef<FPostToneResolvePS> PixelShader(ShaderMap);

    FPostToneResolvePS::FParameters* Parameters = GraphBuilder.AllocParameters<FPostToneResolvePS::FParameters>();
    Parameters->OriginalColor = OriginalColor.Texture;
    Parameters->OriginalSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ModelColor = ModelColor;
    Parameters->ModelSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->Strength = FMath::Clamp(Strength, 0.0f, 2.0f); // composition strength 0..2 (default 1); >1 extrapolates the NR edit
    Parameters->DebugView = static_cast<uint32>(FMath::Clamp(DebugView, 0, 2));
    Parameters->DisplayInputIsLinear = View.Family && GetTonemapperOutputDeviceParameters(*View.Family).OutputDevice
        == static_cast<uint32>(EDisplayOutputFormat::HDR_LinearWithToneCurve) ? 1u : 0u;
    Parameters->SliceOffset = NormalizeSliceOffset(SliceOffsetPercent);
    Parameters->OutputOffset = Output.ViewRect.Min;
    Parameters->OutputSize = Output.ViewRect.Size();
    Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

    AddDrawScreenPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus Post Tone Resolve %dx%d", Output.ViewRect.Width(), Output.ViewRect.Height()),
        View,
        OutputViewport,
        InputViewport,
        VertexShader,
        PixelShader,
        Parameters);
}

void DLSS5OneMinus::AddHDRResolvePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FScreenPassTexture& OriginalColor,
    FRDGTextureRef ModelColor,
    float Strength,
    float WhitePoint,
    int32 CompositionMode,
    int32 DebugView,
    float SliceOffsetPercent,
    const FScreenPassRenderTarget& Output)
{
    const FScreenPassTextureViewport InputViewport(OriginalColor);
    const FScreenPassTextureViewport OutputViewport(Output);
    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
    TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
    TShaderMapRef<FHDRResolvePS> PixelShader(ShaderMap);

    FHDRResolvePS::FParameters* Parameters = GraphBuilder.AllocParameters<FHDRResolvePS::FParameters>();
    Parameters->OriginalColor = OriginalColor.Texture;
    Parameters->OriginalSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ModelColor = ModelColor;
    Parameters->ModelSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->Strength = FMath::Clamp(Strength, 0.0f, 2.0f); // composition strength 0..2 (default 1); >1 extrapolates the NR edit
    Parameters->WhitePoint = FMath::Max(WhitePoint, 0.001f);
    Parameters->CompositionMode = static_cast<uint32>(FMath::Clamp(CompositionMode, 0, 1));
    Parameters->DebugView = static_cast<uint32>(FMath::Clamp(DebugView, 0, 2));
    Parameters->SliceOffset = NormalizeSliceOffset(SliceOffsetPercent);
    Parameters->OutputOffset = Output.ViewRect.Min;
    Parameters->OutputSize = Output.ViewRect.Size();
    Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

    AddDrawScreenPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus HDR Resolve %dx%d", Output.ViewRect.Width(), Output.ViewRect.Height()),
        View,
        OutputViewport,
        InputViewport,
        VertexShader,
        PixelShader,
        Parameters);
}

bool DLSS5OneMinus::AddConsistencyResolvePass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FScreenPassTexture& OriginalColor,
    FRDGTextureRef ModelColor,
    bool bDisplayDomain,
    float Strength,
    float WhitePoint,
    int32 DebugView,
    float SliceOffsetPercent,
    const FScreenPassRenderTarget& Output)
{
    const EConsistencyMode Mode = GetEffectiveConsistencyMode();
    if (Mode == EConsistencyMode::Off)
    {
        return false;
    }

    static int32 LastLoggedMode = INDEX_NONE;
    if (LastLoggedMode != static_cast<int32>(Mode))
    {
        LastLoggedMode = static_cast<int32>(Mode);
        UE_LOG(LogDLSS5OneMinusConsistency, Display,
            TEXT("Consistency resolve active: mode %d, %s domain, view %dx%d."),
            static_cast<int32>(Mode), bDisplayDomain ? TEXT("display") : TEXT("HDR"),
            OriginalColor.ViewRect.Width(), OriginalColor.ViewRect.Height());
    }

    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
    const FIntPoint ViewSize = OriginalColor.ViewRect.Size();
    const float SafeWhite = FMath::Max(WhitePoint, 0.001f);
    const uint32 DisplayDomain = bDisplayDomain ? 1u : 0u;

    // NR's edit as a per-channel log ratio in linear light.
    const FRDGTextureDesc RatioDesc = FRDGTextureDesc::Create2D(
        ViewSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
    FRDGTextureRef LogRatio = GraphBuilder.CreateTexture(RatioDesc, TEXT("DLSS5OneMinus.ConsistencyLogRatio"));
    {
        FConsistencyLogRatioCS::FParameters* Parameters = GraphBuilder.AllocParameters<FConsistencyLogRatioCS::FParameters>();
        Parameters->InputColor = OriginalColor.Texture;
        Parameters->ConsModelColor = ModelColor;
        Parameters->OutputColor = GraphBuilder.CreateUAV(LogRatio);
        Parameters->InputOffset = OriginalColor.ViewRect.Min;
        Parameters->OutputSize = ViewSize;
        Parameters->ConsDisplayDomain = DisplayDomain;
        Parameters->ConsWhitePoint = SafeWhite;
        TShaderMapRef<FConsistencyLogRatioCS> Shader(ShaderMap);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("DLSS5-OneMinus Consistency Log Ratio %dx%d", ViewSize.X, ViewSize.Y),
            Shader,
            Parameters,
            FComputeShaderUtils::GetGroupCount(ViewSize, TileSize));
    }

    // The view's own tone: the log ratio blurred on a quarter-resolution grid.
    const FIntPoint ToneSize(
        FMath::Max(FMath::DivideAndRoundUp(ViewSize.X, ConsistencyToneFactor), 1),
        FMath::Max(FMath::DivideAndRoundUp(ViewSize.Y, ConsistencyToneFactor), 1));
    const FRDGTextureDesc ToneDesc = FRDGTextureDesc::Create2D(
        ToneSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
    FRDGTextureRef ToneA = GraphBuilder.CreateTexture(ToneDesc, TEXT("DLSS5OneMinus.ConsistencyViewTone"));
    FRDGTextureRef ToneB = GraphBuilder.CreateTexture(ToneDesc, TEXT("DLSS5OneMinus.ConsistencyViewToneTemp"));
    {
        FConsistencyDownsampleCS::FParameters* Parameters = GraphBuilder.AllocParameters<FConsistencyDownsampleCS::FParameters>();
        Parameters->InputColor = LogRatio;
        Parameters->OutputColor = GraphBuilder.CreateUAV(ToneA);
        Parameters->OutputSize = ToneSize;
        Parameters->ConsSourceSize = ViewSize;
        Parameters->ConsFactor = ConsistencyToneFactor;
        TShaderMapRef<FConsistencyDownsampleCS> Shader(ShaderMap);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("DLSS5-OneMinus Consistency Downsample %dx%d", ToneSize.X, ToneSize.Y),
            Shader,
            Parameters,
            FComputeShaderUtils::GetGroupCount(ToneSize, TileSize));
    }

    const float Sigma = FMath::Max(GetConsistencyToneBlur() * ViewSize.X / ConsistencyToneFactor, 0.5f);
    const int32 Radius = FMath::Clamp(FMath::CeilToInt(Sigma * 3.0f), 1, 64);
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        FConsistencyBlurCS::FParameters* Parameters = GraphBuilder.AllocParameters<FConsistencyBlurCS::FParameters>();
        Parameters->InputColor = Pass == 0 ? ToneA : ToneB;
        Parameters->OutputColor = GraphBuilder.CreateUAV(Pass == 0 ? ToneB : ToneA);
        Parameters->OutputSize = ToneSize;
        Parameters->ConsDirection = Pass == 0 ? FIntPoint(1, 0) : FIntPoint(0, 1);
        Parameters->ConsSigma = Sigma;
        Parameters->ConsRadius = Radius;
        TShaderMapRef<FConsistencyBlurCS> Shader(ShaderMap);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("DLSS5-OneMinus Consistency Blur %s sigma %.1f", Pass == 0 ? TEXT("H") : TEXT("V"), Sigma),
            Shader,
            Parameters,
            FComputeShaderUtils::GetGroupCount(ToneSize, TileSize));
    }

    // Reference tone and view-to-reference map, when the pipeline supplied them for this frame.
    const FConsistencyFrameReference Reference = GetConsistencyFrameReference();
    const bool bHasReference = Mode == EConsistencyMode::Consumer
        && Reference.ToneTexture.IsValid()
        && Reference.MapTexture.IsValid();
    if (Mode == EConsistencyMode::Consumer && !bHasReference && ShouldWarnMissingConsistencyReference())
    {
        UE_LOG(LogDLSS5OneMinusConsistency, Warning,
            TEXT("Consistency mode is Consumer but no reference tone/map is available; views keep their own NR tone."));
    }
    if (bHasReference && ShouldLogConsistencyReferenceUse())
    {
        UE_LOG(LogDLSS5OneMinusConsistency, Display,
            TEXT("Consistency resolve using reference tone for frame key %d (view %dx%d)."),
            Reference.FrameKey, ViewSize.X, ViewSize.Y);
    }
    FRDGTextureRef ReferenceTone = bHasReference
        ? RegisterExternalTexture(GraphBuilder, Reference.ToneTexture, TEXT("DLSS5OneMinus.ConsistencyReferenceTone"), ERDGTextureFlags::None)
        : GSystemTextures.GetBlackDummy(GraphBuilder);
    FRDGTextureRef ReferenceMap = bHasReference
        ? RegisterExternalTexture(GraphBuilder, Reference.MapTexture, TEXT("DLSS5OneMinus.ConsistencyReferenceMap"), ERDGTextureFlags::None)
        : GSystemTextures.GetBlackDummy(GraphBuilder);

    const FScreenPassTextureViewport InputViewport(OriginalColor);
    const FScreenPassTextureViewport OutputViewport(Output);
    TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
    TShaderMapRef<FConsistencyResolvePS> PixelShader(ShaderMap);
    FConsistencyResolvePS::FParameters* Parameters = GraphBuilder.AllocParameters<FConsistencyResolvePS::FParameters>();
    Parameters->OriginalColor = OriginalColor.Texture;
    Parameters->OriginalSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ModelSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ConsLogRatio = LogRatio;
    Parameters->ConsViewTone = ToneA;
    Parameters->ConsLinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->ConsReferenceTone = ReferenceTone;
    Parameters->ConsReferenceMap = ReferenceMap;
    Parameters->Strength = FMath::Clamp(Strength, 0.0f, 2.0f);
    Parameters->DebugView = static_cast<uint32>(FMath::Clamp(DebugView, 0, 2));
    Parameters->SliceOffset = NormalizeSliceOffset(SliceOffsetPercent);
    Parameters->OutputOffset = Output.ViewRect.Min;
    Parameters->OutputSize = Output.ViewRect.Size();
    Parameters->ConsMode = static_cast<uint32>(Mode);
    Parameters->ConsHasReference = bHasReference ? 1u : 0u;
    Parameters->ConsDisplayDomain = DisplayDomain;
    Parameters->ConsDebug = static_cast<uint32>(GetConsistencyDebug());
    Parameters->ConsWhitePoint = SafeWhite;
    Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

    AddDrawScreenPass(
        GraphBuilder,
        RDG_EVENT_NAME("DLSS5-OneMinus Consistency Resolve mode %d %dx%d", static_cast<int32>(Mode), Output.ViewRect.Width(), Output.ViewRect.Height()),
        View,
        OutputViewport,
        InputViewport,
        VertexShader,
        PixelShader,
        Parameters);
    return true;
}
