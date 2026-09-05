#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "GameFramework/Actor.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/ICrowdyEntityEventHandler.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "CrowdyRowEventTestTypes.generated.h"

struct FCrowdyAttributeChange;
struct FCrowdyRepLayout;

/**
 * The shape this seam exists for: a game's own ACTOR class declaring the events its players send. The
 * owner runs the bodies on a real actor; a client that draws the same player as a row in a table has no
 * instance to run them on, and is the case the tests here drive.
 *
 * Both recipients are declared because they are handled oppositely. A multicast reaches every observer
 * and is what a row-held entity must be able to apply; an owner-only call reaches its recipient over a
 * transport that addresses an actor, so one arriving for an entity with no actor is misuse or forgery.
 */
UCLASS()
class ACrowdyRowEventTestActor : public AActor
{
	GENERATED_BODY()

public:

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void RowMulticast_Implementation(int32 ActionId, float PlayRate);
	CROWDY_EVENT(RowMulticast)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient"))
	void RowOwnerOnly_Implementation(int32 ActionId);
	CROWDY_EVENT(RowOwnerOnly)

	int32 GotActionId = 0;
	int32 CallCount = 0;
};

/**
 * A plain UObject declaring a multicast CrowdyEvent, which is the shape an event authored for entities
 * that only ever exist as rows takes. No actor is ever of this class, so no arriving actor could run one
 * of its calls; that is what makes it the case the shortened spawn wait was reasoned about.
 *
 * Deliberately NOT an ICrowdyEventSource, so a wait shortened for a call declared here is shortened by the
 * subscriber's registered handler and by nothing else. It also stands in for the object a subscriber
 * enrols as a participant for another plane: an entity's participant that declares its own events and
 * none of the actor class's.
 */
UCLASS()
class UCrowdyRowEventTestRowOnly : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void RowOnlyMulticast_Implementation(int32 ActionId);

	int32 GotActionId = 0;
	int32 CallCount = 0;
};

/**
 * A subscriber double that also receives calls as values, so a test can drive the whole route an event
 * declared on an actor class takes to a client holding the entity as a row: known id, no object to invoke
 * on, decoded parameters handed to whatever is registered for the function.
 *
 * Every answer is set by the test rather than derived, because the questions are asked in an order that
 * matters and a double that computed its own answers would hide a reordering.
 */
UCLASS()
class UCrowdyRowEventSpySubscriber : public UObject, public ICrowdyEntitySubscriber,
	public ICrowdyEntityEventHandler
{
	GENERATED_BODY()

public:

	// What IsEntityKnown answers, for every id.
	bool bKnowsEntity = false;

	// What ResolveReceiver offers. Null is the answer for an event declared on a class this client holds no
	// instance of, which is the case the handler route exists for.
	UPROPERTY()
	TObjectPtr<UObject> ReceiverToOffer;

	// The one function this double claims a handler for. Null means nothing is registered, which is what
	// the router must report rather than drop silently. A bare pointer: a UFunction is kept alive by the
	// class that declares it, and a const object reference is not a shape reflection accepts.
	const UFunction* HandledFunction = nullptr;

	virtual bool IsEntityKnown(const FGuid& EntityUUID) const override
	{
		++IsEntityKnownCallCount;
		return bKnowsEntity;
	}

	virtual const UClass* GetEntityClass(const FGuid& EntityUUID) const override
	{
		++GetEntityClassCallCount;
		return nullptr;
	}

	virtual UObject* ResolveReceiver(const FGuid& EntityUUID, const UFunction* Function) override
	{
		++ResolveReceiverCallCount;
		return ReceiverToOffer;
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

	virtual bool HasEntityEventHandler(const UFunction* Function) const override
	{
		++HasEntityEventHandlerCallCount;
		return HandledFunction && HandledFunction == Function;
	}

	// What ChargeEntityEventBudget answers. False stands for an entity whose inbound allowance is spent.
	bool bBudgetAvailable = true;

	virtual bool ChargeEntityEventBudget(const FGuid& EntityUUID, const UFunction* Function) override
	{
		++ChargeEntityEventBudgetCallCount;
		ChargeStep = ++StepCounter;
		return bBudgetAvailable;
	}

	virtual void HandleEntityEvent(const FGuid& EntityUUID, const FGuid& SenderID, const UFunction* Function,
		const FCrowdyEventParams& Params) override
	{
		++HandleEntityEventCallCount;
		HandleStep = ++StepCounter;
		LastEntityUUID = EntityUUID;
		LastSenderID = SenderID;
		LastFunction = Function;

		// Read here rather than by storing Params: the frame it points at is torn down when the call that
		// handed it over returns, so a test that kept it would be reading a dead stack.
		bReadActionId = Params.GetInt32(TEXT("ActionId"), LastActionId);
		bReadPlayRate = Params.GetFloat(TEXT("PlayRate"), LastPlayRate);
		bReadAbsentParam = Params.GetInt32(TEXT("NoSuchParameter"), LastAbsentParam);
	}

	int32 ResolveReceiverCallCount = 0;
	int32 ApplyDecodedStateCallCount = 0;
	int32 ApplyModelChangesCallCount = 0;
	int32 HandleEntityEventCallCount = 0;
	int32 ChargeEntityEventBudgetCallCount = 0;
	mutable int32 IsEntityKnownCallCount = 0;
	mutable int32 GetEntityClassCallCount = 0;
	mutable int32 HasEntityEventHandlerCallCount = 0;

	// A shared tick stamped by each question as it is asked, so a test can state which came FIRST rather
	// than only that both happened. Order is the whole claim where a charge has to precede the work it
	// bounds, and two independent counters cannot express it.
	int32 StepCounter = 0;
	int32 ChargeStep = 0;
	int32 HandleStep = 0;

	FGuid LastEntityUUID;
	FGuid LastSenderID;
	const UFunction* LastFunction = nullptr;

	int32 LastActionId = 0;
	float LastPlayRate = 0.0f;
	int32 LastAbsentParam = -1;
	bool bReadActionId = false;
	bool bReadPlayRate = false;
	bool bReadAbsentParam = false;
};
