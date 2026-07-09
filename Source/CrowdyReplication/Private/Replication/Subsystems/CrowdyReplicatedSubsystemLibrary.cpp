#include "Replication/Subsystems/CrowdyReplicatedSubsystemLibrary.h"

#include "CrowdyReplicationLog.h"
#include "Engine/World.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem, ECrowdyOwnership Ownership)
{
	if (!IsValid(Registry))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterReplicatedSubsystem skipped: no Crowdy Entity Subsystem available (a PIE/Game world is required). Nothing enrolled."));
		return FGuid{};
	}

	if (!IsValid(Subsystem))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterReplicatedSubsystem called with an invalid subsystem. Nothing enrolled."));
		return FGuid{};
	}

	return Registry->RegisterParticipant(Subsystem, Ownership);
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

FGuid UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystem(UObject* Subsystem, ECrowdyOwnership Ownership)
{
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterReplicatedSubsystem called with an invalid subsystem. Nothing enrolled."));
		return FGuid{};
	}

	UWorld* World = Subsystem->GetWorld();
	if (!World)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyReplicatedSubsystemLibrary]: RegisterReplicatedSubsystem could not resolve a world from %s. Nothing enrolled."),
			*Subsystem->GetName());
		return FGuid{};
	}

	// A null result (editor/inactive world with no entity subsystem) is handled as a clean no-op by the core.
	return RegisterReplicatedSubsystemInto(World->GetSubsystem<UCrowdyEntitySubsystem>(), Subsystem, Ownership);
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
