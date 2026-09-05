// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyChannels.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyChannelJoinPlanTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString SessionName = TEXT("__crowdy_session_42");
}

// The session channel is joined whether or not any Multicast CrowdyEvent names a channel. It carries the Game Model
// plane's signals and model-changed pings and the subsystem CrowdyState delta, none of which any CrowdyEvent
// declares, so deriving the join set from RPC usage alone left a project with no channel-routed events joined to
// nothing. A client only receives on channels it has joined, so every signal was then dropped with nothing logged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanIncludesSessionTest,
	"CrowdySDK.Services.ChannelJoinPlanIncludesSession", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanIncludesSessionTest::RunTest(const FString& Parameters)
{
	// The case that was broken: a project with no channel-routed CrowdyEvents at all.
	const TArray<FString> NoRpc = UCrowdyChannels::BuildDesiredJoinNames(TSet<FString>(), SessionName);
	if (TestEqual(TEXT("a project with no multicast channels still joins one"), NoRpc.Num(), 1))
	{
		TestEqual(TEXT("and it is the session channel"), NoRpc[0], SessionName);
	}

	// Named channels are joined alongside it, not instead of it.
	TSet<FString> Named;
	Named.Add(TEXT("guild"));
	Named.Add(TEXT("raid"));
	const TArray<FString> WithNamed = UCrowdyChannels::BuildDesiredJoinNames(Named, SessionName);
	if (TestEqual(TEXT("two named plus the session channel"), WithNamed.Num(), 3))
	{
		TestTrue(TEXT("named channels are kept"), WithNamed.Contains(TEXT("guild")) && WithNamed.Contains(TEXT("raid")));
		TestEqual(TEXT("the session channel is planned last"), WithNamed.Last(), SessionName);
		TestEqual(TEXT("named channels are sorted, so the join order is deterministic"),
			WithNamed[0], FString(TEXT("guild")));
	}

	return true;
}

// A CrowdyEvent naming the session channel explicitly must not queue it twice: the duplicate would be a wasted join
// round trip, and a second create attempt on an app that does not have the channel yet.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanDedupesSessionTest,
	"CrowdySDK.Services.ChannelJoinPlanDedupesSession", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanDedupesSessionTest::RunTest(const FString& Parameters)
{
	TSet<FString> Named;
	Named.Add(SessionName);
	Named.Add(TEXT("guild"));

	const TArray<FString> Plan = UCrowdyChannels::BuildDesiredJoinNames(Named, SessionName);
	if (TestEqual(TEXT("the session channel appears once"), Plan.Num(), 2))
	{
		TestEqual(TEXT("named first"), Plan[0], FString(TEXT("guild")));
		TestEqual(TEXT("session last"), Plan[1], SessionName);
	}

	// An empty name is not a channel, and an unresolved app id (an empty session name) yields nothing to join
	// rather than a request for a channel called "".
	TSet<FString> WithBlank;
	WithBlank.Add(FString());
	TestEqual(TEXT("a blank multicast name is dropped"),
		UCrowdyChannels::BuildDesiredJoinNames(WithBlank, SessionName).Num(), 1);
	TestEqual(TEXT("no session name means nothing to join"),
		UCrowdyChannels::BuildDesiredJoinNames(TSet<FString>(), FString()).Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
