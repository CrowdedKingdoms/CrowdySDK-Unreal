#pragma once

#include "CoreMinimal.h"

// Returns ".<name>" for -crowdysession=<name>, empty without the switch, and for a refused name a suffix no valid
// name produces, so a typo never restores or overwrites the default session.
inline FString CrowdySessionSuffixFromCommandLine(const TCHAR* CommandLine, bool* bOutRefused = nullptr)
{
	if (bOutRefused)
	{
		*bOutRefused = false;
	}

	if (!CommandLine)
	{
		return FString();
	}

	static const TCHAR* const Switch = TEXT("-crowdysession=");
	const TCHAR* Found = FCString::Stristr(CommandLine, Switch);
	if (!Found)
	{
		return FString();
	}

	const TCHAR* Start = Found + FCString::Strlen(Switch);
	const bool bQuoted = *Start == TEXT('"');
	Start += bQuoted ? 1 : 0;
	const int32 Length = FCString::Strcspn(Start, TEXT(" \t\r\n\""));
	bool bValid = Length >= 1 && Length <= 32 && (!bQuoted || Start[Length] == TEXT('"'));
	for (int32 Index = 0; bValid && Index < Length; ++Index)
	{
		const TCHAR C = Start[Index];
		bValid = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '_';
	}

	if (!bValid)
	{
		if (bOutRefused)
		{
			*bOutRefused = true;
		}
		return TEXT(".~invalid");
	}

	// Lowercased because Windows file names ignore case, so B and b would otherwise share one file.
	return TEXT(".") + FString(Length, Start).ToLower();
}
