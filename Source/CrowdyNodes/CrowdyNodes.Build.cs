using UnrealBuildTool;

public class CrowdyNodes : ModuleRules
{
    public CrowdyNodes(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "CrowdyReplication"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "BlueprintGraph",
                "KismetCompiler",
                // FBlueprintCompilationManager, which the compile pass registers itself with.
                "Kismet",
                // UCrowdyUtilities, whose Crowdy authority wrappers the compile pass's tests pin the
                // Has Authority check against.
                "CrowdyServices",
                // ECrowdyEventRecipient and FCrowdyEventContext, whose reflection symbols the compile pass
                // needs. Reached only transitively through CrowdyReplication otherwise, which propagates the
                // include paths but not the import library.
                "CrowdyNet",
                "Slate",
                "SlateCore"
            }
        );
    }
}
