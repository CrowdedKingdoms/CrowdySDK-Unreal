#include "Mass/CrowdyMassActionLibrary.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "CrowdyMassLog.h"
#include "Derivation/CrowdyMassAnimSetDerivation.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/ObjectKey.h"

namespace
{
	// One action id on one class, already reported as unplayable on an actor. The cause is an authoring gap
	// that lasts as long as the anim set does, and the event that hits it repeats, so it is said once
	// rather than once per swing. Keyed on the class as well as the id, because an id that names no clip
	// for one class may name one perfectly well for another, and reporting the first would silence the
	// second. Process-wide rather than per world: what it is protecting is a log, and an id unauthored in
	// one world is unauthored in the next one loaded from the same content.
	TSet<TPair<FObjectKey, int32>> GReportedUnplayableActionIds;

	/**
	 * The animation set an actor's own class renders through.
	 *
	 * The class is the point of this, not the world: an action id is a row position in one set, so an actor
	 * of one class resolving against another class's set plays whatever that other class authored at the
	 * same row, which is a wrong clip rather than a missing one and nothing anywhere reports it.
	 */
	const UCrowdyMassAnimSet* FindAnimSetForActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		const UWorld* World = Actor->GetWorld();
		const UCrowdyMassEntitySubsystem* Subsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
		if (Subsystem)
		{
			return Subsystem->GetRemoteEntityAnimSet(Actor->GetClass());
		}

		// Maps whose profile does not select the Mass backend have no subsystem, and the class's own component still names the set.
		return FCrowdyMassAnimSetDerivation::FromClass(Actor->GetClass()).AnimSet.LoadSynchronous();
	}

	/**
	 * Starts an already-resolved action clip on a mesh, which is the half both entry points share once
	 * the argument about WHICH clip is settled.
	 *
	 * Played as a dynamic montage into SlotName rather than through PlayAnimation, which would put the
	 * component into single-node mode and throw away the animation blueprint for good.
	 */
	UAnimMontage* PlayResolvedActionSequence(USkeletalMeshComponent* Mesh, UAnimSequence* Sequence,
		const int32 ActionId, const float PlayRate, const FName SlotName, const float BlendInSeconds,
		const float BlendOutSeconds, const float StartSecondsIntoAction)
	{
		UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
		if (!AnimInstance)
		{
			// Ordinary rather than a fault: a mesh with no animation blueprint has nothing to play a
			// montage on. Said at Verbose because the body calling this may run on any number of
			// representations and only some of them animate.
			UE_LOG(LogCrowdyMass, Verbose,
				TEXT("[CrowdyMassActionLibrary]: '%s' has no animation instance, so action %d was not played."),
				*Mesh->GetName(), ActionId);
			return nullptr;
		}

		// The same clamp the fragment write applies, through the same function, so a forged rate cannot
		// make an actor play at a speed the crowd refuses.
		const float SafeRate = CrowdyMassClampActionPlayRate(PlayRate);

		// LoopCount 1 is the actor's spelling of the Clamp loop mode the crowd composes for an action:
		// play once. A montage blends itself out at the end, which is what the crowd's hold-last-frame
		// becomes on a representation that has a graph to blend back into.
		//
		// BlendOutTriggerTime of -1 keeps the engine's own default, which is to start blending out one
		// blend length before the clip ends.
		UAnimMontage* Played = AnimInstance->PlaySlotAnimationAsDynamicMontage(
			Sequence, SlotName, BlendInSeconds, BlendOutSeconds, SafeRate, /*LoopCount=*/1,
			/*BlendOutTriggerTime=*/-1.0f, FMath::Max(StartSecondsIntoAction, 0.0f));

		if (!Played)
		{
			// The one failure worth a line here: the graph has no slot by that name, so nothing plays and
			// nothing else would say so.
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassActionLibrary]: '%s' refused action %d; its animation graph has no '%s' slot node."),
				*Mesh->GetName(), ActionId, *SlotName.ToString());
		}

		return Played;
	}
}

UAnimSequence* UCrowdyMassActionLibrary::FindCrowdyMassActionSequence(const AActor* Actor, const int32 ActionId)
{
	const UCrowdyMassAnimSet* AnimSet = FindAnimSetForActor(Actor);
	return AnimSet ? AnimSet->FindActionSequence(ActionId) : nullptr;
}

UAnimMontage* UCrowdyMassActionLibrary::PlayCrowdyMassActionFromSet(USkeletalMeshComponent* Mesh,
	const UCrowdyMassAnimSet* AnimSet, const int32 ActionId, const int32 Variant, const float PlayRate,
	const FName SlotName, const float BlendInSeconds, const float BlendOutSeconds,
	const float StartSecondsIntoAction)
{
	if (!::IsValid(Mesh) || !AnimSet)
	{
		return nullptr;
	}

	// Zero is the "nothing playing" value on the fragment and a negative id is nonsense, so neither names
	// an action here either.
	if (ActionId <= 0)
	{
		return nullptr;
	}

	UAnimSequence* Sequence = AnimSet->FindActionSequence(ActionId);
	if (!Sequence)
	{
		// Keyed on the set rather than on a class, because the set is what the id is a row in and is the
		// only thing a caller reaching here has to fix.
		const TPair<FObjectKey, int32> ReportKey(FObjectKey(AnimSet), ActionId);
		if (!GReportedUnplayableActionIds.Contains(ReportKey))
		{
			GReportedUnplayableActionIds.Add(ReportKey);
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassActionLibrary]: action %d has no sequence an actor can play in animation set '%s'. Row %d of that set authored no Sequence; the crowd may still render it from its slot pair. Author the row's Sequence field so both representations play the same clip."),
				ActionId, *AnimSet->GetPathName(), ActionId);
		}
		return nullptr;
	}

	return PlayResolvedActionSequence(Mesh, Sequence, ActionId, PlayRate, SlotName, BlendInSeconds,
		BlendOutSeconds, StartSecondsIntoAction);
}

bool UCrowdyMassActionLibrary::PlayCrowdyMassAction(USkeletalMeshComponent* Mesh, const int32 ActionId,
	const int32 Variant, const float PlayRate, const FName SlotName, const float BlendInSeconds,
	const float BlendOutSeconds, const float StartSecondsIntoAction)
{
	if (!::IsValid(Mesh))
	{
		return false;
	}

	// Zero is the "nothing playing" value on the fragment and a negative id is nonsense, so neither names
	// an action here either. Refused before anything is reported, because an id of zero is an ordinary
	// value for a body to be handed rather than an authoring gap worth a line.
	if (ActionId <= 0)
	{
		return false;
	}

	// The owning actor is what names the class, and a mesh with no owner names none. Refused rather than
	// falling back to some other class's set: an id resolved against the wrong set is a wrong clip, which
	// is worse than no clip and looks like neither.
	const AActor* Owner = Mesh->GetOwner();
	if (!Owner)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassActionLibrary]: '%s' has no owning actor, so there is no class whose animation set action %d could be resolved against."),
			*Mesh->GetName(), ActionId);
		return false;
	}

	const UCrowdyMassAnimSet* AnimSet = FindAnimSetForActor(Owner);
	UAnimSequence* Sequence = AnimSet ? AnimSet->FindActionSequence(ActionId) : nullptr;
	if (!Sequence)
	{
		const TPair<FObjectKey, int32> ReportKey(FObjectKey(Owner->GetClass()), ActionId);
		if (GReportedUnplayableActionIds.Contains(ReportKey))
		{
			return false;
		}

		GReportedUnplayableActionIds.Add(ReportKey);
		if (!AnimSet)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassActionLibrary]: action %d was not played because no animation set resolved for '%s'. Its Crowdy Mass Anim Set component names no set, the class has no such component, or the named set did not load."),
				ActionId, *Owner->GetClass()->GetPathName());
			return false;
		}

		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassActionLibrary]: action %d has no sequence an actor of '%s' can play. Row %d of animation set '%s' authored no Sequence; the crowd may still render it from its slot pair. Author the row's Sequence field so both representations play the same clip."),
			ActionId, *Owner->GetClass()->GetPathName(), ActionId, *AnimSet->GetPathName());
		return false;
	}

	return PlayResolvedActionSequence(Mesh, Sequence, ActionId, PlayRate, SlotName, BlendInSeconds,
		BlendOutSeconds, StartSecondsIntoAction) != nullptr;
}
