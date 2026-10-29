using UnrealBuildTool;

public class CrowdyVoice : ModuleRules
{
    public CrowdyVoice(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine",
            "CKSharedTypes", "CrowdyNet",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CrowdyCppBridge", "AudioCaptureCore", "UELibSampleRate",
        });
    }
}
