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

// The Leader Source setting decides who writes World entities: the elected host, or nobody. These cases cover that
// every ownership question gets the same answer from it, that a World entity's state is taken only from that writer,
// and that a project relying on a writer it no longer has is told so.
namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderSourceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FCrowdyScopedLeaderSource
	{
		ECrowdyLeaderSource Previous;

		explicit FCrowdyScopedLeaderSource(const ECrowdyLeaderSource Source)
			: Previous(GetDefault<UCrowdySDKDeveloperSettings>()->LeaderSource)
		{
			Set(Source);
		}

		~FCrowdyScopedLeaderSource() { Set(Previous); }

		static void Set(const ECrowdyLeaderSource Source)
		{
			GetMutableDefault<UCrowdySDKDeveloperSettings>()->LeaderSource = Source;
		}
	};

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
	TestEqual(TEXT("the elected host is still known, only no longer used"), Entities->GetHostID(), HostID);
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

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestFalse(TEXT("Server Object: the registry says nobody writes it"), Entities->IsLocallyOwned(WorldID));
	TestFalse(TEXT("and the component gives the same answer"), Component->IsLocallyOwned());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceWorldDeltaNeedsAHostTest,
	"CrowdySDK.LeaderSource.WorldDeltaNeedsAHost", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceWorldDeltaNeedsAHostTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateSubsystemTestTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry ? Registry->FindRepLayout(TargetClass) : nullptr;
	if (!TestNotNull(TEXT("the test target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestTrue(TEXT("RepInt is in the layout"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}

	UCrowdyStateSubsystemTestTarget* Source = NewObject<UCrowdyStateSubsystemTestTarget>();
	Source->RepInt = 42;
	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	const FGuid HostID = FGuid::NewGuid();

	// Each case gets a fresh receiver, so a value applied by one cannot be mistaken for the next one's.
	auto Receive = [&](const FGuid& Local, const FGuid& Sender, const uint8 Flags) -> int32
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitiesWithHost(Local, HostID);
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);
		UCrowdyStateSubsystemTestTarget* Target = NewObject<UCrowdyStateSubsystemTestTarget>();
		const FGuid EntityID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);

		FCrowdyStateDelta Delta;
		Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
		Delta.EntityID = EntityID;
		Delta.SenderID = Sender;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = Flags;
		Delta.Blob = Blob;
		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		return Target->RepInt;
	};

	// Clients learn of a new host at different times, so under Elected Host the host flag decides, not the sender.
	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	TestEqual(TEXT("Elected Host: a change carrying the host flag applies"),
		Receive(FGuid::NewGuid(), HostID, CrowdyStateDeltaFlags::HostSourced), 42);
	TestEqual(TEXT("one without it is ignored"), Receive(FGuid::NewGuid(), HostID, 0), 0);

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	TestEqual(TEXT("Server Object: nothing writes a World entity, so even a host-flagged change is ignored"),
		Receive(FGuid::NewGuid(), HostID, CrowdyStateDeltaFlags::HostSourced), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSourceWarnsWhenNothingWritesItTest,
	"CrowdySDK.LeaderSource.WarnsWhenNothingWritesIt", CrowdyLeaderSourceTestFlags)
bool FCrowdyLeaderSourceWarnsWhenNothingWritesItTest::RunTest(const FString& Parameters)
{
	auto RegisterAndMark = [](UCrowdyEntitySubsystem*& OutEntities, UCrowdyStateReplicator*& OutReplicator)
	{
		OutEntities = MakeEntitiesWithHost(FGuid::NewGuid(), FGuid::NewGuid());
		OutReplicator = MakeReplicator(MakeStateRegistry(), OutEntities->GetLocalPlayerID());
		OutReplicator->SetEntitySubsystemForTest(OutEntities);
		const FGuid WorldID = OutEntities->RegisterParticipant(NewObject<UCrowdyStateSubsystemTestTarget>(), ECrowdyOwnership::Host);
		OutReplicator->MarkStateDirty(WorldID, TEXT("RepInt"));
	};

	UCrowdyEntitySubsystem* Entities = nullptr;
	UCrowdyStateReplicator* Replicator = nullptr;

	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	RegisterAndMark(Entities, Replicator);
	TestFalse(TEXT("Elected Host: a replicated subsystem has a writer, so no warning"),
		Entities->HasWarnedParticipantHasNoWriterForTest());
	TestFalse(TEXT("nor for marking a World entity dirty"), Replicator->HasWarnedWorldEntityHasNoWriterForTest());

	FCrowdyScopedLeaderSource::Set(ECrowdyLeaderSource::ServerObject);
	RegisterAndMark(Entities, Replicator);
	TestTrue(TEXT("Server Object: registering a replicated subsystem warns"), Entities->HasWarnedParticipantHasNoWriterForTest());
	TestTrue(TEXT("and so does marking a World entity dirty"), Replicator->HasWarnedWorldEntityHasNoWriterForTest());
	return true;
}

#endif
