#pragma once

// Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside
// a preprocessor block, so these fixtures are declared unconditionally and only the tests that use them
// are guarded. The sibling fixture header for the event seam is unconditional for the same reason.
//
// Two doubles for one interface, in two files, because they stand in for opposite things: this one holds
// entities and answers about them, the event seam's holds nothing and exists to prove it was not asked.

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "Templates/Function.h"
#include "CrowdyStateFragmentTestTypes.generated.h"

struct FCrowdyAttributeChange;

// A plain (non-net-serialized) USTRUCT whose only member is heap-carrying, so a CrowdyState property of
// this type exercises the scratch container's InitializeValue/DestroyValue pairing for a struct, not only
// for a leaf.
USTRUCT()
struct FCrowdyStateFragmentHeapStruct
{
	GENERATED_BODY()

	UPROPERTY()
	FString Payload;
};

// Fixture carrying every heap-owning CrowdyState leaf kind (FString, FName, a USTRUCT holding an FString),
// so the scratch container's teardown is exercised for each of them, not only the numeric leaves the
// other fixtures already cover.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateFragmentHeapTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	FString RepString;

	UPROPERTY(meta = (CrowdyState))
	FName RepName = NAME_None;

	UPROPERTY(meta = (CrowdyState))
	FCrowdyStateFragmentHeapStruct RepStruct;
};

// A participant that is present but is not a home for state: it declares no CrowdyState property, so the
// registry builds no rep layout for its class. Concrete on purpose, because UObject itself is abstract and
// cannot be constructed to stand in for one.
UCLASS()
class UCrowdyStateLayoutlessParticipant : public UObject
{
	GENERATED_BODY()
};

/**
 * A subscriber double for ICrowdyEntitySubscriber's state plane.
 *
 * The set of ids it HOLDS and the class it names for each are kept as two separate facts, exactly as the
 * interface requires, so a test can produce the case that only exists when they are separate: an id held
 * with no class. SetOwnedClass records both at once (the ordinary entity), SetHeldWithNoClass records only
 * the first (a position-only entity). Nothing is ever invented for an id a test did not register.
 *
 * ApplyDecodedState records every call and, when OnApply is bound, hands the call straight to the test so
 * it can read DecodedContainer while it is still valid: the buffer is shared and reused by the very next
 * call, so nothing about it may be read after ApplyDecodedState returns.
 */
UCLASS()
class UCrowdyStateFragmentSpySubscriber : public UObject, public ICrowdyEntitySubscriber
{
	GENERATED_BODY()

public:

	virtual bool IsEntityKnown(const FGuid& EntityUUID) const override
	{
		++IsEntityKnownCallCount;
		return HeldEntities.Contains(EntityUUID);
	}

	virtual const UClass* GetEntityClass(const FGuid& EntityUUID) const override
	{
		++GetEntityClassCallCount;
		const TWeakObjectPtr<const UClass>* Found = ClassByEntity.Find(EntityUUID);
		return Found ? Found->Get() : nullptr;
	}

	virtual UObject* ResolveReceiver(const FGuid& EntityUUID, const UFunction* Function) override
	{
		++ResolveReceiverCallCount;
		return nullptr;
	}

	virtual void ApplyDecodedState(const FGuid& EntityUUID, const FCrowdyRepLayout& Layout,
		const void* DecodedContainer, TConstArrayView<int32> ChangedIndices) override
	{
		++ApplyDecodedStateCallCount;
		LastEntityUUID = EntityUUID;
		LastChangedIndices = TArray<int32>(ChangedIndices.GetData(), ChangedIndices.Num());
		if (OnApply)
		{
			OnApply(EntityUUID, Layout, DecodedContainer, ChangedIndices);
		}
	}

	virtual void ApplyModelChanges(const FGuid& EntityUUID, const FString& ContainerId,
		TConstArrayView<FCrowdyAttributeChange> Changes) override
	{
		++ApplyModelChangesCallCount;
	}

	// An ordinary entity: held, and with a class recorded for it.
	void SetOwnedClass(const FGuid& EntityUUID, const UClass* Class)
	{
		HeldEntities.Add(EntityUUID);
		ClassByEntity.Add(EntityUUID, Class);
	}

	// A position-only entity: held exactly like any other, with nothing declared for it. The case that
	// distinguishes "not mine" from "mine, and there is nothing to do with this".
	void SetHeldWithNoClass(const FGuid& EntityUUID)
	{
		HeldEntities.Add(EntityUUID);
	}

	TSet<FGuid> HeldEntities;
	TMap<FGuid, TWeakObjectPtr<const UClass>> ClassByEntity;
	TFunction<void(const FGuid&, const FCrowdyRepLayout&, const void*, TConstArrayView<int32>)> OnApply;

	mutable int32 IsEntityKnownCallCount = 0;
	mutable int32 GetEntityClassCallCount = 0;
	int32 ResolveReceiverCallCount = 0;
	int32 ApplyDecodedStateCallCount = 0;
	int32 ApplyModelChangesCallCount = 0;
	FGuid LastEntityUUID;
	TArray<int32> LastChangedIndices;
};
