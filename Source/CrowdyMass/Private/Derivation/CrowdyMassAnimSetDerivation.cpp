#include "Derivation/CrowdyMassAnimSetDerivation.h"

#include "Components/CrowdyMassAnimSetComponent.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "Templates/SubclassOf.h"
#include "Traits/CrowdyMassAnimationTrait.h"
#include "UObject/Class.h"

const TCHAR* LexToString(const ECrowdyMassAnimSetRoute Route)
{
	switch (Route)
	{
	case ECrowdyMassAnimSetRoute::NativeComponent:
		return TEXT("a component declared in C++");
	case ECrowdyMassAnimSetRoute::BlueprintComponent:
		return TEXT("a component added in the Blueprint");
	case ECrowdyMassAnimSetRoute::InheritedOverride:
		return TEXT("an inherited component this Blueprint overrode");
	default:
		return TEXT("nothing");
	}
}

FCrowdyMassDerivedAnimSet FCrowdyMassAnimSetDerivation::FromClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedAnimSet Result;

	if (!ActorClass)
	{
		Result.Reason = TEXT("No class was given, so there is nothing to take an animation set from.");
		return Result;
	}

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' is not an Actor class, so it has no components to take an animation set from."),
			*ActorClass->GetPathName());
		return Result;
	}

	// Nothing here may bring a default object into existence. A class can still be loading or compiling when
	// this runs, and constructing its default object early runs the class constructor at the wrong moment;
	// a class that has not finished forming may not have a constructor to run at all, which asserts rather
	// than failing. The walk below reads the default object and WOULD build one, so the only safe order is
	// to ask whether it already exists and refuse before reaching it.
	const UObject* DefaultObject = ActorClass->GetDefaultObject(/*bCreateIfNeeded=*/false);
	if (!DefaultObject)
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' has no class default object yet, so it is still loading or is not a fully formed class. Ask again once it has finished loading."),
			*ActorClass->GetPathName());
		return Result;
	}

	// A component reaches an actor by three routes and none of them sees the other two: a native component,
	// which lives on the class default object; a component added in a Blueprint, which exists only as a
	// template in that Blueprint's construction script; and an inherited component whose defaults a child
	// Blueprint changed, which is kept in neither of those places.
	//
	// This one call covers all three, and for the third it asks the most derived Blueprint first, so a child
	// that overrode its parent's set is the answer. Walking the three by hand gets that precedence backwards:
	// the parent's construction script template is found first and the child's override never runs.
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, UCrowdyMassAnimSetComponent::StaticClass(),
		[&Result, DefaultObject](const UActorComponent* Component)
		{
			const UCrowdyMassAnimSetComponent* AnimSetComponent = Cast<UCrowdyMassAnimSetComponent>(Component);
			if (!AnimSetComponent)
			{
				return true;
			}

			// Recorded even when the component names nothing, because "you added the component and left it
			// empty" and "you have no such component at all" need different advice.
			Result.bSawComponent = true;

			if (AnimSetComponent->AnimSet.IsNull())
			{
				// Keep looking: a class can carry several of these and only one of them be filled in.
				return true;
			}

			Result.AnimSet = AnimSetComponent->AnimSet;
			Result.ComponentName = AnimSetComponent->GetName();

			const UObject* Owner = AnimSetComponent->GetOuter();
			Result.RouteOwnerName = Owner ? Owner->GetName() : FString();

			// The call above hands back components from all three routes without saying which, so they are
			// told apart by where each one lives. A native component belongs to the class default object.
			// A template does not, and the override handler is the only thing that builds one with the
			// inheritable-template flag, which is what separates an override from a plain Blueprint
			// component: both are outered to a Blueprint class and are otherwise indistinguishable.
			if (Owner == DefaultObject)
			{
				Result.Route = ECrowdyMassAnimSetRoute::NativeComponent;
				return false;
			}

			Result.Route = AnimSetComponent->HasAnyFlags(RF_InheritableComponentTemplate)
				? ECrowdyMassAnimSetRoute::InheritedOverride
				: ECrowdyMassAnimSetRoute::BlueprintComponent;

			return false;
		});

	if (Result.IsValid())
	{
		return Result;
	}

	Result.Reason = Result.bSawComponent
		? FString::Printf(
			TEXT("'%s' has a Crowdy Mass Anim Set component, but no animation set is assigned to it. Assign one on the component's defaults."),
			*ActorClass->GetPathName())
		: FString::Printf(
			TEXT("'%s' has no Crowdy Mass Anim Set component. Add one and assign an animation set, or author a Mass entity config for the class with a Crowdy Mass Animation trait."),
			*ActorClass->GetPathName());

	return Result;
}

FCrowdyMassConfigAnimSet FCrowdyMassAnimSetDerivation::FromConfig(const UMassEntityConfigAsset* Config)
{
	FCrowdyMassConfigAnimSet Result;

	if (!Config)
	{
		return Result;
	}

	for (const UMassEntityTraitBase* Trait : Config->GetConfig().GetTraits())
	{
		const UCrowdyMassAnimationTrait* AnimationTrait = Cast<UCrowdyMassAnimationTrait>(Trait);
		if (!AnimationTrait)
		{
			continue;
		}

		Result.bSawAnimationTrait = true;
		Result.AnimSet = AnimationTrait->AnimSet;
		return Result;
	}

	return Result;
}
