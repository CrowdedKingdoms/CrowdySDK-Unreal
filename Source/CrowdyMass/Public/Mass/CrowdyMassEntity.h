#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyEntityTypes.h"
#include "Mass/EntityElementTypes.h"
#include "Mass/EntityHandle.h"
#include "MassEntityManager.h"
#include "Replication/RPC/ICrowdyEventSource.h"
#include "UObject/Object.h"
#include <type_traits>
#include "CrowdyMassEntity.generated.h"

class UCrowdyMassEntitySubsystem;

/**
 * Whether an identity describes an entity THIS client may originate events for, given who this client
 * is.
 *
 * Free-standing and pure so the rule itself can be asserted. Reached only through the member below it
 * would need a resolved world to answer at all, and a fixture without one refuses for that reason
 * instead, which leaves the comparison untested while looking covered.
 *
 * Role carries the meaning: Owner is "this client simulates and sends", RemoteProxy is "receives from
 * the network". The owner id is checked as well because the two are read by different code, the role
 * gating the send and the owner id stamping the sender, so an identity whose fields disagreed would
 * not refuse, it would send an event under another player's name. An unset owner id is not a
 * disagreement: the send path substitutes the local player as the sender in exactly that case.
 */
CROWDYMASS_API bool IsCrowdyMassIdentityLocallyOwned(ECrowdyRole Role, const FGuid& OwnerID, const FGuid& LocalPlayerID);

/**
 * Stands in for one Mass entity so ordinary object-shaped code can address it.
 *
 * An entity simulated as a row in a table has no actor, and the event system addresses a sender and a
 * receiver as objects. This is the object: one per entity that something actually needs to talk to,
 * holding nothing but the entity's handle and its network id and reading everything else out of the
 * entity's own fragments at the moment it is asked. It owns no state of its own on purpose, so an
 * avatar that is thrown away and made again is indistinguishable from the one it replaced.
 *
 * It is deliberately not a place to put behaviour. Anything that has to happen for many entities
 * belongs in a processor, which visits them in bulk; a per-entity object doing the same work costs a
 * virtual call and a scattered memory read per entity per frame, which at crowd sizes is the whole
 * frame. A handler here writes a fragment and returns.
 *
 * Nothing here extends the entity's life or ends it. The handle is a plain reference: when the entity
 * is gone every accessor answers "no", and this object simply stops being useful. Entity lifetime
 * belongs to the tracker and the rendering backend, and to nothing else.
 */
UCLASS(Blueprintable, BlueprintType)
class CROWDYMASS_API UCrowdyMassEntity : public UObject, public ICrowdyEventSource
{
	GENERATED_BODY()

public:

	// Points this object at one entity. Called by the subsystem that mints it; an avatar is never bound
	// twice, because a re-used id gets a fresh object rather than a repointed one.
	void BindToEntity(UCrowdyMassEntitySubsystem* InOwner, FMassEntityHandle InHandle, const FGuid& InUUID);

	FMassEntityHandle GetHandle() const { return Handle; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass", meta=(DisplayName="Get Entity UUID"))
	FGuid GetUUID() const { return UUID; }

	// True while the entity this object stands for still exists. False once it is destroyed, and false
	// for an object whose owning world has gone away; the object itself remains safe to call either way.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass", meta=(DisplayName="Is Entity Valid"))
	bool IsValid() const;

	/**
	 * Pointer to one of this entity's fragments, or null when the entity is gone or does not carry that
	 * fragment. Both misses are ordinary: an entity built from a different config legitimately lacks
	 * fragments another one has, so callers check the pointer rather than assuming a layout.
	 *
	 * The pointer is valid for this call and no longer. Mass moves an entity's data whenever its
	 * archetype changes, so storing one across a frame hands you another entity's memory.
	 */
	template <typename FragmentType>
	FragmentType* Frag() const
	{
		static_assert(std::is_base_of_v<FMassFragment, FragmentType>,
			"Frag<T> reads a Mass fragment; T must derive from FMassFragment.");

		FMassEntityManager* EntityManager = GetEntityManager();
		if (!EntityManager || !EntityManager->IsEntityValid(Handle))
		{
			return nullptr;
		}

		return EntityManager->GetFragmentDataPtr<FragmentType>(Handle);
	}

	virtual FGuid GetEventEntityID() const override;

	// Reads the entity's own transform fragment, and only that. A caller-supplied position is never
	// accepted anywhere on the send path: the spatial transport decides who receives an event from this
	// number, so letting a caller name it would let one entity announce itself anywhere in the world.
	virtual bool GetEventLocation(FVector& OutLocation) const override;

	virtual FGuid GetEventOwnerID() const override;

	/**
	 * This client's relationship to the entity, as the entity itself records it.
	 *
	 * It answers HostOwned both for a genuine world entity and for an entity nothing has identified yet,
	 * because that is the safe assumption for the send path it exists to serve. Do NOT gate anything that
	 * writes server state on it: the entity registry is where "not identified" and "identified as
	 * host-owned" are different facts.
	 */
	virtual ECrowdyRole GetEventRole() const override;

	virtual bool IsEventLocallyOwned() const override;

	/**
	 * Plays a one-shot action on this entity: a flinch, an attack swing, an emote.
	 *
	 * The one action event the SDK itself declares, so a game that only wants "everyone sees this
	 * animation" declares no event of its own at all. Send it with Send Crowdy Action, which takes the
	 * action by NAME and resolves the id here on the sending machine. There is deliberately no call-site
	 * thunk for it: calling a Crowdy event on an instance is refused for an entity this client is not
	 * the authority for, and an action aimed at another player's character is the ordinary case here
	 * rather than the exception.
	 *
	 * It is declared on this class rather than on an avatar class because this is the class every
	 * stand-in an entity can be given derives, whichever avatar class a game selects. An event declared
	 * further down would stop being deliverable the day a game selects an avatar class outside that
	 * branch.
	 *
	 * ActionId names a row in the animation set of the class this entity is recorded as: id N is
	 * Actions[N-1], and 0 is no action. It carries an id rather than a name because the parameter is
	 * untrusted, and every distinct name arriving off the wire is interned permanently, so a name-shaped
	 * parameter hands a peer a way to grow this process's memory without bound.
	 *
	 * Every parameter is treated as forged and all three are bounded by the write. That bound is what
	 * makes one shared event safe to ship: the worst a forged call can do is make somebody else's
	 * character flinch when it should not have. Nothing that decides a mechanical outcome may ever
	 * travel here; such state belongs on the server-authoritative plane, which is checked.
	 *
	 * WHO SEES IT: every client drawing this entity as a crowd row, and every client drawing it as a
	 * near-tier actor, both of which read the action off the entity's own fragment. A client holding the
	 * same player as a real actor of the game's own class does NOT, including that player's own machine,
	 * because nothing on such an actor declares this event. An action that also needs a world-space body
	 * keeps its own event on the class that owns that body.
	 */
	UFUNCTION(meta=(CrowdyEvent, CrowdyAction, CrowdyRecipient="SpatialMulticast"))
	void PlayCrowdyAction_Implementation(int32 ActionId, int32 Variant, float PlayRate);

private:

	// Weak, because the subsystem holds the avatar and the avatar must not hold the subsystem. It also
	// answers a question a raw pointer could not: a world that has gone away leaves every accessor here
	// returning "no" instead of reading a destroyed entity manager.
	TWeakObjectPtr<UCrowdyMassEntitySubsystem> Owner;

	FMassEntityHandle Handle;

	FGuid UUID;

	FMassEntityManager* GetEntityManager() const;
};
