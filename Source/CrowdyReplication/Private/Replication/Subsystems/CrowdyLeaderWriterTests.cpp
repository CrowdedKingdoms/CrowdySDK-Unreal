#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/UCrowdyClassRegistry.h"

// The Leader writes replicated subsystems and nobody writes World actors. These cases cover that every ownership
// question gets that one answer, that such state is taken only from that writer, that an owned entity takes nobody
// else's writes, and that a project relying on a writer it does not have is told so.
namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderWriterTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyEntitySubsystem* MakeEntitiesWithLeader(const FGuid& LocalPlayer, const FGuid& LeaderID)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
		Entities->SetGameSessionForTest(MakeGameSession(LeaderID));
		return Entities;
	}

	// Sends Target a delta from Sender that sets its RepInt to 42, through the real receive path. False when the
	// class has no RepInt in its layout.
	bool DeliverRepInt(UCrowdyAutoRegistry* Registry, UCrowdyEventRouter* Router, UObject* Target, const FGuid& EntityID,
		const FGuid& Sender, const uint8 Flags)
	{
		UClass* const TargetClass = Target->GetClass();
		const FCrowdyRepLayout* Layout = Registry ? Registry->FindRepLayout(TargetClass) : nullptr;
		const int32 RepIntIndex = Layout ? IndexOfPropertyName(*Layout, TEXT("RepInt")) : INDEX_NONE;
		const FIntProperty* RepInt = CastField<FIntProperty>(TargetClass->FindPropertyByName(TEXT("RepInt")));
		if (RepIntIndex == INDEX_NONE || !RepInt)
		{
			return false;
		}

		UObject* Source = NewObject<UObject>(GetTransientPackage(), TargetClass);
		RepInt->SetPropertyValue_InContainer(Source, 42);
		TBitArray<> Dirty;
		Dirty.Init(false, Layout->Properties.Num());
		Dirty[RepIntIndex] = true;

		FCrowdyStateDelta Delta;
		Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
		Delta.EntityID = EntityID;
		Delta.SenderID = Sender;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = Flags;
		FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Delta.Blob);
		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		return true;
	}

	int32 ReadRepInt(const UObject* Target)
	{
		const FIntProperty* RepInt = CastField<FIntProperty>(Target->GetClass()->FindPropertyByName(TEXT("RepInt")));
		return RepInt ? RepInt->GetPropertyValue_InContainer(Target) : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterWorldEntityHasNoWriterTest,
	"CrowdySDK.Leader.WorldEntityHasNoWriter", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterWorldEntityHasNoWriterTest::RunTest(const FString& Parameters)
{
	const FGuid LeaderID = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(FGuid::NewGuid(), LeaderID);

	const FGuid SubsystemID = Entities->RegisterParticipant(NewObject<UCrowdyStateSubsystemTestTarget>());
	const FGuid ActorID = Entities->RegisterParticipant(NewObject<ACrowdyStateSendTestActor>(GetTransientPackage()),
		ECrowdyOwnership::Host);
	const FCrowdyEntityRecord* Subsystem = Entities->FindRecord(SubsystemID);
	const FCrowdyEntityRecord* Actor = Entities->FindRecord(ActorID);
	if (!TestTrue(TEXT("both register"), Subsystem && Actor))
		return false;

	TestEqual(TEXT("the Leader writes a replicated subsystem"), Entities->GetWriterID(*Subsystem), LeaderID);
	TestFalse(TEXT("nothing writes a World actor"), Entities->GetWriterID(*Actor).IsValid());
	TestEqual(TEXT("the app writer is the Leader"), Entities->GetAppWriterID(), LeaderID);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterOneOwnershipAnswerTest,
	"CrowdySDK.Leader.OneOwnershipAnswer", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterOneOwnershipAnswerTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();

	TestTrue(TEXT("an Owner record naming this client is written here"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, Local, Local, FGuid()));
	TestFalse(TEXT("an Owner record naming another client is not"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, Other, Local, FGuid()));
	TestFalse(TEXT("a remote proxy is not, whatever it names"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::RemoteProxy, Local, Local, Local));
	TestTrue(TEXT("a record with no owner is written by its writer"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), Local, Local));
	TestFalse(TEXT("and by nobody when there is none"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), Local, FGuid()));
	TestTrue(TEXT("an Owner record registered before the local player id arrived is still this client's"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, FGuid(), Local, FGuid()));
	TestFalse(TEXT("a client with no player id writes nothing without an owner"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), FGuid(), FGuid()));

	// This client leads, and a replicated subsystem and a World actor are registered here.
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(Local, Local);
	const FGuid SubsystemID = Entities->RegisterParticipant(NewObject<UCrowdyStateSubsystemTestTarget>());

	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	Component->SetCollaboratorsForTest(Entities, nullptr);
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);

	TestTrue(TEXT("the Leader writes a replicated subsystem"), Entities->IsLocallyOwned(SubsystemID));
	TestFalse(TEXT("but nobody writes a World actor, the Leader included"), Component->IsLocallyOwned());

	UCrowdyEntitySubsystem* Follower = MakeEntitiesWithLeader(Local, FGuid::NewGuid());
	const FGuid FollowerSubsystemID = Follower->RegisterParticipant(NewObject<UCrowdyStateSubsystemTestTarget>());
	TestFalse(TEXT("and a client another player leads does not write it"), Follower->IsLocallyOwned(FollowerSubsystemID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterWorldDeltaNeedsTheLeaderTest,
	"CrowdySDK.Leader.WorldDeltaNeedsTheLeader", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterWorldDeltaNeedsTheLeaderTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FGuid LeaderID = FGuid::NewGuid();

	// Each case gets a fresh receiver of TargetClass with no owner, so a value applied by one cannot be mistaken for
	// the next one's. Returns the receiver's RepInt, which the change sets to 42.
	auto Receive = [&](UClass* TargetClass, const uint8 Flags) -> int32
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(FGuid::NewGuid(), LeaderID);
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);
		UObject* Target = NewObject<UObject>(GetTransientPackage(), TargetClass);
		const FGuid EntityID = Target->IsA<AActor>()
			? Entities->RegisterParticipant(Target, ECrowdyOwnership::Host)
			: Entities->RegisterParticipant(Target);
		if (!DeliverRepInt(Registry, Router, Target, EntityID, LeaderID, Flags))
		{
			AddError(FString::Printf(TEXT("%s has no RepInt in its layout"), *TargetClass->GetName()));
			return -1;
		}
		return ReadRepInt(Target);
	};
	UClass* const Subsystem = UCrowdyStateSubsystemTestTarget::StaticClass();
	UClass* const Actor = ACrowdyStateSendTestActor::StaticClass();

	// Clients learn of a new Leader at different times, so the flag decides, not the sender.
	TestEqual(TEXT("the Leader writes a replicated subsystem, so its flagged change applies"), Receive(Subsystem, CrowdyStateDeltaFlags::HostSourced), 42);
	TestEqual(TEXT("one without the flag is ignored"), Receive(Subsystem, 0), 0);
	TestEqual(TEXT("nothing writes a World actor, so even its flagged change is ignored"), Receive(Actor, CrowdyStateDeltaFlags::HostSourced), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterOwnedEntityDropsFlaggedForeignDeltaTest,
	"CrowdySDK.Leader.OwnedEntityDropsFlaggedForeignDelta", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterOwnedEntityDropsFlaggedForeignDeltaTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FGuid Local = FGuid::NewGuid();
	const FGuid Stranger = FGuid::NewGuid();

	// This client owns the actor, and the stranger is not its owner. The stranger leads, so the flag is the one a
	// Leader stamps: it must still not reach an entity someone else owns.
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(Local, Stranger);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);
	ACrowdyStateSendTestActor* Owned = NewObject<ACrowdyStateSendTestActor>(GetTransientPackage());
	const FGuid OwnedID = FGuid::NewGuid();
	RegisterParticipantAs(Entities, OwnedID, Owned, ECrowdyRole::Owner, Local);

	if (!TestTrue(TEXT("the flagged delta is sent"),
		DeliverRepInt(Registry, Router, Owned, OwnedID, Stranger, CrowdyStateDeltaFlags::HostSourced)))
		return false;
	TestEqual(TEXT("a flagged delta from a non-owner leaves an owned entity untouched"), Owned->RepInt, 0);
	TestEqual(TEXT("and is counted as a foreign write"),
		Router->GetStateParticipantStats().DroppedForeignWriteForOwned, static_cast<int64>(1));

	DeliverRepInt(Registry, Router, Owned, OwnedID, Stranger, 0);
	TestEqual(TEXT("an unflagged one is dropped the same way"), Owned->RepInt, 0);
	TestEqual(TEXT("and counted"), Router->GetStateParticipantStats().DroppedForeignWriteForOwned, static_cast<int64>(2));

	// The same Leader's flagged delta to a replicated subsystem on this client still applies.
	UCrowdyStateSubsystemTestTarget* Subsystem = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid SubsystemID = Entities->RegisterParticipant(Subsystem);
	DeliverRepInt(Registry, Router, Subsystem, SubsystemID, Stranger, CrowdyStateDeltaFlags::HostSourced);
	TestEqual(TEXT("the Leader's flagged delta to a replicated subsystem applies"), Subsystem->RepInt, 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterLeaderWritesItsSubsystemsTest,
	"CrowdySDK.Leader.LeaderWritesItsSubsystems", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterLeaderWritesItsSubsystemsTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(Local, Local);
	UCrowdyStateReplicator* Replicator = MakeReplicator(MakeStateRegistry(), Local);
	Replicator->SetEntitySubsystemForTest(Entities);
	TArray<FCrowdyStateDelta> Sent;
	Replicator->ChannelDispatchHookForTests = [&Sent](const FCrowdyStateDelta& Delta) { Sent.Add(Delta); };

	UCrowdyStateSubsystemTestTarget* Subsystem = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid SubsystemID = Entities->RegisterParticipant(Subsystem);
	const FGuid ActorID = Entities->RegisterParticipant(NewObject<ACrowdyStateSendTestActor>(GetTransientPackage()), ECrowdyOwnership::Host);
	Replicator->HandleEntityRegisteredForTest(SubsystemID);
	Replicator->HandleEntityRegisteredForTest(ActorID);
	TestTrue(TEXT("the Leader tracks a replicated subsystem"), Replicator->IsTrackedForTest(SubsystemID));
	TestFalse(TEXT("but not a World actor, which nothing writes"), Replicator->IsTrackedForTest(ActorID));
	TestEqual(TEXT("only the subsystem is remembered for a Leader change"), Replicator->NumKnownSubsystemsForTest(), 1);

	Subsystem->RepInt = 5;
	Subsystem->RepNotified = 3;
	Replicator->RunReplicationLoopForTest();
	TestTrue(TEXT("its change goes out flagged as the writer's"), Sent.Num() == 1
		&& (Sent[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0 && Sent[0].EntityID == SubsystemID);
	TestEqual(TEXT("and fires its notify here once"), Subsystem->NotifiedOnRepCount, 1);

	const FGuid Other = FGuid::NewGuid();
	Entities->SetGameSessionForTest(MakeGameSession(Other));
	Replicator->HandleLeaderChangedForTest(Other, Local);
	TestFalse(TEXT("losing the lead stops it writing the subsystem"), Replicator->IsTrackedForTest(SubsystemID));

	// The new writer's shadow starts at each property's zero, so taking the lead back re-sends every nonzero value.
	Sent.Reset();
	Entities->SetGameSessionForTest(MakeGameSession(Local));
	Replicator->HandleLeaderChangedForTest(Local, Other);
	Replicator->RunReplicationLoopForTest();
	TestTrue(TEXT("taking the lead again tracks it and re-sends its nonzero values"),
		Replicator->IsTrackedForTest(SubsystemID) && Sent.Num() == 1 && Sent[0].EntityID == SubsystemID);
	TestEqual(TEXT("which changed nothing here, so no notify fires again"), Subsystem->NotifiedOnRepCount, 1);

	Subsystem->RepNotified = 4;
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("a real change after that still fires it"), Subsystem->NotifiedOnRepCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterIdleLeaderSendsNoSubsystemKeyframeTest,
	"CrowdySDK.Leader.IdleLeaderSendsNoSubsystemKeyframe", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterIdleLeaderSendsNoSubsystemKeyframeTest::RunTest(const FString& Parameters)
{
	UCrowdyStateReplicator* Replicator = MakeReplicator(MakeStateRegistry(), FGuid::NewGuid());
	Replicator->SetKeyframeIntervalForTest(1.0f);
	double Now = 100.0;
	Replicator->SetTimeForTest(Now);
	TArray<FCrowdyStateDelta> Sent;
	Replicator->ChannelDispatchHookForTests = [&Sent](const FCrowdyStateDelta& Delta) { Sent.Add(Delta); };

	if (!TestTrue(TEXT("tracked"), Replicator->RegisterOwnedEntityForTest(FGuid::NewGuid(),
		NewObject<UCrowdyStateSubsystemTestTarget>())))
		return false;
	Replicator->RunReplicationLoopForTest();
	Sent.Reset();

	// The server counts a channel message as input, so a subsystem keyframe would keep an idle Leader from being released.
	Replicator->SetLocalPlayerIdle(true);
	Now += 5.0;
	Replicator->SetTimeForTest(Now);
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("an idle Leader sends no subsystem keyframe"), Sent.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderWriterWarnsWhenNothingWritesItTest,
	"CrowdySDK.Leader.WarnsWhenNothingWritesIt", CrowdyLeaderWriterTestFlags)
bool FCrowdyLeaderWriterWarnsWhenNothingWritesItTest::RunTest(const FString& Parameters)
{
	// Marks TargetClass, registered with no owner, dirty on a client that does not lead; true when that warned.
	auto MarkWarns = [](UClass* TargetClass)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitiesWithLeader(FGuid::NewGuid(), FGuid::NewGuid());
		UCrowdyStateReplicator* Replicator = MakeReplicator(MakeStateRegistry(), Entities->GetLocalPlayerID());
		Replicator->SetEntitySubsystemForTest(Entities);
		UObject* Target = NewObject<UObject>(GetTransientPackage(), TargetClass);
		const FGuid ID = Target->IsA<AActor>()
			? Entities->RegisterParticipant(Target, ECrowdyOwnership::Host)
			: Entities->RegisterParticipant(Target);
		Replicator->MarkStateDirty(ID, TEXT("RepInt"));
		return Replicator->HasWarnedWorldEntityHasNoWriterForTest();
	};

	TestTrue(TEXT("nothing writes a World actor, so marking it dirty warns"), MarkWarns(ACrowdyStateSendTestActor::StaticClass()));
	TestFalse(TEXT("a replicated subsystem has the Leader, so it does not"), MarkWarns(UCrowdyStateSubsystemTestTarget::StaticClass()));
	return true;
}

#endif
