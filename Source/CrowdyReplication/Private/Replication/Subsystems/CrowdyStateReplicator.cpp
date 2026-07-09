#include "Replication/Subsystems/CrowdyStateReplicator.h"

#include "CrowdyReplicationLog.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h" // CrowdyChannelPayloadMaxBytes (channel payload cap parity)
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
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
	// A single CrowdyState delta above this many blob bytes will fragment across datagrams on the transport.
	// Advisory only in Phase 3: over-budget deltas still send correctly; the trace line flags a fat layout.
	constexpr int32 CrowdyStateSingleDatagramBudget = 1180;

	// A behavior-gating CVar (not a trace flag), so it lives here beside UCrowdyStateReplicator rather than in
	// CrowdyReplication.cpp's crowdy.*.trace family exactly where crowdy.rpc.loopback lives relative to
	// FCrowdyRPC. Off (0) by default: byte-identical to today, matching that precedent's own invariant.
	TAutoConsoleVariable<int32> CVarCrowdyStateLoopback(
		TEXT("crowdy.state.loopback"), 0,
		TEXT("When non-zero, a sent CrowdyState delta for an owned entity is also decoded onto a local ")
		TEXT("'mirror' entity (a lazily-spawned second instance of the same class, RemoteProxy role, a ")
		TEXT("distinct NetID) so decode/OnRep/host-authority checks can be exercised from one PIE client. ")
		TEXT("Unlike crowdy.rpc.loopback this cannot replay onto the same actor (an owner's own entity ")
		TEXT("silently drops a non-host-sourced delta, and decode's changed-detection would report no ")
		TEXT("change against identical values); the mirror is a genuinely distinct receive target instead."),
		ECVF_Default);

	// Purely cosmetic: offsets the mirror actor from its source so both are visible side by side in PIE.
	constexpr float CrowdyStateLoopbackMirrorOffsetX = 200.f;

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

		// The game session is game-instance-scoped, so it survives level travel; bind its host-change delegate to
		// promote/demote host-owned tracking mid-session. On a fresh world where the host is already elected,
		// OnHostIDUpdated does not fire (equal-value writes are suppressed) TryTrackOwned reads GetHostID() live
		// at re-registration on the new world, so that init-seeding path needs no explicit event.
		if (UCrowdyGameSession* GS = GameInstance->GetSubsystem<UCrowdyGameSession>())
		{
			GameSession = GS;
			GS->OnHostIDUpdated.AddDynamic(this, &UCrowdyStateReplicator::HandleHostChanged);
		}
	}

	// The spawn flow fires OnEntityRegistered, so binding is enough for entities created after us. A
	// replicator created after some entities already registered would miss them; the entity subsystem
	// exposes no cheap owned-set enumeration, so Phase 3 accepts binding-only seeding.

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
		GS->OnHostIDUpdated.RemoveDynamic(this, &UCrowdyStateReplicator::HandleHostChanged);
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

	// PIE or Game only, mirroring UCrowdyAutoReplicator. The bUseStateReplicator gate is resolved in
	// Initialize (the profile is not reliably available in ShouldCreateSubsystem).
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

	if (const FCrowdyEntityRecord* Record = ES->FindRecord(NetID))
	{
		TryTrackOwned(*Record);

		// Remember every host-owned id independent of whether TryTrackOwned tracked it: a non-host does not track
		// it now but must still be able to promote it once we become host (HandleHostChanged reads this set).
		if (Record->Role == ECrowdyRole::HostOwned)
		{
			KnownHostOwnedIDs.Add(NetID);
		}
	}
}

bool UCrowdyStateReplicator::TryTrackOwned(const FCrowdyEntityRecord& Record)
{
	// This client drives an entity it owns; it also drives HostOwned (world/AI) entities, but only while it is
	// the host. A non-host rejects HostOwned records it receives world-entity state as a proxy, never
	// simulates it. Anything else (RemoteProxy, None) is not tracked here.
	bool bTrackAsHostOwned = false;
	if (Record.Role == ECrowdyRole::Owner)
	{
		bTrackAsHostOwned = false;
	}
	else if (Record.Role == ECrowdyRole::HostOwned && IsLocalHost())
	{
		bTrackAsHostOwned = true;
	}
	else
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
	// emits owner-only targeted deltas. Crucially it also has bHostOwned FALSE even for a HostOwned record, so
	// the auto-diff loop RUNS for it a host-owned subsystem replicates on CHANGE (unlike a spatial host-owned
	// world actor, which stays manual-only). An actor participant keeps today's bHostOwned semantics exactly.
	if (Cast<AActor>(Participant) == nullptr)
	{
		State->bNonSpatial = true;
		State->bHostOwned = false;
	}
	else
	{
		State->bHostOwned = bTrackAsHostOwned;
	}

	OwnedEntities.Add(Record.NetID, MoveTemp(State));
	return true;
}

FGuid UCrowdyStateReplicator::ResolveHostID() const
{
	if (bUseHostOverrideForTest)
	{
		return HostIDForTest;
	}
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	return ES ? ES->GetHostID() : FGuid();
}

bool UCrowdyStateReplicator::IsLocalHost() const
{
	const FGuid Host = ResolveHostID();
	if (!Host.IsValid())
	{
		return false;
	}
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid Local = ES ? ES->GetLocalPlayerID() : LocalPlayerID;
	return Host == Local;
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
	KnownHostOwnedIDs.Remove(NetID);
	DestroyLoopbackMirror(NetID);
}

void UCrowdyStateReplicator::HandleHostChanged(const FGuid& NewHostID, const FGuid& PreviousHostID)
{
	// Closes two live bugs in the host-lifecycle: (1) a client PROMOTED to host that never starts driving the
	// world/AI entities it is now responsible for (because OnEntityRegistered fired while it was a non-host and
	// TryTrackOwned rejected the HostOwned records back then), and (2) a client DEMOTED from host that keeps
	// broadcasting world state including keyframe heartbeats for those entities forever.
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid LocalID = ES ? ES->GetLocalPlayerID() : LocalPlayerID;
	const bool bNowHost = NewHostID.IsValid() && NewHostID == LocalID;
	if (bNowHost)
	{
		// Promote: start tracking every known host-owned entity we are not already tracking. TryTrackOwned
		// re-checks IsLocalHost() live (now true) and its OwnedEntities.Contains guard prevents a double-track.
		for (const FGuid& Id : KnownHostOwnedIDs)
		{
			if (OwnedEntities.Contains(Id))
			{
				continue;
			}
			if (ES)
			{
				if (const FCrowdyEntityRecord* R = ES->FindRecord(Id))
				{
					TryTrackOwned(*R);
				}
			}
		}
	}
	else
	{
		// Demote: we are no longer host, so stop broadcasting world state (keyframes included) for entities we no
		// longer author. Only host-owned ids are removed; Owner entities we still own are left untouched.
		for (const FGuid& Id : KnownHostOwnedIDs)
		{
			OwnedEntities.Remove(Id);
		}
	}
}

void UCrowdyStateReplicator::HandleOwnershipChanged(AActor* /*TargetEntity*/, const FGuid& NetID,
	const FGuid& /*NewOwnerID*/, const FGuid& /*PreviousOwnerID*/)
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
		KnownHostOwnedIDs.Remove(NetID);
		return;
	}

	// Maintain the known-host-owned set exactly as HandleEntityRegistered does, so a later host migration still
	// promotes (on becoming host) or demotes (on losing host) this entity correctly after the transfer.
	if (Record->Role == ECrowdyRole::HostOwned)
	{
		KnownHostOwnedIDs.Add(NetID);
	}
	else
	{
		KnownHostOwnedIDs.Remove(NetID);
	}

	// Re-track when we are now the authority: an Owner record always, a HostOwned record only while we are host.
	TryTrackOwned(*Record);
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
	State.ShadowOffsets.SetNumUninitialized(Num);
	State.ShadowProps.SetNumUninitialized(Num);

	// Keep the manual-dirty push flags aligned to the layout. On a fresh build BuildOwnedState already sized
	// this; on a rebuild (layout drift) re-Init clears it losing any pending marks on a layout change is
	// acceptable and rare, and it prevents a stale bit from aliasing a different property's new position.
	State.PendingManualDirty.Init(false, Num);

	// One aligned slot per property, packed in layout order. Each slot is padded up to the property's own
	// required alignment before it is placed, so every Base+Offset satisfies the alignment its load/store
	// and Identical/CopyCompleteValue paths assume. A null property still consumes a positional slot (size
	// 0) so the diff/encode/decode positional ordering is never shifted.
	int32 Total = 0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const FProperty* Property = Layout.Properties[Index].Property;
		State.ShadowProps[Index] = Property;
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
		// Untracked target: the host acts as a super-user over an entity another client owns. Its writes are
		// deliberate one-shot authoritative pushes (never a background diff), which is why there is exactly one
		// explicit push path here and no auto-diff for host authority a second implicit mechanism racing this
		// is what produces "host sets X, client re-sends Y" overwrites. The push encodes the property's CURRENT
		// live value on the next tick and is stamped HostSourced only if we are host; it holds no shadow.
		AActor* Actor = EntitySubsystem.IsValid() ? EntitySubsystem->FindEntity(EntityID) : nullptr;
		UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
		if (!Actor || !Registry)
		{
			UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
				TEXT("[CrowdyStateReplicator] MarkStateDirty on '%s' (entity %s) ignored: not tracked and could not be resolved."),
				*PropertyName.ToString(), *EntityID.ToString());
			return;
		}

		const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Actor->GetClass());
		if (!Layout || !Layout->IsValid())
		{
			return;
		}

		// HostOverride SEND check (PRIMARY enforcement): a host may not override an OwnerOnly entity, so drop the
		// push at the source. A missing entity component is treated as Allow (fall through).
		if (const UCrowdyEntityComponent* Comp = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
			if (Comp->GetHostOverridePolicy() == ECrowdyHostOverride::OwnerOnly)
			{
				UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
					TEXT("[CrowdyStateReplicator] push to %s dropped: entity is OwnerOnly (host may not override it)."),
					*EntityID.ToString());
				return;
			}
		}

		// A host override may target ANY CrowdyState property (no bManualDirty requirement here).
		for (int32 Index = 0; Index < Layout->Properties.Num(); ++Index)
		{
			const FCrowdyRepProperty& RepProp = Layout->Properties[Index];
			if (RepProp.Property && RepProp.Property->GetFName() == PropertyName)
			{
				PendingHostPushes.AddUnique(FCrowdyPendingHostPush{ EntityID, Index });
				return;
			}
		}

		UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
			TEXT("[CrowdyStateReplicator] push on '%s' (entity %s) ignored: no such CrowdyState property."),
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

	UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
		TEXT("[CrowdyStateReplicator] MarkStateDirty on '%s' (entity %s) ignored: no such CrowdyState property."),
		*PropertyName.ToString(), *EntityID.ToString());
}

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

void UCrowdyStateReplicator::AdoptHostValues(const FGuid& EntityID, const FCrowdyRepLayout& Layout,
	const TArray<int32>& ChangedIndices, const void* AppliedContainer)
{
	TUniquePtr<FCrowdyOwnedEntityState>* Found = OwnedEntities.Find(EntityID);
	if (!Found || !Found->IsValid())
	{
		// We do not track it (e.g. it is a proxy on this client) nothing to adopt.
		return;
	}
	FCrowdyOwnedEntityState& State = **Found;

	// The shadow must be aligned to exactly this layout; a drift means the positional slots would not line up,
	// so skip and let the next tick rebuild.
	if (!State.bShadowInitialized || State.ShadowLayoutHash != Layout.LayoutHash)
	{
		return;
	}

	uint8* const ShadowBase = State.ShadowData.GetData();
	if (!ShadowBase || !AppliedContainer)
	{
		return;
	}

	for (int32 Index : ChangedIndices)
	{
		if (!Layout.Properties.IsValidIndex(Index) || !State.ShadowOffsets.IsValidIndex(Index))
		{
			continue;
		}
		const FProperty* Prop = Layout.Properties[Index].Property;
		if (!Prop)
		{
			continue;
		}
		// Copy the host's just-applied value into the owner's shadow so the next diff sees it as already-sent
		// (Identical == true) and does not re-emit a revert. AppliedContainer is only read.
		Prop->CopyCompleteValue(ShadowBase + State.ShadowOffsets[Index],
			Prop->ContainerPtrToValuePtr<void>(const_cast<void*>(AppliedContainer)));
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
	// OwnerID=LocalPlayerID (wrong for a mirror: see GetOrCreateLoopbackMirror's header comment) and also
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

	// OwnerID is left INVALID, never LocalPlayerID. IsLocallyOwned() (UCrowdyEntitySubsystem) checks OwnerID,
	// not Role, so an owner-stamped mirror would still trip DispatchStateDelta's owned-entity gate; and
	// TryTrackOwned tracks any Owner record, which would make this replicator try to mirror the mirror.
	// Role=RemoteProxy (not HostOwned) additionally sidesteps DispatchStateDelta's host-owned-world-entity
	// branch, which has its own HostOwned-specific check.
	Component->InitIdentity(MirrorNetID, /*InOwnerID=*/FGuid(), ECrowdyRole::RemoteProxy, ClassID);

	// Trusted the same way UCrowdyEntitySubsystem::SpawnEntity trusts its own FinishSpawning: BeginPlay's tail
	// registers the mirror into whatever UCrowdyEntitySubsystem the world resolves, with no post-hoc check
	// here. This always succeeds in a real PIE/Game session (the whole CrowdyState plane requires that
	// subsystem to exist, and SourceActor's class is, by definition, already registered through one for it
	// to be a tracked owned entity at all); the sole realistic failure this can't protect against is no
	// UCrowdyEntitySubsystem in this world, which the receive path's own deferred-retry-then-give-up already
	// handles gracefully for a delta whose target never resolves.
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

void UCrowdyStateReplicator::DrainPendingHostPushes()
{
	if (PendingHostPushes.Num() == 0)
	{
		return;
	}

	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry)
	{
		// No registry means no layout to encode against; drop the queue rather than retry a hopeless push forever.
		PendingHostPushes.Reset();
		return;
	}

	// Sender and host status are read live, like the main loop: the local id is assigned asynchronously after
	// sign-in, and only a push emitted while we are host is stamped HostSourced.
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid SenderID = ES ? ES->GetLocalPlayerID() : LocalPlayerID;
	const FGuid HostID = ResolveHostID();
	const bool bLocalIsHost = HostID.IsValid() && SenderID.IsValid() && HostID == SenderID;

	for (const FCrowdyPendingHostPush& Push : PendingHostPushes)
	{
		// The one-shot host push targets a per-owner entity by location, so it is actor-only. A non-actor
		// (subsystem) push id is skipped safely with a verbose log a host-owned subsystem is already tracked
		// and auto-diffs on the host, so it never needs this path (subsystem host-push is out of scope here).
		UObject* Participant = ES ? ES->FindParticipant(Push.EntityID) : nullptr;
		AActor* Actor = Cast<AActor>(Participant);
		if (!Actor)
		{
			if (Participant)
			{
				UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
					TEXT("[CrowdyStateReplicator] host push for non-actor participant %s skipped (subsystem host-push is out of scope)."),
					*Push.EntityID.ToString());
			}
			continue;
		}

		const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Actor->GetClass());
		if (!Layout || !Layout->IsValid() || !Layout->Properties.IsValidIndex(Push.PropertyIndex))
		{
			continue;
		}

		const FCrowdyRepProperty& RepProp = Layout->Properties[Push.PropertyIndex];
		if (!RepProp.Property)
		{
			continue;
		}

		TBitArray<> Dirty;
		Dirty.Init(false, Layout->Properties.Num());
		Dirty[Push.PropertyIndex] = true;

		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(*Layout, Actor, Dirty, /*bKeyframe=*/false, Blob);

		FCrowdyStateDelta Delta;
		Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(Actor->GetClass()));
		Delta.EntityID = Push.EntityID;
		Delta.SenderID = SenderID;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = static_cast<uint8>(bLocalIsHost ? CrowdyStateDeltaFlags::HostSourced : 0);
		Delta.Blob = MoveTemp(Blob);

		UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
			TEXT("[CrowdyStateReplicator] host push entity=%s bytes=%d host=%d"),
			*Push.EntityID.ToString(), Delta.Blob.Num(), bLocalIsHost ? 1 : 0);

		if (DispatchHookForTests)
		{
			DispatchHookForTests(Delta, /*bTargeted=*/false);
		}
		else if (ES)
		{
			ES->DispatchGameEvent(Actor, FInstancedStruct::Make(Delta), ECrowdyTarget::Everyone, Actor,
				ECrowdyDecayRate::No_Decay, RelevanceDistance);
		}
	}

	PendingHostPushes.Reset();
}

void UCrowdyStateReplicator::ReplicationLoop()
{
	// One-shot host super-user pushes drain first: a push can exist for an entity this client does not track, so
	// it must run even when OwnedEntities is empty (the early-out below would otherwise skip it).
	DrainPendingHostPushes();

	if (OwnedEntities.Num() == 0)
	{
		return;
	}

	UCrowdyAutoRegistry* Registry = AutoRegistry.Get();
	if (!Registry)
	{
		return;
	}

	// Resolve the sender live each loop: the local player's authoritative GUID is assigned asynchronously
	// after sign-in, so a value cached at Initialize would be stale/empty for the whole session. Fall back to
	// the injected id on the test-hook path where there is no entity subsystem.
	UCrowdyEntitySubsystem* ES = EntitySubsystem.Get();
	const FGuid SenderID = ES ? ES->GetLocalPlayerID() : LocalPlayerID;

	// Host status is resolved live too (host id and local id both read live). Every delta we emit while we are
	// the host is stamped HostSourced a precedence-by-convention hint; the receiver acts on it only for
	// entities it owns (accept a host correction, drop a foreign non-host delta).
	const FGuid HostID = ResolveHostID();
	const bool bLocalIsHost = HostID.IsValid() && SenderID.IsValid() && HostID == SenderID;
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

			// The channel caps the payload at CrowdyChannelPayloadMaxBytes. Fail loudly rather than let the
			// transport silently truncate it (which the receiver would then drop as a short read), mirroring
			// FCrowdyRPC::RouteOverChannel's guard so the two channel codecs stay at parity.
			if (Payload.Num() > CrowdyChannelPayloadMaxBytes)
			{
				UE_LOG(LogCrowdyReplication, Error,
					TEXT("[CrowdyState] channel delta dropped  encoded payload is %d bytes for entity %s, over the %d-byte channel limit."),
					Payload.Num(), *D.EntityID.ToString(), CrowdyChannelPayloadMaxBytes);
				return;
			}

			ES->PublishReliableRpc(TEXT(""), Payload);
		}
	};

	// Dead entries are collected during iteration and removed afterwards so the TMap is never mutated
	// while it is being walked.
	TArray<FGuid, TInlineAllocator<8>> StaleKeys;

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

		// Classify each property into two dirty sets over the FULL layout: SpatialDirty broadcasts, OwnerOnlyDirty
		// ships as a targeted (single-actor) delta. Auto-diff properties diff against the shadow; manual-dirty
		// properties consult PendingManualDirty; owner-only routing is by the property's own bOwnerOnly flag. A
		// host-owned entity is never auto-diffed (only its explicit marks + heartbeat emit).
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

			bool bDirty = false;
			if (RepProp.bManualDirty)
			{
				bDirty = State.PendingManualDirty.IsValidIndex(Index) && State.PendingManualDirty[Index];
			}
			// Host-owned auto-diff stays OFF: host writes are explicit one-shot pushes (see MarkStateDirty), never a
			// background diff, so a host-owned entity emits only its marked bits plus the keyframe heartbeat.
			else if (!State.bHostOwned)
			{
				const void* Live = RepProp.Property->ContainerPtrToValuePtr<void>(Participant);
				const void* Shadow = ShadowBase + State.ShadowOffsets[Index];
				bDirty = !RepProp.Property->Identical(Live, Shadow, PPF_None);
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
		if (KeyframeInterval > 0.0f && !State.bHeartbeatSuppressed && Now >= State.NextKeyframeTime)
		{
			State.NextKeyframeTime = Now + static_cast<double>(KeyframeInterval);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const FCrowdyRepProperty& RepProp = Layout.Properties[Index];
				if (RepProp.Property && !RepProp.bOwnerOnly && RepProp.bHeartbeat)
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

		const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(Participant->GetClass()));

		if (bAnySpatial)
		{
			TArray<uint8> Blob;
			FCrowdyStateCodec::Encode(Layout, Participant, SpatialDirty, /*bKeyframe=*/false, Blob);

			FCrowdyStateDelta Delta;
			Delta.ClassID = ClassID;
			Delta.EntityID = State.EntityID;
			Delta.SenderID = SenderID;
			Delta.LayoutHash = Layout.LayoutHash;
			Delta.Flags = static_cast<uint8>((bLocalIsHost ? CrowdyStateDeltaFlags::HostSourced : 0)
				| (bEmitKeyframe ? CrowdyStateDeltaFlags::Keyframe : 0));
			Delta.Blob = MoveTemp(Blob);
			SentBytes += Delta.Blob.Num();

			if (Delta.Blob.Num() > CrowdyStateSingleDatagramBudget)
			{
				UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
					TEXT("[CrowdyStateReplicator] delta for %s is %d bytes, over the single-datagram budget; it will fragment."),
					*State.EntityID.ToString(), Delta.Blob.Num());
			}

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
				ES->DispatchGameEvent(Actor, FInstancedStruct::Make(Delta), ECrowdyTarget::Everyone, Actor,
					ECrowdyDecayRate::No_Decay, RelevanceDistance);

				// Debug loopback: also decode this delta onto a local mirror entity, so a single PIE client
				// can watch decode/OnRep fire without a second client. Off (default) costs one CVar read;
				// on, it feeds the exact same Delta through the real receive path retargeted at the
				// mirror's own NetID (never this entity's real one, which the ownership gate would drop).
				if (IsLoopbackEnabled())
				{
					if (AActor* Mirror = GetOrCreateLoopbackMirror(Actor, State.EntityID))
					{
						if (const UCrowdyEntityComponent* MirrorComp = Mirror->FindComponentByClass<UCrowdyEntityComponent>())
						{
							FCrowdyStateDelta MirrorDelta = Delta;
							MirrorDelta.EntityID = MirrorComp->GetNetID();
							MirrorDelta.SenderID = FGuid();
							if (UCrowdyEventRouter* Router = Mirror->GetWorld()->GetSubsystem<UCrowdyEventRouter>())
							{
								Router->ReceiveLoopbackStateDelta(MirrorDelta);
							}
						}
					}
				}
			}

			for (int32 Index = 0; Index < Num; ++Index)
			{
				if (SpatialDirty[Index])
				{
					Sent[Index] = true;
				}
			}
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
				TArray<uint8> OoBlob;
				FCrowdyStateCodec::Encode(Layout, Participant, OwnerOnlyDirty, /*bKeyframe=*/false, OoBlob);

				// Same ClassID/EntityID/SenderID/LayoutHash as the spatial delta (the selector is positional over the
				// full layout, so both share the hash); never the Keyframe flag. Delivered targeted, so the transport
				// marks it bTargetedDelivery and the receiver's echo-drop leaves it alone.
				FCrowdyStateDelta Delta;
				Delta.ClassID = ClassID;
				Delta.EntityID = State.EntityID;
				Delta.SenderID = SenderID;
				Delta.LayoutHash = Layout.LayoutHash;
				Delta.Flags = static_cast<uint8>(bLocalIsHost ? CrowdyStateDeltaFlags::HostSourced : 0);
				Delta.Blob = MoveTemp(OoBlob);
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

					// Debug loopback, same as the spatial path above: also decode this owner-only delta onto the
					// local mirror so a single client can prove owner-only delivery + OnRep without a second one.
					if (IsLoopbackEnabled())
					{
						if (AActor* Mirror = GetOrCreateLoopbackMirror(Actor, State.EntityID))
						{
							if (const UCrowdyEntityComponent* MirrorComp = Mirror->FindComponentByClass<UCrowdyEntityComponent>())
							{
								FCrowdyStateDelta MirrorDelta = Delta;
								MirrorDelta.EntityID = MirrorComp->GetNetID();
								MirrorDelta.SenderID = FGuid();
								if (UCrowdyEventRouter* Router = Mirror->GetWorld()->GetSubsystem<UCrowdyEventRouter>())
								{
									Router->ReceiveLoopbackStateDelta(MirrorDelta);
								}
							}
						}
					}
				}

				for (int32 Index = 0; Index < Num; ++Index)
				{
					if (OwnerOnlyDirty[Index])
					{
						Sent[Index] = true;
					}
				}
			}
		}

		// This tick emitted (the no-change/no-heartbeat case continue'd above), so record the total bytes for the
		// read-only diagnostic. Untouched on a silent tick, so it reflects the last real send.
		State.LastSentBytes = SentBytes;

		// Advance the shadow to what peers now hold, and clear any manual-dirty marks we just satisfied but
		// ONLY for the bits actually sent (spatial + owner-only + keyframe + manual), so an unsent slot's shadow
		// and pending bit are never disturbed.
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (!Sent[Index])
			{
				continue;
			}
			const FCrowdyRepProperty& RepProp = Layout.Properties[Index];
			if (RepProp.Property)
			{
				RepProp.Property->CopyCompleteValue(ShadowBase + State.ShadowOffsets[Index],
					RepProp.Property->ContainerPtrToValuePtr<void>(Participant));
			}
			if (RepProp.bManualDirty && State.PendingManualDirty.IsValidIndex(Index))
			{
				State.PendingManualDirty[Index] = false;
			}
		}
	}

	for (const FGuid& Key : StaleKeys)
	{
		OwnedEntities.Remove(Key);
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
