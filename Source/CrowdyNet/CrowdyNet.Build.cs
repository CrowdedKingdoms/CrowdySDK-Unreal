using UnrealBuildTool;

public class CrowdyNet : ModuleRules
{
	public CrowdyNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"CKSharedTypes",
			"Json",
			"JsonUtilities",
			"HTTP",
			"HTTPServer",
			"WebSockets",
			"Sockets",
			"Networking",
			"OpenSSL"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"EngineSettings"
		});
		
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Crypt32.lib");
		}
	}
}
