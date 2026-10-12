#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyMassActionLibrary.generated.h"

class AActor;
class UAnimMontage;
class UAnimSequence;
class UCrowdyMassAnimSet;
class USkeletalMeshComponent;

/**
 * The actor half of a one-shot action, so that one authored event body is correct on every representation
 * of an entity.
 *
 * An event declared an action (meta=(CrowdyAction)) needs no code at all where a client draws the entity
 * as one of a crowd: the action is started from the arriving parameters. Where a client holds the same
 * entity as a real actor there IS a body, it runs, and it has to play the same clip the crowd plays or the
 * two representations of one event diverge. That is what this is for: the body calls PlayCrowdyMassAction
 * and the clip comes from the same authored row the crowd renders.
 *
 * AN ACTION ID IS A ROW POSITION IN ONE CLASS'S ANIMATION SET, so which class is asking is part of the
 * question, not a detail. Each call here resolves the set of the class of the actor it was given: an actor
 * of one class must never play the clip a different class authored under the same id, which is exactly
 * what a class-less lookup would do and would do silently.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassActionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Plays the action ActionId names on Mesh, taking the clip from the animation set the crowd renders.
	 *
	 * Call this from the event body a game declares an action, passing the parameters the event carried.
	 * The same call is then correct wherever the body runs: on the sender's own pawn, and on an observer
	 * holding the entity as an actor. An observer holding it as a row never runs the body and starts the
	 * action from the same row through its fragment instead.
	 *
	 * Played as a dynamic montage into SlotName rather than through PlayAnimation, which would put the
	 * component into single-node mode and throw away the animation blueprint for good. The mesh's graph
	 * therefore needs a Slot node of that name; the engine's default is "DefaultSlot".
	 *
	 * PlayRate is clamped exactly as the fragment write clamps it, so a forged rate cannot make one
	 * representation play at a speed another refuses. The clip plays once and holds, matching the Clamp
	 * loop mode the crowd composes for an action.
	 *
	 * Variant is accepted and currently selects nothing, on either representation. It is carried on the
	 * wire and clamped, and no authored row branches on it yet; it is taken here so a body written today
	 * does not change shape when it does.
	 *
	 * The animation set is the one the OWNING ACTOR'S class renders through, taken from the mesh rather
	 * than from a parameter so that a body written in Blueprint has nothing extra to fill in. A mesh with
	 * no owner has no class to resolve against and is refused.
	 *
	 * StartSecondsIntoAction seeks the clip forward before it starts, for an action that was already
	 * running before this actor existed to play it. Leave it at zero for an action that starts now; the
	 * engine clamps it to the clip's length either way.
	 *
	 * False, with one warning per action id, when the owning class names no animation set, when the id
	 * names no row in it, or when that row authored no sequence for an actor to play.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim",
		meta=(DefaultToSelf="Mesh", AdvancedDisplay="SlotName,BlendInSeconds,BlendOutSeconds,StartSecondsIntoAction"))
	static bool PlayCrowdyMassAction(USkeletalMeshComponent* Mesh, int32 ActionId, int32 Variant = 0,
		float PlayRate = 1.0f, FName SlotName = TEXT("DefaultSlot"), float BlendInSeconds = 0.1f,
		float BlendOutSeconds = 0.1f, float StartSecondsIntoAction = 0.0f);

	/**
	 * The same playback as PlayCrowdyMassAction, with the animation set handed in rather than worked out
	 * from the mesh's owning class, and the montage handed back so a caller can stop it again.
	 *
	 * For a caller holding an actor that stands in for an entity of a DIFFERENT class: a pooled shell
	 * borrowed to draw a character close to the camera resolves its own class's set, which is either
	 * nothing or the shell's own, and neither is the set the entity's rows were authored in. Since an
	 * action id is a row position in one set, inferring the set from the shell would play whatever that
	 * other set holds at the same row, which is a wrong clip and looks like a working one.
	 *
	 * Null when the id names no row in AnimSet, when that row authored no sequence, or when the mesh has
	 * no animation instance to play a montage on, reported the same way and at the same rate as the
	 * class-resolved call above.
	 */
	static UAnimMontage* PlayCrowdyMassActionFromSet(USkeletalMeshComponent* Mesh, const UCrowdyMassAnimSet* AnimSet,
		int32 ActionId, int32 Variant, float PlayRate, FName SlotName, float BlendInSeconds,
		float BlendOutSeconds, float StartSecondsIntoAction);

	/**
	 * The clip an actor of Actor's class would play for ActionId, or null when that class names no
	 * animation set, the id names no row in it, or that row authored none. Exposed for a body that needs
	 * to do something other than play it into a slot.
	 *
	 * Actor supplies both halves of the question and neither is optional: its world says which crowd is
	 * being drawn, and its CLASS says which animation set the id is a row in. It defaults to self, so a
	 * body running on the actor itself fills it in without the author doing anything.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim", meta=(DefaultToSelf="Actor"))
	static UAnimSequence* FindCrowdyMassActionSequence(const AActor* Actor, int32 ActionId);
};
