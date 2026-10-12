using UnrealBuildTool;

public class CrowdyMass : ModuleRules
{
	public CrowdyMass(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"CrowdyExec",
			"CrowdyReplication",
			"DeveloperSettings",
			"Engine",
			"AnimationCore",
			"MassCore",
			"MassEntity",
			"MassActors",
			"MassCrowd",
			"MassLOD",
			"MassMovement",
			"MassRepresentation",
			"MassSpawner"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CrowdyNet",
			"CrowdySDK",
			"CrowdyCppBridge",
			// FCrowdyMassClassDerivationReport::ResolveActorClass answers for a Blueprint class before
			// anything has spawned one, which is exactly when it is not loaded and no search of loaded
			// classes can find it. The registry knows the asset without it being loaded.
			"AssetRegistry"
		});
	}
}
