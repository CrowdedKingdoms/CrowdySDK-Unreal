// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"

/**
 * The result of parsing an EffectScript source: the AST plus any located diagnostics. A parse with any
 * Error-severity diagnostic still returns a best-effort Program (each bad line is skipped), but callers must
 * check HasErrors() before lowering.
 */
struct FCrowdyEffectParseResult
{
	FCrowdyEffectProgram Program;
	TArray<FCrowdyEffectDiagnostic> Diagnostics;

	bool HasErrors() const
	{
		return Diagnostics.ContainsByPredicate(
			[](const FCrowdyEffectDiagnostic& D) { return D.Severity == ECrowdyEffectSeverity::Error; });
	}
};

/**
 * EffectScript front-end: tokenizer + recursive-descent / precedence-climbing parser. Pure and stateless
 * (the single static entry point), so it is trivially unit-testable and reused unchanged by every authoring
 * surface (the text asset, the future structured picker, the optional typed-C++ builder all converge on this
 * AST and the shared CrowdyEffectLowering). No I/O, no UObject, no attribute knowledge validation against a
 * container's real attributes happens in lowering, not here.
 */
class CROWDYREPLICATION_API FCrowdyEffectParser
{
public:
	// Parse EffectScript into an AST + diagnostics. Never throws; syntax errors become Error diagnostics with
	// a 1-based line/column, and parsing resumes at the next line.
	static FCrowdyEffectParseResult Parse(const FString& Source);

	// Parse a SINGLE EffectScript expression (no statements) into one expression tree. Bounded and nesting-guarded
	// by the same depth / node caps as Parse, so forged or deeply-nested input yields a diagnostic, never a stack
	// overflow. Returns an invalid pointer on any error, appending the located diagnostics to OutDiagnostics.
	// Used to splice sub-expression strings authored in a structured spec (Expression / Formula / If / Call) into
	// the shared AST.
	static TSharedPtr<FCrowdyEffectExpr> ParseExpression(const FString& Source, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics);

	// Tokenize a single EffectScript expression into its flat token stream using the exact same lexer Parse uses.
	// Each token carries its 0-based source range (Start/Len). Lexing diagnostics are dropped here; ParseExpression
	// is the entry point that surfaces them. Used by editor tooling to classify and highlight an expression as the
	// designer types. Bounded by the same size cap as Parse: an over-long input yields an empty array. The final
	// two tokens are the synthetic trailing Newline and End the parser relies on; callers that only care about real
	// tokens should ignore those two kinds.
	static TArray<FCrowdyEffectToken> TokenizeExpression(const FString& Source);
};
