#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyCppReplication.h"
#include "CrowdyNativeExec.h"
#include "CrowdyNativeExecInternal.h"
#include "CrowdyNativeKit.h"
#include "CrowdyNativeKitInternal.h"
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

// Close code 4403 is final: the listener hears the reason once, calls fail, and nothing is dialled again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecAccessEndedCloseIsFinalTest,
	"CrowdySDK.CrowdyCppBridge.ExecAccessEndedCloseIsFinal", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecAccessEndedCloseIsFinalTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}

	int32 Heard = 0;
	FString HeardReason;
	Rig.Connection->OnAccessEnded([&Heard, &HeardReason](const FString& Reason)
	{
		++Heard;
		HeardReason = Reason;
	});

	int32 InFlightReplies = 0;
	FCrowdyNativeExecReply InFlight;
	Rig.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&InFlightReplies, &InFlight](const FCrowdyNativeExecReply& In)
	{
		++InFlightReplies;
		InFlight = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}
	TestFalse(TEXT("access has not ended yet"), Rig.Connection->IsAccessEnded());

	const FString Reason = TEXT("access ended: suspended");
	Rig.Client->TestExecCloseFromServer(4403, true, Reason);
	if (!TestTrue(TEXT("the listener ran"), CrowdyNativeExecPumpUntil(*Rig.Client, [&Heard]() { return Heard > 0; })))
	{
		return false;
	}
	CrowdyNativeExecPumpUntil(*Rig.Client, []() { return false; }, 0.6);
	TestEqual(TEXT("exactly once"), Heard, 1);
	TestEqualSensitive(TEXT("with the server's reason"), HeardReason, Reason);
	TestTrue(TEXT("the connection reports the access ended"), Rig.Connection->IsAccessEnded());
	TestEqualSensitive(TEXT("and the reason"), Rig.Connection->GetAccessEndedReason(), Reason);
	TestEqual(TEXT("nothing was dialled again"), Rig.Client->NumTestExecConnections(), 1);

	TestEqual(TEXT("the call in flight completed once"), InFlightReplies, 1);
	TestEqual(TEXT("as Denied"), static_cast<int32>(InFlight.Status), static_cast<int32>(ECrowdyNativeExecStatus::Denied));
	TestEqualSensitive(TEXT("with the reason"), InFlight.Message, Reason);

	FCrowdyNativeExecReply Later;
	bool bLater = false;
	Rig.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&Later, &bLater](const FCrowdyNativeExecReply& In)
	{
		Later = In;
		bLater = true;
	});
	TestTrue(TEXT("a later call is answered"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bLater]() { return bLater; }));
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Later.Status), static_cast<int32>(ECrowdyNativeExecStatus::Unavailable));
	TestEqual(TEXT("without another dial"), Rig.Client->NumTestExecConnections(), 1);

	TOptional<bool> bConnected;
	Rig.Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
	TestTrue(TEXT("connecting completes"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected.IsSet(); }));
	TestFalse(TEXT("and fails"), bConnected.Get(true));

	int32 LateHeard = 0;
	FString LateReason;
	Rig.Connection->OnAccessEnded([&LateHeard, &LateReason](const FString& In)
	{
		++LateHeard;
		LateReason = In;
	});
	TestEqual(TEXT("a listener added after the end hears it at once"), LateHeard, 1);
	TestEqualSensitive(TEXT("with the same reason"), LateReason, Reason);
	TestEqual(TEXT("the first listener is not told again"), Heard, 1);
	return true;
}

// Any other close is an ordinary loss: no listener runs, and the connection dials again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecOtherClosesAreNotAccessEndedTest,
	"CrowdySDK.CrowdyCppBridge.ExecOtherClosesAreNotAccessEnded", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecOtherClosesAreNotAccessEndedTest::RunTest(const FString& Parameters)
{
	for (const int32 Code : {1006, 1011, 4401})
	{
		FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
		if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
		{
			return false;
		}
		int32 Heard = 0;
		Rig.Connection->OnAccessEnded([&Heard](const FString&) { ++Heard; });

		bool bConnected = false;
		Rig.Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
		if (!CrowdyNativeExecOpen(*this, Rig))
		{
			return false;
		}
		TestTrue(TEXT("it connected"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected; }));

		Rig.Client->TestExecCloseFromServer(Code, Code != 1006, TEXT("access ended: suspended"));
		TestTrue(FString::Printf(TEXT("close %d dials again"), Code), CrowdyNativeExecPumpUntil(*Rig.Client,
			[&Rig]() { return Rig.Client->NumTestExecConnections() >= 2; }));
		TestEqual(FString::Printf(TEXT("close %d did not run the listener"), Code), Heard, 0);
		TestFalse(FString::Printf(TEXT("close %d did not end the access"), Code), Rig.Connection->IsAccessEnded());
		TestTrue(FString::Printf(TEXT("close %d left no reason"), Code), Rig.Connection->GetAccessEndedReason().IsEmpty());
	}
	return true;
}

// Closing the connection by hand is not the server ending access, even when the server's close is still queued.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecCloseDoesNotFireAccessEndedTest,
	"CrowdySDK.CrowdyCppBridge.ExecCloseDoesNotFireAccessEnded", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecCloseDoesNotFireAccessEndedTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}
	int32 Heard = 0;
	Rig.Connection->OnAccessEnded([&Heard](const FString&) { ++Heard; });

	bool bConnected = false;
	Rig.Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}
	TestTrue(TEXT("it connected"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected; }));

	Rig.Connection->Close();
	CrowdyNativeExecPumpUntil(*Rig.Client, []() { return false; }, 0.1);
	TestEqual(TEXT("Close did not run the listener"), Heard, 0);
	TestFalse(TEXT("nor mark the access ended"), Rig.Connection->IsAccessEnded());

	Rig.Client->TestExecCloseFromServer(4403, true, TEXT("access ended: suspended"));
	CrowdyNativeExecPumpUntil(*Rig.Client, []() { return false; }, 0.1);
	TestEqual(TEXT("a server close after Close is not heard"), Heard, 0);
	TestFalse(TEXT("and does not mark the access ended"), Rig.Connection->IsAccessEnded());
	return true;
}

// The server's close reaches the listener's connection while the game thread has not run yet; Close in between wins.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecCloseBeatsQueuedAccessEndedTest,
	"CrowdySDK.CrowdyCppBridge.ExecCloseBeatsQueuedAccessEnded", CrowdyNativeExecTestFlags)
bool FCrowdyNativeExecCloseBeatsQueuedAccessEndedTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeExecRig Rig = CrowdyNativeExecMakeRig();
	if (!TestTrue(TEXT("a connection was created"), Rig.Connection.IsValid()))
	{
		return false;
	}
	int32 Heard = 0;
	Rig.Connection->OnAccessEnded([&Heard](const FString&) { ++Heard; });

	bool bConnected = false;
	Rig.Connection->Connect([&bConnected](bool bOk, const FString&) { bConnected = bOk; });
	if (!CrowdyNativeExecOpen(*this, Rig))
	{
		return false;
	}
	TestTrue(TEXT("it connected"), CrowdyNativeExecPumpUntil(*Rig.Client, [&bConnected]() { return bConnected; }));

	Rig.Client->TestExecCloseFromServer(4403, true, TEXT("access ended: suspended"));
	Rig.Connection->Close();
	CrowdyNativeExecPumpUntil(*Rig.Client, []() { return false; }, 0.1);
	TestEqual(TEXT("the queued notice was dropped"), Heard, 0);
	TestFalse(TEXT("the access is not marked ended"), Rig.Connection->IsAccessEnded());
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

// The engine socket's close code and reason reach CrowdyCPP unchanged, which is how it tells 4403 from any other close.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeTransportForwardsApplicationCloseCodesTest,
	"CrowdySDK.CrowdyCppBridge.NativeTransportForwardsApplicationCloseCodes", CrowdyNativeExecTestFlags)
bool FCrowdyNativeTransportForwardsApplicationCloseCodesTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeFakeSocketRig Rig;
	CrowdyNativeStartFakeSocket(Rig, CrowdyCppTransport::MakeCrowdyNativeWebSocketTransport(), std::string(), 1024);
	if (!TestTrue(TEXT("the engine socket was connected"), Rig.Socket.IsValid() && Rig.Socket->bConnectCalled))
	{
		return false;
	}

	Rig.Socket->ClosedEvent.Broadcast(4403, TEXT("access ended: suspended"), true);
	if (!TestEqual(TEXT("one event"), static_cast<int32>(Rig.Events->size()), 1))
	{
		return false;
	}
	const crowdy::graphql::WebSocketEvent& Event = (*Rig.Events)[0];
	if (!TestTrue(TEXT("a close"), Event.kind == crowdy::graphql::WebSocketEventKind::Close))
	{
		return false;
	}
	TestEqual(TEXT("with the server's code"), static_cast<int32>(Event.close.code), 4403);
	TestEqual(TEXT("and its reason"), FString(UTF8_TO_TCHAR(Event.close.reason.c_str())), FString(TEXT("access ended: suspended")));
	TestTrue(TEXT("as a clean close"), Event.close.clean);
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

namespace
{
	struct FCrowdyNativeKitSentCall
	{
		FString NodeType;
		FString Key;
		FString Method;
		TArray<uint8> Payload;
		bool bValid = false;
	};

	// Reads a call frame: tag, rid, node type (str8), key (str16), method (str8), then the payload.
	FCrowdyNativeKitSentCall CrowdyNativeKitParseCall(const TArray<uint8>& Frame)
	{
		FCrowdyNativeKitSentCall Out;
		int32 At = 5;
		auto Take = [&Frame, &At](int32 Length, FString& Into)
		{
			if (Length < 0 || At + Length > Frame.Num())
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Frame.GetData() + At), Length);
			Into = FString(Converted.Length(), Converted.Get());
			At += Length;
			return true;
		};
		if (Frame.Num() < 6 || Frame[0] != 0x01 || !Take(Frame[At++], Out.NodeType) || At + 2 > Frame.Num())
		{
			return Out;
		}
		const int32 KeyLength = Frame[At] | (Frame[At + 1] << 8);
		At += 2;
		if (!Take(KeyLength, Out.Key) || At >= Frame.Num() || !Take(Frame[At++], Out.Method))
		{
			return Out;
		}
		Out.Payload = TArray<uint8>(Frame.GetData() + At, Frame.Num() - At);
		Out.bValid = true;
		return Out;
	}

	TArray<uint8> CrowdyNativeKitText(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), static_cast<int32>(FCStringAnsi::Strlen(Text)));
	}

	// The rig's connection with a kit over it, dialled and open, and the one frame the kit's first call sent.
	struct FCrowdyNativeKitRig
	{
		FCrowdyNativeExecRig Exec;
		TSharedPtr<FCrowdyNativeKit> Kit;
	};

	bool CrowdyNativeKitMakeRig(FAutomationTestBase& Test, FCrowdyNativeKitRig& Rig)
	{
		Rig.Exec = CrowdyNativeExecMakeRig();
		if (!Test.TestTrue(TEXT("a connection was created"), Rig.Exec.Connection.IsValid()))
		{
			return false;
		}
		Rig.Kit = Rig.Exec.Client->CreateGameKit(42, *Rig.Exec.Connection, 7);
		return Test.TestTrue(TEXT("a kit was made over it"), Rig.Kit.IsValid());
	}

	bool CrowdyNativeKitTakeCall(FAutomationTestBase& Test, FCrowdyNativeKitRig& Rig, FCrowdyNativeKitSentCall& Out, uint32& OutRid)
	{
		TArray<TArray<uint8>> Sent;
		const bool bSent = CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Rig, &Sent]()
		{
			Sent.Append(Rig.Exec.Client->TakeTestExecSentFrames());
			return Sent.Num() > 0;
		});
		if (!Test.TestTrue(TEXT("the kit sent a frame"), bSent) || !Test.TestEqual(TEXT("one frame"), Sent.Num(), 1))
		{
			return false;
		}
		Out = CrowdyNativeKitParseCall(Sent[0]);
		OutRid = CrowdyNativeExecRid(Sent[0]);
		return Test.TestTrue(TEXT("it is a call frame"), Out.bValid);
	}
}

// A kit call rides the SDK's own connection rather than opening one, and its arguments go out exactly as given.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitCallRidesTheSharedConnectionTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.CallRidesTheSharedConnection", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitCallRidesTheSharedConnectionTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}

	// {"a": [true, nil], "f": 2.5, "i": 4000000000, "n": -129, "s": "ok"}, keys in order and every value at its narrowest.
	const TArray<uint8> Args = {0x85, 0xa1, 0x61, 0x92, 0xc3, 0xc0, 0xa1, 0x66, 0xcb, 0x40, 0x04, 0, 0, 0, 0, 0, 0,
		0xa1, 0x69, 0xce, 0xee, 0x6b, 0x28, 0x00, 0xa1, 0x6e, 0xd1, 0xff, 0x7f, 0xa1, 0x73, 0xa2, 0x6f, 0x6b};
	int32 Replies = 0;
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("world"), FString(), TEXT("harvest"), Args, false, 0, [&Replies, &Reply](const FCrowdyNativeKitReply& In)
	{
		++Replies;
		Reply = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	FCrowdyNativeKitSentCall Call;
	uint32 Rid = 0;
	if (!CrowdyNativeKitTakeCall(*this, Rig, Call, Rid))
	{
		return false;
	}
	TestEqual(TEXT("the kit dialled no connection of its own"), Rig.Exec.Client->NumTestExecConnections(), 1);
	TestEqual(TEXT("the call names the hub"), Call.NodeType, FString(TEXT("world")));
	TestTrue(TEXT("the root hub has no key"), Call.Key.IsEmpty());
	TestEqual(TEXT("and the method"), Call.Method, FString(TEXT("harvest")));
	TestEqual(TEXT("the arguments go out byte for byte, integers and the float kept apart"),
		BytesToHexLower(Call.Payload.GetData(), Call.Payload.Num()), BytesToHexLower(Args.GetData(), Args.Num()));

	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(Rid, 0, {0xa2, 0x6f, 0x6b}));
	TestTrue(TEXT("the reply arrived"), CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Replies]() { return Replies > 0; }));
	TestEqual(TEXT("exactly once"), Replies, 1);
	TestTrue(TEXT("it is Ok"), Reply.IsOk());
	TestTrue(TEXT("with the hub's payload"), Reply.Payload == TArray<uint8>({0xa2, 0x6f, 0x6b}));
	return true;
}

// A hub's coded refusal is read into its code and words, and a call sent once carries an action id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitRefusalCarriesTheHubsCodeTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.RefusalCarriesTheHubsCode", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitRefusalCarriesTheHubsCodeTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}

	int32 Replies = 0;
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("matchmaker"), TEXT("ranked"), TEXT("join"), {0x81, 0xa6, 0x72, 0x61, 0x74, 0x69, 0x6e, 0x67, 0x05}, true, 0,
		[&Replies, &Reply](const FCrowdyNativeKitReply& In)
	{
		++Replies;
		Reply = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	FCrowdyNativeKitSentCall Call;
	uint32 Rid = 0;
	if (!CrowdyNativeKitTakeCall(*this, Rig, Call, Rid))
	{
		return false;
	}
	const FString Sent = BytesToHexLower(Call.Payload.GetData(), Call.Payload.Num());
	TestTrue(TEXT("a call sent once carries action_id"), Sent.Contains(TEXT("a9616374696f6e5f6964")));
	TestTrue(TEXT("beside its own argument"), Sent.Contains(TEXT("a6726174696e6705")));

	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(Rid, 1, CrowdyNativeKitText("kit:full: the queue is full")));
	TestTrue(TEXT("the refusal arrived"), CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Replies]() { return Replies > 0; }));
	TestEqual(TEXT("exactly once"), Replies, 1);
	TestEqual(TEXT("as the hub's own refusal"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::AppError));
	TestEqual(TEXT("with its code"), Reply.Code, FString(TEXT("full")));
	TestEqual(TEXT("and its words"), Reply.Message, FString(TEXT("the queue is full")));
	return true;
}

// Arguments the kit cannot send as one value are refused before anything goes out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitRefusesUnsendableArgumentsTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.RefusesUnsendableArguments", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitRefusesUnsendableArgumentsTest::RunTest(const FString& Parameters)
{
	crowdy::graphql::JVal Value;
	TestTrue(TEXT("no arguments are nil"), CrowdyNativeKit::ArgsFromMsgpack({}, Value) && Value.isNull());
	TestFalse(TEXT("a truncated map"), CrowdyNativeKit::ArgsFromMsgpack({0x81, 0xa1, 0x61}, Value));
	TestFalse(TEXT("trailing bytes"), CrowdyNativeKit::ArgsFromMsgpack({0x01, 0x02}, Value));
	TestFalse(TEXT("binary"), CrowdyNativeKit::ArgsFromMsgpack({0xc4, 0x01, 0x00}, Value));
	TestFalse(TEXT("an extension type"), CrowdyNativeKit::ArgsFromMsgpack({0xd4, 0x01, 0x00}, Value));
	TestFalse(TEXT("a key that is not a string"), CrowdyNativeKit::ArgsFromMsgpack({0x81, 0x01, 0x02}, Value));
	TestFalse(TEXT("an integer past int64"), CrowdyNativeKit::ArgsFromMsgpack({0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, Value));
	TestFalse(TEXT("a forged array count"), CrowdyNativeKit::ArgsFromMsgpack({0xdd, 0x7f, 0xff, 0xff, 0xff, 0x01}, Value));
	TArray<uint8> Deep;
	Deep.Init(0x91, 65);
	Deep.Add(0x01);
	TestFalse(TEXT("nesting deeper than 64"), CrowdyNativeKit::ArgsFromMsgpack(Deep, Value));
	Deep.RemoveAt(0);
	TestTrue(TEXT("nesting of 64 is fine"), CrowdyNativeKit::ArgsFromMsgpack(Deep, Value));

	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("world"), FString(), TEXT("harvest"), {0xc4, 0x01, 0x00}, false, 0, [&Reply](const FCrowdyNativeKitReply& In) { Reply = In; });
	TestEqual(TEXT("the call is refused as a bad request"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::BadRequest));
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, []() { return false; }, 0.2);
	TestEqual(TEXT("and nothing was dialled"), Rig.Exec.Client->NumTestExecConnections(), 0);
	return true;
}

// Closing the kit completes its pending call and ends its subscriptions, and the connection it rode keeps working.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitCloseKeepsTheConnectionTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.CloseKeepsTheConnection", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitCloseKeepsTheConnectionTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}

	TArray<FCrowdyNativeExecPush> Pushes;
	bool bSubscribed = false;
	Rig.Kit->Subscribe(TEXT("world"), FString(), TEXT("conditions"),
		[&Pushes](const FCrowdyNativeExecPush& Push) { Pushes.Add(Push); },
		[&bSubscribed](const FCrowdyNativeKitReply& Reply) { bSubscribed = Reply.IsOk(); });
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	TArray<TArray<uint8>> Sent;
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Rig, &Sent]()
	{
		Sent.Append(Rig.Exec.Client->TakeTestExecSentFrames());
		return Sent.Num() > 0;
	});
	if (!TestEqual(TEXT("one subscribe was sent"), Sent.Num(), 1) || !TestEqual(TEXT("it is a subscribe"), static_cast<int32>(Sent[0][0]), 0x02))
	{
		return false;
	}
	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(CrowdyNativeExecRid(Sent[0]), 0, {}));
	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecPushFrame(TEXT("world"), FString(), TEXT("conditions"), {0x80}));
	TestTrue(TEXT("the push reached the kit"), CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Pushes]() { return Pushes.Num() > 0; }));
	TestTrue(TEXT("the subscribe was acknowledged"), bSubscribed);
	TestEqual(TEXT("the kit counts one topic"), Rig.Kit->NumSubscriptions(), 1);

	int32 Replies = 0;
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("world"), FString(), TEXT("harvest"), {0x80}, false, 0, [&Replies, &Reply](const FCrowdyNativeKitReply& In)
	{
		++Replies;
		Reply = In;
	});
	FCrowdyNativeKitSentCall Call;
	uint32 Rid = 0;
	if (!CrowdyNativeKitTakeCall(*this, Rig, Call, Rid))
	{
		return false;
	}

	Rig.Kit->Close();
	TestEqual(TEXT("the pending call completed exactly once"), Replies, 1);
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::Unavailable));
	TestEqual(TEXT("with the kit's canceled message"), Reply.Message, FCrowdyNativeKit::CanceledMessage());
	const TArray<TArray<uint8>> AfterClose = Rig.Exec.Client->TakeTestExecSentFrames();
	TestTrue(TEXT("its subscription was ended on the connection"), AfterClose.ContainsByPredicate([](const TArray<uint8>& Frame) { return Frame.Num() > 0 && Frame[0] == 0x03; }));

	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(Rid, 0, {0xc0}));
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, []() { return false; }, 0.2);
	TestEqual(TEXT("a late reply does not complete it again"), Replies, 1);

	int32 ConnectionReplies = 0;
	Rig.Exec.Connection->Call(TEXT("probe"), FString(), TEXT("echo"), {0x80}, [&ConnectionReplies](const FCrowdyNativeExecReply& In)
	{
		ConnectionReplies += In.IsOk() ? 1 : 0;
	});
	TArray<TArray<uint8>> Later;
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Rig, &Later]()
	{
		Later.Append(Rig.Exec.Client->TakeTestExecSentFrames());
		return Later.Num() > 0;
	});
	if (!TestEqual(TEXT("the connection still sends"), Later.Num(), 1))
	{
		return false;
	}
	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(CrowdyNativeExecRid(Later[0]), 0, {0xc0}));
	TestTrue(TEXT("and still answers"), CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&ConnectionReplies]() { return ConnectionReplies > 0; }));
	TestTrue(TEXT("the connection is still open"), Rig.Exec.Connection->IsConnected());
	return true;
}

// A push already queued for a subscription when it ends is dropped, so a handler whose owner just unsubscribed never runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitQueuedPushAfterUnsubscribeIsDroppedTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.QueuedPushAfterUnsubscribeIsDropped", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitQueuedPushAfterUnsubscribeIsDroppedTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}
	int32 Pushes = 0;
	bool bSubscribed = false;
	const uint64 Handle = Rig.Kit->Subscribe(TEXT("world"), FString(), TEXT("conditions"),
		[&Pushes](const FCrowdyNativeExecPush&) { ++Pushes; },
		[&bSubscribed](const FCrowdyNativeKitReply& Reply) { bSubscribed = Reply.IsOk(); });
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	TArray<TArray<uint8>> Sent;
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Rig, &Sent]()
	{
		Sent.Append(Rig.Exec.Client->TakeTestExecSentFrames());
		return Sent.Num() > 0;
	});
	if (!TestEqual(TEXT("one subscribe was sent"), Sent.Num(), 1))
	{
		return false;
	}
	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecReply(CrowdyNativeExecRid(Sent[0]), 0, {}));
	TestTrue(TEXT("the subscribe was acknowledged"), CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&bSubscribed]() { return bSubscribed; }));

	// Received and queued for the next Poll, but not yet delivered.
	Rig.Exec.Client->TestExecReceiveBinary(CrowdyNativeExecPushFrame(TEXT("world"), FString(), TEXT("conditions"), {0x80}));
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	Rig.Kit->Unsubscribe(Handle);
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, []() { return false; }, 0.2);
	TestEqual(TEXT("the queued push did not reach the handler"), Pushes, 0);
	TestEqual(TEXT("the kit holds no topic"), Rig.Kit->NumSubscriptions(), 0);
	return true;
}

// A call sent once carries its action id inside its arguments, so arguments that are not a map, or none, are refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitOnceNeedsAMapTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.OnceNeedsAMap", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitOnceNeedsAMapTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("session"), TEXT("s1"), TEXT("act"), {0x92, 0x01, 0x02}, true, 0, [&Reply](const FCrowdyNativeKitReply& In) { Reply = In; });
	TestEqual(TEXT("an array sent once is refused as a bad request"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::BadRequest));
	Reply = FCrowdyNativeKitReply();
	Rig.Kit->Call(TEXT("session"), TEXT("s1"), TEXT("act"), {0x92, 0x01, 0x02}, false, 0, [&Reply](const FCrowdyNativeKitReply& In) { Reply = In; });
	TestNotEqual(TEXT("the same array sent plainly is not"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::BadRequest));

	Rig.Kit->Call(TEXT("session"), TEXT("s1"), TEXT("ready"), {}, true, 0, [](const FCrowdyNativeKitReply&) {});
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	TArray<TArray<uint8>> Sent;
	CrowdyNativeExecPumpUntil(*Rig.Exec.Client, [&Rig, &Sent]()
	{
		Sent.Append(Rig.Exec.Client->TakeTestExecSentFrames());
		return Sent.Num() >= 2;
	});
	const FCrowdyNativeKitSentCall* Ready = nullptr;
	TArray<FCrowdyNativeKitSentCall> Calls;
	for (const TArray<uint8>& Frame : Sent)
	{
		Calls.Add(CrowdyNativeKitParseCall(Frame));
	}
	Ready = Calls.FindByPredicate([](const FCrowdyNativeKitSentCall& Call) { return Call.Method == TEXT("ready"); });
	if (!TestNotNull(TEXT("no arguments sent once went out"), Ready))
	{
		return false;
	}
	TestTrue(TEXT("as a map holding only the action id"), BytesToHexLower(Ready->Payload.GetData(), Ready->Payload.Num()).StartsWith(TEXT("81a9616374696f6e5f6964")));
	return true;
}

// Closing the client closes the kits it made, completing what was pending.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitClientCloseClosesTheKitTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.ClientCloseClosesTheKit", CrowdyNativeExecTestFlags)
bool FCrowdyNativeKitClientCloseClosesTheKitTest::RunTest(const FString& Parameters)
{
	FCrowdyNativeKitRig Rig;
	if (!CrowdyNativeKitMakeRig(*this, Rig))
	{
		return false;
	}
	int32 Replies = 0;
	FCrowdyNativeKitReply Reply;
	Rig.Kit->Call(TEXT("world"), FString(), TEXT("harvest"), {0x80}, false, 0, [&Replies, &Reply](const FCrowdyNativeKitReply& In)
	{
		++Replies;
		Reply = In;
	});
	if (!CrowdyNativeExecOpen(*this, Rig.Exec))
	{
		return false;
	}
	FCrowdyNativeKitSentCall Call;
	uint32 Rid = 0;
	if (!CrowdyNativeKitTakeCall(*this, Rig, Call, Rid))
	{
		return false;
	}

	Rig.Exec.Client->Close();
	Rig.Exec.Client->Poll();
	TestTrue(TEXT("the kit is closed with its client"), Rig.Kit->IsClosed());
	TestEqual(TEXT("its pending call completed exactly once"), Replies, 1);
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Reply.Status), static_cast<int32>(ECrowdyNativeExecStatus::Unavailable));
	TestFalse(TEXT("a closed client makes no kit"), Rig.Exec.Client->CreateGameKit(42, *Rig.Exec.Connection, 7).IsValid());
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
