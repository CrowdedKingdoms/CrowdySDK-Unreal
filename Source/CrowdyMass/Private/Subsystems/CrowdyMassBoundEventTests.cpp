#include "Mass/CrowdyMassRowEventTestTypes.h"

void UCrowdyMassBoundEventTestHandler::OnPlayOneShot(const FCrowdyMassEntityTarget Target, const int32 ActionId,
	const int32 Variant, const float PlayRate)
{
	++CallCount;
	GotEntity = Target.Entity;
	GotActionId = ActionId;
	GotVariant = Variant;
	GotPlayRate = PlayRate;
}

void UCrowdyMassBoundEventTestHandler::OnPlayOneShotSwapped(const FCrowdyMassEntityTarget Target, const int32 Variant,
	const int32 ActionId, const float PlayRate)
{
	++CallCount;
	GotEntity = Target.Entity;
	GotActionId = ActionId;
	GotVariant = Variant;
	GotPlayRate = PlayRate;
}

void UCrowdyMassBoundEventTestHandler::OnPlayOneShotPartial(const FCrowdyMassEntityTarget Target, const float ActionId,
	const int32 Variant, const int32 Absent)
{
	++CallCount;
	GotEntity = Target.Entity;
	GotActionIdAsFloat = ActionId;
	GotVariant = Variant;
	GotAbsent = Absent;
}

void UCrowdyMassBoundEventTestHandler::OnPlayOneShotNoTarget(const int32 ActionId)
{
	++CallCount;
	GotActionId = ActionId;
}

void UCrowdyMassBoundEventTestHandler::OnPlayOneShotWithOutput(const FCrowdyMassEntityTarget Target, int32& OutActionId)
{
	++CallCount;
	OutActionId = 0;
}

// Also gated on !UE_BUILD_SHIPPING, not because a test would run there, but because these cases construct
// CrowdyMassDispatchProxy::FScopedDisableForTest, which only exists where the CVar-backed flag it toggles
// does. The two conditions coincide unless a build forces automation tests into a Shipping target.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "Misc/AutomationTest.h"

#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Events/CrowdyMassEventLibrary.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Templates/SharedPointer.h"

/**
 * Covers the observer's half of an event reaching a Blueprint: a handler bound to an object's reflected
 * function rather than registered from C++. What a binding accepts, what it fills and what it deliberately
 * leaves alone, and when it stops being callable at all.
 *
 * The K2 node this exists for is editor-only and compiles a graph down to exactly these calls, so what is
 * exercised here is everything about the feature that survives into a packaged game.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyMassBoundEventTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An int32 whose bits read as 3.14159f. It exists so that a copy made WITHOUT the type check can be
	// told from one that was refused: a plain byte copy of a small integer into a float lands within the
	// default float tolerance of zero, so an ordinary action id would make the refusal untestable.
	constexpr int32 CrowdyMassBoundEventFloatBitPatternId = 1078523331;

	// A standalone Mass entity manager with no World, mirroring the fixture the C++-handler tests use. A
	// separate type rather than a shared one because both are file-local, and two anonymous namespaces in
	// one module must not declare the same name.
	struct FCrowdyMassBoundEventFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassBoundEventFixture()
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

		~FCrowdyMassBoundEventFixture()
		{
			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeEntity(const FGuid& UUID, const UClass* RecordedClass)
		{
			const UScriptStruct* Fragments[] = { FCrowdyMassActionFragment::StaticStruct() };
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);

			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, RecordedClass);
			return Entity;
		}
	};

	UFunction* BoundEventTestFunction()
	{
		return FCrowdyRPC::ResolveFunction(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayOneShot_Implementation"));
	}

	UCrowdyMassBoundEventTestHandler* MakeBoundEventTestHandler()
	{
		return NewObject<UCrowdyMassBoundEventTestHandler>(GetTransientPackage());
	}

	// Marshals a real call and runs the sequence the router performs: charge the allowance, then decode the
	// frame and apply it. Building an FCrowdyEventParams by hand instead would test the binding against a
	// frame no serializer ever produced, which is exactly where a by-name read can look right and be wrong.
	bool DeliverBoundPlayOneShot(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, const FGuid& UUID,
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
}

/**
 * The claim the node exists for: an event declared on a game's own actor class reaches a reflected function
 * on an ordinary object, with the entity it arrived for and the event's own values.
 *
 * Mutating the target write out of the dispatch turns the entity assertion red; mutating the copy table or
 * the ProcessEvent call turns the rest red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventReachesObjectTest,
	"CrowdySDK.Mass.BoundEventReachesObject", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventReachesObjectTest::RunTest(const FString& Parameters)
{
	FCrowdyMassBoundEventFixture Fixture;

	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	UCrowdyMassBoundEventTestHandler* Handler = MakeBoundEventTestHandler();

	// Asked with the dispatch proxy off: with it on this plane claims every event, because it can apply one
	// by running the author's own body on a stand-in whether or not anything was bound. What the binding
	// changes, and what this case is about, is unchanged by that.
	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest ProxyOff;
		TestFalse(TEXT("nothing is claimed before anything is bound"),
			Fixture.Subsystem->HasEntityEventHandler(Fn));
	}

	const FCrowdyMassEventHandlerHandle Bound =
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShot"));

	TestTrue(TEXT("binding a well-shaped handler is accepted"), Bound.IsValid());
	TestTrue(TEXT("and the function is claimed once it is bound"),
		Fixture.Subsystem->HasEntityEventHandler(Fn));

	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 7, 3, 1.5f);

	TestEqual(TEXT("the handler ran once"), Handler->CallCount, 1);
	TestTrue(TEXT("and was told which entity the call arrived for"), Handler->GotEntity == Entity);
	TestEqual(TEXT("the action id landed"), Handler->GotActionId, 7);
	TestEqual(TEXT("the variant landed"), Handler->GotVariant, 3);
	TestEqual(TEXT("the play rate landed"), Handler->GotPlayRate, 1.5f);

	// A second call is a second run, not a one-shot: the binding stands until it is taken back off.
	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 9, 0, 1.0f);
	TestEqual(TEXT("the binding still stands for the next call"), Handler->CallCount, 2);
	TestEqual(TEXT("carrying the new call's values"), Handler->GotActionId, 9);

	return true;
}

/**
 * Parameters are matched BY NAME and only where both sides declare exactly the same type.
 *
 * Two halves, and each is a different mutation. The swapped handler declares the event's first two
 * parameters the other way round: by name each still gets its own value, and a copy made by position would
 * trade them, so dropping the name lookup turns the first half red. The partial handler declares one
 * parameter retyped and one the event does not carry at all: dropping the type check fills the retyped one
 * with reinterpreted bytes, so the second half is what a missing FProperty::SameType turns red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventMatchesParametersByNameTest,
	"CrowdySDK.Mass.BoundEventMatchesParametersByName", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventMatchesParametersByNameTest::RunTest(const FString& Parameters)
{
	FCrowdyMassBoundEventFixture Fixture;

	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	UCrowdyMassBoundEventTestHandler* Swapped = MakeBoundEventTestHandler();
	UCrowdyMassBoundEventTestHandler* Partial = MakeBoundEventTestHandler();

	TestTrue(TEXT("the swapped handler binds"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Swapped, TEXT("OnPlayOneShotSwapped")).IsValid());
	TestTrue(TEXT("the partial handler binds"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Partial, TEXT("OnPlayOneShotPartial")).IsValid());
	TestEqual(TEXT("both bindings are registered for the one event"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 2);

	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, CrowdyMassBoundEventFloatBitPatternId, 3, 1.5f);

	TestEqual(TEXT("a handler that declares the parameters in another order still gets its own action id"),
		Swapped->GotActionId, CrowdyMassBoundEventFloatBitPatternId);
	TestEqual(TEXT("and its own variant, rather than the two trading places"), Swapped->GotVariant, 3);

	TestEqual(TEXT("the partial handler ran"), Partial->CallCount, 1);
	TestEqual(TEXT("the parameter it declares with the same name and type landed"), Partial->GotVariant, 3);

	// Compared exactly rather than within a tolerance: a byte copy of the action id above lands on 3.14,
	// which a tolerant comparison against zero would still accept for a small id.
	TestTrue(TEXT("a parameter retyped since the handler was written is not filled from the wrong property"),
		Partial->GotActionIdAsFloat == 0.0f);
	TestEqual(TEXT("and a parameter the event does not declare keeps its own default"), Partial->GotAbsent, 0);

	return true;
}

/**
 * What a binding refuses, and that each refusal leaves nothing behind.
 *
 * The accepted case at the end is the control that stops the guards closing the gate outright: a set of
 * refusals with no shape that passes reads exactly like a binding that never works.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventRefusesWrongShapeTest,
	"CrowdySDK.Mass.BoundEventRefusesWrongShape", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventRefusesWrongShapeTest::RunTest(const FString& Parameters)
{
	FCrowdyMassBoundEventFixture Fixture;

	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	UCrowdyMassBoundEventTestHandler* Handler = MakeBoundEventTestHandler();

	TestFalse(TEXT("a handler whose first parameter is not the entity target is refused"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShotNoTarget")).IsValid());
	TestFalse(TEXT("a handler with an output parameter is refused"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShotWithOutput")).IsValid());
	TestFalse(TEXT("a function name the object does not declare is refused"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("NoSuchFunction")).IsValid());
	TestFalse(TEXT("no object at all is refused"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, nullptr, TEXT("OnPlayOneShot")).IsValid());
	TestFalse(TEXT("no event at all is refused"),
		Fixture.Subsystem->RegisterObjectEventHandler(nullptr, Handler, TEXT("OnPlayOneShot")).IsValid());

	TestEqual(TEXT("and none of those left a registration behind"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 0);

	TestTrue(TEXT("the shape the node actually produces is accepted"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShot")).IsValid());
	TestEqual(TEXT("leaving exactly one registration"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 1);

	return true;
}

/**
 * A binding stops meaning anything when its object goes, and it can be taken back off before that.
 *
 * The dead-object half is the one worth the test: the object is still in memory and its function is still
 * reflected, so nothing about the call would fail. Holding the object strongly, or dropping the validity
 * check, runs a handler on a destroyed object and turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventStopsWithItsObjectTest,
	"CrowdySDK.Mass.BoundEventStopsWithItsObject", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventStopsWithItsObjectTest::RunTest(const FString& Parameters)
{
	FCrowdyMassBoundEventFixture Fixture;

	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	UCrowdyMassBoundEventTestHandler* Unbound = MakeBoundEventTestHandler();
	TestTrue(TEXT("the handler binds"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Unbound, TEXT("OnPlayOneShot")).IsValid());

	// Bound again with the same object and name: the same binding, not a second one, so one call still runs
	// the handler once.
	TestTrue(TEXT("binding the same object and function again is accepted"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Unbound, TEXT("OnPlayOneShot")).IsValid());
	TestEqual(TEXT("and does not add a second registration"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 1);

	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 7, 0, 1.0f);
	TestEqual(TEXT("one call runs the handler once"), Unbound->CallCount, 1);

	TestTrue(TEXT("the binding is removed"),
		Fixture.Subsystem->UnregisterObjectEventHandler(Fn, Unbound, TEXT("OnPlayOneShot")));
	TestFalse(TEXT("and removing it again reports there was nothing left to remove"),
		Fixture.Subsystem->UnregisterObjectEventHandler(Fn, Unbound, TEXT("OnPlayOneShot")));

	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 8, 0, 1.0f);
	TestEqual(TEXT("an unbound handler does not run again"), Unbound->CallCount, 1);

	// The other way a binding ends: the object it names is destroyed while the binding is still in place.
	UCrowdyMassBoundEventTestHandler* Dying = MakeBoundEventTestHandler();
	TestTrue(TEXT("a second handler binds"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Dying, TEXT("OnPlayOneShot")).IsValid());

	Dying->MarkAsGarbage();

	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest ProxyOff;
		TestFalse(TEXT("a binding whose object is destroyed no longer claims the event"),
			Fixture.Subsystem->HasEntityEventHandler(Fn));
	}

	// Delivered anyway, as a router with a stale idea of what is registered would.
	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 9, 0, 1.0f);

	TestEqual(TEXT("and nothing is run on the destroyed object"), Dying->CallCount, 0);

	// Asked through the unregister rather than through the count, because the count already skips a dead
	// binding: it reads zero whether the entry was dropped or is still sitting there being refused. The
	// unregister still finds an entry that is only dead, so a false here is the entry genuinely being gone.
	TestFalse(TEXT("and the dead binding was dropped on the way, not left to be refused for every call"),
		Fixture.Subsystem->UnregisterObjectEventHandler(Fn, Dying, TEXT("OnPlayOneShot")));

	return true;
}

/**
 * The bind refuses an event whose signature is no longer the one the graph was compiled against, because
 * the node's output pins were grown from that signature and some of them would otherwise sit at their
 * defaults for the rest of the session with nothing to say they had stopped being filled.
 *
 * Both sources are compared: the same call with the function's own id is the control, so a guard that
 * refused everything would fail here rather than read as working.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventSignatureGuardTest,
	"CrowdySDK.Mass.BoundEventSignatureGuard", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventSignatureGuardTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	UCrowdyMassEventLibrary::ResetRefusalLogThrottleForTest();

	const int64 CurrentSignature = FCrowdyRPC::ComputeFunctionID(Fn);

	TestTrue(TEXT("the event resolves when the graph's signature still matches it"),
		UCrowdyMassEventLibrary::ResolveBoundEvent(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayOneShot_Implementation"), CurrentSignature) == Fn);

	AddExpectedErrorPlain(TEXT("does not match the signature this graph was compiled against"),
		EAutomationExpectedErrorFlags::Contains, 1);

	TestNull(TEXT("and is refused when the graph was compiled against a different one"),
		UCrowdyMassEventLibrary::ResolveBoundEvent(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("PlayOneShot_Implementation"), CurrentSignature + 1));

	// A reflected function that exists and simply is not a Crowdy event. Without this case, "not an event"
	// and "no such function" are the same refusal and neither is tested.
	UFunction* NotAnEvent = ACrowdyMassRowEventTestActor::StaticClass()->FindFunctionByName(TEXT("NotACrowdyEvent"));
	if (!TestNotNull(TEXT("the plain reflected function exists"), NotAnEvent))
	{
		return false;
	}

	TestNull(TEXT("a function that is not a Crowdy event is refused even with a matching signature"),
		UCrowdyMassEventLibrary::ResolveBoundEvent(ACrowdyMassRowEventTestActor::StaticClass(),
			TEXT("NotACrowdyEvent"), FCrowdyRPC::ComputeFunctionID(NotAnEvent)));

	return true;
}

/**
 * A bound handler and a C++ handler for one event both run, which is the split the whole change is for:
 * writing a fragment so the entity is drawn flinching, and printing a line, are two different jobs on one
 * event and neither is a variant of the other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBoundEventCoexistsWithCppHandlerTest,
	"CrowdySDK.Mass.BoundEventCoexistsWithCppHandler", CrowdyMassBoundEventTestFlags)
bool FCrowdyMassBoundEventCoexistsWithCppHandlerTest::RunTest(const FString& Parameters)
{
	FCrowdyMassBoundEventFixture Fixture;

	UFunction* Fn = BoundEventTestFunction();
	if (!TestNotNull(TEXT("the actor's event resolved"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassRowEventTestActor::StaticClass());

	const TSharedRef<int32> CppRuns = MakeShared<int32>(0);
	Fixture.Subsystem->RegisterEventHandler(Fn,
		[CppRuns](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context) { ++(*CppRuns); });

	UCrowdyMassBoundEventTestHandler* Handler = MakeBoundEventTestHandler();
	TestTrue(TEXT("the object binding is accepted alongside the C++ handler"),
		Fixture.Subsystem->RegisterObjectEventHandler(Fn, Handler, TEXT("OnPlayOneShot")).IsValid());

	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UUID, 7, 0, 1.0f);

	TestEqual(TEXT("the C++ handler ran"), *CppRuns, 1);
	TestEqual(TEXT("and so did the object binding"), Handler->CallCount, 1);

	// An event aimed at the wrong kind of entity reaches neither of them: the authoring guard is asked once
	// for the call rather than once per handler, and a binding must not be a way around it.
	const FGuid UnrelatedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(UnrelatedUUID, AActor::StaticClass());
	DeliverBoundPlayOneShot(*Fixture.Subsystem, Fn, UnrelatedUUID, 7, 0, 1.0f);

	TestEqual(TEXT("an entity recorded as an unrelated class reaches the C++ handler no more than before"),
		*CppRuns, 1);
	TestEqual(TEXT("nor the object binding"), Handler->CallCount, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
