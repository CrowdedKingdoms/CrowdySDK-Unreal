#include "Derivation/CrowdyMassNearTierDerivation.h"

#include "Components/CrowdyMassNearTierComponent.h"
#include "Components/CrowdyMassNearTierShellComponent.h"
#include "GameFramework/Actor.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"

const TCHAR* LexToString(const ECrowdyMassNearTierRoute Route)
{
	switch (Route)
	{
	case ECrowdyMassNearTierRoute::NativeComponent:
		return TEXT("a component declared in C++");
	case ECrowdyMassNearTierRoute::BlueprintComponent:
		return TEXT("a component added in the Blueprint");
	case ECrowdyMassNearTierRoute::InheritedOverride:
		return TEXT("an inherited component this Blueprint overrode");
	default:
		return TEXT("nothing");
	}
}

FCrowdyMassDerivedNearTierShell FCrowdyMassNearTierDerivation::FromClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedNearTierShell Result;

	if (!ActorClass)
	{
		Result.Reason = TEXT("No class was given, so there is nothing to take a near tier shell from.");
		return Result;
	}

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' is not an Actor class, so it has no components to take a near tier shell from."),
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
	// that overrode its parent's shell is the answer. Walking the three by hand gets that precedence
	// backwards: the parent's construction script template is found first and the child's override never
	// runs.
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, UCrowdyMassNearTierShellComponent::StaticClass(),
		[&Result, DefaultObject](const UActorComponent* Component)
		{
			const UCrowdyMassNearTierShellComponent* ShellComponent = Cast<UCrowdyMassNearTierShellComponent>(Component);
			if (!ShellComponent)
			{
				return true;
			}

			// Recorded even when the component names nothing, because "you added the component and left it
			// empty" and "you have no such component at all" need different advice.
			Result.bSawComponent = true;

			if (ShellComponent->NearTierActorClass.IsNull())
			{
				// Keep looking: a class can carry several of these and only one of them be filled in.
				return true;
			}

			Result.NearTierActorClass = ShellComponent->NearTierActorClass;
			Result.ComponentName = ShellComponent->GetName();

			const UObject* Owner = ShellComponent->GetOuter();
			Result.RouteOwnerName = Owner ? Owner->GetName() : FString();

			// The call above hands back components from all three routes without saying which, so they are
			// told apart by where each one lives. A native component belongs to the class default object.
			// A template does not, and the override handler is the only thing that builds one with the
			// inheritable-template flag, which is what separates an override from a plain Blueprint
			// component: both are outered to a Blueprint class and are otherwise indistinguishable.
			if (Owner == DefaultObject)
			{
				Result.Route = ECrowdyMassNearTierRoute::NativeComponent;
				return false;
			}

			Result.Route = ShellComponent->HasAnyFlags(RF_InheritableComponentTemplate)
				? ECrowdyMassNearTierRoute::InheritedOverride
				: ECrowdyMassNearTierRoute::BlueprintComponent;

			return false;
		});

	if (Result.IsValid())
	{
		return Result;
	}

	Result.Reason = Result.bSawComponent
		? FString::Printf(
			TEXT("'%s' has a Crowdy Mass Near Tier Shell component, but no actor class is assigned to it. Assign one on the component's defaults."),
			*ActorClass->GetPathName())
		: FString::Printf(
			TEXT("'%s' has no Crowdy Mass Near Tier Shell component, so its entities are drawn as crowd rows at every distance. Add one and assign an actor class to have them promoted to actors at close range."),
			*ActorClass->GetPathName());

	return Result;
}

UClass* FCrowdyMassNearTierDerivation::LoadUsableShellClass(const UClass* EntityClass,
	const FCrowdyMassDerivedNearTierShell& Declared, FString& OutRefusal, bool& bOutAskAgainLater)
{
	OutRefusal.Reset();
	bOutAskAgainLater = false;

	const FString EntityClassName = EntityClass ? EntityClass->GetPathName() : FString(TEXT("no class"));

	// Declaring nothing is the ordinary case and not a fault, so it leaves the refusal empty: a class with
	// no shell is drawn as crowd rows at every distance, which is what every class did before promotion
	// existed. Carrying the component and leaving its slot empty is NOT that case, and until this was
	// separated out it was silent on both config paths: the walk composed the two messages and nothing in
	// production ever read either one, so the one authoring mistake that looks exactly like "I never asked
	// for a near tier" was the one nobody was told about.
	if (!Declared.IsValid())
	{
		OutRefusal = Declared.bSawComponent ? Declared.Reason : FString();

		// The walk refuses a class with no default object rather than building one, and says so in its
		// reason. That is a class still loading, not a class that declared nothing, and an answer memoised
		// for it would outlive the moment it was true.
		bOutAskAgainLater = EntityClass && EntityClass->GetDefaultObject(/*bCreateIfNeeded=*/false) == nullptr;
		return nullptr;
	}

	// Loaded here rather than in the walk that read the declaration, because this is the one place that can
	// tell a class that named nothing apart from a class that named something no longer there, and those
	// need different advice.
	UClass* LoadedShellClass = Declared.NearTierActorClass.LoadSynchronous();
	if (!LoadedShellClass || !LoadedShellClass->IsChildOf(AActor::StaticClass()))
	{
		OutRefusal = FString::Printf(
			TEXT("'%s' names near tier actor class '%s' on %s, but it is not an Actor class that could be loaded, so its entities stay crowd rows at every distance. Assign an Actor class that still exists."),
			*EntityClassName, *Declared.NearTierActorClass.ToString(), LexToString(Declared.Route));
		return nullptr;
	}

	// A class that loaded but has not finished forming has no default object to read components off, so it
	// answers the same false as a class carrying no component. Told apart before the message is composed,
	// because the advice differs completely: one is a component to add, the other is nothing to fix.
	if (!LoadedShellClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
	{
		bOutAskAgainLater = true;
		OutRefusal = FString::Printf(
			TEXT("'%s' names near tier actor class '%s' on %s, and that class has no class default object yet, so it is still loading or is not a fully formed class and cannot be checked. Its entities stay crowd rows until something asks again."),
			*EntityClassName, *LoadedShellClass->GetPathName(), LexToString(Declared.Route));
		return nullptr;
	}

	// And being an Actor class is not enough to be promoted TO. Everything a promoted entity does is done
	// by its near tier component, so a class without one is placed once and then stands still while its
	// entity walks away, no longer drawn as a crowd row and no longer hittable. Refused here, because from
	// the outside that is indistinguishable from a promotion that works.
	if (!CanHostNearTierView(LoadedShellClass))
	{
		OutRefusal = FString::Printf(
			TEXT("'%s' names near tier actor class '%s' on %s, but that class carries no Crowdy Mass Near Tier component, so nothing would drive it from the entity: it would be placed once and then stand still while its entity walked away, drawn by neither the crowd nor itself. Its entities stay crowd rows at every distance. Use a Crowdy Mass Near Tier Actor, or add a Crowdy Mass Near Tier component to '%s'."),
			*EntityClassName, *LoadedShellClass->GetPathName(), LexToString(Declared.Route),
			*LoadedShellClass->GetPathName());
		return nullptr;
	}

	return LoadedShellClass;
}

bool FCrowdyMassNearTierDerivation::CanHostNearTierView(const UClass* ShellActorClass)
{
	if (!ShellActorClass || !ShellActorClass->IsChildOf(AActor::StaticClass()))
	{
		return false;
	}

	// Same rule as the walk above: nothing here may bring a class default object into existence, because a
	// class that has not finished forming may have no constructor to run and asserts rather than failing.
	if (!ShellActorClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
	{
		return false;
	}

	bool bFound = false;
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ShellActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, UCrowdyMassNearTierComponent::StaticClass(),
		[&bFound](const UActorComponent* Component)
		{
			bFound = Component != nullptr;

			// Stop at the first one found; keep looking otherwise, since the three routes a component
			// reaches an actor by are reported in no particular order.
			return !bFound;
		});

	return bFound;
}
