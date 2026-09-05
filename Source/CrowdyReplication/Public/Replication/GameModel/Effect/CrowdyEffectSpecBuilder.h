// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"

struct FCrowdyEffectSpec;

/**
 * Builds the shared EffectScript AST (FCrowdyEffectProgram) from an FCrowdyEffectSpec, the intermediate shape the
 * node-graph compiler lowers a graph to, so graph authoring reaches the exact same FCrowdyEffectLowering as the
 * text form. This is how the invariant "every front-end emits the identical AST" holds: the text parser and this
 * builder are two producers of one AST, and there is only one lowering core downstream. Pure and stateless; no
 * UObject, no I/O.
 *
 * Role mapping is load-bearing: the spec's Target is the affected/bound container (the AST's self) and Source is
 * the instigator (the AST's source). The right-hand-side terms are assembled into a precedence-correct tree
 * using the shared CrowdyEffectBinaryPrecedence, so a spec groups exactly as the equivalent text would.
 * Structural problems (an assignment with no value) are appended as diagnostics, never silently dropped.
 *
 * Beyond the leaf operands (number / attribute / magnitude / raw) the builder also splices sub-expressions
 * authored as text: an Expression operand and the If / Call operands are parsed through the shared
 * FCrowdyEffectParser::ParseExpression and grafted into the AST.
 */
class CROWDYREPLICATION_API FCrowdyEffectSpecBuilder
{
public:
	static FCrowdyEffectProgram BuildProgram(const FCrowdyEffectSpec& Spec, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics);
};
