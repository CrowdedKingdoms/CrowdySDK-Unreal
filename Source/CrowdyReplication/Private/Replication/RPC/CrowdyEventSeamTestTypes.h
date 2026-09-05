#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "CrowdyEventSeamTestTypes.generated.h"

struct FCrowdyAttributeChange;
struct FCrowdyRepLayout;

/**
 * A subscriber double that records how many times it was asked about an inbound event, so a test can
 * prove the router never consults it for an entity that already resolves through the ordinary
 * actor/participant registry. Always answers "unknown" and "nothing to receive on" the least useful
 * pair of answers so that if it were ever consulted by mistake, the call it was asked about would
 * visibly fail to deliver rather than accidentally succeeding and looking correct.
 *
 * The state and model planes are present because the interface declares them and answer nothing: this
 * double exists for the event plane, and a call landing on either of them here would be a test reading
 * the wrong seam.
 */
UCLASS()
class UCrowdySpyEntitySubscriber : public UObject, public ICrowdyEntitySubscriber
{
	GENERATED_BODY()

public:

	virtual bool IsEntityKnown(const FGuid& EntityUUID) const override
	{
		++IsEntityKnownCallCount;
		return false;
	}

	virtual const UClass* GetEntityClass(const FGuid& EntityUUID) const override
	{
		++GetEntityClassCallCount;
		return nullptr;
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
	}

	int32 ResolveReceiverCallCount = 0;
	int32 ApplyDecodedStateCallCount = 0;
	int32 ApplyModelChangesCallCount = 0;
	mutable int32 IsEntityKnownCallCount = 0;
	mutable int32 GetEntityClassCallCount = 0;
};
