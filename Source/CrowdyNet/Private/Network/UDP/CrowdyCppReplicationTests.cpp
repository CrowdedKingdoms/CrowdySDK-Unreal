#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppReplication.h"
#include "CrowdyCppVideo.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "Messages/Actor/FActorHeartbeatRequestMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FTextMessageRequest.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Messages/Voxel/FVoxelStateUpdateRequest.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/StrongObjectPtr.h"

#include <atomic>

#include "Serialization/CrowdyWireParitySupport.h"
#include "Network/UDP/CrowdyCppReplicationTestSupport.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationRejectsUnusableTokenTest,
	"CrowdySDK.Transport.MakeRejectsUnusableToken",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationRejectsUnusableTokenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	AddExpectedErrorPlain(TEXT("cannot open a connection without a"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();
	Config.Token.Token = TEXT("too-short");

	const TSharedPtr<FCrowdyCppReplication> Short =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	TestNull(TEXT("a token that cannot sign yields no connection"), Short.Get());

	Config.Token = GoodToken();
	Config.Token.bOk = false;
	const TSharedPtr<FCrowdyCppReplication> NotOk =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	TestNull(TEXT("a token reported as unusable yields no connection"), NotOk.Get());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationStartsIdleTest,
	"CrowdySDK.Transport.MakeStartsIdleAndUnassigned",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationStartsIdleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	const TSharedPtr<FCrowdyCppReplication> Connection =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	TestEqual(TEXT("nothing has been opened yet"), static_cast<int32>(Connection->GetState()),
		static_cast<int32>(ECrowdyCppConnState::Idle));
	TestFalse(TEXT("no server is assigned yet"), Connection->GetAssignment().bOk);
	TestEqual(TEXT("nothing has been delivered yet"), Connection->Poll(), 0);
	TestEqual(TEXT("nothing has been sent yet"), Connection->GetStats().DatagramsSent, static_cast<int64>(0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationRefusedAssignmentTest,
	"CrowdySDK.Transport.RefusedAssignmentReportsFailed",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationRefusedAssignmentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// Declared before the connection so it outlives the network thread that writes to it.
	std::atomic<int32> AssignCalls{0};

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection = FCrowdyCppReplication::Make(Config,
		[&AssignCalls](const FCrowdyCppShouldAbort&)
		{
			AssignCalls.fetch_add(1);
			FCrowdyCppSessionAssignment Answer;
			Answer.bOk = false;
			Answer.ErrorMessage = TEXT("no server for this test");
			return Answer;
		},
		NeverRefresh());

	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	Connection->ConnectAsync();

	const bool bReachedFailed = WaitUntil([&Connection]()
	{
		return Connection->GetState() == ECrowdyCppConnState::Failed;
	});

	TestTrue(TEXT("a refused assignment ends in Failed"), bReachedFailed);
	TestTrue(TEXT("the assignment callback actually ran"), AssignCalls.load() >= 1);
	TestFalse(TEXT("no endpoint was adopted"), Connection->GetAssignment().bOk);

	Connection->Disconnect();
	Connection.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationSendGuardsTest,
	"CrowdySDK.Transport.SendsAreGuardedBeforeConnect",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationSendGuardsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	const TArray<uint8> Payload = {0xde, 0xad};
	FString Error;

	TestFalse(TEXT("an unopened connection refuses a spatial send"),
		Connection->SendSpatial(140, 1, 2, 3, GoldenUuidView(), Payload, 8, 0, Error));
	TestFalse(TEXT("the refusal carries a reason"), Error.IsEmpty());

	Error.Reset();
	TestFalse(TEXT("an actor id of the wrong length is refused"),
		Connection->SendSpatial(140, 1, 2, 3, TArrayView<const uint8>(Payload), Payload, 8, 0, Error));
	TestTrue(TEXT("the refusal names the length"), Error.Contains(TEXT("octets")));

	// A notification opcode is something the server sends, never the client, so it must not be routable outbound.
	Error.Reset();
	TestFalse(TEXT("an inbound-only opcode is refused"),
		Connection->SendSpatial(130, 1, 2, 3, GoldenUuidView(), Payload, 8, 0, Error));
	TestTrue(TEXT("the refusal names the opcode"), Error.Contains(TEXT("130")));

	TestFalse(TEXT("an actor update notification is not sendable"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(130));
	TestTrue(TEXT("an actor update request is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(128));
	TestTrue(TEXT("a voxel update request is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(131));
	TestTrue(TEXT("a client event is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(138));
	TestTrue(TEXT("an actor-to-actor message is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(142));
	TestTrue(TEXT("a heartbeat is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(26));
	TestFalse(TEXT("a server event is not sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(139));

	// The receive gate is the mirror image: request opcodes the client emits must never be handed back to it.
	TestTrue(TEXT("an actor update notification is deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(130));
	TestTrue(TEXT("a server event is deliverable"), FCrowdyCppReplication::IsDeliverableSpatialOpcode(139));
	TestTrue(TEXT("an actor left notification is deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(145));
	TestFalse(TEXT("an actor left notification is not sendable"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(145));
	// Video, which travels the opposite way to actor left: the client sends a fragment and the server fans
	// out a notification, so each opcode has to be refused by the gate the other one passes.
	TestTrue(TEXT("a video packet is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(143));
	TestFalse(TEXT("a video packet is not deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(143));
	TestTrue(TEXT("a video notification is deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(144));
	TestFalse(TEXT("a video notification is not sendable"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(144));
	TestFalse(TEXT("an actor update request is not deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(128));
	TestFalse(TEXT("a voxel update request is not deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(131));
	TestFalse(TEXT("a heartbeat is not deliverable"), FCrowdyCppReplication::IsDeliverableSpatialOpcode(26));

	Connection->Disconnect();
	Connection.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationWireParityTest,
	"CrowdySDK.Transport.SpatialSendMatchesTheGoldenDatagram",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationWireParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> Payload = {0xde, 0xad, 0xbe, 0xef};

	// Sequence numbers belong to the connection, which hands them out in send order starting at zero, so the
	// forty-third send is the one the golden vector was computed for.
	constexpr uint8 GoldenSequence = 42;
	constexpr int32 SendsBeforeTheGoldenOne = GoldenSequence + 1;

	FString Error;
	bool bAllAccepted = true;
	for (int32 Index = 0; Index < SendsBeforeTheGoldenOne; ++Index)
	{
		bAllAccepted &= Fixture.Connection->SendSpatial(140, 1, -2, 3, GoldenUuidView(), Payload, 8,
			static_cast<uint8>(ECrowdyDecayRate::Exponential_Decay), Error);
	}

	if (!TestTrue(FString::Printf(TEXT("every send was accepted (%s)"), *Error), bAllAccepted))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> OnTheWire = Fixture.Server.ReceiveWithSequence(GoldenSequence);
	if (!TestTrue(TEXT("the datagram reached the stand-in server"), OnTheWire.Num() > 0))
	{
		Fixture.Shut();
		return false;
	}

	// Against Unreal's own encoder first: this is the cross-implementation half, and it is what would catch the
	// facade splitting a message the wrong way.
	const TArray<uint8> FromUnreal = ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, -2, 3, Payload, 8,
		static_cast<uint8>(ECrowdyDecayRate::Exponential_Decay), 123456789, GoldenSequence);
	TestEqual(TEXT("the datagram matches Unreal's own encoder"),
		CrowdyWireParity::DescribeDifference(OnTheWire, FromUnreal), FString(TEXT("identical")));

	// Then against the independently derived vector, which is the only check that can catch both encoders
	// drifting the same way.
	TestEqual(TEXT("the datagram matches the golden vector"),
		CrowdyWireParity::DescribeDifference(OnTheWire, CrowdyWireParity::GoldenSpatialDatagram()),
		FString(TEXT("identical")));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationSplitOpcodeParityTest,
	"CrowdySDK.Transport.SplitPayloadSendsMatchUnrealEncoder",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationSplitOpcodeParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// These two opcodes are the ones whose payload the facade takes apart and the library puts back together, so
	// they are the only place a re-split can lose or reorder bytes. The generic-spatial parity test cannot see it.
	TArray<uint8> VoxelPayload;
	VoxelPayload.Append({0x01, 0x00});          // voxel x
	VoxelPayload.Append({0xff, 0xff});          // voxel y
	VoxelPayload.Append({0x03, 0x00});          // voxel z
	VoxelPayload.Append({0x07, 0x00});          // voxel type
	VoxelPayload.Append({0x04, 0x00});          // declared state length
	VoxelPayload.Append({0xaa, 0xbb, 0xcc, 0xdd});

	TArray<uint8> EventPayload;
	EventPayload.Append({0x11, 0x22});          // event type
	EventPayload.Append({0x01, 0x02, 0x03, 0x04, 0x05});

	FString Error;
	const bool bVoxelAccepted = Fixture.Connection->SendSpatial(131, 4, 5, 6, GoldenUuidView(), VoxelPayload, 6, 2,
		Error);
	if (!TestTrue(FString::Printf(TEXT("the voxel send was accepted (%s)"), *Error), bVoxelAccepted))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> VoxelOnTheWire = Fixture.Server.ReceiveWithSequence(0);
	TestEqual(TEXT("the voxel datagram matches Unreal's own encoder"),
		CrowdyWireParity::DescribeDifference(VoxelOnTheWire,
			ExpectedSpatial(ECrowdyMessageType::VOXEL_UPDATE_REQUEST, 7, 4, 5, 6, VoxelPayload, 6, 2, 123456789, 0)),
		FString(TEXT("identical")));

	const bool bEventAccepted = Fixture.Connection->SendSpatial(138, 7, 8, 9, GoldenUuidView(), EventPayload, 3, 1,
		Error);
	if (!TestTrue(FString::Printf(TEXT("the event send was accepted (%s)"), *Error), bEventAccepted))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> EventOnTheWire = Fixture.Server.ReceiveWithSequence(1);
	TestEqual(TEXT("the event datagram matches Unreal's own encoder"),
		CrowdyWireParity::DescribeDifference(EventOnTheWire,
			ExpectedSpatial(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, 7, 7, 8, 9, EventPayload, 3, 1, 123456789,
				1)),
		FString(TEXT("identical")));

	const FCrowdyCppReplicationStats Stats = Fixture.Connection->GetStats();
	TestEqual(TEXT("exactly the two sends were counted"), Stats.DatagramsSent, static_cast<int64>(2));
	TestEqual(TEXT("the counted bytes are the two datagrams"), Stats.BytesSent,
		static_cast<int64>(VoxelOnTheWire.Num() + EventOnTheWire.Num()));
	TestEqual(TEXT("nothing was dropped"), Stats.SendsDropped, static_cast<int64>(0));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationVoxelConsistencyTest,
	"CrowdySDK.Transport.VoxelPayloadMustBeSelfConsistent",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationVoxelConsistencyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// A voxel payload declares its state length separately from the bytes that follow it, and the two have drifted
	// apart in this codebase before. Both directions have to be refused rather than silently reshaped: over-declaring
	// would send a frame the receiver reads past the end of, and under-declaring would drop the tail.
	TArray<uint8> OverDeclared;
	OverDeclared.Append({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
	OverDeclared.Append({0x28, 0x00});          // declares 40 octets of state and carries none

	TArray<uint8> UnderDeclared;
	UnderDeclared.Append({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
	UnderDeclared.Append({0x02, 0x00});         // declares 2 octets of state
	UnderDeclared.Append({0xaa, 0xbb, 0xcc});   // and carries 3

	FString Error;
	TestFalse(TEXT("a voxel payload that declares more state than it carries is refused"),
		Fixture.Connection->SendSpatial(131, 0, 0, 0, GoldenUuidView(), OverDeclared, 8, 0, Error));
	TestTrue(TEXT("the refusal says which way it disagrees"), Error.Contains(TEXT("declares")));

	Error.Reset();
	TestFalse(TEXT("a voxel payload that carries more state than it declares is refused"),
		Fixture.Connection->SendSpatial(131, 0, 0, 0, GoldenUuidView(), UnderDeclared, 8, 0, Error));
	TestTrue(TEXT("the refusal says which way it disagrees"), Error.Contains(TEXT("declares")));

	Error.Reset();
	TestFalse(TEXT("an event payload with no event type is refused"),
		Fixture.Connection->SendSpatial(138, 0, 0, 0, GoldenUuidView(), TArray<uint8>{0x01}, 8, 0, Error));

	Error.Reset();
	TestFalse(TEXT("a heartbeat carrying a payload is refused"),
		Fixture.Connection->SendSpatial(26, 0, 0, 0, GoldenUuidView(), TArray<uint8>{0x01}, 8, 0, Error));

	TestEqual(TEXT("nothing refused reached the wire"), Fixture.Connection->GetStats().DatagramsSent,
		static_cast<int64>(0));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationInboundTest,
	"CrowdySDK.Transport.InboundFramesAreGatedByOpcode",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationInboundTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	TArray<uint8> Delivered;
	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnSpatial = [&Delivered](const FCrowdyCppSpatialMessage& Message)
	{
		Delivered.Add(Message.Opcode);
	};
	Fixture.Connection->SetHandlers(MoveTemp(Handlers));

	// One send so the stand-in server learns where to answer.
	FString Error;
	const TArray<uint8> Ping = {0x01};
	if (!TestTrue(TEXT("the priming send was accepted"),
		Fixture.Connection->SendSpatial(140, 0, 0, 0, GoldenUuidView(), Ping, 8, 0, Error)))
	{
		Fixture.Shut();
		return false;
	}
	if (!TestTrue(TEXT("the priming send arrived"), Fixture.Server.Receive().Num() > 0))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> Payload = {0x01, 0x02, 0x03, 0x04};

	// Both frames are signed with the same token and both verify, so the only thing that can tell them apart is
	// the receive-side opcode gate.
	const TArray<uint8> Notification = ExpectedSpatial(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, 7, 1, 2, 3,
		Payload, 8, 0, 1700000000000, 5);
	const TArray<uint8> Request = ExpectedSpatial(ECrowdyMessageType::ACTOR_UPDATE_REQUEST, 7, 1, 2, 3, Payload, 8, 0,
		1700000000000, 6);

	TestTrue(TEXT("the notification was pushed to the client"), Fixture.Server.SendToClient(Notification));
	TestTrue(TEXT("the request was pushed to the client"), Fixture.Server.SendToClient(Request));

	const bool bSawNotification = WaitUntil([&Fixture, &Delivered]()
	{
		Fixture.Connection->Poll();
		return Delivered.Contains(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION));
	});

	TestTrue(TEXT("a signed notification reaches the handler"), bSawNotification);

	// Give the second frame every chance to arrive before concluding it was dropped.
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Fixture.Connection->Poll();
		FPlatformProcess::Sleep(0.01f);
	}

	TestFalse(TEXT("a signed request opcode is not handed back to the client"),
		Delivered.Contains(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST)));

	const FCrowdyCppReplicationStats Stats = Fixture.Connection->GetStats();
	TestTrue(TEXT("both frames were received and verified"), Stats.MessagesReceived >= 2);
	TestEqual(TEXT("neither frame failed verification"), Stats.HmacFailures, static_cast<int64>(0));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationAbortTest,
	"CrowdySDK.Transport.BlockingCallbackHonoursTheAbort",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationAbortTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// Declared before the connection so they outlive the network thread that writes to them.
	std::atomic<bool> bCallbackEntered{false};
	std::atomic<bool> bSawAbort{false};

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection = FCrowdyCppReplication::Make(Config,
		[&bCallbackEntered, &bSawAbort](const FCrowdyCppShouldAbort& ShouldAbort)
		{
			bCallbackEntered.store(true);

			// Stands in for a real assignment, which is a network round trip. Teardown must not have to wait it out.
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				if (ShouldAbort())
				{
					bSawAbort.store(true);
					break;
				}
				FPlatformProcess::Sleep(0.01f);
			}

			FCrowdyCppSessionAssignment Answer;
			Answer.bOk = false;
			Answer.ErrorMessage = TEXT("abandoned");
			return Answer;
		},
		NeverRefresh());

	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	Connection->ConnectAsync();
	if (!TestTrue(TEXT("the assignment callback was entered"),
		WaitUntil([&bCallbackEntered]() { return bCallbackEntered.load(); })))
	{
		Connection->Disconnect();
		return false;
	}

	const double Started = FPlatformTime::Seconds();
	Connection->Disconnect();
	const double Elapsed = FPlatformTime::Seconds() - Started;

	TestTrue(TEXT("the blocking callback was told to give up"), bSawAbort.load());
	TestTrue(FString::Printf(TEXT("teardown did not wait out the callback (%.2fs)"), Elapsed), Elapsed < 10.0);
	TestEqual(TEXT("a closed connection reports Closed"), static_cast<int32>(Connection->GetState()),
		static_cast<int32>(ECrowdyCppConnState::Closed));

	Connection.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationDisconnectIsTerminalTest,
	"CrowdySDK.Transport.DisconnectIsTerminal",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationDisconnectIsTerminalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	int32 StateCallbacks = 0;
	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnStateChanged = [&StateCallbacks](ECrowdyCppConnState)
	{
		++StateCallbacks;
	};
	Fixture.Connection->SetHandlers(MoveTemp(Handlers));

	// Establish that callbacks were flowing before the close, so the "nothing after" claim below is a real
	// comparison rather than zero against zero.
	const bool bSawAnything = WaitUntil([&Fixture, &StateCallbacks]()
	{
		Fixture.Connection->Poll();
		return StateCallbacks > 0;
	});

	if (!TestTrue(TEXT("state changes were reaching the handler before the close"), bSawAnything))
	{
		Fixture.Shut();
		return false;
	}

	Fixture.Connection->Disconnect();
	const int32 CallbacksAtClose = StateCallbacks;

	TestEqual(TEXT("a closed connection reports Closed"), static_cast<int32>(Fixture.Connection->GetState()),
		static_cast<int32>(ECrowdyCppConnState::Closed));
	TestEqual(TEXT("polling a closed connection delivers nothing"), Fixture.Connection->Poll(), 0);
	TestEqual(TEXT("no handler ran after the close"), StateCallbacks, CallbacksAtClose);
	TestFalse(TEXT("a closed connection reports no endpoint"), Fixture.Connection->GetAssignment().bOk);

	// Reopening is not offered, so asking for it must be inert rather than half-effective.
	Fixture.Connection->ConnectAsync();
	TestEqual(TEXT("a closed connection does not reopen"), static_cast<int32>(Fixture.Connection->GetState()),
		static_cast<int32>(ECrowdyCppConnState::Closed));

	FString Error;
	const TArray<uint8> Payload = {0x01};
	TestFalse(TEXT("a closed connection refuses a spatial send"),
		Fixture.Connection->SendSpatial(140, 0, 0, 0, GoldenUuidView(), Payload, 8, 0, Error));
	TestFalse(TEXT("a closed connection refuses a channel send"),
		Fixture.Connection->SendChannelMessage(1, GoldenUuidView(), Payload, Error));

	// Calling it twice must be a no-op rather than a second teardown.
	Fixture.Connection->Disconnect();
	TestEqual(TEXT("a second close changes nothing"), static_cast<int32>(Fixture.Connection->GetState()),
		static_cast<int32>(ECrowdyCppConnState::Closed));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationHandlerReleaseTest,
	"CrowdySDK.Transport.AHandlerMayReleaseTheConnection",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationHandlerReleaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	// Deciding the transport is dead and dropping it is an ordinary reaction to a Failed state, and it happens
	// while the delivery that reported it is still on the stack.
	bool bReleased = false;
	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnStateChanged = [&Connection, &bReleased](const ECrowdyCppConnState State)
	{
		if (State == ECrowdyCppConnState::Failed && !bReleased)
		{
			bReleased = true;
			Connection.Reset();
		}
	};
	Connection->SetHandlers(MoveTemp(Handlers));

	Connection->ConnectAsync();
	WaitUntil([&Connection]()
	{
		return Connection.IsValid() && Connection->GetState() == ECrowdyCppConnState::Failed;
	});

	// Poll on a raw handle: the last shared reference is what the handler drops, and the call must survive it.
	FCrowdyCppReplication* Raw = Connection.Get();
	const bool bSurvived = WaitUntil([Raw, &bReleased]()
	{
		if (bReleased)
		{
			return true;
		}
		Raw->Poll();
		return bReleased;
	});

	TestTrue(TEXT("the handler ran and released the connection"), bSurvived);
	TestFalse(TEXT("the shared reference is gone"), Connection.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationHandlerThreadTest,
	"CrowdySDK.Transport.HandlersRunOnThePollingThread",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationHandlerThreadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// Declared before the connection so they outlive the network thread that writes to them.
	std::atomic<uint32> NetworkThreadId{0};
	TArray<ECrowdyCppConnState> Observed;
	TArray<uint32> ObservedThreads;

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection = FCrowdyCppReplication::Make(Config,
		[&NetworkThreadId](const FCrowdyCppShouldAbort&)
		{
			NetworkThreadId.store(FPlatformTLS::GetCurrentThreadId());
			FCrowdyCppSessionAssignment Answer;
			Answer.bOk = false;
			Answer.ErrorMessage = TEXT("no server for this test");
			return Answer;
		},
		NeverRefresh());

	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	const uint32 PollingThread = FPlatformTLS::GetCurrentThreadId();

	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnStateChanged = [&Observed, &ObservedThreads](const ECrowdyCppConnState State)
	{
		Observed.Add(State);
		ObservedThreads.Add(FPlatformTLS::GetCurrentThreadId());
	};
	Connection->SetHandlers(MoveTemp(Handlers));

	Connection->ConnectAsync();
	WaitUntil([&Connection]() { return Connection->GetState() == ECrowdyCppConnState::Failed; });

	// Nothing may have been delivered yet: the state changes are queued by the network thread and handed over only
	// by Poll, which is the property being asserted.
	TestEqual(TEXT("no handler ran before the first poll"), Observed.Num(), 0);

	const bool bDelivered = WaitUntil([&Connection, &Observed]()
	{
		Connection->Poll();
		return Observed.Contains(ECrowdyCppConnState::Failed);
	});

	TestTrue(TEXT("the failure reached the handler"), bDelivered);

	const uint32 NetworkThread = NetworkThreadId.load();
	TestTrue(TEXT("the network thread was a different thread"), NetworkThread != 0 && NetworkThread != PollingThread);

	bool bAllOnPollingThread = ObservedThreads.Num() > 0;
	for (const uint32 ThreadId : ObservedThreads)
	{
		bAllOnPollingThread &= ThreadId == PollingThread && ThreadId != NetworkThread;
	}
	TestTrue(TEXT("every callback ran on the polling thread rather than the network thread"), bAllOnPollingThread);

	Connection->Disconnect();
	Connection.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedSpatialParityTest,
	"CrowdySDK.Transport.RoutedSpatialSendsMatchTheHandRolledFrame",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedSpatialParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// The claim under test: taking a real message apart with the adapter and
	// putting it back together through the connection produces the same datagram the hand-rolled transport produces
	// from the same message. Comparing against that message's own Serialize plus the signed trailer means the
	// reference is the shipped encoder, not a restatement of it.
	int32 SendIndex = 0;
	auto RouteAndCompare = [this, &Fixture, &SendIndex](const ICrowdyMessage& Message, const TCHAR* What) -> bool
	{
		// Kept alive across the send: the frame's actor id and payload are views into it.
		TArray<uint8> Serialized;
		FCrowdyCppOutboundFrame Frame;
		FString SplitError;
		const bool bSplit = CrowdyCppSend::SplitMessage(Message, Serialized, Frame, SplitError);
		if (!TestTrue(FString::Printf(TEXT("%s was split (%s)"), What, *SplitError), bSplit))
		{
			return false;
		}

		FString SendError;
		const bool bAccepted = Fixture.Connection->SendSpatial(Frame.Opcode, Frame.ChunkX, Frame.ChunkY, Frame.ChunkZ,
			Frame.Uuid, Frame.Payload, Frame.Distance, Frame.Decay, SendError);
		if (!TestTrue(FString::Printf(TEXT("%s was accepted (%s)"), What, *SendError), bAccepted))
		{
			return false;
		}

		// The connection owns the sequence counter and hands them out in send order from zero, so the datagram is
		// identified by the sequence this send must have rather than by how long it took to arrive.
		const uint8 Sequence = static_cast<uint8>(SendIndex++);
		const TArray<uint8> OnTheWire = Fixture.Server.ReceiveWithSequence(Sequence);
		if (!TestTrue(FString::Printf(TEXT("%s reached the stand-in server"), What), OnTheWire.Num() > 0))
		{
			return false;
		}

		const TArray<uint8> HandRolled = CrowdyWireParity::AppendSignedTail(Message.Serialize(), GoldenTokenString(),
			123456789, Sequence, true);
		TestEqual(FString::Printf(TEXT("%s matches the hand-rolled datagram"), What),
			CrowdyWireParity::DescribeDifference(OnTheWire, HandRolled), FString(TEXT("identical")));
		return true;
	};

	// Opcode 128, and the one that must not break: this is what the Overworld sends for every actor every tick.
	{
		FActorUpdateRequestMessage Message;
		Message.AppID = 7;
		Message.ChunkX = 1;
		Message.ChunkY = -2;
		Message.ChunkZ = 3;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Exponential_Decay;
		Message.StateBytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
		Message.StateSize = Message.StateBytes.Num();
		RouteAndCompare(Message, TEXT("an actor update"));
	}

	// Opcode 131, re-split by the library into its typed parts and re-encoded, so a lost or reordered field shows up
	// here and nowhere else.
	{
		FVoxelStateUpdateRequest Message;
		Message.AppID = 7;
		Message.ChunkX = 4;
		Message.ChunkY = 5;
		Message.ChunkZ = 6;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::Six_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Linear_50;
		Message.Vx = 1;
		Message.Vy = -1;
		Message.Vz = 3;
		Message.VoxelType = 7;
		Message.StateBytes = {0xaa, 0xbb, 0xcc, 0xdd};
		Message.StateSize = static_cast<uint16>(Message.StateBytes.Num());
		Message.bContainsState = true;
		RouteAndCompare(Message, TEXT("a voxel state update"));
	}

	// Opcode 138. The library reads the two-octet event type back out and treats everything after it as opaque state,
	// which is what lets the Target and TargetID this message appends after its payload survive the round trip.
	{
		FGameEventRequest Message;
		Message.AppID = 7;
		Message.ChunkX = 7;
		Message.ChunkY = 8;
		Message.ChunkZ = 9;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::Three_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Exponential_Decay;
		Message.EventType = 0x2211;
		Message.StateBytes = {0x0a, 0x0b, 0x0c};
		Message.StateSize = Message.StateBytes.Num();
		Message.Target = ECrowdyTarget::Everyone;
		Message.TargetID = FGuid(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00);
		RouteAndCompare(Message, TEXT("a game event"));
	}

	// Opcode 142. The server delivers this to one recipient rather than fanning it out, so the library writes zero
	// distance and zero decay whatever it is handed. Parity therefore depends on the message agreeing, which it does
	// only because both of those enums have a zero-valued default.
	{
		FSingleActorRequest Message;
		Message.AppID = 7;
		Message.ChunkX = -11;
		Message.ChunkY = 12;
		Message.ChunkZ = -13;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::None;
		Message.DecayRate = ECrowdyDecayRate::No_Decay;
		Message.EventType = 0x0102;
		Message.StateBytes = {0xfe};
		Message.StateSize = Message.StateBytes.Num();
		Message.Target = ECrowdyTarget::Everyone;
		Message.TargetID = FGuid(1, 2, 3, 4);
		RouteAndCompare(Message, TEXT("a single actor message"));
	}

	// Opcode 136, a straight pass-through with a variable-length payload, which covers the same shape as the audio
	// packet on 134 and the ping on 140.
	{
		FTextMessageRequest Message;
		Message.AppID = 7;
		Message.ChunkX = 0;
		Message.ChunkY = 0;
		Message.ChunkZ = 0;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::Four_Chunks;
		Message.DecayRate = ECrowdyDecayRate::No_Decay;
		Message.UserID = 99;
		Message.Username = TEXT("tester");
		Message.Message = TEXT("hello");
		RouteAndCompare(Message, TEXT("a text message"));
	}

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedChannelParityTest,
	"CrowdySDK.Transport.RoutedChannelSendMatchesTheHandRolledFrame",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedChannelParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// The channel frame has its own layout, shorter than a spatial one and with the signed marker after the payload
	// instead of in a header. Nothing had ever checked it against the vendored encoder, and it is what every reliable
	// RPC and every subsystem-scoped state delta rides on.
	FChannelMessageRequest Message;
	Message.ChannelId = 4242;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.Payload = {0xca, 0xfe, 0xba, 0xbe, 0x00, 0x01};

	TArray<uint8> Serialized;
	FCrowdyCppOutboundFrame Frame;
	FString SplitError;
	const bool bSplit = CrowdyCppSend::SplitMessage(Message, Serialized, Frame, SplitError);
	if (!TestTrue(FString::Printf(TEXT("the channel publish was split (%s)"), *SplitError), bSplit))
	{
		Fixture.Shut();
		return false;
	}

	FString SendError;
	const bool bAccepted = Fixture.Connection->SendChannelMessage(Frame.ChannelId, Frame.Uuid, Frame.Payload,
		SendError);
	if (!TestTrue(FString::Printf(TEXT("the channel publish was accepted (%s)"), *SendError), bAccepted))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> OnTheWire = Fixture.Server.ReceiveWithSequence(0);
	if (!TestTrue(TEXT("the channel datagram reached the stand-in server"), OnTheWire.Num() > 0))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> HandRolled = CrowdyWireParity::AppendSignedTail(Message.Serialize(), GoldenTokenString(),
		123456789, 0, true);
	TestEqual(TEXT("the channel datagram matches the hand-rolled one"),
		CrowdyWireParity::DescribeDifference(OnTheWire, HandRolled), FString(TEXT("identical")));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutingSeamOutcomesTest,
	"CrowdySDK.Transport.SendOutcomesFollowTheInstalledConnection",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutingSeamOutcomesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// The host is a game-instance subsystem, so it can only be constructed inside a game instance. A bare one is
	// enough here: it is never initialised and nothing on the path under test reads it.
	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	if (!TestTrue(TEXT("a game instance to own the host was created"), OuterInstance.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host was created"), Routing.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	FActorUpdateRequestMessage Message;
	Message.AppID = 7;
	Message.ChunkX = 2;
	Message.ChunkY = -4;
	Message.ChunkZ = 6;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.ReplicationDistance = ECrowdyReplicationDistance::Five_Chunks;
	Message.DecayRate = ECrowdyDecayRate::Linear_25;
	Message.StateBytes = {0x10, 0x20, 0x30};
	Message.StateSize = Message.StateBytes.Num();

	Routing->SetConnection(Fixture.Connection);

	TestTrue(TEXT("with a connection installed the host reports routing"), Routing->IsRouting());
	TestEqual(TEXT("a message the wire can carry is sent"),
		static_cast<int32>(Routing->TrySendMessage(Message)),
		static_cast<int32>(ECrowdyCppSendOutcome::Sent));

	// End to end through the real entry point rather than through the split alone: what the seam hands the connection
	// has to be the same datagram the hand-rolled transport would have produced from this message.
	const TArray<uint8> OnTheWire = Fixture.Server.ReceiveWithSequence(0);
	if (TestTrue(TEXT("the routed message reached the stand-in server"), OnTheWire.Num() > 0))
	{
		TestEqual(TEXT("the seam put the hand-rolled datagram on the wire"),
			CrowdyWireParity::DescribeDifference(OnTheWire,
				CrowdyWireParity::AppendSignedTail(Message.Serialize(), GoldenTokenString(), 123456789, 0, true)),
			FString(TEXT("identical")));
	}

	// A message whose actor id was never filled in. The header reserves a fixed 32 octets for it, so
	// there is nothing to write there and the send has to be refused rather than sent addressing nobody.
	FActorUpdateRequestMessage Unroutable = Message;
	Unroutable.UUID.Reset();
	TestEqual(TEXT("a message the wire cannot carry is refused rather than reshaped"),
		static_cast<int32>(Routing->TrySendMessage(Unroutable)),
		static_cast<int32>(ECrowdyCppSendOutcome::Refused));

	// Clearing the connection disposes it, which is why this comes last. There is nowhere else for a message to go,
	// so this outcome means it was dropped.
	Routing->SetConnection(nullptr);
	TestFalse(TEXT("clearing the connection stops the host routing"), Routing->IsRouting());
	TestEqual(TEXT("with no connection installed a message is not routed"),
		static_cast<int32>(Routing->TrySendMessage(Message)),
		static_cast<int32>(ECrowdyCppSendOutcome::NotRouted));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySendWithoutConnectionTest,
	"CrowdySDK.Transport.SendWithoutAConnectionIsNotRouted",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdySendWithoutConnectionTest::RunTest(const FString& Parameters)
{
	// A well-formed message offered to a host with nothing installed. This used to fall through to a second
	// transport, so the outcome had no consequence; now it is the difference between sending and dropping, and
	// nothing else reports it.
	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	if (!TestTrue(TEXT("a game instance to own the host was created"), OuterInstance.IsValid()))
	{
		return false;
	}

	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host was created"), Routing.IsValid()))
	{
		return false;
	}

	FActorUpdateRequestMessage Message;
	Message.AppID = 7;
	Message.ChunkX = 2;
	Message.ChunkY = -4;
	Message.ChunkZ = 6;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.ReplicationDistance = ECrowdyReplicationDistance::Five_Chunks;
	Message.DecayRate = ECrowdyDecayRate::Linear_25;
	Message.StateBytes = {0x10, 0x20, 0x30};
	Message.StateSize = Message.StateBytes.Num();

	TestFalse(TEXT("a host with nothing installed does not report routing"), Routing->IsRouting());
	TestEqual(TEXT("and a well-formed message is not routed"),
		static_cast<int32>(Routing->TrySendMessage(Message)),
		static_cast<int32>(ECrowdyCppSendOutcome::NotRouted));

	// Repeated, because a caller that keeps sending must keep getting the same answer rather than the answer
	// changing once the drop has been reported.
	TestEqual(TEXT("and a second one is still not routed"),
		static_cast<int32>(Routing->TrySendMessage(Message)),
		static_cast<int32>(ECrowdyCppSendOutcome::NotRouted));

	return true;
}

// The spatial ceiling, the sibling of the channel one below, and it had NO coverage at all: disabling the
// check in SendSpatial left the whole suite green, found by slice 2's mutation gate on 2026-09-03.
//
// It matters more than the channel one because the library encodes a long-spatial message into a
// fixed-size stack buffer of exactly kMaxLongSpatialPayload octets. This refusal is what stands between a
// caller that built an oversized frame and that buffer, and the value is not arbitrary: it is the
// IPv6-safe datagram size less the header and the signed tail, so a message past it cannot reach the peer
// whole however it is framed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpatialPayloadCeilingTest,
	"CrowdySDK.Transport.SpatialPayloadCeilingIsRefusedNotTruncated",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdySpatialPayloadCeilingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	constexpr int32 Ceiling = static_cast<int32>(crowdy::wire::kMaxLongSpatialPayload);

	// Pinned, because the whole point of the bound is that it is the datagram size less the framing: a
	// ceiling that drifted would still refuse something, just not the thing that does not fit.
	TestEqual(TEXT("the spatial ceiling is the IPv6-safe datagram less header and signed tail"), Ceiling, 1123);

	TArray<uint8> AtCeiling;
	AtCeiling.SetNumZeroed(Ceiling);
	TArray<uint8> PastCeiling;
	PastCeiling.SetNumZeroed(Ceiling + 1);

	const TArrayView<const uint8> Uuid = GoldenUuidView();

	// Opcode 138 is a client-sendable spatial event, so neither send is refused for a reason other than
	// its size; without this the "refused" half would pass for the wrong reason.
	FString Error;
	TestTrue(FString::Printf(TEXT("a payload at the ceiling is accepted (%s)"), *Error),
		Fixture.Connection->SendSpatial(138, 1, 2, 3, Uuid, AtCeiling, 8, 0, Error));

	Error.Reset();
	TestFalse(TEXT("a payload one octet past the ceiling is refused rather than truncated"),
		Fixture.Connection->SendSpatial(138, 1, 2, 3, Uuid, PastCeiling, 8, 0, Error));
	TestTrue(FString::Printf(TEXT("the refusal says why (%s)"), *Error),
		Error.Contains(TEXT("single datagram")));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedChannelCeilingTest,
	"CrowdySDK.Transport.RoutedChannelPayloadCeilingMatchesTheChannelLimit",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedChannelCeilingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// The connection's channel ceiling is 1024 octets, which is the same number the two producers above this layer
	// already enforce for themselves: a reliable RPC and a channel state delta are each dropped loudly before they
	// get here if their encoded payload is larger. So this pins agreement between the transport and the callers
	// rather than a difference, and it fails if either side moves.
	TArray<uint8> JustUnder;
	JustUnder.SetNumZeroed(1024);
	TArray<uint8> JustOver;
	JustOver.SetNumZeroed(1025);

	const TArrayView<const uint8> Uuid = GoldenUuidView();

	FString Error;
	const bool bUnderAccepted = Fixture.Connection->SendChannelMessage(1, Uuid, JustUnder, Error);
	TestTrue(FString::Printf(TEXT("a payload at the ceiling is accepted (%s)"), *Error), bUnderAccepted);

	const bool bOverAccepted = Fixture.Connection->SendChannelMessage(1, Uuid, JustOver, Error);
	TestFalse(TEXT("a payload past the ceiling is refused rather than truncated"), bOverAccepted);
	TestTrue(FString::Printf(TEXT("the refusal says why (%s)"), *Error), Error.Contains(TEXT("channel message")));

	Fixture.Shut();
	return true;
}

// A server error frame names a sequence number and nothing else. The facade is the only thing that ever sees which
// send the library gave that sequence to, because a queued send returns nothing to its caller, so if it does not
// remember here the link is gone for good. Proved end to end: a real send goes out over the loopback socket, its
// sequence is read off the datagram the server actually received, and an error frame naming that sequence is pushed
// back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerErrorIsAttributedToItsSendTest,
	"CrowdySDK.Transport.ServerErrorIsAttributedToItsSend",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)
bool FCrowdyServerErrorIsAttributedToItsSendTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("the fixture connects"), Fixture.Open()))
	{
		return false;
	}

	bool bReported = false;
	FCrowdyCppSendError Captured;
	TArray<uint8> CapturedUuid;
	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnError = [&bReported, &Captured, &CapturedUuid](const FCrowdyCppSendError& Error)
	{
		Captured = Error;
		// The view borrows the facade's record and does not outlive the call, so it is copied to assert on later.
		CapturedUuid = TArray<uint8>(Error.SendUuid.GetData(), Error.SendUuid.Num());
		bReported = true;
	};
	Fixture.Connection->SetHandlers(Handlers);

	// A text packet rather than the fixture's own priming opcode, so the attribution has to carry this send's
	// opcode rather than merely any opcode.
	constexpr uint8 TextOpcode = static_cast<uint8>(ECrowdyMessageType::CLIENT_TEXT_PACKET);
	FString SendError;
	const TArray<uint8> Payload = {0x41, 0x42, 0x43};
	if (!TestTrue(TEXT("a text send is accepted"),
		Fixture.Connection->SendSpatial(TextOpcode, 1, 2, 3, GoldenUuidView(), Payload, 8, 0, SendError)))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<uint8> OnTheWire = Fixture.Server.Receive();
	if (!TestTrue(TEXT("the send reached the stand-in server"), OnTheWire.Num() > 0))
	{
		Fixture.Shut();
		return false;
	}

	// The sequence is the last octet of the datagram, which is where the protocol puts it on both directions.
	const uint8 Sequence = OnTheWire.Last();

	constexpr uint8 InvalidAppId = 18;
	const TArray<uint8> ErrorFrame =
		{static_cast<uint8>(ECrowdyMessageType::GENERIC_ERROR_MESSAGE), Sequence, InvalidAppId};
	TestTrue(TEXT("an error frame naming that sequence was pushed"), Fixture.Server.SendToClient(ErrorFrame));

	const bool bDelivered = WaitUntil([&Fixture, &bReported]()
	{
		Fixture.Connection->Poll();
		return bReported;
	});
	if (!TestTrue(TEXT("the error frame is delivered"), bDelivered))
	{
		Fixture.Shut();
		return false;
	}

	TestEqual(TEXT("the error reports the code it carried"), static_cast<int32>(Captured.ErrorCode),
		static_cast<int32>(InvalidAppId));
	TestEqual(TEXT("the error reports the sequence it named"), static_cast<int32>(Captured.Sequence),
		static_cast<int32>(Sequence));
	TestTrue(TEXT("the error is attributed to a send"), Captured.bAttributed);
	TestEqual(TEXT("it names the opcode that used that sequence"), static_cast<int32>(Captured.SendOpcode),
		static_cast<int32>(TextOpcode));
	TestFalse(TEXT("it does not claim the send was a channel message"), Captured.bSendWasChannel);
	TestEqual(TEXT("it names the actor that send carried"), CapturedUuid.Num(), GoldenUuidView().Num());
	TestTrue(TEXT("the actor id matches the one sent"),
		CapturedUuid.Num() == GoldenUuidView().Num()
			&& FMemory::Memcmp(CapturedUuid.GetData(), GoldenUuidView().GetData(), CapturedUuid.Num()) == 0);
	TestTrue(TEXT("the attributed send is reported as recent"),
		Captured.SendAgeMs >= 0 && Captured.SendAgeMs < FCrowdyCppReplication::MaxSendAttributionAgeMs);

	TestEqual(TEXT("the error code renders by name rather than by position"),
		FCrowdyCppReplication::DescribeErrorCode(InvalidAppId), FString(TEXT("InvalidAppId")));

	Fixture.Shut();
	return true;
}

// The other half, and the reason the attribution carries a flag rather than always claiming an answer: a sequence
// no send has used cannot be traced to one, and saying so is the difference between a diagnostic and a guess. A
// forged frame can name any sequence it likes, so this is the ordinary case rather than an unlikely one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerErrorWithNoSendIsUnattributedTest,
	"CrowdySDK.Transport.ServerErrorWithNoSendIsUnattributed",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)
bool FCrowdyServerErrorWithNoSendIsUnattributedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("the fixture connects"), Fixture.Open()))
	{
		return false;
	}

	bool bReported = false;
	FCrowdyCppSendError Captured;
	FCrowdyCppReplicationHandlers Handlers;
	Handlers.OnError = [&bReported, &Captured](const FCrowdyCppSendError& Error)
	{
		Captured = Error;
		bReported = true;
	};
	Fixture.Connection->SetHandlers(Handlers);

	// One real send, so the ledger is not simply empty: the sequence named below is a different one, which is what
	// makes this a lookup miss rather than an untouched facade.
	FString SendError;
	const TArray<uint8> Payload = {0x01};
	if (!TestTrue(TEXT("a send is accepted"),
		Fixture.Connection->SendSpatial(140, 0, 0, 0, GoldenUuidView(), Payload, 8, 0, SendError)))
	{
		Fixture.Shut();
		return false;
	}
	const TArray<uint8> OnTheWire = Fixture.Server.Receive();
	if (!TestTrue(TEXT("the send reached the stand-in server"), OnTheWire.Num() > 0))
	{
		Fixture.Shut();
		return false;
	}

	const uint8 UsedSequence = OnTheWire.Last();
	const uint8 UnusedSequence = static_cast<uint8>(UsedSequence + 97);

	constexpr uint8 InvalidRequest = 15;
	const TArray<uint8> ErrorFrame =
		{static_cast<uint8>(ECrowdyMessageType::GENERIC_ERROR_MESSAGE), UnusedSequence, InvalidRequest};
	TestTrue(TEXT("an error frame naming an unused sequence was pushed"), Fixture.Server.SendToClient(ErrorFrame));

	const bool bDelivered = WaitUntil([&Fixture, &bReported]()
	{
		Fixture.Connection->Poll();
		return bReported;
	});
	if (!TestTrue(TEXT("the error frame is delivered"), bDelivered))
	{
		Fixture.Shut();
		return false;
	}

	TestEqual(TEXT("the error still reports the sequence it named"), static_cast<int32>(Captured.Sequence),
		static_cast<int32>(UnusedSequence));
	TestFalse(TEXT("an unused sequence is not attributed to a send"), Captured.bAttributed);
	TestEqual(TEXT("an unattributed error names no actor"), Captured.SendUuid.Num(), 0);

	Fixture.Shut();
	return true;
}

// The heartbeat is the one message whose whole point is that it carries nothing after the header, and the transport
// refuses a heartbeat that carries a payload rather than trimming it. So this asserts the shape at the seam the
// transport actually reads: the split of the serialized message, and then acceptance by the real send path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorHeartbeatIsHeaderOnlyTest,
	"CrowdySDK.Transport.ActorHeartbeatIsHeaderOnly",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)
bool FCrowdyActorHeartbeatIsHeaderOnlyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FActorHeartbeatRequestMessage Heartbeat;
	Heartbeat.AppID = 7;
	Heartbeat.ChunkX = 1;
	Heartbeat.ChunkY = 2;
	Heartbeat.ChunkZ = 3;
	Heartbeat.UUID = FCrowdyActorId::FromOctets(GoldenUuidView());

	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Split;
	FString SplitError;
	if (!TestTrue(FString::Printf(TEXT("a heartbeat splits into a sendable frame (%s)"), *SplitError),
		CrowdyCppSend::SplitMessage(Heartbeat, Storage, Split, SplitError)))
	{
		return false;
	}

	TestEqual(TEXT("it goes out under the heartbeat opcode"), static_cast<int32>(Split.Opcode),
		static_cast<int32>(ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT));
	TestEqual(TEXT("it carries no payload at all"), Split.Payload.Num(), 0);
	TestFalse(TEXT("it is a spatial frame rather than a channel one"), Split.bIsChannel);
	TestEqual(TEXT("it carries the chunk it was addressed to"), Split.ChunkX, static_cast<int64>(1));
	TestEqual(TEXT("it carries the actor id"), Split.Uuid.Num(), 32);

	TestTrue(TEXT("the heartbeat opcode is one a client may send"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(
			static_cast<uint8>(ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT)));
	TestFalse(TEXT("nothing arrives under the heartbeat opcode"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(
			static_cast<uint8>(ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT)));

	// And that the real send path accepts it, since the payload rule is enforced there rather than at the split.
	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("the fixture connects"), Fixture.Open()))
	{
		return false;
	}

	FString SendError;
	const bool bAccepted = Fixture.Connection->SendSpatial(
		static_cast<uint8>(ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT), 1, 2, 3, GoldenUuidView(),
		TArrayView<const uint8>(), 8, 0, SendError);
	TestTrue(FString::Printf(TEXT("a payload-free heartbeat is accepted (%s)"), *SendError), bAccepted);

	const TArray<uint8> OnTheWire = Fixture.Server.Receive();
	TestTrue(TEXT("the heartbeat reaches the stand-in server"), OnTheWire.Num() > 0);

	// The refusal is what makes the "no payload" rule real rather than advisory.
	const TArray<uint8> Payload = {0x01};
	const bool bWithPayloadAccepted = Fixture.Connection->SendSpatial(
		static_cast<uint8>(ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT), 1, 2, 3, GoldenUuidView(),
		Payload, 8, 0, SendError);
	TestFalse(TEXT("a heartbeat carrying a payload is refused rather than trimmed"), bWithPayloadAccepted);

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDrainCostSaysNothingBeforeItHasMeasuredTest,
	"CrowdySDK.Transport.DrainCostSaysNothingBeforeItHasMeasured",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyDrainCostSaysNothingBeforeItHasMeasuredTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	if (!TestTrue(TEXT("a game instance to own the host was created"), OuterInstance.IsValid()))
	{
		return false;
	}

	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host was created"), Routing.IsValid()))
	{
		return false;
	}

	const FCrowdyDrainCost Cost = Routing->GetInboundDrainCost();
	TestEqual(TEXT("a host that has drained nothing has measured nothing"), Cost.Messages, static_cast<int64>(0));

	// Zero, and the sentence must not offer it as a count to raise the allowance to. This is the reading a
	// developer takes before touching a shipped default, and an idle client is exactly when they take it.
	TestEqual(TEXT("and affords nothing"), Cost.MessagesAffordedByWindow, 0);

	const FString Described = Routing->DescribeInboundDrainCost();
	TestTrue(TEXT("the reading says there is nothing to derive from"),
		Described.Contains(TEXT("no inbound messages have been delivered")));
	TestFalse(TEXT("and offers no affordable count"), Described.Contains(TEXT("affords")));
	return true;
}

// What a video send refuses on its own. Whether the server permits video at all is not knowable from here,
// so nothing local claims to know it: an app the server refuses gets an error frame back, and the argument
// checks below are the only refusals a caller can be given synchronously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSendArgumentsTest,
	"CrowdySDK.Transport.VideoSendRefusesWhatItCannotFragment",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoSendArgumentsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FCrowdyCppReplicationConfig Config;
	Config.AppId = 7;
	Config.Token = GoodToken();

	TSharedPtr<FCrowdyCppReplication> Connection =
		FCrowdyCppReplication::Make(Config, AlwaysRefuse(), NeverRefresh());
	if (!TestNotNull(TEXT("a valid configuration yields a connection"), Connection.Get()))
	{
		return false;
	}

	TArray<uint8> Frame;
	Frame.SetNumZeroed(64);

	int32 Queued = -1;
	FString Error;

	// The control on every refusal below: an otherwise valid frame is refused for the connection and for
	// nothing else, so a send path that refused everything unconditionally would not read as enforcing them.
	TestFalse(TEXT("a valid frame still cannot go down an unopened connection"),
		Connection->SendVideoFrame(1, 2, 3, GoldenUuidView(), Frame, 1, 0, 1, 0, Queued, Error));
	TestTrue(TEXT("and says that is why"), Error.Contains(TEXT("not open")));

	// The opcode's other door, which nothing local gates either: it is refused for the connection alone.
	Error.Reset();
	TArray<uint8> RawFragment = { 1, 0, 0, 1, 0, 1, 0xaa };
	TestFalse(TEXT("a raw fragment sent by opcode is refused for the same reason"),
		Connection->SendSpatial(143, 1, 2, 3, GoldenUuidView(), RawFragment, 1, 0, Error));
	TestTrue(TEXT("and says so"), Error.Contains(TEXT("not open")));

	Error.Reset();
	TestFalse(TEXT("a codec the protocol does not assign is refused"),
		Connection->SendVideoFrame(1, 2, 3, GoldenUuidView(), Frame, 1, 2, 1, 0, Queued, Error));
	TestTrue(TEXT("and names it"), Error.Contains(TEXT("codec 2")));

	Error.Reset();
	TestFalse(TEXT("a frame id past what the header carries is refused"),
		Connection->SendVideoFrame(1, 2, 3, GoldenUuidView(), Frame, 65536, 0, 1, 0, Queued, Error));
	TestTrue(TEXT("and names the range"), Error.Contains(TEXT("65535")));

	Error.Reset();
	TestFalse(TEXT("an actor id of the wrong length is refused"),
		Connection->SendVideoFrame(1, 2, 3, TArrayView<const uint8>(Frame), Frame, 1, 0, 1, 0, Queued, Error));
	TestTrue(TEXT("and names the length"), Error.Contains(TEXT("octets")));

	// The refusal whose failure mode is silent: a frame too large to fragment must send nothing rather than
	// a prefix that never completes at the far end.
	TArray<uint8> TooLarge;
	TooLarge.SetNumZeroed(FCrowdyCppVideoAssembler::MaxFragmentBodyBytes
		* FCrowdyCppVideoAssembler::MaxFragments + 1);
	Error.Reset();
	Queued = -1;
	TestFalse(TEXT("a frame past the fragment limit is refused"),
		Connection->SendVideoFrame(1, 2, 3, GoldenUuidView(), TooLarge, 1, 0, 1, 0, Queued, Error));
	TestTrue(TEXT("and says how many fragments a frame may cross as"), Error.Contains(TEXT("16 fragments")));
	TestEqual(TEXT("and queues nothing at all"), Queued, 0);

	// The same two refusals stated against the split itself, which is offered publicly and is where a caller
	// that fragments a frame for itself meets them. Without these the checks could live only in the send and
	// a direct caller would get fragments carrying a codec no receiver decodes and a truncated frame id.
	TArray<TArray<uint8>> Fragments;
	TestFalse(TEXT("the split refuses a codec the protocol does not assign"),
		FCrowdyCppVideoAssembler::FragmentFrame(Frame, 1, 2, Fragments));
	TestEqual(TEXT("and writes no fragment"), Fragments.Num(), 0);

	TestFalse(TEXT("the split refuses a frame id past what the header carries"),
		FCrowdyCppVideoAssembler::FragmentFrame(Frame, 65536, 0, Fragments));
	TestEqual(TEXT("and writes no fragment for that either"), Fragments.Num(), 0);

	TestFalse(TEXT("and refuses a negative frame id rather than casting it"),
		FCrowdyCppVideoAssembler::FragmentFrame(Frame, -1, 0, Fragments));

	// The control on all three: the same frame with arguments the protocol assigns really does split.
	TestTrue(TEXT("but splits a frame whose codec and id the header carries"),
		FCrowdyCppVideoAssembler::FragmentFrame(Frame, 65535, 1, Fragments));
	TestEqual(TEXT("into the one fragment 64 octets need"), Fragments.Num(), 1);

	Connection->Disconnect();
	Connection.Reset();
	return true;
}

// The other half of the send path: a frame really does reach the wire, as several datagrams under the video
// packet opcode rather than one oversized one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoFrameReachesTheWireTest,
	"CrowdySDK.Transport.VideoFrameSendsEveryFragment",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoFrameReachesTheWireTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// Long enough to need three fragments, so the split is observable rather than a single datagram that
	// would pass whether the frame was fragmented or not.
	TArray<uint8> Frame;
	Frame.SetNumZeroed(FCrowdyCppVideoAssembler::MaxFragmentBodyBytes * 2 + 10);
	constexpr int32 ExpectedFragments = 3;

	int32 Queued = 0;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the frame was accepted (%s)"), *Error),
		Fixture.Connection->SendVideoFrame(1, -2, 3, GoldenUuidView(), Frame, 0x0777,
			static_cast<uint8>(ECrowdyVideoCodec::WebP), 8, 0, Queued, Error)))
	{
		Fixture.Shut();
		return false;
	}

	TestEqual(TEXT("as every fragment it splits into"), Queued, ExpectedFragments);

	// Reassembled from what actually crossed the socket, which is what says the fragments carry the header
	// the receive half reads: a split that wrote a header the assembler rejects would deliver nothing here.
	FCrowdyCppVideoAssembler Assembler;
	FCrowdyCppAssembledVideoFrame Assembled;
	int32 SeenVideoDatagrams = 0;
	bool bCompleted = false;

	for (int32 Attempt = 0; Attempt < ExpectedFragments; ++Attempt)
	{
		const TArray<uint8> Datagram = Fixture.Server.Receive();
		if (Datagram.Num() == 0)
		{
			break;
		}

		if (!TestEqual(TEXT("each datagram leads with the video packet opcode"), static_cast<int32>(Datagram[0]),
			static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_PACKET)))
		{
			continue;
		}
		++SeenVideoDatagrams;

		// Split by the production reader rather than by offsets restated here, so the fragment handed to
		// the assembler is the one a receiving client would see.
		FCrowdyFrame Sent;
		if (!TestTrue(TEXT("and splits into a frame"), FCrowdyFrame::FromMessageBytes(Datagram, Sent)
			&& CrowdyFrameSplit::ReadSpatialEnvelope(Sent)))
		{
			continue;
		}

		bCompleted = Assembler.Ingest(GoldenUuidView(), Sent.Body, 100 + Attempt, Assembled);
	}

	TestEqual(TEXT("every fragment reached the wire"), SeenVideoDatagrams, ExpectedFragments);

	if (TestTrue(TEXT("and the fragments reassemble into the frame that was sent"), bCompleted))
	{
		TestEqual(TEXT("of the size that went in"), Assembled.Bytes.Num(), Frame.Num());
		TestEqual(TEXT("under the frame id it was sent with"), Assembled.FrameId, 0x0777);
		TestEqual(TEXT("and the codec it was sent with"), static_cast<int32>(Assembled.Codec),
			static_cast<int32>(ECrowdyVideoCodec::WebP));
	}

	Fixture.Shut();
	return true;
}

namespace
{
	/**
	 * Sends a burst of identical spatial messages and reads back every complete message the stand-in server
	 * received, bundle members included, in the order they crossed the wire. Which sends share a datagram is
	 * the network thread's decision, so the burst is read back by message count rather than datagram count.
	 */
	bool SendBurstAndCollect(FAutomationTestBase& Test, CrowdyReplicationTestSupport::FConnectedFixture& Fixture,
		const int32 Count, TArray<TArray<uint8>>& OutMessages, int32& OutDatagrams, int32& OutBundles)
	{
		using namespace CrowdyReplicationTestSupport;

		const TArray<uint8> Payload = {0xde, 0xad, 0xbe, 0xef};
		FString Error;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!Fixture.Connection->SendSpatial(140, 1, -2, 3, GoldenUuidView(), Payload, 8,
				static_cast<uint8>(ECrowdyDecayRate::Exponential_Decay), Error))
			{
				Test.AddError(FString::Printf(TEXT("send %d was refused: %s"), Index, *Error));
				return false;
			}
		}

		OutDatagrams = 0;
		OutBundles = 0;
		const double Deadline = FPlatformTime::Seconds() + DefaultWaitSeconds;
		while (OutMessages.Num() < Count && FPlatformTime::Seconds() < Deadline)
		{
			const TArray<uint8> Datagram = Fixture.Server.Receive(0.1);
			if (Datagram.Num() == 0)
			{
				continue;
			}
			++OutDatagrams;
			OutBundles += Datagram[0] == static_cast<uint8>(ECrowdyMessageType::MESSAGE_BUNDLE) ? 1 : 0;
			OutMessages.Append(SplitBundle(Datagram));
		}

		if (!Test.TestEqual(TEXT("every message of the burst reached the stand-in server"), OutMessages.Num(), Count))
		{
			return false;
		}

		// Every member is the complete signed message the encoder produced, in send order: a bundle moved the
		// datagram boundary and nothing else.
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const TArray<uint8> Expected = ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, -2, 3, Payload,
				8, static_cast<uint8>(ECrowdyDecayRate::Exponential_Decay), 123456789, static_cast<uint8>(Index));
			if (!Test.TestEqual(FString::Printf(TEXT("message %d is byte-identical to the lone datagram"), Index),
				CrowdyWireParity::DescribeDifference(OutMessages[Index], Expected), FString(TEXT("identical"))))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationBurstIsBundledTest,
	"CrowdySDK.Transport.SendsFromOneDrainShareADatagram",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationBurstIsBundledTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	// More than one bundle can hold, so at least two datagrams leave however the drains fall. For every message
	// to travel alone the network thread would have to drain between every pair of sends, and it passes about
	// once a millisecond against sends microseconds apart.
	constexpr int32 Burst = 64;
	TArray<TArray<uint8>> Messages;
	int32 Datagrams = 0;
	int32 Bundles = 0;
	if (!SendBurstAndCollect(*this, Fixture, Burst, Messages, Datagrams, Bundles))
	{
		Fixture.Shut();
		return false;
	}

	TestTrue(TEXT("at least one datagram was a MESSAGE_BUNDLE"), Bundles > 0);
	TestTrue(TEXT("and fewer datagrams than messages crossed the wire"), Datagrams < Burst);

	const FCrowdyCppReplicationStats Stats = Fixture.Connection->GetStats();
	TestEqual(TEXT("the library counted every message"), Stats.MessagesSent, static_cast<int64>(Burst));
	TestEqual(TEXT("and as many datagrams as the server saw"), Stats.DatagramsSent, static_cast<int64>(Datagrams));
	TestEqual(TEXT("and as many bundles as the server saw"), Stats.BundlesSent, static_cast<int64>(Bundles));
	TestEqual(TEXT("and lost nothing to a failed flush"), Stats.MessagesDropped, static_cast<int64>(0));

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationBundlingOffTest,
	"CrowdySDK.Transport.BundlingOffSendsOneDatagramPerMessage",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyReplicationBundlingOffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// The escape hatch for a replication server that predates client bundles.
	FConnectedFixture Fixture;
	Fixture.bBundleSends = false;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}

	constexpr int32 Burst = 64;
	TArray<TArray<uint8>> Messages;
	int32 Datagrams = 0;
	int32 Bundles = 0;
	if (!SendBurstAndCollect(*this, Fixture, Burst, Messages, Datagrams, Bundles))
	{
		Fixture.Shut();
		return false;
	}

	TestEqual(TEXT("no datagram was a MESSAGE_BUNDLE"), Bundles, 0);
	TestEqual(TEXT("and every message was its own datagram"), Datagrams, Burst);

	const FCrowdyCppReplicationStats Stats = Fixture.Connection->GetStats();
	TestEqual(TEXT("the library counted one datagram per message"), Stats.DatagramsSent, Stats.MessagesSent);
	TestEqual(TEXT("and no bundles"), Stats.BundlesSent, static_cast<int64>(0));

	Fixture.Shut();
	return true;
}

#endif
