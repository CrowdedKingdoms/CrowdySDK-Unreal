#pragma once

// Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside a
// preprocessor block, so the fixtures are declared unconditionally and only the tests that use them are guarded.

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Replication/GameModel/CrowdyAttributeChange.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "CrowdyGameModelSubscriberTestTypes.generated.h"

struct FCrowdyRepLayout;

/**
 * A subscriber double that records what the model plane hands it.
 *
 * The set of ids it HOLDS is kept separate from the class it names for each, as the interface requires, so a
 * test can produce the case that only exists when the two are separate: an id held with no class at all.
 * Nothing is invented for an id a test did not register.
 */
UCLASS()
class UCrowdyGameModelSpySubscriber : public UObject, public ICrowdyEntitySubscriber
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
		const void* DecodedContainer, TConstArrayView<int32> DeliveredIndices) override
	{
		++ApplyDecodedStateCallCount;
	}

	virtual void ApplyModelChanges(const FGuid& EntityUUID, const FString& ContainerId,
		TConstArrayView<FCrowdyAttributeChange> Changes) override
	{
		++ApplyModelChangesCallCount;
		LastEntityUUID = EntityUUID;
		LastContainerId = ContainerId;
		LastChanges = TArray<FCrowdyAttributeChange>(Changes.GetData(), Changes.Num());
		if (OnApplyModelChanges)
		{
			OnApplyModelChanges();
		}
	}

	// An ordinary entity: held, and with a class recorded for it.
	void SetOwnedClass(const FGuid& EntityUUID, const UClass* Class)
	{
		HeldEntities.Add(EntityUUID);
		ClassByEntity.Add(EntityUUID, Class);
	}

	// A position-only entity: held exactly like any other, with nothing declared for it.
	void SetHeldWithNoClass(const FGuid& EntityUUID)
	{
		HeldEntities.Add(EntityUUID);
	}

	// The new value the last call carried for Key, or an empty string when it carried none.
	FString FindLastNewValue(const FName Key) const
	{
		for (const FCrowdyAttributeChange& Change : LastChanges)
		{
			if (Change.Key == Key)
			{
				return Change.NewValueJson;
			}
		}
		return FString();
	}

	bool LastChangesContain(const FName Key) const
	{
		return LastChanges.ContainsByPredicate(
			[Key](const FCrowdyAttributeChange& Change) { return Change.Key == Key; });
	}

	TSet<FGuid> HeldEntities;
	TMap<FGuid, TWeakObjectPtr<const UClass>> ClassByEntity;

	// Run from inside ApplyModelChanges, so a test can prove what a handler reaching back into the subsystem
	// sees at that moment.
	TFunction<void()> OnApplyModelChanges;

	mutable int32 IsEntityKnownCallCount = 0;
	mutable int32 GetEntityClassCallCount = 0;
	int32 ResolveReceiverCallCount = 0;
	int32 ApplyDecodedStateCallCount = 0;
	int32 ApplyModelChangesCallCount = 0;
	FGuid LastEntityUUID;
	FString LastContainerId;
	TArray<FCrowdyAttributeChange> LastChanges;
};
