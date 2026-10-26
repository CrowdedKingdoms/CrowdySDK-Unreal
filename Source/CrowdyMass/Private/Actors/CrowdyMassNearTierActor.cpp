#include "Actors/CrowdyMassNearTierActor.h"

#include "Components/CrowdyMassNearTierComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "CrowdyMassLog.h"
#include "HAL/IConsoleManager.h"

namespace CrowdyMassNearTier
{
#if !UE_BUILD_SHIPPING
	int32 GTrace = 0;
	FAutoConsoleVariableRef CVarTrace(
		TEXT("crowdy.mass.neartier.trace"),
		GTrace,
		TEXT("Say when a crowd entity gains a near tier actor, whether that actor was built or taken from the pool, and when it goes back. 1 on, 0 off."),
		ECVF_Default);
#endif

	bool IsTraceEnabled()
	{
#if !UE_BUILD_SHIPPING
		return GTrace != 0;
#else
		return false;
#endif
	}

	bool HasDrawableSkinnedMesh(const AActor& Actor)
	{
		for (const UActorComponent* Component : Actor.GetComponents())
		{
			const USkinnedMeshComponent* SkinnedMesh = Cast<USkinnedMeshComponent>(Component);
			if (SkinnedMesh && SkinnedMesh->GetSkinnedAsset())
			{
				return true;
			}
		}

		return false;
	}

	USkeletalMeshComponent* ChooseActionMesh(const AActor& Actor)
	{
		// A mesh with no asset can be told to play an animation and will report success, having drawn
		// nothing, so the first skeletal mesh found is the wrong answer whenever an actor carries more than
		// one. That is not a rare shape: a shell whose own mesh is left empty so a subclass can add its own
		// carries exactly two, and the set of an actor's components has no authored order, so picking the
		// first would swallow every action on some runs and none on others.
		USkeletalMeshComponent* FirstWithAsset = nullptr;

		for (UActorComponent* Component : Actor.GetComponents())
		{
			USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component);
			if (!SkeletalMesh || !SkeletalMesh->GetSkinnedAsset())
			{
				continue;
			}

			// The shell's own mesh wins when it has something to draw, because that is the one the crowd's
			// mesh transform is mirrored onto and therefore the one standing where the entity stands.
			const ACrowdyMassNearTierActor* Shell = Cast<ACrowdyMassNearTierActor>(&Actor);
			if (Shell && Shell->Mesh == SkeletalMesh)
			{
				return SkeletalMesh;
			}

			if (!FirstWithAsset)
			{
				FirstWithAsset = SkeletalMesh;
			}
		}

		return FirstWithAsset;
	}

	bool ShouldCarryTraceCollision(const UPrimitiveComponent& Primitive, const USkeletalMeshComponent* ActionMesh)
	{
		static const FName TraceCollisionTag(TEXT("CrowdyTraceCollision"));
		return &Primitive == ActionMesh || Primitive.ComponentHasTag(TraceCollisionTag);
	}
}

ACrowdyMassNearTierActor::ACrowdyMassNearTierActor()
{
	// A picture has nothing to do per frame, and there can be a great many of these. A subclass that does
	// need a tick turns it on in its own defaults, and the promotion then switches that tick on and off
	// with the actor, so it costs nothing while the actor is waiting or demoted.
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Hidden until the promotion says otherwise, and hidden at the ACTOR level rather than by switching off
	// each component. An actor is spawned some frames before the promotion decides to show it, and anything
	// visible in the meantime is a second copy of a character the crowd is still drawing as a row, standing
	// in its bind pose wherever the spawn happened to be asked for. Hiding the actor covers whatever a
	// subclass added without overwriting the visibility that subclass authored, so a part deliberately left
	// hidden is still hidden when the actor is shown.
	SetHidden(true);

	// The root is a bare pivot and the mesh hangs under it, rather than the mesh being the root itself.
	// A skeletal mesh almost always needs placing relative to the thing it represents (the standard
	// character skeleton faces sideways until it is yawed, and a mesh authored about its hips has to be
	// dropped to stand on the ground), and the editor does not let a root component's own location and
	// rotation be edited, so a mesh that IS the root leaves nowhere to say any of that. Keeping the pivot
	// separate is also what lets the crowd's authored mesh transform be mirrored onto this actor, which is
	// what keeps a promoted character facing the way its crowd row faced.
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->PrimaryComponentTick.bStartWithTickEnabled = false;

	NearTier = CreateDefaultSubobject<UCrowdyMassNearTierComponent>(TEXT("NearTier"));
}

void ACrowdyMassNearTierActor::BeginPlay()
{
	Super::BeginPlay();

	// Said again here because a Blueprint subclass edits these defaults and can turn the actor back on
	// without meaning anything by it. Between spawning and being shown this actor stands at whatever
	// transform the spawn was asked for, in its bind pose, while the crowd is still drawing the same
	// character as a row: two copies, one of them wrong.
	SetActorHiddenInGame(true);

	if (Mesh)
	{
		Mesh->SetComponentTickEnabled(false);
	}
}

FVector ACrowdyMassNearTierActor::GetEntityTransformOffset() const
{
	return EntityTransformOffset;
}

FTransform ACrowdyMassNearTierActor::MakeRootTransformFromEntity(const FTransform& EntityTransform) const
{
	// Rotated by the entity, because the offset is a distance in the entity's own space: a shell rooted at
	// the middle of a body sits above the entity's feet whichever way that entity is facing, and adding the
	// offset unrotated would swing the actor around the entity as it turns.
	FTransform RootTransform = EntityTransform;
	RootTransform.AddToTranslation(EntityTransform.GetRotation().RotateVector(GetEntityTransformOffset()));
	return RootTransform;
}

bool ACrowdyMassNearTierActor::IsReadyForRepresentation() const
{
	// Both halves of "there is something to draw and it knows who it is": a mesh with an asset resolved,
	// wherever on this actor it lives, and a join to the entity, without which the first frame shown is a
	// bind pose at a stale transform.
	return NearTier != nullptr
		&& NearTier->IsBoundToEntity()
		&& CrowdyMassNearTier::HasDrawableSkinnedMesh(*this);
}

bool ACrowdyMassNearTierActor::ConsumeCameFromPool()
{
	const bool bResult = bCameFromPool;
	bCameFromPool = false;
	return bResult;
}

bool ACrowdyMassNearTierActor::CanBePooled_Implementation()
{
	// Without this the pool refuses every actor of this class and each promotion is a real spawn and a
	// real destroy, which is the cost the whole near tier exists to bound.
	return true;
}

void ACrowdyMassNearTierActor::PrepareForPooling_Implementation()
{
	// Read before the unbind, because the unbind is what forgets it, and the id of the entity this actor
	// was standing for is the only thing that makes a release line answerable against a promotion line.
	const FGuid ReleasedUUID = NearTier ? NearTier->GetEntityUUID() : FGuid();

	if (NearTier)
	{
		NearTier->UnbindFromEntity();
	}

	// Nothing else on this path switches collision off. An entity destroyed while it is promoted releases
	// its actor straight to the pool without the demotion step that disables it, so the shell would sit in
	// the pool, hidden, at the dead entity's last position, still carrying the collision the view pushed
	// onto it. It would go on blocking traces and camera sweeps through a point where nothing is drawn, and
	// a blocked trace also shortens the search that finds live entities behind it, until the pool happens to
	// hand the shell out again.
	SetActorEnableCollision(false);

	SetActorHiddenInGame(true);

	if (Mesh)
	{
		Mesh->SetComponentTickEnabled(false);
	}

	UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: '%s' released to the actor pool from entity %s."),
		*GetName(), *ReleasedUUID.ToString());
}

void ACrowdyMassNearTierActor::PrepareForGame_Implementation()
{
	// Whoever asks next learns this appearance cost nothing to build. Nothing in the pool itself says so.
	bCameFromPool = true;

	// Drop the previous occupant's join, in case this actor reached the pool by a route that did not clear
	// it. Everything that drives this actor reads that join, so a stale one would drive it from the wrong
	// entity until the promotion got round to setting the right one.
	if (NearTier)
	{
		NearTier->UnbindFromEntity();
	}

	// The engine unhides a pooled actor just before this runs, so this is what keeps it out of sight until
	// the promotion has bound it and decided to show it.
	SetActorHiddenInGame(true);

	// An animation graph is state, and this actor is about to stand for a different character. Stopping the
	// montages is not enough: the graph's state machines, blend weights and variables would still be exactly
	// where the previous occupant left them, so the new one's first frames would be somebody else's. A full
	// reinitialise costs one animation instance per handover and is the one reset no shell author has to
	// remember to write.
	for (UActorComponent* Component : GetComponents())
	{
		if (USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component))
		{
			SkeletalMesh->InitAnim(/*bForceReinit=*/true);
			SkeletalMesh->SetComponentTickEnabled(false);
		}
	}
}
