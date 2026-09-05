#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "UObject/Interface.h"
#include "ICrowdyEntitySubscriber.generated.h"

struct FCrowdyAttributeChange;
struct FCrowdyRepLayout;

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UCrowdyEntitySubscriber : public UInterface
{
	GENERATED_BODY()
};

/**
 * Receives everything addressed to an entity that has no object to address.
 *
 * The router normally resolves an inbound payload's target in the entity registry, which holds actors and
 * enrolled objects, and delivers it onto that object: a call is invoked on it, state is decoded onto its
 * properties, a pulled model value is written onto its member. An entity simulated as a row in a table has
 * nothing there to resolve, and giving it an object purely so payloads have somewhere to land would undo
 * the reason it is a row. So a subscriber registers once and is handed the payloads for such entities,
 * whichever plane they arrive on, and decides for itself what each one does to its own storage.
 *
 * One subscriber at a time. The router asks only after its own lookup has failed, so a subscriber never
 * displaces an actor and never sees a payload that already had a home. Every function is called on the
 * game thread only.
 *
 * Every EntityUUID below comes off the network and is untrusted. Treat it as an arbitrary value: never
 * allocate, and never create state keyed on an id that is not already known.
 */
class CROWDYREPLICATION_API ICrowdyEntitySubscriber
{
	GENERATED_BODY()

public:

	/**
	 * True when this subscriber holds the id at all, whether or not it currently has anywhere to put a
	 * payload for it.
	 *
	 * This is the only question the router's wait decision reads, and it separates "I have never heard of
	 * this id" from "that id is mine and there is nothing to deliver to". Those need opposite handling:
	 *
	 *   - False: not this subscriber's entity. The router keeps the payload on its short spawn-wait queue
	 *     and asks again, because an entity whose creation message is still in flight looks exactly like an
	 *     id nobody holds.
	 *   - True: the entity is this subscriber's. The router stops waiting for that id, for this payload and
	 *     for every later one. If nothing can be done with the payload, that is a refusal, and a refusal is
	 *     final.
	 *
	 * Getting it backwards costs in both directions. Answering false for an id this subscriber does hold
	 * turns every payload for that entity into a wait that can only time out, and a crowd of them fills the
	 * shared wait queue and evicts the payloads for entities that genuinely are still arriving. Answering
	 * true for an id it does not hold takes the payload away from whatever was about to register that
	 * entity.
	 *
	 * Answer from the full set of ids held, not from whatever subset is currently ready to receive
	 * something. In particular this is NOT "GetEntityClass returns non-null": an entity may legitimately be
	 * held with no class, and answering from the class would turn every payload for it into a timeout.
	 */
	virtual bool IsEntityKnown(const FGuid& EntityUUID) const = 0;

	/**
	 * The class this entity is a representation of, or null when there is none recorded for it.
	 *
	 * One class serves all three planes, because the class that declares an entity's replicated properties,
	 * its callable functions and its server-owned container is the same class in every case: the one the
	 * owner runs as a real object on their own machine.
	 *
	 * NULL IS A LEGAL ANSWER FOR AN ENTITY THIS SUBSCRIBER HOLDS, and it does not mean the id is unknown.
	 * An entity may exist purely as a position with nothing declared for it. Such an entity is still known,
	 * still refuses rather than waits, and simply has no routing for the planes that need a class. Never
	 * answer this question by asking whether the id is known, or the other way round.
	 *
	 * What this value is, stated once because all three planes lean on it and they do not lean equally.
	 * It is recorded locally when the entity is created, from the local resolution of what the entity is,
	 * and it is never read off the payload being judged. That makes it sound against a THIRD party: a peer
	 * cannot forge payloads for someone else's entity, because the class the values are interpreted against
	 * is the one this client already decided the entity has, and a payload naming a different one is
	 * dropped. It is NOT sound against the entity's own owner, who chose what their entity claims to be
	 * when it announced itself. So it is a routing decision and an authoring guard, and nothing that must
	 * hold against a modified client may rest on it.
	 */
	virtual const UClass* GetEntityClass(const FGuid& EntityUUID) const = 0;

	/**
	 * Returns the object an inbound call should be delivered to for this entity, or null when there is none.
	 *
	 * Function is the receiver the sender named, supplied so a subscriber keeping more than one object per
	 * entity can pick the one that declares it. The router still checks the returned object against that
	 * function before invoking anything, so a mismatched object drops the call rather than corrupting a
	 * dispatch.
	 *
	 * Returning null is a refusal, not a wait: see IsEntityKnown.
	 */
	virtual UObject* ResolveReceiver(const FGuid& EntityUUID, const UFunction* Function) = 0;

	/**
	 * Copies the values a view-state delta delivered out of DecodedContainer into the entity's own storage.
	 *
	 * DecodedContainer is a raw buffer laid out like an instance of the class GetEntityClass named for this
	 * id, and it is NOT a UObject. Read a value with Layout.Properties[Index].Property, through
	 * ContainerPtrToValuePtr on this buffer; never cast the buffer, never store it, and never hand it to
	 * anything that expects an object. It is valid for the duration of this call only.
	 *
	 * ONE buffer is shared by every entity of that class, so it arrives holding whatever the previous
	 * entity's delta left in it. A delta carries only the properties its sender put on the wire (a keyframe
	 * carries all of them), so a slot that is not listed in DeliveredIndices holds SOME OTHER ENTITY'S
	 * value: not a default, and not this entity's previous value. DeliveredIndices is the only statement of
	 * what this delta actually delivered, so read those slots and nothing else. A value read outside that
	 * set is another entity's state being written into this one.
	 *
	 * DELIVERED, not changed, and the distinction is load-bearing. "Changed" can only be computed against
	 * what a container already held, and this container holds the PREVIOUS entity's values, so an entity
	 * sent the same value its predecessor happened to have would be reported as having been sent nothing.
	 * It would then keep whatever it already held indefinitely, because every later heartbeat re-sending
	 * that value is suppressed for the same reason.
	 *
	 * DeliveredIndices is ascending, and every index in it is valid for Layout.Properties.
	 *
	 * No OnRep notify fires on this path, by design rather than by omission: a notify is a function call on
	 * an object, and an entity that receives its state as data has no object to call one on. Work that must
	 * happen when a value changes happens here, driven by DeliveredIndices.
	 */
	virtual void ApplyDecodedState(const FGuid& EntityUUID, const FCrowdyRepLayout& Layout,
		const void* DecodedContainer, TConstArrayView<int32> DeliveredIndices) = 0;

	/**
	 * Writes the server-owned values a model pull delivered into the entity's own storage.
	 *
	 * ContainerId names the server-side container the values came from. An entity can be told about more
	 * than one, so read it rather than assuming every call is about the same one.
	 *
	 * Unlike a view-state delta, these ARE changes. They are diffed against a cache held per entity rather
	 * than decoded into a buffer shared with other entities, so a key appears here only when its value
	 * differs from what this client last saw for this entity, and a key absent from Changes has genuinely
	 * not moved. Do not carry the reasoning for the state plane over to this one.
	 *
	 * Values are compact JSON, and NewValueJson is empty for a removed key.
	 *
	 * This is reached from a pull the caller already batched. Nothing here may start one, per entity or
	 * otherwise: a pull issued from inside a per-entity path is how a crowd turns one notification into a
	 * request per entity and stalls the game thread.
	 */
	virtual void ApplyModelChanges(const FGuid& EntityUUID, const FString& ContainerId,
		TConstArrayView<FCrowdyAttributeChange> Changes) = 0;
};
