// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The tier-feature pages are served by the AppAccess operations under the session bearer, not by Game Model calls,
// which would send nothing at all.
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioTierFeaturesUseAppAccessTest,
	"CrowdySDK.CrowdyStudio.TierFeaturesUseAppAccess", CrowdyStudioTierFeatureRoutingTests::TestFlags)
bool FCrowdyStudioTierFeaturesUseAppAccessTest::RunTest(const FString& Parameters)
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

	// The grant's success re-reads the grants, so three requests went out in this order.
	if (!TestEqual(TEXT("three requests were sent"), Sent.Num(), 3))
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

	for (int32 Index = 0; Index < Sent.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("request %d names no Game Model operation"), Index),
			Sent[Index].Body.Contains(TEXT("GameModel"), ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("request %d carries the session bearer"), Index),
			Sent[Index].Authorization.Contains(SessionBearer, ESearchCase::CaseSensitive));
		TestFalse(FString::Printf(TEXT("request %d does not carry the app bearer"), Index),
			Sent[Index].Authorization.Contains(AppBearer, ESearchCase::CaseSensitive));
	}

	Client->SetTestOnRequest(nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
