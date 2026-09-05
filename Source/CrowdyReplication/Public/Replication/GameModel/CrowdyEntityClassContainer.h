// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UCrowdyEntitySubsystem;

/**
 * One CrowdyContainer component a class declares, named exactly as the owner's own enrollment names it.
 *
 * InstanceTerm is what UCrowdyEntitySubsystem::RegisterSubParticipant folds into its seed for the real component
 * on the owner's machine: the Blueprint variable name for an SCS component, the subobject name for a native one,
 * or the binding key when the component class supplies one. Deriving the same term here is what makes an id minted
 * for a stand-in equal the id the owner minted for the component itself.
 */
struct FCrowdyDerivedComponentContainer
{
	TWeakObjectPtr<const UClass> ComponentClass;
	FString InstanceTerm;
	FString TypeName;
	bool bInstanceTermFromBindingKey = false;
};

/**
 * Resolves the Game Model container an entity binds from the class the ENTITY records, rather than from whatever
 * object happens to represent it on this machine.
 *
 * An entity the local player owns is represented by its own actor, and that actor's class carries the
 * CrowdyContainer declaration, so the container is read straight off the participant. An entity another client
 * owns can be represented here by a lightweight stand-in instead of an actor (a crowd entity drawn as a table row).
 * The stand-in declares nothing, so without this the entity would enroll and bind no container, and every effect
 * aimed at it would be refused as unbound. The class the entity records is the same class its owner runs, so it
 * carries the same declaration, and reading it here means the declaration is authored once, on the actor.
 *
 * THE RECORDED CLASS IS NOT A SECURITY BOUNDARY. It is recorded locally when the entity is created and is never
 * read off a payload, so a third party cannot forge it for someone else's entity. Its own owner, however, chose
 * what their entity claims to be. Use it to route and to decide what an entity declares; never to decide what an
 * entity is allowed to do. A binding derived from it is read-only: it reads an existing server row by key and must
 * never create one.
 */
namespace CrowdyEntityClassContainer
{
	/**
	 * The class Crowdy class tags are actually stamped on, given any class.
	 *
	 * A Blueprint's skeleton class (SKEL_<Name>_C) is rebuilt from the graph on every compile and never receives
	 * the tags the compiler extension stamps, so a lookup that lands on one has to be redirected to the generated
	 * class. Returns the class itself for a native class, and for any class whose generated form cannot be named.
	 */
	CROWDYREPLICATION_API const UClass* ResolveTaggedClass(const UClass* Class);

	/**
	 * The container type name Class declares, after the normalization above. False (OutTypeName untouched) when the
	 * class declares none. Cooked builds strip UCLASS metadata, so this goes through the shared attribute-registry
	 * accessor, which reads the baked table there.
	 */
	CROWDYREPLICATION_API bool TryGetContainerTypeName(const UClass* Class, FString& OutTypeName);

	/**
	 * The class a ClassID names, or null when the id names nothing this client has loaded. Resolution never loads a
	 * package: an id that arrived with an entity is only as trustworthy as the entity's owner, so it selects among
	 * classes already in memory and answers "unknown" for everything else.
	 */
	CROWDYREPLICATION_API const UClass* ResolveRecordedClass(uint32 ClassID);

	/**
	 * True when the entity records a class OTHER than its participant's own, with that class's id in OutClassID.
	 *
	 * A participant whose class is the one the entity records IS the entity: its class has been read directly, and
	 * the record can add nothing. Only a participant standing in for something else can, which is the case this
	 * whole namespace exists for. Comparing ids rather than classes also answers for a recorded class that is not
	 * loaded here, which is exactly when the resolution below fails.
	 */
	CROWDYREPLICATION_API bool RecordNamesAnotherClass(const UCrowdyEntitySubsystem* Entities, const FGuid& NetID,
		const UObject* Participant, uint32& OutClassID);

	/**
	 * The container class an entity's own class declaration names, for a participant that declares none itself.
	 *
	 * Null when Participant's class already declares a container (that class IS the container, and it binds as it
	 * always has), when the entity is unregistered or records no class, when the recorded class is not loaded here,
	 * or when it declares no container. A non-null answer is always a read-only binding, and OutTypeName carries
	 * the type it binds.
	 */
	CROWDYREPLICATION_API const UClass* ResolveRecordedContainerClass(const UCrowdyEntitySubsystem* Entities,
		const FGuid& NetID, const UObject* Participant, FString& OutTypeName);

	/**
	 * Every CrowdyContainer component an actor class declares, read from the class alone.
	 *
	 * An observed entity is drawn here as a row with no actor and no components, so the components its owner
	 * enrolled cannot be found by walking an object. This walks the class chain the way the owner's own actor is
	 * built - the Simple Construction Script of each Blueprint class, then the first native ancestor's CDO - and
	 * reports the component class and the per-instance term each one will have carried. Never forces CDO creation,
	 * so it is safe during a world-init sweep.
	 *
	 * Derived once per class and cached, because it answers a fact about a class and nothing about an entity.
	 * OutContainers is emptied first and receives a copy, so a caller may enroll from it while another class is
	 * being derived underneath.
	 *
	 * ONLY A DERIVATION THAT COULD READ THE WHOLE CHAIN IS CACHED. A class asked about while it is still loading
	 * or compiling has no CDO and no construction script to read and derives nothing; remembering that would
	 * answer "declares no container" for every row of that class for the life of the process. Such a class is
	 * simply derived again next time, which is what makes an early ask harmless rather than permanent.
	 *
	 * Returns whether the whole chain could be read. False means "ask again", not "declares nothing": an empty
	 * answer with a true return is a class that genuinely has no container components.
	 */
	CROWDYREPLICATION_API bool GetDerivedComponentContainers(const UClass* ActorClass,
		TArray<FCrowdyDerivedComponentContainer>& OutContainers);
}
