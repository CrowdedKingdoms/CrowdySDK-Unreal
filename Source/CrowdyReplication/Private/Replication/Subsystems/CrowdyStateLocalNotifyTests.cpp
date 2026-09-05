#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyAutoRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateLocalNotifyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Hands back one already-discovered slot of a cached layout so a test can retarget its authoring flags
	// (owner-only / manual-dirty / heartbeat) or its notify binding. The slot count, declaration order and the
	// layout hash are untouched, so the wire form stays exactly what the class would produce had it been authored
	// that way. The registry is built fresh per test, so nothing leaks between tests. This is how a combination no
	// shared fixture declares (a notify on a manual-dirty or heartbeat property, or a notify whose body writes to
	// the property it fires for) gets covered without a fixture class per case. Returns null when the layout
	// carries no such property.
	FCrowdyRepProperty* FindMutableLayoutProperty(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		FCrowdyRepLayout& Mutable = const_cast<FCrowdyRepLayout&>(Layout);
		for (FCrowdyRepProperty& Prop : Mutable.Properties)
		{
			if (Prop.Property && Prop.Property->GetFName() == Wanted)
			{
				return &Prop;
			}
		}
		return nullptr;
	}
}

// Characterization of the send path the local notify hooks into: firing a notify on the originator must not
// change one byte of what leaves this client. One changed property still emits exactly one broadcast delta
// decoding to exactly that property, an unchanged tick is still silent, and the shadow still advances so the
// change is not re-sent. Whatever the notify feature does, this must keep holding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifySendWireUnchangedTest,
	"CrowdySDK.State.LocalNotifySendWireUnchanged", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifySendWireUnchangedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
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

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	// Drain the first tick (the shadow starts at the property defaults).
	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	CapturedTargeted.Reset();

	Actor->RepHealth = 12.f;
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("exactly one delta emitted"), Captured.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("the delta is a broadcast"), CapturedTargeted[0]);
	TestTrue(TEXT("the delta is not a keyframe"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) == 0);

	ACrowdyStateApplyTestActor* Dest = NewObject<ACrowdyStateApplyTestActor>();
	TArray<int32> Changed;
	TestTrue(TEXT("delta decodes"),
		FCrowdyStateCodec::Decode(*Layout, Captured[0].LayoutHash, Captured[0].Blob, Dest, Changed));
	TestEqual(TEXT("exactly one changed index on the wire"), Changed.Num(), 1);
	TestEqual(TEXT("the wire carries RepHealth"), Dest->RepHealth, 12.f);

	// The shadow advanced, so an unchanged tick stays silent.
	Captured.Reset();
	CapturedTargeted.Reset();
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("unchanged tick is silent"), Captured.Num(), 0);
	return true;
}

// The core of the change: a property the LOCAL client changed fires its notify locally, on the auto-diff path,
// once per changed property per tick, and only for the property that actually changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyOnAutoDiffTest,
	"CrowdySDK.State.LocalNotifyOnAutoDiff", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyOnAutoDiffTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("apply actor has a layout"),
		Registry->FindRepLayout(ACrowdyStateApplyTestActor::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->DispatchHookForTests = [](const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/) {};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the initial drain tick fires no notify"), Actor->HealthOnRepCount, 0);
	TestEqual(TEXT("the initial drain tick fires no score notify"), Actor->ScoreOnRepCount, 0);

	// One changed property fires exactly its own notify.
	Actor->RepHealth = 5.f;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the changed property fired its notify on the sender"), Actor->HealthOnRepCount, 1);
	TestEqual(TEXT("the unchanged property fired nothing"), Actor->ScoreOnRepCount, 0);

	// A tick with nothing changed fires nothing more.
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("an unchanged tick does not refire"), Actor->HealthOnRepCount, 1);

	// The other notified property, on its own change.
	Actor->RepScore = 3;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the second property fired its own notify"), Actor->ScoreOnRepCount, 1);
	TestEqual(TEXT("the first property did not refire"), Actor->HealthOnRepCount, 1);

	// Both in one tick: one notify each, not two of either.
	Actor->RepHealth = 6.f;
	Actor->RepScore = 4;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("first property fired once more"), Actor->HealthOnRepCount, 2);
	TestEqual(TEXT("second property fired once more"), Actor->ScoreOnRepCount, 2);
	return true;
}

// An entity this client does not drive is never diffed, so it never fires a local notify: only the owned actor
// does, even when both actors changed the same property in the same tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyOnlyForOwnedEntitiesTest,
	"CrowdySDK.State.LocalNotifyOnlyForOwnedEntities", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyOnlyForOwnedEntitiesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("apply actor has a layout"),
		Registry->FindRepLayout(ACrowdyStateApplyTestActor::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->DispatchHookForTests = [](const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/) {};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Owned = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	ACrowdyStateApplyTestActor* Proxy = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("owned actor spawned"), Owned) || !TestNotNull(TEXT("proxy actor spawned"), Proxy))
	{
		return false;
	}

	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = FGuid::NewGuid();
	OwnerRecord.OwnerID = LocalPlayer;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = Owned;

	FCrowdyEntityRecord ProxyRecord;
	ProxyRecord.NetID = FGuid::NewGuid();
	ProxyRecord.OwnerID = FGuid::NewGuid();
	ProxyRecord.Role = ECrowdyRole::RemoteProxy;
	ProxyRecord.Participant = Proxy;

	TestTrue(TEXT("the owner record is tracked"), Rep->TryTrackOwnedForTest(OwnerRecord));
	TestFalse(TEXT("the proxy record is rejected"), Rep->TryTrackOwnedForTest(ProxyRecord));

	Rep->RunReplicationLoopForTest();

	Owned->RepHealth = 9.f;
	Proxy->RepHealth = 9.f;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("the owned entity fired its notify"), Owned->HealthOnRepCount, 1);
	TestEqual(TEXT("the proxy never fires a local notify"), Proxy->HealthOnRepCount, 0);
	return true;
}

// The keyframe heartbeat re-sends a marked property on its interval whether or not it moved. A receiver applies
// nothing and fires nothing for such a value, so the sender must not fire either: only a value that genuinely
// moved since the last send counts as changed on this side too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyKeyframeDoesNotRefireTest,
	"CrowdySDK.State.LocalNotifyKeyframeDoesNotRefire", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyKeyframeDoesNotRefireTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	// RepScore is heartbeat-marked so a keyframe forces it onto the wire while its value never moves.
	FCrowdyRepProperty* Score = FindMutableLayoutProperty(*Layout, TEXT("RepScore"));
	if (!TestNotNull(TEXT("RepScore present in the layout"), Score))
	{
		return false;
	}
	Score->bHeartbeat = true;

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->SetKeyframeIntervalForTest(2.0f);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();

	Actor->RepHealth = 4.f;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the real change fired its notify once"), Actor->HealthOnRepCount, 1);
	TestEqual(TEXT("the untouched heartbeat property fired nothing"), Actor->ScoreOnRepCount, 0);
	Captured.Reset();

	// Well past the interval, with nothing touched: the keyframe goes out but no value moved.
	Rep->SetTimeForTest(100.0);
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("the keyframe emitted exactly one delta"), Captured.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the delta is flagged as a keyframe"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::Keyframe) != 0);
	TestEqual(TEXT("a keyframe does not fire the heartbeat property's notify"), Actor->ScoreOnRepCount, 0);
	TestEqual(TEXT("a keyframe does not refire the previously-changed property"), Actor->HealthOnRepCount, 1);
	return true;
}

// The manual-dirty push path fires the notify too, and on the same terms: a mark on a value that actually moved
// fires once, while a mark on a value that did not move still emits on the wire (the mark forces it) but fires
// nothing, exactly as a receiver would behave.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyManualDirtyPushTest,
	"CrowdySDK.State.LocalNotifyManualDirtyPush", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyManualDirtyPushTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	FCrowdyRepProperty* Score = FindMutableLayoutProperty(*Layout, TEXT("RepScore"));
	if (!TestNotNull(TEXT("RepScore present in the layout"), Score))
	{
		return false;
	}
	Score->bManualDirty = true;

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();

	// Changed but unmarked: a manual-dirty property is never auto-diffed, so nothing emits and nothing fires.
	Actor->RepScore = 55;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("an unmarked manual-dirty change does not emit"), Captured.Num(), 0);
	TestEqual(TEXT("an unmarked manual-dirty change fires no notify"), Actor->ScoreOnRepCount, 0);

	// Marked, and the value did move since the last send: one delta, one notify.
	Rep->MarkStateDirty(Id, TEXT("RepScore"));
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the mark emits exactly one delta"), Captured.Num(), 1);
	TestEqual(TEXT("the mark fires the notify once"), Actor->ScoreOnRepCount, 1);
	Captured.Reset();

	// Marked again with the value untouched: the mark still forces it onto the wire, but nothing moved, so no
	// receiver would fire and neither does the sender.
	Rep->MarkStateDirty(Id, TEXT("RepScore"));
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the re-mark still emits"), Captured.Num(), 1);
	TestEqual(TEXT("a mark on an unmoved value fires nothing"), Actor->ScoreOnRepCount, 1);
	return true;
}

// An owner-only property is auto-diffed and shipped on the targeted delta; its notify fires locally the same as
// a spatial one, which matters most here because no other client ever receives it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyOwnerOnlyPushTest,
	"CrowdySDK.State.LocalNotifyOwnerOnlyPush", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyOwnerOnlyPushTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	FCrowdyRepProperty* Score = FindMutableLayoutProperty(*Layout, TEXT("RepScore"));
	if (!TestNotNull(TEXT("RepScore present in the layout"), Score))
	{
		return false;
	}
	Score->bOwnerOnly = true;

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<bool> CapturedTargeted;
	Rep->DispatchHookForTests = [&CapturedTargeted](const FCrowdyStateDelta& /*Delta*/, bool bTargeted)
	{
		CapturedTargeted.Add(bTargeted);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	CapturedTargeted.Reset();

	Actor->RepScore = 21;
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("exactly one delta emitted"), CapturedTargeted.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("an owner-only change ships targeted"), CapturedTargeted[0]);
	TestEqual(TEXT("the owner-only change fired its notify locally"), Actor->ScoreOnRepCount, 1);
	TestEqual(TEXT("the untouched property fired nothing"), Actor->HealthOnRepCount, 0);
	return true;
}

// The one-shot push over an entity this client does not track encodes the property's CURRENT LIVE value, so by
// the time the push is drained the local member already holds it: the notify fires locally too, exactly once per
// mark, and the mark does not keep firing on later ticks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyHostPushTest,
	"CrowdySDK.State.LocalNotifyHostPush", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyHostPushTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("apply actor has a layout"),
		Registry->FindRepLayout(ACrowdyStateApplyTestActor::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(LocalPlayer);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Target = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("target spawned"), Target))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	RegisterProxyParticipant(ES, Id, Target);
	TestEqual(TEXT("the replicator tracks nothing"), Rep->NumOwnedForTest(), 0);

	// The push path reads the live member, so the write happens before the mark, as a caller would do it.
	Target->RepHealth = 33.f;
	Rep->MarkStateDirty(Id, TEXT("RepHealth"));
	Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("the push emitted exactly one delta"), Captured.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the push is HostSourced"),
		(Captured[0].Flags & CrowdyStateDeltaFlags::HostSourced) != 0);
	TestEqual(TEXT("the push fired the notify locally"), Target->HealthOnRepCount, 1);
	TestEqual(TEXT("a property the push did not carry fires nothing"), Target->ScoreOnRepCount, 0);

	// The queue is one-shot: a later tick neither re-emits nor refires.
	Captured.Reset();
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("a later tick re-emits nothing"), Captured.Num(), 0);
	TestEqual(TEXT("a later tick refires nothing"), Target->HealthOnRepCount, 1);
	return true;
}

// A non-actor (subsystem) participant rides the reliable channel instead of the spatial transport, and its local
// notify fires on exactly the same terms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyNonSpatialParticipantTest,
	"CrowdySDK.State.LocalNotifyNonSpatialParticipant", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyNonSpatialParticipantTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("subsystem target has a layout"),
		Registry->FindRepLayout(UCrowdyStateSubsystemTestTarget::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	int32 SpatialCount = 0;
	TArray<FCrowdyStateDelta> ChannelCaptured;
	Rep->DispatchHookForTests = [&SpatialCount](const FCrowdyStateDelta& /*D*/, bool /*bT*/) { ++SpatialCount; };
	Rep->ChannelDispatchHookForTests = [&ChannelCaptured](const FCrowdyStateDelta& D) { ChannelCaptured.Add(D); };

	// A plain UObject fires ProcessEvent without a world, unlike an actor, so no test world is needed here.
	UCrowdyStateSubsystemTestTarget* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();

	// Tracked through the ownership gate rather than the direct-add seam, so it is the real classification of a
	// non-actor participant as non-spatial that decides which transport this delta takes.
	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.OwnerID = LocalPlayer;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Sub;
	TestTrue(TEXT("subsystem participant tracked"), Rep->TryTrackOwnedForTest(Record));

	Rep->RunReplicationLoopForTest();
	ChannelCaptured.Reset();
	TestEqual(TEXT("the initial drain tick fires no notify"), Sub->NotifiedOnRepCount, 0);

	Sub->RepNotified = 7;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("the change rode the channel"), ChannelCaptured.Num(), 1);
	TestEqual(TEXT("no spatial delta was emitted"), SpatialCount, 0);
	TestEqual(TEXT("the subsystem participant fired its notify locally"), Sub->NotifiedOnRepCount, 1);

	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("an unchanged tick does not refire"), Sub->NotifiedOnRepCount, 1);
	return true;
}

// Ordering and multiplicity: every notify runs only after the whole send walk is over, never inline with the
// dispatch that produced it, and every owned entity that changed gets its own. Firing inline would let a notify
// body register or unregister an entity while the owned-entity map is being iterated.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyDeferredPastDispatchTest,
	"CrowdySDK.State.LocalNotifyDeferredPastDispatch", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyDeferredPastDispatchTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("apply actor has a layout"),
		Registry->FindRepLayout(ACrowdyStateApplyTestActor::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* First = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	ACrowdyStateApplyTestActor* Second = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("first actor spawned"), First) || !TestNotNull(TEXT("second actor spawned"), Second))
	{
		return false;
	}

	// Sampled while a delta is in flight: no notify may have run yet at that point.
	int32 FirstCountDuringDispatch = -1;
	int32 SecondCountDuringDispatch = -1;
	Rep->DispatchHookForTests = [&](const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/)
	{
		FirstCountDuringDispatch = FMath::Max(FirstCountDuringDispatch, First->HealthOnRepCount);
		SecondCountDuringDispatch = FMath::Max(SecondCountDuringDispatch, Second->HealthOnRepCount);
	};

	TestTrue(TEXT("first actor registered"), Rep->RegisterOwnedEntityForTest(FGuid::NewGuid(), First));
	TestTrue(TEXT("second actor registered"), Rep->RegisterOwnedEntityForTest(FGuid::NewGuid(), Second));

	Rep->RunReplicationLoopForTest();

	First->RepHealth = 1.f;
	Second->RepHealth = 2.f;
	FirstCountDuringDispatch = -1;
	SecondCountDuringDispatch = -1;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("no notify had run while a delta was being dispatched"), FirstCountDuringDispatch, 0);
	TestEqual(TEXT("no notify had run for the second entity either"), SecondCountDuringDispatch, 0);
	TestEqual(TEXT("the first entity fired its notify"), First->HealthOnRepCount, 1);
	TestEqual(TEXT("the second entity fired its notify"), Second->HealthOnRepCount, 1);
	return true;
}

// Ownership can transfer between the mark that queues a one-shot push and the tick that drains it. The entity is
// then tracked with a shadow at the property defaults, so the pushed value reads as a change to the diff as well
// and the same write reaches the wire from both paths. Both deltas are deliberate (a push is the only route for a
// property the diff never covers), but one write must run the notify body exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyPushAndDiffFireOnceTest,
	"CrowdySDK.State.LocalNotifyPushAndDiffFireOnce", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyPushAndDiffFireOnceTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	if (!TestNotNull(TEXT("apply actor has a layout"),
		Registry->FindRepLayout(ACrowdyStateApplyTestActor::StaticClass())))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Target = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("target spawned"), Target))
	{
		return false;
	}

	// The host writes a property on an entity another client owns and marks it. The entity is untracked here, so
	// the mark queues a one-shot push rather than setting a dirty bit.
	const FGuid Id = FGuid::NewGuid();
	RegisterProxyParticipant(ES, Id, Target);
	Target->RepHealth = 33.f;
	Rep->MarkStateDirty(Id, TEXT("RepHealth"));
	TestEqual(TEXT("nothing is tracked when the push is queued"), Rep->NumOwnedForTest(), 0);

	// The owner hands the entity over before the queued push is drained.
	FCrowdyEntityRecord Handover;
	Handover.NetID = Id;
	Handover.OwnerID = LocalPlayer;
	Handover.Role = ECrowdyRole::Owner;
	Handover.Participant = Target;
	ES->RegisterEntity(Handover);
	Rep->HandleOwnershipChangedForTest(Target, Id, LocalPlayer, FGuid::NewGuid());
	if (!TestTrue(TEXT("the transfer tracked the entity"), Rep->IsTrackedForTest(Id)))
	{
		return false;
	}

	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("the push and the diff both emitted"), Captured.Num(), 2);
	TestEqual(TEXT("the pushed property fired its notify exactly once"), Target->HealthOnRepCount, 1);
	TestEqual(TEXT("a property neither path carried fires nothing"), Target->ScoreOnRepCount, 0);

	// The queue is one-shot and the diff is now caught up, so the tick after is silent on both counts.
	Captured.Reset();
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("a later tick re-emits nothing"), Captured.Num(), 0);
	TestEqual(TEXT("a later tick refires nothing"), Target->HealthOnRepCount, 1);
	return true;
}

// A notify body that adjusts the very property it fired for is the ordinary shape: clamping a health value,
// normalising a rotator, snapping to a grid. That write happens after the value was already sent, so it has to
// settle rather than read as a fresh change on the next tick and send + notify again, and again, for as long as
// the body keeps adjusting. No fixture declares such a notify, so one layout slot is re-pointed at the plain
// counter the existing notify increments: the replicated property and the property its notify writes are then the
// same one, and the body is deliberately not idempotent so an unsettled shadow never stops re-sending.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLocalNotifyWriteBackSettlesTest,
	"CrowdySDK.State.LocalNotifyWriteBackSettles", CrowdyStateLocalNotifyTestFlags)
bool FCrowdyStateLocalNotifyWriteBackSettlesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	FCrowdyRepProperty* Slot = FindMutableLayoutProperty(*Layout, TEXT("RepScore"));
	if (!TestNotNull(TEXT("RepScore present in the layout"), Slot))
	{
		return false;
	}

	FProperty* SelfAdjusted = ActorClass->FindPropertyByName(FName(TEXT("HealthOnRepCount")));
	if (!TestNotNull(TEXT("the counter property resolves"), SelfAdjusted))
	{
		return false;
	}
	Slot->Property = SelfAdjusted;
	Slot->OnRepFunctionName = FName(TEXT("OnRep_Health"));

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);

	TArray<FCrowdyStateDelta> Captured;
	Rep->DispatchHookForTests = [&Captured](const FCrowdyStateDelta& Delta, bool /*bTargeted*/)
	{
		Captured.Add(Delta);
	};

	FCrowdyStateTestWorld TestWorld;
	ACrowdyStateApplyTestActor* Actor = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("owned actor registered"), Rep->RegisterOwnedEntityForTest(Id, Actor));

	Rep->RunReplicationLoopForTest();
	Captured.Reset();
	TestEqual(TEXT("the initial drain tick fires no notify"), Actor->HealthOnRepCount, 0);

	// Gameplay moves the value; the send fires the notify, whose body adjusts the same property again.
	Actor->HealthOnRepCount = 5;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the change emitted exactly one delta"), Captured.Num(), 1);
	TestEqual(TEXT("the notify ran once and adjusted its own property"), Actor->HealthOnRepCount, 6);
	Captured.Reset();

	// The adjustment settled, so the next tick has nothing to say and nothing to fire.
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the notify's own adjustment does not start a send loop"), Captured.Num(), 0);
	TestEqual(TEXT("the notify does not refire on the tick after"), Actor->HealthOnRepCount, 6);

	// A third tick proves it settled rather than merely lagging one tick behind.
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("it stays settled"), Captured.Num(), 0);
	TestEqual(TEXT("the notify count is unchanged"), Actor->HealthOnRepCount, 6);
	return true;
}

#endif
