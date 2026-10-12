using UnrealBuildTool;

public class CrowdyMassNodes : ModuleRules
{
    public CrowdyMassNodes(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                // The entity classes this node's dropdown is built from are named by the Mass rendering
                // backend config, and the events it offers are Crowdy events declared on those classes.
                "CrowdyMass",
                "CrowdyReplication"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                // The seams the SDK's crowd authoring surfaces are contributed through, used only by this
                // module's own startup. Uncooked-only, like this module, which is the whole reason the
                // contributions are installed from here rather than from an editor module.
                "CrowdyNodes",
                "UnrealEd",
                "BlueprintGraph",
                "KismetCompiler",
                "GraphEditor",
                "Slate",
                "SlateCore",
                // An entity class bound in a map profile is usually a Blueprint nothing has loaded yet.
                // The asset registry names the profile assets that hold those bindings without loading
                // every Blueprint in the project to find them.
                "AssetRegistry",
                "DeveloperSettings"
            }
        );
    }
}
