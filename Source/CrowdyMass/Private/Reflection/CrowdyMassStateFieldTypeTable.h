#pragma once

#include "Containers/ArrayView.h"
#include "Misc/Optional.h"
#include "Reflection/CrowdyMassStateFieldType.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

/**
 * The property a value is actually stored in.
 *
 * An FEnumProperty owns no storage of its own: the numeric it is declared over occupies the member, at the
 * member's own address. That is why an enum is read exactly as the type it is declared over, and why
 * classifying the declaration rather than the storage would refuse the ordinary way a game declares a
 * replicated state.
 */
inline const FProperty& CrowdyMassStateFieldLeafProperty(const FProperty& Property)
{
	const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property);
	const FProperty* Underlying = EnumProperty ? EnumProperty->GetUnderlyingProperty() : nullptr;
	return Underlying ? *Underlying : Property;
}

struct FCrowdyMassStateFieldTypeRow
{
	ECrowdyMassStateFieldType Type;

	// What an author calls this type in their own declaration, which is what every diagnostic prints. One
	// spelling per type, read from here, so a message can never name a type nothing accepts.
	const TCHAR* Name;

	bool (*Accepts)(const FProperty& Leaf);
};

/**
 * One row per accepted type, in enum order.
 *
 * The rows are mutually exclusive by construction: each casts to a distinct concrete property class, and an
 * FByteProperty is not an FIntProperty however alike the two read. That is what lets a property answer at
 * most one type, which the dropped-property diagnostic relies on to name the type to register for.
 */
inline TConstArrayView<FCrowdyMassStateFieldTypeRow> CrowdyMassStateFieldTypeRows()
{
	static const FCrowdyMassStateFieldTypeRow Rows[] =
	{
		{
			ECrowdyMassStateFieldType::UInt8, TEXT("uint8"),
			[](const FProperty& Leaf)
			{
				// The width check is on the resolved byte property rather than on the declaration, so an
				// enum declared over something wider is refused by the same test that refuses an int.
				const FByteProperty* ByteProperty = CastField<FByteProperty>(&Leaf);
				return ByteProperty != nullptr && ByteProperty->GetElementSize() == sizeof(uint8);
			}
		},
		{
			ECrowdyMassStateFieldType::Int32, TEXT("int32"),
			[](const FProperty& Leaf) { return CastField<FIntProperty>(&Leaf) != nullptr; }
		},
		{
			ECrowdyMassStateFieldType::Int64, TEXT("int64"),
			[](const FProperty& Leaf) { return CastField<FInt64Property>(&Leaf) != nullptr; }
		},
		{
			ECrowdyMassStateFieldType::Float, TEXT("float"),
			[](const FProperty& Leaf) { return CastField<FFloatProperty>(&Leaf) != nullptr; }
		},
		{
			ECrowdyMassStateFieldType::Double, TEXT("double"),
			[](const FProperty& Leaf) { return CastField<FDoubleProperty>(&Leaf) != nullptr; }
		},
		{
			ECrowdyMassStateFieldType::Bool, TEXT("bool"),
			[](const FProperty& Leaf)
			{
				// A bitfield is an FBoolProperty too, and it shares its byte with the flags declared beside
				// it, so reading it through a bool pointer would answer whichever of them happened to be set.
				const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Leaf);
				return BoolProperty != nullptr && BoolProperty->IsNativeBool();
			}
		},
	};

	return MakeArrayView(Rows);
}

// What an author writes for FieldType, for a diagnostic to print. Never null: every enumerator has a row.
inline const TCHAR* CrowdyMassStateFieldTypeName(const ECrowdyMassStateFieldType FieldType)
{
	for (const FCrowdyMassStateFieldTypeRow& Row : CrowdyMassStateFieldTypeRows())
	{
		if (Row.Type == FieldType)
		{
			return Row.Name;
		}
	}

	return TEXT("unknown");
}

// True when Property can be read as FieldType, following an enum to the type it is declared over.
inline bool CrowdyMassPropertyMatchesStateFieldType(const FProperty& Property, const ECrowdyMassStateFieldType FieldType)
{
	const FProperty& Leaf = CrowdyMassStateFieldLeafProperty(Property);
	for (const FCrowdyMassStateFieldTypeRow& Row : CrowdyMassStateFieldTypeRows())
	{
		if (Row.Type == FieldType)
		{
			return Row.Accepts(Leaf);
		}
	}

	return false;
}

// The one type a field could be registered for to read Property, or unset when no registration can read it
// at all. The rows are mutually exclusive, so there is never a second answer to choose between.
inline TOptional<ECrowdyMassStateFieldType> CrowdyMassFindStateFieldTypeFor(const FProperty& Property)
{
	const FProperty& Leaf = CrowdyMassStateFieldLeafProperty(Property);
	for (const FCrowdyMassStateFieldTypeRow& Row : CrowdyMassStateFieldTypeRows())
	{
		if (Row.Accepts(Leaf))
		{
			return Row.Type;
		}
	}

	return TOptional<ECrowdyMassStateFieldType>();
}

// Every type a field can be registered for, as one readable list. Built from the rows rather than written
// out, so it cannot go stale the next time a type is added.
inline FString CrowdyMassAcceptedStateFieldTypes()
{
	const TConstArrayView<FCrowdyMassStateFieldTypeRow> Rows = CrowdyMassStateFieldTypeRows();

	FString Accepted;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Index > 0)
		{
			Accepted += Index == Rows.Num() - 1 ? TEXT(" or ") : TEXT(", ");
		}
		Accepted += Rows[Index].Name;
	}

	return Accepted;
}

/**
 * How to name a property's declaration in a diagnostic.
 *
 * The C++ type on its own is misleading in the two cases a reader is least able to work out for themselves.
 * A bitfield reports the width it is packed into, so pairing it with the accepted types reads as "declared
 * as uint8, which is not one of uint8, ...". An enum reports its own name, which says nothing about the type
 * it is read as, so an enum refused for its width looks like enums are refused altogether.
 */
inline FString CrowdyMassDescribeStateFieldProperty(const FProperty& Property)
{
	const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property);
	if (BoolProperty && !BoolProperty->IsNativeBool())
	{
		return TEXT("a bitfield, which shares its byte with the flags declared beside it and so cannot be read on its own");
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
	{
		const FProperty* Underlying = EnumProperty->GetUnderlyingProperty();
		const UEnum* Enum = EnumProperty->GetEnum();
		return FString::Printf(TEXT("the enum %s, declared over %s"),
			Enum ? *Enum->GetName() : TEXT("it names"),
			Underlying ? *Underlying->GetCPPType() : TEXT("a type this build cannot name"));
	}

	return Property.GetCPPType();
}

/**
 * What to tell an author whose property no field can read at all.
 *
 * Each case gets the fix that actually works from where they are standing. A bitfield has a one-word fix
 * that the generic list would never name, and an enum is fixed on the enum's declaration rather than by
 * replacing it with a raw numeric, which is what "declare it as one of these" reads as.
 */
inline FString CrowdyMassStateFieldRemedyFor(const FProperty& Property)
{
	const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property);
	if (BoolProperty && !BoolProperty->IsNativeBool())
	{
		return TEXT("Declare it as a plain bool member rather than a bitfield, or read this value on the actor representation instead.");
	}

	if (CastField<FEnumProperty>(&Property))
	{
		return FString::Printf(TEXT("Declare the enum over one of %s, or read this value on the actor representation instead."),
			*CrowdyMassAcceptedStateFieldTypes());
	}

	return FString::Printf(TEXT("Declare it as one of %s, or read this value on the actor representation instead."),
		*CrowdyMassAcceptedStateFieldTypes());
}

// What to tell an author whose registered type does not match the property. Naming the type the property
// reads as is the fix wherever there is one; where the property reads as no type at all, no registration
// can reach it and the fix is on the declaration instead.
inline FString CrowdyMassStateFieldMismatchRemedyFor(const FProperty& Property)
{
	const TOptional<ECrowdyMassStateFieldType> BindableAs = CrowdyMassFindStateFieldTypeFor(Property);
	if (BindableAs.IsSet())
	{
		return FString::Printf(TEXT("Register the field for %s, which is what this property reads as."),
			CrowdyMassStateFieldTypeName(*BindableAs));
	}

	return CrowdyMassStateFieldRemedyFor(Property);
}
