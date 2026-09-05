#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Replication/State/CrowdyBitwiseCompare.h"
#include "Replication/State/CrowdyBitwiseCompareTestTypes.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "StructUtils/InstancedStruct.h"

namespace
{
	constexpr EAutomationTestFlags AutoReplicatorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using namespace CrowdyAutoReplication;

	FSendInputs Settled()
	{
		FSendInputs Inputs;
		Inputs.bSendOnlyOnChange = true;
		Inputs.bHasSentBefore = true;
		Inputs.bStateOrChunkChanged = false;
		Inputs.bKeyframeDue = false;
		Inputs.bHeartbeatDue = false;
		return Inputs;
	}
}

// The point of the whole slice: an actor that has not changed stops re-sending its state. Before this the loop sent
// a full update every interval unconditionally, so a stationary player paid full state at the replication rate
// forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorSkipsUnchangedTest,
	"CrowdySDK.AutoReplicator.UnchangedActorSendsNothing", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorSkipsUnchangedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an unchanged actor with neither clock due sends nothing"),
		static_cast<int32>(DecideSend(Settled())), static_cast<int32>(ESendDecision::None));

	FSendInputs Changed = Settled();
	Changed.bStateOrChunkChanged = true;
	TestEqual(TEXT("a changed actor sends its state"),
		static_cast<int32>(DecideSend(Changed)), static_cast<int32>(ESendDecision::FullUpdate));

	return true;
}

// An entry that has never sent has nothing to have changed from. Without this its first interval would compare the
// live state against a default-constructed one and could decide, wrongly, that the actor is unchanged, so the actor
// would never announce itself and would simply never appear.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorFirstSendIsUnconditionalTest,
	"CrowdySDK.AutoReplicator.FirstSendIsUnconditional", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorFirstSendIsUnconditionalTest::RunTest(const FString& Parameters)
{
	FSendInputs FirstEver = Settled();
	FirstEver.bHasSentBefore = false;

	TestEqual(TEXT("an actor that has never sent sends its state"),
		static_cast<int32>(DecideSend(FirstEver)), static_cast<int32>(ESendDecision::FullUpdate));

	// Even with a heartbeat due, which is the combination that would otherwise let a first appearance go out as a
	// header carrying no state at all.
	FirstEver.bHeartbeatDue = true;
	TestEqual(TEXT("a first send is a full update rather than a heartbeat"),
		static_cast<int32>(DecideSend(FirstEver)), static_cast<int32>(ESendDecision::FullUpdate));

	return true;
}

// The keyframe is what repairs an observer that arrived late or lost a packet, so it has to beat the heartbeat
// whenever both are due: a heartbeat says the actor is present, and only a full send says what it looks like.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorKeyframeBeatsHeartbeatTest,
	"CrowdySDK.AutoReplicator.KeyframeOutranksHeartbeat", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorKeyframeBeatsHeartbeatTest::RunTest(const FString& Parameters)
{
	FSendInputs KeyframeOnly = Settled();
	KeyframeOnly.bKeyframeDue = true;
	TestEqual(TEXT("an unchanged actor still re-sends its state when a keyframe is due"),
		static_cast<int32>(DecideSend(KeyframeOnly)), static_cast<int32>(ESendDecision::FullUpdate));

	FSendInputs Both = Settled();
	Both.bKeyframeDue = true;
	Both.bHeartbeatDue = true;
	TestEqual(TEXT("a keyframe wins when both are due"),
		static_cast<int32>(DecideSend(Both)), static_cast<int32>(ESendDecision::FullUpdate));

	FSendInputs HeartbeatOnly = Settled();
	HeartbeatOnly.bHeartbeatDue = true;
	TestEqual(TEXT("an unchanged actor with only a heartbeat due sends a heartbeat"),
		static_cast<int32>(DecideSend(HeartbeatOnly)), static_cast<int32>(ESendDecision::Heartbeat));

	return true;
}

// Turning the setting off has to restore the previous behaviour exactly, because it is the escape hatch if the new
// cadence turns out to interact badly with something on a live server. Nothing else may override it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorOptOutSendsEveryIntervalTest,
	"CrowdySDK.AutoReplicator.OptingOutSendsEveryInterval", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorOptOutSendsEveryIntervalTest::RunTest(const FString& Parameters)
{
	FSendInputs OptedOut = Settled();
	OptedOut.bSendOnlyOnChange = false;

	TestEqual(TEXT("with the setting off an unchanged actor still sends every interval"),
		static_cast<int32>(DecideSend(OptedOut)), static_cast<int32>(ESendDecision::FullUpdate));

	// Including when a heartbeat is due, which must not be allowed to downgrade a send the setting says to make.
	OptedOut.bHeartbeatDue = true;
	TestEqual(TEXT("with the setting off a due heartbeat does not replace the full send"),
		static_cast<int32>(DecideSend(OptedOut)), static_cast<int32>(ESendDecision::FullUpdate));

	return true;
}

// Both intervals are documented as disable-able by setting them to zero, which reaches the decision as neither
// clock ever being due. With both off, an unchanged actor goes quiet until it changes, and that is the intended
// (and only) way to get the old always-quiet-until-moved behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorBothClocksOffTest,
	"CrowdySDK.AutoReplicator.BothClocksOffGoesQuiet", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorBothClocksOffTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("with neither clock due an unchanged actor sends nothing"),
		static_cast<int32>(DecideSend(Settled())), static_cast<int32>(ESendDecision::None));

	FSendInputs Moved = Settled();
	Moved.bStateOrChunkChanged = true;
	TestEqual(TEXT("and it still sends the moment it changes"),
		static_cast<int32>(DecideSend(Moved)), static_cast<int32>(ESendDecision::FullUpdate));

	return true;
}

// The byte compare is only ever taken for a type the predicate accepted, so what has to hold is that it
// answers what the reflective walk answers. Both routes are driven over the same value pairs; a route
// that disagreed with the other would make an actor either re-send forever or go silent while moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorChangeDetectionTest,
	"CrowdySDK.AutoReplicator.ChangeDetectionAgreesWithTheReflectiveCompare", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorChangeDetectionTest::RunTest(const FString& Parameters)
{
	FCrowdyBitwiseTransformState Sent;
	Sent.Location = FVector(10.0, 20.0, 30.0);
	Sent.Rotation = FRotator(1.0, 2.0, 3.0);

	FCrowdyBitwiseTransformState Moved = Sent;
	Moved.Location.X = 11.0;

	const FInstancedStruct LastSent = FInstancedStruct::Make(Sent);
	const FInstancedStruct Unchanged = FInstancedStruct::Make(Sent);
	const FInstancedStruct Changed = FInstancedStruct::Make(Moved);

	const int32 Comparable =
		CrowdyBitwiseCompare::ComparableBytes(FCrowdyBitwiseTransformState::StaticStruct());

	for (const int32 Width : {0, Comparable})
	{
		const TCHAR* const Route = Width > 0 ? TEXT("byte compare") : TEXT("property walk");

		TestFalse(FString::Printf(TEXT("%s: an unmoved actor has not changed"), Route),
			HasStateChanged(LastSent, Unchanged, Width));
		TestTrue(FString::Printf(TEXT("%s: a moved actor has"), Route),
			HasStateChanged(LastSent, Changed, Width));
	}

	// A type change is answered before either compare is reached, since two different structs share no
	// footprint to compare over.
	FCrowdyBitwiseFlagsState Flags;
	TestTrue(TEXT("a state that changed type has changed"),
		HasStateChanged(LastSent, FInstancedStruct::Make(Flags), Comparable));

	// The first interval of an entry that has never sent, which DecideSend already makes unconditional.
	// Asserted anyway because it is the one pair where both sides are empty.
	TestFalse(TEXT("two empty states have not changed"),
		HasStateChanged(FInstancedStruct(), FInstancedStruct(), 0));
	TestTrue(TEXT("and an empty state against a real one has"),
		HasStateChanged(FInstancedStruct(), Unchanged, 0));

	// The refused kinds must reach the reflective route intact: two equal strings live in different
	// buffers, so a byte compare would call this changed on every tick forever.
	FCrowdyBitwiseTextState Named;
	Named.Name = TEXT("crowd");
	FCrowdyBitwiseTextState Rebuilt;
	Rebuilt.Name = FString(TEXT("crow")) + TEXT("d");
	TestFalse(TEXT("a string-carrying state is unchanged when its characters match"),
		HasStateChanged(FInstancedStruct::Make(Named), FInstancedStruct::Make(Rebuilt), 0));

	return true;
}

// Resolving the width is itself a reflection walk, so an entry that sends every tick must not resolve it
// every tick. That is invisible to a compare test, which is how the first cut of this slice put the walk it
// exists to remove straight back into the per-entity loop, so the rule gets its own gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoReplicatorCompareWidthResolutionTest,
	"CrowdySDK.AutoReplicator.CompareWidthIsResolvedOnlyWhenTheTypeMoves", AutoReplicatorTestFlags)
bool FCrowdyAutoReplicatorCompareWidthResolutionTest::RunTest(const FString& Parameters)
{
	const UScriptStruct* const Transform = FCrowdyBitwiseTransformState::StaticStruct();
	const UScriptStruct* const Flags = FCrowdyBitwiseFlagsState::StaticStruct();

	// A width no walk would ever produce for this struct, so returning it verbatim is proof the walk did not
	// run rather than proof it happened to agree.
	constexpr int32 Sentinel = 4242;
	TestEqual(TEXT("an entry sending the type it already sent keeps the width it holds"),
		ResolveCompareBytes(Transform, Transform, Sentinel), Sentinel);

	TestEqual(TEXT("an entry whose type moved resolves the new type's width"),
		ResolveCompareBytes(Transform, Flags, Sentinel), CrowdyBitwiseCompare::ComparableBytes(Flags));

	// The first send of an entry, whose recorded state is still empty.
	TestEqual(TEXT("a first send resolves rather than keeping the zero it started with"),
		ResolveCompareBytes(nullptr, Transform, 0), CrowdyBitwiseCompare::ComparableBytes(Transform));

	// An executor that stops producing a snapshot at all, which must clear the width rather than keep one
	// that describes a type no longer being sent.
	TestEqual(TEXT("a state that went empty clears the width"),
		ResolveCompareBytes(Transform, nullptr, Sentinel), 0);

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
