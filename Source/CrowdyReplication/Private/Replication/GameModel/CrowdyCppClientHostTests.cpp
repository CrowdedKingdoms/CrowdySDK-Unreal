#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"

#include "Misc/AutomationTest.h"

// Covers the routing and lifetime rules the shared API client rests on: an operation is looked up in the domain
// the caller names, it is issued against the one configured API origin, it carries the bearer its own domain calls
// for rather than whichever one a previous caller installed, and a disposed client refuses work instead of hanging.
namespace CrowdyCppHostTestSupport
{
	constexpr EAutomationTestFlags HostTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString ApiBaseUrl = TEXT("https://game.test");
	const FString DiscoveryBaseUrl = TEXT("https://api.test");
	const FString ApiEndpoint = TEXT("https://game.test/graphql");
	const FString GameToken = TEXT("game-bearer");
	const FString ManagementToken = TEXT("identity-bearer");

	FCrowdyCppClientConfig TestClientConfig()
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = ApiBaseUrl;
		Config.DiscoveryUrl = DiscoveryBaseUrl;
		return Config;
	}

	// One origin, both bearers installed. The name is still "two plane" because that is what survives the collapse
	// to one origin: two tokens meaning different things, which is the thing these tests are about.
	TSharedPtr<FCrowdyCppClient> MakeTwoPlaneTestClient()
	{
		TSharedPtr<FCrowdyCppClient> Client =
			FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, TestClientConfig());
		if (Client.IsValid())
		{
			Client->SetGameToken(GameToken);
			Client->SetManagementToken(ManagementToken);
		}
		return Client;
	}

	// One operation defined by each domain the facade can address. The point is coverage of the mapping itself:
	// a domain whose arm is missing or wired to the wrong generated set cannot resolve its own operation.
	//
	// Each probe also names the bearer it must carry. That used to be inferred from the URL the request went to,
	// which is no longer possible and was never really the assertion: there is one API origin now, and the bearer
	// is what the facade decides. Stating it per probe is what makes a domain silently re-planed by an edit fail
	// here rather than against a live server, where the wrong bearer answers about the wrong subject instead of
	// erroring. Every value reproduces the endpoint the same operation resolved to before the platform merged its
	// two origins, which is the arrangement the live gates were run against.
	struct FDomainProbe
	{
		ECrowdyCppApiDomain Domain;
		const TCHAR* OperationName;
		const TCHAR* DomainName;
		bool bManagementBearer;
	};

	const FDomainProbe DomainProbes[] = {
		{ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertAutomation"), TEXT("GameModel"), false},
		{ECrowdyCppApiDomain::Auth, TEXT("LogoutAllDevices"), TEXT("Auth"), true},
		{ECrowdyCppApiDomain::Users, TEXT("UsersConnection"), TEXT("Users"), true},
		{ECrowdyCppApiDomain::Apps, TEXT("App"), TEXT("Apps"), true},
		{ECrowdyCppApiDomain::AppAccess, TEXT("AppAccessTiers"), TEXT("AppAccess"), true},
		{ECrowdyCppApiDomain::GameApps, TEXT("GridOwnership"), TEXT("GameApps"), false},
		{ECrowdyCppApiDomain::Organizations, TEXT("CreateOrgRole"), TEXT("Organizations"), true},
		{ECrowdyCppApiDomain::CrowdyStudio, TEXT("CrowdyStudioProjects"), TEXT("CrowdyStudio"), false},
		{ECrowdyCppApiDomain::Teams, TEXT("AddTeamMember"), TEXT("Teams"), false},
		{ECrowdyCppApiDomain::Channels, TEXT("AddChannelMember"), TEXT("Channels"), false},
		{ECrowdyCppApiDomain::Avatars, TEXT("UserAvatars"), TEXT("Avatars"), false},
		{ECrowdyCppApiDomain::State, TEXT("DeleteUserAppState"), TEXT("State"), false},
		{ECrowdyCppApiDomain::Host, TEXT("GameHost"), TEXT("Host"), false},
		{ECrowdyCppApiDomain::Chunks, TEXT("GetChunk"), TEXT("Chunks"), false},
		{ECrowdyCppApiDomain::Voxels, TEXT("ListVoxelUpdatesByDistance"), TEXT("Voxels"), false},
		{ECrowdyCppApiDomain::Actors, TEXT("Actor"), TEXT("Actors"), false},
		{ECrowdyCppApiDomain::ServerStatus, TEXT("ServerWithLeastClients"), TEXT("ServerStatus"), false},
		{ECrowdyCppApiDomain::Platform, TEXT("PlatformConfig"), TEXT("Platform"), true},
	};

	// Issues one operation and drains its completion. The canned transport resolves inline, so the callback has
	// already run by the time this returns.
	FCrowdyCppJsonResult RunOpAndDrain(const TSharedPtr<FCrowdyCppClient>& Client, ECrowdyCppApiDomain Domain,
		const FString& OperationName, bool& bOutCalled)
	{
		FCrowdyCppJsonResult Captured;
		bOutCalled = false;
		Client->RunOp(Domain, OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bOutCalled](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bOutCalled = true;
			});
		Client->Poll();
		return Captured;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRunOpDomainRoutingTest,
	"CrowdySDK.CrowdyCpp.RunOpDomainRouting", CrowdyCppHostTestSupport::HostTestFlags)

bool FCrowdyCppRunOpDomainRoutingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppHostTestSupport;

	for (const FDomainProbe& Probe : DomainProbes)
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
		if (!TestTrue(FString::Printf(TEXT("%s: test client constructed"), Probe.DomainName), Client.IsValid()))
		{
			continue;
		}

		bool bCalled = false;
		const FCrowdyCppJsonResult Result = RunOpAndDrain(Client, Probe.Domain, Probe.OperationName, bCalled);

		TestTrue(FString::Printf(TEXT("%s: completion fired"), Probe.DomainName), bCalled);
		TestTrue(FString::Printf(TEXT("%s: %s resolved a document and was sent"),
			Probe.DomainName, Probe.OperationName), Result.bTransportOk);

		FString Url;
		FString Authorization;
		if (!TestTrue(FString::Printf(TEXT("%s: a request reached the transport"), Probe.DomainName),
			Client->GetLastTestRequest(Url, Authorization)))
		{
			continue;
		}

		// One origin serves every domain, so where the request went is no longer a routing decision and is
		// asserted only to prove the client is not silently pointed somewhere else.
		TestEqual(FString::Printf(TEXT("%s: issued against the configured API origin"), Probe.DomainName),
			Url, ApiEndpoint);

		// The bearer is the decision. It is installed per call rather than seeded once, so an operation cannot
		// inherit whichever token the previous caller left behind: under a live server the wrong bearer does not
		// error, it answers about the wrong subject, which is exactly what no canned test could otherwise catch.
		TestEqual(FString::Printf(TEXT("%s: carried the expected bearer"), Probe.DomainName),
			Authorization, TEXT("Bearer ") + (Probe.bManagementBearer ? ManagementToken : GameToken));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRunOpAmbiguousPlaneTest,
	"CrowdySDK.CrowdyCpp.RunOpAmbiguousPlaneNeedsCaller", CrowdyCppHostTestSupport::HostTestFlags)

bool FCrowdyCppRunOpAmbiguousPlaneTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppHostTestSupport;

	// "Me" answers about a different subject under each bearer: the signed-in user under the session token, the
	// app-scoped identity under the game one. A default would be wrong half the time and would only show up
	// against a live server, so the call has to refuse until the caller says which it wants.
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
		if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
		{
			return false;
		}

		bool bCalled = false;
		FCrowdyCppJsonResult Captured;
		Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("Me"), MakeShared<FJsonObject>(),
			[&Captured, &bCalled](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bCalled = true;
			});
		Client->Poll();

		TestTrue(TEXT("completion fired"), bCalled);
		TestFalse(TEXT("an ambiguous operation is refused without a plane"), Captured.bTransportOk);

		FString Url;
		FString Authorization;
		TestFalse(TEXT("nothing reached the transport"), Client->GetLastTestRequest(Url, Authorization));
	}

	// Named explicitly, the same operation carries the bearer the caller asked for.
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
		bool bCalled = false;
		Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("Me"), MakeShared<FJsonObject>(),
			[&bCalled](FCrowdyCppJsonResult) { bCalled = true; },
			ECrowdyCppTokenPlane::Management);
		Client->Poll();

		TestTrue(TEXT("completion fired"), bCalled);

		FString Url;
		FString Authorization;
		if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
		{
			TestEqual(TEXT("issued against the configured API origin"), Url, ApiEndpoint);
			TestEqual(TEXT("carried the identity bearer"), Authorization, TEXT("Bearer ") + ManagementToken);
		}
	}

	// And the other way, so the override is proven to select rather than merely to unblock.
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
		Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("Me"), MakeShared<FJsonObject>(),
			[](FCrowdyCppJsonResult) {}, ECrowdyCppTokenPlane::Game);
		Client->Poll();

		FString Url;
		FString Authorization;
		if (TestTrue(TEXT("a request reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
		{
			TestEqual(TEXT("issued against the configured API origin"), Url, ApiEndpoint);
			TestEqual(TEXT("carried the game bearer"), Authorization, TEXT("Bearer ") + GameToken);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRunOpWrongDomainTest,
	"CrowdySDK.CrowdyCpp.RunOpWrongDomainFailsClosed", CrowdyCppHostTestSupport::HostTestFlags)

bool FCrowdyCppRunOpWrongDomainTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppHostTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	// A real operation name, looked up in a domain that does not define it. Resolving against the wrong generated
	// set must fail here rather than send a document the server would reject.
	bool bCalled = false;
	const FCrowdyCppJsonResult Result =
		RunOpAndDrain(Client, ECrowdyCppApiDomain::Auth, TEXT("GameModelInvoke"), bCalled);

	TestTrue(TEXT("completion fired"), bCalled);
	TestFalse(TEXT("an operation the domain does not define is rejected"), Result.bTransportOk);
	TestTrue(TEXT("the failure names the operation"), Result.ErrorMessage.Contains(TEXT("GameModelInvoke")));

	FString Url;
	FString Authorization;
	TestFalse(TEXT("nothing reached the transport"), Client->GetLastTestRequest(Url, Authorization));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppClosedClientTest,
	"CrowdySDK.CrowdyCpp.ClosedClientFailsClosed", CrowdyCppHostTestSupport::HostTestFlags)

bool FCrowdyCppClosedClientTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppHostTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeTwoPlaneTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->Close();
	Client->Close(); // disposing twice is a no-op, since an owner may close on an error path and again on teardown

	// A disposed client must report failure rather than leave a caller waiting on a completion that never comes.
	// A latent Blueprint node whose pins never fire is indistinguishable from a hang.
	bool bCalled = false;
	const FCrowdyCppJsonResult OpResult =
		RunOpAndDrain(Client, ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), bCalled);
	TestTrue(TEXT("run op completion fired"), bCalled);
	TestFalse(TEXT("run op failed"), OpResult.bTransportOk);

	bool bReadCalled = false;
	bool bReadOk = true;
	Client->ReadContainerState(1, TEXT("c-1"),
		[&bReadCalled, &bReadOk](FCrowdyCppContainerStateResult Result)
		{
			bReadCalled = true;
			bReadOk = Result.bOk;
		});
	Client->Poll();
	TestTrue(TEXT("container read completion fired"), bReadCalled);
	TestFalse(TEXT("container read failed"), bReadOk);

	bool bSeedCalled = false;
	bool bSeedOk = true;
	Client->SeedSchema(TEXT("{}"),
		[&bSeedCalled, &bSeedOk](FCrowdyCppStudioOpResult Result)
		{
			bSeedCalled = true;
			bSeedOk = Result.bOk;
		});
	Client->Poll();
	TestTrue(TEXT("seed completion fired"), bSeedCalled);
	TestFalse(TEXT("seed failed"), bSeedOk);

	FString Url;
	FString Authorization;
	TestFalse(TEXT("nothing reached the transport after disposal"), Client->GetLastTestRequest(Url, Authorization));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppAdminHostLifecycleTest,
	"CrowdySDK.CrowdyCpp.AdminClientHostLifecycle", CrowdyCppHostTestSupport::HostTestFlags)

bool FCrowdyCppAdminHostLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppHostTestSupport;

	TSharedPtr<FCrowdyCppAdminClientHost> Host =
		FCrowdyCppAdminClientHost::Create(TestClientConfig(), TEXT("admin-bearer"));
	if (!TestTrue(TEXT("admin host created"), Host.IsValid()))
	{
		return false;
	}

	const TSharedRef<FCrowdyCppClient> Client = Host->GetClient();

	// Releasing the host is the whole disposal protocol: it closes the client and its pump unregisters itself, so a
	// caller that forgets an explicit teardown call cannot leave one polling. Holding the client past the host is
	// what makes that observable here, and it must come back disposed rather than merely unpumped.
	const TWeakPtr<FCrowdyCppAdminClientHost> WeakHost = Host;
	Host.Reset();
	TestFalse(TEXT("releasing the last reference destroys the host"), WeakHost.IsValid());

	bool bCalled = false;
	bool bOk = true;
	Client->SeedSchema(TEXT("{}"),
		[&bCalled, &bOk](FCrowdyCppStudioOpResult Result)
		{
			bCalled = true;
			bOk = Result.bOk;
		});
	TestTrue(TEXT("a call on the outlived client still completes"), bCalled);
	TestFalse(TEXT("the host disposed its client on the way out"), bOk);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
