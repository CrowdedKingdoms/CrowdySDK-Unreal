// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UI/GameModel/CrowdyReconcileSummary.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyReconcileSummaryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryUncheckedTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryUnchecked", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryUncheckedTest::RunTest(const FString& Parameters)
{
	const FCrowdySchemaSyncReport Report; // bValid defaults to false
	TestEqual(TEXT("An unchecked report reads as not checked yet"),
		CrowdyReconcileSummary::BuildCountLine(Report), FString(TEXT("Not checked yet.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryCleanTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryClean", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryCleanTest::RunTest(const FString& Parameters)
{
	FCrowdySchemaSyncReport Report;
	Report.bValid = true;
	TestEqual(TEXT("A clean valid report reads as everything matching"),
		CrowdyReconcileSummary::BuildCountLine(Report), FString(TEXT("Everything matches your project.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryToAddFieldsTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryToAddFields", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryToAddFieldsTest::RunTest(const FString& Parameters)
{
	// Each create field alone must contribute exactly 1 to ToAdd, one assertion per field so a dropped field
	// fails on its own line instead of being masked by a sibling that still works.
	{
		FCrowdySchemaSyncReport Report;
		Report.TypesToCreate = 1;
		TestEqual(TEXT("TypesToCreate alone contributes 1 to ToAdd"), CrowdyReconcileSummary::BuildCounts(Report).ToAdd, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.PropsToCreate = 1;
		TestEqual(TEXT("PropsToCreate alone contributes 1 to ToAdd"), CrowdyReconcileSummary::BuildCounts(Report).ToAdd, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.FunctionsToCreate = 1;
		TestEqual(TEXT("FunctionsToCreate alone contributes 1 to ToAdd"), CrowdyReconcileSummary::BuildCounts(Report).ToAdd, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.AutomationsToCreate = 1;
		TestEqual(TEXT("AutomationsToCreate alone contributes 1 to ToAdd"), CrowdyReconcileSummary::BuildCounts(Report).ToAdd, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.TriggersToCreate = 1;
		TestEqual(TEXT("TriggersToCreate alone contributes 1 to ToAdd"), CrowdyReconcileSummary::BuildCounts(Report).ToAdd, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryToUpdateFieldsTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryToUpdateFields", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryToUpdateFieldsTest::RunTest(const FString& Parameters)
{
	// Each update field alone must contribute exactly 1 to ToUpdate.
	{
		FCrowdySchemaSyncReport Report;
		Report.TypesToUpdate = 1;
		TestEqual(TEXT("TypesToUpdate alone contributes 1 to ToUpdate"), CrowdyReconcileSummary::BuildCounts(Report).ToUpdate, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.PropsToUpdate = 1;
		TestEqual(TEXT("PropsToUpdate alone contributes 1 to ToUpdate"), CrowdyReconcileSummary::BuildCounts(Report).ToUpdate, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.FunctionsToUpdate = 1;
		TestEqual(TEXT("FunctionsToUpdate alone contributes 1 to ToUpdate"), CrowdyReconcileSummary::BuildCounts(Report).ToUpdate, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.AutomationsToUpdate = 1;
		TestEqual(TEXT("AutomationsToUpdate alone contributes 1 to ToUpdate"), CrowdyReconcileSummary::BuildCounts(Report).ToUpdate, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.TriggersToUpdate = 1;
		TestEqual(TEXT("TriggersToUpdate alone contributes 1 to ToUpdate"), CrowdyReconcileSummary::BuildCounts(Report).ToUpdate, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryServerOnlyFieldsTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryServerOnlyFields", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryServerOnlyFieldsTest::RunTest(const FString& Parameters)
{
	// Each server-only field alone must contribute exactly 1 to ServerOnly.
	{
		FCrowdySchemaSyncReport Report;
		Report.ServerOnlyTypeCount = 1;
		TestEqual(TEXT("ServerOnlyTypeCount alone contributes 1 to ServerOnly"), CrowdyReconcileSummary::BuildCounts(Report).ServerOnly, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.ServerOnlyPropCount = 1;
		TestEqual(TEXT("ServerOnlyPropCount alone contributes 1 to ServerOnly"), CrowdyReconcileSummary::BuildCounts(Report).ServerOnly, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.ServerOnlyFunctionCount = 1;
		TestEqual(TEXT("ServerOnlyFunctionCount alone contributes 1 to ServerOnly"), CrowdyReconcileSummary::BuildCounts(Report).ServerOnly, 1);
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.ServerOnlyAutomationCount = 1;
		TestEqual(TEXT("ServerOnlyAutomationCount alone contributes 1 to ServerOnly"), CrowdyReconcileSummary::BuildCounts(Report).ServerOnly, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryLineSegmentsTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryLineSegments", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryLineSegmentsTest::RunTest(const FString& Parameters)
{
	// Additions-only omits the "to change" segment entirely rather than showing it as zero.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.TypesToCreate = 2;
		const FString Line = CrowdyReconcileSummary::BuildCountLine(Report);
		TestTrue(TEXT("An additions-only line names the additions"), Line.Contains(TEXT("to add")));
		TestFalse(TEXT("An additions-only line has no change segment"), Line.Contains(TEXT("to change")));
	}
	// Updates-only omits the "to add" segment entirely.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.TypesToUpdate = 2;
		const FString Line = CrowdyReconcileSummary::BuildCountLine(Report);
		TestTrue(TEXT("An updates-only line names the changes"), Line.Contains(TEXT("to change")));
		TestFalse(TEXT("An updates-only line has no add segment"), Line.Contains(TEXT("to add")));
	}
	// The server-only segment reaches the line at all, which counting alone does not prove.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.ServerOnlyTypeCount = 3;
		TestEqual(TEXT("A server-only-only line reads as only on server"),
			CrowdyReconcileSummary::BuildCountLine(Report), FString(TEXT("3 only on server")));
	}
	// The whole joined string, separator included. The separator is the one non-ASCII character in the summary
	// and nothing else asserts it, so a silent change to it would otherwise ship green.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.TypesToCreate = 1;
		Report.TypesToUpdate = 2;
		TestEqual(TEXT("Two segments join with the strip's separator"),
			CrowdyReconcileSummary::BuildCountLine(Report), FString(TEXT("1 to add  ·  2 to change")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryAppliedTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryApplied", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryAppliedTest::RunTest(const FString& Parameters)
{
	// An applied report keeps the counts it wrote so the detailed report can describe them. The one-line summary
	// must not repeat those as outstanding work, or it contradicts the Schema indicator beside it.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.bApplied = true;
		Report.TypesToCreate = 12;
		const FString Line = CrowdyReconcileSummary::BuildCountLine(Report);
		TestFalse(TEXT("An applied report does not read as pending additions"), Line.Contains(TEXT("to add")));
		TestTrue(TEXT("An applied report reads as synced"), Line.Contains(TEXT("Synced")));
	}
	// The same counts before the write still read as outstanding, so the branch above is a genuine fork.
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.bApplied = false;
		Report.TypesToCreate = 12;
		TestTrue(TEXT("The same counts before a write read as pending"),
			CrowdyReconcileSummary::BuildCountLine(Report).Contains(TEXT("12 to add")));
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.bApplied = true;
		TestEqual(TEXT("An unchecked report reads as unchecked even when marked applied"),
			CrowdyReconcileSummary::BuildCountLine(Report), FString(TEXT("Not checked yet.")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReconcileSummaryShouldOpenDetailsTest,
	"CrowdySDK.CrowdyStudio.ReconcileSummaryShouldOpenDetails", CrowdyReconcileSummaryTestFlags)

bool FCrowdyReconcileSummaryShouldOpenDetailsTest::RunTest(const FString& Parameters)
{
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		TestFalse(TEXT("A clean valid report does not open Details"), CrowdyReconcileSummary::ShouldOpenDetails(Report));
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = false;
		Report.Warnings.Add(TEXT("something"));
		TestFalse(TEXT("An invalid report does not open Details even with a warning"), CrowdyReconcileSummary::ShouldOpenDetails(Report));
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.Warnings.Add(TEXT("something"));
		TestTrue(TEXT("A valid report with a warning opens Details"), CrowdyReconcileSummary::ShouldOpenDetails(Report));
	}
	{
		FCrowdySchemaSyncReport Report;
		Report.bValid = true;
		Report.StatusNote = TEXT("Applied 2 of 3, re-apply to finish.");
		TestTrue(TEXT("A valid report with a status note opens Details"), CrowdyReconcileSummary::ShouldOpenDetails(Report));
	}
	return true;
}

#endif
