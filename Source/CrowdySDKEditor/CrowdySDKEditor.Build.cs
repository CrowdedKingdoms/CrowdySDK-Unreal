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
                "ContentBrowser",
                // Public rather than private: CrowdyCustomEventCustomization.h is a public header and names
                // FCrowdyActionParameterSpec, so a consumer of this module needs CrowdyNodes' include path
                // and import library, and a private dependency propagates neither. This direction only:
                // CrowdyNodes is UncookedOnly and must not depend back on an editor module.
                "CrowdyNodes"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CrowdyNet",
                "CrowdyReplication",
                // Reached only transitively through CrowdySDK otherwise, which propagates the include
                // paths but not the import library.
                "CrowdyServices",
                "UnrealEd",
                "ApplicationCore",
                "Slate",
                "SlateCore",
                "ToolWidgets",
                "EditorWidgets",
                "PropertyEditor",
                "BlueprintGraph",
                "KismetWidgets",
                "Kismet",
                "EditorStyle",
                "KismetCompiler",
                "GraphEditor",
                "InputCore",
                "WorkspaceMenuStructure",
                "AssetTools",
                "AssetRegistry",
                "ToolMenus",
                "Projects",
                "CrowdyStudio",
                "ClassViewer",
                "DeveloperSettings",
                "Json"
            }
        );
    }
}
