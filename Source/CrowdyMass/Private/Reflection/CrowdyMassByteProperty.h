#pragma once

#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

/**
 * A reflected property read as a single byte, including a byte-wide enum member.
 *
 * An enum declared `enum class E : uint8` reflects as an FEnumProperty wrapping an FByteProperty, and one
 * declared TEnumAsByte reflects as a plain FByteProperty. Both are one byte at the same address, so
 * anything that reads or writes such a field has to accept either form. Accepting only the plain byte
 * refuses the ordinary way a game declares a state, which is the enum, and the refusal looks exactly like
 * the field never arriving.
 *
 * Null for anything that is not one byte, an enum whose underlying type is wider included: a wider enum
 * cannot be read through a byte pointer at all, so it is refused rather than truncated.
 *
 * One home for the two call sites this has, because a second copy in another anonymous namespace in this
 * module would collide with the first under a unity build.
 */
inline const FByteProperty* CrowdyMassAsByteProperty(const FProperty& Property)
{
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property))
	{
		return ByteProperty;
	}

	const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property);
	return EnumProperty ? CastField<FByteProperty>(EnumProperty->GetUnderlyingProperty()) : nullptr;
}

/**
 * True when Property is one byte wide and can therefore be read through a byte pointer.
 *
 * The element-size check is on the resolved byte property rather than on the declaration, so an enum whose
 * underlying type is wider than a byte is refused by the same test that refuses a float or an int.
 */
inline bool CrowdyMassIsSingleByteProperty(const FProperty& Property)
{
	const FByteProperty* ByteProperty = CrowdyMassAsByteProperty(Property);
	return ByteProperty != nullptr && ByteProperty->GetElementSize() == sizeof(uint8);
}

/**
 * The enum a byte-wide property is declared as, or null when it is a plain byte naming no enum.
 *
 * The two declarations reach it differently: `enum class E : uint8` reflects as an FEnumProperty that
 * names the enum directly, while TEnumAsByte reflects as an FByteProperty carrying the enum beside the
 * byte. Both answer the same UEnum, which is what lets a property be matched by the TYPE it was declared
 * as rather than by the name an author happened to give it.
 *
 * Null is a real answer and not a failure: a raw uint8 is a valid byte field that simply belongs to no
 * enum, so a caller matching on a type must skip it rather than treat it as a mismatch to report.
 */
inline const UEnum* CrowdyMassGetByteEnum(const FProperty& Property)
{
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
	{
		return EnumProperty->GetEnum();
	}

	const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property);
	return ByteProperty ? ByteProperty->Enum : nullptr;
}
