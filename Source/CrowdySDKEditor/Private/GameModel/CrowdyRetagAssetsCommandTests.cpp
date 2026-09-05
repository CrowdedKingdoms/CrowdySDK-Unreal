// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "GameModel/CrowdyRetagAssetsCommand.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyRetagAssetsCommandTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Named apart from the other fixtures in this module because adaptive unity merges its translation units and
	// two anonymous-namespace helpers sharing a name redefine each other.
	int32 RetagAssetsIndexOf(const TArray<FSoftObjectPath>& Ordered, const FSoftObjectPath& Path)
	{
		return Ordered.IndexOfByKey(Path);
	}
}

// The migration's whole reason for existing: a child resaved before its parent overwrites a package hash the
// parent's own resave immediately invalidates, so the order this produces is load-bearing, not cosmetic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRetagAssetsTopoSortTest,
	"CrowdySDK.GameModel.RetagAssetsTopoSortOrdersParentFirst", CrowdyRetagAssetsCommandTestFlags)
bool FCrowdyRetagAssetsTopoSortTest::RunTest(const FString& Parameters)
{
	const FSoftObjectPath Grandparent(TEXT("/Game/Grandparent.Grandparent"));
	const FSoftObjectPath Parent(TEXT("/Game/Parent.Parent"));
	const FSoftObjectPath Child(TEXT("/Game/Child.Child"));

	// A three-deep chain: every ancestor must land strictly before its descendant.
	{
		TMap<FSoftObjectPath, FSoftObjectPath> ChildToParent;
		ChildToParent.Add(Child, Parent);
		ChildToParent.Add(Parent, Grandparent);

		const TArray<FSoftObjectPath> Candidates = { Child, Parent, Grandparent };
		const TArray<FSoftObjectPath> Ordered =
			CrowdyRetagAssetsCommand::TopoSortParentFirst(ChildToParent, Candidates);

		const int32 GrandparentIndex = RetagAssetsIndexOf(Ordered, Grandparent);
		const int32 ParentIndex = RetagAssetsIndexOf(Ordered, Parent);
		const int32 ChildIndex = RetagAssetsIndexOf(Ordered, Child);

		TestEqual(TEXT("every candidate in a chain is placed exactly once"), Ordered.Num(), 3);
		TestTrue(TEXT("the grandparent precedes the parent"), GrandparentIndex < ParentIndex);
		TestTrue(TEXT("the parent precedes the child"), ParentIndex < ChildIndex);
	}

	// A cycle: two Blueprints each recorded as the other's parent (a corrupt or reparented-mid-scan tag). The sort
	// must terminate and still place both, rather than recursing forever.
	{
		const FSoftObjectPath A(TEXT("/Game/CycleA.CycleA"));
		const FSoftObjectPath B(TEXT("/Game/CycleB.CycleB"));

		TMap<FSoftObjectPath, FSoftObjectPath> ChildToParent;
		ChildToParent.Add(A, B);
		ChildToParent.Add(B, A);

		const TArray<FSoftObjectPath> Candidates = { A, B };
		const TArray<FSoftObjectPath> Ordered =
			CrowdyRetagAssetsCommand::TopoSortParentFirst(ChildToParent, Candidates);

		TestEqual(TEXT("a two-node cycle still places both nodes exactly once, without hanging"), Ordered.Num(), 2);
		TestTrue(TEXT("the first cycle member is present"), Ordered.Contains(A));
		TestTrue(TEXT("the second cycle member is present"), Ordered.Contains(B));
	}

	// A parent that is not itself a candidate (already tag-current, or not a Blueprint at all): it must never
	// appear in the output, since this pass never touches it.
	{
		const FSoftObjectPath OutsideParent(TEXT("/Game/AlreadyCurrent.AlreadyCurrent"));

		TMap<FSoftObjectPath, FSoftObjectPath> ChildToParent;
		ChildToParent.Add(Child, OutsideParent);

		const TArray<FSoftObjectPath> Candidates = { Child };
		const TArray<FSoftObjectPath> Ordered =
			CrowdyRetagAssetsCommand::TopoSortParentFirst(ChildToParent, Candidates);

		TestEqual(TEXT("only the actual candidate is placed"), Ordered.Num(), 1);
		TestTrue(TEXT("the candidate itself is present"), Ordered.Contains(Child));
		TestFalse(TEXT("a parent outside the candidate set never appears in the output"),
			Ordered.Contains(OutsideParent));
	}

	// An asset with no recorded parent at all is a root, and still has to appear.
	{
		TMap<FSoftObjectPath, FSoftObjectPath> ChildToParent; // deliberately empty
		const TArray<FSoftObjectPath> Candidates = { Child };
		const TArray<FSoftObjectPath> Ordered =
			CrowdyRetagAssetsCommand::TopoSortParentFirst(ChildToParent, Candidates);

		TestEqual(TEXT("a parentless candidate is still placed"), Ordered.Num(), 1);
		TestTrue(TEXT("and it is the candidate itself"), Ordered.Contains(Child));
	}

	return true;
}

// The tag reader has to unwrap the export-text form the registry stores a Blueprint's parent class in, and
// terminate on the tag's explicit no-parent marker rather than treating "None" as a literal path to resolve.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRetagAssetsExportPathTest,
	"CrowdySDK.GameModel.RetagAssetsResolvesExportPathToObjectPath", CrowdyRetagAssetsCommandTestFlags)
bool FCrowdyRetagAssetsExportPathTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an export-text class path resolves to its object path"),
		CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(TEXT("Class'/Game/X.X_C'")),
		FString(TEXT("/Game/X.X_C")));

	TestTrue(TEXT("the literal \"None\" (no parent) resolves to empty"),
		CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(TEXT("None")).IsEmpty());

	TestTrue(TEXT("an empty tag value resolves to empty"),
		CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(FString()).IsEmpty());

	TestEqual(TEXT("a value that is already a plain object path is left usable"),
		CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(TEXT("/Game/X.X_C")),
		FString(TEXT("/Game/X.X_C")));

	return true;
}

#endif
