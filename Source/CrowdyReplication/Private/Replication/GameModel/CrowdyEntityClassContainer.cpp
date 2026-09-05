// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyEntityClassContainer.h"

#include "CrowdyGameModelLog.h"
#include "Components/ActorComponent.h"
#include "Core/FCrowdyTypeID.h" // FCrowdyClassID, CROWDY_INVALID_CLASS_ID
#include "Data/CrowdyEntityTypes.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyBindingKeyProvider.h"
#include "Replication/GameModel/CrowdyContainerStandIn.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"
#include "UObject/Script.h" // FEditorScriptExecutionGuard, so a Blueprint binding key can be read off a template
#include "UObject/SoftObjectPath.h"
#include "Utils/UCrowdyClassRegistry.h" // ClassID -> class path, the only record of what class an entity is

const UClass* CrowdyEntityClassContainer::ResolveTaggedClass(const UClass* Class)
{
	if (!Class)
	{
		return nullptr;
	}

#if WITH_EDITORONLY_DATA
	// A Blueprint class that cannot name the asset it was generated from is left exactly as it is: asking it for
	// its authoritative class is fatal in an uncooked build. Native classes always answer "itself" and are safe.
	if (!Class->HasAnyClassFlags(CLASS_Native) && !Class->ClassGeneratedBy)
	{
		return Class;
	}
#endif

	const UClass* Authoritative = Class->GetAuthoritativeClass();
	return Authoritative ? Authoritative : Class;
}

bool CrowdyEntityClassContainer::TryGetContainerTypeName(const UClass* Class, FString& OutTypeName)
{
	const UClass* Tagged = ResolveTaggedClass(Class);
	return Tagged && FCrowdyAttributeRegistry::GetContainerTypeName(Tagged, OutTypeName);
}

const UClass* CrowdyEntityClassContainer::ResolveRecordedClass(const uint32 ClassID)
{
	if (ClassID == CROWDY_INVALID_CLASS_ID)
	{
		return nullptr;
	}

	const FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(static_cast<FCrowdyClassID>(ClassID));
	if (!ClassPath.IsValid())
	{
		return nullptr;
	}

	// ResolveClass, never TryLoadClass. The registry is built from the classes this client scanned at startup, so an
	// entity class it actually draws is already in memory; loading one here would let an id chosen elsewhere decide
	// when this client pulls a package in.
	return ClassPath.ResolveClass();
}

bool CrowdyEntityClassContainer::RecordNamesAnotherClass(const UCrowdyEntitySubsystem* Entities, const FGuid& NetID,
	const UObject* Participant, uint32& OutClassID)
{
	OutClassID = CROWDY_INVALID_CLASS_ID;
	if (!Entities || !Participant || !NetID.IsValid())
	{
		return false;
	}

	const FCrowdyEntityRecord* Record = Entities->FindRecord(NetID);
	if (!Record || Record->ClassID == CROWDY_INVALID_CLASS_ID)
	{
		return false;
	}
	if (Record->ClassID == UCrowdyClassRegistry::Get()->GetID(Participant->GetClass()))
	{
		return false;
	}

	OutClassID = Record->ClassID;
	return true;
}

const UClass* CrowdyEntityClassContainer::ResolveRecordedContainerClass(const UCrowdyEntitySubsystem* Entities,
	const FGuid& NetID, const UObject* Participant, FString& OutTypeName)
{
	if (!Entities || !Participant || !NetID.IsValid())
	{
		return nullptr;
	}

	// A stand-in names the class it represents outright. Asked before anything else because it is the only route
	// that answers for a component class: a class id inverts only for a class the startup scan registered, and the
	// components hanging off an entity actor are not in that scan.
	if (const UCrowdyContainerStandIn* StandIn = Cast<UCrowdyContainerStandIn>(Participant))
	{
		FString StandInType;
		if (!StandIn->RepresentedClass || !TryGetContainerTypeName(StandIn->RepresentedClass, StandInType))
		{
			return nullptr;
		}
		OutTypeName = MoveTemp(StandInType);
		return StandIn->RepresentedClass;
	}

	// The participant's own class first: when it declares a container, that class IS the container and nothing here
	// applies. This is the only precedence in play, and it holds because a declaring participant can always bind.
	FString ParticipantType;
	if (TryGetContainerTypeName(Participant->GetClass(), ParticipantType))
	{
		return nullptr;
	}

	uint32 RecordedClassID = CROWDY_INVALID_CLASS_ID;
	if (!RecordNamesAnotherClass(Entities, NetID, Participant, RecordedClassID))
	{
		return nullptr;
	}

	const UClass* RecordedClass = ResolveRecordedClass(RecordedClassID);
	if (!RecordedClass)
	{
		// Not an error on its own: an entity drawn as a class this client never loaded simply has no declaration to
		// read here. BindParticipantContainer says so once, because the effects aimed at it are refused either way.
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] entity %s records class id %u, which resolves to no loaded class here, so no container is derived from it."),
			*NetID.ToString(), RecordedClassID);
		return nullptr;
	}

	FString RecordedType;
	if (!TryGetContainerTypeName(RecordedClass, RecordedType))
	{
		return nullptr;
	}

	OutTypeName = MoveTemp(RecordedType);
	return RecordedClass;
}

namespace
{
	// Keyed on the actor class, holding the class it was derived from so a recycled object index can never hand
	// back another class's answer. Process-wide because it answers a fact about a class, not about a world.
	struct FCrowdyDerivedContainerEntry
	{
		TWeakObjectPtr<const UClass> SourceClass;
		TArray<FCrowdyDerivedComponentContainer> Containers;
	};

	TMap<FObjectKey, FCrowdyDerivedContainerEntry>& DerivedComponentContainerCache()
	{
		static TMap<FObjectKey, FCrowdyDerivedContainerEntry> Cache;
		return Cache;
	}

	// The binding key the owner's enrollment would have read, taken off the template or CDO subobject rather than
	// off a live component. An empty answer falls back to the object name, exactly as the owner's path does.
	FString ReadTemplateBindingKey(const UActorComponent* Template)
	{
		if (!Template || !Template->GetClass()->ImplementsInterface(UCrowdyBindingKeyProvider::StaticClass()))
		{
			return FString();
		}
		FEditorScriptExecutionGuard ScriptGuard;
		return ICrowdyBindingKeyProvider::Execute_GetCrowdyBindingKey(Template);
	}

	void AddDerivedContainer(TArray<FCrowdyDerivedComponentContainer>& Out, const UClass* ComponentClass,
		const UActorComponent* Template, const FString& InstanceTerm)
	{
		FString TypeName;
		if (!ComponentClass || InstanceTerm.IsEmpty()
			|| !CrowdyEntityClassContainer::TryGetContainerTypeName(ComponentClass, TypeName))
		{
			return;
		}

		const FString BindingKey = ReadTemplateBindingKey(Template);
		const FString Term = BindingKey.IsEmpty() ? InstanceTerm : BindingKey;

		// A child Blueprint overriding an inherited component reaches the same variable name twice on the way up
		// the chain. The most derived class is walked first, so the first answer is the one an instance will have.
		for (const FCrowdyDerivedComponentContainer& Existing : Out)
		{
			if (Existing.InstanceTerm == Term && Existing.ComponentClass == ComponentClass)
			{
				return;
			}
		}

		FCrowdyDerivedComponentContainer& Added = Out.AddDefaulted_GetRef();
		Added.ComponentClass = ComponentClass;
		Added.InstanceTerm = Term;
		Added.TypeName = MoveTemp(TypeName);
		Added.bInstanceTermFromBindingKey = !BindingKey.IsEmpty();
	}

	// Whether the whole chain could actually be read. False means the answer is "not yet", never "nothing":
	// a class still being serialized or compiled has no CDO and no construction script to read, and the empty
	// answer that produces must never be remembered as this class's set of containers.
	bool DeriveComponentContainers(const UClass* ActorClass, TArray<FCrowdyDerivedComponentContainer>& Out)
	{
		// Mirrors UCrowdyAutoRegistry::ResolveDefaultEntityComponent's chain walk, most derived first, but collects
		// every match instead of stopping at the first. GetDefaultObject(false) never forces CDO creation: classes
		// may still be compiling when a world-init sweep reaches this.
		for (const UClass* Current = ActorClass; Current; Current = Current->GetSuperClass())
		{
			const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current);
			if (!BPClass)
			{
				// The first native ancestor owns every native component, so the chain ends here either way.
				const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false));
				if (!CDO)
				{
					return false;
				}
				for (const UActorComponent* Component : CDO->GetComponents())
				{
					if (Component)
					{
						// A default subobject carries the name CreateDefaultSubobject was given, on the CDO and on
						// every instance alike, so the CDO's name is the instance term the owner enrolled under.
						AddDerivedContainer(Out, Component->GetClass(), Component, Component->GetName());
					}
				}
				return true;
			}

			// A class the loader has not finished with holds no construction script yet, and it will hold one a
			// moment later. Reading it now answers "no components" for a class that has them.
			if (Current->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects))
			{
				return false;
			}

			if (!BPClass->SimpleConstructionScript)
			{
				continue; // a Blueprint class with no components of its own still has ancestors that may have some
			}

			for (const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}
				const UActorComponent* Template = Node->ComponentTemplate;
				// The template's class is what an instance will be; ComponentClass is only what the node declares.
				const UClass* ComponentClass = Template ? Template->GetClass() : Node->ComponentClass.Get();
				// The variable name, never the template's own name: a template is suffixed with _GEN_VARIABLE and
				// the instance the engine builds from it is named for the variable instead.
				AddDerivedContainer(Out, ComponentClass, Template, Node->GetVariableName().ToString());
			}
		}

		// An AActor subclass always reaches a native ancestor, so falling out of the loop means the chain itself
		// was unreadable rather than exhausted.
		return false;
	}
}

bool CrowdyEntityClassContainer::GetDerivedComponentContainers(const UClass* ActorClass,
	TArray<FCrowdyDerivedComponentContainer>& OutContainers)
{
	check(IsInGameThread());
	OutContainers.Reset();
	if (!ActorClass || !ActorClass->IsChildOf<AActor>())
	{
		return false;
	}

	TMap<FObjectKey, FCrowdyDerivedContainerEntry>& Cache = DerivedComponentContainerCache();
	const FObjectKey Key(ActorClass);
	if (const FCrowdyDerivedContainerEntry* Cached = Cache.Find(Key))
	{
		if (Cached->SourceClass.Get() == ActorClass)
		{
			OutContainers = Cached->Containers;
			return true;
		}
	}

	FCrowdyDerivedContainerEntry Entry;
	Entry.SourceClass = ActorClass;
	const bool bComplete = DeriveComponentContainers(ActorClass, Entry.Containers);
	OutContainers = Entry.Containers;

	// Only a derivation that could read the whole chain is remembered. A class asked about while it is still
	// loading derives nothing, and caching that would answer "no containers" for every row of that class for the
	// rest of the process, with the route diagnostic naming the wrong cause.
	if (bComplete)
	{
		Cache.Add(Key, MoveTemp(Entry));
	}

	return bComplete;
}
