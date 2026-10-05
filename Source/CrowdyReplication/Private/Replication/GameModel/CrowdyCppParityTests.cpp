#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppBridge.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "crowdy/default_origin.hpp"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCppParityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The vendored library's linkage and build configuration, checked the same way the crowdy.cpp.selftest console
// command checks it: whether the crypto provider is reachable, whether RTTI survived the build settings, and whether
// the generated operation tables resolve. Each of those can fail silently behind a green compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBridgeSelfTestTest,
	"CrowdySDK.CrowdyCpp.BridgeSelfTest", CrowdyCppParityTestFlags)
bool FCrowdyCppBridgeSelfTestTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the vendored CrowdyCPP build is linked and correctly configured"),
		FCrowdyCppBridge::SelfTest());
	return true;
}

// An operation name the library does not know must fail without a round trip rather than reaching the server as an
// empty document.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppUnknownOperationTest,
	"CrowdySDK.CrowdyCpp.RunOpUnknownOperation", CrowdyCppParityTestFlags)
bool FCrowdyCppUnknownOperationTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 Delivered = 0;
	FCrowdyCppJsonResult Captured;
	Client->RunOp(ECrowdyCppApiDomain::Teams, TEXT("NoSuchOperation"), MakeShared<FJsonObject>(),
		[&Delivered, &Captured](FCrowdyCppJsonResult Result)
		{
			++Delivered;
			Captured = MoveTemp(Result);
		});

	FString Url;
	FString Authorization;
	TestEqual(TEXT("answered before any poll"), Delivered, 1);
	TestFalse(TEXT("an unknown operation fails"), Captured.bTransportOk);
	TestTrue(TEXT("the failure names the operation that could not be resolved"),
		Captured.ErrorMessage.Contains(TEXT("NoSuchOperation")));
	TestFalse(TEXT("no request was sent"), Client->GetLastTestRequest(Url, Authorization));
	return true;
}

// The vendored CrowdyCPP library is compiled from source, so its behaviour is whatever that snapshot does, pinned to
// the version recorded in VENDOR.txt. A version bump must be a deliberate, reviewed edit to the constant below rather
// than something that rides along silently in a sync script's output.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVendoredVersionTest,
	"CrowdySDK.CrowdyCpp.VendoredVersion", CrowdyCppParityTestFlags)
bool FCrowdyCppVendoredVersionTest::RunTest(const FString& Parameters)
{
	static const FString ExpectedVendoredCrowdyCppVersion = TEXT("0.56.0");

	// Each SDK branch vendors its own tier, so the tier VENDOR.txt records must be the one actually compiled in.
	const FString CompiledCrowdyCppTier = UTF8_TO_TCHAR(crowdy::kDefaultTier);

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
	if (!TestTrue(TEXT("the CrowdySDK plugin is found"), Plugin.IsValid()))
	{
		return false;
	}

	const FString VendorFilePath = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Source"), TEXT("CrowdyCppBridge"), TEXT("ThirdParty"), TEXT("CrowdyCPP"), TEXT("VENDOR.txt"));
	FString VendorFileContents;
	if (!TestTrue(TEXT("VENDOR.txt is present under the vendored CrowdyCPP directory"),
		FFileHelper::LoadFileToString(VendorFileContents, *VendorFilePath)))
	{
		return false;
	}

	TArray<FString> Lines;
	VendorFileContents.ParseIntoArrayLines(Lines);
	FString ParsedVersion;
	FString ParsedTier;
	for (const FString& Line : Lines)
	{
		const FString TrimmedLine = Line.TrimStartAndEnd();
		if (ParsedVersion.IsEmpty() && TrimmedLine.StartsWith(TEXT("version:")))
		{
			ParsedVersion = TrimmedLine.Mid(8).TrimStartAndEnd();
		}
		else if (ParsedTier.IsEmpty() && TrimmedLine.StartsWith(TEXT("tier:")))
		{
			// Recorded as "<name> (<origin>)", and only the name is pinned here.
			ParsedTier = TrimmedLine.Mid(5).TrimStartAndEnd();
			int32 SpaceIndex = INDEX_NONE;
			if (ParsedTier.FindChar(TEXT(' '), SpaceIndex))
			{
				ParsedTier = ParsedTier.Left(SpaceIndex);
			}
		}
	}

	if (!TestTrue(TEXT("VENDOR.txt has a version: line"), !ParsedVersion.IsEmpty()))
	{
		return false;
	}

	TestEqual(TEXT("the vendored CrowdyCPP version matches the pinned expectation"),
		ParsedVersion, ExpectedVendoredCrowdyCppVersion);

	if (TestTrue(TEXT("VENDOR.txt has a tier: line"), !ParsedTier.IsEmpty()))
	{
		TestEqual(TEXT("the tier VENDOR.txt records is the tier compiled in"),
			ParsedTier, CompiledCrowdyCppTier);
	}
	return true;
}

// A project that never picks a Backend gets the tier this build was released for, and that Backend's built-in
// host must be the origin the vendored CrowdyCPP was generated for, or the SDK and the library disagree on it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReleaseBackendIsVendoredOriginTest,
	"CrowdySDK.CrowdyCpp.ReleaseBackendIsVendoredOrigin", CrowdyCppParityTestFlags)
bool FCrowdyReleaseBackendIsVendoredOriginTest::RunTest(const FString& Parameters)
{
	const ECrowdyEnvironment Release = UCrowdySDKDeveloperSettings::GetReleaseEnvironment();
	if (!TestTrue(TEXT("the vendored tier names a built-in Backend"), Release != ECrowdyEnvironment::Custom))
	{
		return false;
	}

	UCrowdySDKDeveloperSettings* Settings = NewObject<UCrowdySDKDeveloperSettings>(GetTransientPackage());
	Settings->Environment = Release;
	TestEqual(TEXT("the release Backend's host is the vendored default origin"),
		Settings->GetDiscoveryUrl(), FString(UTF8_TO_TCHAR(crowdy::kDefaultHttpOrigin)));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
