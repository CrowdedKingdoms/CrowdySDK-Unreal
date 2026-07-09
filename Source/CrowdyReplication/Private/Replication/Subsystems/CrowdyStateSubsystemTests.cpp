#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "UObject/UnrealType.h"

// Subsystem CrowdyState over the channel (Subsystem Replication Phase 1). These tests exercise a non-actor
// (subsystem) participant end to end: deterministic identity, host-gated enroll -> track (non-spatial,
// auto-diffing), channel-vs-spatial dispatch routing, the FCrowdyStateDelta channel codec, apply + OnRep onto
// a worldless plain UObject, host promotion, and the ForwardChannelRpc discriminator.
namespace
{
	constexpr EAutomationTestFlags CrowdyStateSubsystemTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package NewObject
	// trips a ClassWithin ensure, so outer it to a bare GameInstance (the established Phase 1 idiom).
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

	// A bare entity subsystem (UWorldSubsystem, no ClassWithin) with a local player id set, so
	// RegisterParticipant / FindRecord / GetLocalPlayerID resolve headlessly.
	UCrowdyEntitySubsystem* MakeEntitySubsystem(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		ES->SetLocalPlayerID(LocalPlayer);
		return ES;
	}

	UCrowdyEventRouter* MakeRouter(UCrowdyAutoRegistry* Registry, UCrowdyEntitySubsystem* Entities)
	{
		UCrowdyEventRouter* Router = NewObject<UCrowdyEventRouter>(GetTransientPackage());
		Router->SetAutoRegistryForTest(Registry);
		Router->SetEntitySubsystemForTest(Entities);
		return Router;
	}

	int32 IndexOfPropertyName(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		for (int32 Index = 0; Index < Layout.Properties.Num(); ++Index)
		{
			const FProperty* Prop = Layout.Properties[Index].Property;
			if (Prop && Prop->GetFName() == Wanted)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	FCrowdyInboundEvent MakeInboundStateEvent(const FCrowdyStateDelta& Delta)
	{
		FCrowdyInboundEvent Event;
		Event.Payload = FInstancedStruct::Make(Delta);
		Event.bTargetedDelivery = false;
		Event.SenderID = Delta.SenderID;
		Event.Target = ECrowdyTarget::Everyone;
		return Event;
	}

	// Registers any UObject as a resolvable RemoteProxy participant so FindParticipant(EntityId) returns it
	// (its OwnerID is a fresh non-local guid, so IsLocallyOwned is false and the owned-entity gate is bypassed).
	void RegisterProxyParticipant(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, UObject* Participant)
	{
		FCrowdyEntityRecord Rec;
		Rec.NetID = EntityId;
		Rec.OwnerID = FGuid::NewGuid();
		Rec.Role = ECrowdyRole::RemoteProxy;
		Rec.Participant = Participant;
		Entities->RegisterEntity(Rec);
	}
}

// A host participant's NetID is derived from its class path: two mints of the same class produce the SAME id,
// a different class a DIFFERENT one, and both are valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemDeterministicIdentityTest,
	"CrowdySDK.State.SubsystemDeterministicIdentity", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemDeterministicIdentityTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entity subsystem created"), ES))
	{
		return false;
	}

	UCrowdyStateSubsystemTestTarget* SubA1 = NewObject<UCrowdyStateSubsystemTestTarget>();
	UCrowdyStateSubsystemTestTarget* SubA2 = NewObject<UCrowdyStateSubsystemTestTarget>();
	UCrowdyStateSubsystemTestTargetB* SubB = NewObject<UCrowdyStateSubsystemTestTargetB>();

	const FGuid IdA1 = ES->RegisterParticipant(SubA1, ECrowdyOwnership::Host);
	const FGuid IdA2 = ES->RegisterParticipant(SubA2, ECrowdyOwnership::Host);
	const FGuid IdB = ES->RegisterParticipant(SubB, ECrowdyOwnership::Host);

	TestTrue(TEXT("class A mint is valid"), IdA1.IsValid());
	TestTrue(TEXT("class B mint is valid"), IdB.IsValid());
	TestEqual(TEXT("same class -> same deterministic id"), IdA1, IdA2);
	TestNotEqual(TEXT("different class -> different id"), IdA1, IdB);
	return true;
}

// Enroll a host-owned subsystem: with local == host it is tracked (non-spatial), and changing a CrowdyState
// property then running the loop emits exactly one CHANNEL delta (auto-diff runs, unlike a spatial host-owned
// world actor) and zero spatial deltas. With local != host it is not tracked and emits nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemEnrollTracksOnHostAutoDiffsTest,
	"CrowdySDK.State.SubsystemEnrollTracksOnHostAutoDiffs", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemEnrollTracksOnHostAutoDiffsTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	// The subsystem class must have a rep layout (it carries CrowdyState properties).
	if (!TestNotNull(TEXT("subsystem class has a layout"),
		Registry->FindRepLayout(UCrowdyStateSubsystemTestTarget::StaticClass())))
	{
		return false;
	}

	// Host case: local IS host.
	{
		const FGuid LocalPlayer = FGuid::NewGuid();
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		Rep->SetHostIDForTest(LocalPlayer);
		UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
		Rep->SetEntitySubsystemForTest(ES);

		int32 SpatialCount = 0;
		TArray<FCrowdyStateDelta> ChannelCaptured;
		Rep->DispatchHookForTests = [&SpatialCount](const FCrowdyStateDelta& /*D*/, bool /*bT*/) { ++SpatialCount; };
		Rep->ChannelDispatchHookForTests = [&ChannelCaptured](const FCrowdyStateDelta& D) { ChannelCaptured.Add(D); };

		UCrowdyStateSubsystemTestTarget* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();
		const FGuid NetID = ES->RegisterParticipant(Sub, ECrowdyOwnership::Host);

		// Drive registration through the normal path (FindRecord -> TryTrackOwned).
		Rep->HandleEntityRegisteredForTest(NetID);
		TestTrue(TEXT("host-owned subsystem tracked when local is host"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("exactly one owned entry"), Rep->NumOwnedForTest(), 1);
		TestEqual(TEXT("the host-owned id is remembered"), Rep->NumKnownHostOwnedForTest(), 1);

		// Drain the first tick (everything default -> nothing changes, no keyframe due at t=0).
		Rep->RunReplicationLoopForTest();
		ChannelCaptured.Reset();
		SpatialCount = 0;

		// Change a spatial (auto-diff) property: for a subsystem this DOES emit (bHostOwned is false), over the channel.
		Sub->RepInt = 5;
		Rep->RunReplicationLoopForTest();
		TestEqual(TEXT("subsystem change emits exactly one channel delta"), ChannelCaptured.Num(), 1);
		TestEqual(TEXT("subsystem change emits no spatial delta"), SpatialCount, 0);
	}

	// Non-host case: someone else holds the host, so the host-owned subsystem is NOT tracked and emits nothing.
	{
		const FGuid LocalPlayer = FGuid::NewGuid();
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
		Rep->SetTimeForTest(0.0);
		Rep->SetHostIDForTest(FGuid::NewGuid()); // someone else is host
		UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
		Rep->SetEntitySubsystemForTest(ES);

		int32 ChannelCount = 0;
		Rep->ChannelDispatchHookForTests = [&ChannelCount](const FCrowdyStateDelta& /*D*/) { ++ChannelCount; };

		UCrowdyStateSubsystemTestTarget* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();
		const FGuid NetID = ES->RegisterParticipant(Sub, ECrowdyOwnership::Host);
		Rep->HandleEntityRegisteredForTest(NetID);

		TestFalse(TEXT("non-host does not track the host-owned subsystem"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("non-host tracks nothing"), Rep->NumOwnedForTest(), 0);

		Sub->RepInt = 9;
		Rep->RunReplicationLoopForTest();
		TestEqual(TEXT("non-host emits no channel delta"), ChannelCount, 0);
	}

	return true;
}

// Dispatch routing: a subsystem participant's delta hits the CHANNEL hook, an actor participant's delta hits the
// SPATIAL hook (broadcast, bTargeted false) in the same replicator on the same tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemDeltaRoutesToChannelNotSpatialTest,
	"CrowdySDK.State.SubsystemDeltaRoutesToChannelNotSpatial", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemDeltaRoutesToChannelNotSpatialTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->SetHostIDForTest(LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	int32 SpatialCount = 0;
	TArray<bool> SpatialTargeted;
	int32 ChannelCount = 0;
	Rep->DispatchHookForTests = [&SpatialCount, &SpatialTargeted](const FCrowdyStateDelta& /*D*/, bool bT)
	{
		++SpatialCount;
		SpatialTargeted.Add(bT);
	};
	Rep->ChannelDispatchHookForTests = [&ChannelCount](const FCrowdyStateDelta& /*D*/) { ++ChannelCount; };

	// A subsystem participant (host-owned, non-spatial) enrolled via the normal path.
	UCrowdyStateSubsystemTestTarget* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid SubID = ES->RegisterParticipant(Sub, ECrowdyOwnership::Host);
	Rep->HandleEntityRegisteredForTest(SubID);
	TestTrue(TEXT("subsystem tracked"), Rep->IsTrackedForTest(SubID));

	// An actor participant (owner, spatial) registered directly.
	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid ActorID = FGuid::NewGuid();
	TestTrue(TEXT("actor registered"), Rep->RegisterOwnedEntityForTest(ActorID, Actor));

	// Drain the first tick.
	Rep->RunReplicationLoopForTest();
	SpatialCount = 0;
	SpatialTargeted.Reset();
	ChannelCount = 0;

	// Change one spatial property on each; the subsystem routes to the channel, the actor to the spatial broadcast.
	Sub->RepInt = 1;
	Actor->RepInt = 2;
	Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("subsystem delta hits the channel hook"), ChannelCount, 1);
	if (TestEqual(TEXT("actor delta hits the spatial hook"), SpatialCount, 1))
	{
		TestFalse(TEXT("the actor's spatial delta is a broadcast"), SpatialTargeted[0]);
	}
	return true;
}

// The FCrowdyStateDelta channel codec round-trips every field, and a wrong tag / wrong version / truncated /
// trailing-byte / forged-Blob-length payload each drops cleanly (the forged length does not OOM: it is
// bounds-checked before any allocation). All drops log Warnings, not Errors, so no AddExpectedError is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateChannelStateCodecRoundTripTest,
	"CrowdySDK.State.ChannelStateCodecRoundTrip", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateChannelStateCodecRoundTripTest::RunTest(const FString& Parameters)
{
	FCrowdyStateDelta Delta;
	Delta.ClassID = 0x0123456789ABCDEFll;
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = 0x7766554433221100ll;
	Delta.Flags = CrowdyStateDeltaFlags::HostSourced | CrowdyStateDeltaFlags::Keyframe;
	Delta.Blob = TArray<uint8>({ 1, 2, 3, 4, 5, 6, 7 });

	TArray<uint8> Payload;
	FCrowdyStateCodec::EncodeChannelStateDelta(Delta, Payload);

	TestTrue(TEXT("payload is non-empty"), Payload.Num() > 2);
	TestEqual(TEXT("payload leads with the state kind tag"), static_cast<int32>(Payload[0]),
		static_cast<int32>(CrowdyChannelStateDeltaTag));
	TestEqual(TEXT("second byte is the state channel version"), static_cast<int32>(Payload[1]),
		static_cast<int32>(CrowdyChannelStateDeltaVersion));

	// Round-trip.
	{
		FCrowdyStateDelta Out;
		const bool bOk = FCrowdyStateCodec::DecodeChannelStateDelta(Payload, Out);
		TestTrue(TEXT("payload decodes"), bOk);
		TestEqual(TEXT("ClassID round-trips"), Out.ClassID, Delta.ClassID);
		TestEqual(TEXT("EntityID round-trips"), Out.EntityID, Delta.EntityID);
		TestEqual(TEXT("SenderID round-trips"), Out.SenderID, Delta.SenderID);
		TestEqual(TEXT("LayoutHash round-trips"), Out.LayoutHash, Delta.LayoutHash);
		TestEqual(TEXT("Flags round-trip"), static_cast<int32>(Out.Flags), static_cast<int32>(Delta.Flags));
		TestEqual(TEXT("Blob round-trips"), Out.Blob, Delta.Blob);
	}

	// Wrong kind tag.
	{
		TArray<uint8> Bad = Payload;
		Bad[0] = 0x01;
		FCrowdyStateDelta Out;
		TestFalse(TEXT("a wrong kind tag drops"), FCrowdyStateCodec::DecodeChannelStateDelta(Bad, Out));
	}

	// Wrong version.
	{
		TArray<uint8> Bad = Payload;
		Bad[1] = static_cast<uint8>(CrowdyChannelStateDeltaVersion + 1);
		FCrowdyStateDelta Out;
		TestFalse(TEXT("a wrong version drops"), FCrowdyStateCodec::DecodeChannelStateDelta(Bad, Out));
	}

	// Truncated (drop the last byte of the body).
	{
		TArray<uint8> Bad = Payload;
		Bad.SetNum(Bad.Num() - 1);
		FCrowdyStateDelta Out;
		TestFalse(TEXT("a truncated payload drops"), FCrowdyStateCodec::DecodeChannelStateDelta(Bad, Out));
	}

	// Trailing byte.
	{
		TArray<uint8> Bad = Payload;
		Bad.Add(0xAB);
		FCrowdyStateDelta Out;
		TestFalse(TEXT("a trailing byte drops"), FCrowdyStateCodec::DecodeChannelStateDelta(Bad, Out));
	}

	// Forged Blob length must NOT OOM: encode an EMPTY-blob delta (so the last 4 bytes ARE the int32 length
	// prefix, value 0), then overwrite them with a huge positive value. Decode must reject it up front.
	{
		FCrowdyStateDelta EmptyBlobDelta = Delta;
		EmptyBlobDelta.Blob.Reset();
		TArray<uint8> EmptyPayload;
		FCrowdyStateCodec::EncodeChannelStateDelta(EmptyBlobDelta, EmptyPayload);

		const int32 N = EmptyPayload.Num();
		if (TestTrue(TEXT("empty-blob payload has room for a length prefix"), N >= 4))
		{
			// Little-endian 0x7FFFFFFF (a ~2GB forged length).
			EmptyPayload[N - 4] = 0xFF;
			EmptyPayload[N - 3] = 0xFF;
			EmptyPayload[N - 2] = 0xFF;
			EmptyPayload[N - 1] = 0x7F;
			FCrowdyStateDelta Out;
			TestFalse(TEXT("a forged Blob length drops without OOM"),
				FCrowdyStateCodec::DecodeChannelStateDelta(EmptyPayload, Out));
		}
	}

	return true;
}

// Apply onto a subsystem participant: an inbound FCrowdyStateDelta resolved via FindParticipant writes the
// values onto the plain UObject and fires its CrowdyOnRep. No editor world is needed  a plain UObject fires
// ProcessEvent without one (unlike an AActor).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemApplyFiresOnRepTest,
	"CrowdySDK.State.SubsystemApplyFiresOnRep", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemApplyFiresOnRepTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* SubClass = UCrowdyStateSubsystemTestTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(SubClass);
	if (!TestNotNull(TEXT("subsystem class has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(SubClass));

	// Source: change RepInt and RepNotified (the OnRep'd property).
	UCrowdyStateSubsystemTestTarget* Source = NewObject<UCrowdyStateSubsystemTestTarget>();
	Source->RepInt = 42;
	Source->RepNotified = 7;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepNotified") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (TestTrue(*FString::Printf(TEXT("%s present in layout"), Name), Index != INDEX_NONE))
		{
			Dirty[Index] = true;
		}
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid(); // foreign, so no self-echo drop
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	// Register the target subsystem as a resolvable proxy participant (no world needed).
	UCrowdyStateSubsystemTestTarget* Target = NewObject<UCrowdyStateSubsystemTestTarget>();
	RegisterProxyParticipant(Entities, Delta.EntityID, Target);

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("RepInt applied to the subsystem"), Target->RepInt, 42);
	TestEqual(TEXT("RepNotified applied to the subsystem"), Target->RepNotified, 7);
	TestEqual(TEXT("OnRep_Notified fired exactly once"), Target->NotifiedOnRepCount, 1);
	return true;
}

// A non-host RECEIVES a host-owned subsystem's state through the real deployment path: it enrolls its OWN
// singleton via RegisterParticipant(Host) -> Role=HostOwned, OwnerID=FGuid() (unlike the RemoteProxy fixture used
// above, whose fresh non-local owner bypasses both authority gates). So the receive path runs the HostOwned
// else-if gate (CrowdyEventRouter DispatchStateDelta): a non-HostSourced delta MUST drop (values unchanged, no
// OnRep), a HostSourced host correction MUST apply and fire OnRep. This is the core Phase 1 authority behavior a
// RemoteProxy target cannot exercise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemHostOwnedApplyRespectsHostSourcedTest,
	"CrowdySDK.State.SubsystemHostOwnedApplyRespectsHostSourced", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemHostOwnedApplyRespectsHostSourcedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* SubClass = UCrowdyStateSubsystemTestTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(SubClass);
	if (!TestNotNull(TEXT("subsystem class has a layout"), Layout))
	{
		return false;
	}

	// A NON-host receiver: local player is valid and is NOT the host, so a locally-enrolled host-owned singleton
	// has OwnerID=FGuid() (never equal to the local player), making bWeOwnTarget false and routing through the
	// HostOwned else-if gate rather than the owned-entity gate.
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid HostID = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(SubClass));

	// Source: the values the host is replicating.
	UCrowdyStateSubsystemTestTarget* Source = NewObject<UCrowdyStateSubsystemTestTarget>();
	Source->RepInt = 42;
	Source->RepNotified = 7;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepNotified") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (TestTrue(*FString::Printf(TEXT("%s present in layout"), Name), Index != INDEX_NONE))
		{
			Dirty[Index] = true;
		}
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	// Enroll the RECEIVING singleton the real way: RegisterParticipant(Host) -> HostOwned record, OwnerID invalid.
	UCrowdyStateSubsystemTestTarget* Target = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid EntityID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("host-owned participant enrolled"), EntityID.IsValid()))
	{
		return false;
	}
	if (const FCrowdyEntityRecord* Record = Entities->FindRecord(EntityID))
	{
		TestEqual(TEXT("enrolled participant is HostOwned"), static_cast<int32>(Record->Role),
			static_cast<int32>(ECrowdyRole::HostOwned));
		TestFalse(TEXT("host-owned record has no local owner"), Entities->IsLocallyOwned(EntityID));
	}

	auto MakeDelta = [&](uint8 Flags)
	{
		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = EntityID;
		Delta.SenderID = HostID; // foreign to the receiver, so no self-echo drop
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = Flags;
		Delta.Blob = Blob;
		return Delta;
	};

	// A non-HostSourced delta for a host-owned world entity must be dropped (only the host may write world state).
	Router->DispatchEvent(MakeInboundStateEvent(MakeDelta(/*Flags=*/0)));
	TestEqual(TEXT("non-host delta does NOT apply RepInt"), Target->RepInt, 0);
	TestEqual(TEXT("non-host delta does NOT apply RepNotified"), Target->RepNotified, 0);
	TestEqual(TEXT("non-host delta fires no OnRep"), Target->NotifiedOnRepCount, 0);

	// A HostSourced correction applies and fires OnRep.
	Router->DispatchEvent(MakeInboundStateEvent(MakeDelta(CrowdyStateDeltaFlags::HostSourced)));
	TestEqual(TEXT("HostSourced delta applies RepInt"), Target->RepInt, 42);
	TestEqual(TEXT("HostSourced delta applies RepNotified"), Target->RepNotified, 7);
	TestEqual(TEXT("HostSourced delta fires OnRep exactly once"), Target->NotifiedOnRepCount, 1);
	return true;
}

// A non-host does not track an enrolled host-owned subsystem, but HandleHostChanged(newHost=local) promotes it
// to tracked (and it then diffs over the channel).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateSubsystemNonHostRejectsThenPromotesTest,
	"CrowdySDK.State.SubsystemNonHostRejectsThenPromotes", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateSubsystemNonHostRejectsThenPromotesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherHost = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeReplicator(Registry, LocalPlayer);
	Rep->SetTimeForTest(0.0);
	Rep->SetHostIDForTest(OtherHost); // someone else holds the host
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	int32 ChannelCount = 0;
	Rep->ChannelDispatchHookForTests = [&ChannelCount](const FCrowdyStateDelta& /*D*/) { ++ChannelCount; };

	UCrowdyStateSubsystemTestTarget* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();
	const FGuid NetID = ES->RegisterParticipant(Sub, ECrowdyOwnership::Host);
	Rep->HandleEntityRegisteredForTest(NetID);

	// Non-host: remembered but not tracked.
	TestEqual(TEXT("non-host tracks nothing"), Rep->NumOwnedForTest(), 0);
	TestEqual(TEXT("non-host still remembers the host-owned subsystem id"), Rep->NumKnownHostOwnedForTest(), 1);

	// Become host: the change event promotes the known host-owned subsystem to tracked.
	Rep->SetHostIDForTest(LocalPlayer);
	Rep->HandleHostChangedForTest(LocalPlayer, OtherHost);
	TestTrue(TEXT("subsystem tracked after promotion"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("exactly one owned entry after promotion"), Rep->NumOwnedForTest(), 1);

	// Drain, then prove it now diffs over the channel.
	Rep->RunReplicationLoopForTest();
	ChannelCount = 0;
	Sub->RepInt = 3;
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("promoted subsystem now emits over the channel"), ChannelCount, 1);
	return true;
}

// The ForwardChannelRpc discriminator: a state channel payload (leading 0xC5) decodes as a state delta and is
// rejected by the RPC decoder; an RPC channel payload (leading version byte) decodes as an RPC and is rejected
// by the state decoder. Unit-level via the two codecs + the peek, no channel/bridge stood up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateForwardChannelDiscriminatesTest,
	"CrowdySDK.State.ForwardChannelDiscriminates", CrowdyStateSubsystemTestFlags)
bool FCrowdyStateForwardChannelDiscriminatesTest::RunTest(const FString& Parameters)
{
	// A state channel payload.
	FCrowdyStateDelta Delta;
	Delta.ClassID = 7;
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = 99;
	Delta.Flags = 0;
	Delta.Blob = TArray<uint8>({ 10, 20, 30 });

	TArray<uint8> StatePayload;
	FCrowdyStateCodec::EncodeChannelStateDelta(Delta, StatePayload);

	// An RPC channel payload.
	FCrowdyRpcCall Call;
	Call.ClassID = 3;
	Call.EntityID = FGuid::NewGuid();
	Call.SenderID = FGuid::NewGuid();
	Call.FunctionID = 5;
	Call.ParamBlob = TArray<uint8>({ 40, 50 });

	TArray<uint8> RpcPayload;
	FCrowdyRPC::EncodeChannelRpc(Call, /*Flags=*/0, RpcPayload);

	// The discriminator: distinct leading bytes.
	TestTrue(TEXT("state payload non-empty"), StatePayload.Num() > 0);
	TestTrue(TEXT("rpc payload non-empty"), RpcPayload.Num() > 0);
	TestEqual(TEXT("state payload leads with the state tag"), static_cast<int32>(StatePayload[0]),
		static_cast<int32>(CrowdyChannelStateDeltaTag));
	TestNotEqual(TEXT("rpc payload does NOT lead with the state tag"), static_cast<int32>(RpcPayload[0]),
		static_cast<int32>(CrowdyChannelStateDeltaTag));

	// The state payload routes to state decode, and the RPC decoder rejects it (its version byte is 0xC5).
	{
		FCrowdyStateDelta OutState;
		TestTrue(TEXT("state payload decodes as a state delta"),
			FCrowdyStateCodec::DecodeChannelStateDelta(StatePayload, OutState));

		FCrowdyRpcCall OutCall;
		uint8 OutFlags = 0;
		TestFalse(TEXT("the RPC decoder rejects a state payload"),
			FCrowdyRPC::DecodeChannelRpc(StatePayload, OutCall, OutFlags));
	}

	// The RPC payload routes to RPC decode, and the state decoder rejects it (its tag byte is not 0xC5).
	{
		FCrowdyRpcCall OutCall;
		uint8 OutFlags = 0;
		TestTrue(TEXT("rpc payload decodes as an RPC"),
			FCrowdyRPC::DecodeChannelRpc(RpcPayload, OutCall, OutFlags));

		FCrowdyStateDelta OutState;
		TestFalse(TEXT("the state decoder rejects an RPC payload"),
			FCrowdyStateCodec::DecodeChannelStateDelta(RpcPayload, OutState));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
