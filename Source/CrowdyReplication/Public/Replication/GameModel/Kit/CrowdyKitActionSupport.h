// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

// Shared helpers for the per-genre kit node libraries (Combat, Leaderboards, Worldsim). These live in one header
// rather than as file-local functions because CrowdyReplication builds with UE unity batching: two anonymous-namespace
// helpers of the same name in co-batched translation units would be a redefinition. Keeping them inline here gives one
// definition every kit action file shares.
namespace CrowdyKitActionSupport
{
	// Upper bound on the number of listed container rows a kit read node pulls per call. A list read fans out one
	// state pull per row, so an unbounded (forged or degenerate) row count would amplify one response into that many
	// requests. Any read past this cap is dropped with a warning rather than issued. A real per-app board or node set
	// is well under this.
	inline constexpr int32 MaxKitListRows = 4096;

	// Resolve the Game Model subsystem from any world context object. Null when there is no world or no subsystem
	// (for example outside a play world).
	inline UCrowdyGameModelSubsystem* ResolveModel(const UObject* Context)
	{
		const UWorld* World = Context ? Context->GetWorld() : nullptr;
		return World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	}

	// Saturating double-to-int32: a forged/out-of-range server number clamps to the int32 bounds instead of an
	// out-of-range float-to-int conversion (undefined, a garbage sentinel on MSVC).
	inline int32 ClampDoubleToInt32(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			return 0;
		}
		if (Value >= static_cast<double>(TNumericLimits<int32>::Max()))
		{
			return TNumericLimits<int32>::Max();
		}
		if (Value <= static_cast<double>(TNumericLimits<int32>::Min()))
		{
			return TNumericLimits<int32>::Min();
		}
		return static_cast<int32>(Value);
	}

	// Encode a string as a SetDataProperty ValueJson. A property write's ValueJson is the JSON-encoded value (a
	// string is quoted, a number is bare), so a string property must pass through here rather than the raw text. The
	// backslash and quote are escaped so the value cannot break out of the string literal, and control characters
	// below 0x20 are escaped so a designer-supplied string carrying one still yields valid JSON.
	inline FString MakeStringValueJson(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len() + 2);
		Out.AppendChar(TEXT('"'));
		for (const TCHAR C : Value)
		{
			switch (C)
			{
			case TEXT('\\'): Out.Append(TEXT("\\\\")); break;
			case TEXT('"'): Out.Append(TEXT("\\\"")); break;
			case TEXT('\b'): Out.Append(TEXT("\\b")); break;
			case TEXT('\f'): Out.Append(TEXT("\\f")); break;
			case TEXT('\n'): Out.Append(TEXT("\\n")); break;
			case TEXT('\r'): Out.Append(TEXT("\\r")); break;
			case TEXT('\t'): Out.Append(TEXT("\\t")); break;
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
		Out.AppendChar(TEXT('"'));
		return Out;
	}
}
