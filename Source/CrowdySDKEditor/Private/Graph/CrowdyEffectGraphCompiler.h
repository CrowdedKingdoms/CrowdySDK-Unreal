// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

class UCrowdyEffectGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * A compile diagnostic paired with the graph node it originated from, so the asset editor can stamp an on-node error
 * badge. Node is best-effort: a whole-graph problem (a missing Result, an over-deep chain) may leave it unset, in
 * which case the caller shows it on the output node.
 */
struct FCrowdyEffectGraphNodeDiagnostic
{
	ECrowdyEffectSeverity Severity = ECrowdyEffectSeverity::Error;
	TWeakObjectPtr<const UEdGraphNode> Node;
	FString Message;
};

/**
 * Compiles an effect node graph into the shared structured effect spec, so the graph front-end lowers through the
 * exact same FCrowdyEffectSpecBuilder -> FCrowdyEffectLowering path the text and picker surfaces use. The graph to
 * spec step is the only new logic; everything downstream is the already-tested structured path.
 *
 * Every value subtree is emitted as one EffectScript expression string and handed to the spec as a single
 * Expression operand, so the builder's own parser validates it exactly as the text form. Binary operations are
 * always fully parenthesized on emit, so operator precedence survives the round-trip through text. Pure and
 * stateless; no I/O.
 */
class FCrowdyEffectGraphCompiler
{
public:
	// Builds the structured spec from the graph's single Result node. An unconnected required input (a write value,
	// a binary-op operand) is recorded as an Error diagnostic, never a crash; a missing Result node is likewise an
	// error and yields an empty spec.
	static FCrowdyEffectSpec CompileToSpec(const UCrowdyEffectGraph* Graph, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics);

	// The same compile, additionally reporting each diagnostic paired with the graph node it came from, so the editor
	// can render on-node error badges. The flat OutDiagnostics is populated identically to CompileToSpec.
	static FCrowdyEffectSpec CompileToSpecWithNodeDiagnostics(
		const UCrowdyEffectGraph* Graph,
		TArray<FCrowdyEffectDiagnostic>& OutDiagnostics,
		TArray<FCrowdyEffectGraphNodeDiagnostic>& OutNodeDiagnostics);

	// Emits the EffectScript expression string produced by the value node feeding SourceOutputPin. Recursive over
	// the whole value-node set: Tuning -> "$Param"; Attribute -> "self."/"source." + attr; Constant -> the literal
	// (numbers/bools bare, strings quoted and escaped, null -> "null"); BinaryOp / Compare / Logic -> a fully
	// parenthesized "(a <glyph> b)"; Unary -> "(<glyph>x)"; If -> "if(c, t, e)"; Call -> "callee(args)" (or
	// "fn:callee(args)"); ReadRef -> "ref(id).attr". Every compound form is fully parenthesized so the emitted text
	// reparses to the exact tree the graph describes, precedence intact. Depth is bounded, so an over-deep graph or
	// a cycle records a diagnostic and returns empty rather than overflowing the stack.
	static FString EmitExpr(const UEdGraphPin* SourceOutputPin, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics, int32 Depth = 0);

	// Convenience for tests and later slices: compile the graph, then run it through the shared builder and lowering,
	// folding any graph / builder diagnostics into the returned result ahead of the lowering diagnostics.
	static FCrowdyEffectLoweringResult CompileToFunction(const UCrowdyEffectGraph* Graph, const FCrowdyEffectLoweringContext& Context);
};
