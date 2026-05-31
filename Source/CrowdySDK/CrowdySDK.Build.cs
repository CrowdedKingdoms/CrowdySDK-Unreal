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
				"OpenSSL",
				"Json",
				"JsonUtilities",
				"HTTP",
				"WebSockets",
				"AudioCapture",
				"UELibSampleRate",
				"libOpus",
				"AudioCaptureCore",
				"AudioMixer",
				"Voice",
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
