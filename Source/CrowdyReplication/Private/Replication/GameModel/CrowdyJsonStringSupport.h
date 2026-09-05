#pragma once

#include "CoreMinimal.h"

// Escapes an FString into a valid JSON string body (without the surrounding quotes). A Server Owned string value
// can contain a double-quote or backslash (a display name, a path); hand-wrapping it in quotes without escaping
// would produce malformed JSON that the reading path cannot parse back. Covers the standard JSON escapes; other
// control chars go to \uXXXX. Shared so the subsystem, the value codec, and the schema sync escape identically and
// an encoded string round-trips through the server.
inline FString EscapeJsonStringBody(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 8);
	for (const TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('\"'): Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\b'): Out += TEXT("\\b"); break;
		case TEXT('\f'): Out += TEXT("\\f"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default:
			if (C < 0x20)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(C));
			}
			else
			{
				Out.AppendChar(C);
			}
			break;
		}
	}
	return Out;
}
