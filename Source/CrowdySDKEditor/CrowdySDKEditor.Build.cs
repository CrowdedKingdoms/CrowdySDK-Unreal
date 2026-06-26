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
                "CrowdyNet",
                "CrowdyReplication",
                "UnrealEd",
                "Slate",
                "SlateCore",
                "EditorWidgets",
                "PropertyEditor",
                "BlueprintGraph",
                "KismetWidgets",
                "Kismet",
                "EditorStyle",
                "KismetCompiler",
                "GraphEditor",
                "InputCore",
                "AssetTools",
                "AssetRegistry",
                "ToolMenus",
                "Projects",
                "CrowdyStudio",
                "Json"
            }
        );
    }
}
