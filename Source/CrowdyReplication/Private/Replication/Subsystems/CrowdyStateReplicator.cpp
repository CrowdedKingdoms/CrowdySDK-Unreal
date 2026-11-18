#include "Replication/Subsystems/CrowdyStateReplicator.h"

#include "CrowdyReplicationLog.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyBitwiseCompare.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates, EReloadCompleteReason
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UCrowdyClassRegistry.h"

namespace
{
	// Sends Dirty in parts that fit Limit and marks what went in Sent. A property too big to go alone stays
	// out of Sent, so it stays dirty without holding back the rest.
	template <typename FSendBlob>
	void SendWithinLimit(FCrowdyOwnedEntityState& State, const FCrowdyRepLayout& Layout, const UObject* Participant,
		const TBitArray<>& Dirty, const int32 Limit, TBitArray<>& Sent, FSendBlob&& SendBlob)
	{
		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(Layout, Participant, Dirty, /*bKeyframe=*/false, Blob);
		if (Blob.Num() <= Limit)
		{
			SendBlob(MoveTemp(Blob));
			Sent.CombineWithBitwiseOR(Dirty, EBitwiseOperatorFlags::MaintainSize);
			return;
		}

		TBitArray<> Part(false, Dirty.Num());
		TArray<uint8> PartBlob;
		for (TConstSetBitIterator<> It(Dirty); It; ++It)
		{
			const int32 Index = It.GetIndex();
			Part[Index] = true;
			FCrowdyStateCodec::Encode(Layout, Participant, Part, /*bKeyframe=*/false, Blob);
			if (Blob.Num() <= Limit)
			{
				Exchange(PartBlob, Blob);
				continue;
			}

			Part[Index] = false;
			if (!PartBlob.IsEmpty())
			{
				SendBlob(MoveTemp(PartBlob));
				PartBlob.Reset();
				Sent.CombineWithBitwiseOR(Part, EBitwiseOperatorFlags::MaintainSize);
				Part.Init(false, Dirty.Num());

				Part[Index] = true;
				FCrowdyStateCodec::Encode(Layout, Participant, Part, /*bKeyframe=*/false, Blob);
				if (Blob.Num() <= Limit)
				{
					Exchange(PartBlob, Blob);
					continue;
				}
				Part[Index] = false;
			}

			if (!State.WarnedTooLarge.IsValidIndex(Index))
			{
				State.WarnedTooLarge.Init(false, Dirty.Num());
			}
			UE_CLOG(!State.WarnedTooLarge[Index], LogCrowdyReplication, Warning,
				TEXT("[CrowdyState] %s on %s (entity %s) is %d bytes, over the %d one update can carry. It is not being sent, and clients keep their old value until it fits."),
				*Layout.Properties[Index].Property->GetName(), *GetNameSafe(Participant), *State.EntityID.ToString(),
				Blob.Num(), Limit);
			State.WarnedTooLarge[Index] = true;
		}

		if (!PartBlob.IsEmpty())
		{
			SendBlob(MoveTemp(PartBlob));
			Sent.CombineWithBitwiseOR(Part, EBitwiseOperatorFlags::MaintainSize);
		}
	}

	// A behavior-gating CVar (not a trace flag), so it lives here beside UCrowdyStateReplicator rather than in
	// CrowdyReplication.cpp's crowdy.*.trace family exactly where crowdy.rpc.loopback lives relative to
	// FCrowdyRPC. Off (0) by default: byte-identical to today, matching that precedent's own invariant.
	TAutoConsoleVariable<int32> CVarCrowdyStateLoopback(
		TEXT("crowdy.state.loopback"), 0,
		TEXT("When non-zero, a sent CrowdyState delta for an owned entity is also decoded onto a local ")
		TEXT("'mirror' entity (a lazily-spawned second instance of the same class, RemoteProxy role, a ")
		TEXT("distinct NetID) so decode/OnRep/ownership checks can be exercised from one PIE client. ")
		TEXT("Unlike crowdy.rpc.loopback this cannot replay onto the same actor (an owner's own entity ")
		TEXT("drops every foreign delta, and decode's changed-detection would report no ")
		TEXT("change against identical values); the mirror is a genuinely distinct receive target instead."),
		ECVF_Default);

	// Purely cosmetic: offsets the mirror actor from its source so both are visible side by side in PIE.
	constexpr float CrowdyStateLoopbackMirrorOffsetX = 200.f;

	void WarnUnknownStateProperty(TSet<TPair<FObjectKey, FName>>& Warned, const UClass* Class, FName PropertyName, const FGuid& EntityID)
	{
		bool bAlreadyWarned = false;
		Warned.Add(TPair<FObjectKey, FName>(FObjectKey(Class), PropertyName), &bAlreadyWarned);
		if (bAlreadyWarned)
		{
			return;
		}

		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyStateReplicator] MarkStateDirty on '%s' (class '%s', entity %s) ignored: no such CrowdyState property."),
			*PropertyName.ToString(), *GetNameSafe(Class), *EntityID.ToString());
	}

	// DestroyValue every InitializeValue'd shadow slot, through the snapshotted properties rather than the
	// registry's layout: the layout object may have been freed by a rescan, while these UClass-owned
	// FProperty* stay valid across one. A null slot (a recompile left a hole) is skipped, never crashed.
	void DestroyShadowSlots(FCrowdyOwnedEntityState& State)
	{
		if (!State.bShadowInitialized)
		{
			return;
		}

		uint8* const Base = State.ShadowData.GetData();
		if (!Base)
		{
			return;
		}

		const int32 Count = FMath::Min(State.ShadowProps.Num(), State.ShadowOffsets.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (const FProperty* Property = State.ShadowProps[Index])
			{
				Property->DestroyValue(Base + State.ShadowOffsets[Index]);
			}
		}
	}
}

FCrowdyOwnedEntityState::~FCrowdyOwnedEntityState()
{
	// A never-built, moved-from, or abandoned state (see UCrowdyStateReplicator::HandleReloadComplete) has
	// no live slots to destroy; DestroyShadowSlots guards on bShadowInitialized.
	DestroyShadowSlots(*this);
}

void UCrowdyStateReplicator::SetRegistryForTest(UCrowdyAutoRegistry* InRegistry)
{
	AutoRegistry = InRegistry;
}

void UCrowdyStateReplicator::SetEntitySubsystemForTest(UCrowdyEntitySubsystem* InES)
{
	EntitySubsystem = InES;
}

void UCrowdyStateReplicator::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The entity subsystem must exist before we bind its registration delegates.
	Collection.InitializeDependency(UCrowdyEntitySubsystem::StaticClass());

	const UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return;
	}

	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	bIsTicking = Profile && Profile->bUseStateReplicator;
	if (bIsTicking)
	{
		ReplicationInterval = 1.0f / FMath::Max(1, Profile->ReplicationIntervalHz);
		RelevanceDistance = Profile->StateRelevanceDistance;
		// A profile value <= 0 disables the keyframe heartbeat map-wide (on-change replication is untouched); a
		// positive value clamps up to a sane floor. ReplicationLoop guards `KeyframeInterval > 0` before firing,
		// so 0 is a clean off switch.
		KeyframeInterval = Profile->StateKeyframeIntervalSeconds > 0.f
			? FMath::Max(0.1f, Profile->StateKeyframeIntervalSeconds)
			: 0.f;
	}

	if (UCrowdyEntitySubsystem* ES = World->GetSubsystem<UCrowdyEntitySubsystem>())
	{
		EntitySubsystem = ES;
		LocalPlayerID = ES->GetLocalPlayerID();
		ES->OnEntityRegistered.AddDynamic(this, &UCrowdyStateReplicator::HandleEntityRegistered);
		ES->OnEntityUnregistered.AddDynamic(this, &UCrowdyStateReplicator::HandleEntityUnregistered);
		ES->OnEntityOwnershipChanged.AddDynamic(this, &UCrowdyStateReplicator::HandleOwnershipChanged);
	}

	// GameInstance is null during the transient world at UGameEngine::Init; guard it.
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>();

		// The game session survives level travel, and a Leader already in place when this world starts is read live
		// at registration, so only a change mid-session needs the event.
		if (UCrowdyGameSession* GS = GameInstance->GetSubsystem<UCrowdyGameSession>())
		{
			GameSession = GS;
			GS->OnHostIDUpdated.AddUObject(this, &UCrowdyStateReplicator::HandleLeaderChanged);
		}
	}

	// The spawn flow fires OnEntityRegistered, so binding is enough for entities created after us. A
	// replicator created after some entities already registered would miss them; the entity subsystem
	// exposes no cheap owned-set enumeration, so this accepts binding-only seeding as a known limitation.

#if WITH_EDITOR
	// A Live Coding reload rebuilds the registry's layouts and may reinstance replicated actor classes,
	// stranding this replicator's per-entity shadows; clean up any stale ones when it completes.
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddWeakLambda(
		this, [this](EReloadCompleteReason) { HandleReloadComplete(); });
#endif
}

void UCrowdyStateReplicator::Deinitialize()
{
	if (UCrowdyEntitySubsystem* ES = EntitySubsystem.Get())
	{
		ES->OnEntityRegistered.RemoveDynamic(this, &UCrowdyStateReplicator::HandleEntityRegistered);
		ES->OnEntityUnregistered.RemoveDynamic(this, &UCrowdyStateReplicator::HandleEntityUnregistered);
		ES->OnEntityOwnershipChanged.RemoveDynamic(this, &UCrowdyStateReplicator::HandleOwnershipChanged);
	}

	if (UCrowdyGameSession* GS = GameSession.Get())
	{
		GS->OnHostIDUpdated.RemoveAll(this);
	}

#if WITH_EDITOR
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}
#endif

	// Destructors run DestroyValue on every shadow slot.
	OwnedEntities.Empty();

	// Loopback mirrors are real actors in the world; destroy every survivor before teardown rather than
	// leaving them to the world's own actor GC (harmless either way, but explicit is cheap and matches how
	// OwnedEntities' shadow buffers are torn down deliberately above rather than left to fall out of scope).
	for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : LoopbackMirrors)
	{
		if (AActor* Mirror = Pair.Value.Get())
		{
			Mirror->Destroy();
		}
	}
	LoopbackMirrors.Empty();

	Super::Deinitialize();
}

bool UCrowdyStateReplicator::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	// PIE or Game only, mirroring UCrowdyAutoReplicator. The bUseStateReplicator gate is checked in Initialize.
	return World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game;
}

TStatId UCrowdyStateReplicator::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyStateReplicator, STATGROUP_Tickables);
}

void UCrowdyStateReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bIsTicking)
	{
		return;
	}

	ReplicationAccumulator += DeltaTime;
	if (ReplicationAccumulator >= ReplicationInterval)
	{
		ReplicationAccumulator -= ReplicationInterval;
		ReplicationLoop();
	}
}

void UCrowdyStateReplicator::HandleEntityRegistered(const FGuid& NetID)
{
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	if (!ES)
	{
		return;
	}

	const FCrowdyEntityRecord* Record = ES->FindRecord(NetID);
	if (!Record)
	{
		return;
	}

	TryTrackOwned(*Record);

	// Every replicated subsystem is remembered, tracked or not, so a later Leader change can promote or demote it.
	if (IsLeaderWritten(*Record))
	{
		KnownSubsystemIDs.Add(NetID);
	}
}

bool UCrowdyStateReplicator::IsLeaderWritten(const FCrowdyEntityRecord& Record)
{
	return Record.Role == ECrowdyRole::HostOwned && Record.IsReplicatedSubsystem();
}

bool UCrowdyStateReplicator::TryTrackOwned(const FCrowdyEntityRecord& Record)
{
	// This client drives what it owns, and the replicated subsystems while it is the Leader. A World entity has no
	// writer, and anything else (RemoteProxy, None) is not tracked here.
	const bool bOwned = Record.Role == ECrowdyRole::Owner || (IsLeaderWritten(Record) && IsLocalLeader());
	if (!bOwned)
	{
		return false;
	}

	UObject* Participant = Record.GetParticipant();
	if (!Participant)
	{
		return false;
	}

	// A re-register / duplicate broadcast leaves the existing shadow intact rather than re-zeroing it.
	if (OwnedEntities.Contains(Record.NetID))
	{
		return false;
	}

	TUniquePtr<FCrowdyOwnedEntityState> State;
	if (!BuildOwnedState(Record.NetID, Participant, State))
	{
		return false;
	}

	// A non-actor participant (a subsystem) is non-spatial: its deltas ride the reliable channel, and it never
	// emits owner-only targeted deltas.
	State->bNonSpatial = Cast<AActor>(Participant) == nullptr;

	OwnedEntities.Add(Record.NetID, MoveTemp(State));
	return true;
}

FGuid UCrowdyStateReplicator::ResolveLeaderID() const
{
	if (bUseLeaderOverrideForTest)
	{
		return LeaderIDForTest;
	}
	const UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	return ES ? ES->GetAppWriterID() : FGuid();
}

bool UCrowdyStateReplicator::IsLocalLeader() const
{
	const FGuid Leader = ResolveLeaderID();
	if (!Leader.IsValid())
	{
		return false;
	}
	const UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid Local = ES ? ES->GetLocalPlayerID() : LocalPlayerID;
	return Leader == Local;
}

double UCrowdyStateReplicator::GetLoopTimeSeconds() const
{
	if (bUseTimeOverrideForTest)
	{
		return TestTimeSeconds;
	}
	const UWorld* World = GetWorld();
	return World ? static_cast<double>(World->GetTimeSeconds()) : 0.0;
}

void UCrowdyStateReplicator::HandleEntityUnregistered(const FGuid& NetID)
{
	OwnedEntities.Remove(NetID);
	KnownSubsystemIDs.Remove(NetID);
	DestroyLoopbackMirror(NetID);
}

void UCrowdyStateReplicator::HandleLeaderChanged(const FGuid& NewLeaderID, const FGuid& /*PreviousLeaderID*/)
{
	// A client that stops leading stops sending the subsystems; one that starts leading starts.
	const UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid LocalID = ES ? ES->GetLocalPlayerID() : LocalPlayerID;
	if (!NewLeaderID.IsValid() || NewLeaderID != LocalID)
	{
		for (const FGuid& Id : KnownSubsystemIDs)
		{
			OwnedEntities.Remove(Id);
		}
		return;
	}

	if (!ES)
	{
		return;
	}
	for (const FGuid& Id : KnownSubsystemIDs)
	{
		const FCrowdyEntityRecord* Record = ES->FindRecord(Id);
		if (Record && !OwnedEntities.Contains(Id) && TryTrackOwned(*Record))
		{
			OwnedEntities[Id]->bQuietResend = true;
		}
	}
}

void UCrowdyStateReplicator::HandleOwnershipChanged(AActor* /*TargetEntity*/, const FGuid& NetID,
	const FGuid& /*NewWriterID*/, const FGuid& /*PreviousWriterID*/)
{
	// Drop any tracking/shadow this client held for the entity under its previous owner; ReassignOwnership has
	// already re-pointed the record, so re-derive everything from it. Remove is a no-op when we did not track it.
	OwnedEntities.Remove(NetID);

	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	if (!ES)
	{
		return;
	}

	const FCrowdyEntityRecord* Record = ES->FindRecord(NetID);
	if (!Record)
	{
		KnownSubsystemIDs.Remove(NetID);
		return;
	}

	if (IsLeaderWritten(*Record))
	{
		KnownSubsystemIDs.Add(NetID);
	}
	else
	{
		KnownSubsystemIDs.Remove(NetID);
	}

	TryTrackOwned(*Record);

	// A new Leader may hold stale values, so it starts from what it has and sends only what it writes from here on.
	const TUniquePtr<FCrowdyOwnedEntityState>* Tracked = Record->bLeaderOwned ? OwnedEntities.Find(NetID) : nullptr;
	if (Tracked && Tracked->IsValid())
	{
		SeedShadowFromLive(**Tracked);
	}
}

void UCrowdyStateReplicator::SeedShadowFromLive(FCrowdyOwnedEntityState& State)
{
	const UObject* Participant = State.Participant.Get();
	if (!Participant || !State.bShadowInitialized)
	{
		return;
	}

	uint8* const Base = State.ShadowData.GetData();
	for (int32 Index = 0; Index < State.ShadowProps.Num(); ++Index)
	{
		if (const FProperty* Property = State.ShadowProps[Index])
		{
			Property->CopyCompleteValue(Base + State.ShadowOffsets[Index], Property->ContainerPtrToValuePtr<void>(Participant));
		}
	}
}

bool UCrowdyStateReplicator::BuildOwnedState(const FGuid& EntityID, UObject* Participant,
	TUniquePtr<FCrowdyOwnedEntityState>& Out)
{
	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry || !Participant)
	{
		return false;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Participant->GetClass());
	if (!Layout || !Layout->IsValid())
	{
		return false;
	}

	Out = MakeUnique<FCrowdyOwnedEntityState>();
	Out->EntityID = EntityID;
	Out->Participant = Participant;
	Out->PendingManualDirty.Init(false, Layout->Properties.Num());

	// Per-entity heartbeat kill switch (B), read once from the authored component config: a null component
	// (e.g. a test fixture or a non-actor subsystem, which carries no entity component) or
	// StateHeartbeat==Inherit leaves it enabled; only ==Off suppresses this entity's keyframe heartbeat.
	// On-change replication is unaffected regardless. Only an actor can carry a UCrowdyEntityComponent.
	if (AActor* Actor = Cast<AActor>(Participant))
	{
		if (const UCrowdyEntityComponent* Comp = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
			Out->bHeartbeatSuppressed = Comp->GetStateHeartbeat() == ECrowdyStateHeartbeat::Off;

			const UWorld* World = Actor->GetWorld();
			const UCrowdyAutoReplicator* Movement = World ? World->GetSubsystem<UCrowdyAutoReplicator>() : nullptr;
			Out->bContinuousOnMovement = Movement && Movement->CarriesUpdatesOf(Comp);
		}
	}

	// Seed the first heartbeat about one interval out, offset by a small deterministic per-entity stagger in
	// [0, KeyframeInterval) so many entities spawned together do not fire their keyframes on the same tick. The
	// spawn InitialState already carried the first baseline, so the first heartbeat is genuinely the next one.
	{
		const double Stagger = static_cast<double>(KeyframeInterval)
			* (static_cast<double>(GetTypeHash(EntityID) & 0xFF) / 256.0);
		Out->NextKeyframeTime = GetLoopTimeSeconds() + static_cast<double>(KeyframeInterval) + Stagger;
	}

	// The layout pointer is intentionally not cached: the registry frees its layouts on a rescan, so the
	// loop re-resolves the live layout each tick and InitShadow snapshots what teardown needs.
	InitShadow(*Out, *Layout);
	return true;
}

void UCrowdyStateReplicator::InitShadow(FCrowdyOwnedEntityState& State, const FCrowdyRepLayout& Layout)
{
	if (State.bShadowInitialized)
	{
		return;
	}

	const int32 Num = Layout.Properties.Num();
	State.ShadowLayoutHash = Layout.LayoutHash;
	State.ClassID = State.Participant.IsValid()
		? UCrowdyClassRegistry::Get()->GetID(State.Participant->GetClass())
		: CROWDY_INVALID_CLASS_ID;
	State.ShadowOffsets.SetNumUninitialized(Num);
	State.ShadowProps.SetNumUninitialized(Num);
	State.ShadowCompareBytes.SetNumUninitialized(Num);

	// Keep the manual-dirty push flags aligned to the layout. On a fresh build BuildOwnedState already sized
	// this; on a rebuild (layout drift) re-Init clears it losing any pending marks on a layout change is
	// acceptable and rare, and it prevents a stale bit from aliasing a different property's new position.
	State.PendingManualDirty.Init(false, Num);
	State.WarnedTooLarge.Empty();

	// One aligned slot per property, packed in layout order. Each slot is padded up to the property's own
	// required alignment before it is placed, so every Base+Offset satisfies the alignment its load/store
	// and Identical/CopyCompleteValue paths assume. A null property still consumes a positional slot (size
	// 0) so the diff/encode/decode positional ordering is never shifted.
	int32 Total = 0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const FProperty* Property = Layout.Properties[Index].Property;
		State.ShadowProps[Index] = Property;
		State.ShadowCompareBytes[Index] = CrowdyBitwiseCompare::ComparableBytes(Property);
		const int32 Alignment = Property ? FMath::Max(1, Property->GetMinAlignment()) : 1;
		const int32 Size = Property ? Property->GetSize() : 0;
		Total = Align(Total, Alignment);
		State.ShadowOffsets[Index] = Total;
		Total += Size;
	}

	// Zero-fill before InitializeValue so inter-slot padding is deterministic and an all-null layout yields
	// a well-defined empty buffer.
	State.ShadowData.SetNumZeroed(Total);

	uint8* const Base = State.ShadowData.GetData();
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (const FProperty* Property = Layout.Properties[Index].Property)
		{
			Property->InitializeValue(Base + State.ShadowOffsets[Index]);
		}
	}

	State.bShadowInitialized = true;

	UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
		TEXT("[CrowdyStateReplicator] shadow built for %s: %d slots, %d bytes."),
		*State.EntityID.ToString(), Num, Total);
}

void UCrowdyStateReplicator::SetContinuousOnMovement(const FGuid& EntityID, const bool bOnMovement)
{
	if (const TUniquePtr<FCrowdyOwnedEntityState>* State = OwnedEntities.Find(EntityID))
	{
		(*State)->bContinuousOnMovement = bOnMovement;
	}
}

void UCrowdyStateReplicator::SetLocalPlayerIdle(const bool bIdle)
{
	bLocalPlayerIdle = bIdle;
}

bool UCrowdyStateReplicator::RegisterOwnedEntityForTest(const FGuid& EntityID, UObject* Participant)
{
	TUniquePtr<FCrowdyOwnedEntityState> State;
	if (!BuildOwnedState(EntityID, Participant, State))
	{
		return false;
	}
	// Classify spatial vs non-spatial exactly as TryTrackOwned does, so a test that registers a subsystem
	// participant directly still routes it over the channel.
	if (State.IsValid() && Cast<AActor>(Participant) == nullptr)
	{
		State->bNonSpatial = true;
	}
	OwnedEntities.Add(EntityID, MoveTemp(State));
	return true;
}

void UCrowdyStateReplicator::MarkStateDirty(const FGuid& EntityID, FName PropertyName)
{
	TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	if (!Found || !Found->IsValid())
	{
		// Only the writer sends an entity's state, and every other client would drop the change anyway.
		WarnIfWorldEntityHasNoWriter(EntityID);
		UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
			TEXT("[CrowdyStateReplicator] MarkStateDirty on '%s' (entity %s) ignored: this client does not write it."),
			*PropertyName.ToString(), *EntityID.ToString());
		return;
	}
	FCrowdyOwnedEntityState& State = **Found;

	UObject* Participant = State.Participant.Get();
	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Participant || !Registry)
	{
		return;
	}

	// Re-resolve the live layout (never cache it the registry frees layouts on a rescan).
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Participant->GetClass());
	if (!Layout || !Layout->IsValid())
	{
		return;
	}

	for (int32 Index = 0; Index < Layout->Properties.Num(); ++Index)
	{
		const FCrowdyRepProperty& RepProp = Layout->Properties[Index];
		if (!RepProp.Property || RepProp.Property->GetFName() != PropertyName)
		{
			continue;
		}

		// Only manual-dirty properties are pushed this way; a non-manual property is covered by auto-diff, so
		// marking it would be a no-op the send loop ignores. Keep the semantics tight and skip it (verbose log).
		if (!RepProp.bManualDirty)
		{
			UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
				TEXT("[CrowdyStateReplicator] MarkStateDirty on '%s' (entity %s) ignored: not a manual-dirty property."),
				*PropertyName.ToString(), *EntityID.ToString());
			return;
		}

		if (State.PendingManualDirty.IsValidIndex(Index))
		{
			State.PendingManualDirty[Index] = true;
		}
		return;
	}

	WarnUnknownStateProperty(WarnedUnknownStateProperties, Participant->GetClass(), PropertyName, EntityID);
}

void UCrowdyStateReplicator::WarnIfWorldEntityHasNoWriter(const FGuid& EntityID)
{
	if (bWarnedWorldEntityHasNoWriter)
		return;

	const FCrowdyEntityRecord* Record = EntitySubsystem.IsValid() ? EntitySubsystem->FindRecord(EntityID) : nullptr;
	if (!Record || Record->Role != ECrowdyRole::HostOwned || Record->IsReplicatedSubsystem())
		return;

	bWarnedWorldEntityHasNoWriter = true;
	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[CrowdyStateReplicator]: State was marked dirty on World entity %s, which no client writes, so every other client ignores the change. ")
		TEXT("Keep a World entity's truth in a Server Object."),
		*EntityID.ToString());
}

void UCrowdyStateReplicator::ForceFullState(const FGuid& EntityID)
{
	const TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	if (!Found || !Found->IsValid())
	{
		return;
	}
	(*Found)->bSendAllOnce = true;
	(*Found)->bQuietResend = true;
}

void UCrowdyStateReplicator::SeedFromLive(const FGuid& EntityID)
{
	const TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	if (Found && Found->IsValid())
	{
		SeedShadowFromLive(**Found);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
bool UCrowdyStateReplicator::IsFullStateForcedForTest(const FGuid& EntityID) const
{
	const TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	return Found && Found->IsValid() && (*Found)->bSendAllOnce;
}
#endif

void UCrowdyStateReplicator::MarkAllStateDirty(const FGuid& EntityID)
{
	TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	if (!Found || !Found->IsValid())
	{
		UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
			TEXT("[CrowdyStateReplicator] MarkAllStateDirty ignored: entity %s is not tracked."),
			*EntityID.ToString());
		return;
	}
	FCrowdyOwnedEntityState& State = **Found;

	UObject* Participant = State.Participant.Get();
	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Participant || !Registry)
	{
		return;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Participant->GetClass());
	if (!Layout || !Layout->IsValid())
	{
		return;
	}

	for (int32 Index = 0; Index < Layout->Properties.Num(); ++Index)
	{
		if (Layout->Properties[Index].bManualDirty && State.PendingManualDirty.IsValidIndex(Index))
		{
			State.PendingManualDirty[Index] = true;
		}
	}
}

bool UCrowdyStateReplicator::IsStateReplicated(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry)
	{
		return false;
	}
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Actor->GetClass());
	return Layout && Layout->IsValid();
}

int32 UCrowdyStateReplicator::GetLastSentStateBytes(const AActor* Actor) const
{
	// -1 means "not driven by this client": only an owned entry has a last-sent byte count to report. Guard the
	// null query first so a stale (GC'd) weak actor whose Get() is also null cannot false-match it.
	if (!Actor)
	{
		return -1;
	}
	for (const TPair<FGuid, TUniquePtr<FCrowdyOwnedEntityState>>& Pair : OwnedEntities)
	{
		const FCrowdyOwnedEntityState* State = Pair.Value.Get();
		if (State && State->GetActor() == Actor)
		{
			return State->LastSentBytes;
		}
	}
	return -1;
}

bool UCrowdyStateReplicator::IsLoopbackEnabled()
{
	return CVarCrowdyStateLoopback.GetValueOnGameThread() != 0;
}

AActor* UCrowdyStateReplicator::GetOrCreateLoopbackMirror(AActor* SourceActor, const FGuid& SourceEntityID)
{
	if (!IsValid(SourceActor))
	{
		return nullptr;
	}

	if (TWeakObjectPtr<AActor>* Found = LoopbackMirrors.Find(SourceEntityID))
	{
		if (AActor* Existing = Found->Get())
		{
			return Existing;
		}
		// Stale (the mirror was destroyed out from under us); fall through and respawn.
		LoopbackMirrors.Remove(SourceEntityID);
	}

	UWorld* World = SourceActor->GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FTransform MirrorTransform = SourceActor->GetActorTransform();
	MirrorTransform.AddToTranslation(FVector(CrowdyStateLoopbackMirrorOffsetX, 0.f, 0.f));

	// A bespoke, purely local spawn: SpawnActorDeferred lets us inject an identity before BeginPlay, exactly
	// like UCrowdyEntitySubsystem::SpawnEntity, but deliberately NOT that helper it hardcodes Role::Owner and
	// WriterID=LocalPlayerID (wrong for a mirror: see GetOrCreateLoopbackMirror's header comment) and also
	// broadcasts a spawn announcement over the real transport, which a purely-local debug mirror must not do.
	AActor* Mirror = World->SpawnActorDeferred<AActor>(SourceActor->GetClass(), MirrorTransform,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Mirror))
	{
		return nullptr;
	}

	UCrowdyEntityComponent* Component = Mirror->FindComponentByClass<UCrowdyEntityComponent>();
	if (!Component)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyStateReplicator] loopback mirror for '%s' (entity %s) has no UCrowdyEntityComponent; ")
			TEXT("destroying it and disabling loopback for this entity."),
			*GetNameSafe(SourceActor->GetClass()), *SourceEntityID.ToString());
		Mirror->Destroy();
		return nullptr;
	}

	const FGuid MirrorNetID = FGuid::NewGuid();
	const uint32 ClassID = UCrowdyClassRegistry::Get()->GetID(SourceActor->GetClass());

	// WriterID is left INVALID, never LocalPlayerID. IsLocallyOwned() (UCrowdyEntitySubsystem) checks WriterID,
	// not Role, so an owner-stamped mirror would still trip DispatchStateDelta's owned-entity gate; and
	// TryTrackOwned tracks any Owner record, which would make this replicator try to mirror the mirror.
	Component->InitIdentity(MirrorNetID, /*InWriterID=*/FGuid(), /*InOwnerUserID=*/0, ECrowdyRole::RemoteProxy, ClassID);

	// Trusted the same way UCrowdyEntitySubsystem::SpawnEntity trusts its own FinishSpawning: BeginPlay's tail
	// registers the mirror into whatever UCrowdyEntitySubsystem the world resolves, with no post-hoc check
	// here. This always succeeds in a real PIE/Game session (the whole CrowdyState plane requires that
	// subsystem to exist, and SourceActor's class is, by definition, already registered through one for it
	// to be a tracked owned entity at all); the sole realistic failure this can't protect against is no
	// UCrowdyEntitySubsystem in this world, which the receive path's own deferred-retry-then-give-up already
	// handles gracefully for a delta whose target never resolves.
	Component->OwnedBy = ECrowdyOwnedBy::Player;
	Mirror->FinishSpawning(MirrorTransform);

	LoopbackMirrors.Add(SourceEntityID, Mirror);

	UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
		TEXT("[CrowdyStateReplicator] loopback mirror spawned for entity %s: mirror NetID=%s"),
		*SourceEntityID.ToString(), *MirrorNetID.ToString());

	return Mirror;
}

void UCrowdyStateReplicator::DestroyLoopbackMirror(const FGuid& SourceEntityID)
{
	TWeakObjectPtr<AActor> Mirror;
	if (LoopbackMirrors.RemoveAndCopyValue(SourceEntityID, Mirror))
	{
		if (AActor* Actor = Mirror.Get())
		{
			Actor->Destroy();
		}
	}
}

void UCrowdyStateReplicator::FlushLocalNotifies(const TArray<FCrowdyPendingLocalNotify>& Notifies)
{
	if (Notifies.Num() == 0)
	{
		return;
	}

	// ProcessEvent is game-thread only. Everything that fills this list runs from the replication tick, which is
	// already the game thread, so this asserts the contract rather than marshalling for it.
	ensure(IsInGameThread());

	// The list is a local of the send loop and is not reachable from a notify body, so it cannot grow or shrink
	// while it is walked even though a body is free to mark state dirty, destroy its actor, or register and
	// unregister entities.
	for (const FCrowdyPendingLocalNotify& Notify : Notifies)
	{
		// Re-resolved every iteration: an earlier notify in this same flush may have destroyed this container.
		UObject* Container = Notify.Container.Get();
		if (!IsValid(Container))
		{
			continue;
		}

		if (UFunction* Function = Container->FindFunction(Notify.FunctionName))
		{
			// Notify bindings are validated at discovery, so this only ever calls parameterless functions; the
			// check is defense in depth for a baked layout whose function drifted.
			if (Function->NumParms == 0)
			{
				Container->ProcessEvent(Function, nullptr);
			}
		}
	}
}

void UCrowdyStateReplicator::ResyncShadowsAfterLocalNotifies(const TArray<FCrowdyPendingLocalNotify>& Notifies)
{
	if (Notifies.Num() == 0)
	{
		return;
	}

	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry)
	{
		return;
	}

	for (const FCrowdyPendingLocalNotify& Notify : Notifies)
	{
		if (Notify.PropertyIndex == INDEX_NONE)
		{
			continue;
		}

		// An entity a notify body unregistered simply has nothing to fold.
		TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(Notify.EntityID);
		if (!Found || !Found->IsValid())
		{
			continue;
		}
		FCrowdyOwnedEntityState& State = **Found;

		// The entity must still be the same object the notify ran on: a body is free to unregister the entity and
		// re-register the id against something else, and folding one object's value into another's shadow would
		// silence a change that was never sent.
		UObject* Participant = State.Participant.Get();
		if (!Participant || Participant != Notify.Container.Get() || !State.bShadowInitialized)
		{
			continue;
		}

		// Re-resolve the live layout and re-check the hash: the shadow's positional slots only line up while it
		// matches, and a notify body is free to do anything, a registry rescan included.
		const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Participant->GetClass());
		if (!Layout || !Layout->IsValid() || Layout->LayoutHash != State.ShadowLayoutHash
			|| !Layout->Properties.IsValidIndex(Notify.PropertyIndex)
			|| !State.ShadowOffsets.IsValidIndex(Notify.PropertyIndex))
		{
			continue;
		}

		const FProperty* Property = Layout->Properties[Notify.PropertyIndex].Property;
		uint8* const ShadowBase = State.ShadowData.GetData();
		if (!Property || !ShadowBase)
		{
			continue;
		}

		Property->CopyCompleteValue(ShadowBase + State.ShadowOffsets[Notify.PropertyIndex],
			Property->ContainerPtrToValuePtr<void>(Participant));
	}
}

void UCrowdyStateReplicator::ReplicationLoop()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_StateReplicationLoop);

	if (OwnedEntities.Num() == 0)
	{
		return;
	}

	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry)
	{
		return;
	}

	// Local notifies for the slots that changed on THIS client this tick, fired once the walk below is finished.
	// Deferred because a notify body may register or unregister an entity, which would rehash the map being
	// iterated.
	TArray<FCrowdyPendingLocalNotify> PendingNotifies;

	// Resolve the sender live each loop: the local player's authoritative GUID is assigned asynchronously
	// after sign-in, so a value cached at Initialize would be stale/empty for the whole session. Fall back to
	// the injected id on the test-hook path where there is no entity subsystem.
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid SenderID = ES ? ES->GetLocalPlayerID() : LocalPlayerID;

	// The Leader stamps its replicated subsystems' deltas HostSourced, which is how a receiver knows the subsystem's
	// writer sent them. Read live, like the sender.
	const FGuid LeaderID = ResolveLeaderID();
	const bool bLocalIsLeader = LeaderID.IsValid() && SenderID.IsValid() && LeaderID == SenderID;
	const double Now = GetLoopTimeSeconds();

	// Routes a non-spatial (subsystem) participant's delta over the reliable channel: the test hook when bound,
	// else encode + PublishReliableRpc on the default session channel (empty name). A subsystem has no world
	// location, so it never rides the spatial DispatchGameEvent.
	const auto RouteNonSpatial = [this, ES](const FCrowdyStateDelta& D)
	{
		if (ChannelDispatchHookForTests)
		{
			ChannelDispatchHookForTests(D);
		}
		else if (ES)
		{
			TArray<uint8> Payload;
			FCrowdyStateCodec::EncodeChannelStateDelta(D, Payload);
			ES->PublishReliableRpc(TEXT(""), Payload);
		}
	};

	// Dead entries are collected during iteration and removed afterwards so the TMap is never mutated
	// while it is being walked.
	TArray<FGuid, TInlineAllocator<8>> StaleKeys;

	// Debug loopback only (crowdy.state.loopback): deltas already retargeted at a mirror entity, decoded through
	// the real receive path once the walk is over. Replaying one inline would run the mirror's notify bodies
	// mid-walk, and such a body is free to register or unregister an entity, which rehashes the map being
	// iterated. Always empty while the CVar is off.
	TArray<FCrowdyStateDelta> LoopbackDeltas;

	for (const TPair<FGuid, TUniquePtr<FCrowdyOwnedEntityState>>& Pair : OwnedEntities)
	{
		FCrowdyOwnedEntityState* StatePtr = Pair.Value.Get();
		if (!StatePtr)
		{
			StaleKeys.Add(Pair.Key);
			continue;
		}
		FCrowdyOwnedEntityState& State = *StatePtr;

		// Participant is the diff container (any UObject); Actor is non-null only for a spatial participant and
		// is used solely for the spatial dispatch context/location.
		UObject* Participant = State.Participant.Get();
		if (!Participant)
		{
			StaleKeys.Add(Pair.Key);
			continue;
		}
		AActor* Actor = State.GetActor();

		// Re-resolve the live layout every tick: the registry frees its FCrowdyRepLayout objects on a rescan,
		// so a cached pointer would dangle. This is a cache hit (a TMap::Find) once the class is discovered.
		const FCrowdyRepLayout* LayoutPtr = Registry->FindRepLayout(Participant->GetClass());
		if (!LayoutPtr || !LayoutPtr->IsValid())
		{
			// The class lost its rep layout (a mid-session unannotation, or a rescan in flight): skip rather
			// than diff against a stale shadow. Not fatal, so a Warning, never an Error.
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[CrowdyStateReplicator] owned entity %s has no rep layout this tick; skipping."),
				*State.EntityID.ToString());
			continue;
		}

		// If the property set drifted under us (hash mismatch) or the shadow was abandoned, its positional
		// slots no longer line up with this layout; rebuild against the fresh layout and skip this tick so the
		// next one diffs cleanly. In steady state this branch is never taken (same class -> same hash).
		if (!State.bShadowInitialized || State.ShadowLayoutHash != LayoutPtr->LayoutHash)
		{
			DestroyShadowSlots(State);
			State.bShadowInitialized = false;
			InitShadow(State, *LayoutPtr);
			continue;
		}

		const FCrowdyRepLayout& Layout = *LayoutPtr;
		const int32 Num = Layout.Properties.Num();
		uint8* const ShadowBase = State.ShadowData.GetData();
		const bool bQuietResend = State.bQuietResend;
		State.bQuietResend = false;
		const bool bSendAll = State.bSendAllOnce;
		State.bSendAllOnce = false;

		// Classify each property into two dirty sets over the FULL layout: SpatialDirty broadcasts, OwnerOnlyDirty
		// ships as a targeted (single-actor) delta. Auto-diff properties diff against the shadow; manual-dirty
		// properties consult PendingManualDirty; owner-only routing is by the property's own bOwnerOnly flag.
		TBitArray<> SpatialDirty;
		SpatialDirty.Init(false, Num);
		TBitArray<> OwnerOnlyDirty;
		OwnerOnlyDirty.Init(false, Num);

		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FCrowdyRepProperty& RepProp = Layout.Properties[Index];
			if (!RepProp.Property)
			{
				continue;
			}

			// Movement updates carry the value, which leaves only the owner's own notify to run here.
			if (RepProp.bContinuous && State.bContinuousOnMovement)
			{
				const void* Live =RepProp.Property->ContainerPtrToValuePtr<void>(Participant);
				void* Shadow = ShadowBase + State.ShadowOffsets[Index];
				if (RepProp.Property->Identical(Live, Shadow, PPF_None))
				{
					continue;
				}
				if (RepProp.OnRepFunctionName != NAME_None)
				{
					PendingNotifies.Add(FCrowdyPendingLocalNotify{ Participant, RepProp.OnRepFunctionName, State.EntityID, Index });
				}
				RepProp.Property->CopyCompleteValue(Shadow, Live);
				continue;
			}

			bool bDirty = bSendAll;
			if (!bDirty && RepProp.bManualDirty)
			{
				bDirty = State.PendingManualDirty.IsValidIndex(Index) && State.PendingManualDirty[Index];
			}
			else if (!bDirty)
			{
				const void* Live = RepProp.Property->ContainerPtrToValuePtr<void>(Participant);
				const void* Shadow = ShadowBase + State.ShadowOffsets[Index];
				const int32 CompareBytes = State.ShadowCompareBytes[Index];
				bDirty = CompareBytes > 0
					? FMemory::Memcmp(Live, Shadow, CompareBytes) != 0
					: !RepProp.Property->Identical(Live, Shadow, PPF_None);
			}

			if (!bDirty)
			{
				continue;
			}

			if (RepProp.bOwnerOnly)
			{
				OwnerOnlyDirty[Index] = true;
			}
			else
			{
				SpatialDirty[Index] = true;
			}
		}

		// Keyframe heartbeat: on its interval, force every CrowdyHeartbeat-marked (opt-in), non-owner-only property
		// into the spatial set so a late/desynced observer gets a full baseline. Owner-only properties are NEVER
		// part of a keyframe (they would leak to all observers on the broadcast); an unmarked property replicates
		// on change but is never re-sent while unchanged. The whole heartbeat is gated off by the map interval
		// being 0 (A) or this entity's StateHeartbeat==Off (B). Advance to Now + interval, not NextKeyframeTime +
		// interval, so a starved loop does not fire a catch-up burst. bEmitKeyframe is set ONLY once a marked
		// property is actually added, so an entity whose timer is due but has no marked properties neither emits an
		// empty keyframe nor mislabels a same-tick on-change delta as a keyframe (the timer still advances, so the
		// empty check runs once per interval rather than every tick).
		bool bEmitKeyframe = false;
		const bool bKeyframeDue = KeyframeInterval > 0.0f && !State.bHeartbeatSuppressed && Now >= State.NextKeyframeTime;
		if (bKeyframeDue && bLocalPlayerIdle)
		{
			// Skipped in whole intervals, so the entity keeps its place in the stagger.
			State.NextKeyframeTime += KeyframeInterval * FMath::FloorToDouble((Now - State.NextKeyframeTime) / KeyframeInterval + 1.0);
		}
		if (bKeyframeDue && !bLocalPlayerIdle)
		{
			State.NextKeyframeTime = Now + static_cast<double>(KeyframeInterval);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const FCrowdyRepProperty& RepProp = Layout.Properties[Index];
				// With no movement update to ride, a continuous value is on the keyframe whatever its heartbeat mark says.
				const bool bOnKeyframe = RepProp.bContinuous ? !State.bContinuousOnMovement : RepProp.bHeartbeat;
				if (RepProp.Property && !RepProp.bOwnerOnly && bOnKeyframe)
				{
					SpatialDirty[Index] = true;
					bEmitKeyframe = true;
				}
			}
		}

		const bool bAnySpatial = SpatialDirty.Contains(true);
		const bool bAnyOwnerOnly = OwnerOnlyDirty.Contains(true);
		if (!bAnySpatial && !bAnyOwnerOnly)
		{
			// Nothing changed and no heartbeat due emit nothing. ShadowUpdatesPostSend relies on this silence.
			continue;
		}

		// Sent = union of every index actually encoded across both deltas; the shadow (and pending-clear) update
		// below walks exactly this set so an untouched slot keeps its last-sent value.
		TBitArray<> Sent;
		Sent.Init(false, Num);

		// Sum of the spatial + owner-only blob bytes emitted THIS tick, for the read-only GetLastSentStateBytes
		// diagnostic. Captured from Delta.Blob.Num() after each MoveTemp (the local Blob is empty by then). Only
		// written to State.LastSentBytes below, and only on a tick that actually emits, so the field otherwise
		// keeps reflecting the last real send.
		int32 SentBytes = 0;

		// Positive answers only, so a class whose registration was refused over an id clash is re-asked
		// rather than emitting CROWDY_INVALID_CLASS_ID for the rest of its life once the clash is cleared.
		// That refusal is the only answer the sealed registry can still change, and re-asking costs a
		// pointer-keyed lookup. Same rule FindRepLayout follows for a class with no layout.
		if (State.ClassID == CROWDY_INVALID_CLASS_ID)
		{
			State.ClassID = UCrowdyClassRegistry::Get()->GetID(Participant->GetClass());
		}

		const int64 ClassID = static_cast<int64>(State.ClassID);
		const bool bHostSourced = State.bNonSpatial && bLocalIsLeader;

		if (bAnySpatial)
		{
			const int32 Limit = State.bNonSpatial ? CrowdyStateBlobLimit::Channel : CrowdyStateBlobLimit::Event;
			SendWithinLimit(State, Layout, Participant, SpatialDirty, Limit, Sent, [&](TArray<uint8>&& Blob)
			{
				FCrowdyStateDelta Delta;
				Delta.ClassID = ClassID;
				Delta.EntityID = State.EntityID;
				Delta.SenderID = SenderID;
				Delta.LayoutHash = Layout.LayoutHash;
				Delta.Flags = static_cast<uint8>((bHostSourced ? CrowdyStateDeltaFlags::HostSourced : 0)
					| (bEmitKeyframe ? CrowdyStateDeltaFlags::Keyframe : 0));
				Delta.Blob = MoveTemp(Blob);
				SentBytes += Delta.Blob.Num();

				UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
					TEXT("[CrowdyStateReplicator] emit %s%s entity=%s bytes=%d"),
					State.bNonSpatial ? TEXT("channel ") : TEXT(""),
					bEmitKeyframe ? TEXT("keyframe") : TEXT("delta"), *State.EntityID.ToString(), Delta.Blob.Num());

				// A non-spatial (subsystem) participant rides the reliable channel; a spatial actor broadcasts over
				// the spatial transport. The two are branched FIRST so a bound spatial hook never captures a channel
				// delta and vice versa (this is what lets a test distinguish the two dispatch kinds).
				if (State.bNonSpatial)
				{
					RouteNonSpatial(Delta);
				}
				else if (DispatchHookForTests)
				{
					DispatchHookForTests(Delta, /*bTargeted=*/false);
				}
				else if (ES && Actor)
				{
					// Sent from a view of Delta rather than a copy of it into an FInstancedStruct: the send is
					// synchronous, so Delta outlives it, and the blob is not duplicated to put it on the wire.
					ES->DispatchGameEventView(Actor, FCrowdyStateDelta::StaticStruct(), &Delta,
						ECrowdyTarget::Everyone, Actor, ECrowdyDecayRate::No_Decay, RelevanceDistance);

					// Debug loopback: also decode this delta onto a local mirror entity, so a single PIE client
					// can watch decode/OnRep fire without a second client. Off (default) costs one CVar read;
					// on, it queues the exact same Delta retargeted at the mirror's own NetID (never this entity's
					// real one, which the ownership gate would drop) for replay through the real receive path
					// after the walk.
					if (IsLoopbackEnabled())
					{
						if (AActor* Mirror = GetOrCreateLoopbackMirror(Actor, State.EntityID))
						{
							if (const UCrowdyEntityComponent* MirrorComp = Mirror->FindComponentByClass<UCrowdyEntityComponent>())
							{
								FCrowdyStateDelta MirrorDelta = Delta;
								MirrorDelta.EntityID = MirrorComp->GetNetID();
								MirrorDelta.SenderID = FGuid();
								LoopbackDeltas.Add(MoveTemp(MirrorDelta));
							}
						}
					}
				}
			});
		}

		if (bAnyOwnerOnly)
		{
			if (State.bNonSpatial)
			{
				// A non-spatial singleton has no per-owner target, so an owner-only property cannot be delivered
				// targeted; it is intentionally never emitted for a subsystem. Mark the bits Sent anyway so the
				// shadow advances and they do not re-diff (and re-log) every tick.
				UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
					TEXT("[CrowdyStateReplicator] entity %s is non-spatial; owner-only propert(ies) not emitted (no per-owner target)."),
					*State.EntityID.ToString());

				for (int32 Index = 0; Index < Num; ++Index)
				{
					if (OwnerOnlyDirty[Index])
					{
						Sent[Index] = true;
					}
				}
			}
			else
			{
				SendWithinLimit(State, Layout, Participant, OwnerOnlyDirty, CrowdyStateBlobLimit::Event, Sent,
					[&](TArray<uint8>&& Blob)
				{
					// Same ClassID/EntityID/SenderID/LayoutHash as the spatial delta (the selector is positional over the
					// full layout, so both share the hash); never the Keyframe flag. Delivered targeted, so the transport
					// marks it bTargetedDelivery and the receiver's echo-drop leaves it alone.
					FCrowdyStateDelta Delta;
					Delta.ClassID = ClassID;
					Delta.EntityID = State.EntityID;
					Delta.SenderID = SenderID;
					Delta.LayoutHash = Layout.LayoutHash;
					Delta.Flags = static_cast<uint8>(bHostSourced ? CrowdyStateDeltaFlags::HostSourced : 0);
					Delta.Blob = MoveTemp(Blob);
					SentBytes += Delta.Blob.Num();

					UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
						TEXT("[CrowdyStateReplicator] emit owner-only entity=%s bytes=%d"),
						*State.EntityID.ToString(), Delta.Blob.Num());

					if (DispatchHookForTests)
					{
						DispatchHookForTests(Delta, /*bTargeted=*/true);
					}
					else if (ES && Actor)
					{
						ES->DispatchSingleActorMessage(Actor, FInstancedStruct::Make(Delta));

						// Debug loopback, same as the spatial path above: queue this owner-only delta for the local
						// mirror so a single client can prove owner-only delivery + OnRep without a second one.
						if (IsLoopbackEnabled())
						{
							if (AActor* Mirror = GetOrCreateLoopbackMirror(Actor, State.EntityID))
							{
								if (const UCrowdyEntityComponent* MirrorComp = Mirror->FindComponentByClass<UCrowdyEntityComponent>())
								{
									FCrowdyStateDelta MirrorDelta = Delta;
									MirrorDelta.EntityID = MirrorComp->GetNetID();
									MirrorDelta.SenderID = FGuid();
									LoopbackDeltas.Add(MoveTemp(MirrorDelta));
								}
							}
						}
					}
				});
			}
		}

		// Untouched on a tick that sent nothing, so it reflects the last real send.
		if (SentBytes > 0)
		{
			State.LastSentBytes = SentBytes;
		}

		// Advance the shadow to what peers now hold, and clear any manual-dirty marks we just satisfied but
		// ONLY for the bits actually sent (spatial + owner-only + keyframe + manual), so an unsent slot's shadow
		// and pending bit are never disturbed. The shadow still holds the previously-sent value at the top of
		// each iteration, which is what decides whether this property's notify fires locally.
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (!Sent[Index])
			{
				continue;
			}
			const FCrowdyRepProperty& RepProp = Layout.Properties[Index];
			if (RepProp.Property)
			{
				void* const ShadowSlot = ShadowBase + State.ShadowOffsets[Index];
				void* const LiveValue = RepProp.Property->ContainerPtrToValuePtr<void>(Participant);

				// Fire the notify only where the value genuinely MOVED since the last send, which is exactly
				// what a receiver reports as changed. A keyframe re-sending an unchanged property, and a dirty
				// mark on a value that did not move, both still go on the wire but change nothing anywhere, so
				// neither may fire a notify here either.
				if (RepProp.OnRepFunctionName != NAME_None && !bQuietResend
					&& !RepProp.Property->Identical(LiveValue, ShadowSlot, PPF_None))
				{
					PendingNotifies.Add(FCrowdyPendingLocalNotify{ Participant, RepProp.OnRepFunctionName,
						State.EntityID, Index });
				}

				RepProp.Property->CopyCompleteValue(ShadowSlot, LiveValue);
			}
			if (RepProp.bManualDirty && State.PendingManualDirty.IsValidIndex(Index))
			{
				State.PendingManualDirty[Index] = false;
			}
			if (State.WarnedTooLarge.IsValidIndex(Index))
			{
				State.WarnedTooLarge[Index] = false;
			}
		}
	}

	for (const FGuid& Key : StaleKeys)
	{
		OwnedEntities.Remove(Key);
	}

	FlushLocalNotifies(PendingNotifies);
	ResyncShadowsAfterLocalNotifies(PendingNotifies);

	// Debug loopback only: replay what was just sent onto the mirror entities, through the real receive path, now
	// that the owned-entity walk is over and a mirror's notify body is free to register or unregister entities.
	if (LoopbackDeltas.Num() > 0)
	{
		if (UWorld* World = GetWorld())
		{
			if (UCrowdyEventRouter* Router = World->GetSubsystem<UCrowdyEventRouter>())
			{
				for (const FCrowdyStateDelta& MirrorDelta : LoopbackDeltas)
				{
					Router->ReceiveLoopbackStateDelta(MirrorDelta);
				}
			}
		}
	}
}

#if WITH_EDITOR
void UCrowdyStateReplicator::HandleReloadComplete()
{
	// A Live Coding reload just rebuilt the registry's layouts and may have reinstanced replicated actor
	// classes. An entity whose actor was reinstanced (weak ptr now invalid) holds a shadow whose snapshotted
	// FProperty* are stale, so abandon that shadow skip DestroyValue, accepting a bounded editor-only leak
	// rather than let the destructor dereference a dangling property. Survivors keep their still-valid shadows
	// and re-resolve the rebuilt layout on the next tick.
	for (auto It = OwnedEntities.CreateIterator(); It; ++It)
	{
		const TUniquePtr<FCrowdyOwnedEntityState>& State = It->Value;
		if (State.IsValid() && State->Participant.IsValid())
		{
			continue;
		}

		if (State.IsValid())
		{
			State->bShadowInitialized = false;
		}
		It.RemoveCurrent();
	}
}
#endif
