using UnrealBuildTool;

public class CrowdyStudio : ModuleRules
{
	public CrowdyStudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"ToolWidgets",
			"UnrealEd",
			"ToolMenus",
			"Projects",
			"DeveloperSettings",
			"HTTP",
			"Json",
			"JsonUtilities",
			"WebBrowser",
			"CrowdyNet",
			"CrowdyReplication",
			"CrowdyServices",
			"CKSharedTypes"
		});
	}
}
