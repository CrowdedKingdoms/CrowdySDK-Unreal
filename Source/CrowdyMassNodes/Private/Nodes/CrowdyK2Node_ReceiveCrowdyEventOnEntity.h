#pragma once

#include "CoreMinimal.h"
#include "Engine/MemberReference.h"
#include "K2Node.h"
#include "CrowdyK2Node_ReceiveCrowdyEventOnEntity.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UEdGraph;
class UScriptStruct;

/**
 * The pin names this node reserves.
 *
 * All but Target contain a space, which a reflected parameter name cannot, so growing a pin per event
 * parameter can never collide with one of them. Target is the exception because it does not stay a pin:
 * it becomes a parameter of the intermediate event this node compiles to, so it has to be spelled like
 * one. An event that declares a parameter of that name is reported as a compile error instead.
 */
namespace CrowdyMassReceiveEventPins
{
	extern const FName Bind;
	extern const FName Unbind;
	extern const FName Then;
	extern const FName OnEvent;
	extern const FName Target;
	extern const FName EventPicker;
}

/**
 * "Receive Crowdy Event on Entity": runs part of a graph whenever a Crowdy event arrives for a crowd
 * entity, with the event's own parameters as typed pins.
 *
 * The mirror of "Call Crowdy Event on Entity", and the observer's half of the same event. On the machine
 * that owns the entity, the event's body runs on a real actor and none of this is involved. On a machine
 * that draws the same player as a row in a table there is no actor to run a body on, so this node is what
 * that machine does with the call instead.
 *
 * The event is picked on the node, and the node grows one output pin per parameter of the picked event
 * plus a Target naming the entity the call arrived for. Anything a graph can do it can do here: print a
 * line, play a sound, spawn an effect at the target, or write the entity's own fragments through the
 * Crowdy Mass field nodes.
 *
 * The dropdown offers nearly what the call node's does, and one thing less: an event the SDK declares on
 * the crowd stand-in base is applied by the crowd plane itself, which runs the event's own body rather
 * than handing the call to anything registered, so there is nothing here to receive. Picking one is a
 * compile error rather than a graph that quietly never runs.
 *
 * Bind is an exec INPUT rather than something that happens by itself, so a graph decides when it starts
 * listening, and Unbind is what stops it. A binding belongs to the object whose graph this is and to that
 * object's world, and it is dropped when either goes.
 *
 * Several nodes may listen to one event, and so may C++ handlers registered for it. All of them run, in
 * the order they were bound, and none of them may depend on that order.
 */
UCLASS()
class UCrowdyK2Node_ReceiveCrowdyEventOnEntity : public UK2Node
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
	virtual bool CanPasteHere(const UEdGraph* TargetGraph) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	virtual void EarlyValidation(class FCompilerResultsLog& MessageLog) const override;
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

	UEdGraphPin* GetBindPin() const;
	UEdGraphPin* GetUnbindPin() const;
	UEdGraphPin* GetThenPin() const;
	UEdGraphPin* GetOnEventPin() const;
	UEdGraphPin* GetTargetPin() const;

	/**
	 * The runtime entries this node compiles to, found by their names in the CrowdyMass module rather than
	 * by a hard reference, so the editor module does not have to know which library class declares them.
	 * Null when the runtime module does not declare one, which the node reports as a compile error.
	 */
	static UFunction* ResolveBindFunction();
	static UFunction* ResolveUnbindFunction();

	/** The struct the Target pin carries. Null when the runtime module does not declare it. */
	static UScriptStruct* ResolveEntityTargetStruct();

	/**
	 * The names the intermediate event already uses for itself, which an event parameter therefore cannot
	 * also use: giving one of these to a parameter would have the compiler rename it, and the binding
	 * matches parameters by name.
	 */
	static bool IsReservedParameterName(FName ParameterName);

private:
	/** Rewrites the dropdown's label from the picked event, which a rename may have redirected. */
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
