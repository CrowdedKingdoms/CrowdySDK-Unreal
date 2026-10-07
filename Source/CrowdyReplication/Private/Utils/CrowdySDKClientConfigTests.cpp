#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKClientConfigFallsBackToDiscoveryTest,
	"CrowdySDK.CrowdyReplication.ClientConfigFallsBackToDiscovery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKClientConfigFallsBackToDiscoveryTest::RunTest(const FString& Parameters)
{
	UCrowdySDKDeveloperSettings* const Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const FString OriginalGameApiHttpUrl = Settings->GameApiHttpUrl;
	ON_SCOPE_EXIT
	{
		Settings->GameApiHttpUrl = OriginalGameApiHttpUrl;
	};

	const FString DiscoveryUrl = Settings->GetDiscoveryUrl();
	if (!TestFalse(TEXT("CONTROL: the settings name a shared origin to fall back to"), DiscoveryUrl.IsEmpty()))
	{
		return false;
	}

	Settings->GameApiHttpUrl.Reset();
	const FCrowdyCppClientConfig Unresolved = Settings->MakeClientConfig();
	TestEqual(TEXT("The shared origin is the discovery URL"), Unresolved.DiscoveryUrl, DiscoveryUrl);
	TestEqual(TEXT("With no game endpoint yet, the API URL falls back to the shared origin"),
		Unresolved.ApiUrl, DiscoveryUrl);

	const FString GameApiUrl = TEXT("https://game.example.test/graphql");
	Settings->GameApiHttpUrl = GameApiUrl;
	const FCrowdyCppClientConfig Resolved = Settings->MakeClientConfig();
	TestEqual(TEXT("A resolved game endpoint is used as the API URL"), Resolved.ApiUrl, GameApiUrl);
	TestEqual(TEXT("and does not replace the shared origin"), Resolved.DiscoveryUrl, DiscoveryUrl);

	return true;
}

#endif
