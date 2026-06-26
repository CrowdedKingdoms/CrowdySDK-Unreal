// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CrowdyServices : ModuleRules
{
	public CrowdyServices(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUsePrecompiled = false;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"CKSharedTypes",
				"CrowdyNet",
				"CrowdyReplication",
				"UMG",
				"GameplayTags",
				"DeveloperSettings",
				"AssetRegistry",
				"Json",
				"JsonUtilities",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
			}
		);
	}
}
