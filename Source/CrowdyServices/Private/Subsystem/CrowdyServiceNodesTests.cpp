#include "CrowdyServiceNodesTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServiceNodesTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using UListener = UCrowdyServiceNodeTestListener;

	/** Activates a node bound as a Blueprint graph binds it, then checks it failed exactly once, never succeeded and let itself go. */
	template <typename NodeType>
	void ExpectFailsOnce(FAutomationTestBase& Test, const TCHAR* Name, TFunctionRef<void(NodeType&, UListener*, UListener*)> Bind)
	{
		const TStrongObjectPtr<NodeType> Node(NewObject<NodeType>());
		const TStrongObjectPtr<UListener> Done(NewObject<UListener>());
		const TStrongObjectPtr<UListener> Failed(NewObject<UListener>());
		Bind(*Node, Done.Get(), Failed.Get());
		Test.TestTrue(FString::Printf(TEXT("%s is held until it finishes"), Name), Node->HasAnyFlags(RF_StrongRefOnFrame));

		Node->Activate();
		for (int32 Tick = 0; Tick < 4; ++Tick)
		{
			FTSTicker::GetCoreTicker().Tick(0.f);
		}

		Test.TestEqual(FString::Printf(TEXT("%s fires On Failed exactly once"), Name), Failed->Calls, 1);
		Test.TestEqual(FString::Printf(TEXT("%s never fires On Done"), Name), Done->Calls, 0);
		Test.TestFalse(FString::Printf(TEXT("%s says why in words"), Name), Failed->LastMessage.IsEmpty());
		Test.TestFalse(FString::Printf(TEXT("%s is ready to destroy"), Name), Node->HasAnyFlags(RF_StrongRefOnFrame));
	}
}

using namespace CrowdyServiceNodesTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioNodesFailOnceTest, "CrowdySDK.StudioActions.EveryNodeFailsOnceAndFinishes", Flags)
bool FCrowdyStudioNodesFailOnceTest::RunTest(const FString& Parameters)
{
	ExpectFailsOnce<UCrowdyGetAgentBoot>(*this, TEXT("Get Agent Boot"), [](UCrowdyGetAgentBoot& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleBoot);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleBoot);
	});
	ExpectFailsOnce<UCrowdyGetProviderConsent>(*this, TEXT("Get Provider Consent"), [](UCrowdyGetProviderConsent& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleConsent);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleConsent);
	});
	ExpectFailsOnce<UCrowdyRecordProviderConsent>(*this, TEXT("Record Provider Consent"), [](UCrowdyRecordProviderConsent& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleConsent);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleConsent);
	});
	ExpectFailsOnce<UCrowdyGetModelUsage>(*this, TEXT("Get Model Usage"), [](UCrowdyGetModelUsage& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleUsage);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleUsage);
	});
	ExpectFailsOnce<UCrowdyAuthorizeLiveModding>(*this, TEXT("Authorize Live Modding"), [](UCrowdyAuthorizeLiveModding& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleLiveModding);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleLiveModding);
	});
	ExpectFailsOnce<UCrowdyListStudioProjects>(*this, TEXT("List Studio Projects"), [](UCrowdyListStudioProjects& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleProjectList);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleProjectList);
	});
	ExpectFailsOnce<UCrowdyGetStudioProject>(*this, TEXT("Get Studio Project"), [](UCrowdyGetStudioProject& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleProject);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleProject);
	});
	ExpectFailsOnce<UCrowdyCreateStudioProject>(*this, TEXT("Create Studio Project"), [](UCrowdyCreateStudioProject& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleProject);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleProject);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsNodesFailOnceTest, "CrowdySDK.ModScriptsActions.EveryNodeFailsOnceAndFinishes", Flags)
bool FCrowdyModScriptsNodesFailOnceTest::RunTest(const FString& Parameters)
{
	ExpectFailsOnce<UCrowdyGetScriptStatus>(*this, TEXT("Get Script Status"), [](UCrowdyGetScriptStatus& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleScriptStatus);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleScriptStatus);
	});
	ExpectFailsOnce<UCrowdyGetScriptLog>(*this, TEXT("Get Script Log"), [](UCrowdyGetScriptLog& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleScriptLog);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleScriptLog);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsStudioNodesFailOnceTest, "CrowdySDK.ModsActions.StudioNodesFailOnceAndFinish", Flags)
bool FCrowdyModsStudioNodesFailOnceTest::RunTest(const FString& Parameters)
{
	ExpectFailsOnce<UCrowdyBuildClientMod>(*this, TEXT("Build Client Mod"), [](UCrowdyBuildClientMod& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleBuild);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleBuild);
	});
	ExpectFailsOnce<UCrowdyDeployClientMod>(*this, TEXT("Deploy Client Mod"), [](UCrowdyDeployClientMod& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleClientInfo);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleClientInfo);
	});
	ExpectFailsOnce<UCrowdyRunProject>(*this, TEXT("Run Project"), [](UCrowdyRunProject& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleRun);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleRun);
	});
	ExpectFailsOnce<UCrowdyGetProjectStatus>(*this, TEXT("Get Project Status"), [](UCrowdyGetProjectStatus& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleProjectStatus);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleProjectStatus);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsGridChannelsNodeFailsOnceTest, "CrowdySDK.ClaimsActions.ListGridChannelsFailsOnceAndFinishes", Flags)
bool FCrowdyClaimsGridChannelsNodeFailsOnceTest::RunTest(const FString& Parameters)
{
	ExpectFailsOnce<UCrowdyListGridChannels>(*this, TEXT("List Grid Channels"), [](UCrowdyListGridChannels& Node, UListener* Done, UListener* Failed)
	{
		Node.OnDone.AddDynamic(Done, &UListener::HandleGridChannels);
		Node.OnFailed.AddDynamic(Failed, &UListener::HandleGridChannels);
	});
	return true;
}

#endif
