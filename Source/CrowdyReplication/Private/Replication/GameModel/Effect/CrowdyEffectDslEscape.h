#pragma once

#include "CoreMinimal.h"

// Escapes a string literal for re-embedding inside EffectScript double quotes, mirroring the escapes the
// tokenizer accepts so a printed string round-trips back to a re-lexable literal (a decoded newline goes back
// out as \n, not a raw linefeed).
inline FString DslStringEscape(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 2);
	for (const TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		default: Out.AppendChar(C); break;
		}
	}
	return Out;
}
