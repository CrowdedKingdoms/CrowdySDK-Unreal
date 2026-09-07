#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

// The CrowdyStudio console used to send its own hand-written GraphQL text. It now names an operation and lets
// FCrowdyCppClient::RunOp resolve it against the vendored CrowdyCPP client's generated operation tables. This file
// pins that routing: every (domain, operation) pair the console issues must still resolve to a document, and the
// management-plane ones must still reach the management endpoint under the management bearer while the game-plane
// ones reach the game endpoint under the game bearer. A vendored-library bump that renames or drops one of these
// operations, or moves it to a different endpoint, should fail here rather than only the first time someone clicks
// the affected button against a live server.
namespace
{
	constexpr EAutomationTestFlags CrowdyStudioRoutingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// One (domain, operation name, plane) triple the console routes through RunOp. Plane is the one the console
	// actually issues the operation under. It used to match the endpoint the vendored operation table assigned;
	// since the platform merged its two GraphQL origins that table names both for almost everything, so the plane
	// is now the bridge's own per-domain answer and these values are what pin it.
	struct FRoutedOperation
	{
		ECrowdyCppApiDomain Domain;
		const TCHAR* OperationName;
		ECrowdyCppTokenPlane Plane;
	};

	const FRoutedOperation kRoutedOperations[] =
	{
		{ ECrowdyCppApiDomain::Organizations, TEXT("MyOrganizations"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Organizations, TEXT("CreateOrganization"), ECrowdyCppTokenPlane::Management },

		{ ECrowdyCppApiDomain::Apps, TEXT("MyApps"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("CreateApp"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("UpdateApp"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("ArchiveApp"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("App"), ECrowdyCppTokenPlane::Management },

		{ ECrowdyCppApiDomain::AppAccess, TEXT("RuntimePermissions"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::AppAccess, TEXT("AppAccessTiers"), ECrowdyCppTokenPlane::Management },

		{ ECrowdyCppApiDomain::Teams, TEXT("Teams"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("SetTeamPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("CreateTeam"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamMembers"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamRoles"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("AddTeamMember"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("RemoveTeamMember"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("SetTeamMemberRoles"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("CreateTeamRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("UpdateTeamRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("DeleteTeamRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("DeleteTeam"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("UpdateTeam"), ECrowdyCppTokenPlane::Game },

		{ ECrowdyCppApiDomain::Channels, TEXT("Channels"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("SetChannelPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("CreateChannel"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelMembers"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelRoles"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("AddChannelMember"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("RemoveChannelMember"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("SetChannelMemberRoles"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("CreateChannelRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("UpdateChannelRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("DeleteChannelRole"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("DeleteChannel"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("UpdateChannel"), ECrowdyCppTokenPlane::Game },

		{ ECrowdyCppApiDomain::GameApps, TEXT("NearbyGridPermissions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("CreateGrid"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("GridPermissionLimits"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("SetGridPermissionLimits"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("GridGroupGrants"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("AssignGroupToGrid"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("RevokeGroupFromGrid"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("GridUserPermissions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("GrantGridPermissions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("RevokeGridPermissions"), ECrowdyCppTokenPlane::Game },

		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertContainerType"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelPropertyDefs"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertPropertyDef"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelFunctions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertFunction"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteFunction"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelFeatures"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDefineFeature"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelTierFeatures"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelGrantTierFeature"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelRevokeTierFeature"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelSetPolicy"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelSeed"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomations"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomationTriggers"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertAutomation"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertAutomationTrigger"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteAutomation"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteContainerType"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeletePropertyDef"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerState"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteContainer"), ECrowdyCppTokenPlane::Game },
	};

	// What issuing one operation on a canned-transport test client revealed. Resolution and transport delivery are
	// reported separately so a caller can tell "the name did not resolve" apart from "it resolved but something
	// about the canned round trip itself failed".
	struct FProbeResult
	{
		bool bResolved = false;
		bool bTransportOk = false;
		FString ErrorMessage;
	};

	// Issues one operation and reports whether it resolved to a document, without inferring that from the error
	// text. RunOp resolves an operation name against the domain's generated table before it ever reaches the
	// transport: a name the domain does not define is answered right inside the RunOp call, because it never reaches
	// the GraphQL client's dispatcher queue. A name that DOES resolve is handed to that dispatcher and only
	// completes once Poll() drains it. So "did the completion already run before Poll() was ever called" is a
	// direct read of whether resolution happened, not a guess from wording in the message.
	FProbeResult ProbeResolves(const TSharedPtr<FCrowdyCppClient>& Client, ECrowdyCppApiDomain Domain,
		const TCHAR* OperationName, ECrowdyCppTokenPlane Plane)
	{
		FProbeResult Probe;
		bool bFired = false;
		FCrowdyCppJsonResult Captured;
		Client->RunOp(Domain, OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bFired](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bFired = true;
			},
			Plane);

		if (bFired)
		{
			// Already delivered without a Poll(): the pre-flight rejection path, which means the name did not
			// resolve to a document.
			Probe.bResolved = false;
			Probe.ErrorMessage = Captured.ErrorMessage;
			return Probe;
		}

		Client->Poll();
		Probe.bResolved = bFired;
		Probe.bTransportOk = Captured.bTransportOk;
		Probe.ErrorMessage = Captured.ErrorMessage;
		return Probe;
	}
}

// Every (domain, operation) pair the CrowdyStudio console routes through RunOp must still resolve to a document in
// the vendored CrowdyCPP operation tables. A canned 200 response with an empty data object stands in for the
// server, so this is a pure name-resolution check with no network: it catches a renamed or removed operation the
// moment a vendored-library bump lands, rather than the first time someone clicks the affected button against a
// live server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioRoutedOperationsResolveTest,
	"CrowdySDK.CrowdyStudio.RoutedOperationsResolve", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioRoutedOperationsResolveTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("{\"data\":{}}");

	for (const FRoutedOperation& Op : kRoutedOperations)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, 200);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Op.OperationName), Client.IsValid()))
		{
			continue;
		}
		Client->SetManagementToken(TEXT("test-management-token"));

		const FProbeResult Probe = ProbeResolves(Client, Op.Domain, Op.OperationName, Op.Plane);
		TestTrue(FString::Printf(TEXT("%s resolves to a document"), Op.OperationName), Probe.bResolved);
		TestTrue(FString::Printf(TEXT("%s reaches the canned transport"), Op.OperationName), Probe.bTransportOk);
	}
	return true;
}

// A deliberately bogus operation name in a real, routed domain must fail to resolve the same way a renamed or
// dropped vendored operation would. This is the proof that the resolves-check above can actually fail: a check
// that always passes is not a gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioRoutedOperationUnknownFailsTest,
	"CrowdySDK.CrowdyStudio.RoutedOperationUnknownFails", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioRoutedOperationUnknownFailsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructs"), Client.IsValid()))
	{
		return false;
	}
	Client->SetManagementToken(TEXT("test-management-token"));

	const FProbeResult Probe = ProbeResolves(Client, ECrowdyCppApiDomain::GameModel,
		TEXT("GameModelThisOperationDoesNotExist"), ECrowdyCppTokenPlane::Game);

	TestFalse(TEXT("a bogus operation name does not resolve"), Probe.bResolved);
	TestTrue(TEXT("the failure names the operation that could not be resolved"),
		Probe.ErrorMessage.Contains(TEXT("GameModelThisOperationDoesNotExist")));
	return true;
}

// The bearer an operation carries is not implied by whether it resolves: a management-plane operation that resolves
// fine but is accidentally issued under the game plane (or vice versa) would only be caught by a live server
// rejecting it or, worse, quietly answering about the wrong app. One representative operation per domain is issued
// against a test client with two clearly distinct bearers, and the captured request is checked against the bearer
// the console actually uses for that domain.
//
// This used to check the endpoint too, because the two planes were two servers. They are one server now, so the URL
// is asserted only to prove the client stayed where it was configured, and the bearer carries the whole assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioRoutedOperationsEndpointTest,
	"CrowdySDK.CrowdyStudio.RoutedOperationsEndpoint", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioRoutedOperationsEndpointTest::RunTest(const FString& Parameters)
{
	const FString ApiUrl = TEXT("https://game.example.test/graphql");
	const FString GameBearer = TEXT("game-bearer-token");
	const FString ManagementBearer = TEXT("management-bearer-token");

	FCrowdyCppClientConfig ClientConfig;
	ClientConfig.ApiUrl = ApiUrl;
	ClientConfig.DiscoveryUrl = TEXT("https://api.example.test/graphql");

	const FRoutedOperation Representatives[] =
	{
		{ ECrowdyCppApiDomain::Organizations, TEXT("MyOrganizations"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("MyApps"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::AppAccess, TEXT("RuntimePermissions"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Teams, TEXT("Teams"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("Channels"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("NearbyGridPermissions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), ECrowdyCppTokenPlane::Game },
	};

	for (const FRoutedOperation& Rep : Representatives)
	{
		const TSharedPtr<FCrowdyCppClient> Client =
			FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, ClientConfig);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Rep.OperationName), Client.IsValid()))
		{
			continue;
		}
		Client->SetGameToken(GameBearer);
		Client->SetManagementToken(ManagementBearer);

		bool bFired = false;
		FCrowdyCppJsonResult Captured;
		Client->RunOp(Rep.Domain, Rep.OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bFired](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bFired = true;
			},
			Rep.Plane);
		if (!bFired)
		{
			Client->Poll();
		}
		if (!TestTrue(FString::Printf(TEXT("%s reaches the canned transport"), Rep.OperationName), Captured.bTransportOk))
		{
			continue;
		}

		FString ActualUrl;
		FString ActualAuthorization;
		if (!TestTrue(FString::Printf(TEXT("%s recorded a captured request"), Rep.OperationName),
			Client->GetLastTestRequest(ActualUrl, ActualAuthorization)))
		{
			continue;
		}

		const bool bManagement = Rep.Plane == ECrowdyCppTokenPlane::Management;
		const FString ExpectedBearer = bManagement ? ManagementBearer : GameBearer;
		const FString UnexpectedBearer = bManagement ? GameBearer : ManagementBearer;

		TestEqual(FString::Printf(TEXT("%s is issued against the configured API origin"), Rep.OperationName),
			ActualUrl, ApiUrl);
		TestTrue(FString::Printf(TEXT("%s carries the %s bearer"), Rep.OperationName,
			bManagement ? TEXT("management") : TEXT("game")), ActualAuthorization.Contains(ExpectedBearer));
		TestFalse(FString::Printf(TEXT("%s does not carry the other plane's bearer"), Rep.OperationName),
			ActualAuthorization.Contains(UnexpectedBearer));
	}
	return true;
}

// The same check with no plane named by the caller, which is how every service subsystem and every Game Model
// runtime call issues its operations. Those used to inherit the plane from the vendored operation table's endpoint
// assignment; the platform has since merged its two GraphQL origins into one and that table no longer names an
// endpoint at all. The bridge answers per domain instead, and the values below are the ones each operation resolved
// to before the merge, which is the arrangement the live gates were run against. A domain whose default drifts
// sends a syntactically valid request under the wrong bearer, which a server answers rather than refuses, so
// nothing downstream would notice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDefaultPlaneTest,
	"CrowdySDK.CrowdyStudio.RoutedOperationsDefaultPlane", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioDefaultPlaneTest::RunTest(const FString& Parameters)
{
	const FString ApiUrl = TEXT("https://game.example.test/graphql");
	const FString GameBearer = TEXT("game-bearer-token");
	const FString ManagementBearer = TEXT("management-bearer-token");

	FCrowdyCppClientConfig ClientConfig;
	ClientConfig.ApiUrl = ApiUrl;
	ClientConfig.DiscoveryUrl = TEXT("https://api.example.test/graphql");

	const FRoutedOperation Representatives[] =
	{
		{ ECrowdyCppApiDomain::Organizations, TEXT("MyOrganizations"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Apps, TEXT("MyApps"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::AppAccess, TEXT("RuntimePermissions"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Platform, TEXT("PlatformConfig"), ECrowdyCppTokenPlane::Management },

		{ ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerState"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::GameApps, TEXT("NearbyGridPermissions"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::CrowdyStudio, TEXT("CrowdyStudioProjects"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teams, TEXT("Teams"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Channels, TEXT("Channels"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Avatars, TEXT("MyAvatars"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::State, TEXT("UserAppState"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Host, TEXT("AmIGameHost"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Chunks, TEXT("GetChunk"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Voxels, TEXT("ListVoxels"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Actors, TEXT("Actor"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::Teleport, TEXT("TeleportRequest"), ECrowdyCppTokenPlane::Game },

		// The three domains that never agreed on one plane, so these come from the per-operation exceptions rather
		// than from a domain-wide answer. Their siblings are covered by MixedDomainStillNeedsACallerPlane below.
		{ ECrowdyCppApiDomain::Auth, TEXT("LogoutAllDevices"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Users, TEXT("UsersConnection"), ECrowdyCppTokenPlane::Management },
		{ ECrowdyCppApiDomain::Users, TEXT("UpdateUserState"), ECrowdyCppTokenPlane::Game },
		{ ECrowdyCppApiDomain::ServerStatus, TEXT("ServerWithLeastClients"), ECrowdyCppTokenPlane::Game },
	};

	for (const FRoutedOperation& Rep : Representatives)
	{
		const TSharedPtr<FCrowdyCppClient> Client =
			FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, ClientConfig);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Rep.OperationName), Client.IsValid()))
		{
			continue;
		}
		Client->SetGameToken(GameBearer);
		Client->SetManagementToken(ManagementBearer);

		bool bFired = false;
		FCrowdyCppJsonResult Captured;
		// No plane argument: this is the point of the test.
		Client->RunOp(Rep.Domain, Rep.OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bFired](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bFired = true;
			});
		if (!bFired)
		{
			Client->Poll();
		}
		if (!TestTrue(FString::Printf(TEXT("%s is issued without a caller-named plane"), Rep.OperationName),
			Captured.bTransportOk))
		{
			continue;
		}

		FString ActualUrl;
		FString ActualAuthorization;
		if (!TestTrue(FString::Printf(TEXT("%s recorded a captured request"), Rep.OperationName),
			Client->GetLastTestRequest(ActualUrl, ActualAuthorization)))
		{
			continue;
		}

		const bool bManagement = Rep.Plane == ECrowdyCppTokenPlane::Management;
		TestEqual(FString::Printf(TEXT("%s is issued against the configured API origin"), Rep.OperationName),
			ActualUrl, ApiUrl);
		TestTrue(FString::Printf(TEXT("%s defaults to the %s bearer"), Rep.OperationName,
			bManagement ? TEXT("management") : TEXT("game")),
			ActualAuthorization.Contains(bManagement ? ManagementBearer : GameBearer));
		TestFalse(FString::Printf(TEXT("%s does not default to the other plane's bearer"), Rep.OperationName),
			ActualAuthorization.Contains(bManagement ? GameBearer : ManagementBearer));
	}
	return true;
}

// The other half of the default: an operation that was reachable at both endpoints before the merge answers about
// a different subject under each bearer, so it has never had a default and must still refuse rather than guess.
// Without this, adding a domain-wide answer would quietly convert those refusals into a coin flip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioMixedDomainNeedsPlaneTest,
	"CrowdySDK.CrowdyStudio.MixedDomainStillNeedsACallerPlane", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioMixedDomainNeedsPlaneTest::RunTest(const FString& Parameters)
{
	struct FAmbiguousOperation
	{
		ECrowdyCppApiDomain Domain;
		const TCHAR* OperationName;
	};

	const FAmbiguousOperation Ambiguous[] =
	{
		{ ECrowdyCppApiDomain::Users, TEXT("Me") },
		{ ECrowdyCppApiDomain::Users, TEXT("UpdateGamertag") },
		{ ECrowdyCppApiDomain::Auth, TEXT("Logout") },
		{ ECrowdyCppApiDomain::ServerStatus, TEXT("VersionInfo") },
	};

	for (const FAmbiguousOperation& Op : Ambiguous)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Op.OperationName), Client.IsValid()))
		{
			continue;
		}
		Client->SetGameToken(TEXT("game-bearer-token"));
		Client->SetManagementToken(TEXT("management-bearer-token"));

		bool bFired = false;
		FCrowdyCppJsonResult Captured;
		Client->RunOp(Op.Domain, Op.OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bFired](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bFired = true;
			});
		if (!bFired)
		{
			Client->Poll();
		}

		TestFalse(FString::Printf(TEXT("%s is refused without a caller-named plane"), Op.OperationName),
			Captured.bTransportOk);
		TestTrue(FString::Printf(TEXT("%s says the caller must name the plane"), Op.OperationName),
			Captured.ErrorMessage.Contains(TEXT("name the plane")));
	}
	return true;
}

// A caller that names a plane must still win over the domain's default, since that is what lets one domain serve
// both planes where an operation genuinely answers about different subjects under the two bearers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCallerPlaneOverridesDefaultTest,
	"CrowdySDK.CrowdyStudio.CallerPlaneOverridesDefault", CrowdyStudioRoutingTestFlags)
bool FCrowdyStudioCallerPlaneOverridesDefaultTest::RunTest(const FString& Parameters)
{
	FCrowdyCppClientConfig ClientConfig;
	ClientConfig.ApiUrl = TEXT("https://game.example.test/graphql");
	ClientConfig.DiscoveryUrl = TEXT("https://api.example.test/graphql");

	const TSharedPtr<FCrowdyCppClient> Client =
		FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, ClientConfig);
	if (!TestTrue(TEXT("test client constructs"), Client.IsValid()))
	{
		return false;
	}
	Client->SetGameToken(TEXT("game-bearer-token"));
	Client->SetManagementToken(TEXT("management-bearer-token"));

	bool bFired = false;
	FCrowdyCppJsonResult Captured;
	// Apps defaults to the management plane; asking for the game plane must move it.
	Client->RunOp(ECrowdyCppApiDomain::Apps, TEXT("MyApps"), MakeShared<FJsonObject>(),
		[&Captured, &bFired](FCrowdyCppJsonResult Result)
		{
			Captured = MoveTemp(Result);
			bFired = true;
		},
		ECrowdyCppTokenPlane::Game);
	if (!bFired)
	{
		Client->Poll();
	}
	if (!TestTrue(TEXT("the override reaches the canned transport"), Captured.bTransportOk))
	{
		return false;
	}

	FString ActualUrl;
	FString ActualAuthorization;
	if (!TestTrue(TEXT("the override recorded a captured request"),
		Client->GetLastTestRequest(ActualUrl, ActualAuthorization)))
	{
		return false;
	}

	TestEqual(TEXT("the override is issued against the configured API origin"), ActualUrl, ClientConfig.ApiUrl);
	TestTrue(TEXT("a named plane overrides the domain default"),
		ActualAuthorization.Contains(TEXT("game-bearer-token")));
	TestFalse(TEXT("the domain's own default bearer did not win"),
		ActualAuthorization.Contains(TEXT("management-bearer-token")));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
