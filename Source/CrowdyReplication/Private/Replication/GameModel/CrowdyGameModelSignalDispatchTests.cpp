// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h" // FCrowdyMutationApplied
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyContainerStandIn.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h" // MakeSignalHandlerName
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

// DispatchSignal is the whole client half of a signal: it resolves the local object bound to a container id, calls
// its parameterless OnSignal_<Name>, and broadcasts OnCrowdySignal whether or not anything resolved. It is public so
// these can drive it with no channel and no network, which means they prove DISPATCH and say nothing about whether
// a frame ever arrives.
namespace
{
	constexpr EAutomationTestFlags CrowdySignalDispatchTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names so this TU never collides with the apply / sub-participant helpers in a unity build.
	UCrowdyGameModelSubsystem* MakeSignalModel(UCrowdyEntitySubsystem*& OutEntities)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		OutEntities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		OutEntities->SetLocalPlayerID(FGuid::NewGuid());
		Model->SetEntitySubsystemForTest(OutEntities);
		return Model;
	}

	UCrowdyGameModelTestTarget* MakeSignalSpy(UCrowdyGameModelSubsystem* Model)
	{
		UCrowdyGameModelTestTarget* Spy = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
		Model->OnCrowdySignal.AddDynamic(Spy, &UCrowdyGameModelTestTarget::HandleCrowdySignal);
		return Spy;
	}
}

// The assumption the whole signal feature rests on: a CrowdyContainer COMPONENT enrolled as a sub-participant is
// reached by name, on its own derived NetID, and not through its anchor. The anchor carries an identically named
// handler so a resolution that fell back to the anchor cannot pass this.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalReachesComponentSubParticipantTest,
	"CrowdySDK.GameModel.SignalReachesAComponentSubParticipant", CrowdySignalDispatchTestFlags)
bool FCrowdySignalReachesComponentSubParticipantTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("anchor created"), Anchor)
		|| !TestNotNull(TEXT("component created"), Comp))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid SubNetID = Entities->RegisterSubParticipant(Comp, AnchorNetID);
	if (!TestTrue(TEXT("the anchor registered"), AnchorNetID.IsValid())
		|| !TestTrue(TEXT("the component enrolled"), SubNetID.IsValid())
		|| !TestTrue(TEXT("the component has its own entity id"), SubNetID != AnchorNetID))
	{
		return false;
	}
	TestTrue(TEXT("the record holds the component, not its anchor"), Entities->FindParticipant(SubNetID) == Comp);

	Model->BindEntityContainerForTest(AnchorNetID, TEXT("c-anchor"));
	Model->BindEntityContainerForTest(SubNetID, TEXT("c-comp"));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-comp"));

	TestEqual(TEXT("the component's handler ran once"), Comp->WaveSignalCount, 1);
	TestEqual(TEXT("the anchor's identically named handler did not run"), Anchor->WaveSignalCount, 0);
	TestEqual(TEXT("the delegate fired once"), Spy->SignalDelegateCount, 1);
	TestFalse(TEXT("and it carried a target"), Spy->bLastSignalTargetWasNull);
	TestTrue(TEXT("the target is the component"), Spy->LastSignalTarget.Get() == Comp);
	TestEqual(TEXT("the broadcast names the signal"), Spy->LastSignalName, FString(TEXT("Wave")));
	TestEqual(TEXT("and the container it arrived for"), Spy->LastSignalContainerId, FString(TEXT("c-comp")));

	// The anchor's own container still reaches the anchor, so the two are addressed independently.
	Model->DispatchSignal(TEXT("Wave"), TEXT("c-anchor"));
	TestEqual(TEXT("the anchor's container reaches the anchor"), Anchor->WaveSignalCount, 1);
	TestEqual(TEXT("and does not re-run the component"), Comp->WaveSignalCount, 1);
	return true;
}

// The handler runs BEFORE the delegate, so a listener that reads the container in its OnCrowdySignal handler sees
// what the handler already wrote rather than the state before it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalDispatchOrderIsHandlerThenDelegateTest,
	"CrowdySDK.GameModel.SignalDispatchOrderIsHandlerThenDelegate", CrowdySignalDispatchTestFlags)
bool FCrowdySignalDispatchOrderIsHandlerThenDelegateTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Target = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid NetID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);
	Model->BindEntityContainerForTest(NetID, TEXT("c-order"));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-order"));

	// Both sequence numbers come from one monotonic source, so this compares two numbers rather than two timestamps.
	if (!TestTrue(TEXT("the handler ran"), Target->WaveSignalSeq > 0)
		|| !TestTrue(TEXT("the delegate fired"), Spy->LastSignalDelegateSeq > 0))
	{
		return false;
	}
	TestTrue(TEXT("the handler ran before the broadcast"), Target->WaveSignalSeq < Spy->LastSignalDelegateSeq);
	return true;
}

// A container nothing local is bound to still broadcasts, with a null target. That is the difference between "the
// frame never arrived" and "the frame arrived and had nowhere to land", and it is the only discrimination a client
// can make from a flat handler counter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalUnboundContainerBroadcastsWithNullTargetTest,
	"CrowdySDK.GameModel.SignalUnboundContainerBroadcastsWithNullTarget", CrowdySignalDispatchTestFlags)
bool FCrowdySignalUnboundContainerBroadcastsWithNullTargetTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Target = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid NetID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);
	Model->BindEntityContainerForTest(NetID, TEXT("c-bound"));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-nobody"));

	TestEqual(TEXT("no handler ran"), Target->WaveSignalCount, 0);
	TestEqual(TEXT("the delegate fired once anyway"), Spy->SignalDelegateCount, 1);
	TestTrue(TEXT("with no target"), Spy->bLastSignalTargetWasNull);
	TestEqual(TEXT("naming the container that had nowhere to land"),
		Spy->LastSignalContainerId, FString(TEXT("c-nobody")));

	// An empty name or an empty container id is not a delivery with nowhere to land, so neither broadcasts at all.
	Model->DispatchSignal(FString(), TEXT("c-bound"));
	Model->DispatchSignal(TEXT("Wave"), FString());
	TestEqual(TEXT("a malformed dispatch broadcasts nothing"), Spy->SignalDelegateCount, 1);
	return true;
}

// A handler found by name but taking parameters is refused rather than called: ProcessEvent with a null frame
// against a function expecting arguments is a corrupted call frame, not a no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalWrongArityHandlerIsNotCalledTest,
	"CrowdySDK.GameModel.SignalWrongArityHandlerIsNotCalled", CrowdySignalDispatchTestFlags)
bool FCrowdySignalWrongArityHandlerIsNotCalledTest::RunTest(const FString& Parameters)
{
	// Literal, not a regex: AddExpectedError treats its argument as a pattern. It matches a warning as readily as an
	// error, so this locks that the refusal is REPORTED, not the level it is reported at.
	AddExpectedErrorPlain(TEXT("a signal handler must be parameterless"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("component created"), Comp))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid SubNetID = Entities->RegisterSubParticipant(Comp, AnchorNetID);
	Model->BindEntityContainerForTest(SubNetID, TEXT("c-comp"));

	// Asserted rather than assumed: if the fixture ever lost the one-parameter handler this test would pass by
	// finding nothing at all, which is a different branch.
	TestNotNull(TEXT("the fixture carries a one-parameter handler"),
		Comp->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Bad"))));

	Model->DispatchSignal(TEXT("Bad"), TEXT("c-comp"));

	TestEqual(TEXT("the wrong-arity handler was not called"), Comp->BadSignalCount, 0);
	TestEqual(TEXT("the delegate still fired"), Spy->SignalDelegateCount, 1);
	TestFalse(TEXT("and still carried the resolved target"), Spy->bLastSignalTargetWasNull);
	TestTrue(TEXT("which is the component"), Spy->LastSignalTarget.Get() == Comp);
	return true;
}

// One container row is meant to have one local holder. When two local entities end up on one anyway, the NEWEST
// holder receives and only it, which is one deterministic answer rather than an order-of-binding accident.
// Unbinding it hands the row to the holder still on it, and unbinding that leaves the row with no target at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalReachesTheNewestBoundNetIDTest,
	"CrowdySDK.GameModel.SignalReachesTheNewestBoundNetID", CrowdySignalDispatchTestFlags)
bool FCrowdySignalReachesTheNewestBoundNetIDTest::RunTest(const FString& Parameters)
{
	// Literal, not a regex, and it matches the warning the second bind reports for an unsupported shape.
	AddExpectedErrorPlain(TEXT("a container row is meant to have one local holder"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	// Two DIFFERENT classes on purpose: RegisterParticipant seeds a Host NetID from the class path alone, so two
	// instances of one class collapse onto a single entity and there would be nothing to tell apart.
	UCrowdyGameModelTestTarget* Older = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTargetDerived* Newer = NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("older created"), Older)
		|| !TestNotNull(TEXT("newer created"), Newer))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid OlderNetID = Entities->RegisterParticipant(Older, ECrowdyOwnership::Host);
	const FGuid NewerNetID = Entities->RegisterParticipant(Newer, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the two are distinct entities"), OlderNetID.IsValid() && NewerNetID != OlderNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(OlderNetID, TEXT("c-shared"));
	Model->BindEntityContainerForTest(NewerNetID, TEXT("c-shared"));
	if (!TestEqual(TEXT("both hold the one row"), Model->GetBoundNetIDCountForTest(TEXT("c-shared")), 2))
	{
		return false;
	}
	// Asserted rather than assumed: a fixture that bound these the other way round would satisfy every assertion
	// below while proving the opposite rule.
	TestTrue(TEXT("the one called newest here really did bind second"),
		Model->GetBindEpochForTest(NewerNetID) > Model->GetBindEpochForTest(OlderNetID));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-shared"));

	TestEqual(TEXT("the newest-bound object is signalled"), Newer->WaveSignalCount, 1);
	TestEqual(TEXT("the object it replaced is not"), Older->WaveSignalCount, 0);
	TestTrue(TEXT("and the broadcast names the newest-bound object"), Spy->LastSignalTarget.Get() == Newer);
	TestEqual(TEXT("one dispatch is one broadcast, never one per binding"), Spy->SignalDelegateCount, 1);

	// Unbinding the newest falls back to the holder still on the row rather than leaving it unreachable.
	Model->UnbindEntityContainerForTest(NewerNetID);
	TestEqual(TEXT("one holder is left"), Model->GetBoundNetIDCountForTest(TEXT("c-shared")), 1);
	Model->DispatchSignal(TEXT("Wave"), TEXT("c-shared"));

	TestEqual(TEXT("the survivor now receives"), Older->WaveSignalCount, 1);
	TestEqual(TEXT("and the unbound object receives no more"), Newer->WaveSignalCount, 1);
	TestEqual(TEXT("both dispatches broadcast"), Spy->SignalDelegateCount, 2);

	// Unbinding the last holder empties the row, which is a broadcast with no target rather than no broadcast.
	Model->UnbindEntityContainerForTest(OlderNetID);
	TestEqual(TEXT("no holder is left"), Model->GetBoundNetIDCountForTest(TEXT("c-shared")), 0);
	Model->DispatchSignal(TEXT("Wave"), TEXT("c-shared"));

	TestEqual(TEXT("nothing ran on either object"), Older->WaveSignalCount + Newer->WaveSignalCount, 2);
	TestEqual(TEXT("the delegate fired anyway"), Spy->SignalDelegateCount, 3);
	TestTrue(TEXT("carrying no target"), Spy->bLastSignalTargetWasNull);
	return true;
}

// Removing a holder must not reorder the ones left: the row is read newest-last, so a swap-remove of the oldest
// would move the last holder into its slot and promote the wrong one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerRowKeepsBindOrderAcrossUnbindTest,
	"CrowdySDK.GameModel.ContainerRowKeepsBindOrderAcrossUnbind", CrowdySignalDispatchTestFlags)
bool FCrowdyContainerRowKeepsBindOrderAcrossUnbindTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("a container row is meant to have one local holder"),
		EAutomationExpectedErrorFlags::Contains, 2);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	// Three DIFFERENT classes, for the same reason two are used elsewhere: a Host NetID is seeded from the class
	// path, so two instances of one class would collapse onto a single entity.
	UCrowdyGameModelTestTarget* First = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTargetDerived* Middle = NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Last = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("first created"), First)
		|| !TestNotNull(TEXT("middle created"), Middle) || !TestNotNull(TEXT("last created"), Last))
	{
		return false;
	}

	const FGuid FirstNetID = Entities->RegisterParticipant(First, ECrowdyOwnership::Host);
	const FGuid MiddleNetID = Entities->RegisterParticipant(Middle, ECrowdyOwnership::Host);
	const FGuid LastNetID = Entities->RegisterParticipant(Last, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the three are distinct entities"),
		FirstNetID.IsValid() && MiddleNetID != FirstNetID && LastNetID != FirstNetID && LastNetID != MiddleNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(FirstNetID, TEXT("c-row3"));
	Model->BindEntityContainerForTest(MiddleNetID, TEXT("c-row3"));
	Model->BindEntityContainerForTest(LastNetID, TEXT("c-row3"));
	if (!TestEqual(TEXT("all three hold the one row"), Model->GetBoundNetIDCountForTest(TEXT("c-row3")), 3))
	{
		return false;
	}

	// Unbinding the OLDEST is what a swap-remove would answer differently: it would drop the newest into slot 0.
	Model->UnbindEntityContainerForTest(FirstNetID);
	TestEqual(TEXT("two holders are left"), Model->GetBoundNetIDCountForTest(TEXT("c-row3")), 2);

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-row3"));

	TestEqual(TEXT("the newest holder still receives"), Last->WaveSignalCount, 1);
	TestEqual(TEXT("the one it was bound after does not"), Middle->WaveSignalCount, 0);
	TestEqual(TEXT("and neither does the unbound one"), First->WaveSignalCount, 0);
	return true;
}

// The newest holder is the answer only while it still exists. An entity whose object was collected without its
// registration being torn down leaves a row that resolves to nothing, so the holder still alive receives instead
// of the signal being dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalSkipsACollectedHolderTest,
	"CrowdySDK.GameModel.SignalSkipsACollectedHolder", CrowdySignalDispatchTestFlags)
bool FCrowdySignalSkipsACollectedHolderTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("a container row is meant to have one local holder"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Survivor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTargetDerived* Doomed = NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("survivor created"), Survivor)
		|| !TestNotNull(TEXT("doomed created"), Doomed))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid SurvivorNetID = Entities->RegisterParticipant(Survivor, ECrowdyOwnership::Host);
	const FGuid DoomedNetID = Entities->RegisterParticipant(Doomed, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the two are distinct entities"), SurvivorNetID.IsValid() && DoomedNetID != SurvivorNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(SurvivorNetID, TEXT("c-dead"));
	Model->BindEntityContainerForTest(DoomedNetID, TEXT("c-dead"));

	// Garbage, not merely unreferenced: a weak pointer to a garbage object already reads as null, which is the
	// state a record is in when its participant dies without the registration hearing about it.
	Doomed->MarkAsGarbage();
	if (!TestNull(TEXT("the newest holder no longer resolves"), Entities->FindParticipant(DoomedNetID))
		|| !TestEqual(TEXT("but it is still on the row"), Model->GetBoundNetIDCountForTest(TEXT("c-dead")), 2))
	{
		return false;
	}

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-dead"));

	TestEqual(TEXT("the holder still alive received"), Survivor->WaveSignalCount, 1);
	TestEqual(TEXT("the delegate fired once"), Spy->SignalDelegateCount, 1);
	TestTrue(TEXT("naming it, not the collected one"), Spy->LastSignalTarget.Get() == Survivor);

	// The same row, asked by the other two by-id paths: a dropped signal and a starved re-pull are one defect.
	Model->HandleModelChangedByContainer(TEXT("c-dead"));
	FGuid RefreshTarget;
	if (!TestTrue(TEXT("the notification queued a refresh"),
		Model->TryGetPendingRefreshPullTargetForTest(TEXT("c-dead"), RefreshTarget)))
	{
		return false;
	}
	TestEqual(TEXT("addressed to the holder still alive"), RefreshTarget, SurvivorNetID);
	return true;
}

// Every path that addresses a container by id answers from one index, so a confirmed invoke's writes, the signal
// that invoke triggers and the re-pull a model-changed notification queues all name the same object. A second
// holder on the row cannot split them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalAndInvokeMutationTargetTheSameObjectTest,
	"CrowdySDK.GameModel.SignalAndInvokeMutationTargetTheSameObject", CrowdySignalDispatchTestFlags)
bool FCrowdySignalAndInvokeMutationTargetTheSameObjectTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("a container row is meant to have one local holder"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Older = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTargetDerived* Newer = NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("older created"), Older)
		|| !TestNotNull(TEXT("newer created"), Newer))
	{
		return false;
	}

	const FGuid OlderNetID = Entities->RegisterParticipant(Older, ECrowdyOwnership::Host);
	const FGuid NewerNetID = Entities->RegisterParticipant(Newer, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the two are distinct entities"), OlderNetID.IsValid() && NewerNetID != OlderNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(OlderNetID, TEXT("c-shared"));
	Model->BindEntityContainerForTest(NewerNetID, TEXT("c-shared"));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-shared"));

	// The invoke is issued BY the older holder, on its OWN container. That is the shape a real invoke takes and the
	// one a self-addressed shortcut answers differently from the resolver the signal just used.
	TArray<FCrowdyMutationApplied> SelfWrite;
	FCrowdyMutationApplied& Hp = SelfWrite.AddDefaulted_GetRef();
	Hp.ContainerId = TEXT("c-shared");
	Hp.Key = TEXT("hp");
	Hp.NewValueJson = TEXT("77");
	Model->ApplyInvokeMutations(OlderNetID, TEXT("c-shared"), SelfWrite);

	TestEqual(TEXT("the signal reached the newest-bound object"), Newer->WaveSignalCount, 1);
	TestEqual(TEXT("and the invoke's write reached that same object"), Newer->Hp, 77);
	TestEqual(TEXT("the holder that issued the invoke received no signal"), Older->WaveSignalCount, 0);
	TestEqual(TEXT("and was not written either"), Older->Hp, 100);

	// A mutation with no container id is an older server saying "the invoke's own", so it takes the same route.
	TArray<FCrowdyMutationApplied> LegacyWrite;
	FCrowdyMutationApplied& Mana = LegacyWrite.AddDefaulted_GetRef();
	Mana.Key = TEXT("mana");
	Mana.NewValueJson = TEXT("33");
	Model->ApplyInvokeMutations(OlderNetID, TEXT("c-shared"), LegacyWrite);

	TestEqual(TEXT("an id-less write lands on the same object"), Newer->Mana, 33);
	TestEqual(TEXT("and not on the one that issued it"), Older->Mana, 50);

	// A write addressed ACROSS to this row from an invoke on another container resolves it the same way.
	TArray<FCrowdyMutationApplied> CrossWrite;
	FCrowdyMutationApplied& Gold = CrossWrite.AddDefaulted_GetRef();
	Gold.ContainerId = TEXT("c-shared");
	Gold.Key = TEXT("gold");
	Gold.NewValueJson = TEXT("9");
	Model->ApplyInvokeMutations(FGuid(), TEXT("c-elsewhere"), CrossWrite);

	TestEqual(TEXT("a cross-container write lands on the same object"), Newer->Gold, 9);
	TestEqual(TEXT("and not on the other holder"), Older->Gold, 0);

	// The third path that addresses this row by id: an inbound notification queues its re-pull for the same object.
	Model->HandleModelChangedByContainer(TEXT("c-shared"));
	FGuid RefreshTarget;
	if (!TestTrue(TEXT("the notification queued a refresh"),
		Model->TryGetPendingRefreshPullTargetForTest(TEXT("c-shared"), RefreshTarget)))
	{
		return false;
	}
	TestEqual(TEXT("addressed to the object the signal and the invoke both chose"), RefreshTarget, NewerNetID);

	// The one thing still answered from the invoke's own entity: a row nothing holds any more. Its writes go where
	// the invoke was issued rather than nowhere, which is what an unbound-target invoke used to rely on.
	Model->UnbindEntityContainerForTest(OlderNetID);
	Model->UnbindEntityContainerForTest(NewerNetID);
	TArray<FCrowdyMutationApplied> AfterUnbind;
	FCrowdyMutationApplied& Late = AfterUnbind.AddDefaulted_GetRef();
	Late.Key = TEXT("hp");
	Late.NewValueJson = TEXT("5");
	Model->ApplyInvokeMutations(OlderNetID, TEXT("c-shared"), AfterUnbind);

	TestEqual(TEXT("an invoke whose row went away still writes the entity that issued it"), Older->Hp, 5);
	TestEqual(TEXT("and not the object that had been resolving for it"), Newer->Hp, 77);
	return true;
}

// On a client that draws an entity as a row, a component container is held by a behaviour-less stand-in. A signal
// for such a container is re-addressed to the entity holding it, the same re-addressing the attribute-apply path
// makes, so it arrives instead of being dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalToAContainerStandInRelaysToItsAnchorTest,
	"CrowdySDK.GameModel.SignalToAContainerStandInRelaysToItsAnchor", CrowdySignalDispatchTestFlags)
bool FCrowdySignalToAContainerStandInRelaysToItsAnchorTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyContainerStandIn* StandIn = NewObject<UCrowdyContainerStandIn>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("anchor created"), Anchor)
		|| !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);
	StandIn->RepresentedClass = UCrowdyGameModelTestComponent::StaticClass();

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid StandInNetID = Entities->RegisterSubParticipantAs(
		StandIn, AnchorNetID, UCrowdyGameModelTestComponent::StaticClass(), TEXT("Attributes"));
	if (!TestTrue(TEXT("the stand-in enrolled"), StandInNetID.IsValid()))
	{
		return false;
	}
	Model->BindEntityContainerForTest(StandInNetID, TEXT("c-drawn"));

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-drawn"));

	// Asserted rather than assumed: a stand-in that had gained a handler would pass the count below without the
	// relay ever running, which is a different branch.
	TestNull(TEXT("a stand-in has no handler to call"),
		StandIn->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Wave"))));
	TestEqual(TEXT("the entity holding the container ran the handler"), Anchor->WaveSignalCount, 1);
	TestEqual(TEXT("the delegate fired once"), Spy->SignalDelegateCount, 1);
	TestFalse(TEXT("carrying a target"), Spy->bLastSignalTargetWasNull);
	TestTrue(TEXT("which is the object the handler ran on"), Spy->LastSignalTarget.Get() == Anchor);

	// The same re-addressing the attribute-apply path makes, which the signal path now consults too.
	TestEqual(TEXT("both paths re-address this container to its anchor"),
		Model->ResolveSubscriberEntityIDForTest(StandInNetID), AnchorNetID);

	// A name neither object answers to is not a relay: the broadcast falls back to the object bound to the
	// container, which is what tells a listener the row has a local holder at all.
	Model->DispatchSignal(TEXT("Nope"), TEXT("c-drawn"));
	TestEqual(TEXT("nothing more ran on the anchor"), Anchor->WaveSignalCount, 1);
	TestEqual(TEXT("the delegate fired again"), Spy->SignalDelegateCount, 2);
	TestTrue(TEXT("naming the object bound to the container"), Spy->LastSignalTarget.Get() == StandIn);
	return true;
}

// The parameterless-handler contract holds on the relayed object too: a component container whose holder has no
// handler and whose entity has a wrong-arity one is refused and reported rather than called with a corrupted frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalRelayedToAWrongArityHandlerIsNotCalledTest,
	"CrowdySDK.GameModel.SignalRelayedToAWrongArityHandlerIsNotCalled", CrowdySignalDispatchTestFlags)
bool FCrowdySignalRelayedToAWrongArityHandlerIsNotCalledTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("a signal handler must be parameterless"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	// The entity holding the row is the fixture carrying the one-parameter handler, so the refusal has to happen
	// after the relay rather than before it.
	UCrowdyGameModelTestComponent* Anchor = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	UCrowdyContainerStandIn* StandIn = NewObject<UCrowdyContainerStandIn>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("anchor created"), Anchor)
		|| !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);
	StandIn->RepresentedClass = UCrowdyGameModelTestComponent::StaticClass();

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid StandInNetID = Entities->RegisterSubParticipantAs(
		StandIn, AnchorNetID, UCrowdyGameModelTestComponent::StaticClass(), TEXT("Attributes"));
	if (!TestTrue(TEXT("the stand-in enrolled"), StandInNetID.IsValid()))
	{
		return false;
	}
	Model->BindEntityContainerForTest(StandInNetID, TEXT("c-drawn"));

	TestNull(TEXT("the stand-in answers to no handler of that name"),
		StandIn->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Bad"))));
	TestNotNull(TEXT("and the entity holding it carries the one-parameter handler"),
		Anchor->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Bad"))));

	Model->DispatchSignal(TEXT("Bad"), TEXT("c-drawn"));

	TestEqual(TEXT("the wrong-arity handler was not called"), Anchor->BadSignalCount, 0);
	TestEqual(TEXT("the delegate still fired"), Spy->SignalDelegateCount, 1);
	TestTrue(TEXT("naming the object bound to the container, since nothing ran"),
		Spy->LastSignalTarget.Get() == StandIn);
	return true;
}

// A handler found by name is where the signal stops, whatever its signature. The re-address to the entity holding
// the container is for a container whose holder answers to the name at all, so an object that answers badly is
// reported rather than quietly passed over in favour of somebody else's handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalWrongArityHandlerIsNotRelayedPastTest,
	"CrowdySDK.GameModel.SignalWrongArityHandlerIsNotRelayedPast", CrowdySignalDispatchTestFlags)
bool FCrowdySignalWrongArityHandlerIsNotRelayedPastTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("a signal handler must be parameterless"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	// A stand-in, so the re-address is available to be declined rather than unavailable in the first place.
	UCrowdyGameModelBadSignalStandIn* Holder = NewObject<UCrowdyGameModelBadSignalStandIn>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("anchor created"), Anchor)
		|| !TestNotNull(TEXT("holder created"), Holder))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);
	Holder->RepresentedClass = UCrowdyGameModelTestComponent::StaticClass();

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid HolderNetID = Entities->RegisterSubParticipant(Holder, AnchorNetID);
	if (!TestTrue(TEXT("the holder enrolled under the anchor"), HolderNetID.IsValid() && HolderNetID != AnchorNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(HolderNetID, TEXT("c-bad"));

	// Asserted rather than assumed: if the anchor lost its handler this would pass by finding nothing to relay to.
	TestNotNull(TEXT("the anchor carries a correct handler of the same name"),
		Anchor->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Wave"))));
	// And the re-address really does reach it, so refusing here is a decision rather than a dead end.
	TestEqual(TEXT("the holder's container re-addresses to the anchor"),
		Model->ResolveSubscriberEntityIDForTest(HolderNetID), AnchorNetID);

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-bad"));

	TestEqual(TEXT("the wrong-arity handler was not called"), Holder->WaveSignalCount, 0);
	TestEqual(TEXT("and the anchor's correct one was not called in its place"), Anchor->WaveSignalCount, 0);
	TestEqual(TEXT("the delegate still fired"), Spy->SignalDelegateCount, 1);
	TestTrue(TEXT("naming the object bound to the container, since nothing ran"),
		Spy->LastSignalTarget.Get() == Holder);
	return true;
}

// The re-address is for a container whose holder is a stand-in, not for every sub-participant. A real component
// that simply has no handler of that name keeps the signal: relaying it would hand the component's signals to its
// actor's identically named handler, and a parameterless handler cannot tell which container called it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySignalToAComponentIsNotRelayedToItsActorTest,
	"CrowdySDK.GameModel.SignalToAComponentIsNotRelayedToItsActor", CrowdySignalDispatchTestFlags)
bool FCrowdySignalToAComponentIsNotRelayedToItsActorTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	UCrowdyGameModelTestTarget* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	// A real component container carrying no signal handler at all, which is the authoring shape at issue.
	UCrowdyGameModelTestKeyedComponent* Comp =
		NewObject<UCrowdyGameModelTestKeyedComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("anchor created"), Anchor)
		|| !TestNotNull(TEXT("component created"), Comp))
	{
		return false;
	}
	UCrowdyGameModelTestTarget* Spy = MakeSignalSpy(Model);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	const FGuid CompNetID = Entities->RegisterSubParticipant(Comp, AnchorNetID);
	if (!TestTrue(TEXT("the component enrolled under the anchor"), CompNetID.IsValid() && CompNetID != AnchorNetID))
	{
		return false;
	}
	Model->BindEntityContainerForTest(CompNetID, TEXT("c-plain"));

	TestNull(TEXT("the component answers to no handler of that name"),
		Comp->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Wave"))));
	TestNotNull(TEXT("while the actor holding it carries one"),
		Anchor->FindFunction(UCrowdyEffect::MakeSignalHandlerName(TEXT("Wave"))));
	// The discriminator: the re-address would reach the anchor, so declining it is a decision and not a dead end.
	TestEqual(TEXT("and the component's container does re-address to the anchor"),
		Model->ResolveSubscriberEntityIDForTest(CompNetID), AnchorNetID);

	Model->DispatchSignal(TEXT("Wave"), TEXT("c-plain"));

	TestEqual(TEXT("the actor's handler did not run for the component's container"), Anchor->WaveSignalCount, 0);
	TestEqual(TEXT("the delegate still fired"), Spy->SignalDelegateCount, 1);
	TestTrue(TEXT("naming the component bound to the container"), Spy->LastSignalTarget.Get() == Comp);
	return true;
}

// An empty container id and an unbound entity id are refused where the binding is written, so no reader downstream
// has to answer for a row keyed on nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerBindRefusesEmptyKeysTest,
	"CrowdySDK.GameModel.ContainerBindRefusesEmptyKeys", CrowdySignalDispatchTestFlags)
bool FCrowdyContainerBindRefusesEmptyKeysTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	Model->BindEntityContainerForTest(FGuid(0xE0, 1, 2, 3), FString());
	TestEqual(TEXT("an empty container id mints no row"), Model->GetBoundNetIDCountForTest(FString()), 0);

	Model->BindEntityContainerForTest(FGuid(), TEXT("c-no-entity"));
	TestEqual(TEXT("and an unbound entity id binds nothing"),
		Model->GetBoundNetIDCountForTest(TEXT("c-no-entity")), 0);
	return true;
}

// Two local holders on one container row is not a supported shape, so the bind that creates it says so, naming the
// row, the holder arriving and the one already there. It is reported where the state comes into existence and not
// where a notification resolves it, because resolution runs per delivered notification. An idempotent re-bind
// creates nothing, so it neither reports again nor moves the epoch the merge windows are keyed on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerMultiBindIsWarnedPerHolderTest,
	"CrowdySDK.GameModel.ContainerMultiBindIsWarnedPerHolder", CrowdySignalDispatchTestFlags)
bool FCrowdyContainerMultiBindIsWarnedPerHolderTest::RunTest(const FString& Parameters)
{
	const FGuid First(0xD0, 1, 2, 3);
	const FGuid Second(0xD1, 1, 2, 3);
	const FGuid Third(0xD2, 1, 2, 3);

	// The whole formatted message, so the row and both entities it names are gated rather than the sentence alone.
	// One occurrence each: a resolver that reported instead would make these several, a silent bind none.
	AddExpectedErrorPlain(FString::Printf(
		TEXT("container c-row is now bound by entity %s as well as %s; a container row is meant to have one local holder, so model changes and signals for it reach the newest binding only."),
		*Second.ToString(), *First.ToString()), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(FString::Printf(
		TEXT("container c-row is now bound by entity %s as well as %s; a container row is meant to have one local holder, so model changes and signals for it reach the newest binding only."),
		*Third.ToString(), *Second.ToString()), EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyGameModelSubsystem* Model = MakeSignalModel(Entities);
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	Model->BindEntityContainerForTest(First, TEXT("c-row"));
	TestEqual(TEXT("the first holder is recorded"), Model->GetBoundNetIDCountForTest(TEXT("c-row")), 1);

	Model->BindEntityContainerForTest(Second, TEXT("c-row"));
	TestEqual(TEXT("the second is recorded rather than refused"),
		Model->GetBoundNetIDCountForTest(TEXT("c-row")), 2);

	// A third holder is a second occurrence, not a repeat of the first: each one is its own new ambiguity.
	Model->BindEntityContainerForTest(Third, TEXT("c-row"));
	TestEqual(TEXT("the third is recorded too"), Model->GetBoundNetIDCountForTest(TEXT("c-row")), 3);

	const uint32 EpochBeforeRebind = Model->GetBindEpochForTest(Third);
	Model->BindEntityContainerForTest(Third, TEXT("c-row"));
	TestEqual(TEXT("an idempotent re-bind adds no holder"), Model->GetBoundNetIDCountForTest(TEXT("c-row")), 3);
	// The point of that early return: a fresh epoch would refuse every merge window already open against this one.
	TestEqual(TEXT("and leaves the epoch the open merge windows are keyed on"),
		Model->GetBindEpochForTest(Third), EpochBeforeRebind);

	// Resolving the ambiguous row repeatedly, which is what a live session does: none of these may report again.
	Model->DispatchSignal(TEXT("Wave"), TEXT("c-row"));
	Model->DispatchSignal(TEXT("Wave"), TEXT("c-row"));
	Model->HandleModelChangedByContainer(TEXT("c-row"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
