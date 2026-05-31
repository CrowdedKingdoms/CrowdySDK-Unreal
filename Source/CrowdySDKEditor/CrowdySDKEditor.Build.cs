using UnrealBuildTool;

public class CrowdySDKEditor : ModuleRules
{
    public CrowdySDKEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "CrowdySDK",
                "ContentBrowser"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "Slate",
                "SlateCore",
                "PropertyEditor",
                "BlueprintGraph",
                "KismetWidgets",
                "Kismet",
                "EditorStyle",
                "StructUtils",
                "KismetCompiler",
                "GraphEditor",
                "InputCore",
                "AssetTools",
                "ToolMenus"
            }
        );
    }
}
