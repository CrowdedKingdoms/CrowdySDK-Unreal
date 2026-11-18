// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * A nesting-depth pre-scan for a raw JSON string, run at every server-facing parse boundary BEFORE the string is
 * handed to FJsonSerializer::Deserialize. UE's JSON reader parses iteratively and admits unbounded nesting, but
 * the resulting FJsonValue tree is walked and destroyed RECURSIVELY on the C++ stack, so a forged deeply-nested
 * payload builds a DOM that overflows the stack when it is canonicalized or destroyed. Rejecting an over-deep
 * payload before it is parsed means such a DOM is never built. Availability-only (a rejected parse), never a
 * truth-plane concern; the parser still validates everything else.
 *
 * This lives in the lowest shared module so every parse boundary can reach it without forcing a dependency on the
 * networking module.
 */
namespace CrowdyJsonSafety
{
	// Deepest object/array nesting a server JSON payload may carry: far above any real response, far below a stack overflow.
	inline constexpr int32 MaxNestingDepth = 64;

	// True when the JSON string's object/array nesting never exceeds MaxDepth. A single pass over the raw text
	// tracking brace/bracket balance, ignoring nesting characters inside string literals (a stringified bracket
	// never becomes a nested DOM node, so it cannot be a teardown-overflow vector). This is a depth filter, not a
	// validator: a well-formed shallow payload always passes and is then fully validated by the parser. Takes an
	// FString or a UTF-8 view alike: every character it looks at is ASCII.
	template <typename CharRangeType>
	bool IsNestingWithinLimit(const CharRangeType& Json, int32 MaxDepth = MaxNestingDepth)
	{
		int32 Depth = 0;
		bool bInString = false;
		bool bEscaped = false;
		for (const auto Char : Json)
		{
			if (bInString)
			{
				if (bEscaped)
				{
					bEscaped = false;
				}
				else if (Char == '\\')
				{
					bEscaped = true;
				}
				else if (Char == '"')
				{
					bInString = false;
				}
				continue;
			}

			if (Char == '"')
			{
				bInString = true;
			}
			else if (Char == '{' || Char == '[')
			{
				if (++Depth > MaxDepth)
				{
					return false;
				}
			}
			else if (Char == '}' || Char == ']')
			{
				--Depth;
			}
		}
		return true;
	}
}
