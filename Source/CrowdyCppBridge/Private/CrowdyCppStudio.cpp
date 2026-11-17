#include "CrowdyCppStudio.h"

#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/studio/diagnostics.hpp"
THIRD_PARTY_INCLUDES_END

#include <exception>
#include <string>
#include <vector>

namespace
{
	ECrowdyCppStudioTarget ToBridgeTarget(crowdy::studio::CrowdyStudioTarget Target)
	{
		return Target == crowdy::studio::CrowdyStudioTarget::Client ? ECrowdyCppStudioTarget::Client : ECrowdyCppStudioTarget::Server;
	}

	ECrowdyCppStudioDiagnosticSeverity ToBridgeSeverity(crowdy::studio::CrowdyStudioDiagnosticSeverity Severity)
	{
		switch (Severity)
		{
		case crowdy::studio::CrowdyStudioDiagnosticSeverity::Error:   return ECrowdyCppStudioDiagnosticSeverity::Error;
		case crowdy::studio::CrowdyStudioDiagnosticSeverity::Warning: return ECrowdyCppStudioDiagnosticSeverity::Warning;
		case crowdy::studio::CrowdyStudioDiagnosticSeverity::Hint:    return ECrowdyCppStudioDiagnosticSeverity::Hint;
		default:                                                      return ECrowdyCppStudioDiagnosticSeverity::Info;
		}
	}

	ECrowdyCppStudioDiagnosticSource ToBridgeSource(crowdy::studio::CrowdyStudioDiagnosticSource Source)
	{
		switch (Source)
		{
		case crowdy::studio::CrowdyStudioDiagnosticSource::Rustc:   return ECrowdyCppStudioDiagnosticSource::Rustc;
		case crowdy::studio::CrowdyStudioDiagnosticSource::Runtime: return ECrowdyCppStudioDiagnosticSource::Runtime;
		default:                                                    return ECrowdyCppStudioDiagnosticSource::LocalAdvisory;
		}
	}

	FString Utf8Text(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Value.data()), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	FCrowdyCppStudioDiagnostic ToBridgeDiagnostic(const crowdy::studio::CrowdyStudioDiagnostic& Diagnostic)
	{
		FCrowdyCppStudioDiagnostic Out;
		Out.Target = ToBridgeTarget(Diagnostic.target);
		Out.Path = Utf8Text(Diagnostic.path);
		Out.Line = static_cast<int32>(Diagnostic.line);
		Out.Column = static_cast<int32>(Diagnostic.column);
		Out.EndLine = static_cast<int32>(Diagnostic.endLine.value_or(0));
		Out.EndColumn = static_cast<int32>(Diagnostic.endColumn.value_or(0));
		Out.Severity = ToBridgeSeverity(Diagnostic.severity);
		Out.Message = Utf8Text(Diagnostic.message);
		Out.Code = Diagnostic.code.has_value() ? Utf8Text(*Diagnostic.code) : FString();
		Out.Source = ToBridgeSource(Diagnostic.source);
		return Out;
	}
}

namespace CrowdyCppStudio
{
	TArray<FCrowdyCppStudioDiagnostic> ParseRustcDiagnostics(const FString& Output, ECrowdyCppStudioTarget DefaultTarget)
	{
		TArray<FCrowdyCppStudioDiagnostic> Diagnostics;

		// Cut before converting: CrowdyCPP reads no more than this many bytes, and every character is at least one.
		const FString Bounded = Output.Left(static_cast<int32>(crowdy::studio::kCrowdyStudioDiagnosticMaxInputBytes));
		const FTCHARToUTF8 Utf8(*Bounded, Bounded.Len());
		const crowdy::studio::CrowdyStudioTarget Target = DefaultTarget == ECrowdyCppStudioTarget::Client
			? crowdy::studio::CrowdyStudioTarget::Client : crowdy::studio::CrowdyStudioTarget::Server;

		std::vector<crowdy::studio::CrowdyStudioDiagnostic> Parsed;
		try
		{
			Parsed = crowdy::studio::parseRustcDiagnostics(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), Target);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Reading compiler diagnostics failed: %hs"), Ex.what());
			return Diagnostics;
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Reading compiler diagnostics threw a non-standard exception"));
			return Diagnostics;
		}

		Diagnostics.Reserve(static_cast<int32>(Parsed.size()));
		for (const crowdy::studio::CrowdyStudioDiagnostic& Diagnostic : Parsed)
		{
			Diagnostics.Add(ToBridgeDiagnostic(Diagnostic));
		}
		return Diagnostics;
	}
}
