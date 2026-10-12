#pragma once

#include "CoreMinimal.h"
#include "Engine/MemberReference.h"
#include "K2Node.h"
#include "CrowdyK2Node_CallCrowdyEventOnEntity.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class FNodeHandlingFunctor;
class UEdGraph;
class UScriptStruct;

/** The pin names this node reserves. Each contains a space, which a reflected parameter name cannot,
 *  so growing a pin per event parameter can never collide with one of the node's own pins. */
namespace CrowdyMassCallEventPins
{
	extern const FName Target;
	extern const FName EventClass;
	extern const FName EventName;
	extern const FName EventSignature;
	extern const FName EventPicker;
}

/**
 * "Call Crowdy Event on Entity": sends a Crowdy event to one Mass entity.
 *
 * An actor graph types its call node by casting the actor to its class, which is what makes the class's
 * events appear in the context menu. A Mass entity has no instance to cast, so the event is picked on the
 * node instead: the Event dropdown lists every Crowdy event of every entity class the Mass rendering
 * backend is configured for, plus the SDK's own events under "Any Crowd Entity", grouped by class, and
 * picking one implicitly picks its class. The SDK group is always there, so the headers appear as soon as
 * a project configures one entity class of its own.
 *
 * The node grows one input pin per parameter of the picked event, taken from the event's own signature,
 * so a struct, an enum, an object reference or a container gets the pin it would get on an ordinary call
 * node. The picked event is stored as a member reference, so renaming it redirects the node rather than
 * silently detaching it.
 *
 * The picked class is what the call is ADDRESSED by, not a claim about the entity plugged into Target.
 * Whether the entity can actually receive the event is decided at runtime, from the entity itself.
 */
UCLASS()
class UCrowdyK2Node_CallCrowdyEventOnEntity : public UK2Node
{
	GENERATED_BODY()

public:
	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual void PostReconstructNode() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual void ValidateNodeDuringCompilation(class FCompilerResultsLog& MessageLog) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual FNodeHandlingFunctor* CreateNodeHandler(FKismetCompilerContext& CompilerContext) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node

	/** The picked event, or null when nothing is picked or the stored reference no longer resolves. */
	UFunction* GetPickedEvent() const;

	/** The class the picked event is declared on, or null when the reference does not resolve. */
	UClass* GetPickedEventClass() const;

	/** What the Event dropdown shows: the picked event's name, or an empty string when none is picked. */
	FString GetPickedEventDisplayName() const;

	/**
	 * Records the picked event and rebuilds the pin set around its signature. Called by the Event pin's
	 * dropdown; the rebuild is deferred to the next tick because the widget that made the choice keeps
	 * using the pins after this returns.
	 */
	void SetPickedEvent(UClass* DeclaringClass, FName FunctionName);

	/** Forgets the picked event and drops the pins that were grown from it. */
	void ClearPickedEvent();

	/** The pins the call passes, in the order the runtime entry declares them, followed by the event's
	 *  own parameters in declaration order. Returns false with a reason when a pin is missing. */
	bool CollectCallPins(TArray<UEdGraphPin*>& OutPins, FString& OutProblem) const;

	UEdGraphPin* GetThenPin() const;
	UEdGraphPin* GetTargetPin() const;

	/**
	 * The runtime entry this node compiles to, found by its name in the CrowdyMass module rather than by
	 * a hard reference, so the editor module does not have to know which library class declares it.
	 * Null when the runtime module does not declare it, which the node reports as a compile error.
	 */
	static UFunction* ResolveSendFunction();

	/** The struct the Target pin carries. Null when the runtime module does not declare it. */
	static UScriptStruct* ResolveEntityTargetStruct();

private:
	/**
	 * Rewrites every pin value derived from the picked event: the dropdown's label, and the class and
	 * name the call passes. A rebuild copies the old pins' values onto the new ones, which would restore
	 * the name the event had before it was renamed, so this runs again after that copy.
	 */
	void RefreshEventDerivedPins();

	/** Queues a rebuild for the next editor tick, coalescing a burst of changes into one. */
	void ScheduleReconstruct();

	/**
	 * The picked event, held as a member reference so a rename redirects to the renamed function instead
	 * of leaving a name that matches nothing. Always an external reference to the declaring class, so the
	 * node keeps meaning the same event when it is copied into another Blueprint.
	 */
	UPROPERTY()
	FMemberReference EventReference;

	/** True between scheduling a deferred rebuild and it firing. Transient, never serialized. */
	bool bReconstructPending = false;
};
