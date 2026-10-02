using UnrealBuildTool;
using System.IO;

public class DLSS5OneMinus : ModuleRules
{
    public DLSS5OneMinus(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
            "MovieRenderPipelineCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "RenderCore",
            "RHI",
            "Renderer",
            "DLSS"
        });

        PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));
        PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "D3D12RHI",
                "NGX"
            });

            AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
            PublicSystemLibraries.Add("bcrypt.lib");
            PublicDefinitions.Add("DLSS5ONEMINUS_WITH_D3D12=1");
        }
        else
        {
            PublicDefinitions.Add("DLSS5ONEMINUS_WITH_D3D12=0");
        }
    }
}
