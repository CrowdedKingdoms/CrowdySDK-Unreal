#include "Dev/CrowdyMassStateDeltaBuilder.h"

#include "Reflection/CrowdyMassByteProperty.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "Utils/UCrowdyClassRegistry.h"

namespace
{
	// A scratch property container shaped like an instance of the delta's class, so the codec can address
	// values by the layout's own property offsets. No instance is ever constructed: constructing one would
	// build components and subobjects, and an actor class constructed with no world is a trap. Mirrors
	// FCrowdyStateScratchContainer (Replication/Subsystems/CrowdyEventRouter.h), but this one lives for a
	// single Build call rather than being cached per class, so it is a plain local RAII type instead of the
	// router's cached TUniquePtr slot.
	struct FCrowdyMassDeltaScratch
	{
		uint8* Data = nullptr;
		TArray<const FProperty*> InitializedProps;

		explicit FCrowdyMassDeltaScratch(const UClass& Class)
		{
			const int32 Size = Class.GetPropertiesSize();
			if (Size <= 0)
			{
				return;
			}

			// Zeroed before anything is initialized, so a property the layout does not name is never read
			// as uninitialized memory.
			Data = static_cast<uint8*>(FMemory::Malloc(Size, FMath::Max(1, Class.GetMinAlignment())));
			FMemory::Memzero(Data, Size);
		}

		~FCrowdyMassDeltaScratch()
		{
			// Each initialized property is destroyed exactly once, through the list this container itself
			// snapshotted, never through a layout that could since have moved.
			for (const FProperty* Property : InitializedProps)
			{
				if (Property)
				{
					Property->DestroyValue_InContainer(Data);
				}
			}

			if (Data)
			{
				FMemory::Free(Data);
				Data = nullptr;
			}
		}

		FCrowdyMassDeltaScratch(const FCrowdyMassDeltaScratch&) = delete;
		FCrowdyMassDeltaScratch& operator=(const FCrowdyMassDeltaScratch&) = delete;

		// Initializes every property the layout names, matching FCrowdyEventRouter::ResolveStateScratch: a
		// CrowdyState layout is restricted to POD leaves and plain USTRUCTs, so nothing in it points at
		// something that has to be constructed to be readable, but a leaf may own heap (an FString, say)
		// and must still be initialized rather than left zeroed.
		void InitializeLayoutProperties(const FCrowdyRepLayout& Layout)
		{
			InitializedProps.Reserve(Layout.Properties.Num());
			for (const FCrowdyRepProperty& RepProperty : Layout.Properties)
			{
				if (!RepProperty.Property)
				{
					continue;
				}

				RepProperty.Property->InitializeValue_InContainer(Data);
				InitializedProps.Add(RepProperty.Property);
			}
		}
	};

	/**
	 * The single-byte CrowdyState property of that name, as the OUTER property rather than as the byte
	 * inside it.
	 *
	 * Refuses anything that is not a byte, because writing one is all this builder does. A field registered
	 * for another type is bound perfectly well on the receive side
	 * (UCrowdyMassEntitySubsystem::ResolveStateFragmentClass); driving one from here would need a writer for
	 * that type, and answering with a wider property to a byte writer would synthesize a delta no real
	 * sender could produce.
	 *
	 * A byte-wide enum member counts, because that is how a game declares a replicated state and it is
	 * what a real sender puts on the wire. Answering with the byte inside an enum property would be wrong
	 * in a way that builds and runs: that byte is owned by the enum property rather than by the container,
	 * so its own offset is not the member's, and writing through it in-container lands at the start of the
	 * container and corrupts whichever property happens to be declared first.
	 */
	const FProperty* ResolveByteFieldByName(const FCrowdyRepLayout& Layout, FName Name)
	{
		for (const FCrowdyRepProperty& RepProperty : Layout.Properties)
		{
			if (!RepProperty.Property || RepProperty.Property->GetFName() != Name)
			{
				continue;
			}

			return CrowdyMassIsSingleByteProperty(*RepProperty.Property) ? RepProperty.Property : nullptr;
		}

		return nullptr;
	}

	// Writes one byte onto the property's own address, taking both declaration forms. Mirrors what the
	// receiving side reads: it takes the value through the outer property's address in every case.
	void WriteByteField(const FProperty& Property, void* Container, const uint8 Value)
	{
		void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Container);
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
			return;
		}

		*static_cast<uint8*>(ValuePtr) = Value;
	}
}

bool FCrowdyMassStateDeltaBuilder::Build(const UClass& EntityClass, const FCrowdyRepLayout& Layout,
	const FGuid& EntityID, const FGuid& SenderID, TConstArrayView<FCrowdyMassStateDeltaByteField> Fields,
	FCrowdyStateDelta& OutDelta, FString& OutWhy)
{
	// The receiver decodes a delta against the class it resolved locally, so a builder that let the layout
	// and the class it claims to describe disagree would forge a delta no real sender could ever send.
	// This also bounds the scratch container below: that buffer is sized from EntityClass while every
	// write into it is at an offset the LAYOUT names, so two classes that disagree means writing past the
	// end of the allocation. This is a memory-safety check and not only a validity one.
	if (Layout.OwnerClass.Get() != &EntityClass)
	{
		OutWhy = FString::Printf(TEXT("the layout was built for '%s', not '%s'"),
			Layout.OwnerClass.IsValid() ? *Layout.OwnerClass->GetName() : TEXT("no class"),
			*EntityClass.GetName());
		return false;
	}

	if (!EntityID.IsValid())
	{
		OutWhy = TEXT("the entity id is invalid, and the router drops an invalid entity id before anything else");
		return false;
	}

	if (!SenderID.IsValid())
	{
		OutWhy = TEXT("the sender id is invalid");
		return false;
	}

	const FCrowdyClassID ClassID = UCrowdyClassRegistry::Get()->GetID(&EntityClass);
	if (ClassID == CROWDY_INVALID_CLASS_ID)
	{
		OutWhy = FString::Printf(TEXT("'%s' has no registered CrowdyClassID"), *EntityClass.GetName());
		return false;
	}

	// One keyframe carries every slot in the layout, and an owner-only property is not something a real
	// sender puts in the same delta as the rest: it goes only to the entity's own client, in a delta of
	// its own. Refusing here keeps the promise the rest of these guards make, that this builder cannot
	// produce a shape no sender produces. Without it a class that later gained an owner-only byte would
	// keep every count in a gate lining up while the bytes stopped resembling the wire.
	for (const FCrowdyRepProperty& RepProperty : Layout.Properties)
	{
		if (!RepProperty.bOwnerOnly)
		{
			continue;
		}

		OutWhy = FString::Printf(
			TEXT("'%s' carries the owner-only CrowdyState property '%s', which a real sender delivers in a delta of its own rather than in a keyframe with everything else"),
			*EntityClass.GetName(),
			RepProperty.Property ? *RepProperty.Property->GetName() : TEXT("(unnamed)"));
		return false;
	}

	TArray<const FProperty*, TInlineAllocator<8>> ResolvedFields;
	ResolvedFields.Reserve(Fields.Num());
	for (const FCrowdyMassStateDeltaByteField& Field : Fields)
	{
		const FProperty* ByteProperty = ResolveByteFieldByName(Layout, Field.PropertyName);
		if (!ByteProperty)
		{
			OutWhy = FString::Printf(
				TEXT("'%s' carries no CrowdyState property named '%s' that is a single byte"),
				*EntityClass.GetName(), *Field.PropertyName.ToString());
			return false;
		}

		ResolvedFields.Add(ByteProperty);
	}

	FCrowdyMassDeltaScratch Scratch(EntityClass);
	if (!Scratch.Data)
	{
		OutWhy = FString::Printf(TEXT("'%s' has no properties to shape a scratch container from"),
			*EntityClass.GetName());
		return false;
	}

	Scratch.InitializeLayoutProperties(Layout);

	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		WriteByteField(*ResolvedFields[Index], Scratch.Data, Fields[Index].Value);
	}

	OutDelta.EntityID = EntityID;
	OutDelta.SenderID = SenderID;
	OutDelta.ClassID = static_cast<int64>(ClassID);
	OutDelta.LayoutHash = Layout.LayoutHash;

	// Set for parity with what a real sender puts on the wire, not because anything downstream of this
	// reads it: the codec does not set the bit itself, the replicator sets it by hand when it decides a
	// send is a keyframe, and the receive path for an entity with no participant never looks at Flags at
	// all. Assigned rather than OR-ed, so a caller reusing one delta cannot accumulate bits from a
	// previous build.
	OutDelta.Flags = CrowdyStateDeltaFlags::Keyframe;

	// A keyframe carries every slot whatever the dirty set says, so the dirty set is left empty and the
	// keyframe flag is the only thing deciding what goes out. Passing an all-set dirty mask as well would
	// work and would be worse: two independent reasons for the same bytes to be present means neither one
	// can be shown to be doing anything, and a flag nothing depends on is a flag that can stop being set.
	FCrowdyStateCodec::Encode(Layout, Scratch.Data, TBitArray<>(),
		/*bKeyframe=*/true, OutDelta.Blob);

	return true;
}
