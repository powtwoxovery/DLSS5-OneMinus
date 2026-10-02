#include "DLSS5OneMinusColorPass.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

namespace
{
    constexpr int32 TileSize = 8;

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
}

IMPLEMENT_GLOBAL_SHADER(FDisplayEncodeCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "DisplayEncodeMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FHDREncodeCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "HDREncodeMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMotionGuideCS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "MotionGuideMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPostToneResolvePS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "PostToneResolveMain", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FHDRResolvePS, "/Plugin/DLSS5OneMinus/Private/DLSS5OneMinusNR.usf", "HDRResolveMain", SF_Pixel);

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
