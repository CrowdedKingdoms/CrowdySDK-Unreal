// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "CrowdyEffectGraphNodes.generated.h"

namespace CrowdyEffectGraphPins
{
	// The permissive value-pin category. Any value flows here: a number, a string, an attribute read, a magnitude. A
	// value input accepts either category, since an attribute or a call genuinely can be a boolean at runtime, so the
	// scheme stays conservative and never over-restricts a legitimate wire.
	extern const FName ValueCategory;

	// The boolean-pin category. A producer declares it only when its result is definitely a boolean (a comparison, a
	// logic connective, a logical not, a bool constant); a consumer declares it when it strictly requires a boolean (a
	// condition gate, an If's condition, a logic input, a not's input). The schema still lets an ambiguous value feed a
	// boolean input, but refuses a plainly non-boolean one (a number/string constant, an arithmetic result, a negate).
	// The compiler reads connectivity only and ignores categories, so this changes authoring guidance, not the output.
	extern const FName BoolCategory;
}

/** The literal kind a constant node holds. Number and Bool emit bare; String emits quoted and escaped; Null emits "null". */
UENUM()
enum class ECrowdyEffectGraphConstantType : uint8
{
	Number,
	Bool,
	String,
	Null
};

/** The arithmetic operator a binary-op node applies. A graph-local set, so it carries % (which the picker term operator does not). */
UENUM()
enum class ECrowdyEffectGraphArithOp : uint8
{
	Add,
	Subtract,
	Multiply,
	Divide,
	Modulo
};

/** The boolean connective a logic node applies. */
UENUM()
enum class ECrowdyEffectGraphLogicOp : uint8
{
	And,
	Or
};

/** The prefix operator a unary node applies: logical not, or arithmetic negate. */
UENUM()
enum class ECrowdyEffectGraphUnaryOp : uint8
{
	Not,
	Negate
};

/** One write the Result node performs: TargetRole.Attribute <Op> <the value wired into this write's input pin>. */
USTRUCT()
struct FCrowdyEffectGraphWrite
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Write")
	ECrowdyEffectRole TargetRole = ECrowdyEffectRole::Target;

	UPROPERTY(EditAnywhere, Category = "Write")
	FString Attribute;

	UPROPERTY(EditAnywhere, Category = "Write")
	ECrowdyEffectAssignmentOp Op = ECrowdyEffectAssignmentOp::Set;

	// A stable identity for this write's input pin, so a wired value follows the write it belongs to across a
	// details-panel edit (insert / remove / reorder), not the array index. Assigned on first pin allocation; the pin
	// name stays index-based so the compiler still reads Writes[i] against WritePinName(i). Internal, not authored.
	UPROPERTY()
	FGuid PinId;
};

/**
 * One require gate the Result node applies: a boolean value wired into this condition's single input pin must be
 * true for the effect to run. Build the boolean from the Compare / And-Or / Not / is_null value nodes (or a bare
 * bool attribute) and wire its result here. The optional Note documents the condition and labels its pin.
 */
USTRUCT()
struct FCrowdyEffectGraphRequire
{
	GENERATED_BODY()

	// An optional human note describing what this condition checks (e.g. "target is alive"). Labels the pin and reads
	// on the condition's row; it does not affect compilation.
	UPROPERTY(EditAnywhere, Category = "Condition")
	FString Note;

	// A stable identity for this condition's input pin, so a wired boolean follows the condition it belongs to across
	// a details-panel edit (insert / remove / reorder) rather than the array index. Assigned on first pin allocation.
	// Internal, not authored.
	UPROPERTY()
	FGuid PinId;
};

/**
 * The visual family a node belongs to, which decides its title colour. Families match the right-click palette's
 * categories, so a node's colour tells you which section it came from without reading its title.
 */
enum class ECrowdyEffectGraphNodeFamily : uint8
{
	Attribute,
	Value,
	Arithmetic,
	Comparison,
	Logic,
	Function,
	ServerFunction,
	Flow,
	Result
};

/**
 * The base of every effect-graph node. Holds no state of its own; it only groups the concrete value / result nodes
 * under one type so the compiler can walk a graph and the schema can restrict placement.
 */
UCLASS(Abstract)
class UCrowdyEffectGraphNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	// Which family this node draws as. Overridden per node kind; the base maps the family to a colour, so the whole
	// palette lives in one place and each node kind only has to say what it is.
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const { return ECrowdyEffectGraphNodeFamily::Value; }

	// The colour a family draws in. Static so the palette is unit-testable without constructing nodes.
	static FLinearColor FamilyColor(ECrowdyEffectGraphNodeFamily Family);

	virtual FLinearColor GetNodeTitleColor() const override;

	// Rebuilds the node's pins from its current properties, preserving existing links by pin name. A node's pin set
	// depends on its properties (the Result node's writes / conditions, the Call node's argument count), so an edit
	// in the details panel must refresh the pins.
	virtual void ReconstructNode() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	// Creates the single value output pin every producing node exposes.
	UEdGraphPin* CreateValueOutputPin(FName PinName);

	// Creates a value input pin under the permissive value category.
	UEdGraphPin* CreateValueInputPin(FName PinName);

	// Creates an output / input pin under the boolean category, for a producer whose result is definitely boolean or a
	// consumer that strictly requires one.
	UEdGraphPin* CreateBoolOutputPin(FName PinName);
	UEdGraphPin* CreateBoolInputPin(FName PinName);
};

/** A tuning magnitude read: emits "$ParamName". */
UCLASS()
class UCrowdyEffectGraphNode_Tuning : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Value; }

	UPROPERTY(EditAnywhere, Category = "Tuning", meta = (DisplayName = "Param Name"))
	FString ParamName;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName OutputPinName();
};

/** An attribute read: emits "self.Attribute" (Target) or "source.Attribute" (Source). */
UCLASS()
class UCrowdyEffectGraphNode_Attribute : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Attribute; }

	UPROPERTY(EditAnywhere, Category = "Attribute")
	ECrowdyEffectRole Role = ECrowdyEffectRole::Target;

	UPROPERTY(EditAnywhere, Category = "Attribute")
	FString Attribute;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName OutputPinName();
};

/** A literal constant: a number, a bool, a string, or null, selected by ConstantType. */
UCLASS()
class UCrowdyEffectGraphNode_Constant : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Value; }

	UPROPERTY(EditAnywhere, Category = "Constant")
	ECrowdyEffectGraphConstantType ConstantType = ECrowdyEffectGraphConstantType::Number;

	UPROPERTY(EditAnywhere, Category = "Constant",
		meta = (EditCondition = "ConstantType != ECrowdyEffectGraphConstantType::Null", EditConditionHides))
	FString Literal;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName OutputPinName();
};

/** A binary arithmetic operation joining two value inputs. Always fully parenthesized on emit, so precedence is preserved. */
UCLASS()
class UCrowdyEffectGraphNode_BinaryOp : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Arithmetic; }

	UPROPERTY(EditAnywhere, Category = "Arithmetic", meta = (DisplayName = "Operator"))
	ECrowdyEffectGraphArithOp Op = ECrowdyEffectGraphArithOp::Add;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName InputAPinName();
	static FName InputBPinName();
	static FName OutputPinName();
};

/** A comparison joining two value inputs into a boolean: emits "(A <cmp> B)". */
UCLASS()
class UCrowdyEffectGraphNode_Compare : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Comparison; }

	UPROPERTY(EditAnywhere, Category = "Compare", meta = (DisplayName = "Comparator"))
	ECrowdyEffectComparator Comparator = ECrowdyEffectComparator::GreaterOrEqual;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName InputAPinName();
	static FName InputBPinName();
	static FName OutputPinName();
};

/** A boolean connective joining two value inputs: emits "(A && B)" or "(A || B)". */
UCLASS()
class UCrowdyEffectGraphNode_Logic : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Logic; }

	UPROPERTY(EditAnywhere, Category = "Logic", meta = (DisplayName = "Operator"))
	ECrowdyEffectGraphLogicOp Op = ECrowdyEffectGraphLogicOp::And;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName InputAPinName();
	static FName InputBPinName();
	static FName OutputPinName();
};

/** A prefix operation on one value input: emits "(!X)" or "(-X)". */
UCLASS()
class UCrowdyEffectGraphNode_Unary : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	// The one node that changes family with its operator, matching where each operator sits in the palette: a logical
	// not is Logic, an arithmetic negate is Arithmetic.
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override;

	UPROPERTY(EditAnywhere, Category = "Unary", meta = (DisplayName = "Operator"))
	ECrowdyEffectGraphUnaryOp Op = ECrowdyEffectGraphUnaryOp::Not;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName InputPinName();
	static FName OutputPinName();
};

/** A ternary select: emits "if(<Cond>, <Then>, <Else>)" from its three value inputs. */
UCLASS()
class UCrowdyEffectGraphNode_If : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Flow; }

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName ConditionPinName();
	static FName ThenPinName();
	static FName ElsePinName();
	static FName OutputPinName();
};

/**
 * A builtin function call: emits "Callee(a, b, ...)" for one of the language's builtins (Max, Min, Clamp, Coalesce,
 * ...). Each of the ArgCount arguments is a value input pin. Callee stores the language's own lowercase spelling
 * ("to_string"); the node title shows the Unreal-style label ("To String").
 *
 * Calling an authored server function is the separate Server Function node below. bIsFnCall is the serialized flag
 * both share and the compiler reads; it is not author-facing, so a call node cannot silently become a server call.
 */
UCLASS()
class UCrowdyEffectGraphNode_Call : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Function; }

	// A builtin's name is chosen when the node is placed and is never typed: the palette offers one entry per builtin,
	// so a misspelling can only come from editing this field. It stays editable for a server call, whose name is
	// whatever the author called their function.
	UPROPERTY(EditAnywhere, Category = "Call",
		meta = (DisplayName = "Server Function Name", EditCondition = "bIsFnCall", EditConditionHides))
	FString Callee;

	UPROPERTY()
	bool bIsFnCall = false;

	UPROPERTY(EditAnywhere, Category = "Call", meta = (DisplayName = "Argument Count", ClampMin = "0", ClampMax = "16"))
	int32 ArgCount = 0;

	// Also constrains ArgCount to what the named builtin accepts, so a fixed-arity builtin ("not" takes one operand)
	// can never carry extra argument pins. Done here rather than only in the details panel because every edit path
	// (palette preset, details, inline body, a legacy asset loading) reaches pin allocation.
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	// The input pin name for the argument at the given index, shared by the node (which creates them) and the
	// compiler (which finds them) so the two never drift.
	static FName ArgPinName(int32 Index);
	static FName OutputPinName();
};

/**
 * A call to a function authored on this effect and uploaded by the schema sync: emits "fn:Callee(a, b, ...)". This is
 * the only call node whose name is typed, because only the author knows what their own functions are called. It draws
 * in its own colour so it never reads as one of the language's builtins.
 */
UCLASS()
class UCrowdyEffectGraphNode_ServerCall : public UCrowdyEffectGraphNode_Call
{
	GENERATED_BODY()

public:
	UCrowdyEffectGraphNode_ServerCall();

	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override
	{
		return ECrowdyEffectGraphNodeFamily::ServerFunction;
	}

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
};

/** A read through an explicit container reference: emits "ref(<Id>).Attribute" from the wired id value and the attribute. */
UCLASS()
class UCrowdyEffectGraphNode_ReadRef : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Attribute; }

	UPROPERTY(EditAnywhere, Category = "Read Ref")
	FString Attribute;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	static FName IdPinName();
	static FName OutputPinName();
};

/**
 * The output node. Each write descriptor gets one value input pin; each condition (require) gets one boolean value
 * input pin. Keyword conditions are emitted directly and take no pins. The compiler reads the Writes /
 * KeywordConditions / Requires arrays and their pins to build the effect spec.
 */
UCLASS()
class UCrowdyEffectGraphNode_Result : public UCrowdyEffectGraphNode
{
	GENERATED_BODY()

public:
	virtual ECrowdyEffectGraphNodeFamily GetNodeFamily() const override { return ECrowdyEffectGraphNodeFamily::Result; }

	UPROPERTY(EditAnywhere, Category = "Result", meta = (DisplayName = "Writes (attribute mutations)"))
	TArray<FCrowdyEffectGraphWrite> Writes;

	// Closed policy keywords required for the effect to apply (owner_of_self, is_host, ...). Emitted verbatim; they
	// carry no input pins, since they are invoke-policy leaves, not value expressions.
	UPROPERTY(EditAnywhere, Category = "Result", meta = (DisplayName = "Policy conditions"))
	TArray<ECrowdyEffectPolicyKeyword> KeywordConditions;

	// Boolean require gates. Each spawns one value input pin; wire a boolean value node (Compare / And-Or / Not /
	// is_null / a bool attribute) into it, and the effect only runs when every wired condition is true.
	UPROPERTY(EditAnywhere, Category = "Result", meta = (DisplayName = "Conditions (only run if...)"))
	TArray<FCrowdyEffectGraphRequire> Requires;

	// When set, the effect answers the caller with a value: wire the value to return into the Return pin this
	// spawns. The declared type of the returned value is set on the effect asset, alongside the graph, not here.
	UPROPERTY(EditAnywhere, Category = "Result", meta = (DisplayName = "Returns a value"))
	bool bReturnsValue = false;

	// A stable identity for the Return pin, so a wired value survives a details-panel edit the same way a write's
	// or a condition's does. Assigned on first pin allocation. Internal, not authored.
	UPROPERTY()
	FGuid ReturnPinId;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;

	// Preserves wired values by each write's / condition's stable PinId (stamped on the pin's PersistentGuid) rather
	// than by array index, so inserting, removing, or reordering a write in the details panel never remaps a value
	// onto a different write.
	virtual void ReconstructNode() override;

	// A condition with no author note reads from whatever is wired into it, so a wired condition never shows as a
	// blank prompt. Evaluated on demand (a pin's friendly name is fixed at allocation, before any link exists).
	virtual FText GetPinDisplayName(const UEdGraphPin* Pin) const override;

	// A one-line description of the boolean wired into the condition at RequireIndex (the driving node's title), or
	// empty when nothing is wired or the index is out of range. Shared by the pin label and the details panel, so the
	// two always describe a condition the same way.
	FString DescribeWiredCondition(int32 RequireIndex) const;

	// The single Result node is the graph's output: it is created with the graph and must not be deleted or copied,
	// so a graph always has exactly one.
	virtual bool CanUserDeleteNode() const override { return false; }
	virtual bool CanDuplicateNode() const override { return false; }

	// The input pin names for the write / condition at the given index. Shared by the node (which creates them) and
	// the compiler (which finds them), so the two never drift.
	static FName WritePinName(int32 Index);
	static FName RequirePinName(int32 Index);

	// The Return pin's name, shared by the node (which creates it) and the compiler (which finds it).
	static FName ReturnPinName();
};
