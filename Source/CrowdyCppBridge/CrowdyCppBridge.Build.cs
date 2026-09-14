using System.IO;
using UnrealBuildTool;

// Boundary module that vendors the CrowdyCPP C++ SDK (see ThirdParty/CrowdyCPP)
// and compiles it from source so its build settings match the engine. The
// module supplies Unreal-backed implementations of CrowdyCPP's pluggable
// logging, clock, crypto, HTTP and WebSocket services, and is the single place
// calls into the library funnel through so its exception-based error model is
// contained.
public class CrowdyCppBridge : ModuleRules
{
	public CrowdyCppBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		// Vendored third-party C/C++ compiles cleanly only outside shared PCHs
		// and unity groups (it includes winsock2 directly and must not inherit
		// engine forced includes ahead of its own headers).
		PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;
		bUseUnity = false;

		// CrowdyCPP's GraphQL and kit layers report errors by throwing.
		bEnableExceptions = true;
		CppStandard = CppStandardVersion.Cpp20;

		// The library dynamic_casts to recover typed GraphQL errors (client.cpp's
		// token-refresh error mapping and kit/core.hpp's error classification).
		// UBT defaults RTTI off, which MSVC reports only as a warning while
		// producing a cast that always fails, so this is load-bearing rather than
		// a build-cleanliness setting.
		bUseRTTI = true;

		// The vendored library is warning-clean under its own flags but not the
		// engine's stricter set; keep those warnings from failing the build
		// without weakening the engine-wide policy elsewhere.
		ShadowVariableWarningLevel = WarningLevel.Off;
		UndefinedIdentifierWarningLevel = WarningLevel.Off;
		bWarningsAsErrors = false;

		string ThirdParty = Path.Combine(ModuleDirectory, "ThirdParty", "CrowdyCPP");
		PublicIncludePaths.Add(Path.Combine(ThirdParty, "include"));
		PrivateIncludePaths.Add(Path.Combine(ThirdParty, "third_party", "yyjson"));

		// Mirrors the library's own CROWDY_WITH_OPENSSL build. Without it the
		// client's constructor falls back to the provider whose every primitive
		// fails, so signing would break silently rather than fail to build.
		PrivateDefinitions.Add("CROWDY_HAS_OPENSSL=1");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// The public client header exposes TSharedPtr<FJsonObject> in its result types, so Json is a public dep.
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"OpenSSL",
			// FHttpModule for the async transport, FWebSocketsModule for the GraphQL subscription transport, and
			// CKSharedTypes for the shared JSON nesting-depth guard.
			"HTTP",
			"WebSockets",
			"CKSharedTypes"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Native UDP socket (ws2_32) and OpenSSL's Windows crypto backend.
			PublicSystemLibraries.Add("ws2_32.lib");
			PublicSystemLibraries.Add("Crypt32.lib");
		}
	}
}
