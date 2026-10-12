#include "Dev/CrowdyMassDispatchProxyProbe.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "CrowdyMassLog.h"
#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Package.h"

#if !UE_BUILD_SHIPPING

namespace
{
	// Somewhere no class's own default could plausibly be, so a read-back at this position proves the
	// projection landed rather than matching a value that was already there.
	const FVector ProxyProbeProjectedLocation(1234.0, -5678.0, 91011.0);

	constexpr int32 ProxyProbeDirtyIntValue = 0x5A5A5A;

	// The production construction, not a copy of it. A probe that built its proxy its own way could report
	// a shape the shipped dispatch does not have.
	AActor* ConstructProxy(UWorld* World, UClass* ActorClass)
	{
		return CrowdyMassDispatchProxy::Construct(World, ActorClass);
	}

	void ReadComponents(const AActor* Actor, TArray<FCrowdyMassProxyComponentReading>& OutReadings)
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component)
			{
				continue;
			}

			FCrowdyMassProxyComponentReading Reading;
			Reading.Name = Component->GetFName();
			Reading.ClassName = Component->GetClass()->GetFName();
			Reading.bRegistered = Component->IsRegistered();

			if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
			{
				Reading.bHasAnimInstance = Skeletal->GetAnimInstance() != nullptr;
			}

			OutReadings.Add(MoveTemp(Reading));
		}
	}

	// Direct references only. A class holding its components in an array or a map would have those clobbered
	// by a wholesale reset too, and this would not see it, so a false here is conclusive and a true is only
	// evidence about the references it can read.
	bool OwnsEveryComponentItPointsAt(const AActor* Actor)
	{
		for (const FProperty* Property = Actor->GetClass()->PropertyLink; Property;
			Property = Property->PropertyLinkNext)
		{
			const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property);
			if (!AsObject)
			{
				continue;
			}

			const UObject* Value = AsObject->GetObjectPropertyValue_InContainer(Actor);
			if (Value && Value->IsA<UActorComponent>() && !Value->IsIn(Actor))
			{
				return false;
			}
		}

		return true;
	}

	// The reset the design describes, written the obvious way: every property copied back from the class
	// default object.
	void NaiveResetFromCdo(AActor* Proxy)
	{
		const UObject* Cdo = Proxy->GetClass()->GetDefaultObject();
		for (FProperty* Property = Proxy->GetClass()->PropertyLink; Property;
			Property = Property->PropertyLinkNext)
		{
			Property->CopyCompleteValue_InContainer(Proxy, Cdo);
		}
	}

	// The SHIPPED reset, so the guarded readings below measure the reset the dispatch path actually runs
	// rather than a second one written to agree with it.
	void GuardedResetFromCdo(AActor* Proxy)
	{
		CrowdyMassDispatchProxy::ResetToClassDefaults(Proxy);
	}

	// An integer the guarded reset is expected to restore, used to prove the reset did something rather
	// than that it skipped everything. Null when the class declares no such property, which the reading
	// reports rather than passing over.
	FIntProperty* FindResettableInt(const UClass* Class)
	{
		FIntProperty* Inherited = nullptr;

		for (FProperty* Property = Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
		{
			if (Property->HasAnyPropertyFlags(CPF_InstancedReference))
			{
				continue;
			}

			FIntProperty* AsInt = CastField<FIntProperty>(Property);
			if (!AsInt)
			{
				continue;
			}

			// The class's own property answers for the thing an author would actually write from a body.
			// One inherited from a base is only a fallback, since a class that declares no integer of its
			// own would otherwise leave this unmeasured.
			if (AsInt->GetOwnerClass() == Class)
			{
				return AsInt;
			}

			Inherited = Inherited ? Inherited : AsInt;
		}

		return Inherited;
	}

	void DispatchOnProxy(AActor* Proxy, UFunction* Function)
	{
		const int32 FrameSize = FMath::Max<int32>(Function->ParmsSize, 1);
		void* Frame = FMemory_Alloca(FrameSize);
		FMemory::Memzero(Frame, FrameSize);

		FIntProperty* FirstInt = nullptr;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Frame);
			FirstInt = FirstInt ? FirstInt : CastField<FIntProperty>(*It);
		}

		// A value the frame could not have arrived carrying by accident, so a body that reads its parameter
		// back proves the frame reached it rather than that everything happened to be zero.
		if (FirstInt)
		{
			FirstInt->SetPropertyValue_InContainer(Frame, CrowdyMassProxyProbeParameterSentinel);
		}

		Proxy->ProcessEvent(Function, Frame);

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Frame);
		}
	}
}

FString FCrowdyMassDispatchProxyProbeReading::ToReport() const
{
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("[dispatch proxy probe] class '%s'"), *ClassName.ToString()));

	if (!Refusal.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("  REFUSED: %s"), *Refusal));
		return FString::Join(Lines, LINE_TERMINATOR);
	}

	Lines.Add(FString::Printf(TEXT("  proxy constructed: %s"), bProxyConstructed ? TEXT("yes") : TEXT("NO")));
	Lines.Add(FString::Printf(TEXT("  proxy resolves a world: %s"), bWorldResolved ? TEXT("yes") : TEXT("NO")));
	Lines.Add(FString::Printf(TEXT("  world reports actors initialized: %s"),
		bWorldActorsInitialized ? TEXT("yes") : TEXT("no")));
	Lines.Add(FString::Printf(TEXT("  a script body would be allowed to run here: %s"),
		bScriptExecutionAllowedHere ? TEXT("yes") : TEXT("NO")));
	Lines.Add(FString::Printf(TEXT("  root component present: %s"), bRootComponentPresent ? TEXT("yes") : TEXT("NO")));
	Lines.Add(FString::Printf(TEXT("  proxy is in the level's actor list: %s"),
		bProxyPresentInLevelActorList ? TEXT("YES, it was spawned after all") : TEXT("no")));

	Lines.Add(FString::Printf(TEXT("  components on the proxy: %d"), ProxyComponents.Num()));
	for (const FCrowdyMassProxyComponentReading& Component : ProxyComponents)
	{
		Lines.Add(FString::Printf(TEXT("    %s (%s) registered=%s animInstance=%s"),
			*Component.Name.ToString(), *Component.ClassName.ToString(),
			Component.bRegistered ? TEXT("yes") : TEXT("no"),
			Component.bHasAnimInstance ? TEXT("yes") : TEXT("no")));
	}

	if (bComparisonSpawned)
	{
		Lines.Add(FString::Printf(TEXT("  components on a genuinely spawned actor: %d"), SpawnedComponentCount));
		Lines.Add(FString::Printf(TEXT("  components ONLY a spawned actor has: %d"),
			ComponentsOnlyOnASpawnedActor.Num()));
		for (const FName& Missing : ComponentsOnlyOnASpawnedActor)
		{
			Lines.Add(FString::Printf(TEXT("    missing from the proxy: %s"), *Missing.ToString()));
		}
	}
	else
	{
		Lines.Add(TEXT("  no comparison actor was spawned, so nothing is known about what spawning adds."));
	}

	Lines.Add(FString::Printf(TEXT("  transform projected: %s (asked for %s, read back %s)"),
		bTransformProjected ? TEXT("yes") : TEXT("NO"),
		*ProjectedLocation.ToString(), *ReadBackLocation.ToString()));

	Lines.Add(FString::Printf(TEXT("  event function resolved: %s, dispatched: %s"),
		bFunctionResolved ? TEXT("yes") : TEXT("no"), bFunctionDispatched ? TEXT("yes") : TEXT("no")));

	Lines.Add(FString::Printf(TEXT("  a wholesale reset from the class default object steals the proxy's components: %s"),
		bNaiveCdoResetStealsComponents ? TEXT("YES") : TEXT("no")));
	Lines.Add(FString::Printf(TEXT("  a reset that skips subobject references keeps them: %s"),
		bGuardedCdoResetKeepsComponents ? TEXT("yes") : TEXT("NO")));
	Lines.Add(FString::Printf(TEXT("  that guarded reset still restores a plain property: %s"),
		bGuardedCdoResetClearsPlainProperties ? TEXT("yes") : TEXT("not proven, the class declares no plain int")));

	return FString::Join(Lines, LINE_TERMINATOR);
}

FCrowdyMassDispatchProxyProbeReading CrowdyMassProbeDispatchProxy(UWorld* World, UClass* ActorClass,
	const FName FunctionName, const bool bSpawnComparisonActor)
{
	FCrowdyMassDispatchProxyProbeReading Reading;
	Reading.ClassName = ActorClass ? ActorClass->GetFName() : NAME_None;

	if (!World || !World->PersistentLevel)
	{
		Reading.Refusal = TEXT("no world with a persistent level to outer a proxy to.");
		return Reading;
	}

	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Reading.Refusal = TEXT("the class is not an actor class.");
		return Reading;
	}

	if (ActorClass->HasAnyClassFlags(CLASS_Abstract))
	{
		Reading.Refusal = TEXT("the class is abstract, so no proxy of it can be constructed.");
		return Reading;
	}

	// Strong for the whole probe and beyond it: nothing else references a proxy, and a collection partway
	// through would leave the readings describing objects that have gone.
	TStrongObjectPtr<AActor>& Proxy = Reading.Proxy;
	Proxy.Reset(ConstructProxy(World, ActorClass));
	if (!Proxy.IsValid())
	{
		Reading.Refusal = TEXT("constructing the proxy returned nothing.");
		return Reading;
	}

	Reading.bProxyConstructed = true;
	Reading.bWorldResolved = Proxy->GetWorld() != nullptr;
	Reading.bWorldActorsInitialized = World->AreActorsInitialized();
	Reading.bScriptExecutionAllowedHere = Reading.bWorldResolved
		&& (Reading.bWorldActorsInitialized || GAllowActorScriptExecutionInEditor);
	Reading.bRootComponentPresent = Proxy->GetRootComponent() != nullptr;
	Reading.bProxyPresentInLevelActorList = World->PersistentLevel->Actors.Contains(Proxy.Get());

	ReadComponents(Proxy.Get(), Reading.ProxyComponents);

	if (bSpawnComparisonActor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		if (AActor* Spawned = World->SpawnActor<AActor>(ActorClass, FTransform::Identity, SpawnParams))
		{
			Reading.bComparisonSpawned = true;

			TArray<FCrowdyMassProxyComponentReading> SpawnedComponents;
			ReadComponents(Spawned, SpawnedComponents);
			Reading.SpawnedComponentCount = SpawnedComponents.Num();

			TSet<FName> ProxyComponentNames;
			for (const FCrowdyMassProxyComponentReading& Component : Reading.ProxyComponents)
			{
				ProxyComponentNames.Add(Component.Name);
			}

			for (const FCrowdyMassProxyComponentReading& Component : SpawnedComponents)
			{
				if (!ProxyComponentNames.Contains(Component.Name))
				{
					Reading.ComponentsOnlyOnASpawnedActor.Add(Component.Name);
				}
			}

			Spawned->Destroy();
		}
	}

	Reading.ProjectedLocation = ProxyProbeProjectedLocation;
	Reading.bTransformProjected = Proxy->SetActorLocation(ProxyProbeProjectedLocation);
	Reading.ReadBackLocation = Proxy->GetActorLocation();

	if (UFunction* Function = FunctionName.IsNone() ? nullptr : ActorClass->FindFunctionByName(FunctionName))
	{
		Reading.bFunctionResolved = true;
		DispatchOnProxy(Proxy.Get(), Function);
		Reading.bFunctionDispatched = true;
	}

	// Both resets run on their own throwaway proxies, so neither reading is taken from an object the other
	// already damaged.
	TStrongObjectPtr<AActor> NaiveSubject(ConstructProxy(World, ActorClass));
	if (NaiveSubject.IsValid())
	{
		NaiveResetFromCdo(NaiveSubject.Get());
		Reading.bNaiveCdoResetStealsComponents = !OwnsEveryComponentItPointsAt(NaiveSubject.Get());
	}

	TStrongObjectPtr<AActor> GuardedSubject(ConstructProxy(World, ActorClass));
	if (GuardedSubject.IsValid())
	{
		FIntProperty* Dirtied = FindResettableInt(ActorClass);
		if (Dirtied)
		{
			Dirtied->SetPropertyValue_InContainer(GuardedSubject.Get(), ProxyProbeDirtyIntValue);
		}

		GuardedResetFromCdo(GuardedSubject.Get());
		Reading.bGuardedCdoResetKeepsComponents = OwnsEveryComponentItPointsAt(GuardedSubject.Get());
		Reading.bGuardedCdoResetClearsPlainProperties = Dirtied
			&& Dirtied->Identical_InContainer(GuardedSubject.Get(), ActorClass->GetDefaultObject());
	}

	return Reading;
}

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassProxyProbe(
	TEXT("crowdy.mass.proxyprobe"),
	TEXT("Builds a never-spawned dispatch proxy of a class and reports what is real on it: which components exist, which are registered, whether a body would be allowed to run, and what only a genuinely spawned actor has. Args: <ClassPathOrName> [FunctionName] [SpawnComparison 0|1, default 1]. The comparison spawns and immediately destroys one real actor of the class, which runs its construction script and its BeginPlay."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogCrowdyMass, Warning,
					TEXT("[dispatch proxy probe]: name a class, for example crowdy.mass.proxyprobe /Game/Characters/BP_Hero.BP_Hero_C"));
				return;
			}

			UClass* ActorClass = UClass::TryFindTypeSlow<UClass>(Args[0]);
			if (!ActorClass)
			{
				ActorClass = LoadObject<UClass>(nullptr, *Args[0]);
			}

			if (!ActorClass)
			{
				UE_LOG(LogCrowdyMass, Warning,
					TEXT("[dispatch proxy probe]: nothing loaded for '%s'. A Blueprint class path ends in _C."),
					*Args[0]);
				return;
			}

			const FName FunctionName = Args.Num() > 1 ? FName(*Args[1]) : NAME_None;
			const bool bSpawnComparison = Args.Num() > 2 ? Args[2] != TEXT("0") : true;

			const FCrowdyMassDispatchProxyProbeReading Reading =
				CrowdyMassProbeDispatchProxy(World, ActorClass, FunctionName, bSpawnComparison);
			UE_LOG(LogCrowdyMass, Display, TEXT("%s"), *Reading.ToReport());
		}));
#endif
