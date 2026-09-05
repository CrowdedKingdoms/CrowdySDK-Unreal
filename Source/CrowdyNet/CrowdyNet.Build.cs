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
			"HTTPServer",
			"Sockets",
			"Networking",
			"OpenSSL",
			// The vendored API client. Public because the client host and the admin host now take
			// FCrowdyCppClientConfig in their own public headers: how a client is addressed stopped being one
			// string and became a pair (the app's datacenter, and the shared origin to fall back on), and a struct
			// is what makes an old two-string call fail to compile instead of quietly meaning something else.
			//
			// This still drags no crowdy:: type into consumers. CrowdyCppClient.h is Unreal types only; the
			// library's own headers stay behind the pimpl, which was the point of the original split.
			"CrowdyCppBridge"
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
