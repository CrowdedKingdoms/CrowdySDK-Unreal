// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectTimerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The plain authoring route produces valid expression source on both fields: a fixed delay becomes bare digits (an
// int expression), and a plain dedupe key becomes a QUOTED string literal. The quoting is the point: the server
// parses these as expressions, so an unquoted key would read as an identifier and be rejected at upsert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerPlainAuthoringTest,
	"CrowdySDK.Replication.EffectTimerPlainAuthoring", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerPlainAuthoringTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("start_boss_wave");
	Authored.DelayMs = 30000;
	Authored.DedupeKey = TEXT("boss_wave");

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	TestEqual(TEXT("function name carried"), Wire.FunctionName, FString(TEXT("start_boss_wave")));
	TestEqual(TEXT("fixed delay becomes bare digits"), Wire.DelayMsExpression, FString(TEXT("30000")));
	TestEqual(TEXT("plain dedupe key is quoted as a string literal"),
		Wire.DedupeKeyExpression, FString(TEXT("\"boss_wave\"")));
	TestTrue(TEXT("target left empty means self"), Wire.Target.IsEmpty());
	return true;
}

// A dedupe key containing a quote or a backslash still round-trips as one literal rather than terminating the
// string early and producing an unparseable expression.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerDedupeKeyEscapingTest,
	"CrowdySDK.Replication.EffectTimerDedupeKeyEscaping", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerDedupeKeyEscapingTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("tick");
	Authored.DedupeKey = TEXT("wave\"key\\path");

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	TestEqual(TEXT("embedded quote and backslash are escaped inside the literal"),
		Wire.DedupeKeyExpression, FString(TEXT("\"wave\\\"key\\\\path\"")));
	return true;
}

// An empty dedupe key means "queue an independent fire each time", so no expression is emitted at all rather than
// an empty string literal, which would dedupe every timer in the app against one blank key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerNoDedupeKeyTest,
	"CrowdySDK.Replication.EffectTimerNoDedupeKey", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerNoDedupeKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("tick");
	Authored.DelayMs = 500;

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	TestTrue(TEXT("no dedupe expression emitted"), Wire.DedupeKeyExpression.IsEmpty());
	return true;
}

// The advanced override fields win over the plain ones and are passed through verbatim, so a delay or key computed
// from model state reaches the server unquoted and unmodified.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerExpressionOverridesTest,
	"CrowdySDK.Replication.EffectTimerExpressionOverrides", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerExpressionOverridesTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("respawn");
	Authored.DelayMs = 1000;
	Authored.DedupeKey = TEXT("ignored");
	Authored.DelayExpression = TEXT("self.respawn_delay_ms");
	Authored.DedupeKeyExpression = TEXT("concat(\"respawn:\", $self_container_id)");
	Authored.Target = TEXT("$target_id");

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	TestEqual(TEXT("delay expression overrides the fixed delay"),
		Wire.DelayMsExpression, FString(TEXT("self.respawn_delay_ms")));
	TestEqual(TEXT("dedupe expression overrides the plain key and is NOT re-quoted"),
		Wire.DedupeKeyExpression, FString(TEXT("concat(\"respawn:\", $self_container_id)")));
	TestEqual(TEXT("explicit target carried"), Wire.Target, FString(TEXT("$target_id")));
	return true;
}

// The server caps a function at four declared timers, so a fifth is an authoring-time error rather than a rejected
// upsert discovered during a sync.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerCountLimitTest,
	"CrowdySDK.Replication.EffectTimerCountLimit", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerCountLimitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the documented server ceiling is four"), UCrowdyEffect::MaxTimersPerEffect, 4);
	return true;
}

// Authored params carry a per-attempt snapshot the fired invocation reads as $<name>. Each row lands on the wire
// struct in the order it was authored, with the same start/end trim BuildTimerInput already applies to every
// other timer field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerParamsMappingTest,
	"CrowdySDK.Replication.EffectTimerParamsMapping", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerParamsMappingTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("respawn");

	FCrowdyEffectTimerParam First;
	First.Name = TEXT("  target_hp  ");
	First.Expression = TEXT("  self.hp - 5  ");
	Authored.Params.Add(First);

	FCrowdyEffectTimerParam Second;
	Second.Name = TEXT("cooldown_ms");
	Second.Expression = TEXT("1000");
	Authored.Params.Add(Second);

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	if (TestEqual(TEXT("both params carried"), Wire.Params.Num(), 2))
	{
		TestEqual(TEXT("first param name trimmed"), Wire.Params[0].Name, FString(TEXT("target_hp")));
		TestEqual(TEXT("first param expression trimmed"), Wire.Params[0].Expression, FString(TEXT("self.hp - 5")));
		TestEqual(TEXT("second param name in authored order"), Wire.Params[1].Name, FString(TEXT("cooldown_ms")));
		TestEqual(TEXT("second param expression in authored order"), Wire.Params[1].Expression, FString(TEXT("1000")));
	}
	return true;
}

// Unlike a plain Dedupe Key, a timer param expression is never quoted for the author: it is model expression
// source through and through, so wrapping it in quotes would turn a function call into an unparseable string
// literal server-side. This is the convention closest to being broken by accident, since the sibling Dedupe Key
// field DOES get quoted for a plain value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerParamExpressionVerbatimTest,
	"CrowdySDK.Replication.EffectTimerParamExpressionVerbatim", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerParamExpressionVerbatimTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("respawn");

	FCrowdyEffectTimerParam Param;
	Param.Name = TEXT("snapshot_id");
	Param.Expression = TEXT("concat(\"respawn:\", $self_container_id)");
	Authored.Params.Add(Param);

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	if (TestEqual(TEXT("one param carried"), Wire.Params.Num(), 1))
	{
		TestEqual(TEXT("expression passed through verbatim, not re-quoted"), Wire.Params[0].Expression,
			FString(TEXT("concat(\"respawn:\", $self_container_id)")));
	}
	return true;
}

// A timer param may not shadow a name the effect layer already injects into the invocation (source_id here); doing
// so would make the author's own binding silently unreachable or, worse, collide with the server-injected value.
// This is caught at Compile() time as an Error, before the asset ever reaches a sync.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerReservedParamNameTest,
	"CrowdySDK.Replication.EffectTimerReservedParamName", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerReservedParamNameTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelTestComponent::StaticClass();
	Effect->FunctionName = TEXT("tick");
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.armor -= 1");

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("tick_again");

	FCrowdyEffectTimerParam Param;
	Param.Name = TEXT("source_id");
	Param.Expression = TEXT("1");
	Timer.Params.Add(Param);
	Effect->Timers.Add(Timer);

	const FCrowdyEffectLoweringResult Result = Effect->Compile();

	bool bFoundReservedNameError = false;
	for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Severity == ECrowdyEffectSeverity::Error
			&& Diagnostic.Message.Contains(TEXT("source_id"))
			&& Diagnostic.Message.Contains(TEXT("reserved")))
		{
			bFoundReservedNameError = true;
		}
	}
	TestTrue(TEXT("the reserved param name is rejected as a compile Error"), bFoundReservedNameError);
	TestTrue(TEXT("Compile reports the effect as having errors"), Result.HasErrors());
	return true;
}

// Two rows claiming one name send two values the fired function can only read under a single $name, and the schema
// diff matches a timer's params by name, so a duplicate can also make a real difference compare as no change at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerDuplicateParamNameTest,
	"CrowdySDK.Replication.EffectTimerDuplicateParamName", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerDuplicateParamNameTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelTestComponent::StaticClass();
	Effect->FunctionName = TEXT("tick");
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.armor -= 1");

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("tick_again");

	FCrowdyEffectTimerParam First;
	First.Name = TEXT("expected_team");
	First.Expression = TEXT("self.armor");
	Timer.Params.Add(First);

	FCrowdyEffectTimerParam Second;
	// Trailing whitespace, because the names are compared after trimming: the two rows collide on the wire even
	// though the authored strings differ.
	Second.Name = TEXT("expected_team ");
	Second.Expression = TEXT("1");
	Timer.Params.Add(Second);

	Effect->Timers.Add(Timer);

	const FCrowdyEffectLoweringResult Result = Effect->Compile();

	bool bFoundDuplicateError = false;
	for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Severity == ECrowdyEffectSeverity::Error
			&& Diagnostic.Message.Contains(TEXT("expected_team"))
			&& Diagnostic.Message.Contains(TEXT("twice")))
		{
			bFoundDuplicateError = true;
		}
	}
	TestTrue(TEXT("a name declared twice on one timer is a compile Error"), bFoundDuplicateError);
	TestTrue(TEXT("Compile reports the effect as having errors"), Result.HasErrors());
	return true;
}

// A row whose name is blank is dropped from the wire, so an author who wrote the expression and forgot the name gets
// a value that never arrives. Silence is the wrong answer there: the compile says so instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerParamEmptyNameWarnsTest,
	"CrowdySDK.Replication.EffectTimerParamEmptyNameWarns", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerParamEmptyNameWarnsTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelTestComponent::StaticClass();
	Effect->FunctionName = TEXT("tick");
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.armor -= 1");

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("tick_again");

	FCrowdyEffectTimerParam Nameless;
	Nameless.Expression = TEXT("self.armor");
	Timer.Params.Add(Nameless);

	Effect->Timers.Add(Timer);

	const FCrowdyEffectLoweringResult Result = Effect->Compile();

	bool bFoundWarning = false;
	for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Severity == ECrowdyEffectSeverity::Warning
			&& Diagnostic.Message.Contains(TEXT("no name")))
		{
			bFoundWarning = true;
		}
	}
	TestTrue(TEXT("an expression with no name is warned about"), bFoundWarning);
	// A Warning and not an Error: the effect is still shippable, it just does less than the author wrote.
	TestFalse(TEXT("the effect still compiles"), Result.HasErrors());
	return true;
}

// A row with no expression AND no name is an empty row an author has not filled in yet, so it stays silent: only a
// half-filled row is worth a diagnostic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerFullyEmptyParamIsSilentTest,
	"CrowdySDK.Replication.EffectTimerFullyEmptyParamIsSilent", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerFullyEmptyParamIsSilentTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelTestComponent::StaticClass();
	Effect->FunctionName = TEXT("tick");
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.armor -= 1");

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("tick_again");
	Timer.Params.Add(FCrowdyEffectTimerParam());
	Effect->Timers.Add(Timer);

	const FCrowdyEffectLoweringResult Result = Effect->Compile();

	for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
	{
		TestFalse(TEXT("an untouched param row produces no timer-param diagnostic"),
			Diagnostic.Message.Contains(TEXT("no name")));
	}
	TestFalse(TEXT("the effect still compiles"), Result.HasErrors());
	return true;
}

// No Params authored means nothing to bind, so the wire timer carries an empty array rather than a placeholder
// entry, matching the convention that an unauthored optional field is omitted rather than defaulted on the wire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerEmptyParamsOmittedTest,
	"CrowdySDK.Replication.EffectTimerEmptyParamsOmitted", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerEmptyParamsOmittedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("tick");

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	TestTrue(TEXT("no params authored means no params on the wire"), Wire.Params.IsEmpty());
	return true;
}

// A row with no name has nothing to bind the evaluated value to, so it is dropped rather than reaching the server
// as a nameless parameter it cannot do anything with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerParamEmptyNameSkippedTest,
	"CrowdySDK.Replication.EffectTimerParamEmptyNameSkipped", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerParamEmptyNameSkippedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectTimer Authored;
	Authored.FunctionName = TEXT("tick");

	FCrowdyEffectTimerParam Blank;
	Blank.Expression = TEXT("1");
	Authored.Params.Add(Blank);

	FCrowdyEffectTimerParam Whitespace;
	Whitespace.Name = TEXT("   ");
	Whitespace.Expression = TEXT("2");
	Authored.Params.Add(Whitespace);

	FCrowdyEffectTimerParam Valid;
	Valid.Name = TEXT("hp_snapshot");
	Valid.Expression = TEXT("self.hp");
	Authored.Params.Add(Valid);

	const FCrowdyGameModelTimer Wire = UCrowdyEffect::BuildTimerInput(Authored);

	if (TestEqual(TEXT("only the named row survives"), Wire.Params.Num(), 1))
	{
		TestEqual(TEXT("the surviving row is the named one"), Wire.Params[0].Name, FString(TEXT("hp_snapshot")));
	}
	return true;
}

// Building the same authored timer twice must produce field-identical wire structs, both with and without params.
// This is the layer BuildTimerInput can be pinned at from this module: the omitted-field-becomes-a-server-default
// drift this project has repeatedly hit is a property of the schema-diff comparison itself
// (OneTimerEquals in CrowdyStudio/Private/GameModel/CrowdySchemaSync.cpp), which lives in a different module and
// is not reachable from a CrowdyReplication test. See the report for where that coverage belongs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTimerBuildIsStableTest,
	"CrowdySDK.Replication.EffectTimerBuildIsStable", CrowdyEffectTimerTestFlags)
bool FCrowdyEffectTimerBuildIsStableTest::RunTest(const FString& Parameters)
{
	{
		FCrowdyEffectTimer Authored;
		Authored.FunctionName = TEXT("tick");
		Authored.DelayMs = 250;
		Authored.DedupeKey = TEXT("tick_key");

		const FCrowdyGameModelTimer WireA = UCrowdyEffect::BuildTimerInput(Authored);
		const FCrowdyGameModelTimer WireB = UCrowdyEffect::BuildTimerInput(Authored);

		TestEqual(TEXT("function name stable (no params)"), WireA.FunctionName, WireB.FunctionName);
		TestEqual(TEXT("target stable (no params)"), WireA.Target, WireB.Target);
		TestEqual(TEXT("delay expression stable (no params)"), WireA.DelayMsExpression, WireB.DelayMsExpression);
		TestEqual(TEXT("dedupe expression stable (no params)"), WireA.DedupeKeyExpression, WireB.DedupeKeyExpression);
		TestEqual(TEXT("params stay empty on both builds"), WireA.Params.Num(), WireB.Params.Num());
	}
	{
		FCrowdyEffectTimer Authored;
		Authored.FunctionName = TEXT("respawn");
		Authored.DelayExpression = TEXT("self.respawn_delay_ms");

		FCrowdyEffectTimerParam Param;
		Param.Name = TEXT("hp_snapshot");
		Param.Expression = TEXT("self.hp");
		Authored.Params.Add(Param);

		const FCrowdyGameModelTimer WireA = UCrowdyEffect::BuildTimerInput(Authored);
		const FCrowdyGameModelTimer WireB = UCrowdyEffect::BuildTimerInput(Authored);

		TestEqual(TEXT("delay expression stable (with params)"), WireA.DelayMsExpression, WireB.DelayMsExpression);
		if (TestEqual(TEXT("same param count on both builds"), WireA.Params.Num(), WireB.Params.Num())
			&& TestEqual(TEXT("exactly one param"), WireA.Params.Num(), 1))
		{
			TestEqual(TEXT("param name stable"), WireA.Params[0].Name, WireB.Params[0].Name);
			TestEqual(TEXT("param expression stable"), WireA.Params[0].Expression, WireB.Params[0].Expression);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
