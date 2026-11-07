#include "Replication/State/CrowdyContinuousTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/World.h"
#include "Messages/Actor/CrowdyUpdateExtras.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Replication/CrowdyBoundedMemoryReader.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateFragmentTestTypes.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Serialization/MemoryWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyContinuousTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TArray<uint8> MakeContinuousExtras(const FCrowdyRepLayout& Layout, const AActor& Source, const TBitArray<>& Bits)
	{
		TArray<uint8> Values;
		FCrowdyStateCodec::Encode(Layout, &Source, Bits, /*bKeyframe=*/false, Values);

		TArray<uint8> Extras;
		CrowdyUpdateExtras::AppendContinuousEntry(Extras, Layout.LayoutHash, Values);
		return Extras;
	}

	void RegisterContinuousEntity(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, AActor* Actor, const bool bOwned)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = EntityId;
		Record.OwnerID = bOwned ? Entities->GetLocalPlayerID() : FGuid::NewGuid();
		Record.Role = bOwned ? ECrowdyRole::Owner : ECrowdyRole::RemoteProxy;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);
	}

	FCrowdyActorUpdate MakeUpdate(const FGuid& UUID, const TArray<uint8>& Extras)
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

	float DecodedLean(const FCrowdyRepLayout& Layout, const void* Container)
	{
		const int32 Index = IndexOfPropertyName(Layout, TEXT("RepLean"));
		return *Layout.Properties[Index].Property->ContainerPtrToValuePtr<float>(Container);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousLayoutSettlesTheMarkerTest,
	"CrowdySDK.State.ContinuousLayoutSettlesTheMarker", CrowdyContinuousTestFlags)
bool FCrowdyContinuousLayoutSettlesTheMarkerTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyContinuousTestActor::StaticClass());
	if (!TestNotNull(TEXT("the moving class has a layout"), Layout))
	{
		return false;
	}

	const int32 Plain = IndexOfPropertyName(*Layout, TEXT("RepPlain"));
	const int32 Lean = IndexOfPropertyName(*Layout, TEXT("RepLean"));
	const int32 Stance = IndexOfPropertyName(*Layout, TEXT("RepStance"));
	TestTrue(TEXT("the marked properties are flagged"), Layout->Properties[Lean].bContinuous && Layout->Properties[Stance].bContinuous);
	TestFalse(TEXT("an unmarked one is not"), Layout->Properties[Plain].bContinuous);
	TestTrue(TEXT("the bits say the same"), Layout->HasContinuous() && Layout->ContinuousBits[Lean] && Layout->ContinuousBits[Stance] && !Layout->ContinuousBits[Plain]);

	AddExpectedErrorPlain(TEXT("does not replicate movement"), EAutomationExpectedErrorFlags::Contains, 1);
	const FCrowdyRepLayout* Still = Registry->FindRepLayout(ACrowdyContinuousStillTestActor::StaticClass());
	if (TestNotNull(TEXT("the still class keeps its property"), Still))
	{
		TestFalse(TEXT("as ordinary state, because nothing would carry it"), Still->HasContinuous() || Still->Properties[0].bContinuous);
	}

	AddExpectedErrorPlain(TEXT("together with CrowdyOwnerOnly or CrowdyManualDirty"), EAutomationExpectedErrorFlags::Contains, 2);
	const FCrowdyRepLayout* Conflict = Registry->FindRepLayout(ACrowdyContinuousConflictTestActor::StaticClass());
	if (TestNotNull(TEXT("the conflicting class keeps its properties"), Conflict))
	{
		TestEqual(TEXT("both of them"), Conflict->Properties.Num(), 2);
		TestFalse(TEXT("as ordinary state: a movement update reaches everyone and has its own clock"), Conflict->HasContinuous());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousSurvivesTheBakeTest,
	"CrowdySDK.State.ContinuousSurvivesTheBake", CrowdyContinuousTestFlags)
bool FCrowdyContinuousSurvivesTheBakeTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* Class = ACrowdyContinuousTestActor::StaticClass();
	const FCrowdyRepLayout* Live = Registry->FindRepLayout(Class);
	if (!TestNotNull(TEXT("layout"), Live))
	{
		return false;
	}

	const FSoftClassPath Path(Class);
	UCrowdyBakedRegistry* Baked = NewObject<UCrowdyBakedRegistry>(GetTransientPackage());
	UCrowdyBakedRegistry::MakeBakedRepProperties(*Live, Path, Baked->RepProperties);
	Baked->RepLayoutHashes.Add({ Path, Live->LayoutHash });

	FCrowdyRepLayout Assembled;
	if (!TestTrue(TEXT("the baked rows assemble"), UCrowdyAutoRegistry::BuildLayoutFromBaked(Class, Baked, Assembled)))
	{
		return false;
	}
	for (int32 Index = 0; Index < Live->Properties.Num(); ++Index)
	{
		TestEqual(TEXT("the flag survives, or a cooked build sends these as deltas"), Assembled.Properties[Index].bContinuous, Live->Properties[Index].bContinuous);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousDecodeRefusesOtherPropertiesTest,
	"CrowdySDK.State.ContinuousDecodeRefusesOtherProperties", CrowdyContinuousTestFlags)
bool FCrowdyContinuousDecodeRefusesOtherPropertiesTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyContinuousTestActor::StaticClass());
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	ACrowdyContinuousTestActor* Source = NewObject<ACrowdyContinuousTestActor>();
	Source->RepPlain = 7;
	Source->RepLean = 0.5f;

	TBitArray<> Mixed;
	Mixed.Init(true, Layout->Properties.Num());
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Mixed, /*bKeyframe=*/false, Blob);

	ACrowdyContinuousTestActor* Target = NewObject<ACrowdyContinuousTestActor>();
	TArray<int32> Changed;
	TestFalse(TEXT("a blob naming an ordinary property is refused"),
		FCrowdyStateCodec::Decode(*Layout, Layout->LayoutHash, Blob, Target, Changed, nullptr, nullptr, /*bContinuousOnly=*/true));
	TestEqual(TEXT("before its ordinary value was written"), Target->RepPlain, 0);
	TestEqual(TEXT("and before its continuous one was"), Target->RepLean, 0.f);

	FCrowdyStateCodec::Encode(*Layout, Source, Layout->ContinuousBits, /*bKeyframe=*/false, Blob);
	TestTrue(TEXT("continuous values alone pass"),
		FCrowdyStateCodec::Decode(*Layout, Layout->LayoutHash, Blob, Target, Changed, nullptr, nullptr, /*bContinuousOnly=*/true));
	TestEqual(TEXT("and are written"), Target->RepLean, 0.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousLeavesTheDeltasTest,
	"CrowdySDK.State.ContinuousLeavesTheDeltas", CrowdyContinuousTestFlags)
bool FCrowdyContinuousLeavesTheDeltasTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyContinuousTestActor::StaticClass());
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	UCrowdyStateReplicator* Replicator = NewObject<UCrowdyStateReplicator>(GetTransientPackage());
	Replicator->SetRegistryForTest(Registry);
	Replicator->SetLocalPlayerIDForTest(FGuid::NewGuid());
	Replicator->SetKeyframeIntervalForTest(1.0f);
	double Now = 100.0;
	Replicator->SetTimeForTest(Now);

	TArray<FCrowdyStateDelta> Sent;
	Replicator->DispatchHookForTests = [&Sent](const FCrowdyStateDelta& Delta, bool) { Sent.Add(Delta); };

	FCrowdyStateTestWorld TestWorld;
	ACrowdyContinuousTestActor* Actor = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	const FGuid EntityId = FGuid::NewGuid();
	if (!TestTrue(TEXT("tracked"), Replicator->RegisterOwnedEntityForTest(EntityId, Actor)))
	{
		return false;
	}
	Replicator->SetContinuousOnMovement(EntityId, true);
	Replicator->RunReplicationLoopForTest();
	Sent.Reset();

	auto PresentCount = [&](const FCrowdyStateDelta& Delta)
	{
		ACrowdyContinuousTestActor* Into = NewObject<ACrowdyContinuousTestActor>();
		TArray<int32> Changed;
		TArray<int32> Present;
		FCrowdyStateCodec::Decode(*Layout, Delta.LayoutHash, Delta.Blob, Into, Changed, &Present);
		return Present;
	};

	Actor->RepLean = 0.5f;
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("a continuous change sends no delta"), Sent.Num(), 0);
	TestEqual(TEXT("and the owner's own notify still runs"), Actor->LeanOnRepCount, 1);
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("once per change"), Actor->LeanOnRepCount, 1);

	Actor->RepPlain = 3;
	Replicator->RunReplicationLoopForTest();
	if (TestEqual(TEXT("an ordinary change still does"), Sent.Num(), 1))
	{
		const TArray<int32> Present = PresentCount(Sent[0]);
		TestEqual(TEXT("carrying that property alone"), Present.Num(), 1);
		TestTrue(TEXT("not the continuous one beside it"), Present.Num() == 1 && Present[0] == IndexOfPropertyName(*Layout, TEXT("RepPlain")));
	}
	Sent.Reset();

	Now += 5.0;
	Replicator->SetTimeForTest(Now);
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("a heartbeat-marked continuous property is not on the delta keyframe"), Sent.Num(), 0);

	// With its updates stopped nothing carries the value, so it goes back to being a delta.
	Replicator->SetContinuousOnMovement(EntityId, false);
	Actor->RepLean = 0.75f;
	Replicator->RunReplicationLoopForTest();
	if (TestTrue(TEXT("without the movement stream the value goes as a delta"), Sent.Num() > 0))
	{
		TestTrue(TEXT("naming the continuous property"), PresentCount(Sent[0]).Contains(IndexOfPropertyName(*Layout, TEXT("RepLean"))));
	}
	Sent.Reset();

	// Off the stream a keyframe must carry it, heartbeat mark or not: a host's World entity has no other way out.
	Now += 5.0;
	Replicator->SetTimeForTest(Now);
	Replicator->RunReplicationLoopForTest();
	if (TestEqual(TEXT("the keyframe goes"), Sent.Num(), 1))
	{
		const TArray<int32> Present = PresentCount(Sent[0]);
		TestTrue(TEXT("with the continuous property that has no heartbeat mark"), Present.Contains(IndexOfPropertyName(*Layout, TEXT("RepLean"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousGoesOnChangeAndKeyframeTest,
	"CrowdySDK.Replication.ContinuousGoesOnChangeAndKeyframe", CrowdyContinuousTestFlags)
bool FCrowdyContinuousGoesOnChangeAndKeyframeTest::RunTest(const FString&)
{
	using CrowdyAutoReplication::ShouldCarryExtras;

	TestFalse(TEXT("steady values stay off an update that only moves"), ShouldCarryExtras(true, false, false, true));
	TestTrue(TEXT("a change goes"), ShouldCarryExtras(true, true, false, true));
	TestTrue(TEXT("a keyframe repeats them for a late joiner"), ShouldCarryExtras(true, false, true, true));
	TestTrue(TEXT("the first send carries them"), ShouldCarryExtras(true, false, false, false));
	TestTrue(TEXT("extras set by hand go on every update"), ShouldCarryExtras(false, false, false, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBoundedReaderRefusesAnOversizedNameTest,
	"CrowdySDK.State.BoundedReaderRefusesAnOversizedName", CrowdyContinuousTestFlags)
bool FCrowdyBoundedReaderRefusesAnOversizedNameTest::RunTest(const FString&)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, /*bIsPersistent=*/true);
	FString Forged = FString::ChrN(NAME_SIZE, TEXT('a'));
	Writer << Forged;

	FCrowdyBoundedMemoryReader Reader(Bytes, /*bIsPersistent=*/true);
	FName Name;
	Reader << Name;
	TestTrue(TEXT("a name past the engine's limit fails the read instead of the process"), Reader.IsError());
	TestTrue(TEXT("and yields nothing"), Name.IsNone());

	TArray<uint8> Good;
	FMemoryWriter GoodWriter(Good, /*bIsPersistent=*/true);
	FName Stance(TEXT("Crouched"));
	GoodWriter << Stance;

	FCrowdyBoundedMemoryReader GoodReader(Good, /*bIsPersistent=*/true);
	FName Read;
	GoodReader << Read;
	TestEqual(TEXT("an ordinary name still reads"), Read, Stance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousFillsTheUpdateExtrasTest,
	"CrowdySDK.Replication.ContinuousFillsTheUpdateExtras", CrowdyContinuousTestFlags)
bool FCrowdyContinuousFillsTheUpdateExtrasTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* Class = ACrowdyContinuousTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Class);
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	FCrowdyStateTestWorld TestWorld;
	ACrowdyContinuousTestActor* Actor = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	UCrowdyAutoReplicator* Replicator = NewObject<UCrowdyAutoReplicator>(Actor->GetWorld());
	Replicator->SetAutoRegistryForTest(Registry);

	// The bare actor was never set up as an entity class; that does not change what it captures.
	AddExpectedErrorPlain(TEXT("has no entity class id"), EAutomationExpectedErrorFlags::Contains, 1);
	Replicator->RegisterReplicationComponent(Actor->Entity, /*bPresenceOnly=*/false);

	Actor->RepLean = 0.5f;
	TestTrue(TEXT("the first capture is a change"), Replicator->CaptureContinuousForTest(Actor->Entity, *Actor));
	TestFalse(TEXT("the same values are not"), Replicator->CaptureContinuousForTest(Actor->Entity, *Actor));

	int64 LayoutHash = 0;
	TConstArrayView<uint8> Values;
	const TArray<uint8> Extras = Replicator->GetExtrasForTest(Actor->Entity);
	TestTrue(TEXT("the extras hold one continuous entry"), CrowdyUpdateExtras::ReadContinuousEntry(
		CrowdyUpdateExtras::FindEntry(Extras, CrowdyUpdateExtras::ContinuousType), LayoutHash, Values));
	TestEqual(TEXT("under the class's layout hash"), LayoutHash, Layout->LayoutHash);

	ACrowdyContinuousTestActor* Into = NewObject<ACrowdyContinuousTestActor>();
	TArray<int32> Changed;
	TestTrue(TEXT("whose values decode"), FCrowdyStateCodec::Decode(*Layout, LayoutHash, TArray<uint8>(Values), Into, Changed,
		nullptr, nullptr, /*bContinuousOnly=*/true));
	TestEqual(TEXT("to what the actor holds"), Into->RepLean, 0.5f);

	Actor->RepPlain = 9;
	TestFalse(TEXT("an ordinary property is none of its business"), Replicator->CaptureContinuousForTest(Actor->Entity, *Actor));
	Actor->RepStance = 2;
	TestTrue(TEXT("a continuous one is a change"), Replicator->CaptureContinuousForTest(Actor->Entity, *Actor));

	TestFalse(TEXT("SetExtras would be overwritten here, so it refuses"), Replicator->SetExtras(Actor->Entity, {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousReachesTheActorTest,
	"CrowdySDK.State.ContinuousReachesTheActor", CrowdyContinuousTestFlags)
bool FCrowdyContinuousReachesTheActorTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyContinuousTestActor::StaticClass());
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;
	ACrowdyContinuousTestActor* Proxy = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	ACrowdyContinuousTestActor* Owned = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	const FGuid ProxyId = FGuid::NewGuid();
	const FGuid OwnedId = FGuid::NewGuid();
	RegisterContinuousEntity(Entities, ProxyId, Proxy, /*bOwned=*/false);
	RegisterContinuousEntity(Entities, OwnedId, Owned, /*bOwned=*/true);

	ACrowdyContinuousTestActor* Source = NewObject<ACrowdyContinuousTestActor>();
	Source->RepLean = 0.5f;
	Source->RepStance = 2;
	const TArray<uint8> Extras = MakeContinuousExtras(*Layout, *Source, Layout->ContinuousBits);

	Router->DispatchContinuousExtras(ProxyId, Extras);
	TestEqual(TEXT("the value lands"), Proxy->RepLean, 0.5f);
	TestEqual(TEXT("each of them"), Proxy->RepStance, 2);
	TestEqual(TEXT("and its notify fires"), Proxy->LeanOnRepCount, 1);

	Router->DispatchContinuousExtras(ProxyId, Extras);
	TestEqual(TEXT("a keyframe repeating it fires nothing"), Proxy->LeanOnRepCount, 1);

	Router->DispatchContinuousExtras(OwnedId, Extras);
	TestEqual(TEXT("the owner's own echo writes nothing"), Owned->RepLean, 0.f);

	Source->RepLean = 0.75f;
	TArray<uint8> Values;
	FCrowdyStateCodec::Encode(*Layout, Source, Layout->ContinuousBits, /*bKeyframe=*/false, Values);
	TArray<uint8> WrongLayout;
	CrowdyUpdateExtras::AppendContinuousEntry(WrongLayout, Layout->LayoutHash + 1, Values);
	Router->DispatchContinuousExtras(ProxyId, WrongLayout);
	TestEqual(TEXT("another layout's values are ignored"), Proxy->RepLean, 0.5f);

	TBitArray<> Mixed;
	Mixed.Init(true, Layout->Properties.Num());
	Source->RepPlain = 9;
	Router->DispatchContinuousExtras(ProxyId, MakeContinuousExtras(*Layout, *Source, Mixed));
	TestEqual(TEXT("a peer cannot reach an ordinary property this way"), Proxy->RepPlain, 0);
	TestEqual(TEXT("nor land the rest of that entry"), Proxy->RepLean, 0.5f);

	const TArray<uint8> Other = { 2, 0, 1, 0, 0xAA };
	Router->DispatchContinuousExtras(ProxyId, Other);
	TestEqual(TEXT("an entry of another type is not read"), Proxy->RepLean, 0.5f);

	ACrowdyContinuousTestActor* World = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	const FGuid WorldId = FGuid::NewGuid();
	FCrowdyEntityRecord WorldRecord;
	WorldRecord.NetID = WorldId;
	WorldRecord.Role = ECrowdyRole::HostOwned;
	WorldRecord.Participant = World;
	Entities->RegisterEntity(WorldRecord);
	Router->DispatchContinuousExtras(WorldId, Extras);
	TestEqual(TEXT("an update names no sender, so it cannot write a World entity"), World->RepLean, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousReachesTheSubscriberTest,
	"CrowdySDK.State.ContinuousReachesTheSubscriber", CrowdyContinuousTestFlags)
bool FCrowdyContinuousReachesTheSubscriberTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* Class = ACrowdyContinuousTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Class);
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>(GetTransientPackage());
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	const FGuid RowId = FGuid::NewGuid();
	Subscriber->SetOwnedClass(RowId, Class);

	float Received = 0.f;
	Subscriber->OnApply = [&](const FGuid&, const FCrowdyRepLayout& Applied, const void* Container, TConstArrayView<int32>)
	{
		Received = DecodedLean(Applied, Container);
	};

	ACrowdyContinuousTestActor* Source = NewObject<ACrowdyContinuousTestActor>();
	Source->RepLean = 0.5f;
	const TArray<uint8> Extras = MakeContinuousExtras(*Layout, *Source, Layout->ContinuousBits);

	Router->DispatchContinuousExtras(RowId, Extras);
	TestEqual(TEXT("a row gets the values, though the update names no class"), Subscriber->ApplyDecodedStateCallCount, 1);
	TestEqual(TEXT("decoded"), Received, 0.5f);

	Router->DispatchContinuousExtras(FGuid::NewGuid(), Extras);
	TestEqual(TEXT("an entity nobody holds is dropped, never waited for"), Router->NumDeferredForTest(), 0);

	TBitArray<> Mixed;
	Mixed.Init(true, Layout->Properties.Num());
	Router->DispatchContinuousExtras(RowId, MakeContinuousExtras(*Layout, *Source, Mixed));
	TestEqual(TEXT("an ordinary property is refused here too"), Subscriber->ApplyDecodedStateCallCount, 1);

	TArray<uint8> Values;
	FCrowdyStateCodec::Encode(*Layout, Source, Layout->ContinuousBits, /*bKeyframe=*/false, Values);
	TArray<uint8> WrongLayout;
	CrowdyUpdateExtras::AppendContinuousEntry(WrongLayout, Layout->LayoutHash + 1, Values);
	Router->DispatchContinuousExtras(RowId, WrongLayout);
	TestEqual(TEXT("and so is another layout's"), Subscriber->ApplyDecodedStateCallCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContinuousRidesTheAppliedUpdateTest,
	"CrowdySDK.Replication.ContinuousRidesTheAppliedUpdate", CrowdyContinuousTestFlags)
bool FCrowdyContinuousRidesTheAppliedUpdateTest::RunTest(const FString&)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyContinuousTestActor::StaticClass());
	if (!TestNotNull(TEXT("layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;
	ACrowdyContinuousTestActor* Applied = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	ACrowdyContinuousTestActor* Appearing = TestWorld.Spawn<ACrowdyContinuousTestActor>();
	const FGuid AppliedId = FGuid::NewGuid();
	const FGuid AppearingId = FGuid::NewGuid();
	RegisterContinuousEntity(Entities, AppliedId, Applied, /*bOwned=*/false);
	RegisterContinuousEntity(Entities, AppearingId, Appearing, /*bOwned=*/false);

	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());
	Manager->SetEventRouterForTest(Router);
	Manager->BindToTrackerForTest(Tracker.Get());
	Manager->AllocateSlotForTest(AppliedId);
	Manager->AllocateSlotForTest(AppearingId);

	ACrowdyContinuousTestActor* Source = NewObject<ACrowdyContinuousTestActor>();
	Source->RepLean = 0.5f;
	const TArray<uint8> Extras = MakeContinuousExtras(*Layout, *Source, Layout->ContinuousBits);

	TArray<FCrowdyActorUpdate> Batch;
	Batch.Add(MakeUpdate(AppliedId, Extras));
	Manager->DrainUpdateBatchForTest(Batch);
	TestEqual(TEXT("an applied update delivers its values"), Applied->RepLean, 0.5f);

	// That update is never applied, so without this a value sent once never reaches a late joiner.
	Tracker->SetupQueuesForTest();
	Tracker->QueueAppearancesForTest({ MakeUpdate(AppearingId, Extras) });
	TestEqual(TEXT("an appearance waits for the tracker's tick, outside Mass processing"), Appearing->RepLean, 0.f);
	Tracker->FlushAppearancesForTest();
	TestEqual(TEXT("the update that makes an entity appear delivers them too"), Appearing->RepLean, 0.5f);

	int32 Announced = 0;
	Manager->OnUpdateExtras.AddLambda([&Announced](const FGuid&, int32, TConstArrayView<uint8>) { ++Announced; });
	Tracker->OnRemoteEntityAppearedExtras.Broadcast(FGuid::NewGuid(), Extras);
	TestEqual(TEXT("an entity with no slot has nothing to apply them to"), Announced, 0);
	return true;
}

#endif
