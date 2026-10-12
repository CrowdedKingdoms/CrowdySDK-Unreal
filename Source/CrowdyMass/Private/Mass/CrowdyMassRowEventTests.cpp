#include "Mass/CrowdyMassRowEventTestTypes.h"

void ACrowdyMassRowEventTestActor::PlayOneShot_Implementation(int32 ActionId, int32 Variant, float PlayRate)
{
	GotActionId = ActionId;
	++CallCount;
}

void ACrowdyMassRowEventTestActor::PlayDeclaredAction_Implementation(int32 ActionId, int32 Variant, float PlayRate)
{
	GotActionId = ActionId;
	++CallCount;
}

void ACrowdyMassRowEventTestActor::PlayIdlessAction_Implementation(int32 Variant, float PlayRate)
{
	++CallCount;
}

void ACrowdyMassRowEventTestActor::NotACrowdyEvent(int32 ActionId)
{
	GotActionId = ActionId;
}

// The signature fixtures exist to be reflected over, never called. Bodies so the module links.
void UCrowdyMassActionSignatureTestTarget::WellFormedAction(int32 ActionId, int32 Variant, float PlayRate) {}
void UCrowdyMassActionSignatureTestTarget::NoIdAction(int32 Variant, float PlayRate) {}
void UCrowdyMassActionSignatureTestTarget::WrongTypedAction(float ActionId) {}
void UCrowdyMassActionSignatureTestTarget::OutputIdAction(int32& ActionId) {}
void UCrowdyMassActionSignatureTestTarget::WrongTypedVariantAction(int32 ActionId, float Variant) {}
void UCrowdyMassActionSignatureTestTarget::WrongTypedPlayRateAction(int32 ActionId, int32 PlayRate) {}
void UCrowdyMassActionSignatureTestTarget::DoublePlayRateAction(int32 ActionId, double PlayRate) {}

// Also gated on !UE_BUILD_SHIPPING, not because a test would run there, but because these cases construct
// CrowdyMassDispatchProxy::FScopedDisableForTest, which only exists where the CVar-backed flag it toggles
// does. The two conditions coincide unless a build forces automation tests into a Shipping target.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "Misc/AutomationTest.h"

#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Templates/SharedPointer.h"
#include <limits>

// Covers the CrowdyMass half of the actor-class event route: which functions this subsystem claims a
// handler for, what it refuses before running one, and that an inbound call is billed against the entity's
// allowance exactly once whichever of the two routes carries it. The router's half is covered in
// CrowdySDK/CrowdyReplication's CrowdyRowEventTests.cpp.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassRowEventTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager with no World, MassEntitySubsystem, spawner, or config asset. The
	// entity carries the action fragment, which is what a handler writes; RegisterTrackedEntityForTest and
	// SetStateEntityClassForTest stand in for the bookkeeping a real SpawnRemoteEntity would have recorded.
	struct FCrowdyMassRowEventFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassRowEventFixture()
		{
			// The owner is not optional even though the constructor defaults it away: PostInitialize
			// registers relation types, whose observers are outered to exactly this owner, and a manager
			// built with none takes the process down with "Object is not packaged".
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassRowEventFixture()
		{
			EntityManager->Deinitialize();
		}

		// A tracked entity carrying the action fragment, recorded as being of RecordedClass. Pass null for
		// an entity nothing has said the class of yet.
		FMassEntityHandle MakeEntity(const FGuid& UUID, const UClass* RecordedClass)
		{
			const UScriptStruct* Fragments[] = { FCrowdyMassActionFragment::StaticStruct() };
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);

			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, RecordedClass);
			return Entity;
		}

		FCrowdyMassActionFragment* Action(const FMassEntityHandle Entity) const
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(Entity);
		}

		// The action id currently on an entity, or -1 when it carries no action fragment at all. A sentinel
		// rather than a dereference, so a fixture that ever stopped adding the fragment fails an assertion
		// instead of taking the whole suite down.
		int32 ActionId(const FMassEntityHandle Entity) const
		{
			const FCrowdyMassActionFragment* Fragment = Action(Entity);
			return Fragment ? Fragment->ActionId : -1;
		}
	};

	UFunction* PlayOneShotFunction()
	{
		return FCrowdyRPC::ResolveFunction(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayOneShot_Implementation"));
	}

	// Marshals a real call and runs the exact sequence the router performs: charge the allowance, and only
	// then decode the frame and apply it. Building an FCrowdyEventParams by hand instead would test the
	// handler against a frame no serializer ever produced.
	//
	// The charge step is part of the sequence rather than an optional extra. Calling only HandleEntityEvent
	// would spend nothing, so every allowance assertion in this file would hold whether or not anything
	// charged at all. Returns whether the call was delivered, since a refusal is now a real outcome here.
	bool DeliverPlayOneShot(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, const FGuid& UUID,
		int32 ActionId, int32 Variant, float PlayRate)
	{
		if (!Subsystem.ChargeEntityEventBudget(UUID, Fn))
		{
			return false;
		}

		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
			&ACrowdyMassRowEventTestActor::PlayOneShot_Implementation, ActionId, Variant, PlayRate);

		FCrowdyRPC::DecodeCall(Fn, Info, Call,
			[&Subsystem, &UUID, Fn](const FCrowdyEventParams& Params)
			{
				Subsystem.HandleEntityEvent(UUID, FGuid::NewGuid(), Fn, Params);
			});
		return true;
	}

	UFunction* DeclaredActionFunction()
	{
		return FCrowdyRPC::ResolveFunction(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayDeclaredAction_Implementation"));
	}

	UFunction* IdlessActionFunction()
	{
		return FCrowdyRPC::ResolveFunction(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayIdlessAction_Implementation"));
	}

	// The same charge-then-decode-then-apply sequence as DeliverPlayOneShot, against the declared-action
	// event. Written out rather than shared, because the marshalled member function pointer has to name the
	// exact signature being sent.
	bool DeliverDeclaredAction(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, const FGuid& UUID,
		int32 ActionId, int32 Variant, float PlayRate)
	{
		if (!Subsystem.ChargeEntityEventBudget(UUID, Fn))
		{
			return false;
		}

		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
			&ACrowdyMassRowEventTestActor::PlayDeclaredAction_Implementation, ActionId, Variant, PlayRate);

		FCrowdyRPC::DecodeCall(Fn, Info, Call,
			[&Subsystem, &UUID, Fn](const FCrowdyEventParams& Params)
			{
				Subsystem.HandleEntityEvent(UUID, FGuid::NewGuid(), Fn, Params);
			});
		return true;
	}

	bool DeliverIdlessAction(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, const FGuid& UUID,
		int32 Variant, float PlayRate)
	{
		if (!Subsystem.ChargeEntityEventBudget(UUID, Fn))
		{
			return false;
		}

		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
			&ACrowdyMassRowEventTestActor::PlayIdlessAction_Implementation, Variant, PlayRate);

		FCrowdyRPC::DecodeCall(Fn, Info, Call,
			[&Subsystem, &UUID, Fn](const FCrowdyEventParams& Params)
			{
				Subsystem.HandleEntityEvent(UUID, FGuid::NewGuid(), Fn, Params);
			});
		return true;
	}
}

/**
 * An event the author DECLARED an action starts one on an entity held as a row with nothing registered for
 * it at all.
 *
 * This is what removes the second authoring step for the ordinary case: the game declares the event once on
 * its own actor class, writes the body once for the owner, and a crowd entity reacts with no registration
 * call, no handler object and no Blueprint graph.
 *
 * Deleting the declared-action fall-through in HandleEntityEvent turns this red, and so does answering
 * false from HasEntityEventHandler for a declared action, since the router would then never decode it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredActionNeedsNoRegistrationTest,
	"CrowdySDK.Mass.DeclaredActionNeedsNoRegistration", CrowdyMassRowEventTestFlags)
bool FCrowdyMassDeclaredActionNeedsNoRegistrationTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = DeclaredActionFunction();
	if (!TestNotNull(TEXT("the declared action event resolved"), Fn))
	{
		return false;
	}

	// Asked before anything is delivered, because this is the answer the router reads to decide whether to
	// decode the call at all. A subsystem that only applied the action at HandleEntityEvent would never be
	// given one.
	TestTrue(TEXT("the door claims a declared action with nothing registered"),
		Fixture.Subsystem->HasEntityEventHandler(Fn));
	TestEqual(TEXT("and it claims it without anything having been registered"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 0);

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	TestTrue(TEXT("the call was delivered"),
		DeliverDeclaredAction(*Fixture.Subsystem, Fn, UUID, 7, 2, 1.5f));

	const FCrowdyMassActionFragment* Action = Fixture.Action(Entity);
	if (!TestNotNull(TEXT("the entity carries an action fragment"), Action))
	{
		return false;
	}

	TestEqual(TEXT("the action id landed"), Action->ActionId, 7);
	TestEqual(TEXT("the variant landed"), Action->Variant, 2);
	TestEqual(TEXT("the play rate landed"), Action->PlayRate, 1.5f);
	TestEqual(TEXT("and the retrigger serial moved exactly once"), Action->Serial, 1);

	return true;
}

/**
 * An event that carries an int32 called ActionId and was NOT declared an action starts nothing.
 *
 * The invariant behind the whole shape: which of a game's events mean "play an action" is the game's to
 * say. A plugin that recognised the parameter name instead would be deciding what a game's nouns mean, and
 * would start animations on events that merely happened to spell a parameter the same way.
 *
 * The two events differ in the marker and in nothing else, so making the fall-through key on the signature
 * rather than on the declaration turns this red while leaving every other case in this file green.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnActionIdAloneDeclaresNothingTest,
	"CrowdySDK.Mass.AnActionIdAloneDeclaresNothing", CrowdyMassRowEventTestFlags)
bool FCrowdyMassAnActionIdAloneDeclaresNothingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Undeclared = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the undeclared event resolved"), Undeclared))
	{
		return false;
	}

	// Stated rather than assumed: this event really does carry the parameter a name-based rule would fire
	// on, so its refusal below is about the missing declaration and not about a missing id.
	TestTrue(TEXT("the undeclared event does carry an int32 action id"),
		CrowdyMassDeclaresActionId(Undeclared));

	TestEqual(TEXT("and nothing is registered for it"),
		Fixture.Subsystem->NumEventHandlersRegistered(Undeclared), 0);

	// Asked with the dispatch proxy off, because with it on this plane claims every event: it can apply
	// any of them by running the author's own body on a stand-in. What this case is about is narrower and
	// unchanged by that, namely that an int32 called ActionId does not by itself make an event an action.
	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest ProxyOff;
		TestFalse(TEXT("and the door claims nothing for it as a declared action"),
			Fixture.Subsystem->HasEntityEventHandler(Undeclared));
	}

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	DeliverPlayOneShot(*Fixture.Subsystem, Undeclared, UUID, 7, 2, 1.5f);

	TestEqual(TEXT("and nothing was written"), Fixture.ActionId(Entity), 0);

	return true;
}

/**
 * A handler the game registered REPLACES the declared action rather than running alongside it.
 *
 * Handlers ordinarily coexist, and this one case cannot: two writers of one action slot bump the retrigger
 * serial twice for a single inbound event, which downstream is indistinguishable from the action having
 * been started again. A game that wants something other than the standard write must be able to say so by
 * registering, and get exactly its own behaviour.
 *
 * Both halves are asserted because either alone is weak: the id alone would pass if the default ran first
 * and the handler second, and the serial alone would pass if neither had run.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRegisteredHandlerReplacesDeclaredActionTest,
	"CrowdySDK.Mass.RegisteredHandlerReplacesTheDeclaredAction", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRegisteredHandlerReplacesDeclaredActionTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = DeclaredActionFunction();
	if (!TestNotNull(TEXT("the declared action event resolved"), Fn))
	{
		return false;
	}

	// Writes a different id from the one the call carries, so which of the two ran is readable off the
	// fragment rather than inferred.
	Fixture.Subsystem->RegisterEventHandler(Fn,
		[](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)
		{
			if (FCrowdyMassActionFragment* Action = Context.GetFragment<FCrowdyMassActionFragment>())
			{
				CrowdyMassWriteAction(*Action, 99, 0, 1.0f);
			}
		});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	TestTrue(TEXT("the call was delivered"),
		DeliverDeclaredAction(*Fixture.Subsystem, Fn, UUID, 7, 2, 1.5f));

	const FCrowdyMassActionFragment* Action = Fixture.Action(Entity);
	if (!TestNotNull(TEXT("the entity carries an action fragment"), Action))
	{
		return false;
	}

	TestEqual(TEXT("the registered handler's write is what landed"), Action->ActionId, 99);
	TestEqual(TEXT("and the retrigger serial moved once, so the default did not also write"),
		Action->Serial, 1);

	return true;
}

/**
 * The typed C++ send builds its frame from the event's reflected parameters, so each argument lands at its own
 * offset as its declared type, whatever the caller's literal was (an int literal for the float rate here).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedEventFrameTest,
	"CrowdySDK.Mass.TypedEventFramePlacesEachArgumentByItsParameter", CrowdyMassRowEventTestFlags)
bool FCrowdyMassTypedEventFrameTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = DeclaredActionFunction();
	if (!TestNotNull(TEXT("the declared action event resolved"), Fn))
	{
		return false;
	}

	int32 ActionId = 0;
	int32 Variant = 0;
	float PlayRate = 0.0f;
	const bool bUsed = FCrowdyRPC::WithCheckedFrame(Fn, &ACrowdyMassRowEventTestActor::PlayDeclaredAction_Implementation,
		[Fn, &ActionId, &Variant, &PlayRate](const void* Frame)
		{
			ActionId = *CastFieldChecked<FIntProperty>(Fn->FindPropertyByName(TEXT("ActionId")))->ContainerPtrToValuePtr<int32>(Frame);
			Variant = *CastFieldChecked<FIntProperty>(Fn->FindPropertyByName(TEXT("Variant")))->ContainerPtrToValuePtr<int32>(Frame);
			PlayRate = *CastFieldChecked<FFloatProperty>(Fn->FindPropertyByName(TEXT("PlayRate")))->ContainerPtrToValuePtr<float>(Frame);
			return true;
		},
		7, 2, 3);

	TestTrue(TEXT("the frame was handed over"), bUsed);
	TestEqual(TEXT("the action id is in its slot"), ActionId, 7);
	TestEqual(TEXT("the variant is in its slot"), Variant, 2);
	TestEqual(TEXT("the int literal arrived as the float rate"), PlayRate, 3.0f);
	return true;
}

/**
 * A Blueprint receive node bound to a declared action runs AND the action still plays.
 *
 * The node replaces the event's body on a row, but Blueprint has no way to start the action itself, so the
 * declared write stays. A C++ handler still replaces both (the case above).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundNodeKeepsDeclaredActionTest,
	"CrowdySDK.Mass.BoundReceiveNodeKeepsTheDeclaredAction", CrowdyMassRowEventTestFlags)
bool FCrowdyMassBoundNodeKeepsDeclaredActionTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = DeclaredActionFunction();
	if (!TestNotNull(TEXT("the declared action event resolved"), Fn))
	{
		return false;
	}

	UCrowdyMassBoundEventTestHandler* Handler = NewObject<UCrowdyMassBoundEventTestHandler>(GetTransientPackage());
	TestTrue(TEXT("the receive node's binding is accepted"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShot")).IsValid());

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	TestTrue(TEXT("the call was delivered"),
		DeliverDeclaredAction(*Fixture.Subsystem, Fn, UUID, 7, 2, 1.5f));

	TestEqual(TEXT("the bound graph ran once"), Handler->CallCount, 1);

	const FCrowdyMassActionFragment* Action = Fixture.Action(Entity);
	if (!TestNotNull(TEXT("the entity carries an action fragment"), Action))
	{
		return false;
	}

	TestEqual(TEXT("and the declared action still landed"), Action->ActionId, 7);
	TestEqual(TEXT("once"), Action->Serial, 1);

	return true;
}

/**
 * The action contract is described for an author, and each way of getting it wrong reads differently.
 *
 * This is what an editor asks at compile time, where the box has been ticked and the event has not run
 * yet. Runtime already reports an idless action, but only on the machine observing a crowd and only once
 * an event has actually arrived, which is a long way from the person who ticked the box.
 *
 * A single "this event is wrong" sentence would pass a test that only counted non-empty answers, so each
 * case pins the fact its own remedy turns on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionContractIsDescribedTest,
	"CrowdySDK.Mass.ActionContractIsDescribed", CrowdyMassRowEventTestFlags)
bool FCrowdyMassActionContractIsDescribedTest::RunTest(const FString& Parameters)
{
	const UClass* Target = UCrowdyMassActionSignatureTestTarget::StaticClass();

	const UFunction* WellFormed = Target->FindFunctionByName(TEXT("WellFormedAction"));
	const UFunction* NoId = Target->FindFunctionByName(TEXT("NoIdAction"));
	const UFunction* WrongType = Target->FindFunctionByName(TEXT("WrongTypedAction"));
	const UFunction* Output = Target->FindFunctionByName(TEXT("OutputIdAction"));

	if (!TestTrue(TEXT("all four signature fixtures resolved"),
		WellFormed && NoId && WrongType && Output))
	{
		return false;
	}

	// The half that keeps the check worth running: an event that can carry an action is not complained
	// about, so a warning in a compile log means something.
	TestTrue(TEXT("a well formed action event has nothing said about it"),
		CrowdyMassDescribeActionEventProblem(WellFormed).IsEmpty());

	const FString NoIdProblem = CrowdyMassDescribeActionEventProblem(NoId);
	TestTrue(TEXT("an event with no id at all names the parameter to add"),
		NoIdProblem.Contains(TEXT("no int32 'ActionId' input parameter")));
	TestTrue(TEXT("and says its body still runs where the entity is an actor, so the box is not read as broken"),
		NoIdProblem.Contains(TEXT("body still runs")));
	TestTrue(TEXT("and states the numbering once, where the author is looking"),
		NoIdProblem.Contains(TEXT("ids start at 1")));

	// The two cases where a parameter of the right name IS present. Told apart because their remedies are
	// different and neither is "add a parameter", which is the sentence above.
	const FString WrongTypeProblem = CrowdyMassDescribeActionEventProblem(WrongType);
	TestTrue(TEXT("a wrongly typed id names the type it actually has"),
		WrongTypeProblem.Contains(TEXT("declared as float")));
	TestFalse(TEXT("and is not told to add a parameter it already has"),
		WrongTypeProblem.Contains(TEXT("no int32 'ActionId' input parameter")));

	const FString OutputProblem = CrowdyMassDescribeActionEventProblem(Output);
	TestTrue(TEXT("an output id is named as an output rather than as a missing or mistyped one"),
		OutputProblem.Contains(TEXT("is an output")));
	TestFalse(TEXT("and is not reported as the wrong type, which it is not"),
		OutputProblem.Contains(TEXT("rather than an integer")));

	// The optional pair. An event that declares neither is fine and says nothing; one that declares an
	// unreadable one is the case with no other symptom, because reading nothing and reading the default
	// are the same value in the running game.
	const UFunction* BadVariant = Target->FindFunctionByName(TEXT("WrongTypedVariantAction"));
	const UFunction* BadRate = Target->FindFunctionByName(TEXT("WrongTypedPlayRateAction"));
	const UFunction* DoubleRate = Target->FindFunctionByName(TEXT("DoublePlayRateAction"));
	if (!TestTrue(TEXT("the optional-parameter fixtures resolved"), BadVariant && BadRate && DoubleRate))
	{
		return false;
	}

	const FString VariantProblem = CrowdyMassDescribeActionEventProblem(BadVariant);
	TestTrue(TEXT("an unreadable variant is reported even though the action itself plays"),
		VariantProblem.Contains(TEXT("'Variant' parameter is declared as float")));
	TestTrue(TEXT("and says what is seen instead, which is the value it is confusable with"),
		VariantProblem.Contains(TEXT("uses variant 0")));

	TestTrue(TEXT("an unreadable play rate is reported the same way"),
		CrowdyMassDescribeActionEventProblem(BadRate).Contains(TEXT("'PlayRate' parameter is declared as int32")));

	// The half that keeps the rate check from being written as "must be a float": a double is read, because
	// the handler reads the rate through GetFloat, which accepts either width.
	TestTrue(TEXT("a double play rate is read, so nothing is said about it"),
		CrowdyMassDescribeActionEventProblem(DoubleRate).IsEmpty());

	return true;
}

/**
 * An event declared an action but carrying no id is reported, and starts nothing.
 *
 * It cannot be answered by refusing to claim the event: that would drop it before decoding and leave the
 * mistake looking exactly like an event nobody sent. So it is claimed, delivered, and found to carry
 * nothing, which is the only point at which there is anything to say.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredActionWithNoIdStartsNothingTest,
	"CrowdySDK.Mass.DeclaredActionWithNoIdStartsNothing", CrowdyMassRowEventTestFlags)
bool FCrowdyMassDeclaredActionWithNoIdStartsNothingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = IdlessActionFunction();
	if (!TestNotNull(TEXT("the idless action event resolved"), Fn))
	{
		return false;
	}

	// The two halves of the question, asked separately, because the whole point is that they can disagree:
	// the author declared an action, and the signature cannot carry one.
	TestTrue(TEXT("the door still claims it, so the mistake is reachable"),
		Fixture.Subsystem->HasEntityEventHandler(Fn));
	TestFalse(TEXT("and the signature declares no action id"), CrowdyMassDeclaresActionId(Fn));

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	TestTrue(TEXT("the call was delivered"),
		DeliverIdlessAction(*Fixture.Subsystem, Fn, UUID, 2, 1.5f));

	TestEqual(TEXT("and no action was started"), Fixture.ActionId(Entity), 0);

	return true;
}

/**
 * A declared action aimed at an entity of the wrong kind is refused, exactly as a registered handler's
 * would be.
 *
 * The fall-through was added below the class guard rather than beside the handler lookup, and this is what
 * holds it there. A destination added at an early-out skips every check between it and the old path, which
 * is the shape this codebase has been bitten by before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredActionStillPassesTheClassGuardTest,
	"CrowdySDK.Mass.DeclaredActionStillPassesTheClassGuard", CrowdyMassRowEventTestFlags)
bool FCrowdyMassDeclaredActionStillPassesTheClassGuardTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = DeclaredActionFunction();
	if (!TestNotNull(TEXT("the declared action event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID,
		ACrowdyMassRowEventUnrelatedTestActor::StaticClass());

	TestFalse(TEXT("the allowance step refuses an event aimed at the wrong kind of entity"),
		DeliverDeclaredAction(*Fixture.Subsystem, Fn, UUID, 7, 2, 1.5f));

	TestEqual(TEXT("and nothing was written"), Fixture.ActionId(Entity), 0);

	return true;
}

/**
 * The claim the whole re-key exists for: an event declared on a game's own ACTOR class, applied to an
 * entity that has no object at all, writing the same action fragment the avatar-declared event writes.
 *
 * Mutating CrowdyMassWriteAction's clamps or its serial bump turns this red; so does mutating
 * CrowdyMassMakeActionEventHandler's parameter names, since the values would stop landing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventHandlerWritesActionFragmentTest,
	"CrowdySDK.Mass.RowEventHandlerWritesActionFragment", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventHandlerWritesActionFragmentTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	TestFalse(TEXT("nothing is claimed before a handler is registered"),
		Fixture.Subsystem->IsEventHandlerRegistered(Fn));

	Fixture.Subsystem->RegisterEventHandler(Fn, CrowdyMassMakeActionEventHandler());

	TestTrue(TEXT("the function is claimed once a handler is registered"),
		Fixture.Subsystem->IsEventHandlerRegistered(Fn));
	TestTrue(TEXT("and the router asks the same question through the interface"),
		Fixture.Subsystem->HasEntityEventHandler(Fn));

	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 7, 3, 1.5f);

	FCrowdyMassActionFragment* Action = Fixture.Action(Entity);
	if (!TestNotNull(TEXT("the entity carries an action fragment"), Action))
	{
		return false;
	}

	TestEqual(TEXT("the action id landed"), Action->ActionId, 7);
	TestEqual(TEXT("the variant landed"), Action->Variant, 3);
	TestEqual(TEXT("the play rate landed"), Action->PlayRate, 1.5f);
	TestEqual(TEXT("the retrigger serial was bumped"), Action->Serial, 1);
	TestEqual(TEXT("and the trigger reads as outstanding to the animation pass"), Action->AppliedSerial, 0);

	// Every value came off the wire, so the clamps are the only thing standing between a peer and a frozen
	// or absurd animation. A variant far outside the authored range and a non-finite rate are what a forged
	// call looks like.
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 9, 1000000, std::numeric_limits<float>::infinity());

	TestEqual(TEXT("a forged variant is clamped to the authored range"),
		Action->Variant, CrowdyMassMaxActionVariant);
	TestEqual(TEXT("a non-finite play rate falls back to the ordinary one"), Action->PlayRate, 1.0f);
	TestEqual(TEXT("and the retrigger serial moved again"), Action->Serial, 2);

	// Zero is the fragment's own "nothing is playing" value, so an event carrying it must not be able to
	// cancel an action rather than start one.
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 0, 0, 1.0f);
	TestEqual(TEXT("an id of zero writes nothing at all"), Action->ActionId, 9);
	TestEqual(TEXT("and does not move the serial"), Action->Serial, 2);

	return true;
}

/**
 * The authoring guard: an event is applied only to an entity this client has recorded as the kind the
 * event was declared for. It is not a security boundary, since the recorded class traces back to what the
 * entity's own sender said it was; it catches an event aimed at the wrong kind of entity.
 *
 * The derived case is the control that stops the guard closing the gate outright. A guard that demanded an
 * exact class would refuse every game whose players derive from a shared base, and would read exactly like
 * a guard that works.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventRecordedClassDecidesTest,
	"CrowdySDK.Mass.RowEventRecordedClassDecides", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventRecordedClassDecidesTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	Fixture.Subsystem->RegisterEventHandler(Fn, CrowdyMassMakeActionEventHandler());

	// Recorded as a subclass of the declaring class: the ordinary case, and it must land.
	const FGuid DerivedUUID = FGuid::NewGuid();
	const FMassEntityHandle Derived =
		Fixture.MakeEntity(DerivedUUID, ACrowdyMassRowEventDerivedTestActor::StaticClass());
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, DerivedUUID, 5, 0, 1.0f);
	TestEqual(TEXT("an entity recorded as a subclass of the declaring class is a legitimate target"),
		Fixture.ActionId(Derived), 5);

	// Recorded as something unrelated: the event was authored for a different kind of entity.
	const FGuid UnrelatedUUID = FGuid::NewGuid();
	const FMassEntityHandle Unrelated = Fixture.MakeEntity(UnrelatedUUID, AActor::StaticClass());
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UnrelatedUUID, 5, 0, 1.0f);
	TestEqual(TEXT("an entity recorded as an unrelated class is refused"),
		Fixture.ActionId(Unrelated), 0);

	// Recorded as nothing at all: this client has not been told what the entity is, so it is not yet known
	// to be the kind the event was declared for. Ordinary and temporary, not a fault.
	const FGuid UnknownUUID = FGuid::NewGuid();
	const FMassEntityHandle Unknown = Fixture.MakeEntity(UnknownUUID, nullptr);
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UnknownUUID, 5, 0, 1.0f);
	TestEqual(TEXT("an entity with no recorded class is refused"), Fixture.ActionId(Unknown), 0);

	// An id nothing tracks reaches nothing and mints nothing.
	const FGuid UntrackedUUID = FGuid::NewGuid();
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UntrackedUUID, 5, 0, 1.0f);
	TestTrue(TEXT("an untracked id is still untracked afterwards"),
		Fixture.Subsystem->GetAvatarState(UntrackedUUID) == ECrowdyMassAvatarState::NoEntity);

	return true;
}

/**
 * One inbound call costs the entity one token, whichever route carries it.
 *
 * The two routes are tried in order, so the object route sees every actor-class call before refusing it.
 * If it charged on the way to that refusal, every such call would be billed twice and an entity's whole
 * allowance would be spent at half the rate the ceiling says. Mutating ResolveReceiver's CanAvatarReceive
 * guard away (so it charges before GetOrCreateAvatar refuses) turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventIsChargedExactlyOnceTest,
	"CrowdySDK.Mass.RowEventIsChargedExactlyOnce", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventIsChargedExactlyOnceTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	// Held indirectly so the handler the subsystem stores does not outlive a stack local.
	const TSharedRef<int32> Delivered = MakeShared<int32>(0);
	Fixture.Subsystem->RegisterEventHandler(Fn,
		[Delivered](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)
		{
			++(*Delivered);
		});

	// The object route, asked exactly as the router asks it. No avatar class derives a game's actor class,
	// so this is the refusal every actor-class call takes on the way to the handler.
	constexpr int32 ObjectRouteAttempts = 40;
	int32 ObjectRouteReceivers = 0;
	for (int32 Index = 0; Index < ObjectRouteAttempts; ++Index)
	{
		ObjectRouteReceivers += Fixture.Subsystem->ResolveReceiver(UUID, Fn) ? 1 : 0;
	}

	TestEqual(TEXT("no avatar can receive a function declared on an actor class"), ObjectRouteReceivers, 0);
	TestTrue(TEXT("and nothing was minted on the way to that answer"),
		Fixture.Subsystem->GetAvatarState(UUID) == ECrowdyMassAvatarState::NotMinted);

	// A full burst, delivered through the route that does carry the call. Every one must land: the refusals
	// above spent nothing, so the whole allowance is still there.
	const int32 Burst = static_cast<int32>(CrowdyMassReceiveBurstEvents);
	for (int32 Index = 0; Index < Burst; ++Index)
	{
		DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);
	}

	TestEqual(TEXT("a full burst lands after the object route refused the same number of calls"),
		*Delivered, Burst);

	// The half this case is actually named for, and without it the whole test holds whether or not anything
	// is charged at all: an allowance nothing spends is indistinguishable from an allowance nobody reached.
	// One call past the burst must be refused, and it must be refused BEFORE the frame is decoded, which is
	// what the delivery helper's return value reports.
	const bool bPastBurstDelivered = DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);
	TestFalse(TEXT("the call past the burst is refused rather than applied"), bPastBurstDelivered);
	TestEqual(TEXT("and nothing more reached the handler"), *Delivered, Burst);

	return true;
}

/**
 * A function nothing is registered for must not be claimed, because claiming it is what stops the router
 * reporting a missing registration. Nothing is registered for the avatar-declared events either, which is
 * the point: those reach their avatar through the object route and never come here.
 *
 * CONDITIONAL on the dispatch proxy since 2026-08-23. With the proxy on there IS no unregistered event
 * this plane cannot apply, because it applies one by running the author's own body on a stand-in, so the
 * missing-registration report the claim used to suppress no longer describes anything. The claim is asked
 * here with the proxy off, which is the shape this case was written for; the half below it, that a
 * delivered call with nothing registered writes nothing, holds either way and is asked either way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventUnregisteredFunctionIsNotClaimedTest,
	"CrowdySDK.Mass.RowEventUnregisteredFunctionIsNotClaimed", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventUnregisteredFunctionIsNotClaimedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	UFunction* AvatarFn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn)
		|| !TestNotNull(TEXT("the avatar's event resolved"), AvatarFn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest ProxyOff;
		TestFalse(TEXT("an unregistered actor function is not claimed"),
			Fixture.Subsystem->HasEntityEventHandler(Fn));
		TestFalse(TEXT("nor is an event declared on the avatar class, which travels the object route"),
			Fixture.Subsystem->HasEntityEventHandler(AvatarFn));
	}

	// Not conditional on anything: there is no function to run a body for, with the proxy on or off.
	TestFalse(TEXT("nor is nothing at all"), Fixture.Subsystem->HasEntityEventHandler(nullptr));

	// Delivered anyway, as a router with a stale idea of what is registered would: nothing may be written.
	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 5, 0, 1.0f);
	TestEqual(TEXT("and an unregistered function writes nothing even if it is delivered"),
		Fixture.ActionId(Entity), 0);

	// The avatar route is unaffected by any of this: its own event still resolves an avatar.
	TestNotNull(TEXT("an avatar-declared event still resolves its avatar"),
		Fixture.Subsystem->ResolveReceiver(UUID, AvatarFn));

	return true;
}

/**
 * One event has more than one meaning, so handlers COEXIST: registering the same function again adds a
 * second handler and both run. Writing a fragment so the entity is drawn flinching and printing a line are
 * two different jobs on one event, and neither is a variant of the other.
 *
 * The unregister half is the same case from the other side. Without it, coexisting handlers would leave a
 * C++ caller no way at all to take one back off, since re-registering no longer replaces anything.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventHandlersCoexistTest,
	"CrowdySDK.Mass.RowEventHandlersCoexist", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventHandlersCoexistTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	const TSharedRef<int32> First = MakeShared<int32>(0);
	const TSharedRef<int32> Second = MakeShared<int32>(0);

	const FCrowdyMassEventHandlerHandle FirstHandle = Fixture.Subsystem->RegisterEventHandler(Fn,
		[First](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context) { ++(*First); });
	const FCrowdyMassEventHandlerHandle SecondHandle = Fixture.Subsystem->RegisterEventHandler(Fn,
		[Second](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context) { ++(*Second); });

	TestTrue(TEXT("registering hands back a handle"), FirstHandle.IsValid() && SecondHandle.IsValid());
	TestEqual(TEXT("both handlers are registered for the one function"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 2);

	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);

	TestEqual(TEXT("the handler registered first runs"), *First, 1);
	TestEqual(TEXT("and so does the one registered after it"), *Second, 1);

	// Removing one leaves the other running, which is what says the removal named a handler rather than
	// emptying the list.
	TestTrue(TEXT("the first handler is removed"), Fixture.Subsystem->UnregisterEventHandler(FirstHandle));
	TestFalse(TEXT("and removing it twice reports that there was nothing left to remove"),
		Fixture.Subsystem->UnregisterEventHandler(FirstHandle));

	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);

	TestEqual(TEXT("the removed handler does not run again"), *First, 1);
	TestEqual(TEXT("and the one still registered does"), *Second, 2);

	TestTrue(TEXT("the last handler is removed"), Fixture.Subsystem->UnregisterEventHandler(SecondHandle));
	TestEqual(TEXT("and nothing is registered for the function any more"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 0);

	// The claim itself is asked with the dispatch proxy off: with it on this plane claims every event,
	// because it can apply one by running the author's own body on a stand-in whether or not a handler was
	// ever registered.
	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest ProxyOff;
		TestFalse(TEXT("and the function is no longer claimed at all"),
			Fixture.Subsystem->HasEntityEventHandler(Fn));
	}

	return true;
}

/**
 * A handler may register and unregister from inside a handler that is already running, which is the
 * ordinary shape of a one-shot binding that takes itself back off.
 *
 * Two claims, and the second is the one worth the test: the handler doing the removing runs to its own
 * return, and the one it added does not run for the call already being delivered. A walk over the live
 * list rather than a copy of it turns the second red, and a removal that destroyed the entry rather than
 * dropping it from the list turns the first into a crash.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventHandlerMayEditTheListTest,
	"CrowdySDK.Mass.RowEventHandlerMayEditTheList", CrowdyMassRowEventTestFlags)
bool FCrowdyMassRowEventHandlerMayEditTheListTest::RunTest(const FString& Parameters)
{
	FCrowdyMassRowEventFixture Fixture;

	UFunction* Fn = PlayOneShotFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	const TSharedRef<int32> SelfRemoving = MakeShared<int32>(0);
	const TSharedRef<int32> AddedFromInside = MakeShared<int32>(0);
	const TSharedRef<int32> RanToCompletion = MakeShared<int32>(0);
	const TSharedRef<FCrowdyMassEventHandlerHandle> OwnHandle = MakeShared<FCrowdyMassEventHandlerHandle>();

	UCrowdyMassEntitySubsystem* Subsystem = Fixture.Subsystem;
	*OwnHandle = Subsystem->RegisterEventHandler(Fn,
		[Subsystem, Fn, SelfRemoving, AddedFromInside, RanToCompletion, OwnHandle](
			const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)
		{
			++(*SelfRemoving);
			Subsystem->UnregisterEventHandler(*OwnHandle);
			Subsystem->RegisterEventHandler(Fn,
				[AddedFromInside](const FCrowdyEventParams&, const FCrowdyMassViewStateContext&)
				{
					++(*AddedFromInside);
				});

			// Read back after both edits: reaching this line at all is what says the entry was not freed
			// underneath the call that removed it.
			++(*RanToCompletion);
		});

	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);

	TestEqual(TEXT("the handler ran"), *SelfRemoving, 1);
	TestEqual(TEXT("and ran past its own removal"), *RanToCompletion, 1);
	TestEqual(TEXT("a handler registered mid-dispatch does not run for the call being delivered"),
		*AddedFromInside, 0);

	DeliverPlayOneShot(*Fixture.Subsystem, Fn, UUID, 1, 0, 1.0f);

	TestEqual(TEXT("the self-removed handler does not run for the next call"), *SelfRemoving, 1);
	TestEqual(TEXT("and the one it registered does"), *AddedFromInside, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
