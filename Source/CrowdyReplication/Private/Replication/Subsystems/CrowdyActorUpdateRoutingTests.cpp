// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/Subsystems/CrowdyActorUpdateRouting.h"

// An inbound actor update takes one of two routes, and the two must never both carry the same actor: one is
// answered where the update is received, the other waits for a consumer to decide whether to spawn it, and a
// consumer runs far enough behind that anything it forwarded for an actor now on screen would be stale. These
// cases are the rule that keeps the routes apart, which is the thing per-actor ordering rests on.
namespace
{
	constexpr EAutomationTestFlags CrowdyActorUpdateRoutingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using namespace CrowdyActorUpdateRouting;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTrackedActorIsAnsweredWhereItArrivesTest,
	"CrowdySDK.Replication.TrackedActorIsAnsweredWhereItArrives",
	CrowdyActorUpdateRoutingTestFlags)

bool FCrowdyTrackedActorIsAnsweredWhereItArrivesTest::RunTest(const FString&)
{
	TestEqual(TEXT("an actor already on screen is gathered, not queued"),
		static_cast<int32>(RouteOnReceive(/*bTracked*/ true)),
		static_cast<int32>(EReceiveRoute::Gather));

	TestEqual(TEXT("an actor nobody has seen is queued for the spawn decision"),
		static_cast<int32>(RouteOnReceive(/*bTracked*/ false)),
		static_cast<int32>(EReceiveRoute::QueueForSpawnDecision));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyQueuedUpdateForATrackedActorIsDroppedTest,
	"CrowdySDK.Replication.QueuedUpdateForATrackedActorIsDropped",
	CrowdyActorUpdateRoutingTestFlags)

bool FCrowdyQueuedUpdateForATrackedActorIsDroppedTest::RunTest(const FString&)
{
	// The load-bearing one, and the reason is that the update can no longer be ORDERED, not that something
	// newer is known to have replaced it. Once the actor is tracked its updates go straight from where they
	// arrive to the drain, while this one waited behind a queue and a batch window, so delivering it can put
	// an older position after a newer one and nothing downstream compares timestamps to notice.
	TestEqual(TEXT("a queued update for an actor now on screen is dropped as stale"),
		static_cast<int32>(JudgeQueuedUpdate(/*bTracked*/ true, /*bSpawnPending*/ false)),
		static_cast<int32>(EQueuedVerdict::DropStale));

	// Briefly both, at the moment a spawn completes. Stale still wins, or the same update would spawn a
	// duplicate of an actor that is already on screen.
	TestEqual(TEXT("tracked outranks pending when an update is both"),
		static_cast<int32>(JudgeQueuedUpdate(/*bTracked*/ true, /*bSpawnPending*/ true)),
		static_cast<int32>(EQueuedVerdict::DropStale));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyQueuedUpdateStillSpawnsAnUnknownActorTest,
	"CrowdySDK.Replication.QueuedUpdateStillSpawnsAnUnknownActor",
	CrowdyActorUpdateRoutingTestFlags)

bool FCrowdyQueuedUpdateStillSpawnsAnUnknownActorTest::RunTest(const FString&)
{
	// The spawn path is the reason the queue exists, so it has to survive the routes being split.
	TestEqual(TEXT("an actor nobody has seen spawns"),
		static_cast<int32>(JudgeQueuedUpdate(/*bTracked*/ false, /*bSpawnPending*/ false)),
		static_cast<int32>(EQueuedVerdict::Spawn));

	TestEqual(TEXT("an actor whose spawn is already under way does not spawn twice"),
		static_cast<int32>(JudgeQueuedUpdate(/*bTracked*/ false, /*bSpawnPending*/ true)),
		static_cast<int32>(EQueuedVerdict::DropAlreadySpawning));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOnlyOneRouteEverCarriesAnActorTest,
	"CrowdySDK.Replication.OnlyOneRouteEverCarriesAnActor",
	CrowdyActorUpdateRoutingTestFlags)

bool FCrowdyOnlyOneRouteEverCarriesAnActorTest::RunTest(const FString&)
{
	// Stated as the property rather than as four separate answers, because this is what actually has to
	// hold: for either value of tracked, exactly one of the two routes can deliver the update, so no actor
	// can have updates arriving down both at once and no ordering between them has to be defined.
	for (const bool bSpawnPending : { false, true })
	{
		for (const bool bTracked : { false, true })
		{
			const bool bGathersOnReceive = RouteOnReceive(bTracked) == EReceiveRoute::Gather;
			const bool bQueueCanDeliver = JudgeQueuedUpdate(bTracked, bSpawnPending) == EQueuedVerdict::Spawn;

			TestFalse(
				FString::Printf(TEXT("tracked=%d pending=%d delivers down only one route"),
					bTracked ? 1 : 0, bSpawnPending ? 1 : 0),
				bGathersOnReceive && bQueueCanDeliver);
		}
	}
	return true;
}

#endif
