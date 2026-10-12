// Also gated on !UE_BUILD_SHIPPING, not because a test would run there, but because these cases construct
// CrowdyMassDispatchProxy::FScopedDisableForTest, which only exists where the CVar-backed flag it toggles
// does. The two conditions coincide unless a build forces automation tests into a Shipping target.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dev/CrowdyMassDispatchProxyTestTypes.h"
#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassRowEventTestTypes.h"
#include "MassCommonFragments.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Target/CrowdyMassEntityTargetTestTypes.h"
#include "Templates/SharedPointer.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/Script.h"

/**
 * Tier 1 of author-once RPC delivery: an inbound event for an entity this client holds as a ROW runs the
 * event's own body, the one the author wrote on their actor class, on a per-class stand-in.
 *
 * The prototype cases in CrowdyMassDispatchProxyProbeTests.cpp measure what a never-spawned actor IS.
 * These measure what the shipped receive path DOES with one: that the memo is per class, that an event
 * arriving through the ordinary charge-decode-apply sequence reaches the body at the entity's own
 * position, that nothing carries from one entity to the next, and that a game which registered its own
 * handler still gets only that.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyMassProxyDispatchTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/**
	 * An Editor world plus a standalone entity manager, and the subsystem outered to that world so its
	 * proxies have a persistent level to belong to.
	 *
	 * The script execution guard stands in for the one thing an Editor world cannot supply: AActor's
	 * ProcessEvent runs a body only where the world reports its actors initialized, which a played level
	 * does and a bare test world never does. A Game world is not the answer instead, because this project's
	 * play-gated world subsystems assert on teardown against an entity manager a test world never built.
	 */
	struct FCrowdyMassProxyDispatchFixture
	{
		UWorld* World = nullptr;
		FEditorScriptExecutionGuard ScriptGuard;
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassProxyDispatchFixture()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);

			// The owner is not optional even though the constructor defaults it away: PostInitialize
			// registers relation types whose observers are outered to exactly this owner.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			// Outered to the world, not to the transient package, because that is what UWorldSubsystem
			// answers GetWorld with and a proxy is outered to that world's persistent level.
			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(World);
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassProxyDispatchFixture()
		{
			EntityManager->Deinitialize();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		// A tracked entity carrying a transform and an action fragment, recorded as being of RecordedClass.
		FMassEntityHandle MakeEntity(const FGuid& UUID, const UClass* RecordedClass, const FVector& Location)
		{
			const UScriptStruct* Fragments[] =
			{
				FTransformFragment::StaticStruct(),
				FCrowdyMassActionFragment::StaticStruct(),
			};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 2));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);

			if (FTransformFragment* Transform = EntityManager->GetFragmentDataPtr<FTransformFragment>(Entity))
			{
				Transform->GetMutableTransform().SetLocation(Location);
			}

			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, RecordedClass);
			return Entity;
		}
	};

	UFunction* ProxyPresentFunction()
	{
		return FCrowdyRPC::ResolveFunction(ACrowdyMassDispatchProxyTestActor::StaticClass(),
			TEXT("ProxyPresent_Implementation"));
	}

	// The decode-and-apply half of the router's sequence, with no allowance charged. This is the direct
	// caller HandleEntityEvent is public for, and the only way to reach the dispatch with a call the
	// allowance step declines for free.
	//
	// The receiver's declaring class is a template parameter rather than fixed, because MarshalCall
	// addresses a receiver by member-function pointer and which class declares it is precisely the thing
	// the refusal case varies.
	template <typename C>
	void ApplyProxyIntEvent(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, void (C::*MemberFn)(int32),
		const FGuid& UUID, int32 Value)
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info, MemberFn, Value);

		FCrowdyRPC::DecodeCall(Fn, Info, Call,
			[&Subsystem, &UUID, Fn](const FCrowdyEventParams& Params)
			{
				Subsystem.HandleEntityEvent(UUID, FGuid::NewGuid(), Fn, Params);
			});
	}

	// The exact sequence the router performs: charge the allowance, and only then decode the frame and
	// apply it. Building an FCrowdyEventParams by hand would test the dispatch against a frame no
	// serializer ever produced, and skipping the charge would leave the guards it owns unexercised. False
	// means the allowance step refused the call and nothing was decoded.
	template <typename C>
	bool DeliverProxyIntEvent(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, void (C::*MemberFn)(int32),
		const FGuid& UUID, int32 Value)
	{
		if (!Subsystem.ChargeEntityEventBudget(UUID, Fn))
		{
			return false;
		}

		ApplyProxyIntEvent(Subsystem, Fn, MemberFn, UUID, Value);
		return true;
	}

	/**
	 * How many tokens this entity's inbound allowance holds, read the only way anything outside the subsystem
	 * can read it: by spending it.
	 *
	 * Charged with a function the entity's own class CAN host, so every refusal counted is the allowance
	 * running out and not a guard. Nothing is decoded or applied, so each iteration costs exactly the one
	 * token that is being counted.
	 *
	 * The bucket refills on a clock, so this is only a true reading taken far faster than the refill rate; at
	 * thirty tokens a second a whole burst is spent in well under the time one token takes to come back. The
	 * cap is there so a machine slow enough to break that could not spin here forever.
	 */
	int32 SpendWholeAllowance(UCrowdyMassEntitySubsystem& Subsystem, UFunction* HostableFn, const FGuid& UUID)
	{
		const int32 Cap = static_cast<int32>(CrowdyMassReceiveBurstEvents) * 4;

		int32 Spent = 0;
		while (Spent < Cap && Subsystem.ChargeEntityEventBudget(UUID, HostableFn))
		{
			++Spent;
		}
		return Spent;
	}

	bool DeliverProxyPresent(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, const FGuid& UUID, int32 Value)
	{
		return DeliverProxyIntEvent(Subsystem, Fn,
			&ACrowdyMassDispatchProxyTestActor::ProxyPresent_Implementation, UUID, Value);
	}

	ACrowdyMassDispatchProxyTestActor* ProxyFor(UCrowdyMassEntitySubsystem& Subsystem)
	{
		return Cast<ACrowdyMassDispatchProxyTestActor>(
			Subsystem.GetOrCreateDispatchProxy(ACrowdyMassDispatchProxyTestActor::StaticClass()));
	}

}

/**
 * One proxy per class, not one per entity. This is the whole affordability claim of the tier: memory is
 * O(distinct classes) whether the crowd is ten or two thousand, which is what makes this categorically
 * unlike the avatar cache that has to be capped and reclaimed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchIsMemoisedPerClassTest,
	"CrowdySDK.Mass.ProxyDispatchIsMemoisedPerClass", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchIsMemoisedPerClassTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	AActor* First = Fixture.Subsystem->GetOrCreateDispatchProxy(
		ACrowdyMassDispatchProxyTestActor::StaticClass());
	AActor* Second = Fixture.Subsystem->GetOrCreateDispatchProxy(
		ACrowdyMassDispatchProxyTestActor::StaticClass());
	AActor* Other = Fixture.Subsystem->GetOrCreateDispatchProxy(
		ACrowdyMassRowEventTestActor::StaticClass());

	if (!TestNotNull(TEXT("a proxy is built for an ordinary actor class"), First)
		|| !TestNotNull(TEXT("and for a second, unrelated class"), Other))
	{
		return false;
	}

	TestTrue(TEXT("asking twice for the same class hands back the same proxy"), First == Second);
	TestTrue(TEXT("a different class gets a proxy of its own"), First != Other);
	TestTrue(TEXT("the proxy is of the class asked for"),
		First->GetClass() == ACrowdyMassDispatchProxyTestActor::StaticClass());

	// The claim that this thing was never spawned. Spawning is what puts an actor in its level's list, so
	// a proxy appearing there would mean everything else that follows from being unspawned is void.
	TestFalse(TEXT("the proxy is not in the level's actor list, so it really was never spawned"),
		Fixture.World->PersistentLevel->Actors.Contains(First));

	TestNull(TEXT("no proxy is built for a null class"), Fixture.Subsystem->GetOrCreateDispatchProxy(nullptr));

	return true;
}

/**
 * The load-bearing case: an event arriving through the ordinary charge-decode-apply sequence runs the
 * author's own body, with the call's parameter and the entity's own position.
 *
 * The position matters as much as the call. A body doing world-space presentation at its own location is
 * the whole of what this tier can carry, and one running at the origin would put every entity's effect in
 * the same place while still reporting that the body ran.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchRunsTheAuthorsBodyTest,
	"CrowdySDK.Mass.ProxyDispatchRunsTheAuthorsBody", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchRunsTheAuthorsBodyTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	// Somewhere no default could plausibly be, so a read-back here proves the projection landed rather
	// than matching a value that was already there.
	const FVector EntityLocation(1234.0, -5678.0, 91011.0);

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), EntityLocation);

	if (!TestTrue(TEXT("the call was delivered rather than refused"),
			DeliverProxyPresent(*Fixture.Subsystem, Fn, UUID, 4242)))
	{
		return false;
	}

	const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("the dispatch built a proxy of the entity's recorded class"), Proxy))
	{
		return false;
	}

	TestEqual(TEXT("the author's own body ran exactly once"), Proxy->PresentRunCount, 1);
	TestEqual(TEXT("and the call's parameter reached it"), Proxy->CarriedValue, 4242);
	TestEqual(TEXT("and it stood where the entity stands, not at the origin"),
		Proxy->PresentSawLocation, EntityLocation);

	return true;
}

/**
 * Nothing an event body writes may carry into the next entity's dispatch. The stand-in is shared by every
 * entity of the class, so without the reset a body's own member would read as the previous entity's value,
 * which is the shared-mirror shape rather than a stand-in.
 *
 * Read on ENTRY, before the body writes: recording it afterwards would show this dispatch's own value
 * whether or not the reset happened.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchCarriesNothingBetweenEntitiesTest,
	"CrowdySDK.Mass.ProxyDispatchCarriesNothingBetweenEntities", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchCarriesNothingBetweenEntitiesTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	const FVector FirstLocation(100.0, 0.0, 0.0);
	const FVector SecondLocation(-900.0, 25.0, 7.0);

	const FGuid FirstUUID = FGuid::NewGuid();
	const FGuid SecondUUID = FGuid::NewGuid();
	Fixture.MakeEntity(FirstUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FirstLocation);
	Fixture.MakeEntity(SecondUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), SecondLocation);

	if (!TestTrue(TEXT("the first entity's call was delivered"),
			DeliverProxyPresent(*Fixture.Subsystem, Fn, FirstUUID, 777)))
	{
		return false;
	}

	const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("the dispatch built a proxy"), Proxy))
	{
		return false;
	}

	// Asserted before the second call, so a fixture whose first body never ran cannot make the second
	// assertion below pass by having written nothing to carry.
	if (!TestEqual(TEXT("the first body wrote its value onto the shared stand-in"), Proxy->CarriedValue, 777))
	{
		return false;
	}

	if (!TestTrue(TEXT("the second entity's call was delivered"),
			DeliverProxyPresent(*Fixture.Subsystem, Fn, SecondUUID, 888)))
	{
		return false;
	}

	TestTrue(TEXT("both calls ran on the one stand-in"), ProxyFor(*Fixture.Subsystem) == Proxy);
	TestEqual(TEXT("both bodies ran"), Proxy->PresentRunCount, 2);
	TestEqual(TEXT("the second body entered on the class default, not the first entity's value"),
		Proxy->PresentSawCarriedValueOnEntry, 0);
	TestEqual(TEXT("and it stood where the SECOND entity stands"), Proxy->PresentSawLocation, SecondLocation);

	return true;
}

/**
 * A game that registered its own handler has already said what this event means for an entity held as a
 * row, so the author's body is NOT also run: one inbound event would otherwise do that work twice.
 *
 * The stand-in is a fallback, the same shape as the declared-action write beside it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchYieldsToARegisteredHandlerTest,
	"CrowdySDK.Mass.ProxyDispatchYieldsToARegisteredHandler", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchYieldsToARegisteredHandlerTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	int32 HandlerRunCount = 0;
	const FCrowdyMassEventHandlerHandle Handle = Fixture.Subsystem->RegisterEventHandler(Fn,
		[&HandlerRunCount](const FCrowdyEventParams&, const FCrowdyMassViewStateContext&)
		{
			++HandlerRunCount;
		});

	if (!TestTrue(TEXT("the handler registered"), Handle.IsValid()))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);

	if (!TestTrue(TEXT("the call was delivered"), DeliverProxyPresent(*Fixture.Subsystem, Fn, UUID, 55)))
	{
		return false;
	}

	TestEqual(TEXT("the game's own handler ran"), HandlerRunCount, 1);

	// Asked through the memo rather than through a proxy the dispatch might have built: a proxy that was
	// never built is also a body that never ran, and this has to tell the two apart from a body that ran.
	const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("a proxy can be built for the class, so its body could have run"), Proxy))
	{
		return false;
	}

	TestEqual(TEXT("but the author's body did not also run"), Proxy->PresentRunCount, 0);

	return true;
}

/**
 * A body the stand-in cannot host is REFUSED, and said, rather than run against an object of the wrong
 * class.
 *
 * The class guard admits an event declared on a COMPONENT of the entity's class on purpose: a class never
 * mentions the components its instances carry, and an observer holding the entity as a real actor resolves
 * the body onto the component that declares it. A stand-in has no component to resolve onto. It is
 * constructed rather than spawned, so it carries only what its C++ constructor made and nothing a
 * Blueprint adds. Handing such a function to ProcessEvent would apply the declaring class's property
 * offsets to this actor's memory, where every one of them names a different variable.
 *
 * Refused at BOTH ends of the receive path, and this covers both. Through the router's own sequence the
 * allowance step turns it away, before the call can cost the entity it names a token; driven straight at the
 * apply step, the dispatch turns it away itself, because a public entry point cannot depend on its callers
 * to have asked for an allowance first.
 *
 * Refusing is only half of it. An event that lands nothing in silence looks exactly like one that never
 * arrived, so the refusal is said once per declaring class: once, because the cause is a declaration and
 * outlasts every event; not never, because that is the reading this plane is hardest to diagnose from. The
 * two ends share the one report, so which of them refuses first cannot change whether it is said.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyRefusesForeignDeclaringClassTest,
	"CrowdySDK.Mass.ProxyRefusesForeignDeclaringClass", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyRefusesForeignDeclaringClassTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* ActorFn = ProxyPresentFunction();
	UFunction* ComponentFn = UCrowdyMassEventClassCheckTestComponent::StaticClass()
		->FindFunctionByName(TEXT("ComponentEvent_Implementation"));
	if (!TestNotNull(TEXT("the actor's own event resolves"), ActorFn)
		|| !TestNotNull(TEXT("the component's event resolves"), ComponentFn))
	{
		return false;
	}

	// Why this refusal is reachable at all, read from the guard rather than assumed. Nothing upstream of the
	// receive path stops a component-declared event aimed at an entity of this class, so it arrives and has to
	// be stopped here; a case that assumed otherwise would be testing an unreachable branch.
	TestTrue(TEXT("the class guard admits a component-declared event for this recorded class"),
		CrowdyMassTarget::CheckEventClass(ComponentFn, ACrowdyMassDispatchProxyTestActor::StaticClass())
			== CrowdyMassTarget::EClassCheckResult::Ok);

	TestEqual(TEXT("and nothing is registered for it, so the stand-in is the path it would take"),
		Fixture.Subsystem->NumEventHandlersRegistered(ComponentFn), 0);

	// A call the stand-in CAN host, first. It leaves the stand-in somewhere only a dispatch could have put
	// it, so "the refused call did not move it" says something: in a fixture that had never dispatched at
	// all, that would be true for the wrong reason.
	const FVector HostedLocation(300.0, -400.0, 500.0);
	const FGuid HostedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(HostedUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), HostedLocation);

	// The hosted call is not marked a one-shot action, so this plane says so once for that function. Named
	// here as an assertion rather than left as noise, because it is the line that claims a body RAN: said
	// for the refused calls below, which are a different function and so get a line of their own, it would
	// tell the reader the opposite of what happened and this count would be two.
	AddExpectedErrorPlain(TEXT("started no action, because it is not marked as a one-shot action"),
		EAutomationExpectedErrorFlags::Contains, 1);

	if (!TestTrue(TEXT("the actor-declared call was delivered"),
			DeliverProxyPresent(*Fixture.Subsystem, ActorFn, HostedUUID, 99)))
	{
		return false;
	}

	ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("the dispatch built a stand-in"), Proxy))
	{
		return false;
	}

	if (!TestEqual(TEXT("and ran the actor's own body on it, at that entity's position"),
			Proxy->PresentSawLocation, HostedLocation))
	{
		return false;
	}

	// Once, across every refusal below, whichever step does the refusing. A second occurrence fails this as
	// loudly as none at all, which is the throttle: the cause is one declaration, and a crowd repeating it per
	// entity per event is what buries the line that says which declaration to move.
	AddExpectedErrorPlain(TEXT("which does not derive it, so its body cannot run on a stand-in"),
		EAutomationExpectedErrorFlags::Contains, 1);

	const FVector RefusedLocation(-11.0, 22.0, -33.0);
	const FGuid FirstRefusedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(FirstRefusedUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), RefusedLocation);

	// Through the router's own sequence, the call stops at the allowance step, which is what keeps it from
	// costing the entity it names a token for a call that could never land.
	TestFalse(TEXT("the component-declared call is refused before the frame is even decoded"),
		DeliverProxyIntEvent(*Fixture.Subsystem, ComponentFn,
			&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, FirstRefusedUUID, 7));

	// Driven straight at the apply step, the way a caller that never asked for an allowance would, so the
	// dispatch's own refusal is measured rather than assumed to be unreachable. Without it, a direct caller
	// would run a component's body against an actor of an unrelated class.
	ApplyProxyIntEvent(*Fixture.Subsystem, ComponentFn,
		&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, FirstRefusedUUID, 7);

	// The observable consequence of refusing BEFORE the projection: a dispatch that went ahead would have
	// moved the shared stand-in to the refused entity's own position first.
	TestEqual(TEXT("the stand-in was not even moved for the refused call"),
		Proxy->GetActorLocation(), HostedLocation);
	TestEqual(TEXT("and no further body ran on it"), Proxy->PresentRunCount, 1);

	// A second entity of the same class, so the throttle is measured rather than assumed from one event.
	const FGuid SecondRefusedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(SecondRefusedUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), RefusedLocation);

	TestFalse(TEXT("the second component-declared call is refused there too"),
		DeliverProxyIntEvent(*Fixture.Subsystem, ComponentFn,
			&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, SecondRefusedUUID, 8));

	ApplyProxyIntEvent(*Fixture.Subsystem, ComponentFn,
		&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, SecondRefusedUUID, 8);

	TestEqual(TEXT("which was refused as well, leaving the stand-in where the hosted entity put it"),
		Proxy->GetActorLocation(), HostedLocation);
	TestEqual(TEXT("and still nothing further ran on it"), Proxy->PresentRunCount, 1);

	return true;
}

/**
 * A call the stand-in can never host costs the entity it names NOTHING.
 *
 * The allowance is keyed on the TARGET entity, because the target id is the only key the receive path has,
 * and that makes every token an event can spend something a peer can spend on somebody else's crowd row.
 * The rate is set far above legitimate play precisely so it only ever engages against a flood; what makes
 * that safe is that a call incapable of applying anything cannot spend a token at all. Refusing such a call
 * AFTER charging it turns the allowance from a CPU guard into the cheapest way to silence another player's
 * presentation: thirty structurally dead calls a second, and their real events start being dropped.
 *
 * Measured as the TOKEN COUNT, not as the refusal. A case asserting only that the delivery returned false
 * stays green against exactly the defect this covers, because the old code refused the call too, one step
 * further down and one token later.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyUnhostableEventSpendsNoBudgetTest,
	"CrowdySDK.Mass.ProxyUnhostableEventSpendsNoBudget", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyUnhostableEventSpendsNoBudgetTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* HostedFn = ProxyPresentFunction();
	UFunction* ComponentFn = UCrowdyMassEventClassCheckTestComponent::StaticClass()
		->FindFunctionByName(TEXT("ComponentEvent_Implementation"));
	if (!TestNotNull(TEXT("the actor's own event resolves"), HostedFn)
		|| !TestNotNull(TEXT("the component's event resolves"), ComponentFn))
	{
		return false;
	}

	// Why the flood is reachable at all, read from the guards rather than assumed. A component-declared event
	// is admitted for an entity of this class, nothing is registered to apply it, and the author did not
	// declare it an action, so the stand-in is the only way it could ever land and the stand-in cannot host it.
	// A case built on an event any of those three refused would prove nothing about this one.
	TestTrue(TEXT("the class guard admits a component-declared event for this recorded class"),
		CrowdyMassTarget::CheckEventClass(ComponentFn, ACrowdyMassDispatchProxyTestActor::StaticClass())
			== CrowdyMassTarget::EClassCheckResult::Ok);
	TestEqual(TEXT("and nothing is registered for it"),
		Fixture.Subsystem->NumEventHandlersRegistered(ComponentFn), 0);
	TestFalse(TEXT("and the author did not declare it an action"),
		FCrowdyRPC::GetFnInfo(ComponentFn).bIsAction);

	// Said once for the declaration, however many entities the flood names.
	AddExpectedErrorPlain(TEXT("which does not derive it, so its body cannot run on a stand-in"),
		EAutomationExpectedErrorFlags::Contains, 1);

	const FGuid VictimUUID = FGuid::NewGuid();
	const FGuid UntouchedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(VictimUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);
	Fixture.MakeEntity(UntouchedUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);

	// A whole burst of them, which is what it would take to empty the bucket if any of them charged.
	const int32 FloodCount = static_cast<int32>(CrowdyMassReceiveBurstEvents);
	int32 AcceptedCount = 0;
	for (int32 Index = 0; Index < FloodCount; ++Index)
	{
		if (DeliverProxyIntEvent(*Fixture.Subsystem, ComponentFn,
				&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, VictimUUID, Index))
		{
			++AcceptedCount;
		}
	}

	TestEqual(TEXT("every one of the flood's calls was refused"), AcceptedCount, 0);

	// The whole assertion, and it is about the TOKENS rather than about the refusals above: the flooded entity
	// must still be able to spend everything a fresh one can. A second entity of the same class, which the
	// flood never named, is measured beside it so the number is anchored rather than compared to a constant a
	// broken fixture could also produce: if charging had stopped working entirely both readings would be zero,
	// and the untouched entity is what refuses to let that pass.
	//
	// Read as "at least", not "exactly", because the bucket refills on a clock and a stalled machine can only
	// ever hand back MORE. Everything this covers moves the reading the other way: sixty charged refusals
	// leave nothing, and one charged refusal leaves one token short.
	const int32 VictimTokens = SpendWholeAllowance(*Fixture.Subsystem, HostedFn, VictimUUID);
	const int32 UntouchedTokens = SpendWholeAllowance(*Fixture.Subsystem, HostedFn, UntouchedUUID);

	TestTrue(TEXT("an entity nothing was aimed at can spend a full burst"), UntouchedTokens >= FloodCount);
	TestTrue(TEXT("and so can the flooded one, so the flood spent none of its allowance"),
		VictimTokens >= FloodCount);

	return true;
}

/**
 * The dispatch reports whether the body RAN, not whether it was called.
 *
 * AActor::ProcessEvent runs a body only where the actor's world reports its actors initialized or actor
 * script has been allowed to run, and it declines silently otherwise: no return value, no log, nothing. A
 * dispatch that answered "the body ran" for one of those makes the caller announce that an event worked in a
 * world that executed none of it, which is the one reading this whole path is hardest to diagnose from,
 * because it is also what a correctly working event looks like.
 *
 * The two halves are tested together because they are one claim. Reading only "are the actors initialized"
 * says no body ever runs in an editor context, where the guard below lets every one of them run; reporting
 * that answer as the return value would then hand exactly the opposite lie to the caller.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchReportsWhetherTheBodyRanTest,
	"CrowdySDK.Mass.ProxyDispatchReportsWhetherTheBodyRan", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchReportsWhetherTheBodyRanTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	const FGuid SilentUUID = FGuid::NewGuid();
	const FGuid RunningUUID = FGuid::NewGuid();
	Fixture.MakeEntity(SilentUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);
	Fixture.MakeEntity(RunningUUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);

	// Said once for the world, and only for the dispatch below that really cannot run anything. Under a
	// predicate that read the initialized flag alone it would be said for the second dispatch too, where the
	// body does run.
	AddExpectedErrorPlain(TEXT("neither reports its actors initialized nor allows actor script to run"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// Named by ENTITY, and that is what makes this an assertion rather than a formality. The line it stands
	// for is said once per function, so the two dispatches are competing for it: a dispatch that claimed a
	// body it never ran would spend that one report on the silent entity, and this line, which names the
	// entity whose body did run, would never appear at all.
	AddExpectedErrorPlain(FString::Printf(TEXT("arrived for entity %s and started no action"),
			*RunningUUID.ToString()), EAutomationExpectedErrorFlags::Contains, 1);

	{
		// An editor world never reports its actors initialized, so withdrawing script execution leaves nothing
		// that can run a body. The fixture's own guard is what allows one every other time.
		TGuardValue<bool> NoScriptExecution(GAllowActorScriptExecutionInEditor, false);

		if (!TestTrue(TEXT("the call was delivered"), DeliverProxyPresent(*Fixture.Subsystem, Fn, SilentUUID, 5)))
		{
			return false;
		}

		const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
		if (!TestNotNull(TEXT("a stand-in was built, so nothing else explains a body not running"), Proxy))
		{
			return false;
		}

		TestEqual(TEXT("and the engine ran no body in a world that executes none"), Proxy->PresentRunCount, 0);
	}

	// The same call with script execution back, so neither half of the case can pass because of something
	// about the fixture.
	if (!TestTrue(TEXT("the call was delivered again"),
			DeliverProxyPresent(*Fixture.Subsystem, Fn, RunningUUID, 6)))
	{
		return false;
	}

	const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("the stand-in is there"), Proxy))
	{
		return false;
	}

	TestEqual(TEXT("and this time the body really ran"), Proxy->PresentRunCount, 1);
	TestEqual(TEXT("with this call's parameter"), Proxy->CarriedValue, 6);

	return true;
}

/**
 * The router reads HasEntityEventHandler BEFORE it decodes anything, so an event this plane can apply
 * only through the stand-in has to be claimed there or it is dropped before its parameters exist. With the
 * stand-in off, an event nothing registered a handler for is not claimed, which is what this client did
 * before the stand-in existed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchClaimsUnregisteredEventsTest,
	"CrowdySDK.Mass.ProxyDispatchClaimsUnregisteredEvents", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchClaimsUnregisteredEventsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	// Nothing is registered for it and it is not declared an action, so before the stand-in this was
	// exactly the event the router reported as having nothing to apply it.
	TestEqual(TEXT("sanity: nothing is registered for this event"),
		Fixture.Subsystem->NumEventHandlersRegistered(Fn), 0);

	TestTrue(TEXT("the plane claims it, so the router decodes it rather than dropping it"),
		Fixture.Subsystem->HasEntityEventHandler(Fn));

	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest Disabled;
		TestFalse(TEXT("with the stand-in off, an unregistered event is not claimed"),
			Fixture.Subsystem->HasEntityEventHandler(Fn));
	}

	TestFalse(TEXT("a null function is never claimed"), Fixture.Subsystem->HasEntityEventHandler(nullptr));

	return true;
}

/**
 * With the stand-in off, an inbound event nothing registered a handler for lands nothing. The switch is
 * what a session measuring the per-event cost, or one chasing a body that misbehaves on observers only,
 * reaches for, and a switch that does not switch anything is worse than none.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassProxyDispatchCanBeTurnedOffTest,
	"CrowdySDK.Mass.ProxyDispatchCanBeTurnedOff", CrowdyMassProxyDispatchTestFlags)
bool FCrowdyMassProxyDispatchCanBeTurnedOffTest::RunTest(const FString& Parameters)
{
	FCrowdyMassProxyDispatchFixture Fixture;

	UFunction* Fn = ProxyPresentFunction();
	if (!TestNotNull(TEXT("the event's receiver function resolves"), Fn))
	{
		return false;
	}

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassDispatchProxyTestActor::StaticClass(), FVector::ZeroVector);

	{
		const CrowdyMassDispatchProxy::FScopedDisableForTest Disabled;
		if (!TestTrue(TEXT("the call still reaches HandleEntityEvent when driven directly"),
				DeliverProxyPresent(*Fixture.Subsystem, Fn, UUID, 11)))
		{
			return false;
		}

		const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
		if (!TestNotNull(TEXT("a proxy can still be built by hand, so nothing else explains a body not running"),
				Proxy))
		{
			return false;
		}

		TestEqual(TEXT("with the stand-in off, the author's body did not run"), Proxy->PresentRunCount, 0);
	}

	// The same call with it back on, so the case cannot pass because of something about the fixture.
	if (!TestTrue(TEXT("the call was delivered with the stand-in back on"),
			DeliverProxyPresent(*Fixture.Subsystem, Fn, UUID, 22)))
	{
		return false;
	}

	const ACrowdyMassDispatchProxyTestActor* Proxy = ProxyFor(*Fixture.Subsystem);
	if (!TestNotNull(TEXT("the proxy is there"), Proxy))
	{
		return false;
	}

	TestEqual(TEXT("and now the body ran"), Proxy->PresentRunCount, 1);
	TestEqual(TEXT("with this call's parameter"), Proxy->CarriedValue, 22);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
