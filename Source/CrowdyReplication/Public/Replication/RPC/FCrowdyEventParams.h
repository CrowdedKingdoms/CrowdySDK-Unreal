#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

/**
 * One inbound call's parameters, already decoded, addressed by name.
 *
 * This is what a call looks like to a receiver that has no object to run it on. The ordinary receive path
 * ends in ProcessEvent, which needs an instance of the declaring class; an entity simulated as a row in a
 * table has none, so its parameters are handed over as values instead and whoever holds that entity's
 * storage decides what they mean for it.
 *
 * BY NAME, never by position. A parameter list is positional on the wire, so a reader keyed on an index
 * would silently start reading a different parameter the day one is inserted above it. A name that no
 * longer resolves reads as absent, which a caller can see, rather than as some other parameter's bytes.
 *
 * EVERY VALUE HERE CAME OFF THE NETWORK and was chosen by whoever sent the call. Clamp anything used as
 * an index, a count or a duration, and never let one decide an outcome that must hold against a modified
 * client.
 *
 * Valid for the duration of the call that handed it over and no longer: Frame is a stack frame that is
 * torn down on return. Never store it, and never keep a pointer that FindParam handed back.
 */
struct CROWDYREPLICATION_API FCrowdyEventParams
{
	// The function the sender named. Its reflected parameters are what Frame is laid out for.
	const UFunction* Function = nullptr;

	// The decoded parameter frame. Not a UObject: read a value out of it through FindParam or one of the
	// typed reads below, never by casting it.
	const void* Frame = nullptr;

	/**
	 * The named input parameter's property, with OutValue pointing at its value inside the frame, or null
	 * when this function declares no such input parameter. OutValue is left null on every miss, so a
	 * caller that ignores the return value still cannot read from a stale pointer.
	 *
	 * A true output parameter is refused rather than returned: an output never rides the wire, so what
	 * sits in its slot is a default this call did not send.
	 */
	const FProperty* FindParam(FName ParamName, const void*& OutValue) const;

	/**
	 * The same, narrowed to one reflected property type. Null when the parameter is absent OR is declared
	 * as something else, which is the point: a parameter that has been retyped since a handler was written
	 * reads as absent instead of being reinterpreted through the wrong property.
	 */
	template <typename PropertyType>
	const PropertyType* FindTypedParam(FName ParamName, const void*& OutValue) const
	{
		const FProperty* Property = FindParam(ParamName, OutValue);
		const PropertyType* Typed = CastField<const PropertyType>(Property);
		if (!Typed)
		{
			OutValue = nullptr;
		}
		return Typed;
	}

	// Each read leaves Out untouched and answers false when the parameter is absent or is declared as a
	// type this read cannot produce without losing information.
	bool GetBool(FName ParamName, bool& OutValue) const;

	// Strictly an int32 parameter. An int64 is deliberately NOT narrowed here: the value is untrusted, so
	// a silent truncation would let a sender choose which low bits land.
	bool GetInt32(FName ParamName, int32& OutValue) const;

	// An int64 parameter, or an int32 widened, which is lossless.
	bool GetInt64(FName ParamName, int64& OutValue) const;

	// A float parameter, or a double narrowed. Both are accepted because a Blueprint-declared event's
	// "float" pin reflects as a double while a C++ one reflects as a float, and a handler should not have
	// to know which language declared the event it is registered for. A non-finite value is passed
	// through as it arrived: clamping is the caller's decision, and silently substituting a number here
	// would hide that the wire carried one.
	bool GetFloat(FName ParamName, float& OutValue) const;

	// The same pair, kept at double width.
	bool GetDouble(FName ParamName, double& OutValue) const;

	bool GetName(FName ParamName, FName& OutValue) const;

	bool GetString(FName ParamName, FString& OutValue) const;
};
