#pragma once

#include "CoreMinimal.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassEntity.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "CrowdyMassCharacterAvatar.generated.h"

/**
 * The standard avatar for a remote player rendered as a Mass entity: it receives that entity's
 * one-shot actions and writes them into its action fragment.
 *
 * This is the form for a game that declares its actions HERE, on an avatar class. A game that declares
 * them on the actor class its own players run keeps that one declaration and registers the same write
 * through UCrowdyMassEntitySubsystem::RegisterEventHandler instead (CrowdyMassMakeActionEventHandler is
 * this class's two handlers in that form), which is the only way an event declared on an actor can reach
 * an observer who holds that player as a row rather than as an actor.
 *
 * Two events, and deliberately only two. Both are cosmetic and self-limiting: the worst a forged one
 * can do is make somebody else's character swing or flinch when it should not have. That bound is the
 * reason they are safe to ship, because the relay does not yet bind a sender to the entity it claims
 * to speak for, so a forged sender id is honored. Nothing that decides a mechanical outcome may be
 * added here while that is true: no death, no damage, no targeting, no collision, no lifetime. Such
 * state belongs on the server-authoritative plane, which is checked, not on this view plane, which is
 * not.
 *
 * Each handler writes one fragment and returns. It starts nothing, spawns nothing, destroys nothing,
 * and visits no other entity; a handler that looped over entities would run per event on the game
 * thread, which is precisely the cost this whole design exists to avoid. Everything batched happens in
 * a processor afterwards.
 *
 * An observer who comes into range while an action is already playing sees nothing of it, and that is
 * intended. Actions are events, never state, so there is nothing to replay and therefore nothing that
 * can replay a swing that finished seconds ago on an entity that has since respawned under the same
 * id.
 */
UCLASS(Blueprintable, BlueprintType)
class CROWDYMASS_API UCrowdyMassCharacterAvatar : public UCrowdyMassEntity
{
	GENERATED_BODY()

public:

	/**
	 * Plays a one-shot action on this entity: an attack swing, an emote, and similar.
	 *
	 * ActionId names the action in the vocabulary the sending game and the receiving anim set share; it
	 * is resolved against the entity's anim set by the animation pass, and an id that does not resolve
	 * there simply plays nothing. Zero and negative ids are refused here: zero is the fragment's own
	 * "no action" value, so honoring it would let an event cancel an action rather than start one.
	 *
	 * It carries an id rather than a name because the parameter is untrusted. Every distinct name that
	 * arrives off the wire is interned permanently, so a name-shaped parameter hands a peer a way to
	 * grow this process's memory without bound by sending garbage; an integer cannot.
	 */
	UFUNCTION(meta=(CrowdyEvent, CrowdyRecipient="SpatialMulticast"))
	void PlayOneShot_Implementation(int32 ActionId, int32 Variant, float PlayRate);
	CROWDY_EVENT(PlayOneShot)

	/**
	 * Plays a hit reaction on this entity: a flinch, a stagger, and similar.
	 *
	 * It writes the same action slot PlayOneShot does, and exists separately so a game can rate-limit,
	 * suppress or restyle reactions to being hit without touching every other one-shot, and so the two
	 * can be told apart in a trace. It carries no play rate: a flinch reads as a reaction to a specific
	 * blow, and a peer choosing how fast somebody else's character recoils is not worth the parameter.
	 *
	 * It carries no damage, no instigator and no outcome, and it changes nothing but which animation
	 * plays. Whether the hit landed, and what it did, is decided elsewhere.
	 */
	UFUNCTION(meta=(CrowdyEvent, CrowdyRecipient="SpatialMulticast"))
	void HitReact_Implementation(int32 ActionId, int32 Variant);
	CROWDY_EVENT(HitReact)

private:

	// The single write both events share. Clamps every field, bumps the retrigger serial, and returns.
	void WriteAction(int32 ActionId, int32 Variant, float PlayRate);
};
