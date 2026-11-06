#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyActorUpdateExtrasTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyActorUpdate MakeExtrasTestUpdate(const FGuid& UUID, const TArray<uint8>& Extras)
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
			MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->State = FInstancedStruct::Make(FCrowdyActorState());
		Message->Extras = Extras;

		FCrowdyActorUpdate Update;
		Update.UUID = UUID;
		Update.ServerTimestamp = 1000;
		Update.Message = Message;
		return Update;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateExtrasReachTheDrainTest,
	"CrowdySDK.Replication.ActorUpdateExtrasReachTheDrain",
	CrowdyActorUpdateExtrasTestFlags)

bool FCrowdyActorUpdateExtrasReachTheDrainTest::RunTest(const FString&)
{
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());

	const FGuid WithExtras = FGuid::NewGuid();
	const FGuid Without = FGuid::NewGuid();
	Manager->AllocateSlotForTest(WithExtras);
	Manager->AllocateSlotForTest(Without);

	TArray<FGuid> SeenIds;
	TArray<TArray<uint8>> SeenExtras;
	Manager->OnUpdateExtras.AddLambda([&](const FGuid& UUID, int32, const TConstArrayView<uint8> Extras)
	{
		SeenIds.Add(UUID);
		SeenExtras.Emplace(Extras);
	});

	const TArray<uint8> Extras = { 1, 0, 2, 0, 0xAA, 0xBB };
	TArray<FCrowdyActorUpdate> Batch;
	Batch.Add(MakeExtrasTestUpdate(WithExtras, Extras));
	Batch.Add(MakeExtrasTestUpdate(Without, {}));
	Manager->DrainUpdateBatchForTest(Batch);

	TestEqual(TEXT("both updates reached the backend"), Backend->ExtractedStates.Num(), 2);
	if (!TestEqual(TEXT("only the update with extras was announced"), SeenIds.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("under its own id"), SeenIds[0] == WithExtras);
	TestTrue(TEXT("with its extras exactly"), SeenExtras[0] == Extras);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorSetExtrasRefusesTest,
	"CrowdySDK.Replication.AutoReplicatorSetExtrasRefuses",
	CrowdyActorUpdateExtrasTestFlags)

bool FCrowdyAutoReplicatorSetExtrasRefusesTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};

	UCrowdyAutoReplicator* Replicator = NewObject<UCrowdyAutoReplicator>(World);
	auto MakeComponent = [World]() -> UCrowdyEntityComponent*
	{
		const AActor* Actor = World->SpawnActor<AActor>(ACrowdyStateOverlapActor::StaticClass());
		return Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	};

	UCrowdyEntityComponent* Sending = MakeComponent();
	UCrowdyEntityComponent* Presence = MakeComponent();
	UCrowdyEntityComponent* Unregistered = MakeComponent();
	if (!TestNotNull(TEXT("sending"), Sending) || !TestNotNull(TEXT("presence"), Presence) || !TestNotNull(TEXT("unregistered"), Unregistered))
	{
		return false;
	}
	// The bare actors were never set up as entity classes; that does not change what SetExtras accepts.
	AddExpectedErrorPlain(TEXT("has no entity class id"), EAutomationExpectedErrorFlags::Contains, 2);
	Replicator->RegisterReplicationComponent(Sending, /*bPresenceOnly=*/false);
	Replicator->RegisterReplicationComponent(Presence, /*bPresenceOnly=*/true);

	const TArray<uint8> Valid = { 1, 0, 2, 0, 0xAA, 0xBB };
	TestTrue(TEXT("an entry that sends full updates takes valid extras"), Replicator->SetExtras(Sending, Valid));
	TestTrue(TEXT("and takes empty ones, which clear them"), Replicator->SetExtras(Sending, {}));
	TestFalse(TEXT("a presence entry, which never sends a full update, refuses them"), Replicator->SetExtras(Presence, Valid));
	TestFalse(TEXT("bytes that do not read as entries are refused"), Replicator->SetExtras(Sending, TArray<uint8>({ 1, 0, 5 })));
	TestFalse(TEXT("a component not registered here is refused"), Replicator->SetExtras(Unregistered, Valid));
	return true;
}

#endif
