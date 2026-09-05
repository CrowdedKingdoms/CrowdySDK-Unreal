// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * The one canonical text form of a number the Game Model layer writes anywhere the text is stored, compared, or
 * shipped to another machine.
 *
 * The rule is that a value has exactly one spelling: an integral value prints as an integer, so 10 and 10.0 are the
 * same text, and a fractional value uses one fixed float print. Formatting a double as it happens to arrive is fine
 * while the text is only ever compared against itself in the same process; the moment it is persisted or compared
 * across machines, two spellings of one value read as a difference that is not there.
 */
namespace CrowdyCanonicalNumber
{
	inline FString ToText(double Number)
	{
		// The guard keeps the cast inside int64 range: a value at or beyond ~9.2e18, or a NaN, has no integer
		// spelling, so it takes the float path instead of an undefined cast.
		if (FMath::Abs(Number) < 9.2e18 && Number == FMath::RoundToDouble(Number))
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)));
		}
		return FString::SanitizeFloat(Number);
	}
}
