#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UClass;
class UFunction;

/** One Crowdy event that can be sent to a Mass entity, as offered by the node's dropdown. */
struct FCrowdyMassEventEntry
{
	/**
	 * The class that DECLARES the function, which is not always the entity class this entry is listed
	 * under: an inherited event appears under every entity class that inherits it, and is always
	 * addressed by the class that declares it.
	 */
	TWeakObjectPtr<UClass> DeclaringClass;

	/** The reflected function name. A C++ receiver is declared as <Event>_Implementation. */
	FName FunctionName;

	/** The name to show, which drops the _Implementation suffix so it reads as the call site does. */
	FString DisplayName;

	/** The parameter list, for the dropdown entry's tooltip. */
	FText Tooltip;
};

/** The events of one registered entity class, which is how the dropdown groups its entries. */
struct FCrowdyMassEventClassGroup
{
	TWeakObjectPtr<UClass> EntityClass;

	/** The entity class name with a Blueprint's generated-class suffix removed. */
	FString DisplayName;

	TArray<FCrowdyMassEventEntry> Events;
};

/** Everything the dropdown needs, plus why it may be empty. */
struct FCrowdyMassEventCatalog
{
	TArray<FCrowdyMassEventClassGroup> Groups;

	/**
	 * True when the asset registry had not finished scanning while this was built, so an entity class
	 * bound only by a map profile may be missing. An empty catalog then means "not known yet" rather
	 * than "there are none", and the dropdown says so instead of showing a confident empty list.
	 */
	bool bAssetRegistryStillScanning = false;

	/** How many entity classes were found before any were filtered out for declaring no events. */
	int32 NumEntityClassesConsidered = 0;
};

/**
 * Which node's dropdown is being built, because the two nodes do NOT offer the same list.
 *
 * Sending an event to an entity and receiving one for an entity look like the same question and are not:
 * an event the crowd plane applies itself is sendable but has nothing for a graph to receive, because the
 * plane runs the event's own body on a stand-in and never offers the call to a registered handler.
 */
enum class ECrowdyMassEventCatalogPurpose : uint8
{
	/** "Call Crowdy Event on Entity". Offers everything an entity can be sent, the SDK's own events included. */
	Send,

	/** "Receive Crowdy Event on Entity". Drops the events the crowd plane applies itself. */
	Receive
};

namespace CrowdyMassEventCatalog
{
	/**
	 * True when a function is a Crowdy event that can be sent to an entity: it carries the CrowdyEvent
	 * marker, it is one of the two RPC-style forms (a Blueprint event marked Crowdy Replicates, or a
	 * C++ CROWDY_EVENT receiver, which is always declared as <Event>_Implementation), and its signature
	 * can ride the transport. A struct handler, which carries the CrowdyEvent marker alone and is
	 * dispatched with a payload rather than called, is deliberately not offered.
	 */
	bool IsCallableCrowdyEvent(const UFunction* Function);

	/** Describes why a function cannot be sent, or an empty string when it can. Never loads anything. */
	FString DescribeCallProblem(const UFunction* Function);

	/**
	 * True when the crowd plane applies this event itself rather than offering it to anything registered.
	 *
	 * An event declared on the stand-in base every crowd entity is given resolves a stand-in on every
	 * machine that receives it, and the event's own body runs on that object. The observer path that hands
	 * a call to registered handlers is reached only when no such object can be resolved, so for these
	 * events it never is: a graph bound to one would compile clean and never run. Sendable, therefore, and
	 * not receivable, which is the one place the two nodes' lists have to differ.
	 */
	bool IsAppliedByTheCrowdPlane(const UFunction* Function);

	/** The name to show for a function: the reflected name without its _Implementation suffix. */
	FString MakeDisplayName(FName FunctionName);

	/**
	 * True when a class may be offered as an entity to send to. It has to be an actor class, not
	 * deprecated, not a reinstancing artefact, and not hidden from class pickers.
	 *
	 * Hiding is the engine's own Hidden / HideDropDown, not a Crowdy marker, so a dev-only or test-only
	 * entity opts out of every picker at once rather than only this one.
	 */
	bool IsSelectableEntityClass(const UClass* Class);

	/**
	 * Builds the dropdown's contents: every class the Mass rendering backend binds in a map profile,
	 * plus every loaded class that carries a Crowdy entity component, keeping those that declare at
	 * least one callable Crowdy event.
	 *
	 * For Send it adds one further group, for the events the SDK declares itself on the stand-in base
	 * every entity is given, which can be sent to an entity of any class. That group is always present,
	 * so a Send catalog is empty only when the SDK's own class could not be resolved, while a Receive
	 * catalog is empty whenever the project configures no entity class that declares an event of its own.
	 * Either way, anything reporting an INCOMPLETE list has to read bAssetRegistryStillScanning rather
	 * than count groups.
	 *
	 * This reads map-profile assets off disk and resolves the entity classes they name, so it belongs
	 * to the authoring UI and must not be called while a Blueprint is compiling or a cook is running.
	 */
	void Build(FCrowdyMassEventCatalog& OutCatalog, ECrowdyMassEventCatalogPurpose Purpose);
}
