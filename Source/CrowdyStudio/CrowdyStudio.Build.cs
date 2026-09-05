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
			"AssetRegistry",
			"PropertyEditor",
			"Json",
			"JsonUtilities",
			"WebBrowser",
			"CrowdyNet",
			"CrowdyReplication",
			"CrowdyServices",
			"CKSharedTypes",
			// Private everywhere: a public header may name a bridge type only as a forward declaration,
			// and never a crowdy:: type at all.
			"CrowdyCppBridge"
		});
	}
}
