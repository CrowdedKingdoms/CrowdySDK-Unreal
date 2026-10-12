// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The Access page's features and grants are Game API fields under the app bearer; its tier list is a management read
// under the session bearer.
namespace CrowdyStudioTierFeatureRoutingTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FSentRequest
	{
		FString Body;
		FString OperationName;
		FString Authorization;
	};

	FString ReadOperationName(const FString& Body)
	{
		TSharedPtr<FJsonObject> Request;
		FString Name;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request) && Request.IsValid())
		{
			Request->TryGetStringField(TEXT("operationName"), Name);
		}
		return Name;
	}

	void DrainClient(const TSharedPtr<FCrowdyCppClient>& Client)
	{
		for (int32 Pass = 0; Pass < 6; ++Pass)
		{
			Client->Poll();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioTierFeaturesBearTheAppTokenTest,
	"CrowdySDK.CrowdyStudio.TierFeaturesBearTheAppToken", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioTierFeaturesBearTheAppTokenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const FString SessionBearer = TEXT("studio-session-bearer");
	const FString AppBearer = TEXT("studio-app-bearer");

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	TArray<FSentRequest> Sent;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &Sent](const FString&)
	{
		FSentRequest& Request = Sent.AddDefaulted_GetRef();
		FString Url;
		RawClient->GetLastTestRequest(Url, Request.Authorization);
		RawClient->GetLastTestRequestBody(Request.Body);
		Request.OperationName = ReadOperationName(Request.Body);
	});

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), SessionBearer, AppBearer);

	Controller->FetchFeatures();
	Client->Poll();
	Controller->GrantTierFeature(7, TEXT("combat"));
	Client->Poll();
	Client->Poll();
	Controller->FetchAppAccessTiers();
	Client->Poll();

	// The grant's success re-reads the grants, so four requests went out in this order.
	if (!TestEqual(TEXT("four requests were sent"), Sent.Num(), 4))
	{
		Client->SetTestOnRequest(nullptr);
		return false;
	}
	TestTrue(TEXT("the feature list is the AppFeatures query"),
		Sent[0].OperationName.Equals(TEXT("AppFeatures"), ESearchCase::CaseSensitive)
		&& Sent[0].Body.Contains(TEXT("query AppFeatures"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the grant is the GrantTierFeature mutation"),
		Sent[1].OperationName.Equals(TEXT("GrantTierFeature"), ESearchCase::CaseSensitive)
		&& Sent[1].Body.Contains(TEXT("mutation GrantTierFeature"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the re-read is the TierFeatures query"),
		Sent[2].OperationName.Equals(TEXT("TierFeatures"), ESearchCase::CaseSensitive)
		&& Sent[2].Body.Contains(TEXT("query TierFeatures"), ESearchCase::CaseSensitive));

	TestTrue(TEXT("the tier list is the AppAccessTiers query"),
		Sent[3].OperationName.Equals(TEXT("AppAccessTiers"), ESearchCase::CaseSensitive));

	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestTrue(FString::Printf(TEXT("request %d carries the app bearer"), Index),
			Sent[Index].Authorization.Contains(AppBearer, ESearchCase::CaseSensitive));
		TestFalse(FString::Printf(TEXT("request %d does not carry the session bearer"), Index),
			Sent[Index].Authorization.Contains(SessionBearer, ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("the tier list carries the session bearer"), Sent[3].Authorization.Contains(SessionBearer, ESearchCase::CaseSensitive));
	TestFalse(TEXT("the tier list does not carry the app bearer"), Sent[3].Authorization.Contains(AppBearer, ESearchCase::CaseSensitive));

	Client->SetTestOnRequest(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAccessListsLoadOncePerAppTest,
	"CrowdySDK.CrowdyStudio.AccessListsLoadOncePerApp", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioAccessListsLoadOncePerAppTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	TArray<FString> Sent;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &Sent](const FString&)
	{
		FString Body;
		RawClient->GetLastTestRequestBody(Body);
		Sent.Add(ReadOperationName(Body));
	});

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("studio-session-bearer"), TEXT("studio-app-bearer"));

	auto Drain = [&Client]()
	{
		for (int32 Pass = 0; Pass < 6; ++Pass)
		{
			Client->Poll();
		}
	};

	Controller->EnsureAccessListsLoaded();
	Drain();
	TestEqual(TEXT("signed out, the page asks for nothing"), Sent.Num(), 0);

	FCrowdyStudioControllerTestAccess::SetSignedIn(*Controller);
	Controller->EnsureAccessListsLoaded();
	Drain();
	if (!TestEqual(TEXT("signed in, the three lists are requested"), Sent.Num(), 3))
	{
		Client->SetTestOnRequest(nullptr);
		return false;
	}
	TestTrue(TEXT("the features are one of them"), Sent.Contains(TEXT("AppFeatures")));
	TestTrue(TEXT("the tier-feature grants are one of them"), Sent.Contains(TEXT("TierFeatures")));
	TestTrue(TEXT("the access tiers are one of them"), Sent.Contains(TEXT("AppAccessTiers")));

	Controller->EnsureAccessListsLoaded();
	Drain();
	TestEqual(TEXT("a second paint of the same app issues nothing"), Sent.Num(), 3);

	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);
	Controller->EnsureAccessListsLoaded();
	Drain();
	TestEqual(TEXT("an app switch makes the page owe its lists again"), Sent.Num(), 6);

	Client->SetTestOnRequest(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioOrgTokenReadsFeaturesOnTheManagementBearerTest,
	"CrowdySDK.CrowdyStudio.OrgTokenReadsFeaturesOnTheManagementBearer", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioOrgTokenReadsFeaturesOnTheManagementBearerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const FString OrgBearer = TEXT("studio-org-bearer");

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	TArray<FSentRequest> Sent;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &Sent](const FString&)
	{
		FSentRequest& Request = Sent.AddDefaulted_GetRef();
		FString Url;
		RawClient->GetLastTestRequest(Url, Request.Authorization);
		RawClient->GetLastTestRequestBody(Request.Body);
		Request.OperationName = ReadOperationName(Request.Body);
	});

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::SetOrgTokenScope(*Controller);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), OrgBearer, FString());

	TestFalse(TEXT("an org token with no app token cannot use the game plane"), Controller->CanUseGamePlane());
	TestTrue(TEXT("an org token can use the app access operations"), Controller->CanUseAppAccess());

	Controller->FetchFeatures();
	DrainClient(Client);
	Controller->GrantTierFeature(7, TEXT("combat"));
	DrainClient(Client);

	if (!TestTrue(TEXT("the feature read and the grant were sent"), Sent.Num() >= 2))
	{
		Client->SetTestOnRequest(nullptr);
		return false;
	}
	TestEqual(TEXT("the first request is the AppFeatures query"), Sent[0].OperationName, FString(TEXT("AppFeatures")));
	TestEqual(TEXT("the second request is the GrantTierFeature mutation"), Sent[1].OperationName, FString(TEXT("GrantTierFeature")));
	for (int32 Index = 0; Index < Sent.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("request %d carries the org bearer"), Index),
			Sent[Index].Authorization.Contains(OrgBearer, ESearchCase::CaseSensitive));
	}
	TestFalse(TEXT("the replies clear the write flag"), Controller->IsAccessWriteInFlight());

	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("session-token"), TEXT("app-token"));
	TestTrue(TEXT("a held app token makes the game plane usable"), Controller->CanUseGamePlane());

	Client->SetTestOnRequest(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAccessWriteFlagTest,
	"CrowdySDK.CrowdyStudio.AccessWriteFlag", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioAccessWriteFlagTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("studio-session-bearer"), TEXT("studio-app-bearer"));

	TestFalse(TEXT("no write is in flight to begin with"), Controller->IsAccessWriteInFlight());

	Controller->GrantTierFeature(7, TEXT("combat"));
	TestTrue(TEXT("the grant sets the write flag"), Controller->IsAccessWriteInFlight());

	DrainClient(Client);
	TestFalse(TEXT("the reply clears the write flag"), Controller->IsAccessWriteInFlight());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAccessListsLoadingSignalTest,
	"CrowdySDK.CrowdyStudio.AccessListsLoadingSignal", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioAccessListsLoadingSignalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("studio-session-bearer"), TEXT("studio-app-bearer"));
	FCrowdyStudioControllerTestAccess::SetSignedIn(*Controller);

	TestTrue(TEXT("before the lists are requested the page reads as loading"), Controller->AreAccessListsLoading());

	Controller->EnsureAccessListsLoaded();
	TestTrue(TEXT("right after the reads are issued the page reads as loading"), Controller->AreAccessListsLoading());

	DrainClient(Client);
	TestFalse(TEXT("once every reply has landed the page is done loading"), Controller->AreAccessListsLoading());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDefineFeatureKeyShapeTest,
	"CrowdySDK.CrowdyStudio.DefineFeatureKeyShape", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioDefineFeatureKeyShapeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioTierFeatureRoutingTests;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	TArray<FString> Sent;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &Sent](const FString&)
	{
		FString Body;
		RawClient->GetLastTestRequestBody(Body);
		Sent.Add(ReadOperationName(Body));
	});

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("studio-session-bearer"), TEXT("studio-app-bearer"));

	Controller->DefineFeature(TEXT("VIP Pass"), FString());
	Controller->DefineFeature(FString::ChrN(65, TEXT('a')), FString());
	DrainClient(Client);
	TestEqual(TEXT("a key no Server Object could require is not sent"), Sent.Num(), 0);

	Controller->DefineFeature(TEXT("vip.pass-2_b"), FString());
	DrainClient(Client);
	TestTrue(TEXT("a key of letters, digits, underscores, hyphens and dots is defined"), Sent.Num() > 0 && Sent[0].Equals(TEXT("DefineAppFeature"), ESearchCase::CaseSensitive));

	Client->SetTestOnRequest(nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
