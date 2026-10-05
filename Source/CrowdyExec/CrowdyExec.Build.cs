using UnrealBuildTool;

public class CrowdyExec : ModuleRules
{
	public CrowdyExec(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CrowdyNet"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"GameplayTags",
			"CrowdyCppBridge",
			"CrowdyReplication",
			"CrowdySDK",
			"CrowdyServices"
		});
	}
}
