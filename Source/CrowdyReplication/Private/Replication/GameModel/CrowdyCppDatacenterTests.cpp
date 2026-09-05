#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

// Covers what the platform's collapse to one API origin replaced two endpoints with: finding which datacenter an app
// is served from, moving there, and refusing to be moved somewhere else. Under direct connect a client is pinned to
// one instance, so these are the paths that decide whether it can recover when that instance goes away, and none of
// them fails loudly when it is wrong. A client that quietly stays put reads as a slow server.
namespace CrowdyCppDatacenterTestSupport
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Two hosts in one estate (they share their last two labels) and one outside it, matching the shape the estate
	// bound actually compares.
	const FString HomeApiUrl = TEXT("https://ck-api-1.prod.cp.cks-env.com");
	const FString HomeEndpoint = TEXT("https://ck-api-1.prod.cp.cks-env.com/graphql");
	const FString SiblingApiUrl = TEXT("https://ck-api-4.prod.cp.cks-env.com");
	const FString SiblingEndpoint = TEXT("https://ck-api-4.prod.cp.cks-env.com/graphql");
	const FString OffEstateApiUrl = TEXT("https://ck-api-4.attacker.example.com");
	const FString DiscoveryUrl = TEXT("https://ck.prod.cp.cks-env.com");

	FCrowdyCppClientConfig HomeConfig()
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = HomeApiUrl;
		Config.DiscoveryUrl = DiscoveryUrl;
		return Config;
	}

	TSharedPtr<FCrowdyCppClient> MakeClient(const FString& CannedBody, int32 Status = 200)
	{
		return FCrowdyCppClient::MakeForTest(CannedBody, Status, HomeConfig());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppAppDiscoveryTest,
	"CrowdySDK.CrowdyCpp.AppDiscoveryResolvesPlacement", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppAppDiscoveryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	// Two apps in one call, one placed and one not. The unplaced entry is the case worth pinning: it means "stay on
	// the shared origin", and folding it into a failure would send a perfectly working client looking for a home.
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT(
		"{\"data\":{\"appDiscovery\":["
		"{\"appId\":\"78473868461312\",\"datacenterCode\":\"or\","
		"\"gameApiUrl\":\"https://ck-api-4.prod.cp.cks-env.com\","
		"\"gameApiWsUrl\":\"wss://ck-api-4.prod.cp.cks-env.com\"},"
		"{\"appId\":\"99\",\"datacenterCode\":\"\",\"gameApiUrl\":\"\",\"gameApiWsUrl\":\"\"}]}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bFired = false;
	FCrowdyCppAppDiscoveryResult Captured;
	Client->ResolveAppEndpoints({TEXT("78473868461312"), TEXT("99")},
		[&Captured, &bFired](FCrowdyCppAppDiscoveryResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		});
	Client->Poll();

	if (!TestTrue(TEXT("completion fired"), bFired) || !TestTrue(TEXT("discovery succeeded"), Captured.bOk))
	{
		return false;
	}
	if (!TestEqual(TEXT("both apps came back"), Captured.Endpoints.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("the placed app names its datacenter"), Captured.Endpoints[0].DatacenterCode, TEXT("or"));
	TestEqual(TEXT("the placed app names its endpoint"), Captured.Endpoints[0].GameApiUrl, SiblingApiUrl);
	TestTrue(TEXT("the placed app reports itself placed"), Captured.Endpoints[0].IsPlaced());
	TestFalse(TEXT("the unplaced app reports itself unplaced"), Captured.Endpoints[1].IsPlaced());

	// No bearer, and that is the point of the call: a client asks this before it holds any credential, so a token
	// riding along would mean the answer depended on being signed in already.
	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("discovery carries no Authorization header"), Authorization, FString());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppAppDiscoveryEmptyInputTest,
	"CrowdySDK.CrowdyCpp.AppDiscoveryRefusesAnEmptyList", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppAppDiscoveryEmptyInputTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"data\":{\"appDiscovery\":[]}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	bool bFired = false;
	FCrowdyCppAppDiscoveryResult Captured;
	Client->ResolveAppEndpoints({FString()},
		[&Captured, &bFired](FCrowdyCppAppDiscoveryResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		});
	Client->Poll();

	// Answered without a round trip. The server rejects an empty list, and finding that out costs a request whose
	// failure tells the caller nothing it could not have known before sending.
	TestTrue(TEXT("completion fired"), bFired);
	TestFalse(TEXT("an empty list is refused"), Captured.bOk);

	FString Url;
	FString Authorization;
	TestFalse(TEXT("nothing reached the transport"), Client->GetLastTestRequest(Url, Authorization));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppMoveToDatacenterTest,
	"CrowdySDK.CrowdyCpp.MoveToDatacenterIsBoundedByEstate", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppMoveToDatacenterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"data\":{}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("the client starts where it was configured"), Client->GetApiEndpoint(), HomeEndpoint);

	// A sibling instance in the same estate is where a real redirect points.
	TestTrue(TEXT("a move within the estate is accepted"), Client->MoveToDatacenter(SiblingApiUrl));
	TestEqual(TEXT("the endpoint followed the move"), Client->GetApiEndpoint(), SiblingEndpoint);

	// Same URL again is not a move. Reporting one would let a caller believe it had recovered while sitting on the
	// endpoint that just failed, which is how a redirect loop starts.
	TestFalse(TEXT("moving to the current endpoint is not a move"), Client->MoveToDatacenter(SiblingApiUrl));

	// The bound that matters. The directive arrives authenticated, so this is not about whether the server is who it
	// says; it is about what a server may ask for. One compromised instance must not be able to walk a fleet onto an
	// origin it chose.
	TestFalse(TEXT("a move outside the estate is refused"), Client->MoveToDatacenter(OffEstateApiUrl));
	TestEqual(TEXT("a refused move leaves the endpoint alone"), Client->GetApiEndpoint(), SiblingEndpoint);

	TestFalse(TEXT("an empty target is refused"), Client->MoveToDatacenter(FString()));
	TestEqual(TEXT("an empty target leaves the endpoint alone"), Client->GetApiEndpoint(), SiblingEndpoint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRedirectOvertakenTest,
	"CrowdySDK.CrowdyCpp.RedirectOvertakenByAnotherRequestStillRetries", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppRedirectOvertakenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	// Requests to one endpoint can be in flight together, and every one of them is answered with the same
	// WRONG_DATACENTER. The first to complete moves the client; the redirect handler then reports "no move" to the
	// rest only because the target is already current. Those requests must retry too - at the endpoint the client
	// now sits on - rather than surface the redirect to their callers as an error. This is the startup shape of any
	// caller that fans out queries before first contact resolved the app's datacenter. The in-flight hook below
	// plays the winning request by moving the client while the losing request is still on the wire.
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"data\":{}}"));
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetGameToken(TEXT("game-bearer"));

	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString::Printf(TEXT(
		"{\"errors\":[{\"message\":\"App 78473868461312 is served from datacenter 'or', not 'va'.\","
		"\"extensions\":{\"code\":\"WRONG_DATACENTER\",\"appId\":\"78473868461312\","
		"\"appDatacenter\":\"or\",\"servedBy\":\"va\","
		"\"gameApiUrl\":\"%s\",\"gameApiWsUrl\":\"wss://ck-api-4.prod.cp.cks-env.com\"}}]}"), *SiblingApiUrl));
	Client->SetTestResponseScript(MoveTemp(Script));

	int32 RequestCount = 0;
	TWeakPtr<FCrowdyCppClient> WeakClient = Client;
	Client->SetTestOnRequest([WeakClient, &RequestCount](const FString&)
	{
		++RequestCount;
		if (RequestCount == 1)
		{
			if (const TSharedPtr<FCrowdyCppClient> Pinned = WeakClient.Pin())
			{
				Pinned->MoveToDatacenter(SiblingApiUrl);
			}
		}
	});

	bool bFired = false;
	FCrowdyCppJsonResult Captured;
	Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), MakeShared<FJsonObject>(),
		[&Captured, &bFired](FCrowdyCppJsonResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		});
	// Two pumps: the first delivers the redirect and issues the retry, the second delivers the retry's answer.
	Client->Poll();
	Client->Poll();

	TestTrue(TEXT("completion fired"), bFired);
	TestTrue(TEXT("the overtaken request retried and succeeded"), Captured.bTransportOk);
	TestEqual(TEXT("exactly one retry was issued"), RequestCount, 2);

	FString Url;
	FString Authorization;
	if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		TestEqual(TEXT("the retry went to the moved-to endpoint, not the refused one"), Url, SiblingEndpoint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppAppUnavailableTest,
	"CrowdySDK.CrowdyCpp.AppUnavailableSurfacesItsCode", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppAppUnavailableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	// APP_UNAVAILABLE carries no endpoint on purpose: the app's own datacenter cannot serve it and there is nowhere
	// to move to, so a caller has to be able to tell it apart from an ordinary failure and stop retrying elsewhere.
	// It is deliberately not first in the array, because a partially resolved query reports whatever failed first
	// and the routing error can sit behind it.
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT(
		"{\"errors\":["
		"{\"message\":\"Field resolution failed\",\"extensions\":{\"code\":\"INTERNAL_ERROR\"}},"
		"{\"message\":\"This game is temporarily unavailable. Try again shortly.\","
		"\"extensions\":{\"code\":\"APP_UNAVAILABLE\",\"appId\":\"78473868461312\","
		"\"appDatacenter\":\"or\",\"servedBy\":\"or\",\"retryable\":true}}]}"),
		200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetGameToken(TEXT("game-bearer"));

	bool bFired = false;
	FCrowdyCppJsonResult Captured;
	Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), MakeShared<FJsonObject>(),
		[&Captured, &bFired](FCrowdyCppJsonResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bFired);
	TestFalse(TEXT("the operation failed"), Captured.bTransportOk);
	TestEqual(TEXT("the unavailable code is surfaced even from behind another error"),
		Captured.ErrorCode, TEXT("APP_UNAVAILABLE"));

	// The server writes this one for a player to read, so it is worth showing rather than substituting generic text.
	TestTrue(TEXT("the server's own message is kept"),
		Captured.ErrorMessage.Contains(TEXT("temporarily unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppOrdinaryFailureHasNoCodeTest,
	"CrowdySDK.CrowdyCpp.TransportFailureCarriesNoServerCode", CrowdyCppDatacenterTestSupport::TestFlags)

bool FCrowdyCppOrdinaryFailureHasNoCodeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppDatacenterTestSupport;

	// A non-2xx with no GraphQL errors, which is the shape a gateway or auth-middleware rejection arrives in. The
	// code must stay empty rather than borrow one, or a caller branching on APP_UNAVAILABLE would have to also
	// verify the failure came from the server at all.
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("gateway timeout"), 504);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetGameToken(TEXT("game-bearer"));

	bool bFired = false;
	FCrowdyCppJsonResult Captured;
	Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), MakeShared<FJsonObject>(),
		[&Captured, &bFired](FCrowdyCppJsonResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		});
	Client->Poll();

	TestTrue(TEXT("completion fired"), bFired);
	TestFalse(TEXT("the operation failed"), Captured.bTransportOk);
	TestEqual(TEXT("a transport failure carries no server code"), Captured.ErrorCode, FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
