using UnrealBuildTool;

public class CrowdyReplication : ModuleRules
{
	public CrowdyReplication(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CrowdyNet",
			"DeveloperSettings",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CrowdyCppBridge",
			// IPluginManager, used by the vendored-version drift test to locate VENDOR.txt.
			"Projects"
		});
	}
}
