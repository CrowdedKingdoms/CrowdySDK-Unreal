#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Mass/CrowdyMassEntity.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "CrowdyMassTargetLibrary.generated.h"

/**
 * Everything a graph can do to one Mass-rendered entity, addressed as the entity itself.
 *
 * A developer never handles an entity id here. Every function takes the FCrowdyMassEntityTarget a trace
 * (or any other lookup) already handed back, and resolves what the SDK needs from the entity underneath
 * it: the handle's own serial says whether the entity is still there, and the entity's identity fragment
 * says who it is. An id carried alongside the handle could disagree with the entity it names; an id read
 * out of the entity cannot.
 *
 * A target dies with the entity, deliberately. Mass bumps a slot's serial when it is reused, so a target
 * held across a despawn does not quietly start naming whatever respawned into the same slot, and a queued
 * hit cannot follow a player through death onto their fresh body.
 *
 * The field accessors below read and write the fragments a game authors, by the name that game registered
 * (CrowdyMassFields::RegisterField). They hand back values, never pointers, and they refuse to run while
 * Mass is processing; see Target/CrowdyMassFieldAccess.h for both reasons.
 *
 * Game thread only.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassTargetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * True when the target still names a live entity that is ready to be read or written.
	 *
	 * Stricter than FCrowdyMassEntityTarget::IsSet, which only says both halves of the struct are filled
	 * in. This asks the entity manager, so it also answers false for an entity that has since despawned
	 * and for a handle whose slot has been reused by another entity.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Target", meta=(DisplayName="Is Crowdy Mass Target Live"))
	static bool IsTargetLive(const FCrowdyMassEntityTarget& Target);

	/**
	 * True when a game has registered a fragment field under this name.
	 *
	 * Worth checking once at startup: an unregistered name is not an error anywhere, every read simply
	 * answers false forever, which looks exactly like an entity that never changes.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Fields", meta=(DisplayName="Is Crowdy Mass Field Registered"))
	static bool IsFieldRegistered(FName FieldName);

	/**
	 * Reads one registered fragment field off the entity the target names. False when the entity is gone,
	 * carries no such fragment, no field is registered under that name, or the field is not of this type.
	 *
	 * A byte field also reads back a single-byte enum member, so an authored state enum does not have to be
	 * declared as a raw byte to be reachable from a graph.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Get Crowdy Mass Field (Byte)"))
	static bool GetFieldByte(const FCrowdyMassEntityTarget& Target, FName FieldName, uint8& OutValue);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Get Crowdy Mass Field (Integer)"))
	static bool GetFieldInt(const FCrowdyMassEntityTarget& Target, FName FieldName, int32& OutValue);

	/** Reads a field declared either float or double; both arrive here as the one Blueprint real type. */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Get Crowdy Mass Field (Float)"))
	static bool GetFieldFloat(const FCrowdyMassEntityTarget& Target, FName FieldName, double& OutValue);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Get Crowdy Mass Field (Boolean)"))
	static bool GetFieldBool(const FCrowdyMassEntityTarget& Target, FName FieldName, bool& bOutValue);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Get Crowdy Mass Field (Vector)"))
	static bool GetFieldVector(const FCrowdyMassEntityTarget& Target, FName FieldName, FVector& OutValue);

	/**
	 * Writes one registered fragment field on the entity the target names. False when the entity is gone,
	 * carries no such fragment, no field is registered under that name, the field is not of this type, or
	 * Mass is processing right now.
	 *
	 * That last refusal is the one to know about, and it has two causes. A graph reached from a delegate
	 * broadcast inside a Mass chunk loop is writing a fragment during the schedule, racing every processor
	 * declared to read it. So is an ordinary tick that happens to overlap a Mass phase, because phases run
	 * alongside the game thread rather than blocking it, which makes that second one intermittent. Either
	 * way the write is refused and reported rather than performed. Writing from a phase-end delegate, or
	 * from a broadcast deferred to the end of the frame, puts the same graph outside the schedule.
	 *
	 * What a graph writes here is a VISUAL. Nothing mechanical may be decided from a fragment: a fragment
	 * is local, and the same field on the same entity holds different values on two machines. Anything
	 * that has to be agreed on has to be decided by the server.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Set Crowdy Mass Field (Byte)"))
	static bool SetFieldByte(const FCrowdyMassEntityTarget& Target, FName FieldName, uint8 Value);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Set Crowdy Mass Field (Integer)"))
	static bool SetFieldInt(const FCrowdyMassEntityTarget& Target, FName FieldName, int32 Value);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Set Crowdy Mass Field (Float)"))
	static bool SetFieldFloat(const FCrowdyMassEntityTarget& Target, FName FieldName, double Value);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Set Crowdy Mass Field (Boolean)"))
	static bool SetFieldBool(const FCrowdyMassEntityTarget& Target, FName FieldName, bool bValue);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Fields", meta=(DisplayName="Set Crowdy Mass Field (Vector)"))
	static bool SetFieldVector(const FCrowdyMassEntityTarget& Target, FName FieldName, FVector Value);

	/**
	 * Puts the entity the target names into an authored pose by name, or clears its pose when PoseName is
	 * NAME_None. False when the entity is gone, carries no pose fragment (an archetype with no animation
	 * trait has nothing to pose), Mass is processing, or a pose was named for an entity whose class this
	 * client has not learned yet.
	 *
	 * The name is resolved against the animation set of the CLASS this entity stands for, taken from the
	 * target itself, so a caller names a pose and never an index and never a class. A name that is not
	 * authored in that class's set resolves to "no pose", which CLEARS the pose rather than leaving it
	 * alone, and says so in the log. An entity with no class yet is REFUSED instead: there is no set to
	 * look the name up in, so answering "no pose" would clear the pose while reporting success. Clearing
	 * still works on such an entity, because clearing needs no set.
	 *
	 * Presentation only, exactly like a pose that arrives over the network. Setting one on an entity that
	 * also receives view state is temporary by nature: the next update carrying a pose overwrites it.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim", meta=(DisplayName="Set Crowdy Mass Entity Pose"))
	static bool SetEntityPose(const FCrowdyMassEntityTarget& Target, FName PoseName);

	/**
	 * Plays a one-shot action by name on the entity the target names, on every machine that draws it.
	 *
	 * This is the whole of what "everyone sees my swing" costs: no event to declare, no parameters to
	 * name, no handler to register. It sends the SDK's own action event
	 * (UCrowdyMassEntity::PlayCrowdyAction), which every crowd stand-in can receive.
	 *
	 * The name is resolved against the animation set of the CLASS the entity is recorded as, taken from
	 * the target rather than from the caller, exactly as Set Crowdy Mass Entity Pose resolves a pose.
	 * Only the resolved id travels: a name-shaped parameter off the wire is interned permanently, so it
	 * would hand a peer a way to grow a receiver's memory without bound.
	 *
	 * ANY client may play an action on ANY entity, deliberately. It is the one thing on this plane that
	 * is safe to let a peer do to somebody else's character, because it decides nothing: no damage, no
	 * death, no targeting, no lifetime. Anything that must be agreed on has to be decided by the server.
	 *
	 * Variant selects among an action's authored variations, and PlayRate scales its playback. Both are
	 * bounded on arrival, and an id naming no row simply plays nothing.
	 *
	 * False, with a line in the log naming which of four reasons it is, when no action was sent: the
	 * target names no live routable entity, this client has not learned its class yet, that class names
	 * no animation set, or the set authors no row under this name. True means the call went to the
	 * transport, which has no delivery receipt.
	 *
	 * WHO SEES IT: everyone drawing the entity as a crowd row or as a near-tier actor, this client
	 * included. A machine holding the same player as a real actor of the game's own class does not; an
	 * action that also needs a world-space body keeps its own event on that class.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim",
		meta=(DisplayName="Send Crowdy Action", AdvancedDisplay="Variant,PlayRate"))
	static bool SendEntityAction(const FCrowdyMassEntityTarget& Target, FName ActionName, int32 Variant = 0,
		float PlayRate = 1.0f);

	/**
	 * Forgets when the Mass-processing send refusal was last reported, so the next one reports again.
	 *
	 * That refusal repeats for as long as its cause lasts, so it is spaced out rather than said once, and
	 * the spacing is process-wide and time-based. Without this a test asserting the refusal was reported
	 * would pass or fail on what ran before it and how recently.
	 */
	static void ResetSendThrottleForTest();
};
