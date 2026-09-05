#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyAuthPayloads.h"

// Covers the thing about the migrated authentication surface that only a live server would otherwise reveal: which
// bearer each call carries. This is where the answers are least obvious, because these calls are how a caller
// obtains a bearer rather than uses one: a sign-in carries nothing at all, and an app-token refresh carries the GAME
// bearer rather than the session one, because the token being rotated is what authorizes its own rotation. Getting
// one wrong produces a request that is syntactically fine and semantically about the wrong subject.
//
// Which endpoint they reach stopped being a question when the platform collapsed to one API origin; the URL is
// asserted only to prove the client stayed where it was configured.
namespace CrowdyAuthBridgeTestSupport
{
	constexpr EAutomationTestFlags AuthTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString GameBaseUrl = TEXT("https://game.test");
	const FString DiscoveryBaseUrl = TEXT("https://api.test");
	const FString ApiEndpoint = TEXT("https://game.test/graphql");
	const FString GameToken = TEXT("game-bearer");
	const FString ManagementToken = TEXT("identity-bearer");

	// Every sign-in path returns this shape under its own root field; the canned transport answers every request
	// with the same body, so one payload serves whichever operation the test issues.
	const FString SignInBody = TEXT(
		"{\"data\":{\"login\":{\"token\":\"session-abc\",\"gameTokenId\":\"902\","
		"\"user\":{\"userId\":\"7781\",\"email\":\"player@example.test\",\"gamertag\":\"Tagged\"}}}}");

	const FString AppTokenBody = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2026-07-27T12:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"discoveryUrl\":\"https://api.test\","
		"\"launchUrl\":\"https://launch.test\"}}}");

	TSharedPtr<FCrowdyCppClient> MakeAuthTestClient(const FString& CannedBody, int32 Status = 200)
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = GameBaseUrl;
		Config.DiscoveryUrl = DiscoveryBaseUrl;
		TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(CannedBody, Status, Config);
		if (Client.IsValid())
		{
			Client->SetGameToken(GameToken);
			Client->SetManagementToken(ManagementToken);
		}
		return Client;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthSignInParsesSessionTest,
	"CrowdySDK.Auth.SignInParsesSession", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthSignInParsesSessionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(SignInBody);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAuthResult Captured;
	Client->SignInWithPassword(TEXT("player@example.test"), TEXT("secret"),
		[&Captured, &bCalled](FCrowdyCppAuthResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestTrue(TEXT("sign-in succeeded"), Captured.bOk);
	TestEqual(TEXT("session token"), Captured.SessionToken, TEXT("session-abc"));
	// Both ids arrive as BigInt decimal strings, so a widening that silently dropped them would show up here.
	TestEqual(TEXT("session game token id"), Captured.SessionGameTokenID, static_cast<int64>(902));
	TestEqual(TEXT("user id"), Captured.UserID, static_cast<int64>(7781));
	TestEqual(TEXT("email"), Captured.Email, TEXT("player@example.test"));
	TestEqual(TEXT("gamertag"), Captured.Gamertag, TEXT("Tagged"));

	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("sign-in is issued against the configured API origin"), Url, ApiEndpoint);
		// A sign-in is how a caller obtains a bearer, so it must carry none. Leaking a previously installed token
		// onto a public mutation would put credential material on a request that has no use for it.
		TestEqual(TEXT("sign-in carries no bearer"), Authorization, FString());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthSignInWithoutTokenFailsTest,
	"CrowdySDK.Auth.SignInWithoutTokenFails", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthSignInWithoutTokenFailsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	// A reachable server that answers without a token has signed nobody in. Reporting that as success would install
	// an empty bearer and surface much later as an opaque rejection on an unrelated call.
	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(
		TEXT("{\"data\":{\"login\":{\"token\":\"\",\"gameTokenId\":\"1\",\"user\":{\"userId\":\"2\"}}}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAuthResult Captured;
	Client->SignInWithPassword(TEXT("player@example.test"), TEXT("hunter2"),
		[&Captured, &bCalled](FCrowdyCppAuthResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestFalse(TEXT("a tokenless answer is not a sign-in"), Captured.bOk);
	TestTrue(TEXT("the reason names the missing token"), Captured.ErrorMessage.Contains(TEXT("no session token")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAbsentAnswersAreFaultsTest,
	"CrowdySDK.Auth.AbsentAnswersAreFaults", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthAbsentAnswersAreFaultsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	// The vendored twins coerce an absent answer into an empty value rather than an error, which for these three
	// operations means the caller cannot tell "the server said no" from "the server said nothing". Those mean
	// opposite things at a sign-in screen, so an absent answer has to arrive as a failure.
	{
		// A sign-in carrying a token but no user identifies nobody, and accepting it would persist a session whose
		// user id is zero, which every later restore would then read back.
		const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(
			TEXT("{\"data\":{\"login\":{\"token\":\"session-abc\",\"gameTokenId\":\"902\",\"user\":null}}}"));
		FCrowdyCppAuthResult Captured;
		Client->SignInWithPassword(TEXT("a@example.test"), TEXT("p"),
			[&Captured](FCrowdyCppAuthResult Result) { Captured = MoveTemp(Result); });
		Client->Poll();
		TestFalse(TEXT("a sign-in with no user fails"), Captured.bOk);
		TestTrue(TEXT("the reason names the missing user"), Captured.ErrorMessage.Contains(TEXT("no user id")));
	}

	{
		// A null provider list would otherwise read as "this server offers no sign-in providers", and the sign-in
		// screen would render zero buttons with no error and no way forward.
		const TSharedPtr<FCrowdyCppClient> Client =
			MakeAuthTestClient(TEXT("{\"data\":{\"availableLoginProviders\":null}}"));
		FCrowdyCppStringListResult Captured;
		Client->ListLoginProviders([&Captured](FCrowdyCppStringListResult Result) { Captured = MoveTemp(Result); });
		Client->Poll();
		TestFalse(TEXT("a null provider list fails"), Captured.bOk);
	}

	{
		// A real list still reads, and a blank entry is dropped rather than handed on as a provider id.
		const TSharedPtr<FCrowdyCppClient> Client =
			MakeAuthTestClient(TEXT("{\"data\":{\"availableLoginProviders\":[\"google\",\"\",\"discord\"]}}"));
		FCrowdyCppStringListResult Captured;
		Client->ListLoginProviders([&Captured](FCrowdyCppStringListResult Result) { Captured = MoveTemp(Result); });
		Client->Poll();
		TestTrue(TEXT("a real provider list reads"), Captured.bOk);
		TestEqual(TEXT("the blank entry is dropped"), Captured.Values.Num(), 2);
	}

	{
		// The server refusing to remove an account's last sign-in method and the server answering nothing at all are
		// different outcomes; reporting the second as the first tells the user something untrue.
		const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(TEXT("{\"data\":{\"unlinkIdentity\":null}}"));
		FCrowdyCppBoolResult Captured;
		Client->UnlinkIdentity(TEXT("id-1"), [&Captured](FCrowdyCppBoolResult Result) { Captured = MoveTemp(Result); });
		Client->Poll();
		TestFalse(TEXT("a null unlink answer fails"), Captured.bOk);
		TestFalse(TEXT("and is not reported as a refusal"), Captured.bValue);
	}

	{
		// A genuine refusal still reads as one.
		const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(TEXT("{\"data\":{\"unlinkIdentity\":false}}"));
		FCrowdyCppBoolResult Captured;
		Client->UnlinkIdentity(TEXT("id-1"), [&Captured](FCrowdyCppBoolResult Result) { Captured = MoveTemp(Result); });
		Client->Poll();
		TestTrue(TEXT("a real answer reads"), Captured.bOk);
		TestFalse(TEXT("and carries the refusal"), Captured.bValue);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthServerErrorIsReportedTest,
	"CrowdySDK.Auth.ServerErrorIsReported", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthServerErrorIsReportedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(
		TEXT("{\"errors\":[{\"message\":\"Invalid credentials\"}]}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAuthResult Captured;
	Client->SignInWithPassword(TEXT("player@example.test"), TEXT("wrong"),
		[&Captured, &bCalled](FCrowdyCppAuthResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestFalse(TEXT("a rejected sign-in fails"), Captured.bOk);
	// The server's own wording has to survive to the caller: it is what a sign-in screen shows the player.
	TestEqual(TEXT("the server's reason is carried through"), Captured.ErrorMessage, TEXT("Invalid credentials"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthIdentityCallsCarrySessionBearerTest,
	"CrowdySDK.Auth.IdentityCallsCarrySessionBearer", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthIdentityCallsCarrySessionBearerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(
		TEXT("{\"data\":{\"myIdentities\":[{\"identityId\":\"id-1\",\"provider\":\"google\","
		     "\"subject\":\"sub-1\",\"email\":\"player@example.test\",\"emailVerified\":true,"
		     "\"createdAt\":\"2026-01-01T00:00:00Z\",\"lastLoginAt\":\"2026-07-01T00:00:00Z\"}]}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppJsonValueResult Captured;
	Client->ListMyIdentities(
		[&Captured, &bCalled](FCrowdyCppJsonValueResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestTrue(TEXT("the identity list was read"), Captured.bOk);

	// The value is the operation's own selected field, already unwrapped, so it is the array itself rather than the
	// GraphQL data object wrapping it.
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (TestTrue(TEXT("the payload is the unwrapped array"),
		Captured.Value.IsValid() && Captured.Value->TryGetArray(Array) && Array != nullptr))
	{
		TestEqual(TEXT("one identity"), Array->Num(), 1);
	}

	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("issued against the configured API origin"), Url, ApiEndpoint);
		TestEqual(TEXT("carries the session bearer"), Authorization, TEXT("Bearer ") + ManagementToken);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthMintCarriesSessionBearerTest,
	"CrowdySDK.Auth.MintCarriesSessionBearer", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthMintCarriesSessionBearerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(AppTokenBody);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAppTokenResult Captured;
	Client->MintAppToken(31337,
		[&Captured, &bCalled](FCrowdyCppAppTokenResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestTrue(TEXT("the mint succeeded"), Captured.bOk);
	TestEqual(TEXT("app token"), Captured.AppToken, TEXT("app-xyz"));
	TestEqual(TEXT("app game token id"), Captured.AppGameTokenID, static_cast<int64>(5150));
	TestEqual(TEXT("app id stays a string"), Captured.AppID, TEXT("31337"));
	TestEqual(TEXT("expiry"), Captured.ExpiresAt, TEXT("2026-07-27T12:00:00Z"));
	TestEqual(TEXT("game api url"), Captured.GameApiUrl, TEXT("https://game.test"));
	TestEqual(TEXT("game api ws url"), Captured.GameApiWsUrl, TEXT("wss://game.test"));
	// The shared origin, and the reason it is read at all: gameApiUrl names one instance that can go away, and this
	// is the name that survives it. Dropping it on the floor would leave a client with nothing to ask.
	TestEqual(TEXT("discovery url"), Captured.DiscoveryUrl, TEXT("https://api.test"));
	TestEqual(TEXT("launch url"), Captured.LaunchUrl, TEXT("https://launch.test"));

	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("issued against the configured API origin"), Url, ApiEndpoint);
		// The mint spends the session token to obtain an app-scoped one, so the session bearer is what authorizes it.
		TestEqual(TEXT("carries the session bearer"), Authorization, TEXT("Bearer ") + ManagementToken);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefreshCarriesGameBearerTest,
	"CrowdySDK.Auth.RefreshCarriesGameBearerAtIdentityEndpoint", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthRefreshCarriesGameBearerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	// The one call whose endpoint and bearer disagree. The app-scoped token being rotated is what authorizes its own
	// rotation, so sending the session bearer here would be refused by the server and nothing else would show it.
	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(
		TEXT("{\"data\":{\"refreshAppToken\":{\"token\":\"app-next\",\"gameTokenId\":\"5151\",\"appId\":\"31337\","
		     "\"expiresAt\":\"2026-07-27T12:30:00Z\",\"gameApiUrl\":null,\"gameApiWsUrl\":null,\"launchUrl\":null}}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAppTokenResult Captured;
	Client->RefreshAppToken(
		[&Captured, &bCalled](FCrowdyCppAppTokenResult Result)
		{
			Captured = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bCalled);
	TestTrue(TEXT("the refresh succeeded"), Captured.bOk);
	TestEqual(TEXT("the rotated token"), Captured.AppToken, TEXT("app-next"));
	// A null URL is a server answer, not a failure: it means no route has been assigned, and it must read as empty.
	TestEqual(TEXT("a null url reads as empty"), Captured.GameApiUrl, FString());

	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("issued against the configured API origin"), Url, ApiEndpoint);
		TestEqual(TEXT("carries the game bearer"), Authorization, TEXT("Bearer ") + GameToken);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthCallsAreCancelableTest,
	"CrowdySDK.Auth.CallsAreCancelable", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthCallsAreCancelableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;

	// Every auth call registers with the same cancellation plumbing as the rest of the facade. A sign-in whose
	// completion never fired would leave a latent Blueprint node's pins dead, which is indistinguishable from a hang.
	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(SignInBody);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 Delivered = 0;
	int32 Canceled = 0;
	const auto CountAuth = [&Delivered, &Canceled](FCrowdyCppAuthResult Result)
	{
		++Delivered;
		if (!Result.bOk && Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
		{
			++Canceled;
		}
	};

	Client->SignInWithPassword(TEXT("a@example.test"), TEXT("p"), CountAuth);
	Client->CompleteLoginLink(TEXT("one-time"), CountAuth);
	Client->MintAppToken(31337,
		[&Delivered, &Canceled](FCrowdyCppAppTokenResult Result)
		{
			++Delivered;
			if (!Result.bOk && Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
			{
				++Canceled;
			}
		});

	TestEqual(TEXT("three requests pending"), Client->NumPendingRequests(), 3);
	TestEqual(TEXT("cancelling drains all three"), Client->CancelAll(), 3);
	TestEqual(TEXT("each completion was delivered"), Delivered, 3);
	TestEqual(TEXT("each reported cancellation"), Canceled, 3);

	// The server's answers were already queued; they must be discarded rather than delivered a second time.
	Client->Poll();
	TestEqual(TEXT("nothing arrives after cancellation"), Delivered, 3);

	return true;
}

namespace CrowdyAuthBridgeTestSupport
{
	// Parse a JSON fragment into the already-unwrapped value shape the payload readers are handed.
	TSharedPtr<FJsonValue> ParseValue(const FString& Json)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TSharedPtr<FJsonValue> Value;
		FJsonSerializer::Deserialize(Reader, Value);
		return Value;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthGameApiPathTest,
	"CrowdySDK.Auth.GameApiPathIsAppended", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthGameApiPathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthPayloads;

	// The mint answers with a bare host and the Game API GraphQL lives at /graphql, so every Game-API POST 404s if
	// the path is not appended. Appending must also be idempotent, because a refresh re-runs it on an adopted URL.
	TestEqual(TEXT("bare host gains the path"),
		EnsureGameApiGraphqlPath(TEXT("https://game.test")), TEXT("https://game.test/graphql"));
	TestEqual(TEXT("a trailing slash is trimmed first"),
		EnsureGameApiGraphqlPath(TEXT("https://game.test/")), TEXT("https://game.test/graphql"));
	TestEqual(TEXT("an already-pathed url is unchanged"),
		EnsureGameApiGraphqlPath(TEXT("https://game.test/graphql")), TEXT("https://game.test/graphql"));
	TestEqual(TEXT("empty stays empty"), EnsureGameApiGraphqlPath(FString()), FString());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAppTokenFieldsAgreeTest,
	"CrowdySDK.Auth.AppTokenFieldsReduceFromServerPayload", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthAppTokenFieldsAgreeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;
	using namespace CrowdyAuthPayloads;

	// The sign-in pipeline adopts a token through one shared tail, so every field the server sent has to survive
	// the reduction into the exact slot the pipeline reads. Driving a real server payload end to end rather than a
	// hand-filled result keeps a server-side field rename, and a mis-wired slot, both visible here.
	const TSharedPtr<FCrowdyCppClient> Client = MakeAuthTestClient(AppTokenBody);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bCalled = false;
	FCrowdyCppAppTokenResult Minted;
	Client->MintAppToken(31337,
		[&Minted, &bCalled](FCrowdyCppAppTokenResult Result)
		{
			Minted = MoveTemp(Result);
			bCalled = true;
		});
	Client->Poll();

	if (!TestTrue(TEXT("completion fired"), bCalled))
	{
		return false;
	}
	TestTrue(TEXT("the mint succeeded"), Minted.bOk);

	const FCrowdyAppTokenFields Fields = MakeAppTokenFields(Minted);

	TestEqual(TEXT("app token"), Fields.AppToken, TEXT("app-xyz"));
	TestEqual(TEXT("expiry"), Fields.ExpiresAt, TEXT("2026-07-27T12:00:00Z"));
	TestEqual(TEXT("game api url"), Fields.GameApiUrl, TEXT("https://game.test"));
	TestEqual(TEXT("game api ws url"), Fields.GameApiWsUrl, TEXT("wss://game.test"));
	TestEqual(TEXT("launch url"), Fields.LaunchUrl, TEXT("https://launch.test"));
	// The id arrives as a BigInt decimal string, so the widening the pipeline consumes is asserted on the value.
	TestEqual(TEXT("the game token id really was widened"), Fields.AppGameTokenID, static_cast<int64>(5150));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthPayloadReadersRejectBadShapesTest,
	"CrowdySDK.Auth.PayloadReadersRejectBadShapes", CrowdyAuthBridgeTestSupport::AuthTestFlags)

bool FCrowdyAuthPayloadReadersRejectBadShapesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyAuthBridgeTestSupport;
	using namespace CrowdyAuthPayloads;

	{
		bool bSent = false;
		TestTrue(TEXT("a login-link payload reads"),
			ReadLoginLinkPayload(ParseValue(TEXT("{\"sent\":true}")), bSent));
		TestTrue(TEXT("sent"), bSent);

		bool bSentAgain = false;
		// A list is not an object. FJsonValue::AsObject would answer with a shared empty object here, so a reader
		// that tested the returned pointer would read this as a successful send that emailed nothing.
		TestFalse(TEXT("a non-object login-link payload is refused"),
			ReadLoginLinkPayload(ParseValue(TEXT("[]")), bSentAgain));
		TestFalse(TEXT("nothing is invented from a bad shape"), bSentAgain);
	}

	{
		FString AuthorizeUrl;
		FString State;
		TestTrue(TEXT("a social-start payload reads"),
			ReadSocialStartPayload(
				ParseValue(TEXT("{\"authorizeUrl\":\"https://id.test/auth\",\"state\":\"csrf-1\"}")),
				AuthorizeUrl, State));
		TestEqual(TEXT("authorize url"), AuthorizeUrl, TEXT("https://id.test/auth"));
		TestEqual(TEXT("state"), State, TEXT("csrf-1"));

		FString UrlOnly;
		FString NoState;
		// The listener treats an empty expected state as "accept any callback", so a payload without one must not
		// reach it: that is the difference between a bound OAuth flow and one anything on the machine can complete.
		TestFalse(TEXT("a social-start payload with no state is refused"),
			ReadSocialStartPayload(ParseValue(TEXT("{\"authorizeUrl\":\"https://id.test/auth\"}")), UrlOnly, NoState));

		FString NoUrl;
		FString StateOnly;
		TestFalse(TEXT("a social-start payload with no authorize url is refused"),
			ReadSocialStartPayload(ParseValue(TEXT("{\"state\":\"csrf-1\"}")), NoUrl, StateOnly));
	}

	{
		// The authorize URL reaches the platform's URL opener, which on Windows is the shell.
		TestTrue(TEXT("https is navigable"), IsBrowserNavigableUrl(TEXT("https://id.test/auth")));
		TestTrue(TEXT("http is navigable"), IsBrowserNavigableUrl(TEXT("http://localhost:8080/auth")));
		TestFalse(TEXT("a file url is refused"), IsBrowserNavigableUrl(TEXT("file:///C:/Windows/System32/cmd.exe")));
		TestFalse(TEXT("an empty url is refused"), IsBrowserNavigableUrl(FString()));
	}

	{
		const FString IdentityJson =
			TEXT("{\"identityId\":\"id-1\",\"provider\":\"google\",\"subject\":\"sub-1\","
			     "\"email\":\"player@example.test\",\"emailVerified\":true}");

		FCrowdyUserIdentity Identity;
		TestTrue(TEXT("an identity reads"), ReadIdentity(ParseValue(IdentityJson), 7781, Identity));
		TestEqual(TEXT("identity id"), Identity.IdentityId, TEXT("id-1"));
		// The operation does not select userId, so it comes from the signed-in user the call was scoped to.
		TestEqual(TEXT("user id is filled from the session"), Identity.UserId, TEXT("7781"));

		FCrowdyUserIdentity Unsigned;
		TestTrue(TEXT("an identity still reads with no signed-in user"),
			ReadIdentity(ParseValue(IdentityJson), 0, Unsigned));
		TestEqual(TEXT("but nothing is invented for the user id"), Unsigned.UserId, FString());

		FCrowdyUserIdentity Idless;
		// An identity with no id cannot be unlinked later, so surfacing it blank would strand the caller.
		TestFalse(TEXT("an identity with no id is refused"),
			ReadIdentity(ParseValue(TEXT("{\"provider\":\"google\"}")), 7781, Idless));

		FCrowdyUserIdentity FromNull;
		TestFalse(TEXT("a null identity element is refused"),
			ReadIdentity(ParseValue(TEXT("[]")), 7781, FromNull));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
