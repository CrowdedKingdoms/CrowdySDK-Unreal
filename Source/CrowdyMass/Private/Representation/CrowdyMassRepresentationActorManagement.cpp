#include "Representation/CrowdyMassRepresentationActorManagement.h"

#include "Actors/CrowdyMassNearTierActor.h"
#include "Components/CrowdyMassNearTierComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CrowdyMassLog.h"
#include "Fragments/CrowdyMassFragments.h"
#include "GameFramework/Actor.h"
#include "MassActorSpawnerSubsystem.h"
#include "MassCommandBuffer.h"
#include "MassEntityManager.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"

AActor* UCrowdyMassRepresentationActorManagement::GetOrSpawnActor(UMassRepresentationSubsystem& RepresentationSubsystem,
	FMassEntityManager& EntityManager, const FMassEntityHandle MassAgent, const FTransform& Transform,
	const int16 TemplateActorIndex, FMassActorSpawnRequestHandle& InOutSpawnRequestHandle, const float Priority) const
{
	FTransform RootTransform = Transform;

	// The transform belongs to the entity, and where an actor's root sits relative to that is the actor
	// class's own business: a shell rooted at its feet wants nothing added, one rooted at the middle of a
	// body wants half its height. Asked of the class default object, so nothing has to be spawned to know,
	// and converted by the shell itself so that the spawn, the teleport and the per-frame follow cannot
	// disagree about which way the offset points.
	if (const ACrowdyMassNearTierActor* DefaultShell = Cast<ACrowdyMassNearTierActor>(
		RepresentationSubsystem.GetTemplateActorClass(TemplateActorIndex).GetDefaultObject()))
	{
		RootTransform = DefaultShell->MakeRootTransformFromEntity(Transform);
	}

	return Super::GetOrSpawnActor(RepresentationSubsystem, EntityManager, MassAgent, RootTransform, TemplateActorIndex,
		InOutSpawnRequestHandle, Priority);
}

void UCrowdyMassRepresentationActorManagement::SetActorEnabled(const EMassActorEnabledType EnabledType, AActor& Actor,
	const int32 EntityIdx, FMassCommandBuffer& CommandBuffer) const
{
	Super::SetActorEnabled(EnabledType, Actor, EntityIdx, CommandBuffer);

	const bool bEnabled = EnabledType != EMassActorEnabledType::Disabled;

	// What the base class does NOT do, and the reason this override exists: disabling an actor there stops
	// its ticking and its collision and leaves it drawn. The entity that was disabled has gone back to
	// being one row of the crowd, so the row and this actor are both on screen, standing in different
	// places and doing different things.
	//
	// Hidden at the actor level rather than by writing a visibility onto every component. Actor hiding
	// composes with each component's own visibility instead of replacing it, so a sheathed weapon its author
	// left hidden is still hidden when the actor is shown; writing true onto every component would reveal
	// it, and because these actors are pooled rather than rebuilt it would stay revealed for every later
	// occupant of that actor for the rest of the session. It also covers whatever a subclass added, so a
	// demoted actor cannot leave floating decorations where its body used to be.
	Actor.SetActorHiddenInGame(!bEnabled);

	// The view is what copies the entity's transform, animation and collision onto this actor, so a demoted
	// actor that kept ticking it would go on following an entity nobody is drawing it for.
	if (UCrowdyMassNearTierComponent* View = Actor.FindComponentByClass<UCrowdyMassNearTierComponent>())
	{
		View->SetComponentTickEnabled(bEnabled);

		// This runs in the Mass phase at the head of the frame the actor starts being drawn on, and the view
		// does not tick until after physics, so without this the character's first frame on screen is drawn
		// from whatever its mesh was already holding. That single frame is a visible flinch. Done here
		// rather than on bind because an actor is bound when it spawns and shown some frames later, and an
		// action seeked at bind would be that many frames behind by the time anyone saw it.
		if (bEnabled)
		{
			View->PrimeAnimationForDisplay();
		}
	}

	// Runs once per entity when its representation changes, not per frame, which is what makes searching
	// the actor for a mesh affordable at all; the typed shell answers without a search.
	ACrowdyMassNearTierActor* Shell = Cast<ACrowdyMassNearTierActor>(&Actor);
	USkeletalMeshComponent* MeshComponent = Shell ? Shell->Mesh.Get() : Actor.FindComponentByClass<USkeletalMeshComponent>();
	if (!MeshComponent)
	{
		return;
	}

	// The mesh is switched unconditionally: a shell deliberately starts with its animation not ticking so
	// that an actor waiting to be shown does not evaluate a graph for a character the crowd is still
	// drawing, and it has to tick once the actor is the thing being drawn.
	MeshComponent->SetComponentTickEnabled(bEnabled);

	const TArray<USceneComponent*>& AttachedChildren = MeshComponent->GetAttachChildren();
	if (AttachedChildren.Num() == 0)
	{
		return;
	}

	TInlineComponentArray<USceneComponent*, NumInlinedActorComponents> ComponentStack;
	ComponentStack.Append(AttachedChildren);
	while (ComponentStack.Num() > 0)
	{
		USceneComponent* CurrentComponent = ComponentStack.Pop(EAllowShrinking::No);
		if (!CurrentComponent)
		{
			continue;
		}

		ComponentStack.Append(CurrentComponent->GetAttachChildren());

		// Only what was ticking to begin with starts again. A component the author deliberately left dormant
		// would otherwise come to life the first time its actor was promoted.
		CurrentComponent->SetComponentTickEnabled(bEnabled && CurrentComponent->PrimaryComponentTick.bStartWithTickEnabled);
	}
}

void UCrowdyMassRepresentationActorManagement::TeleportActor(const FTransform& Transform, AActor& Actor,
	FMassCommandBuffer& CommandBuffer) const
{
	FTransform RootTransform = Transform;

	if (const ACrowdyMassNearTierActor* Shell = Cast<ACrowdyMassNearTierActor>(&Actor))
	{
		RootTransform = Shell->MakeRootTransformFromEntity(Transform);
	}

	Super::TeleportActor(RootTransform, Actor, CommandBuffer);
}

bool UCrowdyMassRepresentationActorManagement::IsActorReadyForRepresentation(const AActor& Actor) const
{
	// A dedicated server draws nothing, so every signal an answer here could be built from stays false
	// forever and the entity would never leave its default representation.
	if (Actor.GetNetMode() == NM_DedicatedServer)
	{
		return true;
	}

	// The view, not the class, is what makes an actor a near tier shell: a game's own actor class opts in by
	// carrying one, and anything without one is not driven by this plugin at all and is left to answer for
	// itself.
	const UCrowdyMassNearTierComponent* View = Actor.FindComponentByClass<UCrowdyMassNearTierComponent>();
	if (!View)
	{
		return Super::IsActorReadyForRepresentation(Actor);
	}

	// Answering false is not a cross-fade. A moving entity's crowd row stops being drawn the moment the
	// promotion is decided, so this actor is the only thing left that could draw it and every frame spent
	// waiting is a frame in which the entity is not drawn at all. Both answers below are reads of state the
	// actor already holds, so the wait is normally none and never long.
	if (const ACrowdyMassNearTierActor* Shell = Cast<ACrowdyMassNearTierActor>(&Actor))
	{
		return Shell->IsReadyForRepresentation();
	}

	return View->IsBoundToEntity() && CrowdyMassNearTier::HasDrawableSkinnedMesh(Actor);
}

EMassActorSpawnRequestAction UCrowdyMassRepresentationActorManagement::OnPostActorSpawn(
	const FMassActorSpawnRequestHandle& SpawnRequestHandle, FConstStructView SpawnRequest,
	TSharedRef<FMassEntityManager> EntityManager) const
{
	const EMassActorSpawnRequestAction Action = Super::OnPostActorSpawn(SpawnRequestHandle, SpawnRequest, EntityManager);

	const FMassActorSpawnRequest& Request = SpawnRequest.Get<const FMassActorSpawnRequest>();

	AActor* SpawnedActor = Request.SpawnedActor;
	if (!SpawnedActor)
	{
		return Action;
	}

	// The view, not the class, is what makes an actor a near tier shell: a game's own actor class opts in by
	// carrying one. An actor without one has nothing to bind and is left alone.
	UCrowdyMassNearTierComponent* View = SpawnedActor->FindComponentByClass<UCrowdyMassNearTierComponent>();
	if (!View)
	{
		return Action;
	}

	// The first moment both halves of the join are in hand: the entity is known because it asked for this
	// spawn, and the actor is known because it has just arrived. Waiting for the actor to work it out for
	// itself would leave it standing there unjoined for as long as that took.
	FGuid EntityUUID;
	if (const FCrowdyRemoteIdentityFragment* Identity = EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Request.MassAgent))
	{
		EntityUUID = Identity->UUID;
	}

	View->BindToEntity(Request.MassAgent, EntityUUID);

	// Nothing in the actor pool says whether an actor was built or handed back out, and the two cost very
	// different amounts: without this a pool that never returns a hit looks exactly like one that always
	// does. Only this plugin's own shell records it, so an actor of some other class that opted in by
	// carrying the view is reported without that half.
	//
	// Consumed whether or not anyone is listening, because the mark belongs to this appearance: left
	// standing it would be read by whichever appearance happened to be the first after tracing was switched
	// on, and that one would be reported as a pool hit it never was.
	ACrowdyMassNearTierActor* Shell = Cast<ACrowdyMassNearTierActor>(SpawnedActor);
	const bool bFromPool = Shell && Shell->ConsumeCameFromPool();

	if (!CrowdyMassNearTier::IsTraceEnabled())
	{
		return Action;
	}

	UE_CLOG(Shell != nullptr && bFromPool, LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: entity %s took '%s' from the actor pool."),
		*EntityUUID.ToString(), *SpawnedActor->GetName());
	UE_CLOG(Shell != nullptr && !bFromPool, LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: entity %s had '%s' spawned for it; the actor pool had nothing to give."),
		*EntityUUID.ToString(), *SpawnedActor->GetName());
	UE_CLOG(Shell == nullptr, LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: entity %s was bound to '%s', which carries a near tier view but is not this plugin's shell class, so whether it came from the actor pool is not recorded."),
		*EntityUUID.ToString(), *SpawnedActor->GetName());

	return Action;
}
