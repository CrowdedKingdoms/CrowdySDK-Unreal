#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyCppLoopback.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Serialization/CrowdyFrame.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The application context mask rather than the editor one, because the suite is run headless: an
	// editor-only test is simply absent from that run rather than failing in it.
	constexpr EAutomationTestFlags CrowdyLoopbackTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An id is 32 octets and nothing else fits, so the ids a test uses are built from a repeated
	// character rather than written out.
	FCrowdyActorId TestActorId(const ANSICHAR Fill)
	{
		FCrowdyActorId Id;
		FMemory::Memset(Id.Octets, static_cast<uint8>(Fill), FCrowdyActorId::NumOctets);
		return Id;
	}

	/**
	 * An outbound frame the way the send split produces one, with the id and payload as views into
	 * storage the caller keeps. The real split does the same, which is why the caller has to hold both.
	 */
	struct FOutboundFixture
	{
		FCrowdyActorId Id;
		TArray<uint8> Payload;
		FCrowdyCppOutboundFrame Frame;

		explicit FOutboundFixture(const FCrowdyActorId& InId, const uint8 PayloadMarker = 7)
			: Id(InId)
		{
			Payload.Init(PayloadMarker, 48);

			Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST);
			Frame.AppId = 4242;
			Frame.ChunkX = 1;
			Frame.ChunkY = 2;
			Frame.ChunkZ = 3;
			Frame.Uuid = Id.AsOctets();
			Frame.Payload = Payload;
		}
	};

	/** One delivered frame, copied out of the views that only live for the delivery call. */
	struct FDeliveredFrame
	{
		uint8 Opcode = 0;
		int64 AppId = 0;
		int64 ChunkX = 0;
		int64 ChunkY = 0;
		int64 ChunkZ = 0;
		FCrowdyActorId Uuid;
		TArray<uint8> Body;
	};

	int32 DrainInto(FCrowdyCppLoopback& Loopback, TArray<FDeliveredFrame>& Out, const int32 MaxFrames = 1024)
	{
		return Loopback.DrainDue(MaxFrames, 1.0,
			[&Out](const FCrowdyFrame& Frame)
			{
				FDeliveredFrame& Delivered = Out.AddDefaulted_GetRef();
				Delivered.Opcode = Frame.Opcode;
				Delivered.AppId = Frame.Envelope.AppId;
				Delivered.ChunkX = Frame.Envelope.ChunkX;
				Delivered.ChunkY = Frame.Envelope.ChunkY;
				Delivered.ChunkZ = Frame.Envelope.ChunkZ;
				Delivered.Uuid = FCrowdyActorId::FromOctets(Frame.Envelope.Uuid);
				Delivered.Body.Append(Frame.Body.GetData(), Frame.Body.Num());
			});
	}

	FCrowdyLoopbackSettings PunctualSettings()
	{
		FCrowdyLoopbackSettings Settings;
		Settings.LatencySeconds = 0.04f;
		Settings.JitterSeconds = 0.0f;
		Settings.LossFraction = 0.0f;
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackInterceptsOnlyArmedIdsTest,
	"CrowdySDK.CrowdyNet.LoopbackInterceptsOnlyArmedIds",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackInterceptsOnlyArmedIdsTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');
	const FCrowdyActorId Stranger = TestActorId('b');

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(MakeArrayView(&Armed, 1), PunctualSettings());

	const FOutboundFixture ArmedFrame(Armed);
	const FOutboundFixture StrangerFrame(Stranger);

	TestTrue(TEXT("an update for an armed id is taken"), Loopback.TryAcceptOutbound(ArmedFrame.Frame));

	// The whole reason interception is by id: a session running this still has its own player to
	// replicate, and that has to keep reaching the real relay.
	TestFalse(TEXT("an update for an id that was not armed is left to the wire"),
		Loopback.TryAcceptOutbound(StrangerFrame.Frame));

	// A channel message travels on a different frame layout with a different delivery path, so it can
	// never be answered locally whatever id it carries.
	FCrowdyCppOutboundFrame Channel = ArmedFrame.Frame;
	Channel.bIsChannel = true;
	TestFalse(TEXT("a channel message is never taken"), Loopback.TryAcceptOutbound(Channel));

	// Only an actor update has an inbound form to be handed back under. Anything else would reach a
	// decoder that refuses it, so it belongs on the wire.
	FCrowdyCppOutboundFrame OtherOpcode = ArmedFrame.Frame;
	OtherOpcode.Opcode = static_cast<uint8>(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION);
	TestFalse(TEXT("a message that is not an actor update is left to the wire"),
		Loopback.TryAcceptOutbound(OtherOpcode));

	TestEqual(TEXT("only the armed update was counted"), Loopback.GetStats().Accepted, static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackHoldsForTheLatencyTest,
	"CrowdySDK.CrowdyNet.LoopbackHoldsForTheLatency",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackHoldsForTheLatencyTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(MakeArrayView(&Armed, 1), PunctualSettings());

	const FOutboundFixture Sent(Armed);
	TestTrue(TEXT("the update was taken"), Loopback.TryAcceptOutbound(Sent.Frame));

	TArray<FDeliveredFrame> Delivered;

	// The property that makes this a stand-in for a network at all. An update delivered in the call
	// that sent it would give interpolation and the arrival-interval estimate a cadence no two machines
	// produce, and every reading taken against it would describe a game nobody plays.
	Loopback.SetTimeForTests(100.039);
	TestEqual(TEXT("nothing is delivered before the latency has run out"), DrainInto(Loopback, Delivered), 0);

	Loopback.SetTimeForTests(100.041);
	TestEqual(TEXT("the update is delivered once the latency has run out"), DrainInto(Loopback, Delivered), 1);

	if (!TestEqual(TEXT("exactly one frame arrived"), Delivered.Num(), 1))
	{
		return false;
	}

	// A client sends an actor update as a request and receives one as a notification. Handing the
	// request straight back would reach a decoder that refuses it, so this substitution is the one
	// thing the server would have done that has to be done here.
	TestEqual(TEXT("it arrives under the inbound opcode"), static_cast<int32>(Delivered[0].Opcode),
		static_cast<int32>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION));

	TestTrue(TEXT("it carries the id it was sent for"), Delivered[0].Uuid == Armed);
	TestEqual(TEXT("it carries the app it was sent for"), Delivered[0].AppId, static_cast<int64>(4242));
	TestEqual(TEXT("it carries the chunk it was sent from"), Delivered[0].ChunkX, static_cast<int64>(1));
	TestEqual(TEXT("it carries the chunk it was sent from"), Delivered[0].ChunkY, static_cast<int64>(2));
	TestEqual(TEXT("it carries the chunk it was sent from"), Delivered[0].ChunkZ, static_cast<int64>(3));

	// Asserted against the octets that were sent rather than against a second serialization of them.
	// Comparing two serializations would agree even if the payload were dropped at both ends.
	TestEqual(TEXT("the state payload arrives whole"), Delivered[0].Body.Num(), Sent.Payload.Num());
	TestTrue(TEXT("the state payload arrives unchanged"), Delivered[0].Body == Sent.Payload);

	TestEqual(TEXT("nothing is left waiting"), DrainInto(Loopback, Delivered), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackDeliversInDueOrderTest,
	"CrowdySDK.CrowdyNet.LoopbackDeliversInDueOrder",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackDeliversInDueOrderTest::RunTest(const FString&)
{
	const FCrowdyActorId First = TestActorId('a');
	const FCrowdyActorId Second = TestActorId('b');
	const TArray<FCrowdyActorId> Armed = { First, Second };

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(Armed, PunctualSettings());

	const FOutboundFixture Early(First, 1);
	Loopback.TryAcceptOutbound(Early.Frame);

	// Sent a whole interval later, so its delay runs out a whole interval later. A queue that delivered
	// in arrival order rather than due order would still get this right, which is why the drain below
	// is taken in two steps at two times rather than in one.
	Loopback.SetTimeForTests(100.1);
	const FOutboundFixture Late(Second, 2);
	Loopback.TryAcceptOutbound(Late.Frame);

	TArray<FDeliveredFrame> Delivered;

	Loopback.SetTimeForTests(100.05);
	TestEqual(TEXT("only the update whose delay has run out is delivered"), DrainInto(Loopback, Delivered), 1);
	TestTrue(TEXT("and it is the one sent first"), Delivered[0].Uuid == First);

	Loopback.SetTimeForTests(100.15);
	TestEqual(TEXT("the second follows when its own delay runs out"), DrainInto(Loopback, Delivered), 1);
	TestTrue(TEXT("and it is the one sent second"), Delivered[1].Uuid == Second);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackHonoursTheDrainBudgetTest,
	"CrowdySDK.CrowdyNet.LoopbackHonoursTheDrainBudget",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackHonoursTheDrainBudgetTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(MakeArrayView(&Armed, 1), PunctualSettings());

	const FOutboundFixture Sent(Armed);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Loopback.TryAcceptOutbound(Sent.Frame);
	}

	Loopback.SetTimeForTests(100.05);

	TArray<FDeliveredFrame> Delivered;

	// The budget is not a detail of this class, it is the point of it. The real receive drain takes a
	// bounded number of messages per frame, so a local delivery that handed over everything the moment
	// it was due would show a crowd this client could never have taken off the network, and the frame
	// cost measured against it would describe nothing.
	TestEqual(TEXT("a drain stops at the message budget"), DrainInto(Loopback, Delivered, 4), 4);
	TestEqual(TEXT("the rest is still waiting"), Loopback.GetStats().QueueDepth, 6);

	// The number that says the client cannot keep up, which is the reading this mode exists to produce.
	// Asserted against the message-budget counter specifically: a drain reported as having run out of
	// TIME would send the next reader to raise a budget that was not the one that bound, and the two
	// fixes work against each other.
	TestTrue(TEXT("a drain that spent its message allowance with updates still due reports that budget"),
		Loopback.GetStats().DrainsCutShortByMessageBudget > 0);
	TestEqual(TEXT("and does not report the time budget it did not reach"),
		Loopback.GetStats().DrainsCutShortByTimeBudget, static_cast<int64>(0));

	TestEqual(TEXT("the remainder is delivered by later drains"), DrainInto(Loopback, Delivered), 6);
	TestEqual(TEXT("nothing was lost between them"), Delivered.Num(), 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackRefusesPastItsQueueCapTest,
	"CrowdySDK.CrowdyNet.LoopbackRefusesPastItsQueueCap",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackRefusesPastItsQueueCapTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');

	FCrowdyLoopbackSettings Settings = PunctualSettings();
	Settings.MaxQueuedFrames = 3;

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(MakeArrayView(&Armed, 1), Settings);

	const FOutboundFixture Sent(Armed);
	for (int32 Index = 0; Index < 5; ++Index)
	{
		// Still taken, because the update is not going to the wire either way. Refusing it back to the
		// caller would send it to a server this run is deliberately not talking to.
		TestTrue(TEXT("an update past the cap is still taken rather than escaping to the wire"),
			Loopback.TryAcceptOutbound(Sent.Frame));
	}

	const FCrowdyLoopbackStats Stats = Loopback.GetStats();
	TestEqual(TEXT("the queue is held at its cap"), Stats.QueueDepth, 3);
	TestEqual(TEXT("and the surplus is counted rather than lost quietly"), Stats.DroppedToQueueFull,
		static_cast<int64>(2));

	// The newest is refused rather than the oldest evicted, because the oldest is the one closest to
	// being due: dropping it would push the delivered latency of everything that did arrive up by a
	// whole queue, and that latency is the property this is here to hold steady.
	TArray<FDeliveredFrame> Delivered;
	Loopback.SetTimeForTests(100.05);
	TestEqual(TEXT("the frames that were kept are the ones delivered"), DrainInto(Loopback, Delivered), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackDropsToSimulatedLossTest,
	"CrowdySDK.CrowdyNet.LoopbackDropsToSimulatedLoss",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackDropsToSimulatedLossTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');

	FCrowdyLoopbackSettings Settings = PunctualSettings();
	Settings.LossFraction = 1.0f;

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);
	Loopback.Arm(MakeArrayView(&Armed, 1), Settings);

	const FOutboundFixture Sent(Armed);
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestTrue(TEXT("a lost update is still taken rather than escaping to the wire"),
			Loopback.TryAcceptOutbound(Sent.Frame));
	}

	const FCrowdyLoopbackStats Stats = Loopback.GetStats();
	TestEqual(TEXT("every update was accounted for"), Stats.Accepted, static_cast<int64>(5));
	TestEqual(TEXT("and every one was lost"), Stats.DroppedToLoss, static_cast<int64>(5));
	TestEqual(TEXT("so nothing is waiting to be delivered"), Stats.QueueDepth, 0);

	TArray<FDeliveredFrame> Delivered;
	Loopback.SetTimeForTests(100.05);
	TestEqual(TEXT("and nothing is delivered"), DrainInto(Loopback, Delivered), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackDisarmsCompletelyTest,
	"CrowdySDK.CrowdyNet.LoopbackDisarmsCompletely",
	CrowdyLoopbackTestFlags)

bool FCrowdyLoopbackDisarmsCompletelyTest::RunTest(const FString&)
{
	const FCrowdyActorId Armed = TestActorId('a');

	FCrowdyCppLoopback Loopback;
	Loopback.SetTimeForTests(100.0);

	TestFalse(TEXT("a loopback that was never armed intercepts nothing"), Loopback.IsArmed());

	// An empty id set arms nothing, since interception is by id and there would be no id to match. A
	// loopback that reported itself armed on an empty set would take the send path's branch on every
	// message for the rest of the session and never take one.
	Loopback.Arm(TConstArrayView<FCrowdyActorId>(), PunctualSettings());
	TestFalse(TEXT("arming with no ids arms nothing"), Loopback.IsArmed());

	Loopback.Arm(MakeArrayView(&Armed, 1), PunctualSettings());
	TestTrue(TEXT("arming with an id arms"), Loopback.IsArmed());

	const FOutboundFixture Sent(Armed);
	Loopback.TryAcceptOutbound(Sent.Frame);

	Loopback.Disarm();
	TestFalse(TEXT("disarming disarms"), Loopback.IsArmed());

	// Held frames go with it. Delivering them after the run that produced them has stopped would put
	// entities into a world whose generator is no longer moving them.
	TestFalse(TEXT("and an update for the formerly armed id goes to the wire again"),
		Loopback.TryAcceptOutbound(Sent.Frame));

	TArray<FDeliveredFrame> Delivered;
	Loopback.SetTimeForTests(100.05);
	TestEqual(TEXT("and nothing held over is delivered"), DrainInto(Loopback, Delivered), 0);
	return true;
}

#endif
