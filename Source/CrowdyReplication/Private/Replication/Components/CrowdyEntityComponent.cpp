#include "Replication/Components/CrowdyEntityComponent.h"
#include "CrowdyReplicationLog.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Hash/CityHash.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
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
		Record.Actor   = CachedOwner;
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

void UCrowdyEntityComponent::DestroyEntity()
{
	if (IsValid(EntitySubsystem))
		EntitySubsystem->DestroyEntity(GetOwner());
}

const FInstancedStruct& UCrowdyEntityComponent::GetReplicatedState() const
{
	if (IsValid(StateExecutor))
		CachedState = StateExecutor->GetActorState(this);
	return CachedState;
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

	OwnerID = IsValid(EntitySubsystem) ? EntitySubsystem->GetLocalPlayerID() : FGuid();
	Role = ECrowdyRole::Owner;
	
	
}
