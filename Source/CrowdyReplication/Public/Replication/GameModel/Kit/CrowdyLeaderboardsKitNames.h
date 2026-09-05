// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// The Leaderboards kit's server-side names, mirrored from the vendored CrowdyCPP leaderboards blueprint so the
// Blueprint nodes address exactly the type/function/property names a deployed Leaderboards kit created. Kept in one
// place so a node and the deployed schema cannot drift. The runtime nodes compose the existing Game Model subsystem
// seam rather than calling the vendored kit's blocking helpers, so only the names are shared, not the code path.
namespace CrowdyLeaderboardsKitNames
{
	// A per-player entry type name is <TypePrefix>LeaderboardEntry (empty prefix -> "LeaderboardEntry").
	inline constexpr const TCHAR* EntryTypeSuffix = TEXT("LeaderboardEntry");

	// The base submit function name. With a non-empty prefix the deployed function is snake_case(prefix) +
	// "_submit_score".
	inline constexpr const TCHAR* SubmitScoreFunctionBase = TEXT("submit_score");

	// Entry property keys. These are the same regardless of the type prefix (only the type and function names carry
	// the prefix), so a reader never needs the prefix.
	namespace Keys
	{
		inline constexpr const TCHAR* OwnerUserId = TEXT("owner_user_id");
		inline constexpr const TCHAR* BoardId = TEXT("board_id");
		inline constexpr const TCHAR* Score = TEXT("score");
		inline constexpr const TCHAR* Season = TEXT("season");
		inline constexpr const TCHAR* Rank = TEXT("rank");
	}

	// Entry property defaults, mirrored from leaderboards.hpp's property definitions.
	namespace Defaults
	{
		inline constexpr int32 Score = 0;
		inline constexpr int32 Season = 1;
		inline constexpr int32 Rank = 0;
	}

	// Mirror of crowdy::kit::toSnakeCase: a space or hyphen becomes a single underscore, an uppercase letter
	// preceded by a lowercase letter or digit gets an underscore before it, and everything is lowercased. Used to
	// derive the prefixed function names, so a prefix maps to the identical name the kit deployment produced.
	inline FString ToSnakeCase(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len() + 4);
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR C = Name[Index];
			if (C == TEXT(' ') || C == TEXT('-'))
			{
				if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
				{
					Out.AppendChar(TEXT('_'));
				}
				continue;
			}
			if (FChar::IsUpper(C))
			{
				if (Index > 0 && (FChar::IsLower(Name[Index - 1]) || FChar::IsDigit(Name[Index - 1])))
				{
					Out.AppendChar(TEXT('_'));
				}
				Out.AppendChar(FChar::ToLower(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	// The full LeaderboardEntry container type name for a PascalCase prefix: <Prefix>LeaderboardEntry.
	inline FString EntryTypeName(const FString& TypePrefix)
	{
		return TypePrefix + EntryTypeSuffix;
	}

	// The full submit function name for a prefix: "submit_score" for an empty prefix, else snake_case(prefix) +
	// "_submit_score".
	inline FString SubmitScoreFunctionName(const FString& TypePrefix)
	{
		if (TypePrefix.IsEmpty())
		{
			return SubmitScoreFunctionBase;
		}
		return ToSnakeCase(TypePrefix) + TEXT("_") + SubmitScoreFunctionBase;
	}
}
