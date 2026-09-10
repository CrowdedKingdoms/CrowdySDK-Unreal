// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyEffectSpec.generated.h"

/**
 * FCrowdyEffectSpec is the intermediate spec both the node-graph compiler and FCrowdyEffectSpecBuilder speak: the
 * graph editor compiles a node graph down to one of these, and the builder lowers it to the SAME
 * FCrowdyEffectProgram the text parser produces, so text and graph authoring run through one lowering core. Every
 * field is a closed enum or a constrained value, which is what let a designer-facing picker offer only valid
 * choices; that picker authoring surface has since been retired, but the spec and its builder remain the graph
 * compiler's own target shape.
 *
 * Unreal-native vocabulary: the roles are Target (the affected/bound container) and Source (the instigator), which
 * map to the AST's self / source respectively. Operators are the C++ compound-assignment forms the effect author
 * already knows.
 */

/** The authoring surface a UCrowdyEffect compiles from. Both lower to the identical AST. */
UENUM(BlueprintType)
enum class ECrowdyEffectSource : uint8
{
	// The EffectScript text body (the power-user / diff-friendly form).
	Text        UMETA(DisplayName = "Text (EffectScript)"),

	// The node graph below (the Material-Editor-style declarative graph). Stored editor-only and compiled through
	// the editor graph compiler; lowers to the identical AST as the text form.
	Graph       UMETA(DisplayName = "Graph (node graph)")
};

/** An authoring role. Target = the affected container (AST self); Source = the instigator (AST source). */
UENUM(BlueprintType)
enum class ECrowdyEffectRole : uint8
{
	Target UMETA(DisplayName = "Target (self)"),
	Source UMETA(DisplayName = "Source (instigator)")
};

/** The compound-assignment operator applied to the target attribute (the operation the effect performs). */
UENUM(BlueprintType)
enum class ECrowdyEffectAssignmentOp : uint8
{
	Set      UMETA(DisplayName = "= (set)"),
	Add      UMETA(DisplayName = "+= (add)"),
	Subtract UMETA(DisplayName = "-= (subtract)"),
	Multiply UMETA(DisplayName = "*= (multiply)"),
	Divide   UMETA(DisplayName = "/= (divide)")
};

/** A binary operator joining two right-hand-side terms. Grouped by the shared operator precedence. */
UENUM(BlueprintType)
enum class ECrowdyEffectBinaryOp : uint8
{
	Add      UMETA(DisplayName = "+"),
	Subtract UMETA(DisplayName = "-"),
	Multiply UMETA(DisplayName = "*"),
	Divide   UMETA(DisplayName = "/")
};

/** What one right-hand-side (or comparison) operand is. */
UENUM(BlueprintType)
enum class ECrowdyEffectOperandKind : uint8
{
	Number     UMETA(DisplayName = "Number"),            // a literal number (Literal holds the text)
	Attribute  UMETA(DisplayName = "Attribute"),         // a role.attribute read (Role + Name)
	Magnitude  UMETA(DisplayName = "Magnitude ($param)"),// a tuning magnitude (Name is the $param, no sigil)
	Raw        UMETA(DisplayName = "Raw expression"),     // verbatim EffectScript (Literal), spliced unparsed
	// A full EffectScript expression (Literal) that is PARSED, so it participates in precedence and is validated
	// exactly like the text form (attributes resolved, $params checked). Raw, by contrast, is spliced verbatim.
	Expression UMETA(DisplayName = "Expression (EffectScript)"),
	// A ternary if(Cond, Then, Else) built from the flat If sub-expression strings.
	If         UMETA(DisplayName = "If (Cond, Then, Else)"),
	// A function call (Callee + Args) built from the flat Call sub-expression strings.
	Call       UMETA(DisplayName = "Function call")
};

/** A comparison operator for a require row. */
UENUM(BlueprintType)
enum class ECrowdyEffectComparator : uint8
{
	Equal          UMETA(DisplayName = "=="),
	NotEqual       UMETA(DisplayName = "!="),
	Less           UMETA(DisplayName = "<"),
	Greater        UMETA(DisplayName = ">"),
	LessOrEqual    UMETA(DisplayName = "<="),
	GreaterOrEqual UMETA(DisplayName = ">=")
};

/** The kind of a require gate: a closed policy keyword, or a value comparison. */
UENUM(BlueprintType)
enum class ECrowdyEffectRequireKind : uint8
{
	Keyword    UMETA(DisplayName = "Policy keyword"),
	Comparison UMETA(DisplayName = "Comparison")
};

/** A closed invoke-policy keyword (maps to the lowering's structured policy leaf). */
UENUM(BlueprintType)
enum class ECrowdyEffectPolicyKeyword : uint8
{
	Owner       UMETA(DisplayName = "Caller owns the target (owner_of_self)"),
	MyTurn      UMETA(DisplayName = "It is the caller's turn (my_turn)"),
	Host        UMETA(DisplayName = "Caller is the host (host)"),
	Participant UMETA(DisplayName = "Caller is a participant (participant)"),
	Automation  UMETA(DisplayName = "Caller is server automation (automation)"),
	Anyone      UMETA(DisplayName = "Anyone may call it (anyone)")
};

/**
 * A ternary if(Cond, Then, Else). Flat by design: each part is an EffectScript expression STRING, not a nested
 * operand, so the struct never contains itself (UHT rejects a self-recursive USTRUCT). The parser reaches the
 * full sub-tree from each string. Then/Else may be empty (an empty Then/Else lowers to null).
 */
USTRUCT(BlueprintType)
struct FCrowdyEffectIfSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Condition;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Then;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Else;
};

/**
 * A function call Callee(Args...). Flat by design: each argument is an EffectScript expression STRING, so the
 * struct never nests itself. Callee is a builtin name (max, min, if, ...); prefix it with "fn:" to call an
 * authored server function.
 */
USTRUCT(BlueprintType)
struct FCrowdyEffectCallSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Callee;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	TArray<FString> Args;
};

/** One operand: a number, a role.attribute read, a tuning magnitude, a raw/parsed expression, an if, or a call. */
USTRUCT(BlueprintType)
struct FCrowdyEffectOperand
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectOperandKind Kind = ECrowdyEffectOperandKind::Number;

	// Number: the numeric text ("5", "1.5"). Raw: the verbatim expression. Expression: the parsed EffectScript.
	// Ignored for the other kinds.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Literal;

	// Attribute: which role's attribute is read. Ignored for the other kinds.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectRole Role = ECrowdyEffectRole::Source;

	// Attribute: the attribute name. Magnitude: the $param name (no '$'). Ignored for Number / Raw / Expression /
	// If / Call.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Name;

	// If: the ternary parts, built when Kind is If. Ignored otherwise.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FCrowdyEffectIfSpec If;

	// Call: the function call, built when Kind is Call. Ignored otherwise.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FCrowdyEffectCallSpec Call;
};

/** One term of a right-hand side: an operand, and the operator joining it to the previous term. */
USTRUCT(BlueprintType)
struct FCrowdyEffectTerm
{
	GENERATED_BODY()

	// The operator joining this term to the previous one. Ignored on the first term of a right-hand side.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectBinaryOp Op = ECrowdyEffectBinaryOp::Add;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FCrowdyEffectOperand Operand;
};

/** One structured assignment: TargetRole.Attribute <Operator> <Value terms>. */
USTRUCT(BlueprintType)
struct FCrowdyEffectAssignmentSpec
{
	GENERATED_BODY()

	// The affected attribute is TargetRole.Attribute (Target = self, Source = source).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectRole TargetRole = ECrowdyEffectRole::Target;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Attribute;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectAssignmentOp Operator = ECrowdyEffectAssignmentOp::Subtract;

	// The right-hand-side terms (at least one). Grouped by the shared operator precedence when lowered.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	TArray<FCrowdyEffectTerm> Value;
};

/** One require gate: a policy keyword, or a comparison (Left <Comparator> Right). Combined with 'and'. */
USTRUCT(BlueprintType)
struct FCrowdyEffectRequireSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectRequireKind Kind = ECrowdyEffectRequireKind::Keyword;

	// Keyword: which closed policy keyword this gate requires.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectPolicyKeyword Keyword = ECrowdyEffectPolicyKeyword::Owner;

	// Comparison: Left <Comparator> Right.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FCrowdyEffectOperand Left;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectComparator Comparator = ECrowdyEffectComparator::GreaterOrEqual;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FCrowdyEffectOperand Right;
};

/** The whole structured effect: an ordered list of assignments plus the require gates. */
USTRUCT(BlueprintType)
struct FCrowdyEffectSpec
{
	GENERATED_BODY()

	// The assignments, applied transactionally in order (a later one sees earlier writes).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	TArray<FCrowdyEffectAssignmentSpec> Assignments;

	// The require gates. All must pass for the effect to apply (combined with 'and'). Empty leaves the default
	// gate the lowering infers (owner_of_self for a pure-self effect, is_participant when a Source is read).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	TArray<FCrowdyEffectRequireSpec> Requires;

	// Whether this effect answers with a value. Explicit rather than inferred from Return, because an operand has
	// no "unset" kind: a default-constructed operand is a Number with an empty literal, which is indistinguishable
	// from an author who has not filled one in yet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	bool bHasReturn = false;

	// The value the invocation answers with, built only when bHasReturn. The server evaluates it after every
	// mutation, so it reads the values this invocation just wrote.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect",
		meta = (EditCondition = "bHasReturn", EditConditionHides))
	FCrowdyEffectOperand Return;
};
