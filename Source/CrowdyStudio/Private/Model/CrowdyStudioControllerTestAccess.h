// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "Model/CrowdyStudioTypes.h"
#include "Model/FCrowdyStudioController.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"

// Reaches the controller's app-scoped reset and its sign-in and transport state, which are internal because no view
// may drive them directly. Declared a friend by the controller, so it must stay at global scope and keep this exact
// name.
//
// It lives in a header rather than in one test .cpp because two suites need it. A second copy in another translation
// unit would be merged with this one by the unity build and redefine it, so there is exactly one home.
struct FCrowdyStudioControllerTestAccess
{
	// Fill EVERY member the app-scoped reset touches with something recognizable, so a reset line that goes missing
	// leaves a value behind for the assertions below to catch. Anything added to ClearAppScopedState belongs here
	// too, or that line is free to be deleted with the suite still green.
	static void PopulateAppScopedState(FCrowdyStudioController& Controller)
	{
		const TSharedPtr<FStudioGroup> Team = MakeShared<FStudioGroup>();
		Team->GroupId = 11;
		Team->Name = TEXT("Founders");
		Controller.Teams.Add(Team);

		const TSharedPtr<FStudioGroup> Channel = MakeShared<FStudioGroup>();
		Channel->GroupId = 12;
		Channel->Name = TEXT("__crowdy_session_1");
		Controller.Channels.Add(Channel);

		const TSharedPtr<FStudioGroupMember> Member = MakeShared<FStudioGroupMember>();
		Member->UserId = 501;
		Controller.GroupMembers.Add(Member);

		const TSharedPtr<FStudioGroupRole> Role = MakeShared<FStudioGroupRole>();
		Role->RoleName = TEXT("leader");
		Controller.GroupRoles.Add(Role);

		Controller.SelectedGroupId = 11;

		Controller.TeamPolicy.AppId = 1;
		Controller.TeamPolicy.CreationPolicy = TEXT("admin");
		Controller.ChannelPolicy.AppId = 1;
		Controller.ChannelPolicy.CreationPolicy = TEXT("member");

		const TSharedPtr<FStudioGrid> Grid = MakeShared<FStudioGrid>();
		Grid->GridId = 21;
		Controller.NearbyGrids.Add(Grid);
		Controller.GridWhitelistKeys.Add(TEXT("voxel.place"));
		Controller.GridUserEffectiveKeys.Add(TEXT("voxel.break"));

		const TSharedPtr<FStudioGridGroupGrant> Grant = MakeShared<FStudioGridGroupGrant>();
		Grant->GridId = 21;
		Grant->GroupId = 11;
		Controller.GridGroupGrants.Add(Grant);

		const TSharedPtr<FStudioAppFeature> Feature = MakeShared<FStudioAppFeature>();
		Feature->FeatureKey = TEXT("combat");
		Controller.Features.Add(Feature);

		const TSharedPtr<FStudioTierFeature> TierFeature = MakeShared<FStudioTierFeature>();
		TierFeature->TierId = 7;
		TierFeature->FeatureKey = TEXT("combat");
		Controller.TierFeatures.Add(TierFeature);

		const TSharedPtr<FStudioAccessTier> Tier = MakeShared<FStudioAccessTier>();
		Tier->TierId = 7;
		Tier->Name = TEXT("Founder");
		Controller.AccessTiers.Add(Tier);

		// The mark saying the three lists above were requested for some app. Left set across a switch, the page would
		// believe the new app was already loaded and never fill itself.
		Controller.AccessListsAppId = 4242;

		// Replies for an app that was switched away from drop both callbacks, so these are zeroed by the reset, not by a release.
		Controller.AccessReadsInFlight = 2;
		Controller.AccessWritesInFlight = 1;
	}

	static int32 GetAccessReadsInFlight(const FCrowdyStudioController& Controller)
	{
		return Controller.AccessReadsInFlight;
	}

	static int32 GetAccessWritesInFlight(const FCrowdyStudioController& Controller)
	{
		return Controller.AccessWritesInFlight;
	}

	static void ClearAppScopedState(FCrowdyStudioController& Controller)
	{
		Controller.ClearAppScopedState();
	}

	static int64 GetAccessListsAppId(const FCrowdyStudioController& Controller)
	{
		return Controller.AccessListsAppId;
	}

	static void SetSignedIn(FCrowdyStudioController& Controller)
	{
		Controller.bSignedIn = true;
	}

	static void SetOrgTokenScope(FCrowdyStudioController& Controller)
	{
		Controller.AuthScope = ECrowdyStudioAuthScope::OrgToken;
	}

	// Stands the controller up as if an app were selected, without SelectApp's settings write, token mint or reads.
	static void SetSelectedApp(FCrowdyStudioController& Controller, int64 AppId)
	{
		Controller.SelectedAppId = AppId;
	}

	// Issue console operations on Client, with SessionToken as the management bearer and AppToken as the game one.
	static void InstallApiClient(FCrowdyStudioController& Controller, const TSharedRef<FCrowdyCppClient>& Client,
		const FString& SessionToken, const FString& AppToken)
	{
		Controller.ApiClientHost = FCrowdyCppAdminClientHost::CreateForTest(Client);
		Controller.ApiClientDiscoveryUrl = Controller.ResolveDiscoveryUrl();
		Controller.AuthToken = SessionToken;
		Controller.GameAppToken = AppToken;
		Controller.GameApiUrlOverride = TEXT("https://game.example.test/graphql");
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
