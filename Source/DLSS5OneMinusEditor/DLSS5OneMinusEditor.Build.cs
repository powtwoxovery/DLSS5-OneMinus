using UnrealBuildTool;

public class DLSS5OneMinusEditor : ModuleRules
{
    public DLSS5OneMinusEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AssetRegistry",
            "AssetTools",
            "DeveloperSettings",
            "InputCore",
            "RenderCore",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "LevelEditor",
            "ToolMenus",
            "Projects",
            "PropertyEditor",
            "DLSS5OneMinus"
        });
    }
}
