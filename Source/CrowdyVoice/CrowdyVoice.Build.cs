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
            "CrowdyNet",
            "libOpus", "UELibSampleRate", "AudioCapture", "AudioCaptureCore", "AudioMixer", "Voice",
        });
    }
}
