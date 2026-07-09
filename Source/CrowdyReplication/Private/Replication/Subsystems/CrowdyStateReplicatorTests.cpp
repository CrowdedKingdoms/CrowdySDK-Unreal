#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Script.h"
#include "Core/FCrowdyTypeID.h"
#include "CrowdyReplicationLog.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateReplicatorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package
	// NewObject trips a ClassWithin ensure, so outer it to a bare GameInstance (Phase 1 idiom).
	UCrowdyAutoRegistry* MakeStateRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UCrowdyAutoRegistry>(GameInstance);
	}

	// The replicator is a UWorldSubsystem; a plain transient-package NewObject is fine on the hook path
	// (Initialize/Tick are never driven, so no world/GameInstance is touched).
	UCrowdyStateReplicator* MakeReplicator(UCrowdyAutoRegistry* Registry, const FGuid& LocalPlayer)
	{
		UCrowdyStateReplicator* Rep = NewObject<UCrowdyStateReplicator>(GetTransientPackage());
		Rep->SetRegistryForTest(Registry);
		Rep->SetLocalPlayerIDForTest(LocalPlayer);
		return Rep;
	}

	// The host-lifecycle tests need a REAL entity subsystem so FindRecord/GetLocalPlayerID resolve. It is a
	// UWorldSubsystem too, so a transient-package NewObject is fine (Initialize is never driven; RegisterEntity
	// only needs the game thread, which the automation runner provides).
	UCrowdyEntitySubsystem* MakeEntitySubsystem(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		ES->SetLocalPlayerID(LocalPlayer);
		return ES;
	}

	// A real game session carrying an elected host, mirroring the persisted host that survives level travel.
	// UCrowdyGameSession is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so it needs a GameInstance outer.
	UCrowdyGameSession* MakeGameSession(const FGuid& HostID)
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(GameInstance);
		Session->SetHostID(HostID);
		return Session;
	}

	// A HostOwned record for the given actor with a fresh NetID.
	FCrowdyEntityRecord MakeHostOwnedRecord(AActor* Actor, const FGuid& NetID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.Role = ECrowdyRole::HostOwned;
		Record.Participant = Actor;
		return Record;
	}

	// Registers an actor as a resolvable remote-proxy entity so FindEntity(id) returns it, WITHOUT the replicator
	// tracking it. The Phase C untracked-host-push tests need FindEntity to resolve the target while OwnedEntities
	// stays empty, so MarkStateDirty takes the untracked one-shot push path.
	void RegisterUntrackedProxy(UCrowdyEntitySubsystem* ES, const FGuid& Id, AActor* Actor)
	{
		FCrowdyEntityRecord Rec;
		Rec.NetID = Id;
		Rec.Role = ECrowdyRole::RemoteProxy;
		Rec.OwnerID = FGuid::NewGuid();
		Rec.Participant = Actor;
		ES->RegisterEntity(Rec);
	}

	// A minimal EDITOR world so GetOrCreateLoopbackMirror can SpawnActorDeferred/FinishSpawning a mirror (it
	// early-returns null with no world). Same idiom as CrowdyStateApplyTests.cpp's FCrowdyStateTestWorld: an
	// editor-type world sidesteps the project's PIE/Game-gated world subsystems (e.g. CrowdyMass asserts on
	// teardown of a bare Game world that never initialized it) and tears down cleanly.
	struct FCrowdyStateTestWorld
	{
		FEditorScriptExecutionGuard ScriptGuard;
		UWorld* World = nullptr;

		FCrowdyStateTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyStateTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		template <typename T>
		T* Spawn()
		{
			return World->SpawnActor<T>();
		}
	};
}

// The fixed wire payload registers and resolves to a stable non-zero EventType, mirroring the RPC one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateDeltaPayloadRegisteredTest,
	"CrowdySDK.State.StateDeltaPayloadRegistered", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateDeltaPayloadRegisteredTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyStateDelta::StaticStruct());

	FCrowdyTypeID ID = CROWDY_INVALID_TYPE_ID;
	const bool bResolved = UEventPayloadRegistry::Get()->GetID(FCrowdyStateDelta::StaticStruct(), ID);
	TestTrue(TEXT("FCrowdyStateDelta resolves to a payload id"), bResolved);
	TestNotEqual(TEXT("payload id is non-zero"), ID, static_cast<FCrowdyTypeID>(CROWDY_INVALID_TYPE_ID));

	// Idempotent: re-registering yields the same id.
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyStateDelta::StaticStruct());
	FCrowdyTypeID Again = CROWDY_INVALID_TYPE_ID;
	UEventPayloadRegistry::Get()->GetID(FCrowdyStateDelta::StaticStruct(), Again);
	TestEqual(TEXT("payload id is stable"), Again, ID);
	return true;
}

// Only owner-role entities are tracked and only they emit. RegisterOwnedEntityForTest adds owned entries;
// a tick over two owned entries emits exactly two deltas whose EntityIDs match the registered owners.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSendOwnerGateTest,
	"CrowdySDK.State.SendOwnerGate", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateSendOwnerGateTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	TArray<FGuid> Emitted;
	Rep->DispatchHookForTests = [&Emitted](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Emitted.Add(Delta.EntityID);
	};

	ACrowdyStateSendTestActor* OwnedActor = NewObject<ACrowdyStateSendTestActor>();
	ACrowdyStateSendTestActor* ProxyActor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("owned actor created"), OwnedActor)
		|| !TestNotNull(TEXT("proxy actor created"), ProxyActor))
	{
		return false;
	}

	// The ownership gate itself: an Owner record is tracked, a RemoteProxy record for an equally-valid actor
	// is rejected. This drives the same gate HandleEntityRegistered runs, from synthetic records, so a
	// regression that dropped the Role check would fail here.
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = FGuid::NewGuid();
	OwnerRecord.OwnerID = LocalPlayer;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;

	FCrowdyEntityRecord ProxyRecord;
	ProxyRecord.NetID = FGuid::NewGuid();
	ProxyRecord.Role = ECrowdyRole::RemoteProxy;
	ProxyRecord.Participant = ProxyActor;

	TestTrue(TEXT("owner record is tracked"), Rep->TryTrackOwnedForTest(OwnerRecord));
	TestFalse(TEXT("remote-proxy record is rejected by the owner gate"), Rep->TryTrackOwnedForTest(ProxyRecord));
	TestEqual(TEXT("only the owner is tracked"), Rep->NumOwnedForTest(), 1);

	// On a tick, only the owned entity emits; the proxy's change is never diffed.
	OwnedActor->RepInt = 7;
	ProxyActor->RepInt = 9;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("only the owner emits"), Emitted.Num(), 1);
	TestTrue(TEXT("the owner emitted"), Emitted.Contains(OwnerRecord.NetID));

	// Sender is stamped from the local player (the live read falls back to the injected id when there is no
	// entity subsystem) and Flags stay 0 in Phase 3.
	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};
	OwnedActor->RepInt = 11;
	Rep->RunReplicationLoopForTest();
	if (TestEqual(TEXT("one delta after the second change"), Captured.Num(), 1))
	{
		TestEqual(TEXT("sender is the local player"), Captured[0].SenderID, LocalPlayer);
		TestEqual(TEXT("flags stay 0 in Phase 3"), static_cast<int32>(Captured[0].Flags), 0);
	}
	return true;
}

// A hot delta carries only the changed spatial property, and owner-only / manual-dirty changes never emit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSendOnlyChangedTest,
	"CrowdySDK.State.SendOnlyChanged", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateSendOnlyChangedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	// Isolate the split behavior from the keyframe heartbeat: pin the clock to 0 and keep the interval so a
	// heartbeat is never due within this test (NextKeyframeTime is seeded one interval + stagger ahead).
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&Captured, &CapturedTargeted](const FCrowdyStateDelta& Delta, bool bTargeted)
	{
		Captured.Add(Delta);
		CapturedTargeted.Add(bTargeted);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	// First tick drains the initial diff against the zero shadow (everything default, so nothing changed).
	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	CapturedTargeted.Reset();

	// Change exactly one SPATIAL property.
	Actor->RepInt = 42;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("exactly one delta emitted"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestFalse(TEXT("the spatial delta is a broadcast"), CapturedTargeted[0]);
		// Decode into a fresh actor with the same layout; exactly one index changed, and it is RepInt.
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob,
			Dest, Changed);
		TestTrue(TEXT("delta decodes"), bOk);
		TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
		TestEqual(TEXT("decoded RepInt matches"), Dest->RepInt, 42);
	}

	// A no-change tick after the send emits nothing (shadow was updated post-send).
	Captured.Reset();
	CapturedTargeted.Reset();
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("no-change tick is silent"), Captured.Num(), 0);

	// Phase 5: changing an OWNER-ONLY property now emits exactly one TARGETED delta decoding to just the
	// owner-only index (it is auto-diffed, but delivered owner-scoped rather than on the spatial broadcast).
	Actor->RepOwnerOnly = 100;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("owner-only change emits exactly one delta"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestTrue(TEXT("the owner-only delta is targeted"), CapturedTargeted[0]);
		ACrowdyStateSendTestActor* OoDest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob,
			OoDest, Changed);
		TestTrue(TEXT("owner-only delta decodes"), bOk);
		TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
		TestEqual(TEXT("decoded RepOwnerOnly matches"), OoDest->RepOwnerOnly, 100);
	}

	// Manual-dirty change WITHOUT a mark never emits (it is never auto-diffed).
	Captured.Reset();
	CapturedTargeted.Reset();
	Actor->RepManualDirty = 200;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("unmarked manual-dirty change does not emit"), Captured.Num(), 0);
	return true;
}

// After a send the shadow holds the sent values, so a tick with no further change stays silent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateShadowUpdatesPostSendTest,
	"CrowdySDK.State.ShadowUpdatesPostSend", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateShadowUpdatesPostSendTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	int32 EmitCount = 0;
	Rep->DispatchHookForTests = [&EmitCount](const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/)
	{
		++EmitCount;
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	Actor->RepFloat = 3.5f;
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the change emitted once"), EmitCount, 1);

	EmitCount = 0;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("second, unchanged tick is silent"), EmitCount, 0);
	return true;
}

// An executor-overlapping property is dropped from the layout; the non-overlapping one survives. The
// overlap filter logs an Error, so whitelist it (Occurrences 0 == one or more).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateExecutorOverlapDroppedTest,
	"CrowdySDK.State.ExecutorOverlapDropped", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateExecutorOverlapDroppedTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("also lives in its executor state struct"),
		EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyStateOverlapActor::StaticClass());
	if (!TestNotNull(TEXT("overlap actor has a (filtered) layout"), Layout))
	{
		return false;
	}

	bool bHasHealth = false;
	bool bHasMana = false;
	for (const FCrowdyRepProperty& Prop : Layout->Properties)
	{
		if (Prop.Property)
		{
			bHasHealth |= Prop.Property->GetFName() == FName(TEXT("Health"));
			bHasMana |= Prop.Property->GetFName() == FName(TEXT("Mana"));
		}
	}

	TestFalse(TEXT("Health dropped (overlaps executor state struct)"), bHasHealth);
	TestTrue(TEXT("Mana survives (no overlap)"), bHasMana);
	return true;
}

// Manual-dirty is push-only: a change to a bManualDirty property never auto-diffs, and emits exactly once
// after MarkStateDirty flags it (then the mark clears, so a following tick is silent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateManualDirtyPushTest,
	"CrowdySDK.State.ManualDirtyPush", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateManualDirtyPushTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&Captured, &CapturedTargeted](const FCrowdyStateDelta& Delta, bool bTargeted)
	{
		Captured.Add(Delta);
		CapturedTargeted.Add(bTargeted);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	// Drain the first tick.
	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	CapturedTargeted.Reset();

	// Change the manual-dirty property WITHOUT marking it: no emit.
	Actor->RepManualDirty = 55;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("unmarked manual-dirty does not emit"), Captured.Num(), 0);

	// Mark it: exactly one delta decoding to just RepManualDirty. It is not owner-only, so it broadcasts.
	Rep->MarkStateDirty(Id, TEXT("RepManualDirty"));
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("marked manual-dirty emits exactly one delta"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestFalse(TEXT("non-owner-only manual-dirty broadcasts"), CapturedTargeted[0]);
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob, Dest, Changed);
		TestTrue(TEXT("delta decodes"), bOk);
		TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
		TestEqual(TEXT("decoded RepManualDirty matches"), Dest->RepManualDirty, 55);
	}

	// A following tick with no new mark is silent (one mark == one emit).
	Captured.Reset();
	CapturedTargeted.Reset();
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("no further mark is silent"), Captured.Num(), 0);
	return true;
}

// A spatial change and an owner-only change in the same tick produce TWO deltas: a broadcast for the spatial
// property and a targeted one for the owner-only property, each decoding to just its own index.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnerOnlyTargetedTest,
	"CrowdySDK.State.OwnerOnlyTargeted", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateOwnerOnlyTargetedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&Captured, &CapturedTargeted](const FCrowdyStateDelta& Delta, bool bTargeted)
	{
		Captured.Add(Delta);
		CapturedTargeted.Add(bTargeted);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	CapturedTargeted.Reset();

	// Change a spatial AND an owner-only property in one tick.
	Actor->RepInt = 9;
	Actor->RepOwnerOnly = 21;
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("exactly two deltas emitted"), Captured.Num(), 2))
	{
		return false;
	}

	// Find the broadcast and the targeted one (order is spatial-first in the loop, but assert by flag not order).
	int32 SpatialIdx = INDEX_NONE;
	int32 TargetedIdx = INDEX_NONE;
	for (int32 i = 0; i < CapturedTargeted.Num(); ++i)
	{
		if (CapturedTargeted[i]) { TargetedIdx = i; } else { SpatialIdx = i; }
	}
	TestTrue(TEXT("one broadcast delta present"), SpatialIdx != INDEX_NONE);
	TestTrue(TEXT("one targeted delta present"), TargetedIdx != INDEX_NONE);
	TestEqual(TEXT("both deltas share the same LayoutHash"),
		Captured[0].LayoutHash, Captured[1].LayoutHash);

	if (SpatialIdx != INDEX_NONE)
	{
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[SpatialIdx].LayoutHash,
			Captured[SpatialIdx].Blob, Dest, Changed);
		TestTrue(TEXT("spatial delta decodes"), bOk);
		TestEqual(TEXT("spatial delta carries exactly one index"), Changed.Num(), 1);
		TestEqual(TEXT("spatial delta carries RepInt"), Dest->RepInt, 9);
		TestEqual(TEXT("spatial delta does NOT carry RepOwnerOnly"), Dest->RepOwnerOnly, 0);
	}

	if (TargetedIdx != INDEX_NONE)
	{
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[TargetedIdx].LayoutHash,
			Captured[TargetedIdx].Blob, Dest, Changed);
		TestTrue(TEXT("targeted delta decodes"), bOk);
		TestEqual(TEXT("targeted delta carries exactly one index"), Changed.Num(), 1);
		TestEqual(TEXT("targeted delta carries RepOwnerOnly"), Dest->RepOwnerOnly, 21);
		TestEqual(TEXT("targeted delta does NOT carry RepInt"), Dest->RepInt, 0);
	}

	return true;
}

// The keyframe heartbeat fires on its interval with no value change, carrying every non-owner-only property
// (Keyframe flag set); a subsequent small change still emits a hot (non-keyframe) delta.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateKeyframeHeartbeatTest,
	"CrowdySDK.State.KeyframeHeartbeat", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateKeyframeHeartbeatTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	// Count the CrowdyHeartbeat-marked, non-owner-only properties: a keyframe carries exactly these (heartbeat is
	// opt-in per property, so an unmarked property never rides the keyframe even though it still diffs on change).
	int32 HeartbeatCount = 0;
	for (const FCrowdyRepProperty& Prop : Layout->Properties)
	{
		if (Prop.Property && !Prop.bOwnerOnly && Prop.bHeartbeat)
		{
			++HeartbeatCount;
		}
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	// Give every CrowdyHeartbeat-marked property a non-default value so the keyframe, decoded onto a fresh
	// receiver, reports each as changed: Decode records ACTUALLY-changed slots, not merely present ones, so a
	// keyframe of all-default values onto an all-default receiver would report zero. These four are exactly the
	// marked, non-owner-only leaves; they are sent on the first tick and drained below, then the keyframe at t=10
	// re-carries them as a pure heartbeat (no new value change).
	Actor->RepInt = 11;
	Actor->RepFloat = 2.5f;
	Actor->RepName = FName(TEXT("kf"));
	Actor->RepVector = FVector(1.0, 2.0, 3.0);
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	// Drain the first tick (sends the initial values; no keyframe due at t=0 since it is seeded ~interval + stagger out).
	Rep->RunReplicationLoopForTest();
	Captured.Reset();

	// Advance well past the interval + any stagger (interval default 2.0, stagger < interval). No new value change.
	Rep->SetTimeForTest(10.0);
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("keyframe tick emits exactly one delta"), Captured.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the keyframe flag is set"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) != 0);
	{
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob, Dest, Changed);
		TestTrue(TEXT("keyframe decodes"), bOk);
		TestEqual(TEXT("keyframe carries every heartbeat property"), Changed.Num(), HeartbeatCount);
	}

	// A subsequent small change (before the next keyframe is due) still emits a hot, non-keyframe delta.
	Captured.Reset();
	Rep->SetTimeForTest(10.5);
	Actor->RepInt = 3;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the change emits one delta"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestTrue(TEXT("the hot delta is not a keyframe"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) == 0);
	}
	return true;
}

// AdoptHostValues writes a host correction into the owner's shadow so the next diff treats it as already-sent
// and does not re-emit a revert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHostAdoptionTest,
	"CrowdySDK.State.HostAdoption", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHostAdoptionTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = [Layout]() -> int32
	{
		for (int32 i = 0; i < Layout->Properties.Num(); ++i)
		{
			if (Layout->Properties[i].Property && Layout->Properties[i].Property->GetFName() == FName(TEXT("RepInt")))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}();
	if (!TestTrue(TEXT("RepInt present in layout"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	int32 EmitCount = 0;
	Rep->DispatchHookForTests = [&EmitCount](const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/)
	{
		++EmitCount;
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	EmitCount = 0;

	// Simulate a host correction: the receive path would have written RepInt onto the live actor, then adopted
	// the value into the owner's shadow. Emulate exactly that here.
	Actor->RepInt = 50;
	TArray<int32> ChangedIndices;
	ChangedIndices.Add(RepIntIndex);
	Rep->AdoptHostValues(Id, *Layout, ChangedIndices, Actor);

	// The diff now sees live == shadow for RepInt, so no revert delta is produced.
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("adopted host value does not re-emit"), EmitCount, 0);
	return true;
}

// A HostOwned entity is tracked only when the local client is host, is never auto-diffed, and emits (as
// HostSourced) only its explicitly-marked manual-dirty properties. A non-host rejects HostOwned records.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHostOwnedManualOnlyTest,
	"CrowdySDK.State.HostOwnedManualOnly", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHostOwnedManualOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();

	// Local is host: host id == local player id.
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->SetHostIDForTest(LocalPlayer);

	TArray<FCrowdyStateDelta> Captured;
	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&Captured, &CapturedTargeted](const FCrowdyStateDelta& Delta, bool bTargeted)
	{
		Captured.Add(Delta);
		CapturedTargeted.Add(bTargeted);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	FCrowdyEntityRecord HostOwnedRecord;
	HostOwnedRecord.NetID = FGuid::NewGuid();
	HostOwnedRecord.Role = ECrowdyRole::HostOwned;
	HostOwnedRecord.Participant = Actor;

	TestTrue(TEXT("host-owned record tracked when local is host"), Rep->TryTrackOwnedForTest(HostOwnedRecord));
	TestEqual(TEXT("exactly one owned entry"), Rep->NumOwnedForTest(), 1);

	// Drain the first tick.
	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	CapturedTargeted.Reset();

	// Change an AUTO-DIFF (spatial) property: host-owned auto-diff is OFF, so nothing emits.
	Actor->RepInt = 12;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("host-owned auto-diff property does not emit"), Captured.Num(), 0);

	// Mark + change a manual-dirty property: exactly one delta, stamped HostSourced (local is host).
	Actor->RepManualDirty = 77;
	Rep->MarkStateDirty(HostOwnedRecord.NetID, TEXT("RepManualDirty"));
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("host-owned manual-dirty emits exactly one delta"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestTrue(TEXT("host-owned delta is HostSourced"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0);
	}

	// The keyframe heartbeat STILL fires for a host-owned entity (a late observer needs the world entity's
	// baseline), even though its auto-diff is off: advancing past the interval with no mark emits exactly one
	// broadcast keyframe, carrying every non-owner-only property and stamped HostSourced. This guards against a
	// regression that mistakenly gated the keyframe on !bHostOwned.
	Captured.Reset();
	CapturedTargeted.Reset();
	Rep->SetTimeForTest(10.0);
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("host-owned keyframe heartbeat fires"), Captured.Num(), 1);
	if (Captured.Num() == 1)
	{
		TestFalse(TEXT("host-owned keyframe is a broadcast"), CapturedTargeted[0]);
		TestTrue(TEXT("host-owned keyframe has the Keyframe flag"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) != 0);
		TestTrue(TEXT("host-owned keyframe is HostSourced"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0);
	}

	// A fresh replicator whose local != host must REJECT a HostOwned record.
	UCrowdyStateReplicator* NonHostRep = MakeReplicator(Registry, LocalPlayer);
	NonHostRep->SetHostIDForTest(FGuid::NewGuid()); // host is someone else
	ACrowdyStateSendTestActor* Actor2 = NewObject<ACrowdyStateSendTestActor>();
	FCrowdyEntityRecord HostOwnedRecord2;
	HostOwnedRecord2.NetID = FGuid::NewGuid();
	HostOwnedRecord2.Role = ECrowdyRole::HostOwned;
	HostOwnedRecord2.Participant = Actor2;
	TestFalse(TEXT("non-host rejects host-owned record"), NonHostRep->TryTrackOwnedForTest(HostOwnedRecord2));
	TestEqual(TEXT("non-host tracks nothing"), NonHostRep->NumOwnedForTest(), 0);
	return true;
}

// The authority-convention send stamp: with local == host a normal Owner spatial delta carries HostSourced;
// with local != host it does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHostSourcedFlagWhenHostTest,
	"CrowdySDK.State.HostSourcedFlagWhenHost", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHostSourcedFlagWhenHostTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();

	// Case 1: local IS host -> HostSourced set.
	{
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		Rep->SetHostIDForTest(LocalPlayer);

		TArray<FCrowdyStateDelta> Captured;
		Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
		{
			Captured.Add(Delta);
		};

		ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
		const FGuid Id = FGuid::NewGuid();
		TestTrue(TEXT("owned actor registered (host)"), Rep->RegisterOwnedEntityForTest(Id, Actor));
		Rep->RunReplicationLoopForTest();
		Captured.Reset();

		Actor->RepInt = 5;
		Rep->RunReplicationLoopForTest();
		if (TestEqual(TEXT("host emits one delta"), Captured.Num(), 1))
		{
			TestTrue(TEXT("host delta carries HostSourced"),
				(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0);
		}
	}

	// Case 2: local is NOT host -> HostSourced clear.
	{
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		Rep->SetHostIDForTest(FGuid::NewGuid()); // someone else is host

		TArray<FCrowdyStateDelta> Captured;
		Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
		{
			Captured.Add(Delta);
		};

		ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
		const FGuid Id = FGuid::NewGuid();
		TestTrue(TEXT("owned actor registered (non-host)"), Rep->RegisterOwnedEntityForTest(Id, Actor));
		Rep->RunReplicationLoopForTest();
		Captured.Reset();

		Actor->RepInt = 6;
		Rep->RunReplicationLoopForTest();
		if (TestEqual(TEXT("non-host emits one delta"), Captured.Num(), 1))
		{
			TestTrue(TEXT("non-host delta does NOT carry HostSourced"),
				(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) == 0);
		}
	}

	return true;
}

// The crowdy.state.trace CVar gates the info-log path: CrowdyReplicationTrace::State() tracks its int value
// (0 -> false, non-zero -> true) without touching warn/error. The original value is saved and restored so the
// test leaves the console state untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateTraceCvarGateTest,
	"CrowdySDK.State.TraceCvarGate", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateTraceCvarGateTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.state.trace"));
	if (!TestNotNull(TEXT("crowdy.state.trace CVar is registered"), Cvar))
	{
		return false;
	}

	const int32 Original = Cvar->GetInt();

	Cvar->Set(0);
	TestFalse(TEXT("State() is false when the CVar is 0"), CrowdyReplicationTrace::State());

	Cvar->Set(1);
	TestTrue(TEXT("State() is true when the CVar is 1"), CrowdyReplicationTrace::State());

	Cvar->Set(Original);
	return true;
}

// The read-only runtime handle: IsStateReplicated is a class-level query (true for a CrowdyState actor, false
// for a bare actor or null), and GetLastSentStateBytes reports the last local send byte count for a tracked
// owned entity (> 0 after an emitting tick) or -1 for an actor this client does not drive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateRuntimeHandleTest,
	"CrowdySDK.State.RuntimeHandle", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateRuntimeHandleTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	ACrowdyStateSendTestActor* OwnedActor = NewObject<ACrowdyStateSendTestActor>();
	AActor* BareActor = NewObject<AActor>();
	if (!TestNotNull(TEXT("owned actor created"), OwnedActor)
		|| !TestNotNull(TEXT("bare actor created"), BareActor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, OwnedActor));

	// IsStateReplicated is a class-level query: true for the CrowdyState actor, false for a bare actor (no
	// CrowdyState layout) and for a null actor.
	TestTrue(TEXT("CrowdyState actor is state-replicated"), Rep->IsStateReplicated(OwnedActor));
	TestFalse(TEXT("bare actor is not state-replicated"), Rep->IsStateReplicated(BareActor));
	TestFalse(TEXT("null actor is not state-replicated"), Rep->IsStateReplicated(nullptr));

	// Before any send the owned entity reports 0 bytes; the bare (untracked) actor reports -1.
	TestEqual(TEXT("owned entity reports 0 bytes before any send"), Rep->GetLastSentStateBytes(OwnedActor), 0);
	TestEqual(TEXT("untracked actor reports -1"), Rep->GetLastSentStateBytes(BareActor), -1);

	// Change one spatial property and tick: the owned entity now reports a positive last-sent byte count.
	OwnedActor->RepInt = 42;
	Rep->RunReplicationLoopForTest();

	TestTrue(TEXT("owned entity reports positive bytes after an emitting tick"),
		Rep->GetLastSentStateBytes(OwnedActor) > 0);
	TestEqual(TEXT("untracked actor still reports -1"), Rep->GetLastSentStateBytes(BareActor), -1);
	return true;
}

// The persisted-host init-seeding path: when the host is ALREADY elected on arrival (a fresh world after travel,
// or a late spawn into an established session) no host-changed event fires, but registration reads GetHostID()
// live, so a HostOwned entity is tracked at register time with NO OnHostIDUpdated event. The host override is
// deliberately OFF here so the real source chain runs end-to-end: ResolveHostID() -> ES->GetHostID() ->
// GameSession->GetHostID(). A regression that cached the host at Initialize, or broke the ES read-through to the
// session, would drop the tracking and fail this test rather than passing vacuously off the injected override.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHostAlreadyElectedTracksOnRegisterTest,
	"CrowdySDK.State.HostAlreadyElectedTracksOnRegister", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHostAlreadyElectedTracksOnRegisterTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	// Local IS host on arrival but seed it through the REAL persisted source (a game session with the host set),
	// NOT SetHostIDForTest, so the live read-through is exercised with the override off.
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	ES->SetGameSessionForTest(MakeGameSession(LocalPlayer));
	Rep->SetEntitySubsystemForTest(ES);

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeHostOwnedRecord(Actor, NetID));

	// Drive registration exactly as the subsystem delegate would, with NO host-changed event: the live GetHostID
	// read inside TryTrackOwned tracks it because the persisted host (read through the session) is us.
	Rep->HandleEntityRegisteredForTest(NetID);

	TestTrue(TEXT("host-owned entity tracked on register (host already elected)"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("exactly one owned entry"), Rep->NumOwnedForTest(), 1);
	TestEqual(TEXT("the host-owned id is remembered"), Rep->NumKnownHostOwnedForTest(), 1);
	return true;
}

// Mid-session promotion: a non-host registers a HostOwned entity (remembered but not tracked), then becomes host
// and HandleHostChanged promotes it to tracked. Re-running the promotion does not double-track.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateBecameHostPromotesKnownHostOwnedTest,
	"CrowdySDK.State.BecameHostPromotesKnownHostOwned", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateBecameHostPromotesKnownHostOwnedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherHost = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	// Local is NOT host: someone else holds it.
	Rep->SetHostIDForTest(OtherHost);
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeHostOwnedRecord(Actor, NetID));
	Rep->HandleEntityRegisteredForTest(NetID);

	// Non-host: the record is remembered but not tracked.
	TestEqual(TEXT("non-host tracks nothing"), Rep->NumOwnedForTest(), 0);
	TestEqual(TEXT("non-host still remembers the host-owned id"), Rep->NumKnownHostOwnedForTest(), 1);

	// Become host: the change event promotes the known host-owned entity to tracked.
	Rep->SetHostIDForTest(LocalPlayer);
	Rep->HandleHostChangedForTest(LocalPlayer, OtherHost);
	TestTrue(TEXT("known host-owned entity is tracked after promotion"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("exactly one owned entry after promotion"), Rep->NumOwnedForTest(), 1);

	// Re-running the promotion path does not double-track (the Contains guard holds).
	Rep->HandleHostChangedForTest(LocalPlayer, OtherHost);
	TestEqual(TEXT("promotion is idempotent (no double-track)"), Rep->NumOwnedForTest(), 1);
	return true;
}

// Demotion: a host tracking a HostOwned entity loses host and HandleHostChanged drops it, after which the loop
// emits nothing for it a demoted host no longer broadcasts world state, keyframes included.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLostHostDemotesAndStopsEmittingTest,
	"CrowdySDK.State.LostHostDemotesAndStopsEmitting", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateLostHostDemotesAndStopsEmittingTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherHost = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	// Local IS host, so the HostOwned entity is tracked at register time.
	Rep->SetTimeForTest(0.0);
	Rep->SetHostIDForTest(LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeHostOwnedRecord(Actor, NetID));
	Rep->HandleEntityRegisteredForTest(NetID);
	TestTrue(TEXT("host-owned entity tracked while host"), Rep->IsTrackedForTest(NetID));

	// Drain the first tick, then confirm the keyframe heartbeat fires while we are host (a host-owned entity still
	// emits its baseline). Advance well past the interval + stagger with no value change.
	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	Rep->SetTimeForTest(10.0);
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("host-owned keyframe emits while host"), Captured.Num(), 1);

	// Lose host: demotion drops the host-owned entity.
	Rep->SetHostIDForTest(OtherHost);
	Rep->HandleHostChangedForTest(OtherHost, LocalPlayer);
	TestEqual(TEXT("demoted host tracks nothing"), Rep->NumOwnedForTest(), 0);

	// A subsequent loop (clock advanced further so a keyframe would be due) emits nothing: the demoted host no
	// longer broadcasts world state at all.
	Captured.Reset();
	Rep->SetTimeForTest(20.0);
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("demoted host emits nothing (keyframes included)"), Captured.Num(), 0);
	return true;
}

// Demotion is SELECTIVE: losing host drops only the host-owned (world) entities, never the Owner entities this
// client still owns. Registers both while host, demotes, and asserts the Owner survives (still tracked) while the
// host-owned one is dropped guards against a blanket OwnedEntities.Empty() regression in the demote branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateDemotionKeepsOwnerEntitiesTest,
	"CrowdySDK.State.DemotionKeepsOwnerEntities", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateDemotionKeepsOwnerEntitiesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherHost = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetHostIDForTest(LocalPlayer); // local IS host, so the host-owned entity tracks

	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	// A host-owned (world) entity registered through the subsystem path so it lands in KnownHostOwnedIDs.
	ACrowdyStateSendTestActor* WorldActor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid WorldID = FGuid::NewGuid();
	ES->RegisterEntity(MakeHostOwnedRecord(WorldActor, WorldID));
	Rep->HandleEntityRegisteredForTest(WorldID);

	// An Owner entity this client owns: tracked directly (Role=Owner is always tracked, host or not).
	ACrowdyStateSendTestActor* OwnedActor = NewObject<ACrowdyStateSendTestActor>();
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = FGuid::NewGuid();
	OwnerRecord.OwnerID = LocalPlayer;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	TestTrue(TEXT("owner entity tracked"), Rep->TryTrackOwnedForTest(OwnerRecord));

	TestEqual(TEXT("both entities tracked while host"), Rep->NumOwnedForTest(), 2);

	// Lose host: only the host-owned entity is dropped; the Owner entity survives.
	Rep->SetHostIDForTest(OtherHost);
	Rep->HandleHostChangedForTest(OtherHost, LocalPlayer);

	TestEqual(TEXT("exactly one entity remains after demotion"), Rep->NumOwnedForTest(), 1);
	TestTrue(TEXT("the Owner entity survives demotion"), Rep->IsTrackedForTest(OwnerRecord.NetID));
	TestFalse(TEXT("the host-owned entity is dropped by demotion"), Rep->IsTrackedForTest(WorldID));
	return true;
}

// Phase C: MarkStateDirty on an entity this client does NOT track takes the one-shot host-push path. With local as
// host and the target's HostOverride == Allow, a single push emits exactly one broadcast delta stamped HostSourced,
// carrying the property's current live value (RepInt).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateUntrackedHostPushEmitsHostSourcedTest,
	"CrowdySDK.State.UntrackedHostPushEmitsHostSourced", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateUntrackedHostPushEmitsHostSourcedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateHostOverrideActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("host-override actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	// A real ES so FindEntity resolves the untracked target and the live sender read returns LocalPlayer; local IS host.
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(LocalPlayer);

	TArray<FCrowdyStateDelta> Captured;
	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&Captured, &CapturedTargeted](const FCrowdyStateDelta& Delta, bool bTargeted)
	{
		Captured.Add(Delta);
		CapturedTargeted.Add(bTargeted);
	};

	ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("target created"), Target) || !TestNotNull(TEXT("target entity component present"), Target->Entity.Get()))
	{
		return false;
	}
	Target->Entity->HostOverride = ECrowdyHostOverride::Allow;
	Target->RepInt = 88;

	const FGuid Id = FGuid::NewGuid();
	RegisterUntrackedProxy(ES, Id, Target);

	// Not tracked by the replicator, so MarkStateDirty takes the untracked one-shot push path.
	TestEqual(TEXT("replicator tracks nothing"), Rep->NumOwnedForTest(), 0);
	Rep->MarkStateDirty(Id, TEXT("RepInt"));
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("exactly one delta emitted"), Captured.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("the host push is a broadcast"), CapturedTargeted[0]);
	TestTrue(TEXT("the host push is HostSourced"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0);

	ACrowdyStateHostOverrideActor* Dest = NewObject<ACrowdyStateHostOverrideActor>();
	TArray<int32> Changed;
	const bool bOk = FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob, Dest, Changed);
	TestTrue(TEXT("host push decodes"), bOk);
	TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
	TestEqual(TEXT("decoded RepInt matches the live value"), Dest->RepInt, 88);
	return true;
}

// Phase C: the send-side HostOverride check suppresses a push to an OwnerOnly entity a host may not override it.
// With the target's HostOverride == OwnerOnly, MarkStateDirty drops the push and the loop emits nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateUntrackedHostPushOwnerOnlySuppressedTest,
	"CrowdySDK.State.UntrackedHostPushOwnerOnlySuppressed", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateUntrackedHostPushOwnerOnlySuppressedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(LocalPlayer); // local IS host

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("target created"), Target) || !TestNotNull(TEXT("target entity component present"), Target->Entity.Get()))
	{
		return false;
	}
	Target->Entity->HostOverride = ECrowdyHostOverride::OwnerOnly;
	Target->RepInt = 88;

	const FGuid Id = FGuid::NewGuid();
	RegisterUntrackedProxy(ES, Id, Target);

	Rep->MarkStateDirty(Id, TEXT("RepInt"));
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("OwnerOnly entity suppresses the host push"), Captured.Num(), 0);
	return true;
}

// Phase C: a push from a NON-host client is NOT stamped HostSourced. The delta still emits (the send check only
// gates OwnerOnly), but its cleared HostSourced flag is what makes an owning receiver drop it by host precedence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateNonHostPushIsNotHostSourcedTest,
	"CrowdySDK.State.NonHostPushIsNotHostSourced", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateNonHostPushIsNotHostSourcedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(FGuid::NewGuid()); // someone else is host, so local is NOT host

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("target created"), Target) || !TestNotNull(TEXT("target entity component present"), Target->Entity.Get()))
	{
		return false;
	}
	Target->Entity->HostOverride = ECrowdyHostOverride::Allow;
	Target->RepInt = 77;

	const FGuid Id = FGuid::NewGuid();
	RegisterUntrackedProxy(ES, Id, Target);

	Rep->MarkStateDirty(Id, TEXT("RepInt"));
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("exactly one delta emitted"), Captured.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("a non-host push is NOT HostSourced"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) == 0);
	return true;
}

// A (map off-switch) + the load-bearing invariant: with the map keyframe interval set to 0 the heartbeat is
// suppressed map-wide even for CrowdyHeartbeat-marked properties, yet on-change replication is UNAFFECTED  a
// changed property still ships on the next tick, as a non-keyframe delta. A regression that gated the diff on the
// heartbeat, or dropped the `KeyframeInterval > 0` guard, fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatOffStillSendsOnChangeTest,
	"CrowdySDK.State.HeartbeatOffStillSendsOnChange", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHeartbeatOffStillSendsOnChangeTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateSendTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("send actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->SetKeyframeIntervalForTest(0.0f); // heartbeat OFF map-wide

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/) { Captured.Add(Delta); };

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();

	// Advance far past what would be the default interval: with the heartbeat off, NO keyframe fires even though
	// this actor's properties ARE CrowdyHeartbeat-marked.
	Rep->SetTimeForTest(100.0);
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("no keyframe emits when the heartbeat is off"), Captured.Num(), 0);

	// A real change STILL ships on the next tick (on-change is independent of the heartbeat), as a non-keyframe delta.
	Actor->RepInt = 42;
	Rep->RunReplicationLoopForTest();
	if (TestEqual(TEXT("on-change still emits with the heartbeat off"), Captured.Num(), 1))
	{
		TestTrue(TEXT("the on-change delta is NOT a keyframe"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) == 0);
		ACrowdyStateSendTestActor* Dest = NewObject<ACrowdyStateSendTestActor>();
		TArray<int32> Changed;
		TestTrue(TEXT("delta decodes"),
			FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob, Dest, Changed));
		TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
		TestEqual(TEXT("decoded RepInt matches"), Dest->RepInt, 42);
	}
	return true;
}

// The keyframe-mislabel guard: an entity whose keyframe timer is due but which has NO CrowdyHeartbeat-marked
// properties must (a) emit no empty keyframe on an idle tick and (b) NOT stamp the Keyframe flag onto a same-tick
// on-change delta. The host-override fixture is a CrowdyState int with no heartbeat mark. A regression that set
// bEmitKeyframe before confirming a marked property was actually added fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatNoMarkedPropsNoKeyframeTest,
	"CrowdySDK.State.HeartbeatNoMarkedPropsNoKeyframe", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHeartbeatNoMarkedPropsNoKeyframeTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyStateHostOverrideActor::StaticClass());
	if (!TestNotNull(TEXT("host-override actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/) { Captured.Add(Delta); };

	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();

	// Timer well past due, nothing changed: no empty keyframe (the entity has no CrowdyHeartbeat property to carry).
	Rep->SetTimeForTest(10.0);
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("no empty keyframe when nothing is heartbeat-marked"), Captured.Num(), 0);

	// Timer still past due AND a change in the same tick: the delta ships as an ON-CHANGE delta, never mislabeled
	// as a keyframe.
	Actor->RepInt = 7;
	Rep->SetTimeForTest(20.0);
	Rep->RunReplicationLoopForTest();
	if (TestEqual(TEXT("the change emits exactly one delta"), Captured.Num(), 1))
	{
		TestTrue(TEXT("the on-change delta is NOT mislabeled a keyframe"),
			(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) == 0);
	}
	return true;
}

// B (per-entity kill switch): an entity whose UCrowdyEntityComponent sets StateHeartbeat==Off emits NO keyframe
// even though it carries a CrowdyHeartbeat-marked property; the same fixture with StateHeartbeat==Inherit DOES
// emit the keyframe. Proves bHeartbeatSuppressed gates the heartbeat, and only the heartbeat.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatPerEntityOffTest,
	"CrowdySDK.State.HeartbeatPerEntityOff", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateHeartbeatPerEntityOffTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();

	// (i) StateHeartbeat == Off: suppressed, no keyframe despite the marked property.
	{
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		TArray<FCrowdyStateDelta> Captured;
		Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/) { Captured.Add(Delta); };

		ACrowdyStateHeartbeatActor* Actor = NewObject<ACrowdyStateHeartbeatActor>();
		if (!TestNotNull(TEXT("off actor entity component present"), Actor->Entity.Get()))
		{
			return false;
		}
		Actor->Entity->StateHeartbeat = ECrowdyStateHeartbeat::Off;
		Actor->RepBeat = 5; // non-default so a keyframe, if it fired, would carry it

		const FGuid Id = FGuid::NewGuid();
		TestTrue(TEXT("off actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));
		Rep->RunReplicationLoopForTest(); // drains the initial on-change send
		Captured.Reset();

		Rep->SetTimeForTest(10.0);
		Rep->RunReplicationLoopForTest();
		TestEqual(TEXT("StateHeartbeat=Off suppresses the keyframe"), Captured.Num(), 0);
	}

	// (ii) StateHeartbeat == Inherit: the keyframe fires (same fixture, same marked property).
	{
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		TArray<FCrowdyStateDelta> Captured;
		Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/) { Captured.Add(Delta); };

		ACrowdyStateHeartbeatActor* Actor = NewObject<ACrowdyStateHeartbeatActor>();
		if (!TestNotNull(TEXT("inherit actor entity component present"), Actor->Entity.Get()))
		{
			return false;
		}
		Actor->Entity->StateHeartbeat = ECrowdyStateHeartbeat::Inherit;
		Actor->RepBeat = 5;

		const FGuid Id = FGuid::NewGuid();
		TestTrue(TEXT("inherit actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));
		Rep->RunReplicationLoopForTest();
		Captured.Reset();

		Rep->SetTimeForTest(10.0);
		Rep->RunReplicationLoopForTest();
		if (TestEqual(TEXT("StateHeartbeat=Inherit lets the keyframe fire"), Captured.Num(), 1))
		{
			TestTrue(TEXT("the emitted delta is a keyframe"),
				(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) != 0);
		}
	}
	return true;
}

// GetOrCreateLoopbackMirror (crowdy.state.loopback): spawns a distinct RemoteProxy mirror with an INVALID
// OwnerID (never the source's owner) under its own NetID, is idempotent on repeat calls, and returns null
// without leaving a half-registered entry when the source class carries no UCrowdyEntityComponent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLoopbackMirrorSpawnTest,
	"CrowdySDK.State.LoopbackMirrorSpawn", CrowdyStateReplicatorTestFlags)
bool FCrowdyStateLoopbackMirrorSpawnTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);

	FCrowdyStateTestWorld TestWorld;

	// ACrowdyStateHostOverrideActor carries a UCrowdyEntityComponent default subobject (unlike
	// ACrowdyStateSendTestActor, whose send-loop tests register entities purely via test seams with no real
	// component) so the mirror spawned from it also gets one, exactly like a real CrowdyState-tracked class.
	// GetOrCreateLoopbackMirror never touches an entity subsystem itself (only BeginPlay's own registration
	// tail does, resolved from the WORLD, not injectable), and this test's editor-type world never creates one
	// (UCrowdyEntitySubsystem::ShouldCreateSubsystem gates to PIE/Game) so Role/OwnerID are read straight off
	// the mirror's component instead of round-tripping through a registry lookup.
	ACrowdyStateHostOverrideActor* Source = TestWorld.Spawn<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("source actor spawned"), Source))
	{
		return false;
	}
	const FGuid SourceID = FGuid::NewGuid();

	AActor* Mirror = Rep->GetOrCreateLoopbackMirrorForTest(Source, SourceID);
	if (!TestNotNull(TEXT("mirror spawned"), Mirror))
	{
		return false;
	}
	TestTrue(TEXT("mirror is a distinct actor from the source"), Mirror != Source);

	UCrowdyEntityComponent* MirrorComp = Mirror->FindComponentByClass<UCrowdyEntityComponent>();
	if (TestNotNull(TEXT("mirror has an entity component"), MirrorComp))
	{
		TestTrue(TEXT("mirror role is RemoteProxy"), MirrorComp->GetRole() == ECrowdyRole::RemoteProxy);
		TestFalse(TEXT("mirror OwnerID is invalid (never the source's owner)"), MirrorComp->GetOwnerID().IsValid());
		TestNotEqual(TEXT("mirror NetID differs from the source's"), MirrorComp->GetNetID(), SourceID);
	}

	// Idempotent: a second call for the same source entity returns the SAME mirror, no new spawn.
	AActor* MirrorAgain = Rep->GetOrCreateLoopbackMirrorForTest(Source, SourceID);
	TestEqual(TEXT("repeat call returns the same mirror"), MirrorAgain, Mirror);
	TestEqual(TEXT("exactly one mirror tracked"), Rep->NumLoopbackMirrorsForTest(), 1);

	// Guard path: a source with no UCrowdyEntityComponent yields null and leaves no half-registered entry.
	AActor* BareActor = TestWorld.Spawn<AActor>();
	if (TestNotNull(TEXT("bare actor spawned"), BareActor))
	{
		const FGuid BareID = FGuid::NewGuid();
		TestNull(TEXT("no-component guard returns null"), Rep->GetOrCreateLoopbackMirrorForTest(BareActor, BareID));
		TestEqual(TEXT("no half-registered mirror was left"), Rep->NumLoopbackMirrorsForTest(), 1);

		// Calling again is still null, proving the first attempt did not half-register anything that a second
		// call could then find and return.
		TestNull(TEXT("no-component guard returns null again"), Rep->GetOrCreateLoopbackMirrorForTest(BareActor, BareID));
		TestEqual(TEXT("still exactly one real mirror tracked"), Rep->NumLoopbackMirrorsForTest(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
