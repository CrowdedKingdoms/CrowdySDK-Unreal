#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyCppReplication.h"
#include "CrowdyNativeExec.h"
#include "CrowdyNativeExecInternal.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Platform/CrowdyCppWebSocketTransport.h"

#if WITH_WEBSOCKETS
#include "IWebSocket.h"
#endif

THIRD_PARTY_INCLUDES_START
#include "crowdy/domains/exec.hpp"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

namespace
{
	constexpr EAutomationTestFlags CrowdyNativeExecTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString CrowdyNativeExecToHex(const std::string& Bytes)
	{
		return BytesToHexLower(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	}

	std::string CrowdyNativeExecFromHex(const TCHAR* Hex)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(FCString::Strlen(Hex) / 2);
		HexToBytes(FString(Hex), Bytes.GetData());
		return std::string(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	}

	// Every request of the test client answers with this host.
	const TCHAR* CrowdyNativeExecConnectAnswer =
		TEXT("{\"data\":{\"execConnect\":{\"gatewayUrl\":\"wss://gateway.example.test\",\"token\":\"connect-1\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}");

	bool CrowdyNativeExecPumpUntil(FCrowdyCppClient& Client, TFunctionRef<bool()> Done, double Seconds = 3.0)
	{
		const double Until = FPlatformTime::Seconds() + Seconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Until)
			{
				return false;
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Client.Poll();
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	uint32 CrowdyNativeExecRid(const TArray<uint8>& Frame)
	{
		return Frame.Num() < 5 ? 0 : Frame[1] | (Frame[2] << 8) | (Frame[3] << 16) | (static_cast<uint32>(Frame[4]) << 24);
	}

	TArray<uint8> CrowdyNativeExecReply(uint32 Rid, uint8 Status, const TArray<uint8>& Payload)
	{
		TArray<uint8> Frame = {0x81, static_cast<uint8>(Rid), static_cast<uint8>(Rid >> 8), static_cast<uint8>(Rid >> 16),
			static_cast<uint8>(Rid >> 24), Status};
		Frame.Append(Payload);
		return Frame;
	}

	TArray<uint8> CrowdyNativeExecPushFrame(const FString& NodeType, const FString& Key, const FString& Topic,
		const TArray<uint8>& Payload)
	{
		const FTCHARToUTF8 Type(*NodeType);
		const FTCHARToUTF8 InstanceKey(*Key);
		const FTCHARToUTF8 Name(*Topic);
		TArray<uint8> Frame = {0x82, static_cast<uint8>(Type.Length())};
		Frame.Append(reinterpret_cast<const uint8*>(Type.Get()), Type.Length());
		Frame.Add(static_cast<uint8>(InstanceKey.Length()));
		Frame.Add(static_cast<uint8>(InstanceKey.Length() >> 8));
		Frame.Append(reinterpret_cast<const uint8*>(InstanceKey.Get()), InstanceKey.Length());
		Frame.Add(static_cast<uint8>(Name.Length()));
		Frame.Append(reinterpret_cast<const uint8*>(Name.Get()), Name.Length());
		Frame.Append(Payload);
		return Frame;
	}

	// A connection opened on a test client, answered by the scripted gateway up to the open socket.
	struct FCrowdyNativeExecRig
	{
		TSharedPtr<FCrowdyCppClient> Client;
		TSharedPtr<FCrowdyNativeExecConnection> Connection;
		int32 Dials = 0;
	};

	FCrowdyNativeExecRig CrowdyNativeExecMakeRig()
	{
		FCrowdyNativeExecRig Rig;
		Rig.Client = FCrowdyCppClient::MakeForTest(CrowdyNativeExecConnectAnswer, 200);
		if (!Rig.Client.IsValid())
		{
			return Rig;
		}
		TSharedRef<int32> Dials = MakeShared<int32>(0);
		FCrowdyNativeExecOptions Options;
		Options.NodeType = TEXT("probe");
		Options.AllowedGatewayDomains.Add(TEXT("example.test"));
		Rig.Connection = Rig.Client->CreateExecConnection(42, Options, [Dials]()
		{
			++*Dials;
			return FString::Printf(TEXT("app-token-%d"), *Dials);
		});
		return Rig;
	}

	bool CrowdyNativeExecOpen(FAutomationTestBase& Test, FCrowdyNativeExecRig& Rig, int32 Connections = 1)
	{
		if (!Test.TestTrue(TEXT("the connection dialled the gateway"), CrowdyNativeExecPumpUntil(*Rig.Client,
			[&Rig, Connections]() { return Rig.Client->NumTestExecConnections() >= Connections; })))
		{
			return false;
		}
		Rig.Client->TestExecOpen();
		return true;
	}

#if WITH_WEBSOCKETS
	// Records which delegates the transport had bound when it connected, which is what the engine latches.
	class FCrowdyNativeFakeWebSocket final : public IWebSocket
	{
	public:
		bool bConnectCalled = false;
		bool bTextBoundAtConnect = false;
		bool bBinaryBoundAtConnect = false;
		bool bRawBoundAtConnect = false;
		int32 CloseCode = 0;

		virtual void Connect() override
		{
			bConnectCalled = true;
			bTextBoundAtConnect = MessageEvent.IsBound();
			bBinaryBoundAtConnect = BinaryEvent.IsBound();
			bRawBoundAtConnect = RawEvent.IsBound();
		}
		virtual void Close(int32 Code, const FString& Reason) override { CloseCode = Code; }
		virtual bool IsConnected() override { return bConnectCalled && CloseCode == 0; }
		virtual void Send(const FString& Data) override {}
		virtual void Send(const void* Data, SIZE_T Size, bool bIsBinary) override {}
		virtual void SetTextMessageMemoryLimit(uint64 TextMessageMemoryLimit) override {}
		virtual FWebSocketConnectedEvent& OnConnected() override { return ConnectedEvent; }
		virtual FWebSocketConnectionErrorEvent& OnConnectionError() override { return ConnectionErrorEvent; }
		virtual FWebSocketClosedEvent& OnClosed() override { return ClosedEvent; }
		virtual FWebSocketMessageEvent& OnMessage() override { return MessageEvent; }
		virtual FWebSocketBinaryMessageEvent& OnBinaryMessage() override { return BinaryEvent; }
		virtual FWebSocketRawMessageEvent& OnRawMessage() override { return RawEvent; }
		virtual FWebSocketMessageSentEvent& OnMessageSent() override { return MessageSentEvent; }

		FWebSocketConnectedEvent ConnectedEvent;
		FWebSocketConnectionErrorEvent ConnectionErrorEvent;
		FWebSocketClosedEvent ClosedEvent;
		FWebSocketMessageEvent MessageEvent;
		FWebSocketBinaryMessageEvent BinaryEvent;
		FWebSocketRawMessageEvent RawEvent;
		FWebSocketMessageSentEvent MessageSentEvent;
	};

	// One connection of a real transport over a fake engine socket, started and connected.
	struct FCrowdyNativeFakeSocketRig
	{
		TSharedPtr<FCrowdyNativeFakeWebSocket> Socket;
		std::shared_ptr<crowdy::graphql::IWebSocketConnection> Connection;
		TSharedRef<std::vector<crowdy::graphql::WebSocketEvent>> Events = MakeShared<std::vector<crowdy::graphql::WebSocketEvent>>();

		~FCrowdyNativeFakeSocketRig()
		{
			Connection.reset();
			CrowdyCppTransport::FlushPendingWebSocketReleases();
			CrowdyCppTransport::SetWebSocketFactoryForTest(nullptr);
		}
	};

	void CrowdyNativeStartFakeSocket(FCrowdyNativeFakeSocketRig& Rig,
		const std::shared_ptr<crowdy::graphql::IWebSocketTransport>& Transport, const std::string& Subprotocol,
		std::size_t MaxFrameBytes)
	{
		TSharedRef<FCrowdyNativeFakeWebSocket> Fake = MakeShared<FCrowdyNativeFakeWebSocket>();
		Rig.Socket = Fake;
		CrowdyCppTransport::SetWebSocketFactoryForTest([Fake](const FString&, const FString&) -> TSharedRef<IWebSocket>
		{
			return Fake;
		});

		crowdy::graphql::WebSocketConnectRequest Request;
		Request.url = "wss://gateway.example.test/v1/connect";
		Request.subprotocol = Subprotocol;
		Request.maxFrameBytes = MaxFrameBytes;
		Request.connectTimeoutMs = 0;
		Rig.Connection = Transport->createConnection(Request);
		TSharedRef<std::vector<crowdy::graphql::WebSocketEvent>> Events = Rig.Events;
		Rig.Connection->start([Events](crowdy::graphql::WebSocketEvent Event) { Events->push_back(std::move(Event)); });
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
#endif
}

// The expected bytes are rows of the ck-exec protocol's shared golden fixture, so a vendored codec that drifts
// from the platform's wire format fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppExecWireMatchesGoldenFramesTest,
	"CrowdySDK.CrowdyCppBridge.ExecWireMatchesGoldenFrames", CrowdyNativeExecTestFlags)
bool FCrowdyCppExecWireMatchesGoldenFramesTest::RunTest(const FString& Parameters)
{
	namespace exec_wire = crowdy::domains::exec_wire;

	exec_wire::ClientFrame Call;
	Call.kind = exec_wire::ClientFrame::Kind::Call;
	Call.rid = 1;
	Call.nodeType = "combat";
	Call.method = "hit";
	Call.payload = CrowdyNativeExecFromHex(TEXT("82a56d61746368a26d31a6776561706f6e01"));
	const auto Encoded = exec_wire::encode(Call);
	if (!TestTrue(TEXT("a call frame encodes"), Encoded.ok()))
	{
		return false;
	}
	TestEqual(TEXT("the call frame matches the golden bytes"), CrowdyNativeExecToHex(Encoded.value()),
		FString(TEXT("010100000006636f6d62617400000368697482a56d61746368a26d31a6776561706f6e01")));

	const auto Decoded = exec_wire::decode(CrowdyNativeExecFromHex(TEXT("81010000000083a268708ce8d4a5a46869747301a262791f")));
	if (!TestTrue(TEXT("a reply frame decodes"), Decoded.ok()))
	{
		return false;
	}
	TestTrue(TEXT("it is a reply"), Decoded.value().kind == exec_wire::ServerFrame::Kind::Reply);
	TestEqual(TEXT("its request id"), static_cast<int64>(Decoded.value().rid), int64(1));
	TestEqual(TEXT("its status is Ok"), static_cast<int32>(Decoded.value().status), 0);
	TestEqual(TEXT("its payload"), CrowdyNativeExecToHex(Decoded.value().payload),
		FString(TEXT("83a268708ce8d4a5a46869747301a262791f")));

	TestTrue(TEXT("Busy is retryable"), crowdy::domains::execStatusRetryable(crowdy::domains::ExecStatus::Busy));
	TestFalse(TEXT("Denied is not retryable"), crowdy::domains::execStatusRetryable(crowdy::domains::ExecStatus::Denied));
	return true;
}

// A call dials the gateway under the app token, goes out as one binary frame, and its reply comes back through Poll.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecCallReachesItsReplyTest,
	"CrowdySDK.CrowdyCppBridge.ExecCallReachesItsReply", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecCallReachesItsReplyTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	int32 Replies = 0;
	FCrowdyNativeExecReply Reply;
	Rig.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&Replies, &Reply](const FCrowdyNativeExecReply& In)
	{
		++Replies;
		Reply = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}

	FString Url;
	FString Authorization;
	TestTrue(TEXT("execConnect was issued"), Rig.Client->GetLastTestRequest(Url, Authorization));
	TestEqual(TEXT("execConnect carried the app token"), Authorization, FString(TEXT("Bearer app-token-1")));
	FString Body;
	Rig.Client->GetLastTestRequestBody(Body);
	TestTrue(TEXT("the request was execConnect"), Body.Contains(TEXT("execConnect")));

	FString SocketUrl;
	FString Subprotocol;
	TestTrue(TEXT("a gateway socket was opened"), Rig.Client->GetTestExecConnectRequest(SocketUrl, Subprotocol));
	TestEqual(TEXT("the socket URL carries the connect token"), SocketUrl,
		FString(TEXT("wss://gateway.example.test/v1/connect?token=connect-1")));
	TestTrue(TEXT("no subprotocol is requested"), Subprotocol.IsEmpty());

	const TArray<TArray<uint8>> Sent = Rig.Client->TakeTestExecSentFrames();
	if (!TestEqual(TEXT("one frame was sent"), Sent.Num(), 1) || !TestEqual(TEXT("it is a call"), static_cast<int32>(Sent[0][0]), 0x01))
	{
		return false;
	}
	Rig.Client->TestExecReceiveBinary(CrowdyNativeExecReply(CrowdyNativeExecRid(Sent[0]), 0, {0xa2, 0x6f, 0x6b}));

	TestTrue(TEXT("the reply arrived"), CrowdyNativeExecPumpUntil(*Rig.Client, [&Replies]() { return Replies > 0; }));
	TestEqual(TEXT("exactly once"), Replies, 1);
	TestTrue(TEXT("it is Ok"), Reply.IsOk());
	TestTrue(TEXT("with the payload"), Reply.Payload == TArray<uint8>({0xa2, 0x6f, 0x6b}));
	return true;
}

// A subscription goes out as one binary frame, and a push on its topic reaches the handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecPushReachesItsSubscriberTest,
	"CrowdySDK.CrowdyCppBridge.ExecPushReachesItsSubscriber", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecPushReachesItsSubscriberTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	bool bSubscribed = false;
	TArray<FCrowdyNativeExecPush> Pushes;
	Rig.Connection->Subscribe(TEXT("boss"), TEXT("m1"), TEXT("state"),
		[&Pushes](const FCrowdyNativeExecPush& Push) { Pushes.Add(Push); },
		[&bSubscribed](const FCrowdyNativeExecReply& Reply) { bSubscribed = Reply.IsOk(); });
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}

	const TArray<TArray<uint8>> Sent = Rig.Client->TakeTestExecSentFrames();
	if (!TestEqual(TEXT("one frame was sent"), Sent.Num(), 1) || !TestEqual(TEXT("it is a subscribe"), static_cast<int32>(Sent[0][0]), 0x02))
	{
		return false;
	}
	Rig.Client->TestExecReceiveBinary(CrowdyNativeExecReply(CrowdyNativeExecRid(Sent[0]), 0, {}));
	Rig.Client->TestExecReceiveBinary(CrowdyNativeExecPushFrame(TEXT("boss"), TEXT("m1"), TEXT("state"), {0x81, 0xa1, 0x48, 0x05}));

	TestTrue(TEXT("the push arrived"), CrowdyNativeExecPumpUntil(*Rig.Client, [&Pushes]() { return Pushes.Num() > 0; }));
	TestTrue(TEXT("the subscribe was acknowledged"), bSubscribed);
	if (!TestEqual(TEXT("one push"), Pushes.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("its topic"), Pushes[0].Topic, FString(TEXT("state")));
	TestEqual(TEXT("its key"), Pushes[0].Key, FString(TEXT("m1")));
	TestTrue(TEXT("its payload"), Pushes[0].Payload == TArray<uint8>({0x81, 0xa1, 0x48, 0x05}));
	return true;
}

// A reconnect dials again, and that dial asks for the app token afresh rather than reusing whatever bearer was left.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecEveryDialCarriesTheAppTokenTest,
	"CrowdySDK.CrowdyCppBridge.ExecEveryDialCarriesTheAppToken", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecEveryDialCarriesTheAppTokenTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	bool bConnected = false;
	Rig.Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}
	TestTrue(TEXT("it connected"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected; }));

	Rig.Client->TestExecCloseFromServer(1006, false);
	if (!CrowdyNativeExecOpen(*this, Rig, 2))
	{
		return false;
	}

	FString Url;
	FString Authorization;
	Rig.Client->GetLastTestRequest(Url, Authorization);
	TestEqual(TEXT("the reconnect's execConnect carried a freshly resolved app token"), Authorization,
		FString(TEXT("Bearer app-token-2")));
	return true;
}

// Closing the client completes a call that never got an answer, once, rather than leaving it to a pump that closed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecClientCloseCompletesPendingTest,
	"CrowdySDK.CrowdyCppBridge.ExecClientCloseCompletesPending", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecClientCloseCompletesPendingTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	int32 Replies = 0;
	FCrowdyNativeExecReply Reply;
	Rig.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&Replies, &Reply](const FCrowdyNativeExecReply& In)
	{
		++Replies;
		Reply = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}

	Rig.Client->Close();
	Rig.Client->Poll();
	TestEqual(TEXT("the pending call completed exactly once"), Replies, 1);
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::Unavailable));
	TestEqual(TEXT("with the canceled message"), Reply.Message, FCrowdyNativeExecConnection::CanceledMessage());
	TestFalse(TEXT("the connection is closed with its client"), Rig.Connection->IsConnected());
	return true;
}

// A call too large for one frame is refused before anything is dialled or sent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecOversizedCallIsRefusedTest,
	"CrowdySDK.CrowdyCppBridge.ExecOversizedCallIsRefused", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecOversizedCallIsRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	TArray<uint8> Payload;
	Payload.SetNumZeroed(static_cast<int32>(crowdy::graphql::kDefaultWebSocketFrameLimit));
	int32 Replies = 0;
	ECrowdyNativeExecStatus Status = ECrowdyNativeExecStatus::Ok;
	Rig.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), MoveTemp(Payload), [&Replies, &Status](const FCrowdyNativeExecReply& In)
	{
		++Replies;
		Status = In.Status;
	});

	TestEqual(TEXT("refused at once"), Replies, 1);
	TestEqual(TEXT("as BadRequest"), static_cast<int32>(Status), static_cast<int32>(ECrowdyNativeExecStatus::BadRequest));
	CrowdyNativeExecPumpUntil(*Rig.Client, []() { return false; }, 0.1);
	TestEqual(TEXT("nothing was dialled"), Rig.Client->NumTestExecConnections(), 0);
	return true;
}

// A paused app is refused when a host is asked for, before any socket; the player is told the Game API's reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecRefusedDialSaysWhyTest,
	"CrowdySDK.CrowdyCppBridge.ExecRefusedDialSaysWhy", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecRefusedDialSaysWhyTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(
		TEXT("{\"errors\":[{\"message\":\"app 42 is paused: its organization has no funds (insufficient_funds)\",\"extensions\":{\"code\":\"FORBIDDEN\"}}],\"data\":null}"), 200);
	if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
	{
		return false;
	}
	FCrowdyNativeExecOptions Options;
	Options.NodeType = TEXT("probe");
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(TEXT("app-token")); });
	if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
	{
		return false;
	}
	TOptional<bool> bConnected;
	FString Error;
	Connection->Connect([&bConnected, &Error](bool bOk, const FString& Why)
	{
		bConnected = bOk;
		Error = Why;
	});
	if (!TestTrue(TEXT("connecting completes"), CrowdyNativeExecPumpUntil(*Client, [&bConnected]() { return bConnected.IsSet(); })))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), bConnected.GetValue());
	TestEqualSensitive(TEXT("with the Game API's message and code"), Error,
		FString(TEXT("app 42 is paused: its organization has no funds (insufficient_funds) (FORBIDDEN)")));
	TestEqual(TEXT("and no socket was opened"), Client->NumTestExecConnections(), 0);
	TestFalse(TEXT("an ordinary refusal is not a refusal for good"), Connection->IsRefusedForGood());
	return true;
}

// A gateway outside the Game API's estate is refused unless its domain is allowed, and says so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecGatewayOutsideEstateIsRefusedTest,
	"CrowdySDK.CrowdyCppBridge.ExecGatewayOutsideEstateIsRefused", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecGatewayOutsideEstateIsRefusedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(CrowdyNativeExecConnectAnswer, 200);
	if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
	{
		return false;
	}
	FCrowdyNativeExecOptions Options;
	Options.NodeType = TEXT("probe");
	// A suffix that is not a whole label of the gateway's host does not allow it.
	Options.AllowedGatewayDomains.Add(TEXT("ample.test"));
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(TEXT("app-token")); });
	if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("The ck-exec gateway is outside the Game API's domains"), EAutomationExpectedErrorFlags::Contains, 0);
	TOptional<bool> bConnected;
	FString Error;
	Connection->Connect([&bConnected, &Error](bool bOk, const FString& Why)
	{
		bConnected = bOk;
		Error = Why;
	});
	if (!TestTrue(TEXT("connecting completes"), CrowdyNativeExecPumpUntil(*Client, [&bConnected]() { return bConnected.IsSet(); })))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), bConnected.GetValue());
	TestTrue(TEXT("saying the gateway is outside the Game API's domains"),
		Error.StartsWith(TEXT("the Game API named a gateway outside its own domains (gateway.example.test is outside the estate of "), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and where to allow it"), Error.EndsWith(TEXT("add its domain to Allowed Exec Gateway Domains in Project Settings"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("it is a refusal for good"), Connection->IsRefusedForGood());
	TestEqual(TEXT("and no socket was opened"), Client->NumTestExecConnections(), 0);
	TestEqualSensitive(TEXT("GetRefusal is the text the connect reported"), Connection->GetRefusal(), Error);
	return true;
}

// The allow list normalises its entries and waives only the estate refusal, never a credentialed URL.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecAllowListNormalisesAndWaivesOnlyTheEstateTest,
	"CrowdySDK.CrowdyCppBridge.ExecAllowListNormalisesAndWaivesOnlyTheEstate", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecAllowListNormalisesAndWaivesOnlyTheEstateTest::RunTest(const FString& Parameters)
{
	const auto Dial = [this](const TCHAR* GatewayUrl, const TCHAR* Allowed, bool& bRefusedForGood)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(FString::Printf(
			TEXT("{\"data\":{\"execConnect\":{\"gatewayUrl\":\"%s\",\"token\":\"connect-1\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}"), GatewayUrl), 200);
		bRefusedForGood = true;
		if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
		{
			return;
		}
		FCrowdyNativeExecOptions Options;
		Options.NodeType = TEXT("probe");
		Options.AllowedGatewayDomains.Add(Allowed);
		const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(TEXT("app-token")); });
		if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
		{
			return;
		}
		Connection->Connect([](bool, const FString&) {});
		CrowdyNativeExecPumpUntil(*Client, [&Client, &Connection]() { return Client->NumTestExecConnections() >= 1 || Connection->IsRefusedForGood(); });
		bRefusedForGood = Connection->IsRefusedForGood();
	};

	AddExpectedErrorPlain(TEXT("The ck-exec gateway is outside the Game API's domains"), EAutomationExpectedErrorFlags::Contains, 0);
	bool bRefused = false;
	Dial(TEXT("wss://gateway.example.test"), TEXT(".Example.Test"), bRefused);
	TestFalse(TEXT("a leading dot and capitals allow the gateway"), bRefused);
	Dial(TEXT("wss://gateway.example.test"), TEXT("https://example.test/"), bRefused);
	TestFalse(TEXT("a scheme and a path allow the gateway"), bRefused);
	Dial(TEXT("wss://gateway.example.test"), TEXT("*.example.test:443"), bRefused);
	TestFalse(TEXT("a wildcard and a port allow the gateway"), bRefused);
	Dial(TEXT("wss://u:p@allowed.example.test"), TEXT("example.test"), bRefused);
	TestTrue(TEXT("a credentialed URL stays refused even on an allowed domain"), bRefused);
	return true;
}

// The same gateway connects once its domain is allowed, and a later dial that is refused closes the connection for good.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecAllowedGatewayConnectsAndRedialRefusalClosesTest,
	"CrowdySDK.CrowdyCppBridge.ExecAllowedGatewayConnectsAndRedialRefusalCloses", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecAllowedGatewayConnectsAndRedialRefusalClosesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(CrowdyNativeExecConnectAnswer, 200);
	if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
	{
		return false;
	}
	const FString Elsewhere = TEXT("{\"data\":{\"execConnect\":{\"gatewayUrl\":\"wss://gateway.elsewhere.test\",\"token\":\"connect-2\",\"host\":\"host-b\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}");
	Client->SetTestResponseScript({TPair<int32, FString>(200, CrowdyNativeExecConnectAnswer), TPair<int32, FString>(200, Elsewhere)});
	FCrowdyNativeExecOptions Options;
	Options.NodeType = TEXT("probe");
	Options.AllowedGatewayDomains.Add(TEXT("Example.Test"));
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(TEXT("app-token")); });
	if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
	{
		return false;
	}
	bool bConnected = false;
	Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
	if (!TestTrue(TEXT("the allowed gateway was dialled"), CrowdyNativeExecPumpUntil(*Client, [&Client]() { return Client->NumTestExecConnections() >= 1; })))
	{
		return false;
	}
	Client->TestExecOpen();
	TestTrue(TEXT("it connected"), CrowdyNativeExecPumpUntil(*Client, [&bConnected]() { return bConnected; }));
	TestFalse(TEXT("an allowed gateway is not refused"), Connection->IsRefusedForGood());

	AddExpectedErrorPlain(TEXT("The ck-exec gateway is outside the Game API's domains"), EAutomationExpectedErrorFlags::Contains, 0);
	Client->TestExecCloseFromServer(1006, false);
	if (!TestTrue(TEXT("the redial was refused"), CrowdyNativeExecPumpUntil(*Client, [&Connection]() { return Connection->IsRefusedForGood(); })))
	{
		return false;
	}
	CrowdyNativeExecPumpUntil(*Client, []() { return false; }, 0.6);
	TestTrue(TEXT("it stays a refusal for good past CrowdyCPP's own redial"), Connection->IsRefusedForGood());
	TestTrue(TEXT("and says why"), Connection->GetRefusal().StartsWith(TEXT("the Game API named a gateway outside its own domains (gateway.elsewhere.test is outside the estate of "), ESearchCase::CaseSensitive));

	FCrowdyNativeExecReply Reply;
	bool bReplied = false;
	Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&Reply, &bReplied](const FCrowdyNativeExecReply& In)
	{
		Reply = In;
		bReplied = true;
	});
	TestTrue(TEXT("a call after the refusal is answered"), CrowdyNativeExecPumpUntil(*Client, [&bReplied]() { return bReplied; }));
	TestEqualSensitive(TEXT("by a closed connection rather than another dial"), Reply.Message, FString(TEXT("the connection is closed")));
	return true;
}

// The paused-app code is told apart from the expired-token code next to it, and has a name for logs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeAppPausedErrorCodeTest,
	"CrowdySDK.CrowdyCppBridge.AppPausedErrorCode", CrowdyNativeExecTestFlags)
bool FCrowdyNativeAppPausedErrorCodeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("33 is the paused-app code"), FCrowdyCppReplication::IsAppPausedErrorCode(33));
	TestFalse(TEXT("32 is the expired-token code"), FCrowdyCppReplication::IsAppPausedErrorCode(32));
	TestEqualSensitive(TEXT("33 is named"), FCrowdyCppReplication::DescribeErrorCode(33), FString(TEXT("AppPaused")));
	return true;
}

// A vendor reply is a client timeout only when it is the client's own "no reply in" deadline, not one the server reports.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecNoReplyIsTheClientsTimeoutTest,
	"CrowdySDK.CrowdyCppBridge.ExecNoReplyIsTheClientsTimeout", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecNoReplyIsTheClientsTimeoutTest::RunTest(const FString& Parameters)
{
	using crowdy::domains::ExecReply;
	using crowdy::domains::ExecStatus;

	const FCrowdyNativeExecReply Client = CrowdyNativeExec::ReplyFrom(ExecReply{ExecStatus::DeadlineExceeded, "no reply in 20000 ms"});
	TestEqual(TEXT("the client's timeout is a deadline"), static_cast<int32>(Client.Status), static_cast<int32>(ECrowdyNativeExecStatus::DeadlineExceeded));
	TestTrue(TEXT("and no reply"), Client.bNoReply);

	const FCrowdyNativeExecReply Server = CrowdyNativeExec::ReplyFrom(ExecReply{ExecStatus::DeadlineExceeded, "handler exceeded its deadline"});
	TestFalse(TEXT("the server's deadline is not"), Server.bNoReply);

	const FCrowdyNativeExecReply Other = CrowdyNativeExec::ReplyFrom(ExecReply{ExecStatus::Unavailable, "no reply in 20000 ms"});
	TestFalse(TEXT("nor is another status with that text"), Other.bNoReply);
	return true;
}

// The paused-app gate of an app-token answer reaches the result, and is empty when the answer has none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeAppTokenCarriesRuntimeGateTest,
	"CrowdySDK.CrowdyCppBridge.AppTokenCarriesRuntimeGate", CrowdyNativeExecTestFlags)
bool FCrowdyNativeAppTokenCarriesRuntimeGateTest::RunTest(const FString& Parameters)
{
	const auto Mint = [](const TCHAR* Gate)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(FString::Printf(
			TEXT("{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"42\",\"expiresAt\":\"2026-07-27T12:00:00Z\"%s}}}"), Gate), 200);
		FCrowdyCppAppTokenResult Result;
		if (!Client.IsValid())
		{
			return Result;
		}
		Client->SetManagementToken(TEXT("identity-bearer"));
		Client->MintAppToken(42, [&Result](FCrowdyCppAppTokenResult In) { Result = MoveTemp(In); });
		Client->Poll();
		return Result;
	};

	const FCrowdyCppAppTokenResult Paused = Mint(TEXT(",\"runtimeGate\":{\"status\":\"DENIED\",\"reason\":\"out of funds\"}"));
	TestTrue(TEXT("the mint succeeded"), Paused.bOk);
	TestEqualSensitive(TEXT("the gate's status"), Paused.RuntimeGateStatus, FString(TEXT("DENIED")));
	TestEqualSensitive(TEXT("the gate's reason"), Paused.RuntimeGateReason, FString(TEXT("out of funds")));

	const FCrowdyCppAppTokenResult NoReason = Mint(TEXT(",\"runtimeGate\":{\"status\":\"ACTIVE\",\"reason\":null}"));
	TestEqualSensitive(TEXT("an active gate"), NoReason.RuntimeGateStatus, FString(TEXT("ACTIVE")));
	TestTrue(TEXT("with no reason"), NoReason.RuntimeGateReason.IsEmpty());

	const FCrowdyCppAppTokenResult NoGate = Mint(TEXT(""));
	TestTrue(TEXT("the mint without a gate succeeded"), NoGate.bOk);
	TestTrue(TEXT("and has no status"), NoGate.RuntimeGateStatus.IsEmpty());
	return true;
}

// A failure the bridge decides itself says why, rather than leaving CrowdyCPP's "no host for this app".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecDialWithoutTokenSaysWhyTest,
	"CrowdySDK.CrowdyCppBridge.ExecDialWithoutTokenSaysWhy", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecDialWithoutTokenSaysWhyTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(CrowdyNativeExecConnectAnswer, 200);
	if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
	{
		return false;
	}
	FCrowdyNativeExecOptions Options;
	Options.NodeType = TEXT("probe");
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(); });
	if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("A ck-exec connection has no token to dial with."), EAutomationExpectedErrorFlags::Contains, 0);
	TOptional<bool> bConnected;
	FString Error;
	Connection->Connect([&bConnected, &Error](bool bOk, const FString& Why)
	{
		bConnected = bOk;
		Error = Why;
	});
	if (!TestTrue(TEXT("connecting completes"), CrowdyNativeExecPumpUntil(*Client, [&bConnected]() { return bConnected.IsSet(); })))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), bConnected.GetValue());
	TestEqualSensitive(TEXT("saying the player has no app token"), Error, FString(TEXT("the player has no app token; sign in first")));
	return true;
}

// A developer connection asks execConnectAsDeveloper under the session token and opens the host it names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecDeveloperDialTest,
	"CrowdySDK.CrowdyCppBridge.ExecDeveloperDialsAsDeveloper", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecDeveloperDialTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(
		TEXT("{\"data\":{\"execConnectAsDeveloper\":{\"gatewayUrl\":\"wss://gateway.example.test\",\"token\":\"dev-connect-1\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}"), 200);
	if (!TestTrue(TEXT("a test client was made"), Client.IsValid()))
	{
		return false;
	}
	FCrowdyNativeExecOptions Options;
	Options.NodeType = TEXT("probe");
	Options.bAsDeveloper = true;
	Options.AllowedGatewayDomains.Add(TEXT("example.test"));
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client->CreateExecConnection(42, Options, []() { return FString(TEXT("session-token")); });
	if (!TestTrue(TEXT("a connection was created"), Connection.IsValid()))
	{
		return false;
	}
	Connection->Connect([](bool, const FString&) {});
	if (!TestTrue(TEXT("the developer's answer named a gateway it dialled"), CrowdyNativeExecPumpUntil(*Client, [&Client]() { return Client->NumTestExecConnections() >= 1; })))
	{
		return false;
	}
	FString Url;
	FString Authorization;
	Client->GetLastTestRequest(Url, Authorization);
	FString Body;
	Client->GetLastTestRequestBody(Body);
	TestEqual(TEXT("under the developer's session token"), Authorization, FString(TEXT("Bearer session-token")));
	TestTrue(FString::Printf(TEXT("named as the operation execConnectAsDeveloper (%s)"), *Body.Left(200)),
		Body.Replace(TEXT(" "), TEXT("")).Contains(TEXT("\"operationName\":\"ExecConnectAsDeveloper\""), ESearchCase::CaseSensitive));
	return true;
}

// A gateway that refuses the connect token closes before the socket opens; the reason it gives is the failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecRefusedGatewaySaysWhyTest,
	"CrowdySDK.CrowdyCppBridge.ExecRefusedGatewaySaysWhy", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecRefusedGatewaySaysWhyTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}
	TOptional<bool> bConnected;
	FString Error;
	Rig.Connection->Connect([&bConnected, &Error](bool bOk, const FString& Why)
	{
		bConnected = bOk;
		Error = Why;
	});
	if (!TestTrue(TEXT("the connection dialled the gateway"), CrowdyNativeExecPumpUntil(*Rig.Client, [&Rig]() { return Rig.Client->NumTestExecConnections() >= 1; })))
	{
		return false;
	}
	Rig.Client->TestExecCloseFromServer(4401, true);
	if (!TestTrue(TEXT("connecting completes"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected.IsSet(); })))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), bConnected.GetValue());
	TestEqualSensitive(TEXT("with the gateway's reason, not the failure's name"), Error, FString(TEXT("the gateway closed the connection")));
	return true;
}

#if WITH_WEBSOCKETS
// The ck-exec transport binds only the binary delegate before connecting, and joins the engine's fragments.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeTransportReassemblesBinaryTest,
	"CrowdySDK.CrowdyCppBridge.NativeTransportReassemblesBinary", CrowdyNativeExecTestFlags)
bool FCrowdyNativeTransportReassemblesBinaryTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeFakeSocketRig Rig;
	CrowdyNativeStartFakeSocket(Rig, CrowdyCppTransport::MakeCrowdyNativeWebSocketTransport(), std::string(), 16);
	if (!TestTrue(TEXT("the engine socket was connected"), Rig.Socket.IsValid() && Rig.Socket->bConnectCalled))
	{
		return false;
	}
	TestTrue(TEXT("binary was bound before Connect"), Rig.Socket->bBinaryBoundAtConnect);
	TestFalse(TEXT("text was not"), Rig.Socket->bTextBoundAtConnect);
	TestFalse(TEXT("raw was not"), Rig.Socket->bRawBoundAtConnect);

	Rig.Socket->BinaryEvent.Broadcast("ab", 2, false);
	Rig.Socket->BinaryEvent.Broadcast("cd", 2, true);
	if (!TestEqual(TEXT("one event"), static_cast<int32>(Rig.Events->size()), 1))
	{
		return false;
	}
	const crowdy::graphql::WebSocketEvent& Event = (*Rig.Events)[0];
	TestTrue(TEXT("a binary frame"), Event.kind == crowdy::graphql::WebSocketEventKind::Frame
		&& Event.frame.kind == crowdy::graphql::WebSocketFrameKind::Binary);
	TestEqual(TEXT("reassembled"), FString(UTF8_TO_TCHAR(Event.frame.payload.c_str())), FString(TEXT("abcd")));
	return true;
}

// A binary message past the frame limit closes the socket and reports it, and nothing after it is delivered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeTransportClosesOnOversizedMessageTest,
	"CrowdySDK.CrowdyCppBridge.NativeTransportClosesOnOversizedMessage", CrowdyNativeExecTestFlags)
bool FCrowdyNativeTransportClosesOnOversizedMessageTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeFakeSocketRig Rig;
	CrowdyNativeStartFakeSocket(Rig, CrowdyCppTransport::MakeCrowdyNativeWebSocketTransport(), std::string(), 4);
	if (!TestTrue(TEXT("the engine socket was connected"), Rig.Socket.IsValid() && Rig.Socket->bConnectCalled))
	{
		return false;
	}

	Rig.Socket->BinaryEvent.Broadcast("abc", 3, false);
	Rig.Socket->BinaryEvent.Broadcast("de", 2, true);
	Rig.Socket->BinaryEvent.Broadcast("f", 1, true);
	if (!TestEqual(TEXT("one event"), static_cast<int32>(Rig.Events->size()), 1))
	{
		return false;
	}
	const crowdy::graphql::WebSocketEvent& Event = (*Rig.Events)[0];
	TestTrue(TEXT("a frame-too-large error"), Event.kind == crowdy::graphql::WebSocketEventKind::Error
		&& Event.error.kind == crowdy::graphql::WebSocketErrorKind::FrameTooLarge);
	TestEqual(TEXT("the socket was closed as too big"), Rig.Socket->CloseCode, 1009);
	return true;
}

// The GraphQL transport never asks the engine for binary frames, so a hostile server cannot make it buffer any.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeGraphQLTransportRefusesBinaryTest,
	"CrowdySDK.CrowdyCppBridge.GraphQLTransportRefusesBinary", CrowdyNativeExecTestFlags)
bool FCrowdyNativeGraphQLTransportRefusesBinaryTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeFakeSocketRig Rig;
	CrowdyNativeStartFakeSocket(Rig, CrowdyCppTransport::MakeFWebSocketTransport(), "graphql-transport-ws", 1024);
	if (!TestTrue(TEXT("the engine socket was connected"), Rig.Socket.IsValid() && Rig.Socket->bConnectCalled))
	{
		return false;
	}
	TestTrue(TEXT("text was bound before Connect"), Rig.Socket->bTextBoundAtConnect);
	TestFalse(TEXT("binary was not"), Rig.Socket->bBinaryBoundAtConnect);
	TestFalse(TEXT("raw was not"), Rig.Socket->bRawBoundAtConnect);
	return true;
}
#endif

#endif  // WITH_DEV_AUTOMATION_TESTS
