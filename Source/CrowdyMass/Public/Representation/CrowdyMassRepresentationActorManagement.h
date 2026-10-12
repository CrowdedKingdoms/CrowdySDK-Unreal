#pragma once

#include "CoreMinimal.h"
#include "MassRepresentationActorManagement.h"

class AActor;

#include "CrowdyMassRepresentationActorManagement.generated.h"

/**
 * How a crowd entity's near tier actor is spawned, placed, shown and hidden.
 *
 * The stock behaviour is nearly right and wrong in one place that matters: disabling an actor there
 * switches off its ticking and its collision and never touches its visibility, so a disabled actor is
 * still drawn. An entity that has fallen back to being one row of the crowd is then drawn twice, once as
 * the row and once as the actor nobody turned off, and the two disagree about where it is and what it is
 * doing. Hiding the actor on the way down is what this class is for.
 *
 * Everything else here is small: an offset so that an actor's root lands where the entity's transform
 * says the entity is, a readiness answer so an actor is not shown before it has a mesh and knows which
 * entity it stands for, and one log line per promotion and per release, which is the only way to see
 * that the actor pool is being reused at all.
 *
 * What an actor is asked about is its Crowdy Mass Near Tier component, never its class. That component is
 * what joins an actor to its entity and drives it, so a game's own actor class opts in by carrying one and
 * an actor without one is not this plugin's to drive.
 */
UCLASS(meta=(DisplayName="Crowdy Mass Representation Actor Management"))
class CROWDYMASS_API UCrowdyMassRepresentationActorManagement : public UMassRepresentationActorManagement
{
	GENERATED_BODY()

public:
	virtual AActor* GetOrSpawnActor(UMassRepresentationSubsystem& RepresentationSubsystem, FMassEntityManager& EntityManager,
		const FMassEntityHandle MassAgent, const FTransform& Transform, const int16 TemplateActorIndex,
		FMassActorSpawnRequestHandle& InOutSpawnRequestHandle, const float Priority) const override;

	virtual void SetActorEnabled(const EMassActorEnabledType EnabledType, AActor& Actor, const int32 EntityIdx,
		FMassCommandBuffer& CommandBuffer) const override;

	virtual void TeleportActor(const FTransform& Transform, AActor& Actor, FMassCommandBuffer& CommandBuffer) const override;

	virtual bool IsActorReadyForRepresentation(const AActor& Actor) const override;

	virtual EMassActorSpawnRequestAction OnPostActorSpawn(const FMassActorSpawnRequestHandle& SpawnRequestHandle,
		FConstStructView SpawnRequest, TSharedRef<FMassEntityManager> EntityManager) const override;
};
