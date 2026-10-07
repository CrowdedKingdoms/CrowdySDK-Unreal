using UnrealBuildTool;

public class CrowdyExecEditor : ModuleRules
{
	public CrowdyExecEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CrowdyExec"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"ApplicationCore",
			"BlueprintGraph",
			"ContentBrowser",
			"Projects",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"AssetRegistry",
			"CrowdyCppBridge",
			"CrowdyNet",
			"CrowdyReplication",
			"CrowdyStudio",
			"DesktopPlatform",
			"HTTP",
			"InputCore",
			"Json",
			"MainFrame",
			"StructUtilsEditor",
			"KismetWidgets"
		});
	}
}
