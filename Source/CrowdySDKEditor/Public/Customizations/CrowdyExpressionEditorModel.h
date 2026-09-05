// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"

/**
 * Pure, headless-testable core for editing a single EffectScript expression string as classified "chips".
 * No Slate, no UObject, no vocabulary discovery: the widget layer (SCrowdyExpressionEditor) and the picker
 * that embeds it inject the attribute / magnitude / function names, so the classification, the diagnostic
 * ranges, and the completion ranking are all unit-tested independent of any Details-panel plumbing.
 *
 * An expression is a single line, so every character range here is a 0-based [Start, Start+Len) offset into
 * that one line. Classification and diagnostics both run over the shared FCrowdyEffectParser lexer/parser, so
 * what the editor highlights matches exactly what the effect actually compiles.
 */

// Produces the diagnostics a real compile finds in a body, for an editor that would otherwise only see syntax.
// It is asked for an answer about a specific body rather than told one, so what is painted always describes the
// text being painted: a diagnostic's line and column only locate anything in the source that produced it.
DECLARE_DELEGATE_RetVal_OneParam(TArray<FCrowdyEffectDiagnostic>, FCrowdyEffectBodyDiagnosticProvider, const FString&);

// The visual class of one chip in an expression. A role-qualified read (self.<attr> / source.<attr>) coalesces
// its base, dot, and attribute into a single Attribute chip; fn:<name> coalesces into one FunctionName chip.
enum class ECrowdyExpressionChipKind : uint8
{
	Identifier,   // a bareword that is neither a language keyword nor a call
	Attribute,    // a self.<attr> / source.<attr> read, spanning the whole access
	Magnitude,    // a $name tuning parameter (the span includes the leading '$')
	Number,       // a numeric literal
	String,       // a quoted string literal (the span includes the quotes)
	Operator,     // an arithmetic / comparison / logical / assignment operator, or a bare dot / colon
	Paren,        // ( or )
	Comma,
	FunctionName, // an identifier immediately followed by '(', or an fn:<name> callee
	Keyword,      // a language keyword or a builtin (self, source, ref, raw, if, max, min, clamp, ...)
	Unknown
};

// One classified span of an expression. Text is the exact source substring [Start, Start+Len).
struct FCrowdyExpressionChip
{
	ECrowdyExpressionChipKind Kind = ECrowdyExpressionChipKind::Unknown;
	int32 Start = 0;
	int32 Len = 0;
	FString Text;
};

// A parser/lexer diagnostic mapped from its 1-based line/column onto a 0-based character range in the single
// expression line, ready to underline. Len is 0 for a diagnostic located at the end of input (a caret marker).
struct FCrowdyExpressionDiagnosticRange
{
	int32 Start = 0;
	int32 Len = 0;
	FString Message;
	bool bIsError = true;
};

// The colour family a highlight run is painted in. Kept free of any Slate type so the run-building stays a pure
// function; the marshaller maps each value to a concrete text style. Whitespace, bare identifiers, and anything
// unlexable fall to Default.
enum class ECrowdyExpressionHighlightColor : uint8
{
	Default,       // plain identifiers, whitespace, unclassified spans
	Keyword,       // language keywords and builtins (self, if, clamp, ...)
	Attribute,     // a self.<attr> / source.<attr> read
	Magnitude,     // a $param tuning value
	Number,        // a numeric literal
	String,        // a quoted string literal
	Operator,      // arithmetic / comparison / logical / assignment operators
	Punctuation,   // parentheses and commas
	FunctionName   // a call callee or fn:<name>
};

// One contiguous span of the expression to paint. BuildHighlightRuns returns these back to back with no gaps, so
// concatenating [Start, Start+Len) over the runs reproduces the whole source string exactly. bError marks a span
// that overlaps a parser error diagnostic, so the marshaller can underline it.
struct FCrowdyExpressionHighlightRun
{
	int32 Start = 0;
	int32 Len = 0;
	ECrowdyExpressionHighlightColor Color = ECrowdyExpressionHighlightColor::Default;
	bool bError = false;
};

// One offered completion. Insert is spliced into the replace range verbatim; Label is what a menu shows.
struct FCrowdyExpressionCompletion
{
	FString Insert;
	FString Label;
	ECrowdyExpressionChipKind Kind = ECrowdyExpressionChipKind::Identifier;
};

// The container-specific names a completion draws on. Injected, never discovered here, so the model stays pure.
struct FCrowdyExpressionVocabulary
{
	// The target container's attributes: what self.<attr> offers, and what source.<attr> offers too whenever the
	// source is another container of the target's own type.
	TArray<FString> Attributes;
	TArray<FString> Magnitudes;
	TArray<FString> Functions;

	// The declared source container type's attributes, offered after source. only. Three states, and they have to
	// stay apart for the same reason the lowering keeps them apart: an effect with no declared source type shares the
	// target's schema, so bSourceSchemaDeclared false sends source.<attr> to Attributes above. Declared and non-empty
	// offers these instead. Declared and EMPTY offers nothing at all, which is the honest answer when the declared
	// type name resolves to no registered class: completing from the target's attributes there would suggest names the
	// source does not have, which is the guessing this exists to prevent.
	TArray<FString> SourceAttributes;
	bool bSourceSchemaDeclared = false;
};

namespace CrowdyExpressionEditorModel
{
	// Classify an expression into an ordered chip list. Whitespace between tokens is not emitted as a chip, so
	// concatenating chip texts (with the original gaps) reproduces the source. Never fails: unlexable characters
	// become Unknown chips.
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionChip> ClassifyChips(const FString& Expression);

	// Parse the expression and map every diagnostic to a 0-based underline range. Errors and warnings both come
	// back (bIsError distinguishes them); the caller decides how to surface each.
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionDiagnosticRange> DiagnoseRanges(const FString& Expression);

	// Partition the expression into back-to-back coloured runs, splitting at every token boundary and every error
	// boundary so each run is one colour and either wholly inside an error range or wholly outside it. Adjacent runs
	// that share a colour and error state are merged, so the output is the minimal contiguous cover of [0, Len). This
	// is the pure core the syntax-highlight marshaller adapts; it never fails.
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionHighlightRun> BuildHighlightRuns(const FString& Expression);

	// The body-aware counterparts of DiagnoseRanges / BuildHighlightRuns. A full EffectScript body is a multi-line
	// statement list, so diagnostics come from the whole-body parser (not the single-expression parser) and each
	// diagnostic's 1-based (line, column) is mapped onto an absolute offset in the multi-line source. Token colouring
	// already spans lines (chip offsets are absolute), so only the diagnostic mapping differs; both share the same
	// run-building core, so the body highlighter underlines exactly the tokens the body parser flags.
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionDiagnosticRange> DiagnoseBodyRanges(const FString& Body);
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionHighlightRun> BuildBodyHighlightRuns(const FString& Body);

	// The same runs, additionally underlining diagnostics the parser cannot produce (see MapBodyDiagnosticsToRanges).
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionHighlightRun> BuildBodyHighlightRuns(
		const FString& Body, const TArray<FCrowdyEffectDiagnostic>& ExtraDiagnostics);

	// Maps already-produced diagnostics onto underline ranges in Body, for diagnostics the parser cannot produce.
	// Anything that needs the container's attributes (an unknown attribute, one attribute spelled two ways, an
	// operator a type cannot take) comes from the lowering, not the parse, so an editor that only parses reports a
	// body as clean when it is not. Same line/column mapping DiagnoseBodyRanges uses, so both underline alike.
	CROWDYSDKEDITOR_API TArray<FCrowdyExpressionDiagnosticRange> MapBodyDiagnosticsToRanges(
		const FString& Body, const TArray<FCrowdyEffectDiagnostic>& Diagnostics);

	// The number of source lines in a body (one more than the count of '\n'; an empty body is one line). Pure, so
	// the editor's line-number gutter is a unit-tested function of the text rather than a Slate-layout side effect.
	CROWDYSDKEDITOR_API int32 CountLines(const FString& Body);

	// The fixed language keywords and builtins the completion offers regardless of container. Stable, shared, and
	// never injected because they are the same for every effect.
	CROWDYSDKEDITOR_API const TArray<FString>& KeywordNames();
	CROWDYSDKEDITOR_API const TArray<FString>& BuiltinNames();

	// The policy keywords valid as bare identifiers in a `require` clause (owner_of_self / host / my_turn /
	// participant / automation), matching ECrowdyEffectPolicyKeyword and what the lowering recognizes. Completion
	// offers these only inside a require statement, since they are meaningless in any other position.
	CROWDYSDKEDITOR_API const TArray<FString>& PolicyKeywordNames();
}

/**
 * Context-aware completion over one expression. Holds the injected vocabulary and, given a caret offset, decides
 * what to offer: attribute names right after self. / source. / a role + '.', magnitude names right after '$',
 * function names right after 'fn:', otherwise the general set (keywords + builtins + every vocabulary). Ranking
 * is prefix matches before substring matches, then alphabetical (case-insensitive). All pure and Slate-free.
 */
class CROWDYSDKEDITOR_API FCrowdyExpressionCompletionSource
{
public:
	FCrowdyExpressionCompletionSource() = default;
	explicit FCrowdyExpressionCompletionSource(FCrowdyExpressionVocabulary InVocabulary);

	// Ranked completions for the caret at Caret (a 0-based offset in [0, Text.Len()]). OutReplaceStart and
	// OutReplaceLen give the range the chosen Insert should replace (the partial word already typed, if any).
	TArray<FCrowdyExpressionCompletion> GetCompletions(const FString& Text, int32 Caret,
		int32& OutReplaceStart, int32& OutReplaceLen) const;

	// Splice Insert into Text over [ReplaceStart, ReplaceStart+ReplaceLen), clamping defensively. Returns the new
	// text; OutNewCaret is the caret position just past the inserted text. Pure and static.
	static FString ApplyCompletion(const FString& Text, int32 ReplaceStart, int32 ReplaceLen,
		const FString& Insert, int32& OutNewCaret);

	const FCrowdyExpressionVocabulary& GetVocabulary() const { return Vocabulary; }

private:
	FCrowdyExpressionVocabulary Vocabulary;
};
