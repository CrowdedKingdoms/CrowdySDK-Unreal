#pragma once

#include "CoreMinimal.h"

/**
 * The C++ type a registered CrowdyState fragment field reads its value as.
 *
 * A registration hands its ingestor a bare pointer to one property's value, so the cast that ingestor
 * makes is the only thing deciding how those bytes are read. Naming the type here is what lets the
 * resolution refuse a property that cast would misread: a float read through an int pointer is not a
 * failed match to notice later, it is a wrong number nothing downstream can tell from a right one.
 *
 * An enum is read as the type it is declared over, so `enum class E : uint8` is a UInt8 field and
 * `enum class E : int32` is an Int32 one. A TEnumAsByte is a UInt8.
 *
 * A CrowdyState property of a type not listed here (a struct, an FName, an FString) cannot be bound to a
 * fragment field and is read on the actor representation instead.
 */
enum class ECrowdyMassStateFieldType : uint8
{
	UInt8,
	Int32,
	Int64,
	Float,
	Double,

	// A bool declared as its own member. A bitfield shares its byte with the flags beside it, so it cannot
	// be read through a bool pointer and is not one of these.
	Bool,
};
