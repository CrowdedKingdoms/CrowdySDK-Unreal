#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Mass/EntityHandle.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "UObject/Object.h"
#include "CrowdyMassRowEventTestTypes.generated.h"

/**
 * A game's own actor class declaring the one-shot action its players send, which is the whole point of the
 * seam these tests cover: this class is authored once and is the only class involved. The owner runs the
 * body on a real actor; an observer drawing the same player as a Mass row runs the registered handler.
 */
// Hidden so a fixture never reaches an author's class or event picker.
UCLASS(Hidden)
class ACrowdyMassRowEventTestActor : public AActor
{
	GENERATED_BODY()

public:

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast"))
	void PlayOneShot_Implementation(int32 ActionId, int32 Variant, float PlayRate);
	CROWDY_EVENT(PlayOneShot)

	/**
	 * The same event with the author's declaration that it describes a one-shot action.
	 *
	 * The pair matters more than either half: PlayOneShot above carries an int32 called ActionId and is NOT
	 * declared an action, so the two differ in the marker alone. That is what lets a test tell "the game
	 * said this is an action" apart from "a plugin recognised a parameter name", which are the same
	 * observation on any class where only one of the two exists.
	 */
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast", CrowdyAction))
	void PlayDeclaredAction_Implementation(int32 ActionId, int32 Variant, float PlayRate);
	CROWDY_EVENT(PlayDeclaredAction)

	// Declared an action and carrying no id to start one with. An authoring mistake that would otherwise
	// look exactly like an event that never arrived.
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast", CrowdyAction))
	void PlayIdlessAction_Implementation(int32 Variant, float PlayRate);
	CROWDY_EVENT(PlayIdlessAction)

	// An ordinary reflected function carrying none of the Crowdy markers. It exists so that "this is not a
	// Crowdy event" can be told apart from "there is no such function", which are the same answer from any
	// test that only ever asks about functions that do not exist.
	UFUNCTION()
	void NotACrowdyEvent(int32 ActionId);

	int32 GotActionId = 0;
	int32 CallCount = 0;
};

/**
 * Signatures the action-contract check has to separate, on a plain object carrying no Crowdy markers at
 * all.
 *
 * Deliberately not Crowdy events. What the check reads is the parameter list and nothing else, and an
 * event whose parameters are wrong on purpose would also be swept by the auto-registry, whose own refusals
 * would arrive as errors from a completely different subsystem in the middle of these cases.
 */
UCLASS()
class UCrowdyMassActionSignatureTestTarget : public UObject
{
	GENERATED_BODY()

public:

	// The shape an action event is supposed to have, so the check answering "nothing wrong" is exercised
	// rather than assumed from the other three staying quiet.
	UFUNCTION()
	void WellFormedAction(int32 ActionId, int32 Variant, float PlayRate);

	// The mistake an author makes by ticking the box before adding the pins.
	UFUNCTION()
	void NoIdAction(int32 Variant, float PlayRate);

	// The parameter is there under the right name and cannot carry an id. Its own case, because comparing
	// this event against the documentation shows a match.
	UFUNCTION()
	void WrongTypedAction(float ActionId);

	// An output parameter: the call arrived from another machine and there is nobody to return to, so a
	// value written into it goes nowhere. Reads as present to anything checking only the name.
	UFUNCTION()
	void OutputIdAction(int32& ActionId);

	// A readable id with an unreadable variant. The action plays; the variant is silently always 0, which
	// is also what an event declaring no variant at all means, so the two are indistinguishable in the
	// running game and only the declaration can tell them apart.
	UFUNCTION()
	void WrongTypedVariantAction(int32 ActionId, float Variant);

	// The same for the rate, which reads back as 1 forever.
	UFUNCTION()
	void WrongTypedPlayRateAction(int32 ActionId, int32 PlayRate);

	// A double play rate, which IS read: GetFloat accepts either width and narrows. It exists so the check
	// on the rate cannot be written as "must be an FFloatProperty" and still pass.
	UFUNCTION()
	void DoublePlayRateAction(int32 ActionId, double PlayRate);
};

/**
 * A class unrelated to the one the events above are declared on, for the case where an event is aimed at
 * an entity of the wrong kind. The authoring guard must refuse that whether or not the event needed any
 * registration to be delivered.
 */
UCLASS(Hidden)
class ACrowdyMassRowEventUnrelatedTestActor : public AActor
{
	GENERATED_BODY()
};

/**
 * Stands in for the object a receive node binds to: an ordinary object with reflected functions shaped the
 * way the node's intermediate event is, the entity target first and the event's own parameters after it.
 *
 * The variants exist to pin what the binding rules actually are. Swapped is the same parameters in a
 * different order, which by name is no difference at all and by position is a silent swap. Partial has one
 * parameter retyped and one the event does not declare, neither of which may be filled from anything.
 */
UCLASS()
class UCrowdyMassBoundEventTestHandler : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnPlayOneShot(FCrowdyMassEntityTarget Target, int32 ActionId, int32 Variant, float PlayRate);

	UFUNCTION()
	void OnPlayOneShotSwapped(FCrowdyMassEntityTarget Target, int32 Variant, int32 ActionId, float PlayRate);

	UFUNCTION()
	void OnPlayOneShotPartial(FCrowdyMassEntityTarget Target, float ActionId, int32 Variant, int32 Absent);

	// No entity target, which a handler cannot do without: it would have no way of telling which entity the
	// call it is running for arrived for.
	UFUNCTION()
	void OnPlayOneShotNoTarget(int32 ActionId);

	// An output parameter, which there is nobody to return anything to.
	UFUNCTION()
	void OnPlayOneShotWithOutput(FCrowdyMassEntityTarget Target, int32& OutActionId);

	int32 CallCount = 0;
	FMassEntityHandle GotEntity;
	int32 GotActionId = 0;
	int32 GotVariant = 0;
	float GotPlayRate = 0.0f;
	float GotActionIdAsFloat = 0.0f;
	int32 GotAbsent = 0;
};

/**
 * A subclass that redeclares nothing. It stands for the ordinary case of an event declared on a base class
 * and an entity recorded as a derived one, which the authoring guard must accept: a guard that only
 * accepted an exact class would close the gate on every game whose players derive from a shared base.
 */
UCLASS(Hidden)
class ACrowdyMassRowEventDerivedTestActor : public ACrowdyMassRowEventTestActor
{
	GENERATED_BODY()
};
