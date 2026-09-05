// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "GameModel/CrowdyContainerAssetTags.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyContainerAssetTagsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Named apart from the other fixtures in this module because adaptive unity merges its translation units and
	// two anonymous-namespace helpers sharing a name redefine each other.
	CrowdyContainerAssetTags::FContainerTagInput MakeContainerTagInput(
		bool bFormed, bool bUpToDate, bool bMarked, const TCHAR* ClassTypeName)
	{
		CrowdyContainerAssetTags::FContainerTagInput Input;
		Input.bClassFormed = bFormed;
		Input.bClassUpToDate = bUpToDate;
		Input.bMarkedContainer = bMarked;
		Input.ClassContainerTypeName = ClassTypeName ? FString(ClassTypeName) : FString();
		Input.bClassCarriesContainerTag = ClassTypeName != nullptr;
		return Input;
	}
}

// What each Blueprint publishes at save time, and, more importantly, what it refuses to publish. The scan tag is a
// claim that the container answer beside it is definite, so every state where it is not definite has to publish
// nothing at all: publishing the scan tag alone would record the asset as definitively not a container, and a
// container that disappears from a schema plan turns every model the live app already holds into a prune candidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerAssetTagDecisionTest,
	"CrowdySDK.GameModel.ContainerAssetTagDecision", CrowdyContainerAssetTagsTestFlags)
bool FCrowdyContainerAssetTagDecisionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyContainerAssetTags;

	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ true, /*Marked*/ true, TEXT("Chest")));
		TestTrue(TEXT("a compiled container publishes the scan tag"), Decision.bWriteScanTag);
		TestEqual(TEXT("a compiled container publishes its type name"),
			Decision.ContainerTypeName, FString(TEXT("Chest")));
	}

	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ true, /*Marked*/ false, nullptr));
		TestTrue(TEXT("a compiled non-container publishes the scan tag"), Decision.bWriteScanTag);
		TestTrue(TEXT("a compiled non-container publishes no type name"), Decision.ContainerTypeName.IsEmpty());
	}

	// The one that matters most: a container whose Blueprint does not compile has no tag on its class, so the class
	// alone would answer "not a container". Publishing nothing puts it back in the load partition, where it behaves
	// exactly as it did before any of these tags existed.
	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ false, /*Marked*/ true, nullptr));
		TestFalse(TEXT("a container that failed to compile publishes nothing"), Decision.bWriteScanTag);
		TestTrue(TEXT("a container that failed to compile publishes no type name"),
			Decision.ContainerTypeName.IsEmpty());
	}

	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ false, /*UpToDate*/ true, /*Marked*/ false, nullptr));
		TestFalse(TEXT("a Blueprint with no formed class publishes nothing"), Decision.bWriteScanTag);
	}

	// Marked but not yet recompiled: the marker reads true from the moment a designer sets it, while the class tag
	// only appears after a compile. The two disagreeing means the save cannot tell which one describes the asset.
	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ true, /*Marked*/ true, nullptr));
		TestFalse(TEXT("a marked container whose class carries no tag yet publishes nothing"), Decision.bWriteScanTag);
	}

	// And the same disagreement the other way round: a stale tag left on a class whose marker is gone.
	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ true, /*Marked*/ false, TEXT("Chest")));
		TestFalse(TEXT("an unmarked class carrying a stale container tag publishes nothing"), Decision.bWriteScanTag);
	}

	// A zero-length tag value is a hard failure in the registry's fixed-size store, and writing the scan tag with no
	// type name beside it would record a real container as definitively not one. Both are refused together.
	{
		const FContainerTagDecision Decision =
			DecideTags(MakeContainerTagInput(/*Formed*/ true, /*UpToDate*/ true, /*Marked*/ true, TEXT("")));
		TestFalse(TEXT("a container with no resolvable type name publishes nothing"), Decision.bWriteScanTag);
		TestTrue(TEXT("no tag value is ever published empty"), Decision.ContainerTypeName.IsEmpty());
	}

	return true;
}

#endif
