// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphSchema.h"
#include "CrowdyEffectGraphSchema.generated.h"

class UCrowdyEffectGraphNode;

/**
 * What a producer node's value definitely is, when that can be decided from the node alone. Unknown is the honest
 * answer for an attribute read, a tuning parameter, a ref read, a call, or a select: those genuinely can be anything
 * at runtime, so the schema stays permissive for them rather than guessing and refusing a legitimate wire.
 */
enum class ECrowdyEffectGraphValueKind : uint8
{
	Unknown,
	Number,
	Bool,
	String,
	Null,
	// A list, produced by one of the list builtins that returns the whole list (Array, Append, Set At, Remove At).
	// Never valid in arithmetic or a condition, but always valid wherever a value pin merely needs Any, including
	// another list builtin's argument.
	Array
};

/** What an input pin strictly needs from whatever drives it. */
enum class ECrowdyEffectGraphPinNeed : uint8
{
	// No constraint beyond the pin categories (a write target, a select branch, a call argument).
	Any,
	// A true/false value: a condition gate, a select's condition, a logic operand, a logical not's input.
	Boolean,
	// A number to compute with: an arithmetic operand, an arithmetic negate's input.
	Numeric,
	// A container id: the ref-read's id input. Ids are strings, never numbers or booleans.
	Identifier
};

/**
 * A right-click palette action that spawns one effect-graph value node of NodeClass at the click location, wiring it
 * to the pin the drag started from (when any). The Result node is never offered here; it is created with the graph.
 *
 * Several node kinds hold an operator (arithmetic, comparison, logic, unary, constant kind, attribute role), and the
 * menu offers one entry per concrete operation rather than a single generic node. An entry names the operator it
 * spawns through PresetPropertyName / PresetValue; the node's inline selector still changes it afterwards.
 */
USTRUCT()
struct FCrowdyEffectGraphSchemaAction_NewNode : public FEdGraphSchemaAction
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UCrowdyEffectGraphNode> NodeClass = nullptr;

	// The node properties this entry stamps on the node it spawns, keyed by property name, each value in the property
	// system's own text form ("Subtract" for an enum, "true" for a bool, "3" for an int, "clamp" for a string). Empty
	// when the entry spawns the node with its class defaults.
	UPROPERTY()
	TMap<FName, FString> Presets;

	FCrowdyEffectGraphSchemaAction_NewNode() = default;

	// Every entry shares one grouping: the action menu draws a divider between differing groupings, so a distinct
	// grouping per entry would split the palette into one boxed-off row per operation.
	FCrowdyEffectGraphSchemaAction_NewNode(
		FText InCategory, FText InMenuDesc, FText InTooltip, TSubclassOf<UCrowdyEffectGraphNode> InNodeClass,
		FText InKeywords = FText(), TMap<FName, FString> InPresets = {})
		: FEdGraphSchemaAction(MoveTemp(InCategory), MoveTemp(InMenuDesc), MoveTemp(InTooltip), 0, MoveTemp(InKeywords))
		, NodeClass(InNodeClass)
		, Presets(MoveTemp(InPresets))
	{
	}

	static FName StaticGetTypeId()
	{
		static FName Type(TEXT("FCrowdyEffectGraphSchemaAction_NewNode"));
		return Type;
	}

	virtual FName GetTypeId() const override { return StaticGetTypeId(); }

	virtual UEdGraphNode* PerformAction(
		UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode = true) override;

	// Writes each preset into the node through the property system's text import, so one path covers enum, bool,
	// numeric, and string properties. Applied before the node allocates its pins, since an operator can decide a pin's
	// category (a logical not takes a boolean input, an arithmetic negate a plain value) and an argument count decides
	// how many pins exist at all. Returns how many properties were written; a missing or unparseable one is skipped
	// rather than treated as fatal. Static so it is unit-testable without a live menu.
	static int32 ApplyPresets(UCrowdyEffectGraphNode& Node, const TMap<FName, FString>& InPresets);
};

/**
 * The schema for a Crowdy effect graph. It is a pure dataflow graph (value output pins feed value input pins into a
 * single Result node, no execution pins), so the rules are simple: value pins connect only to value pins, an input
 * pin holds a single link (a new connection replaces the old, so the graph never carries the "only the first link is
 * read" ambiguity), and a connection that would close a cycle is refused. The right-click palette offers one action
 * per value-node kind; the Result node is created with the graph and never spawned from the palette.
 */
UCLASS()
class UCrowdyEffectGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	virtual void GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* A, const UEdGraphPin* B) const override;
	virtual FLinearColor GetPinTypeColor(const FEdGraphPinType& PinType) const override;
	virtual void CreateDefaultNodesForGraph(UEdGraph& Graph) const override;

	// True when connecting the producer node feeding OutputPin to the consumer node behind InputPin would close a
	// cycle in the value dataflow (the consumer already reaches the producer). Static so it is unit-testable without
	// a live schema instance.
	static bool WouldConnectionCauseLoop(const UEdGraphPin* OutputPin, const UEdGraphPin* InputPin);

	// True when Node's result can never be a boolean, so wiring it into an input that strictly requires one is a plain
	// authoring mistake: a number, string, or null constant, an arithmetic result, or an arithmetic negate. An
	// attribute, a tuning value, a ref read, a call, or a select is left permissive (it may genuinely be a boolean at
	// runtime), so the connection into a boolean input is allowed. Static so it is unit-testable.
	static bool IsPlainlyNonBooleanProducer(const UEdGraphNode* Node);

	// What Node's output definitely is, or Unknown when the node alone does not decide it.
	static ECrowdyEffectGraphValueKind ClassifyProducer(const UEdGraphNode* Node);

	// What InputPin needs from its driver. Reads the pin's category and its owning node's role, so the rule lives in
	// one place instead of being scattered through CanCreateConnection.
	static ECrowdyEffectGraphPinNeed RequirementForInput(const UEdGraphPin* InputPin);

	// False when a producer of that kind can never satisfy that need. Unknown always passes: the schema refuses only
	// what is definitely wrong, never what is merely unproven.
	static bool IsKindAcceptable(ECrowdyEffectGraphValueKind Kind, ECrowdyEffectGraphPinNeed Need);
};
