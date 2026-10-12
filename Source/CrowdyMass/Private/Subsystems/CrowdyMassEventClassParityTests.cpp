#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Target/CrowdyMassEntityTargetTestTypes.h"
#include "Templates/SharedPointer.h"

/**
 * That one entity is reachable by the same events whether this client holds it as an actor or as a row.
 *
 * The two representations answer two different questions about the same call: the send path asks whether a
 * function may be aimed at an entity of a given class, and the receive path asks whether an arriving call may be
 * applied to the entity's own fragments. Every case here asserts the two against EACH OTHER rather than against
 * a constant, because a receive-side rule that is merely narrower than the send-side one reads as a working
 * guard: the event sends, it lands on every observer holding the entity as an actor, and it is dropped in
 * silence on the observers holding it as a crowd row.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyMassEventClassParityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager with no World, matching the other subsystem fixtures: PostInitialize
	// outers its relation observers to whatever owner the manager was given, and an owner-less manager takes the
	// process down with "Object is not packaged".
	struct FCrowdyMassEventClassParityFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassEventClassParityFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassEventClassParityFixture()
		{
			EntityManager->Deinitialize();
		}

		// A tracked entity recorded as RecordedClass. Pass null for one this client has not been told the class
		// of yet. The fragment is only there because an entity needs an archetype; nothing here writes to it.
		FMassEntityHandle MakeEntity(const FGuid& UUID, const UClass* RecordedClass)
		{
			const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);

			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, RecordedClass);
			return Entity;
		}
	};

	// Marshals a real call and hands its decoded parameters to the subsystem, which is the pair of steps the
	// router performs. Building the parameters by hand would test the guard against a frame no serializer ever
	// produced. MemberFn only supplies the signature; nothing is dispatched through an instance.
	template <typename C>
	void DeliverIntEvent(UCrowdyMassEntitySubsystem& Subsystem, UFunction* Fn, void (C::*MemberFn)(int32),
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
}

/**
 * An event a game declares on a COMPONENT of its actor class reaches an entity held here as a row.
 *
 * A class never mentions the components its instances carry, so a check that demanded the recorded class derive
 * the declaring class refuses every such event. The send path admits it deliberately, and an observer holding
 * the entity as a real actor resolves the body onto the component, so an entity held as a row that refused it
 * would be the one representation the event could not reach.
 *
 * Narrowing the receive-side check back to a bare IsChildOf of the declaring class turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventComponentDeclaredEventLandsTest,
	"CrowdySDK.Mass.RowEventComponentDeclaredEventLands", CrowdyMassEventClassParityTestFlags)
bool FCrowdyMassRowEventComponentDeclaredEventLandsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassEventClassParityFixture Fixture;

	UFunction* ComponentFn = UCrowdyMassEventClassCheckTestComponent::StaticClass()
		->FindFunctionByName(TEXT("ComponentEvent_Implementation"));
	if (!TestNotNull(TEXT("the component's event resolved"), ComponentFn))
	{
		return false;
	}

	// The send side's answer for exactly this pair, read here rather than assumed, so this case states the
	// agreement it is checking instead of hard-coding one half of it.
	TestTrue(TEXT("the send path admits a component-declared event aimed at an entity recorded as the actor class"),
		CrowdyMassTarget::CheckEventClass(ComponentFn, ACrowdyMassEventParityTestTarget::StaticClass())
			== CrowdyMassTarget::EClassCheckResult::Ok);

	// Held indirectly so the handler the subsystem stores does not outlive a stack local.
	const TSharedRef<int32> Delivered = MakeShared<int32>(0);
	Fixture.Subsystem->RegisterEventHandler(ComponentFn,
		[Delivered](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)
		{
			++(*Delivered);
		});

	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeEntity(UUID, ACrowdyMassEventParityTestTarget::StaticClass());

	DeliverIntEvent(*Fixture.Subsystem, ComponentFn,
		&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, UUID, 7);

	TestEqual(TEXT("so the receive path applies it to the same entity held as a row"), *Delivered, 1);

	// The other half of the send side's rule, which the agreement must not lose either: an entity nothing has
	// identified is not yet known to be the kind this event was declared for, component or not.
	AddExpectedErrorPlain(TEXT("Refusing 'ComponentEvent_Implementation'"),
		EAutomationExpectedErrorFlags::Contains, 1);

	const FGuid UnknownUUID = FGuid::NewGuid();
	Fixture.MakeEntity(UnknownUUID, nullptr);
	DeliverIntEvent(*Fixture.Subsystem, ComponentFn,
		&UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation, UnknownUUID, 7);

	TestEqual(TEXT("while an entity whose class has not arrived is still refused"), *Delivered, 1);

	return true;
}

/**
 * The refusal that must survive the agreement: an event declared on an ordinary actor class is still applied
 * only to an entity recorded as that kind of actor.
 *
 * The pair is the point. Widening the receive-side check past what the send side admits would leave this
 * refusal reading exactly like the control beside it, which is the failure a component-shaped fix could
 * plausibly introduce.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRowEventWrongActorClassStillRefusedTest,
	"CrowdySDK.Mass.RowEventWrongActorClassStillRefused", CrowdyMassEventClassParityTestFlags)
bool FCrowdyMassRowEventWrongActorClassStillRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassEventClassParityFixture Fixture;

	UFunction* ActorFn = ACrowdyMassEventParityTestTarget::StaticClass()
		->FindFunctionByName(TEXT("HostRecipientEvent_Implementation"));
	if (!TestNotNull(TEXT("the actor's event resolved"), ActorFn))
	{
		return false;
	}

	TestTrue(TEXT("the send path calls an unrelated recorded class a mismatch"),
		CrowdyMassTarget::CheckEventClass(ActorFn, UCrowdyMassEventClassCheckUnrelatedTestTarget::StaticClass())
			== CrowdyMassTarget::EClassCheckResult::ClassMismatch);

	const TSharedRef<int32> Delivered = MakeShared<int32>(0);
	Fixture.Subsystem->RegisterEventHandler(ActorFn,
		[Delivered](const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)
		{
			++(*Delivered);
		});

	AddExpectedErrorPlain(TEXT("Refusing 'HostRecipientEvent_Implementation'"),
		EAutomationExpectedErrorFlags::Contains, 1);

	const FGuid MismatchedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(MismatchedUUID, UCrowdyMassEventClassCheckUnrelatedTestTarget::StaticClass());
	DeliverIntEvent(*Fixture.Subsystem, ActorFn,
		&ACrowdyMassEventParityTestTarget::HostRecipientEvent_Implementation, MismatchedUUID, 7);

	TestEqual(TEXT("so the receive path refuses it too"), *Delivered, 0);

	// The control that stops the refusal above from being the only thing this handler can produce.
	const FGuid MatchedUUID = FGuid::NewGuid();
	Fixture.MakeEntity(MatchedUUID, ACrowdyMassEventParityTestTarget::StaticClass());
	DeliverIntEvent(*Fixture.Subsystem, ActorFn,
		&ACrowdyMassEventParityTestTarget::HostRecipientEvent_Implementation, MatchedUUID, 7);

	TestEqual(TEXT("while the entity the event was declared for still receives it"), *Delivered, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
