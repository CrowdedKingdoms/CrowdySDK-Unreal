// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyStudioSyncService.h"
#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/FCrowdyStudioController.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Kit/CrowdyGameKitConfig.h"
#include "UObject/Package.h"

// Game Models are deprecated, so the console refuses every Game Model operation itself, at once and without minting
// an app token for it, while the other game-plane domains keep their normal path.
namespace CrowdyStudioGameModelDeprecationTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	bool IsDeprecatedMessage(const FString& Message)
	{
		return Message.Equals(CrowdyCppGameModelDeprecatedMessage, ESearchCase::CaseSensitive);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioGameModelOpRefusedWithoutMintTest,
	"CrowdySDK.CrowdyStudio.GameModelOpRefusedWithoutMint", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioGameModelOpRefusedWithoutMintTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 Sent = 0;
	Client->SetTestOnRequest([&Sent](const FString&) { ++Sent; });

	// A session sign-in with no app token yet, so any game-plane operation that reaches the token check mints first.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(),
		TEXT("studio-session-bearer"), FString());
	FCrowdyStudioControllerTestAccess::SetSessionSignIn(*Controller);

	int32 Refusals = 0;
	Controller->OnStatusMessage.AddLambda([&Refusals](const FString& Message, bool bIsError)
	{
		if (bIsError && IsDeprecatedMessage(Message))
		{
			++Refusals;
		}
	});

	Controller->FetchContainerTypes();

	TestEqual(TEXT("the Game Model read is refused exactly once"), Refusals, 1);
	TestTrue(TEXT("the status carries the deprecation message"), IsDeprecatedMessage(Controller->GetStatusMessage()));
	TestTrue(TEXT("as an error"), Controller->LastStatusWasError());
	TestEqual(TEXT("the read failed before the call returned"),
		static_cast<int32>(FCrowdyStudioControllerTestAccess::GetFamilyLoad(*Controller, ECrowdyModelFamily::Models).State),
		static_cast<int32>(ECrowdyModelLoadState::Failed));

	Client->Poll();
	TestEqual(TEXT("nothing was sent for it, not even a token mint"), Sent, 0);

	// The refusal is scoped to the deprecated domains: a team read still takes the token path and mints.
	Controller->FetchTeams();
	Client->Poll();
	TestTrue(TEXT("a Teams read still mints its app token"), Sent > 0);
	TestEqual(TEXT("and is not refused as deprecated"), Refusals, 1);

	Client->SetTestOnRequest(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioGameModelListsNeverLoadTest,
	"CrowdySDK.CrowdyStudio.GameModelListsNeverLoad", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioGameModelListsNeverLoadTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);

	Controller->EnsureGameModelListsLoaded();

	TestEqual(TEXT("the lists are not marked as loaded for the app"),
		FCrowdyStudioControllerTestAccess::GetGameModelListsAppId(*Controller), static_cast<int64>(0));
	TestEqual(TEXT("no model read was issued"),
		static_cast<int32>(FCrowdyStudioControllerTestAccess::GetFamilyLoad(*Controller, ECrowdyModelFamily::Models).State),
		static_cast<int32>(ECrowdyModelLoadState::NeverRequested));
	TestTrue(TEXT("and nothing was reported"), Controller->GetStatusMessage().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioSchemaPlanRefusedAsDeprecatedTest,
	"CrowdySDK.CrowdyStudio.SchemaPlanRefusedAsDeprecated", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioSchemaPlanRefusedAsDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	// A container stream already running for this app, so a plan that got past the refusal stops at its own guard
	// with a message of its own instead of streaming assets.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	FCrowdyStudioControllerTestAccess::SetSchemaContainerLoadInFlight(*Controller, 42);

	Controller->PlanSchemaSync();

	TestTrue(TEXT("the plan is refused as deprecated"), IsDeprecatedMessage(Controller->GetStatusMessage()));
	TestTrue(TEXT("no plan is reported as running"), Controller->GetSchemaPlanPhase().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPreSeedPlanRefusedAsDeprecatedTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanRefusedAsDeprecated", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioPreSeedPlanRefusedAsDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	const uint64 GenerationBefore = Controller->GetPreSeedPlanGeneration();

	Controller->PlanPreSeed(FString());

	TestTrue(TEXT("the pre-seed plan is refused as deprecated"), IsDeprecatedMessage(Controller->GetStatusMessage()));
	TestEqual(TEXT("no plan was started"), Controller->GetPreSeedPlanGeneration(), GenerationBefore);
	TestFalse(TEXT("nothing is left busy"), Controller->IsPreSeedBusy());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerPurgeRefusedAsDeprecatedTest,
	"CrowdySDK.CrowdyStudio.ContainerPurgeRefusedAsDeprecated", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioContainerPurgeRefusedAsDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Finished = 0;
	Controller->OnContainerPurgeFinished.AddLambda([&Finished]() { ++Finished; });

	Controller->PurgeContainers(FString(), /*ExpectedAppId*/ 100);

	TestEqual(TEXT("the refusal is announced exactly once"), Finished, 1);
	TestTrue(TEXT("the status carries the deprecation message, not a stopped purge"),
		IsDeprecatedMessage(Controller->GetStatusMessage()));
	TestFalse(TEXT("nothing is running"), Controller->IsContainerPurgeInFlight());
	TestTrue(TEXT("the outcome is a stop, not a clean sweep"), Controller->GetLastContainerPurgeOutcome().bStopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioKitDeployRefusedAsDeprecatedTest,
	"CrowdySDK.CrowdyStudio.KitDeployRefusedAsDeprecated", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioKitDeployRefusedAsDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 42);
	const UCrowdyGameKitConfig* Config = NewObject<UCrowdyGameKitConfig>(GetTransientPackage());

	int32 Answers = 0;
	bool bDeployOk = true;
	FString DeployMessage;
	Controller->DeployGameKit(Config, [&Answers, &bDeployOk, &DeployMessage](bool bOk, const FString& Message)
	{
		++Answers;
		bDeployOk = bOk;
		DeployMessage = Message;
	});

	TestEqual(TEXT("the deploy is answered at once, exactly once"), Answers, 1);
	TestFalse(TEXT("as a failure"), bDeployOk);
	TestTrue(TEXT("refused as deprecated rather than for want of a sign-in"), IsDeprecatedMessage(DeployMessage));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioEffectSyncAnswersDeprecatedTest,
	"CrowdySDK.CrowdyStudio.EffectSyncAnswersDeprecated", CrowdyStudioGameModelDeprecationTests::TestFlags)
bool FCrowdyStudioEffectSyncAnswersDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioGameModelDeprecationTests;

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());

	int32 StatusAnswers = 0;
	ECrowdyEffectSyncStatus Status = ECrowdyEffectSyncStatus::Unknown;
	FString StatusMessage;
	auto OnStatus = [&StatusAnswers, &Status, &StatusMessage](ECrowdyEffectSyncStatus InStatus, const FString& Message)
	{
		++StatusAnswers;
		Status = InStatus;
		StatusMessage = Message;
	};

	CrowdyStudioSyncService::RequestEffectSyncStatus(Effect, OnStatus);

	TestEqual(TEXT("the status request is answered at once, exactly once"), StatusAnswers, 1);
	TestEqual(TEXT("as deprecated"), static_cast<int32>(Status), static_cast<int32>(ECrowdyEffectSyncStatus::Deprecated));
	TestTrue(TEXT("with the deprecation message"), IsDeprecatedMessage(StatusMessage));
	TestEqual(TEXT("and the answer is cached, so the toolbar never asks again"),
		static_cast<int32>(CrowdyStudioSyncService::GetCachedStatus(Effect)),
		static_cast<int32>(ECrowdyEffectSyncStatus::Deprecated));

	// An edit resets the cache; the next request must not find a read left in flight to coalesce onto.
	CrowdyStudioSyncService::InvalidateCachedStatus(Effect);
	CrowdyStudioSyncService::RequestEffectSyncStatus(Effect, OnStatus);
	TestEqual(TEXT("a request after an edit is answered at once too"), StatusAnswers, 2);
	TestTrue(TEXT("with the deprecation message rather than a check in progress"), IsDeprecatedMessage(StatusMessage));

	int32 SyncAnswers = 0;
	bool bSyncOk = true;
	FString SyncMessage;
	CrowdyStudioSyncService::SyncEffect(Effect, [&SyncAnswers, &bSyncOk, &SyncMessage](bool bOk, const FString& Message)
	{
		++SyncAnswers;
		bSyncOk = bOk;
		SyncMessage = Message;
	});
	TestEqual(TEXT("a sync is answered at once, exactly once"), SyncAnswers, 1);
	TestFalse(TEXT("as a failure"), bSyncOk);
	TestTrue(TEXT("with the deprecation message"), IsDeprecatedMessage(SyncMessage));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
