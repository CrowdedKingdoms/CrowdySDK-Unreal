// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Async/ParallelFor.h"
#include "Data/FCrowdyGuidSet.h"

// The set decides whether an inbound actor update belongs to something already on screen or to something that has
// to be spawned, so every case here is phrased as that question rather than as a container operation. Getting the
// answer wrong in either direction is visible in game: a live entity reported as new is spawned a second time, and
// a new entity reported as live never appears at all.
namespace
{
	constexpr EAutomationTestFlags CrowdyGuidSetTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Fixed rather than generated, so a failure reproduces exactly. The varying component is last because that is
	// the one the engine's hash spreads over.
	FGuid CrowdyGuidSetTestId(const int32 Index)
	{
		return FGuid(0x0C0FFEE0, 0x0000C0DE, 0x00000000, static_cast<uint32>(Index));
	}

	// Fills a set well past the point where entries have to share a starting position, removes half, and answers how
	// many of the survivors can no longer be found and how many of the removals can still be found. Both are zero for
	// a set that answers honestly.
	void CrowdyGuidSetHalfRemovalSurvey(FCrowdyGuidSet& Set, const int32 Inserted,
		int32& OutMissingSurvivors, int32& OutLingeringRemovals)
	{
		for (int32 Index = 0; Index < Inserted; ++Index)
		{
			Set.Add(CrowdyGuidSetTestId(Index));
		}

		for (int32 Index = 0; Index < Inserted; Index += 2)
		{
			Set.Remove(CrowdyGuidSetTestId(Index));
		}

		OutMissingSurvivors = 0;
		OutLingeringRemovals = 0;

		for (int32 Index = 0; Index < Inserted; ++Index)
		{
			const bool bPresent = Set.Contains(CrowdyGuidSetTestId(Index));
			const bool bShouldBePresent = (Index % 2) != 0;

			if (bShouldBePresent && !bPresent)
				++OutMissingSurvivors;
			else if (!bShouldBePresent && bPresent)
				++OutLingeringRemovals;
		}
	}
}

// Removing one entry must not hide any other.
//
// This is the duplicate-spawn defect: an entry that reads as absent while it is still present sends its owner down
// the "this is new" branch, and the entity that is already on screen is spawned again. It needs entries that share
// a starting position to show up at all, which is the ordinary state of affairs once the tracked population
// approaches the configured ceiling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGuidSetRemoveKeepsOtherEntriesTest,
	"CrowdySDK.Replication.GuidSetRemoveKeepsOtherEntries", CrowdyGuidSetTestFlags)
bool FCrowdyGuidSetRemoveKeepsOtherEntriesTest::RunTest(const FString& Parameters)
{
	FCrowdyGuidSet Set(256);

	int32 MissingSurvivors = 0;
	int32 LingeringRemovals = 0;
	CrowdyGuidSetHalfRemovalSurvey(Set, 200, MissingSurvivors, LingeringRemovals);

	TestEqual(TEXT("Entries that were never removed are all still found"), MissingSurvivors, 0);
	TestEqual(TEXT("Entries that were removed are all gone"), LingeringRemovals, 0);
	TestEqual(TEXT("The set holds exactly the survivors"), Set.Num(), 100);

	return true;
}

// A duplicate and a full set are different answers, and the caller acts on them differently: one means the actor is
// already accounted for, the other means nothing is being tracked for it and nothing will be. An id that is already
// present reports as a duplicate even when the set is full, because that is what is true of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGuidSetSeparatesDuplicateFromFullTest,
	"CrowdySDK.Replication.GuidSetSeparatesDuplicateFromFull", CrowdyGuidSetTestFlags)
bool FCrowdyGuidSetSeparatesDuplicateFromFullTest::RunTest(const FString& Parameters)
{
	FCrowdyGuidSet Set(4);

	for (int32 Index = 0; Index < 4; ++Index)
	{
		TestTrue(TEXT("Every id up to the ceiling is added"),
			Set.Add(CrowdyGuidSetTestId(Index)) == FCrowdyGuidSet::EAddResult::Added);
	}

	TestTrue(TEXT("An id past the ceiling reports the ceiling, not a duplicate"),
		Set.Add(CrowdyGuidSetTestId(4)) == FCrowdyGuidSet::EAddResult::AtCapacity);

	TestTrue(TEXT("An id already present reports as a duplicate even with the set full"),
		Set.Add(CrowdyGuidSetTestId(0)) == FCrowdyGuidSet::EAddResult::AlreadyPresent);

	TestFalse(TEXT("The id that was refused is not in the set"), Set.Contains(CrowdyGuidSetTestId(4)));
	TestEqual(TEXT("The ceiling holds"), Set.Num(), 4);

	// A slot freed by a timeout is usable again, which is what lets a session recover once actors go away.
	TestTrue(TEXT("Removing an id frees room"), Set.Remove(CrowdyGuidSetTestId(0)));
	TestTrue(TEXT("The refused id fits once there is room"),
		Set.Add(CrowdyGuidSetTestId(4)) == FCrowdyGuidSet::EAddResult::Added);
	TestTrue(TEXT("The id that fits is found"), Set.Contains(CrowdyGuidSetTestId(4)));

	return true;
}

// Removing something that was never there is an ordinary outcome, not a failure: the timeout sweep removes an id
// from both sets without knowing which one held it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGuidSetRemoveOfAbsentIdTest,
	"CrowdySDK.Replication.GuidSetRemoveOfAbsentId", CrowdyGuidSetTestFlags)
bool FCrowdyGuidSetRemoveOfAbsentIdTest::RunTest(const FString& Parameters)
{
	FCrowdyGuidSet Set(8);

	TestFalse(TEXT("Nothing is found in an empty set"), Set.Contains(CrowdyGuidSetTestId(0)));
	TestFalse(TEXT("Removing from an empty set answers no"), Set.Remove(CrowdyGuidSetTestId(0)));

	Set.Add(CrowdyGuidSetTestId(0));

	TestTrue(TEXT("Removing a present id answers yes"), Set.Remove(CrowdyGuidSetTestId(0)));
	TestFalse(TEXT("Removing it a second time answers no"), Set.Remove(CrowdyGuidSetTestId(0)));
	TestEqual(TEXT("The set is empty again"), Set.Num(), 0);

	return true;
}

// Shard consumers ask about actors from several worker threads at once while the game thread records the ones it has
// spawned, so concurrent readers and writers are the normal case rather than an edge one. Every id added here is
// distinct, so the count is exact and no interleaving can excuse a miss.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGuidSetConcurrentAddsTest,
	"CrowdySDK.Replication.GuidSetConcurrentAdds", CrowdyGuidSetTestFlags)
bool FCrowdyGuidSetConcurrentAddsTest::RunTest(const FString& Parameters)
{
	constexpr int32 Workers = 8;
	constexpr int32 PerWorker = 250;
	constexpr int32 Total = Workers * PerWorker;

	FCrowdyGuidSet Set(Total);

	ParallelFor(Workers, [&Set](const int32 WorkerIndex)
	{
		for (int32 Offset = 0; Offset < PerWorker; ++Offset)
		{
			Set.Add(CrowdyGuidSetTestId(WorkerIndex * PerWorker + Offset));
			Set.Contains(CrowdyGuidSetTestId(Offset));
		}
	});

	TestEqual(TEXT("Every id added from every worker is held exactly once"), Set.Num(), Total);

	int32 Missing = 0;
	for (int32 Index = 0; Index < Total; ++Index)
	{
		if (!Set.Contains(CrowdyGuidSetTestId(Index)))
			++Missing;
	}

	TestEqual(TEXT("Every id added from every worker is found"), Missing, 0);

	return true;
}

#endif
