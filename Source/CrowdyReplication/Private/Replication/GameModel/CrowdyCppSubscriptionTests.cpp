#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Covers the GraphQL subscription surface end to end without a socket: the graphql-transport-ws handshake is a
// conversation rather than a single round trip, so the test client stands in for the server and plays it by hand.
// What the cases below are actually protecting: that a push reaches the caller only through the pump, that a
// subscription which ends says so exactly once, that traffic on the identity plane cannot disturb the socket, and
// that a payload arriving straight off the wire is bounded before it is decoded.
namespace CrowdyCppSubscriptionTestSupport
{
	constexpr EAutomationTestFlags SubscriptionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString GameBaseUrl = TEXT("https://game.test");
	const FString DiscoveryBaseUrl = TEXT("https://api.test");
	const FString GameToken = TEXT("game-bearer");

	const FString ContainerChangedDocument =
		TEXT("subscription OnContainerChanged($containerId: ID!) { gameModelContainerChanged(containerId: $containerId) { containerId } }");

	TSharedPtr<FCrowdyCppClient> MakeSubscriptionTestClient()
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = GameBaseUrl;
		Config.DiscoveryUrl = DiscoveryBaseUrl;
		TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
		if (Client.IsValid())
		{
			Client->SetGameToken(GameToken);
		}
		return Client;
	}

	TSharedPtr<FJsonObject> ParseFrame(const FString& Frame)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Frame);
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	FString FrameType(const TSharedPtr<FJsonObject>& Frame)
	{
		FString Type;
		if (Frame.IsValid())
		{
			Frame->TryGetStringField(TEXT("type"), Type);
		}
		return Type;
	}

	// The operation id the client assigned, taken from the subscribe frame it sent. Read rather than assumed,
	// because the id is the underlying client's to choose.
	FString FindSubscribeId(const TArray<FString>& Frames, const FString& AfterId = FString())
	{
		for (const FString& Frame : Frames)
		{
			const TSharedPtr<FJsonObject> Parsed = ParseFrame(Frame);
			FString Id;
			if (FrameType(Parsed) == TEXT("subscribe") && Parsed->TryGetStringField(TEXT("id"), Id) && Id != AfterId)
			{
				return Id;
			}
		}
		return FString();
	}

	bool FramesContainCompleteFor(const TArray<FString>& Frames, const FString& OperationId)
	{
		for (const FString& Frame : Frames)
		{
			const TSharedPtr<FJsonObject> Parsed = ParseFrame(Frame);
			FString Id;
			if (FrameType(Parsed) == TEXT("complete") && Parsed->TryGetStringField(TEXT("id"), Id) && Id == OperationId)
			{
				return true;
			}
		}
		return false;
	}

	FString NextFrameFor(const FString& OperationId, const FString& ContainerId)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"next\",\"payload\":{\"data\":{\"gameModelContainerChanged\":{\"containerId\":\"%s\"}}}}"),
			*OperationId, *ContainerId);
	}

	TSharedPtr<FJsonObject> Variables(const FString& ContainerId)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("containerId"), ContainerId);
		return Object;
	}

	// What one subscription saw, so a test can assert on it after the fact rather than inside a lambda.
	//
	// Every test declares this BEFORE the client it belongs to. Disposing a client ends each live subscription and
	// runs the caller's failure handler as it goes, so a recorder declared after the client would already have been
	// destroyed by the time that handler writes into it.
	struct FRecordedStream
	{
		int32 NextCount = 0;
		int32 CompleteCount = 0;
		TArray<FString> ErrorMessages;
		TArray<bool> ErrorWasTerminal;
		TSharedPtr<FJsonObject> LastData;
	};

	FCrowdyCppSubscriptionCallbacks RecordInto(FRecordedStream& Stream)
	{
		FCrowdyCppSubscriptionCallbacks Callbacks;
		Callbacks.OnNext = [&Stream](TSharedPtr<FJsonObject> Data)
		{
			++Stream.NextCount;
			Stream.LastData = Data;
		};
		Callbacks.OnError = [&Stream](const FString& Message, bool bTerminal)
		{
			Stream.ErrorMessages.Add(Message);
			Stream.ErrorWasTerminal.Add(bTerminal);
		};
		Callbacks.OnComplete = [&Stream]()
		{
			++Stream.CompleteCount;
		};
		return Callbacks;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionHandshakeTest,
	"CrowdySDK.CrowdyCpp.SubscriptionHandshake", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionHandshakeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	FRecordedStream Stream;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	const FCrowdyCppSubscriptionHandle Handle = Client->Subscribe(
		ContainerChangedDocument, Variables(TEXT("c-1")), TEXT("OnContainerChanged"), RecordInto(Stream));

	TestTrue(TEXT("subscribing yields a valid handle"), Handle.IsValid());
	TestEqual(TEXT("the subscription counts as active"), Client->NumActiveSubscriptions(), 1);
	TestEqual(TEXT("one connection was opened"), Client->NumTestWebSocketConnections(), 1);

	// Nothing is sent before the socket is up, which is what makes an unreachable server a silent no-op rather
	// than a failed write.
	TestEqual(TEXT("no frame is sent before the socket opens"), Client->TakeTestWebSocketSentFrames().Num(), 0);

	Client->TestWebSocketOpen();
	TArray<FString> Frames = Client->TakeTestWebSocketSentFrames();
	if (!TestEqual(TEXT("opening the socket sends exactly one frame"), Frames.Num(), 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Init = ParseFrame(Frames[0]);
	TestEqual(TEXT("the first frame is connection_init"), FrameType(Init), TEXT("connection_init"));

	// The bearer rides the init payload rather than an upgrade header, so this is the only place it is observable.
	const TSharedPtr<FJsonObject>* InitPayload = nullptr;
	if (TestTrue(TEXT("connection_init carries a payload"),
		Init.IsValid() && Init->TryGetObjectField(TEXT("payload"), InitPayload)))
	{
		FString Authorization;
		(*InitPayload)->TryGetStringField(TEXT("Authorization"), Authorization);
		TestEqual(TEXT("connection_init carries the game bearer"), Authorization, TEXT("Bearer ") + GameToken);
	}

	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));
	Frames = Client->TakeTestWebSocketSentFrames();
	if (!TestEqual(TEXT("the acknowledgement is answered with exactly one frame"), Frames.Num(), 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Subscribe = ParseFrame(Frames[0]);
	TestEqual(TEXT("the second frame is subscribe"), FrameType(Subscribe), TEXT("subscribe"));

	FString OperationId;
	if (!TestTrue(TEXT("the subscribe frame carries an operation id"),
		Subscribe.IsValid() && Subscribe->TryGetStringField(TEXT("id"), OperationId) && !OperationId.IsEmpty()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* SubscribePayload = nullptr;
	if (TestTrue(TEXT("the subscribe frame carries a payload"),
		Subscribe->TryGetObjectField(TEXT("payload"), SubscribePayload)))
	{
		FString Query;
		(*SubscribePayload)->TryGetStringField(TEXT("query"), Query);
		TestEqual(TEXT("the document is sent verbatim"), Query, ContainerChangedDocument);

		const TSharedPtr<FJsonObject>* SentVariables = nullptr;
		if (TestTrue(TEXT("the variables are sent"),
			(*SubscribePayload)->TryGetObjectField(TEXT("variables"), SentVariables)))
		{
			FString ContainerId;
			(*SentVariables)->TryGetStringField(TEXT("containerId"), ContainerId);
			TestEqual(TEXT("the variable value survives the crossing"), ContainerId, TEXT("c-1"));
		}
	}

	Client->TestWebSocketReceiveText(NextFrameFor(OperationId, TEXT("c-1")));

	// The push is queued on the same pump as every other completion, so it lands on the game thread rather than
	// wherever the socket happened to deliver it.
	TestEqual(TEXT("a push waits for the pump"), Stream.NextCount, 0);
	Client->Poll();
	if (!TestEqual(TEXT("the pump delivers the push"), Stream.NextCount, 1))
	{
		return false;
	}

	if (TestTrue(TEXT("the push carries decoded data"), Stream.LastData.IsValid()))
	{
		const TSharedPtr<FJsonObject>* Changed = nullptr;
		if (TestTrue(TEXT("the payload keeps its shape"),
			Stream.LastData->TryGetObjectField(TEXT("gameModelContainerChanged"), Changed)))
		{
			FString ContainerId;
			(*Changed)->TryGetStringField(TEXT("containerId"), ContainerId);
			TestEqual(TEXT("the payload keeps its values"), ContainerId, TEXT("c-1"));
		}
	}

	Client->TestWebSocketReceiveText(
		FString::Printf(TEXT("{\"id\":\"%s\",\"type\":\"complete\"}"), *OperationId));
	Client->Poll();

	TestEqual(TEXT("the server ending the stream is reported once"), Stream.CompleteCount, 1);
	TestEqual(TEXT("no error is reported for a clean stream"), Stream.ErrorMessages.Num(), 0);
	TestEqual(TEXT("a finished subscription stops counting as active"), Client->NumActiveSubscriptions(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionUnsubscribeTest,
	"CrowdySDK.CrowdyCpp.SubscriptionUnsubscribeIsSilent", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionUnsubscribeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	// Two subscriptions on purpose. With only one, unsubscribing closes the socket, and the test double then drops
	// anything the server sends afterwards, so "no push arrived" would prove the double dropped it rather than that
	// the suppression works. Keeping a second one alive keeps the socket up so a push genuinely reaches the client.
	FRecordedStream Ended;
	FRecordedStream Surviving;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	const FCrowdyCppSubscriptionHandle EndedHandle = Client->Subscribe(
		ContainerChangedDocument, Variables(TEXT("c-1")), FString(), RecordInto(Ended));
	const FCrowdyCppSubscriptionHandle SurvivingHandle = Client->Subscribe(
		ContainerChangedDocument, Variables(TEXT("c-2")), FString(), RecordInto(Surviving));

	TestEqual(TEXT("both subscriptions are active"), Client->NumActiveSubscriptions(), 2);
	TestNotEqual(TEXT("the two handles are distinct"), EndedHandle.Id, SurvivingHandle.Id);

	Client->TestWebSocketOpen();
	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));

	const TArray<FString> Frames = Client->TakeTestWebSocketSentFrames();
	const FString EndedId = FindSubscribeId(Frames);
	const FString SurvivingId = FindSubscribeId(Frames, EndedId);
	if (!TestFalse(TEXT("the first subscription was established"), EndedId.IsEmpty())
		|| !TestFalse(TEXT("the second subscription was established"), SurvivingId.IsEmpty()))
	{
		return false;
	}

	TestTrue(TEXT("unsubscribing reports that it did something"), Client->Unsubscribe(EndedHandle));
	TestEqual(TEXT("only the other subscription is left"), Client->NumActiveSubscriptions(), 1);

	// The server is told about the one that ended, and the socket stays up for the one that did not.
	TestTrue(TEXT("the server is told that subscription is over"),
		FramesContainCompleteFor(Client->TakeTestWebSocketSentFrames(), EndedId));
	TestFalse(TEXT("the socket stays open while another subscription is live"),
		Client->WasTestWebSocketClosedByClient());

	// A push already on its way to the ended subscription reaches nobody, while the live one still delivers.
	Client->TestWebSocketReceiveText(NextFrameFor(EndedId, TEXT("c-1")));
	Client->TestWebSocketReceiveText(NextFrameFor(SurvivingId, TEXT("c-2")));
	Client->Poll();

	TestEqual(TEXT("no push arrives after unsubscribing"), Ended.NextCount, 0);
	TestEqual(TEXT("no completion arrives after unsubscribing"), Ended.CompleteCount, 0);
	TestEqual(TEXT("no error arrives after unsubscribing"), Ended.ErrorMessages.Num(), 0);
	TestEqual(TEXT("the surviving subscription still delivers"), Surviving.NextCount, 1);

	// Unsubscribing twice is a no-op rather than an error, the same way cancelling a finished request is.
	TestFalse(TEXT("unsubscribing again does nothing"), Client->Unsubscribe(EndedHandle));

	// The last one out closes the socket.
	TestTrue(TEXT("ending the last subscription reports that it did something"),
		Client->Unsubscribe(SurvivingHandle));
	TestEqual(TEXT("nothing is active afterwards"), Client->NumActiveSubscriptions(), 0);
	TestTrue(TEXT("the last subscription closes the socket"), Client->WasTestWebSocketClosedByClient());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionClosedClientTest,
	"CrowdySDK.CrowdyCpp.SubscriptionClosedClientTellsCaller", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionClosedClientTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	FRecordedStream Stream;
	FRecordedStream Refused;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->Subscribe(ContainerChangedDocument, Variables(TEXT("c-1")), FString(), RecordInto(Stream));
	Client->TestWebSocketOpen();
	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));
	Client->TakeTestWebSocketSentFrames();

	// Disposing the client ends the stream the same way it cancels a pending request: a stream that simply stops
	// is indistinguishable to its caller from a server with nothing to say.
	Client->Close();

	if (TestEqual(TEXT("closing the client tells the subscriber once"), Stream.ErrorMessages.Num(), 1))
	{
		TestEqual(TEXT("the reason names the cancellation"),
			Stream.ErrorMessages[0], FCrowdyCppClient::CanceledErrorMessage());
		TestTrue(TEXT("the failure is terminal"), Stream.ErrorWasTerminal[0]);
	}
	TestEqual(TEXT("nothing is active on a closed client"), Client->NumActiveSubscriptions(), 0);

	// Subscribing on a disposed client fails immediately rather than waiting on a socket that will never open.
	const FCrowdyCppSubscriptionHandle Handle = Client->Subscribe(
		ContainerChangedDocument, Variables(TEXT("c-2")), FString(), RecordInto(Refused));

	TestFalse(TEXT("a closed client hands back no handle"), Handle.IsValid());
	if (TestEqual(TEXT("the refusal is reported once"), Refused.ErrorMessages.Num(), 1))
	{
		TestTrue(TEXT("the refusal is terminal"), Refused.ErrorWasTerminal[0]);
	}

	// A handle that refers to nothing is a no-op, not an error, even after teardown.
	TestFalse(TEXT("unsubscribing on a closed client does nothing"), Client->Unsubscribe(Handle));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionBearerIsolationTest,
	"CrowdySDK.CrowdyCpp.SubscriptionBearerIsolation", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionBearerIsolationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	FRecordedStream Stream;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->Subscribe(ContainerChangedDocument, Variables(TEXT("c-1")), FString(), RecordInto(Stream));
	Client->TestWebSocketOpen();
	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));
	Client->TakeTestWebSocketSentFrames();

	TestEqual(TEXT("one connection so far"), Client->NumTestWebSocketConnections(), 1);

	// The underlying client holds a single bearer that every request swaps to its own plane immediately before it
	// is built. A subscription client sharing that state tears its socket down on each swap and re-authenticates
	// with whichever token was installed at that moment, so this is the regression the isolation exists to stop.
	// ListLoginProviders is deliberate: it is issued with no bearer at all, so it clears the shared token.
	bool bProvidersDone = false;
	Client->ListLoginProviders([&bProvidersDone](FCrowdyCppStringListResult) { bProvidersDone = true; });
	Client->Poll();

	TestTrue(TEXT("the identity-plane call completed"), bProvidersDone);

	// Both halves are checked. The close happens synchronously on the calling thread, so it is the assertion that
	// cannot race; the reconnect that would follow it happens on a background thread, so a connection count alone
	// could read as unchanged simply because the replacement had not been opened yet.
	TestFalse(TEXT("identity-plane traffic does not close the socket"), Client->WasTestWebSocketClosedByClient());
	TestEqual(TEXT("identity-plane traffic does not reconnect the socket"),
		Client->NumTestWebSocketConnections(), 1);

	// Reinstalling the same game token is a no-op too, which matters because the client's owner does exactly that
	// on every resolve.
	Client->SetGameToken(GameToken);
	TestFalse(TEXT("reinstalling the same bearer does not close the socket"),
		Client->WasTestWebSocketClosedByClient());
	TestEqual(TEXT("reinstalling the same bearer does not reconnect the socket"),
		Client->NumTestWebSocketConnections(), 1);
	TestEqual(TEXT("the subscription is still active"), Client->NumActiveSubscriptions(), 1);
	TestEqual(TEXT("no failure was reported"), Stream.ErrorMessages.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionBoundsPayloadTest,
	"CrowdySDK.CrowdyCpp.SubscriptionBoundsForgedPayload", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionBoundsPayloadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	FRecordedStream Stream;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->Subscribe(ContainerChangedDocument, Variables(TEXT("c-1")), FString(), RecordInto(Stream));
	Client->TestWebSocketOpen();
	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));

	const FString OperationId = FindSubscribeId(Client->TakeTestWebSocketSentFrames());
	if (!TestFalse(TEXT("the subscription was established"), OperationId.IsEmpty()))
	{
		return false;
	}

	// A push arrives straight off the wire, and the decode into UE JSON is recursive, so a deep forged payload
	// would otherwise build a document tree that overflows the stack on its way out again.
	const int32 Depth = 300;
	FString Deep;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		Deep += TEXT("[");
	}
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		Deep += TEXT("]");
	}

	Client->TestWebSocketReceiveText(FString::Printf(
		TEXT("{\"id\":\"%s\",\"type\":\"next\",\"payload\":{\"data\":{\"deep\":%s}}}"), *OperationId, *Deep));
	Client->Poll();

	// Refusing to decode is reported as its own failure rather than as an empty push, so the caller can tell it
	// apart from the server genuinely having nothing to say. The stream survives it.
	TestEqual(TEXT("an undecodable payload is not delivered as data"), Stream.NextCount, 0);
	if (TestEqual(TEXT("the refusal is reported"), Stream.ErrorMessages.Num(), 1))
	{
		TestFalse(TEXT("the refusal does not end the stream"), Stream.ErrorWasTerminal[0]);
	}
	TestEqual(TEXT("the subscription survives it"), Client->NumActiveSubscriptions(), 1);

	// A payload of ordinary depth still arrives, so the guard rejects the forged case rather than everything.
	Client->TestWebSocketReceiveText(NextFrameFor(OperationId, TEXT("c-1")));
	Client->Poll();
	TestEqual(TEXT("an ordinary payload still arrives"), Stream.NextCount, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppSubscriptionOperationGateTest,
	"CrowdySDK.CrowdyCpp.SubscriptionOperationGate", CrowdyCppSubscriptionTestSupport::SubscriptionTestFlags)

bool FCrowdyCppSubscriptionOperationGateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppSubscriptionTestSupport;

	FRecordedStream Unknown;
	FRecordedStream NotASubscription;
	FRecordedStream Real;
	const TSharedPtr<FCrowdyCppClient> Client = MakeSubscriptionTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	// A name the domain does not define is refused without opening anything, the same way an unknown request is.
	const FCrowdyCppSubscriptionHandle UnknownHandle = Client->SubscribeOperation(
		ECrowdyCppApiDomain::GameModel, TEXT("NoSuchOperation"), nullptr, RecordInto(Unknown));
	TestFalse(TEXT("an unknown operation hands back no handle"), UnknownHandle.IsValid());
	TestEqual(TEXT("the unknown operation is reported"), Unknown.ErrorMessages.Num(), 1);

	// A real operation that is a query rather than a subscription resolves to a perfectly good document, which is
	// exactly why it has to be refused here instead of at the server.
	const FCrowdyCppSubscriptionHandle QueryHandle = Client->SubscribeOperation(
		ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), nullptr, RecordInto(NotASubscription));
	TestFalse(TEXT("a query hands back no handle"), QueryHandle.IsValid());
	TestEqual(TEXT("the query is reported"), NotASubscription.ErrorMessages.Num(), 1);
	TestEqual(TEXT("neither refusal opened a connection"), Client->NumTestWebSocketConnections(), 0);
	TestEqual(TEXT("neither refusal counts as active"), Client->NumActiveSubscriptions(), 0);

	// The generated subscription resolves and is sent verbatim, so no subscription query text is written by hand.
	const TSharedPtr<FJsonObject> RealVariables = MakeShared<FJsonObject>();
	RealVariables->SetStringField(TEXT("appId"), TEXT("1"));
	const FCrowdyCppSubscriptionHandle RealHandle = Client->SubscribeOperation(
		ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerChanged"), RealVariables, RecordInto(Real));

	if (!TestTrue(TEXT("the generated subscription resolves"), RealHandle.IsValid()))
	{
		return false;
	}

	Client->TestWebSocketOpen();
	Client->TestWebSocketReceiveText(TEXT("{\"type\":\"connection_ack\"}"));

	const TArray<FString> Frames = Client->TakeTestWebSocketSentFrames();
	bool bSentGeneratedDocument = false;
	for (const FString& Frame : Frames)
	{
		const TSharedPtr<FJsonObject> Parsed = ParseFrame(Frame);
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (FrameType(Parsed) != TEXT("subscribe") || !Parsed->TryGetObjectField(TEXT("payload"), Payload))
		{
			continue;
		}
		FString Query;
		(*Payload)->TryGetStringField(TEXT("query"), Query);
		if (Query.StartsWith(TEXT("subscription GameModelContainerChanged")))
		{
			bSentGeneratedDocument = true;
		}
	}
	TestTrue(TEXT("the generated document is what goes on the wire"), bSentGeneratedDocument);
	TestEqual(TEXT("no failure was reported for the real subscription"), Real.ErrorMessages.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
