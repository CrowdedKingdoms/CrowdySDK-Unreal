// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CrowdySDK : ModuleRules
{
	public CrowdySDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		PrecompileForTargets = PrecompileTargetsType.Any;
		bUsePrecompiled = false;
		bUseUnity = false;
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CKSharedTypes",
				"CrowdyNet",
				"CrowdyReplication",
				"CrowdyVoice",
				"CrowdyServices",
				"OpenSSL",
				"Json",
				"JsonUtilities",
				"ProceduralMeshComponent",
				"UMG",
				"GameplayTags",
				"Projects",
				"DeveloperSettings"
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"Sockets",
				"Networking",
				// Private everywhere: a public header may name a bridge type only as a forward declaration,
				// and never a crowdy:: type at all.
				"CrowdyCppBridge",
				// ... add private dependencies that you statically link with here ...
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
