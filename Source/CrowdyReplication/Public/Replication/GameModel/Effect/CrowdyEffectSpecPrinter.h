// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"

struct FCrowdyEffectOperand;
struct FCrowdyEffectTerm;

/**
 * Pure spec-to-text printer: canonicalizes a parsed EffectScript expression (or a structured operand / term
 * chain) back to EffectScript source. It is the inverse of the parser for the syntactic surface, so a golden
 * test can assert print(parse(x)) is stable and a future chip <-> text editor can convert either way. Unlike
 * FCrowdyEffectLowering it resolves nothing (no attribute keys, no clamps, no policy tree): it just renders the
 * AST with minimal, precedence-correct parenthesization. No I/O, no UObject, no context.
 */
class CROWDYREPLICATION_API FCrowdyEffectSpecPrinter
{
public:
	// Render an expression tree to canonical EffectScript. Depth-guarded, so a forged / deeply-nested tree yields
	// a truncated string rather than a stack overflow. An invalid node prints as empty.
	static FString PrintExpression(const TSharedPtr<FCrowdyEffectExpr>& Expr);

	// Render a single structured operand to its EffectScript fragment (for a chip preview). An Expression operand
	// is parsed and re-printed canonically; if it does not parse, its literal text is returned unchanged.
	static FString PrintOperand(const FCrowdyEffectOperand& Operand);

	// Render a term chain (the right-hand side of a structured assignment) as a flat left-to-right EffectScript
	// fragment, joining each operand with its binary operator. This is a readable preview, not a re-parenthesized
	// tree; lowering is what groups by precedence.
	static FString PrintTermChain(const TArray<FCrowdyEffectTerm>& Terms);
};
