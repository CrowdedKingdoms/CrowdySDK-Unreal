#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Gql/CrowdyStudioGqlTestSupport.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Model/CrowdyStudioTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelLintGqlTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioModelLintParseTest,
	"CrowdySDK.CrowdyStudio.ModelLintParse", CrowdyModelLintGqlTestFlags)
bool FCrowdyStudioModelLintParseTest::RunTest(const FString& Parameters)
{
	// A model with one error and one warning. Every field a finding can carry is read, remedy included: it is the
	// only part that says what to do, and a Problems line without it sends the developer back to the docs.
	{
		const TSharedPtr<FJsonObject> Envelope = CrowdyStudioGqlTest::ParseEnvelope(TEXT(
			"{\"data\":{\"gameModelLint\":{\"appId\":\"77\",\"errorCount\":1,\"warningCount\":1,\"clean\":false,"
			"\"findings\":["
			"{\"code\":\"container_type_undefined\",\"severity\":\"ERROR\",\"subjectKind\":\"container_type\","
			"\"subject\":\"Player\",\"message\":\"40 containers name a type nobody defined.\","
			"\"remedy\":\"Re-run gameModelSeed.\",\"count\":40},"
			"{\"code\":\"function_not_defined\",\"severity\":\"WARNING\",\"subjectKind\":\"function\","
			"\"subject\":\"on_join\",\"message\":\"calls apply_bonus, which does not exist.\"}"
			"]}}}"));

		FStudioLintReport Report;
		TestTrue(TEXT("a lint envelope parses"),
			CrowdyStudioGql::ParseModelLint(Envelope, TEXT("gameModelLint"), Report));

		TestTrue(TEXT("the report knows an answer arrived"), Report.bRan);
		TestEqual(TEXT("the app id is read as a BigInt string"), Report.AppId, static_cast<int64>(77));
		TestEqual(TEXT("the error count is the server's"), Report.ErrorCount, 1);
		TestEqual(TEXT("the warning count is the server's"), Report.WarningCount, 1);
		TestFalse(TEXT("an app with an error is not clean"), Report.bClean);

		if (TestEqual(TEXT("both findings are read"), Report.Findings.Num(), 2))
		{
			TestTrue(TEXT("the first finding is the error"), Report.Findings[0].IsError());
			TestEqual(TEXT("its subject is the object's own name"), Report.Findings[0].Subject, FString(TEXT("Player")));
			TestEqual(TEXT("its remedy is carried"), Report.Findings[0].Remedy, FString(TEXT("Re-run gameModelSeed.")));
			TestEqual(TEXT("a summarising row carries how many it stands for"), Report.Findings[0].Count, 40);

			// A warning is not an error, which is the whole point of the split: an app being authored normally is
			// full of these and a tool that treated them alike would be one nobody reads.
			TestFalse(TEXT("the second finding is not an error"), Report.Findings[1].IsError());
			// Absent rather than zero-valued fields stay empty rather than being invented.
			TestTrue(TEXT("an absent remedy stays empty"), Report.Findings[1].Remedy.IsEmpty());
			TestEqual(TEXT("an absent count stays zero"), Report.Findings[1].Count, 0);
		}
	}

	// A clean model. bClean and bRan are different claims and this is the case that separates them: an app with
	// nothing wrong reports zero findings, which is exactly what an app that was never linted also holds.
	{
		const TSharedPtr<FJsonObject> Envelope = CrowdyStudioGqlTest::ParseEnvelope(TEXT(
			"{\"data\":{\"gameModelLint\":{\"appId\":\"1\",\"errorCount\":0,\"warningCount\":0,\"clean\":true,"
			"\"findings\":[]}}}"));

		FStudioLintReport Report;
		TestTrue(TEXT("a clean lint envelope parses"),
			CrowdyStudioGql::ParseModelLint(Envelope, TEXT("gameModelLint"), Report));
		TestTrue(TEXT("clean is read"), Report.bClean);
		TestTrue(TEXT("and it is distinguishable from never having run"), Report.bRan);
		TestEqual(TEXT("with no findings"), Report.Findings.Num(), 0);
	}

	// Case sensitivity: the severity comparison is deliberately case-insensitive, so a server that ever answers in
	// a different case does not silently reclassify every error as a warning.
	{
		const TSharedPtr<FJsonObject> Envelope = CrowdyStudioGqlTest::ParseEnvelope(TEXT(
			"{\"data\":{\"gameModelLint\":{\"appId\":\"1\",\"errorCount\":1,\"warningCount\":0,\"clean\":false,"
			"\"findings\":[{\"code\":\"x\",\"severity\":\"error\",\"subjectKind\":\"function\",\"subject\":\"f\","
			"\"message\":\"m\"}]}}}"));

		FStudioLintReport Report;
		TestTrue(TEXT("a lowercase severity parses"),
			CrowdyStudioGql::ParseModelLint(Envelope, TEXT("gameModelLint"), Report));
		if (TestEqual(TEXT("the finding is read"), Report.Findings.Num(), 1))
		{
			TestTrue(TEXT("a lowercase ERROR is still an error"), Report.Findings[0].IsError());
		}
	}

	// A non-object element inside findings is skipped rather than read as an empty finding, which would show up in
	// a Problems list as a blank row about nothing.
	{
		const TSharedPtr<FJsonObject> Envelope = CrowdyStudioGqlTest::ParseEnvelope(TEXT(
			"{\"data\":{\"gameModelLint\":{\"appId\":\"1\",\"errorCount\":0,\"warningCount\":1,\"clean\":true,"
			"\"findings\":[\"nonsense\",{\"code\":\"c\",\"severity\":\"WARNING\",\"subjectKind\":\"function\","
			"\"subject\":\"f\",\"message\":\"m\"}]}}}"));

		FStudioLintReport Report;
		TestTrue(TEXT("a findings list with junk in it still parses"),
			CrowdyStudioGql::ParseModelLint(Envelope, TEXT("gameModelLint"), Report));
		TestEqual(TEXT("only the real finding is kept"), Report.Findings.Num(), 1);
		TestEqual(TEXT("and it is the one that was an object"), Report.Findings[0].Subject, FString(TEXT("f")));
	}

	// A response carrying no lint node at all is a failure, not an empty clean report. Reporting it as clean would
	// tell a developer their model is fine on the strength of an answer the server never gave.
	{
		FStudioLintReport Report;
		TestFalse(TEXT("an envelope with no lint node is refused"),
			CrowdyStudioGql::ParseModelLint(CrowdyStudioGqlTest::ParseEnvelope(TEXT("{\"data\":{}}")), TEXT("gameModelLint"), Report));
		TestFalse(TEXT("and nothing is claimed to have run"), Report.bRan);
		TestFalse(TEXT("nor claimed clean"), Report.bClean);
	}

	return true;
}

// What the Issues tab reads to decide whether it can be opened at all, and what it says on hover when it cannot.
// The three states are the point: two of them hold an empty findings list and mean opposite things.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioLintTabStateTest,
	"CrowdySDK.CrowdyStudio.ModelLintTabState", CrowdyModelLintGqlTestFlags)
bool FCrowdyStudioLintTabStateTest::RunTest(const FString& Parameters)
{
	// Never linted. A default report and a clean one are byte-identical apart from bRan, and this is the case that
	// must not be reported as "no issues": nobody has asked yet.
	{
		const FStudioLintReport Report;
		TestEqual(TEXT("a report that never ran is NeverRun"),
			static_cast<int32>(CrowdyLintTabStateFor(Report)), static_cast<int32>(ECrowdyLintTabState::NeverRun));
	}

	// Linted, nothing wrong.
	{
		FStudioLintReport Report;
		Report.bRan = true;
		Report.bClean = true;
		TestEqual(TEXT("a report that ran with no findings is Clean"),
			static_cast<int32>(CrowdyLintTabStateFor(Report)), static_cast<int32>(ECrowdyLintTabState::Clean));
	}

	// An error opens the tab.
	{
		FStudioLintReport Report;
		Report.bRan = true;
		Report.bClean = false;
		Report.ErrorCount = 1;
		FStudioLintFinding& Finding = Report.Findings.AddDefaulted_GetRef();
		Finding.Severity = TEXT("ERROR");
		TestEqual(TEXT("an error opens the tab"),
			static_cast<int32>(CrowdyLintTabStateFor(Report)), static_cast<int32>(ECrowdyLintTabState::HasFindings));
	}

	// The case a bClean gate would get wrong: warnings do not make an app unclean, so a clean report can still
	// carry findings, and gating on bClean would leave the surface that shows them permanently shut.
	{
		FStudioLintReport Report;
		Report.bRan = true;
		Report.bClean = true;
		Report.WarningCount = 2;
		Report.Findings.AddDefaulted_GetRef().Severity = TEXT("WARNING");
		Report.Findings.AddDefaulted_GetRef().Severity = TEXT("WARNING");
		TestTrue(TEXT("a warnings-only report is still clean"), Report.bClean);
		TestEqual(TEXT("and still opens the tab"),
			static_cast<int32>(CrowdyLintTabStateFor(Report)), static_cast<int32>(ECrowdyLintTabState::HasFindings));
	}

	// The transition the tab has to survive: findings, then a re-lint that comes back clean. The state has to go
	// back to Clean rather than staying open on a stale list, which is what evicts a user standing on the tab.
	{
		FStudioLintReport Report;
		Report.bRan = true;
		Report.Findings.AddDefaulted_GetRef().Severity = TEXT("ERROR");
		TestEqual(TEXT("open while the finding stands"),
			static_cast<int32>(CrowdyLintTabStateFor(Report)), static_cast<int32>(ECrowdyLintTabState::HasFindings));

		// A re-lint replaces the report wholesale; it never edits the old one in place.
		FStudioLintReport Fixed;
		Fixed.bRan = true;
		Fixed.bClean = true;
		TestEqual(TEXT("and shut again once it is fixed"),
			static_cast<int32>(CrowdyLintTabStateFor(Fixed)), static_cast<int32>(ECrowdyLintTabState::Clean));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
