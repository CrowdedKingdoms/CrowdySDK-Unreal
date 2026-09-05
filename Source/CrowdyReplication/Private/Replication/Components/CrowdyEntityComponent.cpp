#include "Replication/Components/CrowdyEntityComponent.h"
#include "CrowdyReplicationLog.h"

#include "TimerManager.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Replication/GameModel/CrowdyBindingKeyProvider.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h" // complete type for the replicator header's inline test seam
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "WorldPartition/ActorInstanceGuids.h"

#if WITH_DEV_AUTOMATION_TESTS
int32 UCrowdyEntityComponent::OwnershipStepCounter = 0;
#endif

UCrowdyEntityComponent::UCrowdyEntityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UCrowdyEntityComponent::InitIdentity(const FGuid& InNetID, const FGuid& InOwnerID, const ECrowdyRole InRole, const uint32 InClassID)
{
	if (HasBegunPlay())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntityComponent]: InitIdentity called after BeginPlay on '%s' — ignored."),
			*GetNameSafe(GetOwner()));
		return;
	}

	NetID   = InNetID;
	OwnerID = InOwnerID;
	Role    = InRole;
	ClassID = InClassID;
	bIdentityInjected = true;
}

void UCrowdyEntityComponent::AssignPooledIdentity(const FGuid& InNetID, const FGuid& InOwnerID, const ECrowdyRole InRole, const uint32 InClassID)
{
	NetID      = InNetID;
	OwnerID    = InOwnerID;
	Role       = InRole;
	ClassID    = InClassID;
	UUIDString = InNetID.IsValid() ? InNetID.ToString(EGuidFormats::Digits) : FString();
	bIdentityInjected = true;

	// A pooled actor is handed its identity long after BeginPlay, so this is where its ownership becomes
	// announceable; the announcement is queued rather than sent inline so the pool finishes checking the actor
	// out before any listener can act on it.
	ScheduleInitialOwnershipAnnouncement();
}

void UCrowdyEntityComponent::ClearIdentity()
{
	if (IsValid(EntitySubsystem) && NetID.IsValid())
		EntitySubsystem->UnregisterEntity(NetID);

	NetID      = FGuid();
	OwnerID    = FGuid();
	Role       = ECrowdyRole::None;
	ClassID    = CROWDY_INVALID_CLASS_ID;
	UUIDString.Reset();
	bIdentityInjected = false;

	// The actor owns nothing until it is handed a new identity, so drop any queued announcement and re-arm the
	// one-shot: the next assignment announces again.
	bOwnershipAnnounced = false;
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(InitialOwnershipTimer);
}

FGuid UCrowdyEntityComponent::ResolveActorInstanceGuid(const AActor* Actor)
{
	// The engine's per-placement identity: for a level-placed actor this is its ActorGuid, and for an actor
	// inside an instanced/streamed level it composes that with the level-instance guid, so two placements of
	// one sublevel differ and every client agrees. It is cooked into the level and survives World Partition
	// embedding, so no SDK-side minting, salting, or ancestor walk is needed. It is editor-only data: in the
	// editor an actor spawned at runtime is also handed a fresh guid, which is per-run and per-client, while in
	// a packaged build it has none at all. So this answers "what is this placement's id", not "was this actor
	// placed in the level".
	return IsValid(Actor) ? FActorInstanceGuid::GetActorInstanceGuid(*Actor) : FGuid();
}

bool UCrowdyEntityComponent::ShouldUsePlacementGuid(bool bIsLevelPlaced, bool bHasInstanceGuid)
{
	return bIsLevelPlaced && bHasInstanceGuid;
}

FGuid UCrowdyEntityComponent::ComputeStableNetID(bool& bOutUsedPathFallback) const
{
	const AActor* Owner = GetOwner();

	// The placement guid is only a shared id for an actor loaded with its level, which is the same "loaded directly
	// from the map" test the ownership rule uses. Without that check an actor created during play would take this
	// branch in the editor, where the engine hands every spawn a fresh guid, and the other branch in a packaged
	// build, where it has none: two different ids for the same actor depending on how the game was built.
	const bool bLevelPlaced = IsValid(Owner) && Owner->IsNetStartupActor();
	const FGuid InstanceGuid = ResolveActorInstanceGuid(Owner);
	if (ShouldUsePlacementGuid(bLevelPlaced, InstanceGuid.IsValid()))
	{
		bOutUsedPathFallback = false;
		return InstanceGuid;
	}

	// Everything else derives from the actor path. For an actor placed in the level this still agrees across
	// clients. For one created during play it does not, because there is nothing shared to derive it from; the
	// caller warns about that case rather than pretending the id is usable.
	bOutUsedPathFallback = true;
	return FCrowdyModelIdentity::StableNetIDFromActorPath(IsValid(Owner) ? Owner->GetPathName() : GetPathName());
}

void UCrowdyEntityComponent::BeginPlay()
{
	Super::BeginPlay();

	CachedOwner = GetOwner();
	EntitySubsystem = GetWorld()->GetSubsystem<UCrowdyEntitySubsystem>();

	if (!bIdentityInjected)
		ResolveIdentity();

	if (!NetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntityComponent]: Could not resolve a NetID for '%s' — entity not registered."),
			*GetNameSafe(CachedOwner));
		return;
	}

	UUIDString = NetID.ToString(EGuidFormats::Digits);

	if (IsValid(EntitySubsystem))
	{
		if (ClassID == CROWDY_INVALID_CLASS_ID)
			ClassID = UCrowdyClassRegistry::Get()->GetID(CachedOwner->GetClass());

		FCrowdyEntityRecord Record;
		Record.NetID   = NetID;
		Record.OwnerID = OwnerID;
		Record.Role    = Role;
		Record.ClassID = ClassID;
		Record.Participant = CachedOwner;
		EntitySubsystem->RegisterEntity(Record);
	}

	// Identity and role are settled and the entity is registered, so this is where its ownership becomes
	// announceable. The announcement itself is queued for the next tick, not sent here.
	ScheduleInitialOwnershipAnnouncement();

	// Only Dynamic mode runs the continuous snapshot channel, so an executor assigned on a Static entity does
	// nothing at all. Say so, because Static hides the executor field once the mode is switched back.
	if (Mode == ECrowdyEntityMode::Static && IsValid(StateExecutor))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: '%s' has a StateExecutor assigned but its Mode is Static — the executor is unused, because a Static entity is event-only. Set Mode to Dynamic to run the continuous state channel."),
			*GetNameSafe(CachedOwner));
	}

	if (Mode == ECrowdyEntityMode::Dynamic)
	{
		AutoReplicator = GetWorld()->GetSubsystem<UCrowdyAutoReplicator>();

		// No executor is no longer an error. The SDK ships one that snapshots the owner's transform, so a
		// component set to Dynamic replicates on its own and the minimum setup for a replicated actor is
		// this component alone. Assigning an executor stays fully supported and overrides this.
		//
		// Only safe because class identity now travels on every update: while the receiver derived an
		// entity's class from its state struct, one shared default struct would have made every dynamic
		// entity in a project resolve to a single class.
		if (!IsValid(StateExecutor))
		{
			StateExecutor = NewObject<UCrowdyDefaultActorUpdateExecutor>(this);

			UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
				TEXT("[CrowdyEntityComponent]: Dynamic mode on '%s' with no StateExecutor assigned, using the SDK default, which replicates the actor's transform."),
				*GetNameSafe(CachedOwner));
		}

		// Blueprint executors are unknown to the startup scan; the snapshot
		// struct they declare gets registered when the component comes alive.
		if (UScriptStruct* StateStruct = StateExecutor->GetStateStruct())
		{
			UActorUpdatePayloadRegistry::Get()->RegisterStructAuto(StateStruct);

			if (UCrowdyActorManager* Manager = GetWorld()->GetSubsystem<UCrowdyActorManager>())
				Manager->RegisterStateClass(StateStruct, GetOwner()->GetClass());
		}
		else
		{
			UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntityComponent]: StateExecutor '%s' on '%s' does not implement GetStateStruct — its snapshots cannot be serialized unless the struct is registered elsewhere."),
				*GetNameSafe(StateExecutor), *GetNameSafe(CachedOwner));
		}

		if (bAutoRegister && Role == ECrowdyRole::Owner)
			StartReplication();

		// Auto Register only starts the channel for an entity this client owns outright, so a host-owned entity
		// leaves BeginPlay sending nothing. Say so rather than letting a configured entity go quiet.
		if (ShouldReportHostOwnedAutoRegister(Mode, bAutoRegister, Role))
			ReportHostOwnedAutoRegisterSkipped();
	}
}

void UCrowdyEntityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(InitialOwnershipTimer);

	if (IsValid(AutoReplicator))
		AutoReplicator->UnregisterReplicationComponent(this);

	if (IsValid(EntitySubsystem) && NetID.IsValid())
		EntitySubsystem->UnregisterEntity(NetID);

	Super::EndPlay(EndPlayReason);
}

void UCrowdyEntityComponent::StartReplication()
{
	if (Mode != ECrowdyEntityMode::Dynamic)
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntityComponent]: StartReplication on '%s' ignored — component is not in Dynamic mode."),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (!IsValid(AutoReplicator))
		AutoReplicator = GetWorld()->GetSubsystem<UCrowdyAutoReplicator>();

	if (IsValid(AutoReplicator))
		AutoReplicator->RegisterReplicationComponent(this);
}

void UCrowdyEntityComponent::StopReplication()
{
	if (IsValid(AutoReplicator))
		AutoReplicator->UnregisterReplicationComponent(this);
}

void UCrowdyEntityComponent::SendEvent(const FInstancedStruct& Payload, const ECrowdyEventScope Scope)
{
	if (!IsValid(EntitySubsystem))
		return;

	const ECrowdyTarget Target = Scope == ECrowdyEventScope::OwnerOnly
		? ECrowdyTarget::Owner
		: ECrowdyTarget::Entity;
	EntitySubsystem->DispatchGameEvent(GetOwner(), FInstancedStruct(Payload), Target, GetOwner());
}

void UCrowdyEntityComponent::MarkStateDirty(FName PropertyName)
{
	if (!NetID.IsValid() || !GetWorld())
	{
		return;
	}
	if (UCrowdyStateReplicator* Replicator = GetWorld()->GetSubsystem<UCrowdyStateReplicator>())
	{
		Replicator->MarkStateDirty(NetID, PropertyName);
	}
}

void UCrowdyEntityComponent::MarkAllStateDirty()
{
	if (!NetID.IsValid() || !GetWorld())
	{
		return;
	}
	if (UCrowdyStateReplicator* Replicator = GetWorld()->GetSubsystem<UCrowdyStateReplicator>())
	{
		Replicator->MarkAllStateDirty(NetID);
	}
}

void UCrowdyEntityComponent::DestroyEntity()
{
	if (IsValid(EntitySubsystem))
		EntitySubsystem->DestroyEntity(GetOwner());
}

void UCrowdyEntityComponent::RequestOwnership_Implementation(FGuid RequesterID)
{
	if (!IsValid(EntitySubsystem) || !NetID.IsValid())
		return;

	// Only the entity's current authority reacts; on every other client this Multicast body is a harmless no-op.
	if (!IsLocallyOwned())
		return;

	// A malformed request, or one where the requester is already this entity's authority, is ignored. Only the
	// authority runs past the gate above, so its own player id is the one to compare against. This also covers the
	// host requesting a host-owned entity it already controls (whose OwnerID is invalid, so an OwnerID compare would
	// miss it).
	if (!RequesterID.IsValid() || RequesterID == EntitySubsystem->GetLocalPlayerID())
		return;

	if (bAutoApproveOwnershipRequests)
	{
		GrantOwnershipTo(RequesterID);
		return;
	}

	EntitySubsystem->NotifyOwnershipRequested(GetOwner(), RequesterID);
}

void UCrowdyEntityComponent::GrantOwnership_Implementation(FGuid NewOwnerID, FGuid PreviousOwnerID)
{
	if (!IsValid(EntitySubsystem) || !NetID.IsValid())
		return;

	// ReassignOwnership compare-and-swaps on PreviousOwnerID, so a stale or duplicate grant is dropped.
	EntitySubsystem->ReassignOwnership(NetID, NewOwnerID, PreviousOwnerID);
}

void UCrowdyEntityComponent::GrantOwnershipTo(const FGuid& NewOwnerID)
{
	if (!IsValid(EntitySubsystem))
		return;

	if (!NetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: GrantOwnershipTo on '%s' ignored — the entity is not registered."),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (!IsLocallyOwned())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: GrantOwnershipTo on '%s' ignored — this client is not the entity's authority."),
			*GetNameSafe(GetOwner()));
		return;
	}

	// Read the previous owner from the authoritative record so every receiver's compare-and-swap matches.
	const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(NetID);
	const FGuid PreviousOwnerID = Record ? Record->OwnerID : OwnerID;
	if (NewOwnerID == PreviousOwnerID)
		return;

	// Multicast: the body runs locally here (this authority relinquishes/adopts) and announces to every client.
	GrantOwnership(NewOwnerID, PreviousOwnerID);
}

void UCrowdyEntityComponent::ApplyOwnershipReassignment(const FGuid& NewOwnerID, const ECrowdyRole NewRole)
{
	OwnerID = NewOwnerID;
	Role    = NewRole;

	// The Dynamic-mode continuous StateExecutor channel follows authority: the new owner joins the AutoReplicator,
	// a client that just lost ownership leaves it. Static / event-only entities have no continuous channel to move.
	if (Mode == ECrowdyEntityMode::Dynamic)
	{
		if (IsLocallyOwned())
			StartReplication();
		else
			StopReplication();
	}
}

void UCrowdyEntityComponent::BroadcastOwnershipAssigned()
{
	bOwnershipAnnounced = true;

	const FGuid AnnouncedOwnerID = OwnerID;
	const ECrowdyRole AnnouncedRole = Role;
	const bool bAnnouncedLocallyOwned = IsLocallyOwned();

#if WITH_DEV_AUTOMATION_TESTS
	++OwnershipAnnouncementCount;
	LastAnnouncedOwnerID = AnnouncedOwnerID;
	LastAnnouncedRole = AnnouncedRole;
	bLastAnnouncedLocallyOwned = bAnnouncedLocallyOwned;
	LastAnnouncementStep = ++OwnershipStepCounter;
#endif

	OnCrowdyOwnershipAssigned.Broadcast(AnnouncedOwnerID, AnnouncedRole, bAnnouncedLocallyOwned);
}

void UCrowdyEntityComponent::ScheduleInitialOwnershipAnnouncement()
{
	if (bOwnershipAnnounced)
		return;

	UWorld* World = GetWorld();
	if (!World)
		return;

	World->GetTimerManager().ClearTimer(InitialOwnershipTimer);
	InitialOwnershipTimer = World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &UCrowdyEntityComponent::AnnounceInitialOwnership));
}

void UCrowdyEntityComponent::AnnounceInitialOwnership()
{
	// Nothing to announce for an entity that holds no identity: a pooled actor waiting in the pool had its
	// throwaway identity cleared, and it announces when the pool hands it a real one.
	if (bOwnershipAnnounced || !NetID.IsValid() || Role == ECrowdyRole::None)
		return;

	BroadcastOwnershipAssigned();
}

const FInstancedStruct& UCrowdyEntityComponent::GetReplicatedState() const
{
	if (!IsValid(StateExecutor))
	{
		return CachedState;
	}

	// The event thunk resolves GetActorState by name on every call before it reaches the native
	// implementation; resolving it per executor class takes that lookup out of the replication loop.
	const UClass* const ExecutorClass = StateExecutor->GetClass();
	if (ResolvedStateExecutorClass.Get() != ExecutorClass)
	{
		ResolvedStateExecutorClass = ExecutorClass;
		bStateExecutorAnswersNatively = CrowdyActorUpdateExecutor::AnswersStateNatively(StateExecutor);
	}

	CachedState = bStateExecutorAnswersNatively
		? StateExecutor->GetActorState_Implementation(this)
		: StateExecutor->GetActorState(this);
	return CachedState;
}

void UCrowdyEntityComponent::DeriveAuthority(const ECrowdyOwnership InOwnership, const FGuid& InLocalPlayerID,
	ECrowdyRole& OutRole, FGuid& OutOwnerID)
{
	if (InOwnership == ECrowdyOwnership::Host)
	{
		// World/AI entity: owned by whoever is host, no per-client owner id (Guid::Zero, matching
		// FCrowdyEntityRecord's "OwnerID is Guid::Zero for world-static entities").
		OutRole = ECrowdyRole::HostOwned;
		OutOwnerID = FGuid();
	}
	else
	{
		OutRole = ECrowdyRole::Owner;
		OutOwnerID = InLocalPlayerID;
	}
}

ECrowdyOwnership UCrowdyEntityComponent::ResolveEffectiveOwnership(const ECrowdyOwnership InAuthoredOwnership,
	const bool bIsLevelPlaced)
{
	// A level-placed world actor exists on every client before anyone joins, so it follows the ownership it was
	// authored with. An actor spawned at runtime exists because this client spawned it, so this client owns it
	// whatever the asset says; the clients that receive its spawn event get it as a remote proxy instead.
	return bIsLevelPlaced ? InAuthoredOwnership : ECrowdyOwnership::LocalClient;
}

bool UCrowdyEntityComponent::DoesOwnershipMatch(const ECrowdyRole OwnerRole, const FGuid& OwnerNetID,
	const ECrowdyRole TargetRole, const FGuid& TargetOwnerID, const FGuid& HostID)
{
	// The owner acts under the id it owns things as: the host id if it is itself a host-owned world entity,
	// otherwise its own NetID (a player avatar's NetID is its player id, which is what gets stamped as OwnerID
	// on the entities it spawns).
	const FGuid ActingID = (OwnerRole == ECrowdyRole::HostOwned) ? HostID : OwnerNetID;

	// The target is owned by its stamped OwnerID, or by the host if it is a host-owned world entity (those carry
	// Guid::Zero, so resolve the missing id to the host rather than treating it as unowned).
	FGuid OwnedByID = TargetOwnerID;
	if (!OwnedByID.IsValid() && TargetRole == ECrowdyRole::HostOwned)
	{
		OwnedByID = HostID;
	}

	return ActingID.IsValid() && OwnedByID.IsValid() && ActingID == OwnedByID;
}

bool UCrowdyEntityComponent::IsLocallyOwned() const
{
	// LocalClient ownership: we own it iff we are its (local) owner.
	if (Role == ECrowdyRole::Owner)
	{
		return true;
	}

	// Host ownership: a world entity has no per-client owner id; whichever client is the elected host owns it.
	// Fail closed when the host id is unknown (no host elected yet).
	if (Role == ECrowdyRole::HostOwned && IsValid(EntitySubsystem))
	{
		const FGuid HostID = EntitySubsystem->GetHostID();
		return HostID.IsValid() && EntitySubsystem->GetLocalPlayerID() == HostID;
	}

	return false;
}

void UCrowdyEntityComponent::ResolveIdentity()
{
	// An author-supplied binding key overrides IdentityPolicy: the object names its own cross-client identity, for
	// an independently-spawned world object the engine cannot place-identify (see ICrowdyBindingKeyProvider). An
	// empty key falls back to the policy below, so the interface is opt-in and inert unless it returns a value.
	FString BindingKey;
	if (IsValid(CachedOwner) && CachedOwner->GetClass()->ImplementsInterface(UCrowdyBindingKeyProvider::StaticClass()))
	{
		BindingKey = ICrowdyBindingKeyProvider::Execute_GetCrowdyBindingKey(CachedOwner);
	}

	// A binding key does not apply to a player: a player's identity is its account (PlayerDerived), and that branch
	// also establishes the session UUID other systems key on. Ignore a key on a PlayerDerived pawn and warn, so a
	// stray key never silently derails session identity / host election.
	const bool bUseKey = !BindingKey.IsEmpty() && IdentityPolicy != ECrowdyIdentityPolicy::PlayerDerived;
	if (!BindingKey.IsEmpty() && IdentityPolicy == ECrowdyIdentityPolicy::PlayerDerived)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: '%s' supplies a Crowdy binding key but its IdentityPolicy is PlayerDerived - the key is ignored (a player's identity is its account). Use a non-PlayerDerived policy for a keyed world object."),
			*GetNameSafe(CachedOwner));
	}

	// Whether the engine loaded this actor with its level. It decides both which identity an entity can share with
	// other clients and, further down, whether authored Host ownership applies, so it is answered once here.
	const bool bLevelPlaced = IsValid(CachedOwner) && CachedOwner->IsNetStartupActor();

	if (bUseKey)
	{
		NetID = FCrowdyModelIdentity::NetIDFromBindingKey(BindingKey);
	}
	else switch (IdentityPolicy)
	{
	case ECrowdyIdentityPolicy::PlayerDerived:
	{
		const APawn* Pawn = Cast<APawn>(CachedOwner);
		const AController* OwningController = Pawn ? Pawn->GetController() : nullptr;
		const APlayerController* PC = Cast<APlayerController>(OwningController);

		// The pawn questions are asked before the session is looked up, so an actor that could never use this
		// policy is answered by what it is, not by whatever the session happened to be doing.
		UWorld* World = GetWorld();
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UCrowdyGameSession* GameSession = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;

		FCrowdyPlayerDerivedIdentityFacts Facts;
		Facts.bIsPawn = Pawn != nullptr;
		Facts.bHasController = OwningController != nullptr;
		Facts.bControllerIsPlayerController = PC != nullptr;
		Facts.bIsLocalController = PC && PC->IsLocalController();
		Facts.bIsPrimaryPlayer = PC && PC->IsPrimaryPlayer();
		Facts.bHasGameSession = IsValid(GameSession);

		const ECrowdyPlayerDerivedIdentity Verdict = ClassifyPlayerDerivedIdentity(Facts);
		if (Verdict == ECrowdyPlayerDerivedIdentity::Resolved)
		{
			NetID = UHelperFunctions::GetDeterministicID(GameSession->GetUserID());

			// Establishes the session UUID other systems key on (ActorTracker,
			// HostSubsystem, EntitySubsystem all listen to OnOwnerUUIDUpdated).
			GameSession->SetUUID(NetID.ToString(EGuidFormats::Digits));
			break;
		}

		ReportPlayerDerivedFallback(Verdict);
		NetID = FGuid::NewGuid();
		break;
	}
	case ECrowdyIdentityPolicy::Stable:
	{
		// The engine's per-placement identity: identical on every client, distinct per placement of an instanced
		// level, and saved into the level. It is only available to an actor loaded with that level; anything else
		// falls back to the path hash. The two fallback cases mean very different things to an author, so they are
		// reported separately rather than as one message.
		bool bUsedPathFallback = false;
		NetID = ComputeStableNetID(bUsedPathFallback);
		UE_CLOG(bUsedPathFallback && !bLevelPlaced, LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: '%s' uses Stable identity but was created during play, so there is nothing every client can derive the same id from and its id will differ per client. Spawn it through Crowdy SpawnEntity, which assigns an id and announces it, or give it a Crowdy binding key."),
			*GetNameSafe(CachedOwner));
		UE_CLOG(bUsedPathFallback && bLevelPlaced, LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: '%s' was loaded with its level but has no engine instance guid - using path-derived identity, which is not stable under World Partition."),
			*GetNameSafe(CachedOwner));
		break;
	}
	case ECrowdyIdentityPolicy::Random:
		NetID = FGuid::NewGuid();
		break;
	}

	const FGuid LocalID = IsValid(EntitySubsystem) ? EntitySubsystem->GetLocalPlayerID() : FGuid();

	// Only an actor authored into the level can be owned by the host: it is there on every client from the start.
	// Everything else reached this point by being spawned at runtime, and belongs to the client that spawned it.
	// The engine's "loaded directly from the map" flag is the test, answered once above. It is set for every
	// level-placed actor, in the editor and in a packaged build alike, before play begins, so the same asset
	// resolves to the same authority everywhere. One edge to know about: an actor that turns off Net Load On Client
	// is not flagged, so it is classified as runtime-spawned and owned by the client it exists on.
	const ECrowdyOwnership Effective = ResolveEffectiveOwnership(Ownership, bLevelPlaced);

	UE_CLOG(Effective != Ownership, LogCrowdyReplication, Verbose,
		TEXT("[CrowdyEntityComponent]: '%s' is owned by this client - it was spawned at runtime, so its authored Host ownership does not apply (that setting is for actors placed in the level)."),
		*GetNameSafe(CachedOwner));

	DeriveAuthority(Effective, LocalID, Role, OwnerID);

	// A Host-owned entity needs a deterministic NetID that every client computes identically (Stable policy, or a
	// binding key); with any other policy the world entity would get a different id per client and never converge.
	// The test is on the ownership actually applied, not on the authored value: an entity that resolved to this
	// client has a per-client owner and needs no shared id, so warning on the authored value would be noise on
	// every runtime spawn. Warn but continue; the injected-identity spawn paths are unaffected, since they never
	// run identity resolution at all.
	if (Effective == ECrowdyOwnership::Host && IdentityPolicy != ECrowdyIdentityPolicy::Stable && !bUseKey)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: Ownership=Host on '%s' but IdentityPolicy is not Stable — world entities need a deterministic shared NetID; host authority may not converge across clients."),
			*GetNameSafe(CachedOwner));
	}
}

ECrowdyPlayerDerivedIdentity UCrowdyEntityComponent::ClassifyPlayerDerivedIdentity(const FCrowdyPlayerDerivedIdentityFacts& Facts)
{
	if (!Facts.bIsPawn)
		return ECrowdyPlayerDerivedIdentity::NotAPawn;

	if (!Facts.bHasController)
		return ECrowdyPlayerDerivedIdentity::PawnHasNoController;

	if (!Facts.bControllerIsPlayerController)
		return ECrowdyPlayerDerivedIdentity::ControllerIsNotAPlayer;

	if (!Facts.bIsLocalController)
		return ECrowdyPlayerDerivedIdentity::ControllerIsRemote;

	if (!Facts.bIsPrimaryPlayer)
		return ECrowdyPlayerDerivedIdentity::NotThePrimaryPlayer;

	if (!Facts.bHasGameSession)
		return ECrowdyPlayerDerivedIdentity::NoGameSession;

	return ECrowdyPlayerDerivedIdentity::Resolved;
}

const TCHAR* UCrowdyEntityComponent::DescribePlayerDerivedIdentity(const ECrowdyPlayerDerivedIdentity Verdict)
{
	switch (Verdict)
	{
	case ECrowdyPlayerDerivedIdentity::NotAPawn:
		return TEXT("its owner is not a Pawn");
	case ECrowdyPlayerDerivedIdentity::PawnHasNoController:
		return TEXT("the pawn is not possessed by any controller yet");
	case ECrowdyPlayerDerivedIdentity::ControllerIsNotAPlayer:
		return TEXT("the pawn is possessed by a controller that is not a player controller");
	case ECrowdyPlayerDerivedIdentity::ControllerIsRemote:
		return TEXT("the pawn is controlled by another client");
	case ECrowdyPlayerDerivedIdentity::NotThePrimaryPlayer:
		return TEXT("the pawn belongs to a secondary local player, not the primary one");
	case ECrowdyPlayerDerivedIdentity::NoGameSession:
		return TEXT("this client has no Crowdy game session to read a signed-in account from");
	default:
		return TEXT("it resolved from the signed-in account");
	}
}

bool UCrowdyEntityComponent::ShouldReportHostOwnedAutoRegister(const ECrowdyEntityMode InMode, const bool bInAutoRegister,
	const ECrowdyRole InRole)
{
	return InMode == ECrowdyEntityMode::Dynamic && bInAutoRegister && InRole == ECrowdyRole::HostOwned;
}

const TCHAR* UCrowdyEntityComponent::DescribeHostElectionState(const bool bHostKnown, const bool bLocalClientIsHost)
{
	if (!bHostKnown)
		return TEXT("this client does not know yet which client is the host");

	return bLocalClientIsHost
		? TEXT("this client is the elected host")
		: TEXT("another client is the elected host");
}

void UCrowdyEntityComponent::ReportPlayerDerivedFallback(const ECrowdyPlayerDerivedIdentity Verdict)
{
	if (bPlayerDerivedFallbackReported)
		return;

	bPlayerDerivedFallbackReported = true;

	const FString Message = FString::Printf(
		TEXT("[CrowdyEntityComponent]: Identity Policy is Player Derived on '%s' but %s, so it fell back to a random id. A random id is different on every client, so nothing can address this entity across the network. Player Derived reads the signed-in account and fits only the locally controlled primary player pawn: set Identity Policy to Stable for an actor placed in the level, give the actor a Crowdy binding key, or set it to Random deliberately if no other client needs to address it."),
		*GetNameSafe(CachedOwner), DescribePlayerDerivedIdentity(Verdict));

#if WITH_DEV_AUTOMATION_TESTS
	++PlayerDerivedFallbackReportCount;
	LastPlayerDerivedFallbackMessage = Message;
#endif

	UE_LOG(LogCrowdyReplication, Warning, TEXT("%s"), *Message);
}

void UCrowdyEntityComponent::ReportHostOwnedAutoRegisterSkipped()
{
	if (bHostOwnedAutoRegisterReported)
		return;

	bHostOwnedAutoRegisterReported = true;

	const FGuid HostID = IsValid(EntitySubsystem) ? EntitySubsystem->GetHostID() : FGuid();
	const bool bHostKnown = HostID.IsValid();
	const bool bLocalClientIsHost = bHostKnown && EntitySubsystem->GetLocalPlayerID() == HostID;

	const FString Message = FString::Printf(
		TEXT("[CrowdyEntityComponent]: '%s' is Dynamic with Auto Register on, but it is host-owned, so Auto Register did not start its state channel: Auto Register starts the channel only for an entity this client owns outright, and %s. Nothing goes out on the continuous channel for this entity until ownership is granted to a client or Start Replication is called on it. Set Ownership to Local Client if the client this actor lives on should simulate it, or call Start Replication on whichever client is the host."),
		*GetNameSafe(GetOwner()), DescribeHostElectionState(bHostKnown, bLocalClientIsHost));

#if WITH_DEV_AUTOMATION_TESTS
	++HostOwnedAutoRegisterReportCount;
	LastHostOwnedAutoRegisterMessage = Message;
#endif

	UE_LOG(LogCrowdyReplication, Warning, TEXT("%s"), *Message);
}
