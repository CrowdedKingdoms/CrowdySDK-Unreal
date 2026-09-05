// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectMagnitudeJson.h"

namespace
{
	// Wraps a raw string as a JSON string literal, escaping the characters JSON requires.
	FString QuoteJson(const FString& In)
	{
		FString Out = TEXT("\"");
		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
				break;
			}
		}
		Out += TEXT("\"");
		return Out;
	}

	// Strips the surrounding quotes off a JSON string literal and decodes the basic escapes. A value that is not
	// a quoted string is returned verbatim, so partially-typed input still shows something sensible.
	FString UnquoteJson(const FString& In)
	{
		const FString S = In.TrimStartAndEnd();
		if (S.Len() < 2 || !S.StartsWith(TEXT("\"")) || !S.EndsWith(TEXT("\"")))
		{
			return S;
		}
		const FString Inner = S.Mid(1, S.Len() - 2);
		FString Out;
		Out.Reserve(Inner.Len());
		for (int32 Index = 0; Index < Inner.Len(); ++Index)
		{
			const TCHAR C = Inner[Index];
			if (C == TEXT('\\') && Index + 1 < Inner.Len())
			{
				const TCHAR Next = Inner[++Index];
				switch (Next)
				{
				case TEXT('n'):  Out.AppendChar(TEXT('\n')); break;
				case TEXT('r'):  Out.AppendChar(TEXT('\r')); break;
				case TEXT('t'):  Out.AppendChar(TEXT('\t')); break;
				case TEXT('"'):  Out.AppendChar(TEXT('"')); break;
				case TEXT('\\'): Out.AppendChar(TEXT('\\')); break;
				default:         Out.AppendChar(Next); break;
				}
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}
}

namespace CrowdyEffectMagnitudeJson
{
	FString ToCanonicalJson(ECrowdyEffectValueType Type, const FString& RawInput)
	{
		const FString Trimmed = RawInput.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return FString();
		}
		switch (Type)
		{
		case ECrowdyEffectValueType::Int:
			return FString::FromInt(FCString::Atoi(*Trimmed));
		case ECrowdyEffectValueType::Float:
			return FString::SanitizeFloat(FCString::Atod(*Trimmed));
		case ECrowdyEffectValueType::Bool:
			return Trimmed.ToBool() ? FString(TEXT("true")) : FString(TEXT("false"));
		case ECrowdyEffectValueType::String:
		case ECrowdyEffectValueType::ContainerRef:
		default:
			// Quote the raw input, not the trimmed copy: a string default can legitimately carry leading or
			// trailing whitespace (a "Sir " name prefix), and only the empty-default check above should trim.
			return QuoteJson(RawInput);
		}
	}

	FString FromCanonicalJson(ECrowdyEffectValueType Type, const FString& CanonicalJson)
	{
		const FString Trimmed = CanonicalJson.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return FString();
		}
		switch (Type)
		{
		case ECrowdyEffectValueType::String:
		case ECrowdyEffectValueType::ContainerRef:
			return UnquoteJson(Trimmed);
		default:
			return Trimmed;
		}
	}
}
