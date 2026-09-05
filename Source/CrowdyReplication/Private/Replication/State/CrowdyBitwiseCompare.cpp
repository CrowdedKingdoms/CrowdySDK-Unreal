#include "Replication/State/CrowdyBitwiseCompare.h"

#include "UObject/UnrealType.h"

namespace CrowdyBitwiseCompare
{
	int32 ComparableBytes(const FProperty* Property)
	{
		if (!Property)
		{
			return 0;
		}

		// A static array's Identical compares element zero alone, so a compare over the whole element run
		// would answer a different question than the one being replaced.
		if (Property->ArrayDim != 1)
		{
			return 0;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return ComparableBytes(StructProperty->Struct);
		}

		// A bitfield bool compares one masked bit and shares its byte with whatever is packed beside it.
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return BoolProperty->IsNativeBool() ? Property->GetSize() : 0;
		}

		return Property->IsA<FNumericProperty>() || Property->IsA<FEnumProperty>() ? Property->GetSize() : 0;
	}

	int32 ComparableBytes(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return 0;
		}

		// The properties must tile a run starting at offset zero. Trailing padding is excluded rather than
		// refused, because no compare reads it and stopping short of it answers the same question; a gap
		// BETWEEN two properties cannot be stepped over that way, so it makes the struct unanswerable.
		// Summing sizes against the furthest end is what separates the two without assuming field order.
		int32 Sum = 0;
		int32 Extent = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			// A partially comparable member would leave its own padding inside this struct's run.
			if (ComparableBytes(*It) != It->GetSize())
			{
				return 0;
			}

			Sum += It->GetSize();
			Extent = FMath::Max(Extent, It->GetOffset_ForInternal() + It->GetSize());
		}

		return Sum > 0 && Sum == Extent ? Sum : 0;
	}
}
