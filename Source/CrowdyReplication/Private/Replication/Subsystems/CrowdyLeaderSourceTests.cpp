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
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UCrowdyClassRegistry.h"

// The Leader Source setting decides who writes World-owned entities: the elected host writes all of them, or the
// Leader writes replicated subsystems and nobody writes World actors. These cases cover that every ownership
// question gets the same answer from it, that such state is taken only from that writer, and that a project relying
// on a writer it no longer has is told so.
namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderSourceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyEntitySubsystem* MakeEntitiesWithHost(const FGuid& LocalPlayer, const FGuid& HostID)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
		Entities->SetGameSessionForTest(MakeGameSession(HostID));
		return Entities;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceWorldOwnerFollowsTheSettingTest,
	"CrowdySDK.LeaderSource.WorldOwnerFollowsTheSetting", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceWorldOwnerFollowsTheSettingTest::RunTest(const FString& Parameters)
{
	const FGuid HostID = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(FGuid::NewGuid(), HostID);

	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	TestEqual(TEXT("Elected Host: the elected host writes World entities"), Entities->GetWorldOwnerID(), HostID);

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestFalse(TEXT("Server Object: nothing writes World entities"), Entities->GetWorldOwnerID().IsValid());
	TestEqual(TEXT("the elected host is still known, only no longer used"), Entities->GetAppWriterID(), HostID);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceOneOwnershipAnswerTest,
	"CrowdySDK.LeaderSource.OneOwnershipAnswer", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceOneOwnershipAnswerTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();

	TestTrue(TEXT("an Owner record naming this client is written here"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, Local, Local, FGuid()));
	TestFalse(TEXT("an Owner record naming another client is not"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, Other, Local, FGuid()));
	TestFalse(TEXT("a remote proxy is not, whatever it names"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::RemoteProxy, Local, Local, Local));
	TestTrue(TEXT("a World record is written by its World owner"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), Local, Local));
	TestFalse(TEXT("and by nobody when there is none"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), Local, FGuid()));
	TestTrue(TEXT("an Owner record registered before the local player id arrived is still this client's"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::Owner, FGuid(), Local, FGuid()));
	TestFalse(TEXT("a client with no player id is no World owner"),
		FCrowdyEntityRecord::IsWrittenBy(ECrowdyRole::HostOwned, FGuid(), FGuid(), FGuid()));

	// This client is the elected host, and a World participant and a World actor are registered here.
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(Local, Local);
	UCrowdyStateSubsystemTestTarget* Participant = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid WorldID = Entities->RegisterParticipant(Participant, ECrowdyOwnership::Host);

	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	Component->SetCollaboratorsForTest(Entities, nullptr);
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);

	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	TestTrue(TEXT("Elected Host: the registry says the host writes a World entity"), Entities->IsLocallyOwned(WorldID));
	TestTrue(TEXT("and the component gives the same answer"), Component->IsLocallyOwned());

	// Under Server Object the host store holds the Leader, here this client.
	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestTrue(TEXT("Server Object: the Leader writes a replicated subsystem"), Entities->IsLocallyOwned(WorldID));
	TestFalse(TEXT("but nobody writes a World actor"), Component->IsLocallyOwned());
	UCrowdyEntitySubsystem* Follower = MakeEntitiesWithHost(Local, FGuid::NewGuid());
	const FGuid FollowerWorldID = Follower->RegisterParticipant(NewObject<UCrowdyStateSubsystemTestTarget>(), ECrowdyOwnership::Host);
	TestFalse(TEXT("and a client another player leads does not write it"), Follower->IsLocallyOwned(FollowerWorldID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceWorldDeltaNeedsAHostTest,
	"CrowdySDK.LeaderSource.WorldDeltaNeedsAHost", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceWorldDeltaNeedsAHostTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FGuid HostID = FGuid::NewGuid();

	// Each case gets a fresh receiver of TargetClass, registered as World-owned, so a value applied by one cannot be
	// mistaken for the next one's. Returns the receiver's RepInt, which the change sets to 42.
	auto Receive = [&](UClass* TargetClass, const uint8 Flags) -> int32
	{
		const FCrowdyRepLayout* Layout = Registry ? Registry->FindRepLayout(TargetClass) : nullptr;
		const int32 RepIntIndex = Layout ? IndexOfPropertyName(*Layout, TEXT("RepInt")) : INDEX_NONE;
		const FIntProperty* RepInt = CastField<FIntProperty>(TargetClass->FindPropertyByName(TEXT("RepInt")));
		if (RepIntIndex == INDEX_NONE || !RepInt)
		{
			AddError(FString::Printf(TEXT("%s has no RepInt in its layout"), *TargetClass->GetName()));
			return -1;
		}
		UObject* Source = NewObject<UObject>(GetTransientPackage(), TargetClass);
		RepInt->SetPropertyValue_InContainer(Source, 42);
		TBitArray<> Dirty;
		Dirty.Init(false, Layout->Properties.Num());
		Dirty[RepIntIndex] = true;

		UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(FGuid::NewGuid(), HostID);
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);
		UObject* Target = NewObject<UObject>(GetTransientPackage(), TargetClass);
		FCrowdyStateDelta Delta;
		Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
		Delta.EntityID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);
		Delta.SenderID = HostID;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = Flags;
		FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Delta.Blob);
		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		return RepInt->GetPropertyValue_InContainer(Target);
	};
	UClass* const Subsystem = UCrowdyStateSubsystemTestTarget::StaticClass();
	UClass* const Actor = ACrowdyStateSendTestActor::StaticClass();

	// Clients learn of a new host at different times, so the host flag decides, not the sender.
	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	TestEqual(TEXT("Elected Host: a replicated subsystem's change carrying the host flag applies"), Receive(Subsystem, CrowdyStateDeltaFlags::HostSourced), 42);
	TestEqual(TEXT("one without it is ignored"), Receive(Subsystem, 0), 0);
	TestEqual(TEXT("so does a World actor's"), Receive(Actor, CrowdyStateDeltaFlags::HostSourced), 42);

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestEqual(TEXT("Server Object: the Leader writes a replicated subsystem, so its flagged change applies"), Receive(Subsystem, CrowdyStateDeltaFlags::HostSourced), 42);
	TestEqual(TEXT("one without the flag is still ignored"), Receive(Subsystem, 0), 0);
	TestEqual(TEXT("nothing writes a World actor, so even its flagged change is ignored"), Receive(Actor, CrowdyStateDeltaFlags::HostSourced), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceLeaderWritesItsSubsystemsTest,
	"CrowdySDK.LeaderSource.LeaderWritesItsSubsystems", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceLeaderWritesItsSubsystemsTest::RunTest(const FString& Parameters)
{
	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ServerObject);
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(Local, Local);
	UCrowdyStateReplicator* Replicator = MakeReplicator(MakeStateRegistry(), Local);
	Replicator->SetEntitySubsystemForTest(Entities);
	TArray<FCrowdyStateDelta> Sent;
	Replicator->ChannelDispatchHookForTests = [&Sent](const FCrowdyStateDelta& Delta) { Sent.Add(Delta); };

	UCrowdyStateSubsystemTestTarget* Subsystem = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid SubsystemID = Entities->RegisterParticipant(Subsystem, ECrowdyOwnership::Host);
	const FGuid ActorID = Entities->RegisterParticipant(NewObject<ACrowdyStateSendTestActor>(GetTransientPackage()), ECrowdyOwnership::Host);
	Replicator->HandleEntityRegisteredForTest(SubsystemID);
	Replicator->HandleEntityRegisteredForTest(ActorID);
	TestTrue(TEXT("the Leader tracks a replicated subsystem"), Replicator->IsTrackedForTest(SubsystemID));
	TestFalse(TEXT("but not a World actor, which nothing writes"), Replicator->IsTrackedForTest(ActorID));

	Subsystem->RepInt = 5;
	Replicator->RunReplicationLoopForTest();
	TestTrue(TEXT("its change goes out flagged as the writer's"), Sent.Num() == 1
		&& (Sent[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0 && Sent[0].EntityID == SubsystemID);

	const FGuid Other = FGuid::NewGuid();
	Replicator->HandleHostChangedForTest(Other, Local);
	TestFalse(TEXT("losing the lead stops it writing the subsystem"), Replicator->IsTrackedForTest(SubsystemID));

	// The new writer's shadow starts at each property's zero, so taking the lead back re-sends every nonzero value.
	Sent.Reset();
	Replicator->HandleHostChangedForTest(Local, Other);
	Replicator->RunReplicationLoopForTest();
	TestTrue(TEXT("taking the lead again tracks it and re-sends its nonzero values"),
		Replicator->IsTrackedForTest(SubsystemID) && Sent.Num() == 1 && Sent[0].EntityID == SubsystemID);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceWarnsWhenNothingWritesItTest,
	"CrowdySDK.LeaderSource.WarnsWhenNothingWritesIt", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceWarnsWhenNothingWritesItTest::RunTest(const FString& Parameters)
{
	// Marks a World-owned TargetClass dirty on a client that neither leads nor hosts; true when that warned.
	auto MarkWarns = [](UClass* TargetClass)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(FGuid::NewGuid(), FGuid::NewGuid());
		UCrowdyStateReplicator* Replicator = MakeReplicator(MakeStateRegistry(), Entities->GetLocalPlayerID());
		Replicator->SetEntitySubsystemForTest(Entities);
		const FGuid WorldID = Entities->RegisterParticipant(NewObject<UObject>(GetTransientPackage(), TargetClass), ECrowdyOwnership::Host);
		Replicator->MarkStateDirty(WorldID, TEXT("RepInt"));
		return Replicator->HasWarnedWorldEntityHasNoWriterForTest();
	};
	UClass* const Subsystem = UCrowdyStateSubsystemTestTarget::StaticClass();
	UClass* const Actor = ACrowdyStateSendTestActor::StaticClass();

	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	TestFalse(TEXT("Elected Host: a World actor has a writer, so marking it dirty does not warn"), MarkWarns(Actor));
	TestFalse(TEXT("nor does a replicated subsystem"), MarkWarns(Subsystem));

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestTrue(TEXT("Server Object: nothing writes a World actor, so marking it dirty warns"), MarkWarns(Actor));
	TestFalse(TEXT("a replicated subsystem has the Leader, so it does not"), MarkWarns(Subsystem));
	return true;
}

#endif
