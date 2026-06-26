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
			"CoreUObject", // FInstancedStruct moved into CoreUObject in UE 5.8; exposed in SerializationFunctionLibrary.h's public API
			"CKSharedTypes",
			"Json",
			"JsonUtilities",
			"HTTP",
			"WebSockets",
			"Sockets",
			"Networking",
			"OpenSSL"       // SerializationFunctionLibrary.h includes openssl/hmac.h in its header
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"EngineSettings"
		});
	}
}
