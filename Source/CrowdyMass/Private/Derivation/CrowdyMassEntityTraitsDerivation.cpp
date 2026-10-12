#include "Derivation/CrowdyMassEntityTraitsDerivation.h"

#include "Components/CrowdyMassEntityTraitsComponent.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassVisualizationTrait.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"

const TCHAR* LexToString(const ECrowdyMassEntityTraitsRoute Route)
{
	switch (Route)
	{
	case ECrowdyMassEntityTraitsRoute::NativeComponent:
		return TEXT("a component declared in C++");
	case ECrowdyMassEntityTraitsRoute::BlueprintComponent:
		return TEXT("a component added in the Blueprint");
	case ECrowdyMassEntityTraitsRoute::InheritedOverride:
		return TEXT("an inherited component this Blueprint overrode");
	default:
		return TEXT("nothing");
	}
}

ECrowdyMassEntityTraitsRoute RouteOfClassDefaultComponent(const UActorComponent& Component, const UObject* DefaultObject)
{
	// A native component belongs to the class default object. A template does not, and only the override handler builds
	// one with the inheritable-template flag, which is what separates an override from a plain Blueprint component.
	if (Component.GetOuter() == DefaultObject)
	{
		return ECrowdyMassEntityTraitsRoute::NativeComponent;
	}
	return Component.HasAnyFlags(RF_InheritableComponentTemplate)
		? ECrowdyMassEntityTraitsRoute::InheritedOverride
		: ECrowdyMassEntityTraitsRoute::BlueprintComponent;
}

FCrowdyMassDerivedEntityTraits FCrowdyMassEntityTraitsDerivation::FromClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedEntityTraits Result;

	if (!ActorClass)
	{
		Result.Reason = TEXT("No class was given, so there is nothing to take traits from.");
		return Result;
	}

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' is not an Actor class, so it has no components to take traits from."),
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
	// This one call covers all three, and among the Blueprint routes it asks the most derived first, so a
	// child that changed its parent's list is the answer. Walking those by hand gets that precedence
	// backwards: the parent's construction script template is found first and the child's override never
	// runs.
	//
	// It does NOT order the routes against each other: every native component on the class default object is
	// handed back before any Blueprint one, however deep the class that declares it. That is why the visitor
	// below walks past a component that lists nothing instead of letting the first one seen answer.
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, UCrowdyMassEntityTraitsComponent::StaticClass(),
		[&Result, DefaultObject](const UActorComponent* Component)
		{
			const UCrowdyMassEntityTraitsComponent* TraitsComponent = Cast<UCrowdyMassEntityTraitsComponent>(Component);
			if (!TraitsComponent)
			{
				return true;
			}

			// Recorded even when the component lists nothing, because "you added the component and left it
			// empty" and "you have no such component at all" need different advice.
			Result.bSawComponent = true;

			// The first component that LISTS SOMETHING answers, and a component listing nothing is walked
			// past rather than taken. Two filled-in lists have no order to be merged in, so taking one and
			// counting the rest is the only rule an author can predict; counted rather than ignored, because
			// a list that simply stopped taking effect when a second component appeared would have no
			// symptom at all.
			//
			// Walking past an empty one matters because the components do not arrive most-derived first: the
			// engine hands back the class default object's native components before any added in a
			// Blueprint. A C++ base that creates this component to expose the slot would otherwise answer
			// with its empty list and shadow the child Blueprint that filled it in.
			const bool bAlreadyAnswered = Result.Traits.Num() > 0;

			int32 ListedHere = 0;
			for (const TObjectPtr<UMassEntityTraitBase>& Trait : TraitsComponent->Traits)
			{
				if (!Trait)
				{
					++Result.EmptyEntryCount;
					continue;
				}

				++ListedHere;
				if (!bAlreadyAnswered)
				{
					Result.Traits.Add(Trait.Get());
				}
			}

			if (bAlreadyAnswered)
			{
				Result.ShadowedComponentCount += ListedHere > 0 ? 1 : 0;
				return true;
			}

			// Recorded for whichever component is still the candidate. A component listing nothing does not
			// claim the answer, so one that lists something later overwrites this and the name reported is
			// always the name of the component whose traits were taken.
			Result.ComponentName = TraitsComponent->GetName();

			const UObject* Owner = TraitsComponent->GetOuter();
			Result.RouteOwnerName = Owner ? Owner->GetName() : FString();

			// The call above hands back components from all three routes without saying which, so they are
			// told apart by where each one lives.
			Result.Route = RouteOfClassDefaultComponent(*TraitsComponent, DefaultObject);

			// Deliberately keeps walking rather than stopping here, so a class carrying a second such
			// component is still counted above and can be told about it.
			return true;
		});

	if (Result.IsValid())
	{
		return Result;
	}

	if (!Result.bSawComponent)
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' has no Crowdy Mass Entity Traits component, so nothing of its own is added to the config worked out for it."),
			*ActorClass->GetPathName());
		return Result;
	}

	Result.Reason = Result.EmptyEntryCount > 0
		? FString::Printf(
			TEXT("'%s' has a Crowdy Mass Entity Traits component whose %d listed trait(s) are all set to none, so it adds nothing. Pick a trait class on each row, or remove the rows."),
			*ActorClass->GetPathName(), Result.EmptyEntryCount)
		: FString::Printf(
			TEXT("'%s' has a Crowdy Mass Entity Traits component with an empty list, so it adds nothing. Add a trait to it, or remove the component."),
			*ActorClass->GetPathName());

	return Result;
}

bool FCrowdyMassEntityTraitsDerivation::CanAppend(const UMassEntityConfigAsset& Config,
	const UMassEntityTraitBase& Declared, FString& OutRefusal, FString& OutOverridden)
{
	OutRefusal.Reset();
	OutOverridden.Reset();

	const bool bDeclaredDrawsTheEntity = Declared.IsA(UMassVisualizationTrait::StaticClass());

	for (const UMassEntityTraitBase* Existing : Config.GetConfig().GetTraits())
	{
		if (!Existing)
		{
			continue;
		}

		// Two traits that never build in the same net mode are not in each other's way at all, and Mass keeps
		// both on purpose. Every question below is about traits that DO build together.
		if (!static_cast<bool>(Existing->ValidTargetConfig & Declared.ValidTargetConfig))
		{
			continue;
		}

		// The ordinary way to change something worked out for a class: list a trait of the same class and
		// Mass keeps the last one offered. Recorded rather than refused, and reported by the caller, because
		// the engine's own line about it names the trait OBJECT and not the class it replaced or the actor
		// class that asked for it, which makes it impossible to trace back to anything an author wrote.
		if (Existing->GetClass() == Declared.GetClass())
		{
			OutOverridden = Existing->GetClass()->GetName();
			continue;
		}

		// A DIFFERENT visualization trait replaces nothing, so both build. Only one of them can decide how an
		// entity is drawn: the parameters they both write are a const shared fragment, the template keeps the
		// first value offered and discards the rest, and nothing but an ensure says so, which is silent in a
		// shipped game. The config would name a mesh and level table that never take effect.
		if (bDeclaredDrawsTheEntity && Existing->IsA(UMassVisualizationTrait::StaticClass()))
		{
			OutOverridden.Reset();
			OutRefusal = FString::Printf(
				TEXT("'%s' is a second visualization trait beside '%s' rather than a replacement for it, so both would build and which one decides how an entity is drawn is not something this can answer"),
				*Declared.GetClass()->GetName(), *Existing->GetClass()->GetName());
			return false;
		}
	}

	return true;
}
