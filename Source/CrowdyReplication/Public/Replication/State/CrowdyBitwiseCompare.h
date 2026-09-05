#pragma once

#include "CoreMinimal.h"

class FProperty;
class UScriptStruct;

/**
 * How much of a value a memcmp may compare in place of FProperty::Identical.
 *
 * Both send loops ask "has this changed since I last sent it" once per entity per tick, and the reflective
 * answer is a virtual FProperty::Identical per property. A byte compare answers the same question over the
 * run of bytes that belongs to properties which compare bitwise. These functions decide that once per type;
 * a caller that asks per comparison has put the reflection walk back on the hot path.
 *
 * What is provable, and is the whole argument: for every kind accepted here, Identical is a pure function of
 * the value's bytes, so equal bytes can never hide a change. Two disagreements remain and both converge.
 * A byte no compare reads (a struct's own operator==, a float's negative zero) can report a change nobody
 * made, which costs one extra send. And two identical NaN bit patterns compare equal here where Identical
 * calls them different, which stops an unchanged NaN being re-sent on every tick forever.
 */
namespace CrowdyBitwiseCompare
{
	/** Leading bytes of the property's element a memcmp may compare, or zero when it may not. */
	CROWDYREPLICATION_API int32 ComparableBytes(const FProperty* Property);

	/** Leading bytes of the struct a memcmp may compare, or zero when it may not. */
	CROWDYREPLICATION_API int32 ComparableBytes(const UScriptStruct* Struct);
}
