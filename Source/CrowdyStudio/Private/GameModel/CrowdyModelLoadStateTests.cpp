// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "Templates/Function.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelLoadStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every fixture here carries the LoadStateTest prefix. Adaptive unity merges this module's .cpp files into
	// shared translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	const ECrowdyModelLoadState LoadStateTestAllStates[] = {
		ECrowdyModelLoadState::NeverRequested,
		ECrowdyModelLoadState::Loading,
		ECrowdyModelLoadState::Loaded,
		ECrowdyModelLoadState::Failed
	};

	FString LoadStateTestStateName(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::NeverRequested: return TEXT("NeverRequested");
		case ECrowdyModelLoadState::Loading:        return TEXT("Loading");
		case ECrowdyModelLoadState::Loaded:         return TEXT("Loaded");
		default:                                    return TEXT("Failed");
		}
	}

	struct FLoadStateTestSurface
	{
		FString Name;
		TFunction<FString(ECrowdyModelLoadState)> Text;
	};

	// Every surface whose placeholder is keyed by a load state. A surface missing from this list is a surface no
	// case below covers, so it is listed here once and every case walks it.
	TArray<FLoadStateTestSurface> LoadStateTestSurfaces()
	{
		TArray<FLoadStateTestSurface> Surfaces;
		Surfaces.Add({ TEXT("the models rail"), &CrowdyModelEmptyState::ModelRail });
		Surfaces.Add({ TEXT("the attributes table"), &CrowdyModelEmptyState::AttributeTable });
		Surfaces.Add({ TEXT("the functions table"), &CrowdyModelEmptyState::FunctionTable });
		Surfaces.Add({ TEXT("the automations table"), &CrowdyModelEmptyState::AutomationTable });
		Surfaces.Add({ TEXT("the app entry's functions table"), &CrowdyModelEmptyState::FunctionTableAppWide });
		Surfaces.Add({ TEXT("the app entry's automations table"), &CrowdyModelEmptyState::AutomationTableAppWide });
		Surfaces.Add({ TEXT("the live models rail"), &CrowdyModelEmptyState::LiveModelRail });
		Surfaces.Add({ TEXT("the live models table"), &CrowdyModelEmptyState::LiveInstanceTable });
		return Surfaces;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmptyStateTextDiffersForEveryStateTest,
	"CrowdySDK.CrowdyStudio.EmptyStateTextDiffersForEveryState", CrowdyModelLoadStateTestFlags)

bool FCrowdyEmptyStateTextDiffersForEveryStateTest::RunTest(const FString& Parameters)
{
	const int32 StateCount = static_cast<int32>(UE_ARRAY_COUNT(LoadStateTestAllStates));
	for (const FLoadStateTestSurface& Surface : LoadStateTestSurfaces())
	{
		for (int32 Left = 0; Left < StateCount; ++Left)
		{
			const FString LeftText = Surface.Text(LoadStateTestAllStates[Left]);
			TestTrue(FString::Printf(TEXT("%s says something for %s"),
				*Surface.Name, *LoadStateTestStateName(LoadStateTestAllStates[Left])), !LeftText.IsEmpty());

			for (int32 Right = Left + 1; Right < StateCount; ++Right)
			{
				const FString RightText = Surface.Text(LoadStateTestAllStates[Right]);
				TestTrue(FString::Printf(TEXT("%s says something different for %s than for %s"),
					*Surface.Name,
					*LoadStateTestStateName(LoadStateTestAllStates[Left]),
					*LoadStateTestStateName(LoadStateTestAllStates[Right])),
					!LeftText.Equals(RightText, ESearchCase::CaseSensitive));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmptyStateFailureNeverReadsAsLoadingTest,
	"CrowdySDK.CrowdyStudio.EmptyStateFailureNeverReadsAsLoading", CrowdyModelLoadStateTestFlags)

bool FCrowdyEmptyStateFailureNeverReadsAsLoadingTest::RunTest(const FString& Parameters)
{
	// The defect this stands over is a permanent read failure rendering as a progress message forever, which is a
	// page that asks the reader to wait for something that will never arrive.
	for (const FLoadStateTestSurface& Surface : LoadStateTestSurfaces())
	{
		const FString FailedText = Surface.Text(ECrowdyModelLoadState::Failed);
		TestTrue(FString::Printf(TEXT("%s does not describe a failure as reading"), *Surface.Name),
			!FailedText.Contains(TEXT("Reading"), ESearchCase::IgnoreCase));
		TestTrue(FString::Printf(TEXT("%s does not describe a failure as loading"), *Surface.Name),
			!FailedText.Contains(TEXT("Loading"), ESearchCase::IgnoreCase));
		TestTrue(FString::Printf(TEXT("%s names a way to retry"), *Surface.Name),
			FailedText.Contains(TEXT("Refresh"), ESearchCase::CaseSensitive)
				|| FailedText.Contains(TEXT("Select the model again"), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmptyStateModelAndLiveRailsAgreeTest,
	"CrowdySDK.CrowdyStudio.EmptyStateModelAndLiveRailsAgree", CrowdyModelLoadStateTestFlags)

bool FCrowdyEmptyStateModelAndLiveRailsAgreeTest::RunTest(const FString& Parameters)
{
	// Both rails describe the same empty model list, so they must prescribe the same remedy. Two surfaces answering
	// one question differently is how a reader learns to trust neither.
	TestEqual(TEXT("both rails say the same thing about an app with no models"),
		CrowdyModelEmptyState::ModelRail(ECrowdyModelLoadState::Loaded),
		CrowdyModelEmptyState::LiveModelRail(ECrowdyModelLoadState::Loaded));
	TestEqual(TEXT("both rails say the same thing when nothing has been read"),
		CrowdyModelEmptyState::ModelRail(ECrowdyModelLoadState::NeverRequested),
		CrowdyModelEmptyState::LiveModelRail(ECrowdyModelLoadState::NeverRequested));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeLoadStatePrefersTheCacheOverAStaleFailureTest,
	"CrowdySDK.CrowdyStudio.AttributeLoadStatePrefersTheCacheOverAStaleFailure", CrowdyModelLoadStateTestFlags)

bool FCrowdyAttributeLoadStatePrefersTheCacheOverAStaleFailureTest::RunTest(const FString& Parameters)
{
	// A model read once unsuccessfully and once successfully has its attributes in hand, and the record of the older
	// failure must not talk the page out of showing them.
	TestTrue(TEXT("a cached model reads as loaded even with a failure recorded against it"),
		CrowdyModelEmptyState::AttributeLoadState(true, true, false) == ECrowdyModelLoadState::Loaded);
	TestTrue(TEXT("a cached model reads as loaded even while a re-read is out"),
		CrowdyModelEmptyState::AttributeLoadState(true, true, true) == ECrowdyModelLoadState::Loaded);
	TestTrue(TEXT("a failed model with nothing cached reads as failed"),
		CrowdyModelEmptyState::AttributeLoadState(false, true, false) == ECrowdyModelLoadState::Failed);
	TestTrue(TEXT("a model with a read out and nothing else reads as loading"),
		CrowdyModelEmptyState::AttributeLoadState(false, false, true) == ECrowdyModelLoadState::Loading);
	TestTrue(TEXT("a model nobody has asked about reads as never requested"),
		CrowdyModelEmptyState::AttributeLoadState(false, false, false) == ECrowdyModelLoadState::NeverRequested);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationTriggerNoteOnlyFiresOnATriggerFailureTest,
	"CrowdySDK.CrowdyStudio.AutomationTriggerNoteOnlyFiresOnATriggerFailure", CrowdyModelLoadStateTestFlags)

bool FCrowdyAutomationTriggerNoteOnlyFiresOnATriggerFailureTest::RunTest(const FString& Parameters)
{
	for (const ECrowdyModelLoadState Automations : LoadStateTestAllStates)
	{
		for (const ECrowdyModelLoadState Triggers : LoadStateTestAllStates)
		{
			const FString Note = CrowdyModelEmptyState::AutomationTriggerNote(Automations, Triggers);
			const bool bExpected = Automations == ECrowdyModelLoadState::Loaded
				&& Triggers == ECrowdyModelLoadState::Failed;
			TestEqual(FString::Printf(TEXT("automations %s with triggers %s"),
				*LoadStateTestStateName(Automations), *LoadStateTestStateName(Triggers)),
				!Note.IsEmpty(), bExpected);
		}
	}

	// The note belongs to a table that has rows, so it must not read as a placeholder for an empty one.
	const FString Note = CrowdyModelEmptyState::AutomationTriggerNote(
		ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Failed);
	TestTrue(TEXT("the note says the automations are shown"),
		Note.Contains(TEXT("Automations are shown"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAppWideEmptyStateNeverNamesAModelTest,
	"CrowdySDK.CrowdyStudio.AppWideEmptyStateNeverNamesAModel", CrowdyModelLoadStateTestFlags)

bool FCrowdyAppWideEmptyStateNeverNamesAModelTest::RunTest(const FString& Parameters)
{
	// The app entry gathers what belongs to no model, and its own subtitle says so. A section on it that answers
	// "this model has none" is the same pane contradicting itself about what the reader is looking at.
	const FString AppWide[] = {
		CrowdyModelEmptyState::AttributeTableAppWide(),
		CrowdyModelEmptyState::FunctionTableAppWide(ECrowdyModelLoadState::Loaded),
		CrowdyModelEmptyState::AutomationTableAppWide(ECrowdyModelLoadState::Loaded)
	};

	for (const FString& Text : AppWide)
	{
		TestTrue(FString::Printf(TEXT("\"%s\" does not answer for a model"), *Text),
			!Text.Contains(TEXT("This model"), ESearchCase::CaseSensitive));
	}

	// And each really is its own answer rather than the model-scoped one reached by another name.
	TestTrue(TEXT("the app entry's functions answer differs from a model's"),
		!CrowdyModelEmptyState::FunctionTableAppWide(ECrowdyModelLoadState::Loaded)
			.Equals(CrowdyModelEmptyState::FunctionTable(ECrowdyModelLoadState::Loaded), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the app entry's automations answer differs from a model's"),
		!CrowdyModelEmptyState::AutomationTableAppWide(ECrowdyModelLoadState::Loaded)
			.Equals(CrowdyModelEmptyState::AutomationTable(ECrowdyModelLoadState::Loaded), ESearchCase::CaseSensitive));

	// A read that has not landed says nothing about models either way, so the app entry repeats the one sentence
	// the model-scoped surface already spells rather than keeping a second copy that can drift from it.
	for (const ECrowdyModelLoadState State : LoadStateTestAllStates)
	{
		if (State == ECrowdyModelLoadState::Loaded)
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("functions for %s read the same on the app entry"),
			*LoadStateTestStateName(State)),
			CrowdyModelEmptyState::FunctionTableAppWide(State), CrowdyModelEmptyState::FunctionTable(State));
		TestEqual(FString::Printf(TEXT("automations for %s read the same on the app entry"),
			*LoadStateTestStateName(State)),
			CrowdyModelEmptyState::AutomationTableAppWide(State), CrowdyModelEmptyState::AutomationTable(State));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
