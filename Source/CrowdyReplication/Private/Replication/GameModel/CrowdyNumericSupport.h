#pragma once

#include "CoreMinimal.h"

// Saturating double-to-int32: a forged/out-of-range number clamps to the int32 bounds instead of an
// out-of-range float-to-int conversion (undefined, a garbage sentinel on MSVC). Mirrors the codec's SafeDoubleToInt64.
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
