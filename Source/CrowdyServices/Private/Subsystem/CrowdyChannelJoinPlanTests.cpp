// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyChannels.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyChannelJoinPlanTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString SessionName = TEXT("__crowdy_session_42");

	constexpr int64 SessionChannelId = 4242;
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

// A fresh app has no channels authored, so the session channel has to be created before anything can be joined.
// This is the case that fails silently when the plan is derived from the channels CrowdyEvents name: there are
// none, so nothing is planned, nothing is created, and every Game Model signal is dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanCreatesSessionChannelTest,
	"CrowdySDK.Services.ChannelJoinPlanCreatesMissingSessionChannel", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanCreatesSessionChannelTest::RunTest(const FString& Parameters)
{
	const UCrowdyChannels::FCrowdyChannelJoinPlan Plan =
		UCrowdyChannels::BuildJoinPlan(TSet<FString>(), SessionName, TMap<FString, int64>(), TSet<int64>());

	TestTrue(TEXT("the session channel is created when the app does not have it"), Plan.bCreateSessionChannel);
	TestEqual(TEXT("there is nothing to join until it exists"), Plan.ToJoin.Num(), 0);
	TestEqual(TEXT("nothing was already joined"), Plan.AlreadyJoined.Num(), 0);
	TestEqual(TEXT("the session channel is never reported as a missing named channel"), Plan.MissingNamed.Num(), 0);

	return true;
}

// The session channel already exists for the app but this client is not a member, so it is a join, not a create.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanJoinsSessionChannelTest,
	"CrowdySDK.Services.ChannelJoinPlanJoinsExistingSessionChannel", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanJoinsSessionChannelTest::RunTest(const FString& Parameters)
{
	TMap<FString, int64> AppChannels;
	AppChannels.Add(SessionName, SessionChannelId);

	const UCrowdyChannels::FCrowdyChannelJoinPlan Plan =
		UCrowdyChannels::BuildJoinPlan(TSet<FString>(), SessionName, AppChannels, TSet<int64>());

	if (TestEqual(TEXT("exactly one channel is queued for a join"), Plan.ToJoin.Num(), 1))
	{
		TestEqual(TEXT("and it carries the session channel id"), Plan.ToJoin[0].Key, SessionChannelId);
		TestEqual(TEXT("and the session channel name"), Plan.ToJoin[0].Value, SessionName);
	}
	TestFalse(TEXT("a channel that already exists is not created again"), Plan.bCreateSessionChannel);
	TestEqual(TEXT("nothing was already joined"), Plan.AlreadyJoined.Num(), 0);

	return true;
}

// Already a member: the channel is wired up for send and receive directly, without spending a join round trip
// the server would answer with the membership this client already has.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanRegistersMembershipTest,
	"CrowdySDK.Services.ChannelJoinPlanRegistersExistingMembership", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanRegistersMembershipTest::RunTest(const FString& Parameters)
{
	TMap<FString, int64> AppChannels;
	AppChannels.Add(SessionName, SessionChannelId);

	TSet<int64> MemberOf;
	MemberOf.Add(SessionChannelId);

	const UCrowdyChannels::FCrowdyChannelJoinPlan Plan =
		UCrowdyChannels::BuildJoinPlan(TSet<FString>(), SessionName, AppChannels, MemberOf);

	if (TestEqual(TEXT("the existing membership is registered"), Plan.AlreadyJoined.Num(), 1))
	{
		TestEqual(TEXT("and it carries the session channel id"), Plan.AlreadyJoined[0].Key, SessionChannelId);
		TestEqual(TEXT("and the session channel name"), Plan.AlreadyJoined[0].Value, SessionName);
	}
	TestEqual(TEXT("a channel we are already in is not joined again"), Plan.ToJoin.Num(), 0);
	TestFalse(TEXT("nor created again"), Plan.bCreateSessionChannel);

	return true;
}

// A named channel belongs to a designer. Inventing one under the name a CrowdyEvent guessed would hide the typo
// it usually is, so a missing named channel is reported and dropped rather than created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelJoinPlanNeverCreatesNamedTest,
	"CrowdySDK.Services.ChannelJoinPlanNeverCreatesANamedChannel", CrowdyChannelJoinPlanTestFlags)
bool FCrowdyChannelJoinPlanNeverCreatesNamedTest::RunTest(const FString& Parameters)
{
	TSet<FString> Named;
	Named.Add(TEXT("guild"));

	// The session channel exists, so the only thing the app is missing is the named channel.
	TMap<FString, int64> AppChannels;
	AppChannels.Add(SessionName, SessionChannelId);

	const UCrowdyChannels::FCrowdyChannelJoinPlan Plan =
		UCrowdyChannels::BuildJoinPlan(Named, SessionName, AppChannels, TSet<int64>());

	if (TestEqual(TEXT("the missing named channel is reported"), Plan.MissingNamed.Num(), 1))
	{
		TestEqual(TEXT("by name"), Plan.MissingNamed[0], FString(TEXT("guild")));
	}
	TestFalse(TEXT("a missing named channel never triggers a create"), Plan.bCreateSessionChannel);
	TestEqual(TEXT("only the session channel is joined"), Plan.ToJoin.Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
