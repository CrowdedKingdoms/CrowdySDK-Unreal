#include "Replication/Components/CrowdyEntityComponent.h"
#include "CrowdyReplicationLog.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Hash/CityHash.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h" // complete type for the replicator header's inline test seam
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"

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

	if (Mode == ECrowdyEntityMode::Dynamic)
	{
		AutoReplicator = GetWorld()->GetSubsystem<UCrowdyAutoReplicator>();

		if (!IsValid(StateExecutor))
		{
			UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntityComponent]: Dynamic mode on '%s' but no StateExecutor assigned — continuous replication disabled."),
				*GetNameSafe(CachedOwner));
			return;
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
	}
}

void UCrowdyEntityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
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
	// authority runs past the gate above, so its own player id is the one to compare against — this also covers the
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

const FInstancedStruct& UCrowdyEntityComponent::GetReplicatedState() const
{
	if (IsValid(StateExecutor))
		CachedState = StateExecutor->GetActorState(this);
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
	switch (IdentityPolicy)
	{
	case ECrowdyIdentityPolicy::PlayerDerived:
	{
		UCrowdyGameSession* GameSession = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();

		const APawn* Pawn = Cast<APawn>(CachedOwner);
		const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		const bool bIsLocalPlayer = PC && PC->IsLocalController() && PC->IsPrimaryPlayer();

		if (IsValid(GameSession) && bIsLocalPlayer)
		{
			NetID = UHelperFunctions::GetDeterministicID(GameSession->GetUserID());

			// Establishes the session UUID other systems key on (ActorTracker,
			// HostSubsystem, EntitySubsystem all listen to OnOwnerUUIDUpdated).
			// Phase 7 moves this to the login flow.
			GameSession->SetUUID(NetID.ToString(EGuidFormats::Digits));
			break;
		}

		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntityComponent]: PlayerDerived identity on '%s' but it is not the locally controlled pawn — using Random instead."),
			*GetNameSafe(CachedOwner));
		NetID = FGuid::NewGuid();
		break;
	}
	case ECrowdyIdentityPolicy::Stable:
	{
		// Identical on every client for the same level-placed actor; the PIE
		// prefix is stripped, so PIE clients in one process agree too.
		const FString StablePath = UWorld::RemovePIEPrefix(CachedOwner->GetPathName());
		const uint64 PathHash = CityHash64(
			reinterpret_cast<const char*>(*StablePath), StablePath.Len() * sizeof(TCHAR));
		NetID = UHelperFunctions::GetDeterministicID(static_cast<int64>(PathHash));
		break;
	}
	case ECrowdyIdentityPolicy::Random:
		NetID = FGuid::NewGuid();
		break;
	}

	const FGuid LocalID = IsValid(EntitySubsystem) ? EntitySubsystem->GetLocalPlayerID() : FGuid();
	DeriveAuthority(Ownership, LocalID, Role, OwnerID);

	// A Host-owned entity needs a deterministic NetID that every client computes identically (Stable policy);
	// with any other policy the world entity would get a different id per client and never converge. Warn but
	// continue the injected-identity spawn paths are unaffected (they never run ResolveIdentity).
	if (Ownership == ECrowdyOwnership::Host && IdentityPolicy != ECrowdyIdentityPolicy::Stable)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntityComponent]: Ownership=Host on '%s' but IdentityPolicy is not Stable — world entities need a deterministic shared NetID; host authority may not converge across clients."),
			*GetNameSafe(CachedOwner));
	}
}
