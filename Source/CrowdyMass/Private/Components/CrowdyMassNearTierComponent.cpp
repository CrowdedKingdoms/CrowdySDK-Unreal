#include "Components/CrowdyMassNearTierComponent.h"

#include "Actors/CrowdyMassNearTierActor.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "GameFramework/Actor.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassActionLibrary.h"
#include "Mass/EntityFragments.h"
#include "MassActorSubsystem.h"
#include "MassEntityManager.h"
#include "MassMovementFragments.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Subsystems/CrowdyMassServerRecordsSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/ScriptMacros.h"
#include "UObject/UnrealType.h"

namespace CrowdyMassNearTier
{
	FTransform ResolveShellMeshTransform(const FSkinnedMeshInstanceVisualizationDesc& Desc, const FTransform& Authored)
	{
		// COMPOSED, never chosen between. The description says where the crowd puts this mesh, and a
		// description usually carries only part of what an actor needs: a character's is typically a drop
		// to stand the mesh on the ground and no rotation at all, because the instanced path and a skeletal
		// mesh component do not need the same turn to face the same way. Replacing one with the other loses
		// whichever half was not in the winner, which reads as a mesh that is either facing wrong or buried.
		//
		// The shell's own transform is therefore a correction applied FIRST, in the mesh's own space, and
		// the crowd's placement then applies on top: A * B applies A then B. An unauthored shell composes
		// with identity and simply takes the crowd's placement.
		return Authored * MeshTransformFromVisualizationDesc(Desc);
	}

	FTransform MeshTransformFromVisualizationDesc(const FSkinnedMeshInstanceVisualizationDesc& Desc)
	{
		// The first mesh, because a shell draws one and the crowd's first entry is the one it stands for.
		// A description naming several meshes is a crowd built from parts, which an actor shell does not
		// reproduce; the rest are left to whatever that shell authored.
		return Desc.Meshes.IsEmpty() ? FTransform::Identity : Desc.Meshes[0].LocalTransform;
	}

	bool ShouldStartHeldAction(const bool bIsBeingShownNow, const bool bHasStartedAnAction, const int32 StartedSerial,
		const int32 IncomingSerial)
	{
		// The frame an actor starts being drawn on takes the action again whatever its serial says. A mesh
		// that is not being drawn does not tick, so a montage started when this view bound is frozen at the
		// frame it began on, however many frames ago that was, and a mesh out of the pool is holding the
		// pose of whoever it was drawing last. Neither is this entity's animation now, and the serial cannot
		// say so because it has not changed.
		if (bIsBeingShownNow)
		{
			return true;
		}

		// Freshness by INEQUALITY, never by ordering: the serial wraps at the top of its range, and a
		// "newer than" test would lose every action for the rest of the entity's life once it did. The
		// separate "have one at all" flag covers a first bind that lands on a serial of zero.
		return !bHasStartedAnAction || IncomingSerial != StartedSerial;
	}

	float ActionBlendInForSeek(const float AuthoredBlendInSeconds, const float StartSecondsIntoAction)
	{
		// The blend length is writable from a graph, so a negative or non-finite one can reach here and
		// would otherwise be handed to the montage as it stands.
		const float SafeBlendInSeconds = FMath::IsFinite(AuthoredBlendInSeconds)
			? FMath::Max(AuthoredBlendInSeconds, 0.0f)
			: 0.0f;

		// Within one blend length of the start is an action starting from its beginning, allowing for the
		// frame or two of elapsed time every action already carries by the time it is read here. Anything
		// further in is an action that was already running, and blending into the middle of a clip from a
		// default pose is the flinch.
		const bool bStartsWithinTheBlend = FMath::IsFinite(StartSecondsIntoAction)
			&& StartSecondsIntoAction <= SafeBlendInSeconds;

		return bStartsWithinTheBlend ? SafeBlendInSeconds : 0.0f;
	}

	// Authoring faults already reported for one world. Each is a gap that lasts as long as the class does,
	// and a crowd promotes and demotes the same classes continuously, so each is said once rather than once
	// per promotion.
	struct FReportedAnimSetFaults
	{
		// Entity classes with no animation set to drive a near tier actor from.
		TSet<FObjectKey> MissingAnimSetClasses;

		// Shell-class and entity-class pairs naming different animation sets. Both halves are the key: one
		// shell borrowed for two entity classes is two separate faults, and one entity class drawn through
		// two shells is two more.
		TSet<TPair<FObjectKey, FObjectKey>> AnimSetMismatches;
	};

	// Kept per world rather than per process. An editor runs many play sessions in one process, and a fault
	// said in the first of them and never again leaves a crowd standing in its bind pose with nothing left
	// on screen or in the log to explain why.
	static TMap<FObjectKey, FReportedAnimSetFaults> GReportedAnimSetFaultsByWorld;

	static FReportedAnimSetFaults& ReportedAnimSetFaultsForWorld(const UWorld* World)
	{
		// Worlds that have gone are dropped here rather than through a teardown callback. A key that no
		// longer resolves names a play session that has ended, which can never report anything again, and
		// the next session is a different world and starts with nothing reported.
		for (auto It = GReportedAnimSetFaultsByWorld.CreateIterator(); It; ++It)
		{
			if (!It.Key().ResolveObjectPtr())
			{
				It.RemoveCurrent();
			}
		}

		return GReportedAnimSetFaultsByWorld.FindOrAdd(FObjectKey(World));
	}

	static FName GaitNameFromSet(const UCrowdyMassAnimSet* AnimSet, const int32 GaitId)
	{
		const int32 Index = GaitId - 1;
		if (!AnimSet || !AnimSet->Gaits.IsValidIndex(Index))
		{
			return NAME_None;
		}

		return AnimSet->Gaits[Index].GaitName;
	}

	static FName PoseNameFromSet(const UCrowdyMassAnimSet* AnimSet, const int32 PoseId)
	{
		const int32 Index = PoseId - 1;
		if (!AnimSet || !AnimSet->Poses.IsValidIndex(Index))
		{
			return NAME_None;
		}

		return AnimSet->Poses[Index].PoseName;
	}

	// Why a class has no animation set, said in the words of the fix rather than as a null pointer. The
	// three misses are kept apart because they are three different jobs: nobody has asked, nothing names
	// one, or one is named and would not load.
	static const TCHAR* DescribeAnimSetMiss(const ECrowdyMassAnimSetSource Source)
	{
		switch (Source)
		{
		case ECrowdyMassAnimSetSource::NoClass:
			return TEXT("nothing knows which class this entity is, so there is no class to resolve an animation set for");
		case ECrowdyMassAnimSetSource::NotResolved:
			return TEXT("no animation set has been worked out for this class yet");
		case ECrowdyMassAnimSetSource::Undeclared:
			return TEXT("nothing names an animation set for this class: no Mass entity config authored for it carries an animation trait naming one, and the class carries no Crowdy Mass Anim Set component either");
		case ECrowdyMassAnimSetSource::LoadFailed:
			return TEXT("an animation set IS named for this class and would not load, so the asset is missing, renamed or unbuilt");
		case ECrowdyMassAnimSetSource::ClassComponentNotRendered:
			return TEXT("the class names an animation set on its own component, but the Mass entity config its entities are built from installs no animation trait naming that set, so nothing renders it");
		default:
			return TEXT("an animation set was named and did not reach this entity");
		}
	}
}

UCrowdyMassNearTierComponent::UCrowdyMassNearTierComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Every processor that writes the fragments read here runs in the PrePhysics Mass phase, and the one
	// that expires actions is allowed off the game thread, so reading them during PrePhysics is a data
	// race rather than a frame-ordering preference. Do not move this earlier.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UCrowdyMassNearTierComponent::BeginPlay()
{
	Super::BeginPlay();

	// Nothing can bind this view on a map that draws no crowd rows, so it does not tick there.
	if (!UCrowdyMassEntitySubsystem::DrawsCrowdRows(GetWorld()))
	{
		SetComponentTickEnabled(false);
	}
}

void UCrowdyMassNearTierComponent::OnUnregister()
{
	UnbindFromEntity();
	Super::OnUnregister();
}

UCrowdyMassServerRecordsSubsystem* UCrowdyMassNearTierComponent::GetServerRecords() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyMassServerRecordsSubsystem* ForTest = ServerRecordsForTest.Get())
	{
		return ForTest;
	}
#endif
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCrowdyMassServerRecordsSubsystem>() : nullptr;
}

void UCrowdyMassNearTierComponent::WatchServerValues()
{
	UnwatchServerValues();
	UCrowdyMassServerRecordsSubsystem* Records = bBound ? GetServerRecords() : nullptr;
	if (!Records)
	{
		return;
	}
	ServerValuesEntity = BoundEntity;
	ServerValuesHandle = Records->WatchEntity(BoundEntity, FOnCrowdyMassServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyMassNearTierComponent::HandleServerValuesChanged));
}

void UCrowdyMassNearTierComponent::UnwatchServerValues()
{
	UCrowdyMassServerRecordsSubsystem* Records = ServerValuesHandle.IsValid() ? GetServerRecords() : nullptr;
	if (Records)
	{
		Records->UnwatchEntity(ServerValuesEntity, ServerValuesHandle);
	}
	ServerValuesEntity.Reset();
	ServerValuesHandle.Reset();
}

void UCrowdyMassNearTierComponent::HandleServerValuesChanged(const FCrowdyMassEntityTarget& Target, TConstArrayView<FString> Changed)
{
	if (!OnServerValuesChanged.IsBound())
	{
		return;
	}
	OnServerValuesChanged.Broadcast(TArray<FString>(Changed));
}

bool UCrowdyMassNearTierComponent::ReadServerValueInto(FName Name, const FProperty* DestProperty, void* Dest) const
{
	const UCrowdyMassServerRecordsSubsystem* Records = bBound ? GetServerRecords() : nullptr;
	if (Records)
	{
		return Records->ReadServerValueInto(BoundEntity, Name, DestProperty, Dest);
	}
	if (DestProperty && Dest)
	{
		DestProperty->ClearValue(Dest);
	}
	return false;
}

DEFINE_FUNCTION(UCrowdyMassNearTierComponent::execGetServerValue)
{
	P_GET_PROPERTY(FNameProperty, Name);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bRead = false;
	P_NATIVE_BEGIN;
	bRead = P_THIS->ReadServerValueInto(Name, ValueProperty, Value);
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bRead;
}

UCrowdyMassEntitySubsystem* UCrowdyMassNearTierComponent::GetEntitySubsystem() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
}

FMassEntityManager* UCrowdyMassNearTierComponent::GetEntityManager() const
{
	const UCrowdyMassEntitySubsystem* Subsystem = GetEntitySubsystem();
	return Subsystem ? Subsystem->GetMassEntityManager() : nullptr;
}

bool UCrowdyMassNearTierComponent::BindToEntity(const FMassEntityHandle Entity, const FGuid& UUID)
{
	if (!Entity.IsSet())
	{
		UnbindFromEntity();
		return false;
	}

	const FMassEntityManager* EntityManager = GetEntityManager();
	if (!EntityManager || !EntityManager->IsEntityValid(Entity))
	{
		UnbindFromEntity();
		return false;
	}

	if (bBound && BoundEntity == Entity && EntityUUID == UUID)
	{
		return true;
	}

	SetBoundEntity(Entity, UUID);
	ResolveAnimSetForBoundEntity();

	// Pushed here and not left to the first tick. The entity's crowd row stops being traced the moment the
	// actor is enabled, which happens in the Mass phase at the head of the frame, while this component
	// ticks after physics. Everything that traces in between would find a pooled shell still carrying the
	// previous occupant's object type and responses, or a fresh one carrying whatever its class authored.
	ApplyTraceCollisionFromBoundEntity(*EntityManager);

	// The crowd does not draw an entity's mesh at the entity's own transform: it draws it at the mesh
	// transform authored beside the mesh, applied to the entity's transform. A promoted actor that skipped
	// that would face and stand differently from the row it just replaced, which reads as the promotion
	// turning the character rather than as a missing transform.
	ApplyMeshTransformFromBoundEntity(*EntityManager);

	// An actor that is ALREADY on screen when it is bound has to be right on this frame, and only this
	// call can make it so: the promotion primes an actor as it shows it, but a game binding its own actor
	// by hand, and a pooled shell handed straight from one entity to the next, are both bound while
	// visible. An actor still hidden is left alone and primed when it is shown, which is where a promotion
	// does it, so nothing is evaluated twice.
	const AActor* Owner = GetOwner();
	if (Owner && !Owner->IsHidden())
	{
		PrimeAnimationForDisplay();
	}

	UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: '%s' bound to entity %d (uuid %s), animation set '%s'."),
		*GetNameSafe(GetOwner()), Entity.Index, *UUID.ToString(),
		*GetPathNameSafe(ResolvedAnimSet.Get()));

	return true;
}

void UCrowdyMassNearTierComponent::UnbindFromEntity()
{
	const bool bWasBound = bBound;
	UnwatchServerValues();

	// The montage goes first and goes without a blend. This runs as the actor is being taken back, so
	// there is nothing left to blend into and a swing left running would still be playing when the pool
	// hands this actor to the next entity.
	StopActiveActionMontage();

	// Before the record of what was pushed is dropped, because that record is what says how to take it
	// back. A shell released while its entity was still promoted is otherwise left in the pool, invisible,
	// at the dead entity's last transform, blocking every trace aimed through it.
	ClearAppliedTraceCollision();

	BoundEntity.Reset();
	EntityUUID.Invalidate();
	bBound = false;
	bActionsRefused = false;
	StartedActionSerial = 0;
	bHasStartedActionSerial = false;
	bHasAppliedTraceCollision = false;
	ResolvedAnimSet.Reset();
	ActionMesh.Reset();
	ShellOwner.Reset();
	ClearAnimationInputs();

	UE_CLOG(bWasBound && CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: '%s' unbound."), *GetNameSafe(GetOwner()));
}

bool UCrowdyMassNearTierComponent::ResolveEntityFromOwner()
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return false;
	}

	UMassActorSubsystem* ActorSubsystem = World->GetSubsystem<UMassActorSubsystem>();
	if (!ActorSubsystem)
	{
		return false;
	}

	// The engine fills both directions of the actor-to-entity mapping when it spawns an actor for an
	// entity, so an actor promoted out of a crowd needs no explicit bind.
	const FMassEntityHandle Entity = ActorSubsystem->GetEntityHandleFromActor(Owner);
	if (!Entity.IsSet())
	{
		return false;
	}

	FGuid UUID;
	if (const UCrowdyMassEntitySubsystem* Subsystem = GetEntitySubsystem())
	{
		Subsystem->FindUUIDByEntity(Entity, UUID);
	}

	return BindToEntity(Entity, UUID);
}

void UCrowdyMassNearTierComponent::SetBoundEntity(const FMassEntityHandle Entity, const FGuid& UUID)
{
	StopActiveActionMontage();

	// Taken back before the record of it is dropped, so an actor handed straight from one entity to the
	// next cannot keep answering traces as the one it has stopped standing for. The incoming entity pushes
	// its own settings immediately afterwards.
	ClearAppliedTraceCollision();

	BoundEntity = Entity;
	EntityUUID = UUID;
	bBound = Entity.IsSet();
	bActionsRefused = false;

	// Everything remembered about the previous occupant is dropped in the SAME step the new one is taken.
	// A pooled actor outlives its occupants and a fresh entity's fragments are default-constructed, so its
	// action serial starts again from zero: a serial kept from the previous occupant reads as "already
	// played" and eats exactly one action per reuse.
	StartedActionSerial = 0;
	bHasStartedActionSerial = false;
	bHasAppliedTraceCollision = false;
	ResolvedAnimSet.Reset();
	ActionMesh.Reset();
	ShellOwner.Reset();
	ClearAnimationInputs();
	WatchServerValues();

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	ActionMesh = CrowdyMassNearTier::ChooseActionMesh(*Owner);
	ShellOwner = Cast<ACrowdyMassNearTierActor>(Owner);
}

const FTransform& UCrowdyMassNearTierComponent::CaptureAuthoredMeshTransform(const USkeletalMeshComponent& InMesh)
{
	// Captured ONCE, before anything here has written to the mesh, and never re-read afterwards.
	//
	// The obvious implementation reads the mesh's current relative transform each time. That is wrong in a
	// way nothing catches for one promotion: this function then writes its own answer back onto the mesh,
	// so the next bind reads a value that already includes the crowd's placement and composes it in again.
	// A pooled actor crossing the promotion boundary a few times sinks by the crowd's offset every time,
	// which presents as a character that drifts into the ground and disappears rather than as bad maths.
	//
	// Held per component rather than per bind, because the mesh's authored placement belongs to the actor
	// and outlives every entity that borrows it.
	if (!bHasAuthoredMeshTransform)
	{
		AuthoredMeshTransform = InMesh.GetRelativeTransform();
		bHasAuthoredMeshTransform = true;
	}

	return AuthoredMeshTransform;
}

void UCrowdyMassNearTierComponent::ApplyMeshTransformFromBoundEntity(const FMassEntityManager& EntityManager)
{
	const ACrowdyMassNearTierActor* Shell = ShellOwner.Get();
	USkeletalMeshComponent* ShellMesh = Shell ? Shell->Mesh.Get() : nullptr;
	if (!ShellMesh)
	{
		// Only the shell's own mesh is mirrored. A mesh a subclass added is that subclass's to place, and
		// overwriting it here would silently discard whatever it authored.
		return;
	}

	const FMassRepresentationFragment* Representation =
		EntityManager.GetFragmentDataPtr<FMassRepresentationFragment>(BoundEntity);
	const FMassRepresentationSubsystemSharedFragment* SharedFragment =
		EntityManager.GetSharedFragmentDataPtr<FMassRepresentationSubsystemSharedFragment>(BoundEntity);
	UMassRepresentationSubsystem* RepresentationSubsystem =
		SharedFragment ? SharedFragment->RepresentationSubsystem : nullptr;
	if (!Representation || !RepresentationSubsystem)
	{
		return;
	}

	const FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();
	const int32 MeshInfoIndex = Representation->SkinnedMeshDescHandle.ToIndex();
	if (!MeshInfos.IsValidIndex(MeshInfoIndex))
	{
		// An entity drawn through no skinned mesh description has no crowd placement to match, so whatever
		// the shell authored stands.
		return;
	}

	const FTransform Authored = CaptureAuthoredMeshTransform(*ShellMesh);
	const FTransform MeshTransform =
		CrowdyMassNearTier::ResolveShellMeshTransform(MeshInfos[MeshInfoIndex].GetDesc(), Authored);
	ShellMesh->SetRelativeTransform(MeshTransform);

	// Said with the numbers in it, and saying WHICH of the two answers this was. A character facing the
	// wrong way looks identical whether the crowd's placement was never carried across, was carried across
	// and is itself wrong, or was carried across over the top of a correction somebody typed in by hand.
	const FTransform FromCrowd = CrowdyMassNearTier::MeshTransformFromVisualizationDesc(MeshInfos[MeshInfoIndex].GetDesc());
	UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: '%s' placed its mesh at rotation %s, location %s. The crowd's own placement for this entity is rotation %s, location %s, and the shell adds rotation %s, location %s on top of it. If this actor faces differently from the crowd, that difference is what the shell should be authoring."),
		*GetNameSafe(GetOwner()),
		*MeshTransform.GetRotation().Rotator().ToCompactString(), *MeshTransform.GetLocation().ToCompactString(),
		*FromCrowd.GetRotation().Rotator().ToCompactString(), *FromCrowd.GetLocation().ToCompactString(),
		*Authored.GetRotation().Rotator().ToCompactString(), *Authored.GetLocation().ToCompactString());
}

void UCrowdyMassNearTierComponent::ClearAnimationInputs()
{
	SmoothedSpeed = 0.0f;
	WorldVelocity = FVector::ZeroVector;
	LocalVelocity = FVector::ZeroVector;
	GaitId = 0;
	GaitName = NAME_None;
	NamedGaitId = 0;
	PoseId = 0;
	PoseName = NAME_None;
	NamedPoseId = 0;
	ActionId = 0;
	ActionVariant = 0;
	ActionPlayRate = 1.0f;
	ActionSerial = 0;
	ActionElapsedSeconds = 0.0f;
}

void UCrowdyMassNearTierComponent::ResolveAnimSetForBoundEntity()
{
	ResolvedAnimSet.Reset();
	bActionsRefused = false;

	const UCrowdyMassEntitySubsystem* Subsystem = GetEntitySubsystem();
	const FMassEntityManager* EntityManager = GetEntityManager();
	if (!Subsystem || !EntityManager)
	{
		return;
	}

	// The set the ENTITY renders through, which is the only set its action ids are row positions in. Read
	// off the entity itself first: the shared fragment holds the asset the crowd is already drawing it
	// from, so this cannot disagree with what the row shows.
	const UCrowdyMassAnimSet* EntitySet = nullptr;
	if (const FCrowdyMassAnimSetSharedFragment* SharedAnimSet =
		EntityManager->GetConstSharedFragmentDataPtr<FCrowdyMassAnimSetSharedFragment>(BoundEntity))
	{
		EntitySet = SharedAnimSet->AnimSet.Get();
	}

	const UClass* EntityClass = Subsystem->GetEntityClass(EntityUUID);

	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::NoClass;
	if (!EntitySet)
	{
		EntitySet = Subsystem->FindRemoteEntityAnimSet(EntityClass, Source);
	}

	if (!EntitySet)
	{
		// Still bound, and still following the entity. An actor with no animation set has no clip to play
		// and no gait or pose names to report, but where it stands is not an animation question.
		const FObjectKey ReportKey(EntityClass);
		CrowdyMassNearTier::FReportedAnimSetFaults& Faults =
			CrowdyMassNearTier::ReportedAnimSetFaultsForWorld(GetWorld());
		if (!Faults.MissingAnimSetClasses.Contains(ReportKey))
		{
			Faults.MissingAnimSetClasses.Add(ReportKey);
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassNearTier]: '%s' has no animation set to drive it: %s. It will follow the entity and report no gait, no pose and play no action."),
				*GetPathNameSafe(EntityClass), CrowdyMassNearTier::DescribeAnimSetMiss(Source));
		}
		return;
	}

	ResolvedAnimSet = EntitySet;

	const AActor* Owner = GetOwner();
	const UClass* OwnerClass = Owner ? Owner->GetClass() : nullptr;
	const UCrowdyMassAnimSet* OwnerClassSet = OwnerClass ? Subsystem->GetRemoteEntityAnimSet(OwnerClass) : nullptr;
	if (!OwnerClassSet || OwnerClassSet == EntitySet)
	{
		return;
	}

	// A generic shell names no set of its own, which is the ordinary case and no disagreement at all. Two
	// DIFFERENT sets is a disagreement nothing here can settle: an action id is a row position, so the two
	// sets answer the same id with two different clips and neither is provably the one this mesh should
	// show. Motion and pose inputs keep working; only actions are refused, because a wrong clip is worse
	// than no clip and looks like a working one.
	bActionsRefused = true;

	const TPair<FObjectKey, FObjectKey> MismatchKey{FObjectKey(OwnerClass), FObjectKey(EntityClass)};
	CrowdyMassNearTier::FReportedAnimSetFaults& Faults =
		CrowdyMassNearTier::ReportedAnimSetFaultsForWorld(GetWorld());
	if (!Faults.AnimSetMismatches.Contains(MismatchKey))
	{
		Faults.AnimSetMismatches.Add(MismatchKey);
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassNearTier]: near tier actor class '%s' names animation set '%s', but the entity it is drawing (class '%s') renders through '%s'. An action id is a row in one set, so no action will be played for this pairing. Either clear the set on the actor class or give it the entity's set."),
			*GetPathNameSafe(OwnerClass), *GetPathNameSafe(OwnerClassSet), *GetPathNameSafe(EntityClass),
			*GetPathNameSafe(EntitySet));
	}
}

void UCrowdyMassNearTierComponent::TickComponent(const float DeltaTime, const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Which entity this actor stands for is re-read rather than assumed, because the pool hands one actor
	// to a succession of entities. An UNSET answer is left alone: a caller that bound explicitly has no
	// engine mapping to be corrected from, and treating "no mapping" as "wrong entity" would unbind it.
	if (const UWorld* World = GetWorld())
	{
		if (UMassActorSubsystem* ActorSubsystem = World->GetSubsystem<UMassActorSubsystem>())
		{
			const FMassEntityHandle Mapped = ActorSubsystem->GetEntityHandleFromActor(Owner);
			if (Mapped.IsSet() && (!bBound || Mapped != BoundEntity))
			{
				// Through the full resolve, so the handle and the UUID are taken in one step and the
				// caches belonging to the previous occupant are dropped with them.
				ResolveEntityFromOwner();
			}
		}
	}

	if (!bBound)
	{
		return;
	}

	const FMassEntityManager* EntityManager = GetEntityManager();
	if (!EntityManager || !EntityManager->IsEntityValid(BoundEntity))
	{
		// A handle carries a serial, so a slot reused by a different entity fails this rather than naming
		// a stranger.
		UnbindFromEntity();
		return;
	}

	if (bDriveOwnerTransform)
	{
		if (const FTransformFragment* TransformFragment = EntityManager->GetFragmentDataPtr<FTransformFragment>(BoundEntity))
		{
			ApplyTransformFromEntity(TransformFragment->GetTransform());
		}
	}

	// Not the frame this actor starts being drawn on, so an action already under way is left running where
	// it has got to and only a new one starts.
	UpdateAnimationStateFromEntity(*EntityManager, /*bIsBeingShownNow=*/false);
}

void UCrowdyMassNearTierComponent::UpdateAnimationStateFromEntity(const FMassEntityManager& EntityManager,
	const bool bIsBeingShownNow)
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const FMassVelocityFragment* VelocityFragment = EntityManager.GetFragmentDataPtr<FMassVelocityFragment>(BoundEntity);
	WorldVelocity = VelocityFragment ? VelocityFragment->Value : FVector::ZeroVector;
	LocalVelocity = Owner->GetActorTransform().InverseTransformVectorNoScale(WorldVelocity);

	const UCrowdyMassAnimSet* AnimSet = ResolvedAnimSet.Get();

	const FCrowdyMassAnimStateFragment* AnimState = EntityManager.GetFragmentDataPtr<FCrowdyMassAnimStateFragment>(BoundEntity);
	SmoothedSpeed = AnimState ? AnimState->SmoothedSpeed : 0.0f;

	// 0 is a real answer meaning no gait was resolved, and is passed on as it stands. Nudging it to row 1
	// would show the slowest authored gait for an entity whose set resolved nothing, which reads as a
	// working crowd standing oddly still rather than as the configuration fault it is.
	const int32 NewGaitId = AnimState ? static_cast<int32>(AnimState->Gait) : 0;
	if (NewGaitId != NamedGaitId)
	{
		GaitName = CrowdyMassNearTier::GaitNameFromSet(AnimSet, NewGaitId);
		NamedGaitId = NewGaitId;
	}
	GaitId = NewGaitId;

	const FCrowdyMassViewPoseFragment* ViewPose = EntityManager.GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(BoundEntity);
	const int32 NewPoseId = ViewPose ? static_cast<int32>(ViewPose->PoseId) : 0;
	if (NewPoseId != NamedPoseId)
	{
		PoseName = CrowdyMassNearTier::PoseNameFromSet(AnimSet, NewPoseId);
		NamedPoseId = NewPoseId;
	}
	PoseId = NewPoseId;

	ApplyTraceCollisionFromBoundEntity(EntityManager);

	const FCrowdyMassActionFragment* Action = EntityManager.GetFragmentDataPtr<FCrowdyMassActionFragment>(BoundEntity);
	if (!Action)
	{
		ActionId = 0;
		ActionVariant = 0;
		ActionPlayRate = 1.0f;
		ActionElapsedSeconds = 0.0f;
		return;
	}

	ActionId = Action->ActionId;
	ActionVariant = Action->Variant;
	// Clamped through the same function the fragment write and the crowd use, so an actor cannot be made
	// to play at a speed the crowd refuses.
	ActionPlayRate = CrowdyMassClampActionPlayRate(Action->PlayRate);
	ActionSerial = Action->Serial;
	ActionElapsedSeconds = Action->ElapsedSecondsSinceTrigger;

	if (!CrowdyMassNearTier::ShouldStartHeldAction(bIsBeingShownNow, bHasStartedActionSerial, StartedActionSerial,
		Action->Serial))
	{
		return;
	}

	// Recorded whether or not the clip plays, so a refusal is not retried on every frame the action lasts.
	StartedActionSerial = Action->Serial;
	bHasStartedActionSerial = true;

	PlayActionFromFragment(Action->ActionId, ActionPlayRate, Action->ElapsedSecondsSinceTrigger,
		Action->DurationSeconds);
}

void UCrowdyMassNearTierComponent::PrimeAnimationForDisplay()
{
	if (!bBound)
	{
		return;
	}

	const FMassEntityManager* EntityManager = GetEntityManager();
	if (!EntityManager || !EntityManager->IsEntityValid(BoundEntity))
	{
		return;
	}

	// Everything the graph reads, taken as of now rather than as of the first tick after this. The
	// promotion shows this actor during the Mass phase at the head of the frame, while this component
	// ticks after physics, so without this the first frame the character is on screen is drawn from
	// whatever the mesh was already holding: a bind pose on a fresh actor, the previous occupant's pose on
	// one out of the pool, and a locomotion graph with every input still at zero on both.
	UpdateAnimationStateFromEntity(*EntityManager, /*bIsBeingShownNow=*/true);

	ForceCurrentPose();
}

void UCrowdyMassNearTierComponent::ForceCurrentPose()
{
	USkeletalMeshComponent* Mesh = ActionMesh.Get();
	if (!::IsValid(Mesh) || !Mesh->GetAnimInstance())
	{
		return;
	}

	// Enabling a component's tick partway through a frame does not make it tick that frame, and a mesh
	// evaluates its pose in an earlier tick group than this component runs in, so the first frame an actor
	// is shown would otherwise draw a pose nothing had updated. Evaluated here instead, with a delta of
	// zero so that nothing advances: this shows what the graph and the montage hold at this instant, which
	// is what the crowd row being replaced is showing.
	Mesh->TickAnimation(0.0f, /*bNeedsValidRootMotion=*/false);
	Mesh->RefreshBoneTransforms();
}

void UCrowdyMassNearTierComponent::ApplyTransformFromEntity(const FTransform& EntityTransform)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// The shell owns the one conversion from an entity transform to its root, shared with the promotion's spawn and
	// teleport so the two cannot disagree. Any other owner class stands at the entity's own transform.
	const ACrowdyMassNearTierActor* Shell = ShellOwner.Get();
	const FTransform RootTransform = Shell ? Shell->MakeRootTransformFromEntity(EntityTransform) : EntityTransform;

	// Teleported, never swept. The actor is a picture of the entity and must never push back against it:
	// a sweep would let a wall stop the actor while the entity walks on, and the two representations of
	// one character would then disagree for as long as the promotion lasted.
	Owner->SetActorLocationAndRotation(RootTransform.GetLocation(), RootTransform.GetRotation(),
		/*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
}

void UCrowdyMassNearTierComponent::ApplyTraceCollisionFromBoundEntity(const FMassEntityManager& EntityManager)
{
	if (!bApplyTraceCollision)
	{
		return;
	}

	const FCrowdyMassTraceCollisionFragment* TraceCollision =
		EntityManager.GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>(BoundEntity);
	if (!TraceCollision)
	{
		return;
	}

	ApplyTraceCollisionFromFragment(TraceCollision->bEnableTraceCollision, TraceCollision->ObjectType,
		TraceCollision->ResponseToChannels);
}

void UCrowdyMassNearTierComponent::ApplyTraceCollisionFromFragment(const bool bEnable,
	const ECollisionChannel ObjectType, const FCollisionResponseContainer& Responses)
{
	// A change check rather than an unconditional write. Pushing the same settings every frame would walk
	// the owner's whole component list and re-register collision on each of them, for a set of values that
	// changes perhaps twice in an entity's life.
	if (bHasAppliedTraceCollision
		&& bAppliedTraceCollisionEnabled == bEnable
		&& AppliedTraceObjectType == ObjectType
		&& AppliedTraceResponses == Responses)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	bHasAppliedTraceCollision = true;
	bAppliedTraceCollisionEnabled = bEnable;
	AppliedTraceObjectType = ObjectType;
	AppliedTraceResponses = Responses;

	TInlineComponentArray<UPrimitiveComponent*> Primitives(Owner);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		if (!::IsValid(Primitive))
		{
			continue;
		}

		// Only the QUERY half is decided here. Whether the component also simulates or probes is the
		// owner's own business and is carried through untouched, so switching an entity's trace collision
		// off does not quietly disable its physics as well.
		//
		// Read off the body instance without the owner check, because that check reports NoCollision for
		// every component of an actor whose collision is switched off, whatever the component itself holds.
		// The write below goes to the body instance either way, so reading the masked value and writing it
		// back would strip the physics and probe bits from a pooled shell for good.
		const ECollisionEnabled::Type Current = Primitive->BodyInstance.GetCollisionEnabled(/*bCheckOwner=*/false);
		const ECollisionEnabled::Type Desired = CollisionEnabledFromFlags(bEnable,
			CollisionEnabledHasPhysics(Current), CollisionEnabledHasProbe(Current));
		if (Desired != Current)
		{
			Primitive->SetCollisionEnabled(Desired);
		}

		if (!bEnable)
		{
			continue;
		}

		Primitive->SetCollisionObjectType(ObjectType);
		Primitive->SetCollisionResponseToChannels(Responses);
	}
}

void UCrowdyMassNearTierComponent::ClearAppliedTraceCollision()
{
	if (!bHasAppliedTraceCollision)
	{
		return;
	}

	// An actor on its way out takes its collision with it, and its components are unregistering, so there
	// is nothing to take back and nobody left to observe it.
	const AActor* Owner = GetOwner();
	if (!Owner || Owner->IsActorBeingDestroyed())
	{
		return;
	}

	// The exact inverse of the push, through the same walk over the same components: the query bit comes
	// back off and the physics and probe bits are left as they were found. Symmetrical on purpose, so an
	// actor that has stopped standing for an entity stops answering as that entity in the same step it
	// stops following it. The object type and responses are left where they are because a component with
	// no query collision is not asked about either, and the next occupant overwrites them on its bind.
	ApplyTraceCollisionFromFragment(/*bEnable=*/false, AppliedTraceObjectType, AppliedTraceResponses);
}

void UCrowdyMassNearTierComponent::PlayActionFromFragment(const int32 InActionId, const float InPlayRate,
	const float ElapsedSecondsSinceTrigger, const float DurationSeconds)
{
	// Every refusal below says so under the trace. An action that arrives and plays nothing is invisible
	// from the outside and identical to one that never arrived, so silence here sends a reader to the
	// transport, which is the one place the fault cannot be.
	if (InActionId <= 0 || bActionsRefused)
	{
		// An id of 0 on its own is the ordinary empty slot, which every entity holds until something
		// triggers an action and which is read once on the first tick after each bind. Saying so would put
		// a line in the log per promotion for the normal case and bury the refusal that matters.
		UE_CLOG(bActionsRefused && CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
			TEXT("[CrowdyMassNearTier]: '%s' played no action %d: actions are refused for this pairing, because the shell's class names a different animation set from the entity's."),
			*GetNameSafe(GetOwner()), InActionId);
		return;
	}

	const UCrowdyMassAnimSet* AnimSet = ResolvedAnimSet.Get();
	USkeletalMeshComponent* Mesh = ActionMesh.Get();
	if (!AnimSet || !::IsValid(Mesh))
	{
		UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
			TEXT("[CrowdyMassNearTier]: '%s' played no action %d: %s."),
			*GetNameSafe(GetOwner()), InActionId,
			!AnimSet
				? TEXT("no animation set resolved for the entity it stands for")
				: TEXT("it carries no skeletal mesh with a resolved asset to play one on"));
		return;
	}

	// Where in the clip to start, worked out by exactly the function the crowd uses for the same action:
	// the seconds since the trigger, clamped to the action's authored duration. An action already running
	// when this actor was promoted therefore picks up where the row had got to, instead of restarting.
	//
	// Deliberately NOT divided by the play rate. The crowd seeks and expires on unscaled seconds in both
	// directions, so dividing here would make this actor physically correct and put it at a different
	// frame from the crowd for every rate other than 1. One clamp, one clock, both representations.
	const float StartSecondsIntoAction = GetCrowdyMassActionCorrectedPosition(ElapsedSecondsSinceTrigger,
		DurationSeconds);

	// And no blend at all when that start is inside an action already under way, since there is no pose to
	// ease from that this character was ever in.
	const float BlendInSeconds = CrowdyMassNearTier::ActionBlendInForSeek(ActionBlendInSeconds,
		StartSecondsIntoAction);

	ActiveActionMontage = UCrowdyMassActionLibrary::PlayCrowdyMassActionFromSet(Mesh, AnimSet, InActionId,
		ActionVariant, InPlayRate, ActionSlotName, BlendInSeconds, ActionBlendOutSeconds,
		StartSecondsIntoAction);

	// The blend is in the line because it is the only place the choice above is visible from outside: the
	// two branches differ by a tenth of a second of animation and by nothing else.
	UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
		TEXT("[CrowdyMassNearTier]: '%s' started action %d (variant %d, rate %.2f) %.3fs in, blending in over %.3fs; montage '%s'."),
		*GetNameSafe(GetOwner()), InActionId, ActionVariant, InPlayRate, StartSecondsIntoAction,
		BlendInSeconds, *GetNameSafe(ActiveActionMontage.Get()));
}

void UCrowdyMassNearTierComponent::StopActiveActionMontage()
{
	UAnimMontage* Montage = ActiveActionMontage.Get();
	ActiveActionMontage.Reset();
	if (!Montage)
	{
		return;
	}

	USkeletalMeshComponent* Mesh = ActionMesh.Get();
	UAnimInstance* AnimInstance = ::IsValid(Mesh) ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance || !AnimInstance->Montage_IsPlaying(Montage))
	{
		return;
	}

	// No blend out. This runs as the actor stops standing for this entity, so there is nothing left to
	// blend into, and a swing still blending would be visible on whichever entity the pool hands the actor
	// to next.
	AnimInstance->Montage_Stop(0.0f, Montage);
}
