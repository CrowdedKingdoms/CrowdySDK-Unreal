#pragma once

#include "CoreMinimal.h"

enum class ECrowdyCppStudioTarget : uint8
{
	Server,
	Client
};

enum class ECrowdyCppStudioDiagnosticSeverity : uint8
{
	Error,
	Warning,
	Info,
	Hint
};

enum class ECrowdyCppStudioDiagnosticSource : uint8
{
	Rustc,
	LocalAdvisory,
	Runtime
};

/** One problem marker in a Studio project file. Lines and columns count from 1; an end of 0 means none was given. */
struct FCrowdyCppStudioDiagnostic
{
	ECrowdyCppStudioTarget Target = ECrowdyCppStudioTarget::Server;
	FString Path;
	int32 Line = 1;
	int32 Column = 1;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	ECrowdyCppStudioDiagnosticSeverity Severity = ECrowdyCppStudioDiagnosticSeverity::Info;
	FString Message;
	FString Code;
	ECrowdyCppStudioDiagnosticSource Source = ECrowdyCppStudioDiagnosticSource::LocalAdvisory;
};

namespace CrowdyCppStudio
{
	/**
	 * Read rustc output, in its human form or as one JSON message per line, into diagnostics. A path under server/ or
	 * client/ names its own target; any other path gets DefaultTarget. The output is untrusted: at most 1 MiB is read,
	 * at most 256 diagnostics come back, and a line that does not parse is skipped.
	 */
	CROWDYCPPBRIDGE_API TArray<FCrowdyCppStudioDiagnostic> ParseRustcDiagnostics(const FString& Output,
		ECrowdyCppStudioTarget DefaultTarget);

	/** CrowdyCPP's own copy of the platform's CLIENT half starter, Cargo.toml then src/lib.rs, its package named ModuleName, as (path, content). */
	CROWDYCPPBRIDGE_API TArray<TPair<FString, FString>> ClientStarterFiles(const FString& ModuleName);

	/** The Studio origin of the tier ApiUrl belongs to, by the platform's host-name convention; empty when ApiUrl follows none. */
	CROWDYCPPBRIDGE_API FString OfflineStudioOrigin(const FString& ApiUrl);

	/** The wallet page of the Studio at StudioOrigin. */
	CROWDYCPPBRIDGE_API FString StudioWalletUrl(const FString& StudioOrigin);
}
