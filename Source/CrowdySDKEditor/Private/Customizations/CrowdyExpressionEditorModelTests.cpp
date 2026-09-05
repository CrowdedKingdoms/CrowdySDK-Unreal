// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyExpressionEditorModel.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyExpressionEditorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Joins the completion inserts into one comma-separated string so ranking order is a single stable comparison
	// (the automation framework has no TArray equality overload).
	FString CompletionInserts(const TArray<FCrowdyExpressionCompletion>& Completions)
	{
		TArray<FString> Inserts;
		for (const FCrowdyExpressionCompletion& C : Completions)
		{
			Inserts.Add(C.Insert);
		}
		return FString::Join(Inserts, TEXT(","));
	}
}

// A mixed expression classifies into role-qualified attribute chips (self./source. coalesced), a magnitude chip
// carrying its '$', and operator chips, each with the exact source range so the widget can highlight in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionChipTokenClassifyTest,
	"CrowdySDK.Editor.ExpressionChipTokenClassify", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionChipTokenClassifyTest::RunTest(const FString& Parameters)
{
	const FString Expr = TEXT("self.hp - $amount * source.power");
	const TArray<FCrowdyExpressionChip> Chips = CrowdyExpressionEditorModel::ClassifyChips(Expr);

	if (!TestEqual(TEXT("chip count"), Chips.Num(), 5))
	{
		return false;
	}

	TestEqual(TEXT("chip 0 is a self attribute"), static_cast<int32>(Chips[0].Kind),
		static_cast<int32>(ECrowdyExpressionChipKind::Attribute));
	TestEqual(TEXT("chip 0 text"), Chips[0].Text, FString(TEXT("self.hp")));
	TestEqual(TEXT("chip 0 start"), Chips[0].Start, 0);
	TestEqual(TEXT("chip 0 len"), Chips[0].Len, 7);

	TestEqual(TEXT("chip 1 is an operator"), static_cast<int32>(Chips[1].Kind),
		static_cast<int32>(ECrowdyExpressionChipKind::Operator));
	TestEqual(TEXT("chip 1 text"), Chips[1].Text, FString(TEXT("-")));

	TestEqual(TEXT("chip 2 is a magnitude"), static_cast<int32>(Chips[2].Kind),
		static_cast<int32>(ECrowdyExpressionChipKind::Magnitude));
	TestEqual(TEXT("chip 2 text keeps the sigil"), Chips[2].Text, FString(TEXT("$amount")));

	TestEqual(TEXT("chip 3 is an operator"), static_cast<int32>(Chips[3].Kind),
		static_cast<int32>(ECrowdyExpressionChipKind::Operator));
	TestEqual(TEXT("chip 3 text"), Chips[3].Text, FString(TEXT("*")));

	TestEqual(TEXT("chip 4 is a source attribute"), static_cast<int32>(Chips[4].Kind),
		static_cast<int32>(ECrowdyExpressionChipKind::Attribute));
	TestEqual(TEXT("chip 4 text"), Chips[4].Text, FString(TEXT("source.power")));
	TestEqual(TEXT("chip 4 start"), Chips[4].Start, 20);
	TestEqual(TEXT("chip 4 len"), Chips[4].Len, 12);

	// A function call classifies the callee as a FunctionName and the '(' as a Paren.
	const TArray<FCrowdyExpressionChip> CallChips = CrowdyExpressionEditorModel::ClassifyChips(TEXT("max(self.hp, 5)"));
	if (TestTrue(TEXT("call has chips"), CallChips.Num() >= 2))
	{
		TestEqual(TEXT("callee is a function name"), static_cast<int32>(CallChips[0].Kind),
			static_cast<int32>(ECrowdyExpressionChipKind::FunctionName));
		TestEqual(TEXT("callee text"), CallChips[0].Text, FString(TEXT("max")));
		TestEqual(TEXT("open paren"), static_cast<int32>(CallChips[1].Kind),
			static_cast<int32>(ECrowdyExpressionChipKind::Paren));
	}

	return true;
}

// A malformed expression's parser diagnostic maps to the exact 0-based character range of the offending token.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionChipDiagnosticsRangeTest,
	"CrowdySDK.Editor.ExpressionChipDiagnosticsRange", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionChipDiagnosticsRangeTest::RunTest(const FString& Parameters)
{
	// The '<' where a value is expected fails at column 10 (0-based offset 9), a single-character token.
	const TArray<FCrowdyExpressionDiagnosticRange> Ranges =
		CrowdyExpressionEditorModel::DiagnoseRanges(TEXT("self.hp >< 5"));

	if (!TestTrue(TEXT("at least one diagnostic"), Ranges.Num() >= 1))
	{
		return false;
	}
	TestEqual(TEXT("range start is the offending column"), Ranges[0].Start, 9);
	TestEqual(TEXT("range covers the single-character token"), Ranges[0].Len, 1);
	TestTrue(TEXT("severity is an error"), Ranges[0].bIsError);
	TestTrue(TEXT("message is non-empty"), !Ranges[0].Message.IsEmpty());

	// A well-formed expression produces no diagnostics.
	const TArray<FCrowdyExpressionDiagnosticRange> Clean =
		CrowdyExpressionEditorModel::DiagnoseRanges(TEXT("self.hp - $amount"));
	TestEqual(TEXT("clean expression has no ranges"), Clean.Num(), 0);

	return true;
}

// Completions are context-aware and ranked prefix-before-substring, then alphabetically, in each of the three
// sigil contexts (self. / $ / fn:).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionCompletionRankingTest,
	"CrowdySDK.Editor.ExpressionCompletionRanking", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionCompletionRankingTest::RunTest(const FString& Parameters)
{
	FCrowdyExpressionVocabulary Vocabulary;
	Vocabulary.Attributes = { TEXT("harmony"), TEXT("health"), TEXT("power"), TEXT("superpower"), TEXT("hp") };
	Vocabulary.Magnitudes = { TEXT("amount"), TEXT("armor"), TEXT("mana") };
	Vocabulary.Functions = { TEXT("heal"), TEXT("harm"), TEXT("flourish") };
	const FCrowdyExpressionCompletionSource Source(Vocabulary);

	int32 ReplaceStart = 0;
	int32 ReplaceLen = 0;

	// After "self.p": prefix match (power) before substring matches (hp, superpower), alphabetical within a rank.
	{
		const FString Text = TEXT("self.p");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestEqual(TEXT("attribute completions ranked"), Got, FString(TEXT("power,hp,superpower")));
		TestEqual(TEXT("attribute replace start"), ReplaceStart, 5);
		TestEqual(TEXT("attribute replace len"), ReplaceLen, 1);
	}

	// After "$a": magnitude names, prefix (amount, armor) before substring (mana).
	{
		const FString Text = TEXT("$a");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestEqual(TEXT("magnitude completions ranked"), Got, FString(TEXT("amount,armor,mana")));
		TestEqual(TEXT("magnitude replace start"), ReplaceStart, 1);
	}

	// After "fn:h": function names, prefix (harm, heal) before substring (flourish).
	{
		const FString Text = TEXT("fn:h");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestEqual(TEXT("function completions ranked"), Got, FString(TEXT("harm,heal,flourish")));
		TestEqual(TEXT("function replace start"), ReplaceStart, 3);
	}

	// An empty attribute partial ("self.") offers every attribute alphabetically with a zero-length replace range.
	{
		const FString Text = TEXT("self.");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestEqual(TEXT("all attributes alphabetical"), Got, FString(TEXT("harmony,health,hp,power,superpower")));
		TestEqual(TEXT("empty partial replace len"), ReplaceLen, 0);
		TestEqual(TEXT("empty partial replace start"), ReplaceStart, 5);
	}

	return true;
}

// Which container's attributes a completion offers follows the base before the dot. An effect whose source is a
// different container type than its target has two schemas in play, and offering the target's names after `source.`
// suggests attributes the source does not have, which is the guessing the typed source container exists to remove.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionCompletionFollowsTheBaseTest,
	"CrowdySDK.Editor.ExpressionCompletionFollowsTheBase", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionCompletionFollowsTheBaseTest::RunTest(const FString& Parameters)
{
	int32 ReplaceStart = 0;
	int32 ReplaceLen = 0;

	FCrowdyExpressionVocabulary Declared;
	Declared.Attributes = { TEXT("health"), TEXT("shield") };
	Declared.SourceAttributes = { TEXT("charge"), TEXT("chargerate") };
	Declared.bSourceSchemaDeclared = true;
	const FCrowdyExpressionCompletionSource WithSource(Declared);

	{
		const FString Text = TEXT("source.");
		TestEqual(TEXT("a declared source type offers its own attributes"),
			CompletionInserts(WithSource.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString(TEXT("charge,chargerate")));
	}
	{
		const FString Text = TEXT("self.");
		TestEqual(TEXT("self still offers the target's attributes"),
			CompletionInserts(WithSource.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString(TEXT("health,shield")));
	}
	{
		// A base that is neither self nor source names a container of unknown type, which the lowering checks against
		// the target's attributes, so the suggestions have to agree with it.
		const FString Text = TEXT("other.");
		TestEqual(TEXT("an unrecognised base falls to the target's attributes"),
			CompletionInserts(WithSource.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString(TEXT("health,shield")));
	}

	// No declared source type is the state every effect authored before the field existed is in: the source is another
	// container of the target's own type, so source.<attr> must keep completing from the target's attributes.
	FCrowdyExpressionVocabulary Undeclared;
	Undeclared.Attributes = { TEXT("health"), TEXT("shield") };
	Undeclared.SourceAttributes = { TEXT("charge") };
	const FCrowdyExpressionCompletionSource NoSource(Undeclared);
	{
		const FString Text = TEXT("source.");
		TestEqual(TEXT("an undeclared source schema completes from the target"),
			CompletionInserts(NoSource.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString(TEXT("health,shield")));
	}

	// Declared but resolving to nothing (a type name no class carries) offers NOTHING. Falling back to the target's
	// attributes here would be the wrong suggestion rather than a missing one.
	FCrowdyExpressionVocabulary Unresolved;
	Unresolved.Attributes = { TEXT("health"), TEXT("shield") };
	Unresolved.bSourceSchemaDeclared = true;
	const FCrowdyExpressionCompletionSource Unresolvable(Unresolved);
	{
		const FString Text = TEXT("source.");
		TestEqual(TEXT("a declared type that resolves to nothing offers nothing"),
			CompletionInserts(Unresolvable.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString());
	}

	// The partial after the dot still filters, and the replace range still covers only the partial.
	{
		const FString Text = TEXT("source.ch");
		TestEqual(TEXT("a source partial filters the source attributes"),
			CompletionInserts(WithSource.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen)),
			FString(TEXT("charge,chargerate")));
		TestEqual(TEXT("source partial replace start"), ReplaceStart, 7);
		TestEqual(TEXT("source partial replace len"), ReplaceLen, 2);
	}
	return true;
}

// ApplyCompletion splices the chosen insertion over the replace range and reports the caret just past it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionCompletionInsertionTest,
	"CrowdySDK.Editor.ExpressionCompletionInsertion", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionCompletionInsertionTest::RunTest(const FString& Parameters)
{
	int32 NewCaret = 0;

	// Replace the partial "he" at the end with "health".
	{
		const FString Result = FCrowdyExpressionCompletionSource::ApplyCompletion(TEXT("self.he"), 5, 2, TEXT("health"), NewCaret);
		TestEqual(TEXT("end insertion text"), Result, FString(TEXT("self.health")));
		TestEqual(TEXT("end insertion caret"), NewCaret, 11);
	}

	// Replace a partial in the middle, keeping the trailing text intact.
	{
		const FString Result =
			FCrowdyExpressionCompletionSource::ApplyCompletion(TEXT("self.he + $amount"), 5, 2, TEXT("health"), NewCaret);
		TestEqual(TEXT("mid insertion text"), Result, FString(TEXT("self.health + $amount")));
		TestEqual(TEXT("mid insertion caret"), NewCaret, 11);
	}

	// A zero-length replace inserts at the caret.
	{
		const FString Result = FCrowdyExpressionCompletionSource::ApplyCompletion(TEXT("self."), 5, 0, TEXT("power"), NewCaret);
		TestEqual(TEXT("insert at caret text"), Result, FString(TEXT("self.power")));
		TestEqual(TEXT("insert at caret caret"), NewCaret, 10);
	}

	// An out-of-range replace range is clamped rather than crashing.
	{
		const FString Result = FCrowdyExpressionCompletionSource::ApplyCompletion(TEXT("hp"), 100, 50, TEXT("x"), NewCaret);
		TestEqual(TEXT("clamped insertion text"), Result, FString(TEXT("hpx")));
		TestEqual(TEXT("clamped insertion caret"), NewCaret, 3);
	}

	return true;
}

// BuildHighlightRuns partitions an expression into back-to-back coloured runs: each token gets its colour, the gaps
// between tokens are Default, and the whole [0, Len) range is covered contiguously so the marshaller can paint it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionHighlightRunsTest,
	"CrowdySDK.Editor.ExpressionHighlightRuns", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionHighlightRunsTest::RunTest(const FString& Parameters)
{
	const FString Expr = TEXT("self.hp - $amount * source.power");
	const TArray<FCrowdyExpressionHighlightRun> Runs = CrowdyExpressionEditorModel::BuildHighlightRuns(Expr);

	// self.hp | ws | - | ws | $amount | ws | * | ws | source.power
	if (!TestEqual(TEXT("run count"), Runs.Num(), 9))
	{
		return false;
	}

	TestEqual(TEXT("run 0 is an attribute"), static_cast<int32>(Runs[0].Color),
		static_cast<int32>(ECrowdyExpressionHighlightColor::Attribute));
	TestEqual(TEXT("run 0 start"), Runs[0].Start, 0);
	TestEqual(TEXT("run 0 len"), Runs[0].Len, 7);

	TestEqual(TEXT("run 1 is default whitespace"), static_cast<int32>(Runs[1].Color),
		static_cast<int32>(ECrowdyExpressionHighlightColor::Default));

	TestEqual(TEXT("run 2 is an operator"), static_cast<int32>(Runs[2].Color),
		static_cast<int32>(ECrowdyExpressionHighlightColor::Operator));

	TestEqual(TEXT("run 4 is a magnitude"), static_cast<int32>(Runs[4].Color),
		static_cast<int32>(ECrowdyExpressionHighlightColor::Magnitude));
	TestEqual(TEXT("run 4 start"), Runs[4].Start, 10);
	TestEqual(TEXT("run 4 len"), Runs[4].Len, 7);

	TestEqual(TEXT("run 8 is an attribute"), static_cast<int32>(Runs[8].Color),
		static_cast<int32>(ECrowdyExpressionHighlightColor::Attribute));
	TestEqual(TEXT("run 8 start"), Runs[8].Start, 20);
	TestEqual(TEXT("run 8 len"), Runs[8].Len, 12);

	// The runs cover the whole line with no gaps or overlaps, and none is an error span.
	int32 Cursor = 0;
	bool bAnyError = false;
	for (const FCrowdyExpressionHighlightRun& Run : Runs)
	{
		TestEqual(TEXT("runs are contiguous"), Run.Start, Cursor);
		Cursor = Run.Start + Run.Len;
		bAnyError |= Run.bError;
	}
	TestEqual(TEXT("runs cover the whole expression"), Cursor, Expr.Len());
	TestFalse(TEXT("a clean expression has no error runs"), bAnyError);

	// A function call colours the callee as a FunctionName, the parens as Punctuation, and a literal as a Number.
	const TArray<FCrowdyExpressionHighlightRun> CallRuns =
		CrowdyExpressionEditorModel::BuildHighlightRuns(TEXT("max(self.hp, 5)"));
	if (TestTrue(TEXT("call has runs"), CallRuns.Num() >= 1))
	{
		TestEqual(TEXT("callee is a function name"), static_cast<int32>(CallRuns[0].Color),
			static_cast<int32>(ECrowdyExpressionHighlightColor::FunctionName));

		bool bHasNumber = false;
		for (const FCrowdyExpressionHighlightRun& Run : CallRuns)
		{
			bHasNumber |= (Run.Color == ECrowdyExpressionHighlightColor::Number);
		}
		TestTrue(TEXT("the literal is a number run"), bHasNumber);
	}

	// An empty expression yields no runs.
	TestEqual(TEXT("empty expression has no runs"),
		CrowdyExpressionEditorModel::BuildHighlightRuns(FString()).Num(), 0);

	return true;
}

// A parse error marks exactly the offending span as an error run, splitting it out from its neighbours so the
// marshaller underlines only that token.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionHighlightErrorUnderlineTest,
	"CrowdySDK.Editor.ExpressionHighlightErrorUnderline", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionHighlightErrorUnderlineTest::RunTest(const FString& Parameters)
{
	// The '<' at 0-based offset 9 is where a value is expected, so it is the error span.
	const FString Expr = TEXT("self.hp >< 5");
	const TArray<FCrowdyExpressionHighlightRun> Runs = CrowdyExpressionEditorModel::BuildHighlightRuns(Expr);

	const FCrowdyExpressionHighlightRun* ErrorRun = nullptr;
	const FCrowdyExpressionHighlightRun* SpanAtNine = nullptr;
	int32 Cursor = 0;
	for (const FCrowdyExpressionHighlightRun& Run : Runs)
	{
		TestEqual(TEXT("runs are contiguous"), Run.Start, Cursor);
		Cursor = Run.Start + Run.Len;
		if (Run.bError && ErrorRun == nullptr)
		{
			ErrorRun = &Run;
		}
		if (Run.Start <= 9 && 9 < Run.Start + Run.Len)
		{
			SpanAtNine = &Run;
		}
	}
	TestEqual(TEXT("runs cover the whole expression"), Cursor, Expr.Len());

	if (TestNotNull(TEXT("an error run exists"), ErrorRun)
		&& TestNotNull(TEXT("the span at the offending column exists"), SpanAtNine))
	{
		TestTrue(TEXT("the offending column is the error span"), SpanAtNine->bError);
		TestEqual(TEXT("the error span is the single offending character"), SpanAtNine->Len, 1);
	}

	return true;
}

// A multi-line EffectScript body maps a parser diagnostic on a later line onto the correct absolute offset: the
// single-line DiagnoseRanges would place a line-2 error near the top; DiagnoseBodyRanges resolves it against where
// line 2 actually starts, so the underline lands on the offending character.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionBodyDiagnosticsLineMappingTest,
	"CrowdySDK.Editor.ExpressionBodyDiagnosticsLineMapping", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionBodyDiagnosticsLineMappingTest::RunTest(const FString& Parameters)
{
	// Line 1 is a clean assignment (13 chars, so line 2 starts at absolute offset 14); line 2's right-hand side has a
	// stray '<' where a value is expected.
	const FString Body = TEXT("self.mana = 5\nself.hp = self.hp >< 5");
	const int32 Line2Start = 14;

	const TArray<FCrowdyExpressionDiagnosticRange> Ranges = CrowdyExpressionEditorModel::DiagnoseBodyRanges(Body);
	if (!TestTrue(TEXT("at least one diagnostic"), Ranges.Num() >= 1))
	{
		return false;
	}

	TestTrue(TEXT("the error is mapped onto line 2, not line 1"), Ranges[0].Start >= Line2Start);
	TestTrue(TEXT("severity is an error"), Ranges[0].bIsError);
	TestEqual(TEXT("the underline covers the offending '<'"),
		Body.Mid(Ranges[0].Start, Ranges[0].Len), FString(TEXT("<")));

	// A clean multi-line body produces no diagnostics.
	const FString Clean = TEXT("self.hp = self.hp - $damage\nself.mana = self.mana + 5");
	TestEqual(TEXT("clean body has no ranges"), CrowdyExpressionEditorModel::DiagnoseBodyRanges(Clean).Num(), 0);

	return true;
}

// Body highlighting colours a token on a later line by its absolute offset (chip ranges are already whole-body), and
// a clean body has no error runs; the error variant underlines exactly the offending span on its line.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionBodyHighlightRunsTest,
	"CrowdySDK.Editor.ExpressionBodyHighlightRuns", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionBodyHighlightRunsTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("self.hp = self.hp - $damage\nself.mana = self.mana + 5");
	const TArray<FCrowdyExpressionHighlightRun> Runs = CrowdyExpressionEditorModel::BuildBodyHighlightRuns(Body);

	// The runs cover the whole body contiguously and none is an error span.
	int32 Cursor = 0;
	bool bAnyError = false;
	bool bMagnitudeSeen = false;
	for (const FCrowdyExpressionHighlightRun& Run : Runs)
	{
		TestEqual(TEXT("runs are contiguous"), Run.Start, Cursor);
		Cursor = Run.Start + Run.Len;
		bAnyError |= Run.bError;
		bMagnitudeSeen |= (Run.Color == ECrowdyExpressionHighlightColor::Magnitude);
	}
	TestEqual(TEXT("runs cover the whole body"), Cursor, Body.Len());
	TestFalse(TEXT("a clean body has no error runs"), bAnyError);
	TestTrue(TEXT("the $damage magnitude on line 1 is coloured"), bMagnitudeSeen);

	// An attribute on line 2 (source.mana style) is coloured as an attribute at its absolute offset.
	bool bLine2Attribute = false;
	for (const FCrowdyExpressionHighlightRun& Run : Runs)
	{
		if (Run.Start >= 28 && Run.Color == ECrowdyExpressionHighlightColor::Attribute)
		{
			bLine2Attribute = true;
			break;
		}
	}
	TestTrue(TEXT("a line-2 attribute is coloured at its absolute offset"), bLine2Attribute);

	// The error variant marks the offending '<' span on line 2 as an error run.
	const FString BadBody = TEXT("self.mana = 5\nself.hp = self.hp >< 5");
	const TArray<FCrowdyExpressionHighlightRun> BadRuns = CrowdyExpressionEditorModel::BuildBodyHighlightRuns(BadBody);
	bool bErrorSpanIsAngle = false;
	for (const FCrowdyExpressionHighlightRun& Run : BadRuns)
	{
		if (Run.bError && BadBody.Mid(Run.Start, Run.Len) == TEXT("<"))
		{
			bErrorSpanIsAngle = true;
			break;
		}
	}
	TestTrue(TEXT("the error run underlines the '<' on line 2"), bErrorSpanIsAngle);

	return true;
}

// Policy keywords (host, owner_of_self, ...) are offered only inside a `require` statement, and never in an ordinary
// expression position.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionRequirePolicyKeywordsTest,
	"CrowdySDK.Editor.ExpressionRequirePolicyKeywords", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionRequirePolicyKeywordsTest::RunTest(const FString& Parameters)
{
	FCrowdyExpressionVocabulary Vocabulary;
	Vocabulary.Attributes = { TEXT("health"), TEXT("harmony") };
	const FCrowdyExpressionCompletionSource Source(Vocabulary);

	int32 ReplaceStart = 0;
	int32 ReplaceLen = 0;

	// After "require h": the policy keyword "host" and the attribute "health"/"harmony" (both prefix-match 'h'); "host"
	// must be present.
	{
		const FString Text = TEXT("require h");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestTrue(TEXT("require context offers host"), Got.Contains(TEXT("host")));
	}

	// "require own" surfaces owner_of_self.
	{
		const FString Text = TEXT("require own");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestTrue(TEXT("require context offers owner_of_self"), Got.Contains(TEXT("owner_of_self")));
	}

	// The policy keywords are only for require: an assignment line offers none of them.
	{
		const FString Text = TEXT("self.hp = h");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestFalse(TEXT("a non-require line does not offer host"), Got.Contains(TEXT("host")));
	}

	// Require-context detection is per line: a require on line 1 does not leak policy keywords into an assignment on
	// line 2.
	{
		const FString Text = TEXT("require host\nself.hp = h");
		const FString Got = CompletionInserts(Source.GetCompletions(Text, Text.Len(), ReplaceStart, ReplaceLen));
		TestFalse(TEXT("line 2 (assignment) does not offer host"), Got.Contains(TEXT("host")));
	}

	return true;
}

// CountLines is one more than the number of newlines, and an empty body is a single line.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExpressionCountLinesTest,
	"CrowdySDK.Editor.ExpressionCountLines", CrowdyExpressionEditorTestFlags)
bool FCrowdyExpressionCountLinesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty body is one line"), CrowdyExpressionEditorModel::CountLines(FString()), 1);
	TestEqual(TEXT("single line"), CrowdyExpressionEditorModel::CountLines(TEXT("self.hp = 5")), 1);
	TestEqual(TEXT("three lines"), CrowdyExpressionEditorModel::CountLines(TEXT("a\nb\nc")), 3);
	TestEqual(TEXT("a trailing newline opens a new (empty) line"), CrowdyExpressionEditorModel::CountLines(TEXT("a\n")), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
