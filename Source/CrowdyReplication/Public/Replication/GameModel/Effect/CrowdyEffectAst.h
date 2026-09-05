// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

/**
 * Token and AST types for EffectScript, the small text surface a designer/programmer writes to author one
 * Game Model function (an "effect"). Header-only, no UObject: an effect is compiled entirely in engine-side
 * code (tokenizer -> parser -> AST -> lowering), so these structs must outlive any UObject and carry no
 * reflection. Shared by the parser (produces them) and the lowering (consumes them). See CrowdyEffectParser
 * and CrowdyEffectLowering.
 *
 * EffectScript surface (one effect = N lines):
 *   - Assignment:  self.Hp -= source.Str + $base_power     (op IS the operation; LHS is self.<Attr> or source.<Attr>)
 *   - Require:     require self.mana >= $cost               (lowers to the invoke policy)
 *   - Return:      return self.hp                           (the value the invocation answers with; at most one)
 *   - '#' begins a line comment; blank lines are ignored.
 * The RHS / require condition is a pure expression: literals, property reads (self./source./ref(...)),
 * $params, the operators + - * / % == != < > <= >= && || !, if(...), the server builtins, fn:<name>(...)
 * calls, and raw("...") verbatim escapes.
 */

enum class ECrowdyEffectSeverity : uint8
{
	Error,
	Warning
};

// A located diagnostic from tokenizing, parsing, or lowering. Line/Col are 1-based; Col 0 means "no column".
struct FCrowdyEffectDiagnostic
{
	ECrowdyEffectSeverity Severity = ECrowdyEffectSeverity::Error;
	int32 Line = 0;
	int32 Col = 0;
	FString Message;

	FString ToString() const
	{
		const TCHAR* Kind = Severity == ECrowdyEffectSeverity::Error ? TEXT("error") : TEXT("warning");
		if (Col > 0)
		{
			return FString::Printf(TEXT("%s (line %d, col %d): %s"), Kind, Line, Col, *Message);
		}
		return FString::Printf(TEXT("%s (line %d): %s"), Kind, Line, *Message);
	}
};

enum class ECrowdyEffectTokenType : uint8
{
	End,
	Number,      // 42, 3.14
	String,      // "text" (Text holds the unescaped content, no surrounding quotes)
	Identifier,  // a bareword: attribute names, keywords (self/source/ref/require/raw/true/false/null/fn/...)
	Param,       // $name (Text holds the name without the '$')
	Dot,
	Comma,
	Colon,       // for fn:<name>
	LParen,
	RParen,
	Assign,      // =
	PlusAssign,  // +=
	MinusAssign, // -=
	StarAssign,  // *=
	SlashAssign, // /=
	Plus,
	Minus,
	Star,
	Slash,
	Percent,
	Eq,          // ==
	NotEq,       // !=
	Lt,
	Gt,
	Le,          // <=
	Ge,          // >=
	AndAnd,      // &&
	OrOr,        // ||
	Bang,        // !
	Newline
};

struct FCrowdyEffectToken
{
	ECrowdyEffectTokenType Type = ECrowdyEffectTokenType::End;
	FString Text;
	int32 Line = 0;
	int32 Col = 0;

	// The token's 0-based character offset in the source and its source-span length. For a Param the span
	// includes the leading '$'; for a String it includes the surrounding quotes and any escapes, so the range
	// covers the token exactly as written even though Text holds the decoded content. Used by editor tooling to
	// highlight a token in place.
	int32 Start = 0;
	int32 Len = 0;
};

enum class ECrowdyEffectExprKind : uint8
{
	NumberLiteral,
	StringLiteral,
	BoolLiteral,
	NullLiteral,
	Param,          // $name
	Identifier,     // a bareword: only meaningful in a require as a policy keyword (owner, my_turn, host, ...)
	PropertyAccess, // self.attr / source.attr / ref(expr).attr
	Unary,          // !x, -x
	Binary,         // a <op> b
	Call,           // builtin(...) or fn:name(...)
	Raw             // raw("verbatim DSL") an operand escape spliced in unchanged
};

// The base of a property read. Self / Source are the two authoring roles; ExplicitRef is ref(<arg>).
enum class ECrowdyEffectRefBase : uint8
{
	SelfRef,
	SourceRef,
	ExplicitRef
};

// One node of an expression tree. A tagged struct (not a class hierarchy) so it stays a plain value type;
// children are shared pointers. Only the fields relevant to Kind are populated.
struct FCrowdyEffectExpr
{
	ECrowdyEffectExprKind Kind = ECrowdyEffectExprKind::NullLiteral;
	int32 Line = 0;
	int32 Col = 0;

	// Payload for leaf kinds: NumberLiteral -> the numeric text; StringLiteral -> the unescaped content;
	// BoolLiteral -> "true"/"false"; Param -> the name (no '$'); Call -> the callee name; Raw -> the verbatim
	// operand text.
	FString Text;

	// PropertyAccess:
	ECrowdyEffectRefBase RefBase = ECrowdyEffectRefBase::SelfRef;
	TSharedPtr<FCrowdyEffectExpr> RefArg; // ExplicitRef only: the argument to ref(...)
	FString Attr;                         // the accessed attribute identifier, as written

	// Unary (operand in Lhs) / Binary:
	FString Op;
	TSharedPtr<FCrowdyEffectExpr> Lhs;
	TSharedPtr<FCrowdyEffectExpr> Rhs;

	// Call:
	bool bIsFnCall = false; // true for fn:<name>(...), false for a builtin like max(...)
	TArray<TSharedPtr<FCrowdyEffectExpr>> Args;
};

enum class ECrowdyEffectStmtKind : uint8
{
	Assignment,
	Require
};

// The compound-assignment operator, which IS the operation lowered onto the target property.
enum class ECrowdyEffectAssignOp : uint8
{
	Set, // =   override
	Add, // +=
	Sub, // -=
	Mul, // *=
	Div  // /=
};

struct FCrowdyEffectStatement
{
	ECrowdyEffectStmtKind Kind = ECrowdyEffectStmtKind::Assignment;
	int32 Line = 0;
	int32 Col = 0;

	// Assignment: the lvalue is TargetBase.TargetAttr (Self or Source only never an explicit ref on the LHS).
	ECrowdyEffectRefBase TargetBase = ECrowdyEffectRefBase::SelfRef;
	FString TargetAttr;
	ECrowdyEffectAssignOp AssignOp = ECrowdyEffectAssignOp::Set;
	TSharedPtr<FCrowdyEffectExpr> Rhs;

	// Require: the boolean condition.
	TSharedPtr<FCrowdyEffectExpr> Condition;
};

// The parsed effect: an ordered statement list. Assignment order is preserved (the server runs mutations
// transactionally, later ones seeing earlier writes).
struct FCrowdyEffectProgram
{
	TArray<FCrowdyEffectStatement> Statements;

	// The value the invocation answers with, or an invalid pointer for an effect that only changes state. It is a
	// slot rather than a statement kind because the server evaluates it AFTER every mutation has run, so it has no
	// position among the statements: a slot also makes "two returns in one effect" unrepresentable rather than a
	// rule to enforce later.
	TSharedPtr<FCrowdyEffectExpr> ReturnExpr;
};

// The binary-operator precedence, shared by the parser (to climb) and the lowering (to re-parenthesize the
// emitted expression). Higher binds tighter; 0 means "not a binary operator". Both sides MUST agree or the
// lowering would mis-parenthesize, so the table lives here exactly once.
inline int32 CrowdyEffectBinaryPrecedence(const FString& Op)
{
	if (Op == TEXT("||")) return 1;
	if (Op == TEXT("&&")) return 2;
	if (Op == TEXT("==") || Op == TEXT("!=") || Op == TEXT("<") || Op == TEXT(">")
		|| Op == TEXT("<=") || Op == TEXT(">=")) return 3;
	if (Op == TEXT("+") || Op == TEXT("-")) return 4;
	if (Op == TEXT("*") || Op == TEXT("/") || Op == TEXT("%")) return 5;
	return 0;
}
