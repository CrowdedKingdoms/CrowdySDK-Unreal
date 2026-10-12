// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/CrowdyStudioTypes.h"
#include "Model/FCrowdyStudioController.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioAppScopedStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAppSwitchClearsAppScopedStateTest,
	"CrowdySDK.CrowdyStudio.AppSwitchClearsAppScopedState", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioAppSwitchClearsAppScopedStateTest::RunTest(const FString& /*Parameters*/)
{
	// A default-constructed controller touches no network, settings or token vault; Initialize does, and is not called.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	FCrowdyStudioControllerTestAccess::PopulateAppScopedState(*Controller);
	TestTrue(TEXT("Team state is populated before the reset"), Controller->GetTeams().Num() > 0);
	TestTrue(TEXT("Grid state is populated before the reset"), Controller->GetNearbyGrids().Num() > 0);
	TestTrue(TEXT("Access state is populated before the reset"), Controller->GetFeatures().Num() > 0);
	TestTrue(TEXT("Access write flag is populated before the reset"), Controller->IsAccessWriteInFlight());

	// The reset is the one signal that always arrives on an app switch: minting the new app's token can fail, and then
	// nothing else announces the change. A view holding access lists only rebuilds if it is told to.
	int32 FeaturesAnnounced = 0;
	Controller->OnFeaturesChanged.AddLambda([&FeaturesAnnounced]() { ++FeaturesAnnounced; });

	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);

	TestEqual(TEXT("Teams cleared"), Controller->GetTeams().Num(), 0);
	TestEqual(TEXT("Channels cleared"), Controller->GetChannels().Num(), 0);
	TestEqual(TEXT("Group members cleared"), Controller->GetGroupMembers().Num(), 0);
	TestEqual(TEXT("Group roles cleared"), Controller->GetGroupRoles().Num(), 0);
	TestEqual(TEXT("Selected group cleared"), Controller->GetSelectedGroupId(), static_cast<int64>(0));
	TestTrue(TEXT("Team policy cleared"), Controller->GetTeamPolicy().CreationPolicy.IsEmpty());
	TestEqual(TEXT("Team policy app cleared"), Controller->GetTeamPolicy().AppId, static_cast<int64>(0));
	TestTrue(TEXT("Channel policy cleared"), Controller->GetChannelPolicy().CreationPolicy.IsEmpty());
	TestEqual(TEXT("Channel policy app cleared"), Controller->GetChannelPolicy().AppId, static_cast<int64>(0));

	TestEqual(TEXT("Nearby grids cleared"), Controller->GetNearbyGrids().Num(), 0);
	TestEqual(TEXT("Grid whitelist keys cleared"), Controller->GetGridWhitelistKeys().Num(), 0);
	TestEqual(TEXT("Grid user effective keys cleared"), Controller->GetGridUserEffectiveKeys().Num(), 0);
	TestEqual(TEXT("Grid group grants cleared"), Controller->GetGridGroupGrants().Num(), 0);

	TestEqual(TEXT("Features cleared"), Controller->GetFeatures().Num(), 0);
	TestEqual(TEXT("Tier features cleared"), Controller->GetTierFeatures().Num(), 0);
	TestEqual(TEXT("Access tiers cleared"), Controller->GetAccessTiers().Num(), 0);
	TestEqual(TEXT("Access views are told to rebuild"), FeaturesAnnounced, 1);
	// The three lists above are empty, so the mark saying which app they were requested for has to go with them, or
	// the Access page reads "already loaded" for the new app and shows empty lists forever.
	TestEqual(TEXT("Access list load mark cleared"),
		FCrowdyStudioControllerTestAccess::GetAccessListsAppId(*Controller), static_cast<int64>(0));
	// A reply for the previous app drops its callbacks, so nothing else would ever release these.
	TestEqual(TEXT("Access reads in flight cleared"),
		FCrowdyStudioControllerTestAccess::GetAccessReadsInFlight(*Controller), 0);
	TestEqual(TEXT("Access writes in flight cleared"),
		FCrowdyStudioControllerTestAccess::GetAccessWritesInFlight(*Controller), 0);
	TestFalse(TEXT("The write flag reads clear"), Controller->IsAccessWriteInFlight());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
