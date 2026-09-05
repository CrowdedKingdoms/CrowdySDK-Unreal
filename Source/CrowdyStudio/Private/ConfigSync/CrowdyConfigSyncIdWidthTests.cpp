#include "ConfigSync/FCrowdyConfigSync.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioTypes.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#include <type_traits>

// App and organization ids are the server's BigInt scalar, so every field that carries one has to be
// 64-bit the whole way from the GraphQL read to the project settings. Org id was once int32, which
// silently wrapped any real id into a negative number and wrote that to DefaultGame.ini. These tests
// pin the widths and pin that a sync carries an out-of-int32-range id through unchanged.
namespace
{
	constexpr EAutomationTestFlags CrowdyConfigSyncIdWidthTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A real id from the platform. Larger than int32 can hold, so a narrowing conversion anywhere on the
	// path turns it into a different (negative) number rather than failing.
	constexpr int64 kBigIntOrgId = 74637546537472;
	constexpr int64 kBigIntAppId = 74637611160064;

	static_assert(std::is_same_v<decltype(UCrowdySDKDeveloperSettings::OrgId), int64>,
		"OrgId is the server's BigInt scalar; a narrower type wraps real ids into negative numbers.");
	static_assert(std::is_same_v<decltype(UCrowdySDKDeveloperSettings::AppID), int64>,
		"AppID is the server's BigInt scalar; a narrower type wraps real ids into negative numbers.");
	static_assert(std::is_same_v<decltype(FStudioSettingsSnapshot::OrgId), int64>,
		"The settings snapshot must carry an org id at full width or the diff shows a wrapped value.");
	static_assert(std::is_same_v<decltype(FStudioApp::OrgId), int64>,
		"The app record must carry an org id at full width.");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConfigSyncPreservesBigIntIdsTest,
	"CrowdySDK.CrowdyStudio.ConfigSyncPreservesBigIntIds", CrowdyConfigSyncIdWidthTestFlags)
bool FCrowdyConfigSyncPreservesBigIntIdsTest::RunTest(const FString& Parameters)
{
	FStudioApp App;
	App.AppId = kBigIntAppId;
	App.OrgId = kBigIntOrgId;

	const FStudioSettingsSnapshot Proposed = FCrowdyConfigSync::BuildProposedSettings(App);

	TestEqual(TEXT("proposed org id matches the app's org id exactly"), Proposed.OrgId, kBigIntOrgId);
	TestEqual(TEXT("proposed app id matches the app's app id exactly"), Proposed.AppId, kBigIntAppId);

	// The specific failure this guards: a 32-bit round trip maps this id to -395132416.
	TestTrue(TEXT("proposed org id is not truncated to 32 bits"),
		Proposed.OrgId != static_cast<int64>(static_cast<int32>(kBigIntOrgId)));

	return true;
}

// A zero or negative id means an incomplete record, and those must never overwrite settings that
// already hold a good value. Widening the field must not have changed that.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConfigSyncKeepsIdsOnEmptyAppTest,
	"CrowdySDK.CrowdyStudio.ConfigSyncKeepsIdsOnEmptyApp", CrowdyConfigSyncIdWidthTestFlags)
bool FCrowdyConfigSyncKeepsIdsOnEmptyAppTest::RunTest(const FString& Parameters)
{
	const FStudioSettingsSnapshot Current = FCrowdyConfigSync::ReadCurrentSettings();

	const FStudioApp Empty;
	const FStudioSettingsSnapshot Proposed = FCrowdyConfigSync::BuildProposedSettings(Empty);

	TestEqual(TEXT("an app with no org id leaves the current org id alone"), Proposed.OrgId, Current.OrgId);
	TestEqual(TEXT("an app with no app id leaves the current app id alone"), Proposed.AppId, Current.AppId);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
