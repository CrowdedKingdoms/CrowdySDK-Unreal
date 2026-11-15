using System.IO;
using UnrealBuildTool;

public class CrowdyModScriptRuntime : ModuleRules
{
	public CrowdyModScriptRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Projects"
		});

		string Wasmtime = Path.Combine(ModuleDirectory, "ThirdParty", "Wasmtime");
		PrivateIncludePaths.Add(Path.Combine(Wasmtime, "include"));

		if (Target.Platform != UnrealTargetPlatform.Win64 || Target.Architecture != UnrealArch.X64)
		{
			PublicDefinitions.Add("WITH_CROWDY_MOD_SCRIPT_RUNTIME=0");
			return;
		}

		PrivateDependencyModuleNames.Add("OpenSSL");
		PublicAdditionalLibraries.Add(Path.Combine(Wasmtime, "lib", "Win64", "wasmtime.lib"));
		PublicSystemLibraries.AddRange(new string[]
		{
			"advapi32.lib",
			"cfgmgr32.lib",
			"kernel32.lib",
			"ntdll.lib",
			"userenv.lib",
			"ws2_32.lib",
			"dbghelp.lib",
			"user32.lib",
			"bcrypt.lib",
			"legacy_stdio_definitions.lib"
		});
		PublicDefinitions.Add("LIBWASM_STATIC=1");
		PublicDefinitions.Add("WITH_CROWDY_MOD_SCRIPT_RUNTIME=1");
	}
}
