// WITH_METADATA as well as the automation flag, because half of what these assert is what the AUTHOR
// declared: the CrowdyEvent marker, the action marker and the recipient are metadata, and a game target
// still compiles automation tests while carrying none of it. There the same assertions would read false
// and say nothing about the code.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"

#include "Assets/CrowdyMassAnimSet.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Mass/CrowdyMassEntity.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassFieldAccess.h"
#include "Target/CrowdyMassTargetLibrary.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdyBakedRegistry.h"
#include <limits>

// The one action event the SDK ships, so a game that only wants "everyone sees this animation" declares
// nothing at all. Three separate claims are worth pinning and each fails differently: that the event is
// DECLARED the way the transport and the crowd plane both read it, that a call to it actually reaches an
// entity's action slot, and that every value it carries is bounded on the way in. The declaration half is
// the one nothing else covers: an event missing its marker sends, arrives, runs, and starts nothing.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassGenericActionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager and subsystem, with no world: everything under test here is the
	// event's own declaration, the receiver resolution and one fragment write, none of which needs one.
	struct FCrowdyMassGenericActionFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassGenericActionFixture()
		{
			// Outered to the transient package rather than left ownerless: PostInitialize constructs
			// relation observers outered to the manager's owner, and one with none takes the process down.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassGenericActionFixture()
		{
			EntityManager->Deinitialize();
		}

		// An entity carrying the action slot, registered under UUID exactly as SpawnRemoteEntity's own
		// bookkeeping would. The action fragment is the whole point: an entity without one is the separate
		// "archetype carries no animation trait" case.
		FMassEntityHandle MakeActionCapableEntity(const FGuid& UUID)
		{
			const UScriptStruct* Fragments[] = { FCrowdyMassActionFragment::StaticStruct() };
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		FCrowdyMassActionFragment* ReadAction(const FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(Entity);
		}
	};

	// Sends one call to whatever the subsystem resolves as this entity's receiver, which is the road an
	// inbound event really takes: resolve, then run the declaring class's body on the object resolved.
	// Driving the body directly would prove the write and nothing about the routing.
	bool DeliverGenericAction(FCrowdyMassGenericActionFixture& Fixture, const FGuid& UUID, const int32 ActionId,
		const int32 Variant, const float PlayRate)
	{
		UFunction* Function = CrowdyMassFindPlayActionEvent();
		if (!Function)
		{
			return false;
		}

		UObject* Receiver = Fixture.Subsystem->ResolveReceiver(UUID, Function);
		if (!Receiver)
		{
			return false;
		}

		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Function);
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Function, Info,
			&UCrowdyMassEntity::PlayCrowdyAction_Implementation, ActionId, Variant, PlayRate);

		FCrowdyRPC::ApplyCall(Receiver, Function, Info, Call);
		return true;
	}

	UCrowdyMassAnimSet* MakeAnimSetWithActions(const TArray<FName>& ActionNames)
	{
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage());
		for (const FName& ActionName : ActionNames)
		{
			FCrowdyMassActionEntry Entry;
			Entry.ActionName = ActionName;
			AnimSet->Actions.Add(Entry);
		}
		return AnimSet;
	}
}

// Mutating the UFUNCTION line on UCrowdyMassEntity::PlayCrowdyAction_Implementation turns this red one
// claim at a time: dropping CrowdyEvent fails the transport check, dropping CrowdyAction fails the marker
// check, and changing CrowdyRecipient fails the routing check. Renaming a parameter or retyping ActionId
// fails the contract check instead, which is the shape that otherwise sends, arrives and starts nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGenericActionEventIsDeclaredTest,
	"CrowdySDK.Mass.GenericActionEventIsDeclared", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassGenericActionEventIsDeclaredTest::RunTest(const FString& Parameters)
{
	UFunction* Function = CrowdyMassFindPlayActionEvent();
	if (!TestNotNull(TEXT("the SDK's own action event resolves by reflection"), Function))
	{
		return false;
	}

	// The class it is declared ON is the claim, not an implementation detail: every stand-in an entity can
	// be given derives this one, whichever avatar class a game selects, and that is what makes one shipped
	// declaration deliverable everywhere.
	TestTrue(TEXT("it is declared on the stand-in base every crowd entity derives, not on an avatar class"),
		Function->GetOwnerClass() == UCrowdyMassEntity::StaticClass());

	TestTrue(TEXT("the send path recognises it as a Crowdy event"),
		UCrowdyBakedRegistry::IsEventHandlerFunction(Function));

	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Function);
	TestTrue(TEXT("the author declared it a one-shot action, which is what makes a crowd row react to it"),
		Info.bIsAction);

	// Owning Client and Host each reach one named machine, which holds the entity as a real actor and so
	// never as a crowd row; Multicast rides the capped session channel and is not distance-scoped.
	TestTrue(TEXT("it is routed to everyone in range rather than to one named machine"),
		Info.Recipient == ECrowdyEventRecipient::SpatialMulticast);

	TestTrue(TEXT("its signature can actually carry an action id"), CrowdyMassDeclaresActionId(Function));
	TestEqual(TEXT("and the authoring contract reports nothing wrong with it"),
		CrowdyMassDescribeActionEventProblem(Function), FString());

	return true;
}

// Mutating UCrowdyMassEntity::PlayCrowdyAction_Implementation's body (dropping the CrowdyMassWriteAction
// call, or returning before it) turns this red. Mutating the entity's own fragment lookup to ignore its
// result turns it red the same way, which is the point of asserting the entity's slot rather than the
// return value of anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGenericActionLandsOnARowTest,
	"CrowdySDK.Mass.GenericActionLandsOnARow", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassGenericActionLandsOnARowTest::RunTest(const FString& Parameters)
{
	FCrowdyMassGenericActionFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeActionCapableEntity(UUID);

	FCrowdyMassActionFragment* Action = Fixture.ReadAction(Entity);
	if (!TestNotNull(TEXT("the entity carries an action slot to start with"), Action))
	{
		return false;
	}
	TestEqual(TEXT("and nothing is playing on it yet"), Action->ActionId, 0);

	TestTrue(TEXT("the call resolves a receiver and runs"), DeliverGenericAction(Fixture, UUID, 5, 2, 1.5f));

	// Re-read rather than reused: nothing here moves the entity between archetypes, and asserting through a
	// pointer taken before the call would still pass if the write had gone somewhere else entirely.
	Action = Fixture.ReadAction(Entity);
	if (!TestNotNull(TEXT("the action slot is still readable after the call"), Action))
	{
		return false;
	}

	TestEqual(TEXT("the action id landed in the entity's own slot"), Action->ActionId, 5);
	TestEqual(TEXT("the variant landed with it"), Action->Variant, 2);
	TestEqual(TEXT("so did the play rate"), Action->PlayRate, 1.5f);
	TestEqual(TEXT("the retrigger serial moved"), Action->Serial, 1);

	// The animation pass reads Serial against AppliedSerial to know a trigger is outstanding, so a write
	// that moved both would land the action and show nothing.
	TestEqual(TEXT("and the trigger reads as outstanding to the animation pass"), Action->AppliedSerial, 0);

	return true;
}

// Every parameter arrives off the network and is treated as forged. Mutating any single clamp in
// CrowdyMassWriteAction / CrowdyMassClampActionPlayRate turns exactly one of these red, and removing the
// `ActionId <= 0` refusal turns the last two red together.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGenericActionBoundsForgedValuesTest,
	"CrowdySDK.Mass.GenericActionBoundsForgedValues", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassGenericActionBoundsForgedValuesTest::RunTest(const FString& Parameters)
{
	FCrowdyMassGenericActionFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeActionCapableEntity(UUID);

	TestTrue(TEXT("an absurd variant and rate still deliver"),
		DeliverGenericAction(Fixture, UUID, 3, 9999, 1000.0f));
	if (const FCrowdyMassActionFragment* Action = Fixture.ReadAction(Entity))
	{
		TestEqual(TEXT("the variant is clamped to what an anim set could plausibly author"),
			Action->Variant, CrowdyMassMaxActionVariant);
		TestEqual(TEXT("a huge rate is clamped rather than burning through the clip in a frame"),
			Action->PlayRate, CrowdyMassMaxActionPlayRate);
	}

	TestTrue(TEXT("a zero rate delivers"), DeliverGenericAction(Fixture, UUID, 3, 0, 0.0f));
	if (const FCrowdyMassActionFragment* Action = Fixture.ReadAction(Entity))
	{
		TestEqual(TEXT("a zero rate is clamped rather than freezing the animation on its first frame"),
			Action->PlayRate, CrowdyMassMinActionPlayRate);
	}

	// Not clamped, REPLACED. A NaN compares false against every bound, so clamping one returns it unchanged
	// and it reaches the renderer intact.
	TestTrue(TEXT("a non-finite rate delivers"),
		DeliverGenericAction(Fixture, UUID, 3, 0, std::numeric_limits<float>::quiet_NaN()));
	if (const FCrowdyMassActionFragment* Action = Fixture.ReadAction(Entity))
	{
		TestEqual(TEXT("a non-finite rate is replaced with the ordinary one"), Action->PlayRate, 1.0f);
	}

	const FCrowdyMassActionFragment* Before = Fixture.ReadAction(Entity);
	const int32 SerialBeforeRefusals = Before ? Before->Serial : -1;

	// Zero is the slot's own "nothing playing" value, so honouring it would let an event CANCEL an action
	// rather than start one, and a negative id names nothing at all.
	TestTrue(TEXT("an id of zero still delivers as a call"), DeliverGenericAction(Fixture, UUID, 0, 1, 1.0f));
	TestTrue(TEXT("so does a negative id"), DeliverGenericAction(Fixture, UUID, -4, 1, 1.0f));
	if (const FCrowdyMassActionFragment* Action = Fixture.ReadAction(Entity))
	{
		TestEqual(TEXT("but neither wrote anything, so the previous action still stands"), Action->ActionId, 3);
		TestEqual(TEXT("and neither bumped the retrigger serial"), Action->Serial, SerialBeforeRefusals);
	}

	return true;
}

// The name-to-id decision behind Send Crowdy Action, asserted directly. Mutating any branch of
// CrowdyMassDecideActionSend turns exactly one group of these red; the two-set case is the one that turns
// red if the class is ever dropped from the question, which is the failure that plays a wrong action
// rather than none and that nothing anywhere would report.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionNameResolvesAgainstTheTargetClassTest,
	"CrowdySDK.Mass.ActionNameResolvesAgainstTheTargetClass", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassActionNameResolvesAgainstTheTargetClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassAnimSet> Guards(
		MakeAnimSetWithActions({ TEXT("Swing"), TEXT("Flinch") }));
	const TStrongObjectPtr<UCrowdyMassAnimSet> Villagers(
		MakeAnimSetWithActions({ TEXT("Flinch"), TEXT("Wave"), TEXT("Swing") }));

	// Any class will do as a stand-in for "this client knows what the entity is": the decision reads the
	// class to tell "not learned yet" from "learned, and it authors no set", never to look anything up.
	const UClass* KnownClass = UCrowdyMassCharacterAvatar::StaticClass();

	int32 ActionId = INDEX_NONE;
	TestTrue(TEXT("an authored name resolves"),
		CrowdyMassDecideActionSend(KnownClass, Guards.Get(), TEXT("Flinch"), ActionId)
			== ECrowdyMassActionSendVerdict::Send);
	TestEqual(TEXT("to the one-based row position in that class's own set"), ActionId, 2);

	// The whole reason the class is part of the question. The same name is a different row in another
	// class's set, so resolving against the wrong one plays a wrong action and reports success.
	TestTrue(TEXT("the same name against another class's set resolves"),
		CrowdyMassDecideActionSend(KnownClass, Villagers.Get(), TEXT("Flinch"), ActionId)
			== ECrowdyMassActionSendVerdict::Send);
	TestEqual(TEXT("to a different row, which is why an id may never be resolved against another class's set"),
		ActionId, 1);

	TestTrue(TEXT("a name no row carries is refused"),
		CrowdyMassDecideActionSend(KnownClass, Guards.Get(), TEXT("Nope"), ActionId)
			== ECrowdyMassActionSendVerdict::UnknownAction);
	TestEqual(TEXT("and reports no id, because 0 is the slot's own \"nothing playing\" value"), ActionId, 0);

	TestTrue(TEXT("an empty name is refused the same way rather than sending nothing"),
		CrowdyMassDecideActionSend(KnownClass, Guards.Get(), NAME_None, ActionId)
			== ECrowdyMassActionSendVerdict::UnknownAction);
	TestEqual(TEXT("with no id"), ActionId, 0);

	// Told apart deliberately: one is a binding somebody has to make, the other resolves itself once the
	// entity's own client announces what it is, and a single refusal would say the wrong thing to one of them.
	TestTrue(TEXT("a known class that names no animation set is its own refusal"),
		CrowdyMassDecideActionSend(KnownClass, nullptr, TEXT("Flinch"), ActionId)
			== ECrowdyMassActionSendVerdict::NoAnimSet);
	TestEqual(TEXT("with no id"), ActionId, 0);

	TestTrue(TEXT("a class this client has not learned yet is a different refusal"),
		CrowdyMassDecideActionSend(nullptr, Guards.Get(), TEXT("Flinch"), ActionId)
			== ECrowdyMassActionSendVerdict::ClassUnknown);
	TestEqual(TEXT("with no id"), ActionId, 0);

	return true;
}

// Three refusals that each name a different fix are worth telling apart only if each is actually said.
// The trap is that following the printed advice can change WHICH refusal it is without changing the class
// or the name: binding a class to an animation set is exactly what the "names no animation set" line asks
// for, and it turns that verdict into "the set authors no such row". A record keyed on class and name
// alone answers "already said" to the second one, so the author has followed the advice and been rewarded
// with the same silence they started with.
//
// Mutating CrowdyMassShouldReportActionSendRefusal to drop the verdict from the key turns this red on
// exactly that assertion and leaves the once-per-subject assertions green, which is the right split: the
// suppression is still wanted, it is the subject that was too coarse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionRefusalSaysEachReasonOnceTest,
	"CrowdySDK.Mass.ActionRefusalSaysEachReasonOnce", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassActionRefusalSaysEachReasonOnceTest::RunTest(const FString& Parameters)
{
	CrowdyMassResetActionSendRefusalLog();

	const UClass* KnownClass = UCrowdyMassCharacterAvatar::StaticClass();
	const UClass* OtherClass = UCrowdyMassEntity::StaticClass();
	const FName Action(TEXT("HitReact"));

	TestTrue(TEXT("the first refusal for a class and name is said"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, Action, ECrowdyMassActionSendVerdict::NoAnimSet));
	TestFalse(TEXT("and repeating the same refusal every shot says nothing further"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, Action, ECrowdyMassActionSendVerdict::NoAnimSet));

	// The one that matters. Same class, same name, and the author has already acted on what the first line
	// told them; this is the sentence naming what is still left to do.
	TestTrue(TEXT("a DIFFERENT reason for the same class and name is still said"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, Action, ECrowdyMassActionSendVerdict::UnknownAction));
	TestFalse(TEXT("and it too is said only once"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, Action, ECrowdyMassActionSendVerdict::UnknownAction));

	TestTrue(TEXT("another name under the same class and reason is its own subject"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, TEXT("Swing"), ECrowdyMassActionSendVerdict::UnknownAction));
	TestTrue(TEXT("and so is the same name under another class"),
		CrowdyMassShouldReportActionSendRefusal(OtherClass, Action, ECrowdyMassActionSendVerdict::UnknownAction));

	// A null class is the "not learned yet" case, which has no class of its own to key on. It must not
	// collapse into the resolved-class record or the temporary refusal would silence the permanent one.
	TestTrue(TEXT("the not-learned-yet case keys separately from any resolved class"),
		CrowdyMassShouldReportActionSendRefusal(nullptr, Action, ECrowdyMassActionSendVerdict::ClassUnknown));

	// Without a reset nothing can tell suppressed from reported, so the seam is asserted rather than
	// merely used above.
	CrowdyMassResetActionSendRefusalLog();
	TestTrue(TEXT("clearing the record makes the first refusal say itself again"),
		CrowdyMassShouldReportActionSendRefusal(KnownClass, Action, ECrowdyMassActionSendVerdict::NoAnimSet));

	CrowdyMassResetActionSendRefusalLog();
	return true;
}

// Sending an action runs the event's own body on this machine as part of the send, and that body writes
// the entity's action fragment straight into chunk storage. During a Mass phase that races every processor
// declared to read it, so the send has to make the same refusal every field write on this library already
// makes. Two real paths reach it: a graph run from a delegate broadcast inside a chunk loop, and an
// ordinary tick that overlaps a phase.
//
// The scope is entered for real rather than simulated: FMassEntityManager hands out the same
// FScopedProcessing a running processor holds. With no world the call returns false either way, so the
// LOG is the discriminator, and the expected-error count is what asserts it: an expected error that never
// occurs fails the test just as an unexpected one does.
//
// Mutating the IsProcessing refusal in SendEntityAction turns this red on the missing expected error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionSendRefusedDuringProcessingTest,
	"CrowdySDK.Mass.ActionSendIsRefusedWhileMassIsProcessing", CrowdyMassGenericActionTestFlags)
bool FCrowdyMassActionSendRefusedDuringProcessingTest::RunTest(const FString& Parameters)
{
	// The manager needs a real owner: PostInitialize constructs relation observers outered to whatever
	// owner it was given, and an owner-less manager takes the process down.
	TSharedPtr<FMassEntityManager> EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

	FMassEntityManagerStorageInitParams InitParams;
	InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
	EntityManager->Initialize(InitParams);
	EntityManager->PostInitialize();

	CrowdyMassFields::SetEntityManagerForTest(EntityManager.Get());

	// The refusal is spaced out rather than said once, and the spacing is process-wide and time-based, so
	// without this the assertion below would pass or fail on what ran before it and how recently.
	UCrowdyMassTargetLibrary::ResetSendThrottleForTest();

	FCrowdyMassEntityTarget Target;

	// Nothing is expected here, and that is half the assertion: a send outside a phase must not report the
	// refusal. It still returns false, because a target with no world names nothing to route through.
	TestFalse(TEXT("a target naming no world sends nothing"),
		UCrowdyMassTargetLibrary::SendEntityAction(Target, TEXT("HitReact")));

	AddExpectedErrorPlain(TEXT("was sent while Mass processors are running"),
		EAutomationExpectedErrorFlags::Contains, 1);

	{
		FMassEntityManager::FScopedProcessing ProcessingScope = EntityManager->NewProcessingScope();

		TestFalse(TEXT("and a send inside a Mass phase is refused"),
			UCrowdyMassTargetLibrary::SendEntityAction(Target, TEXT("HitReact")));
	}

	CrowdyMassFields::SetEntityManagerForTest(nullptr);
	EntityManager->Deinitialize();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
