#include "Replication/Subsystems/CrowdyReplicatedSubsystemLibrary.h"

#include "CrowdyReplicationLog.h"
#include "Engine/World.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

static bool CanEnrollReplicatedSubsystem(const UCrowdyEntitySubsystem* Registry, const UObject* Subsystem)
{
	if (!IsValid(Registry))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterCrowdyReplicatedSubsystem skipped: no Crowdy Entity Subsystem available (a PIE/Game world is required). Nothing enrolled."));
		return false;
	}

	if (!IsValid(Subsystem))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterCrowdyReplicatedSubsystem called with an invalid subsystem. Nothing enrolled."));
		return false;
	}
	return true;
}

static UWorld* ResolveReplicatedSubsystemWorld(const UObject* Subsystem)
{
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterCrowdyReplicatedSubsystem called with an invalid subsystem. Nothing enrolled."));
		return nullptr;
	}

	UWorld* World = Subsystem->GetWorld();
	UE_CLOG(!World, LogCrowdyReplication, Warning,
		TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterCrowdyReplicatedSubsystem could not resolve a world from %s. Nothing enrolled."),
		*Subsystem->GetName());
	return World;
}

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem)
{
	return CanEnrollReplicatedSubsystem(Registry, Subsystem) ? Registry->RegisterParticipant(Subsystem) : FGuid{};
}

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem, ECrowdyOwnership Ownership)
{
	return CanEnrollReplicatedSubsystem(Registry, Subsystem) ? Registry->RegisterParticipant(Subsystem, Ownership) : FGuid{};
}

void UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem)
{
	// Silent no-op on a missing registry (the per-world entity subsystem may already be gone during world
	// teardown) or a null subsystem. Unenrolling nothing is not a warnable condition.
	if (!IsValid(Registry) || !Subsystem)
	{
		return;
	}

	Registry->UnregisterParticipant(Subsystem);
}

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterCrowdyReplicatedSubsystem(UObject* Subsystem)
{
	// A world with no entity subsystem (an editor or inactive world) is a clean no-op in the core.
	const UWorld* World = ResolveReplicatedSubsystemWorld(Subsystem);
	return World ? RegisterReplicatedSubsystemInto(World->GetSubsystem<UCrowdyEntitySubsystem>(), Subsystem) : FGuid{};
}

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystem(UObject* Subsystem, ECrowdyOwnership Ownership)
{
	const UWorld* World = ResolveReplicatedSubsystemWorld(Subsystem);
	if (!World)
	{
		return FGuid{};
	}

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return RegisterReplicatedSubsystemInto(World->GetSubsystem<UCrowdyEntitySubsystem>(), Subsystem, Ownership);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystem(UObject* Subsystem)
{
	if (!Subsystem)
	{
		return;
	}

	UWorld* World = Subsystem->GetWorld();
	if (!World)
	{
		return;
	}

	UnregisterReplicatedSubsystemInto(World->GetSubsystem<UCrowdyEntitySubsystem>(), Subsystem);
}
