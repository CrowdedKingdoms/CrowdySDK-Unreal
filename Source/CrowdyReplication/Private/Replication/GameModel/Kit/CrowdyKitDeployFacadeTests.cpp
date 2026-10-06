#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Replication/GameModel/Kit/CrowdyGameKitDeploy.h"
#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"
#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"

// The editor deploy facade (CrowdyKitDeployLayers) reports one FCrowdyKitDeployOutcome on the core ticker. A
// pre-network failure (a bad emit) is reported through that async path, delivered on a later tick rather than
// re-entrantly, and never touches the network.
namespace
{
	constexpr EAutomationTestFlags CrowdyKitDeployFacadeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Run the facade and pump the core ticker until the outcome lands (or the bound is hit). bDelivered reports
	// whether OnDone had already fired before the first tick, so a caller can assert the delivery is not re-entrant.
	bool RunDeployFacade(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers, const FString& GameApiUrl,
		FCrowdyKitDeployOutcome& OutOutcome, bool& bOutFiredBeforeTick)
	{
		bool bCalled = false;
		FCrowdyKitDeployOutcome Captured;
		CrowdyKitDeployLayers(Layers, 1, FString(), GameApiUrl, TEXT("admin-token"),
			[&bCalled, &Captured](FCrowdyKitDeployOutcome Outcome)
			{
				Captured = MoveTemp(Outcome);
				bCalled = true;
			});

		// The facade must never call OnDone synchronously inside CrowdyKitDeployLayers.
		bOutFiredBeforeTick = bCalled;

		for (int32 Iteration = 0; Iteration < 16 && !bCalled; ++Iteration)
		{
			FTSTicker::GetCoreTicker().Tick(0.0f);
		}

		OutOutcome = Captured;
		return bCalled;
	}
}

// An empty layer set is a failed emit; the facade reports it through the async outcome (no network), delivered on
// a tick rather than synchronously, with a message and zero steps.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeployFacadeEmitFailureTest,
	"CrowdySDK.CrowdyKit.DeployFacadeEmitFailure", CrowdyKitDeployFacadeTestFlags)
bool FCrowdyKitDeployFacadeEmitFailureTest::RunTest(const FString& Parameters)
{
	FCrowdyKitDeployOutcome Outcome;
	bool bFiredBeforeTick = false;
	const bool bFired = RunDeployFacade(TArray<TObjectPtr<UCrowdyKitLayerPreset>>(), TEXT("https://example/graphql"),
		Outcome, bFiredBeforeTick);

	if (!TestTrue(TEXT("outcome delivered"), bFired))
	{
		return false;
	}
	TestFalse(TEXT("delivery is not re-entrant"), bFiredBeforeTick);
	TestFalse(TEXT("deploy failed"), Outcome.bOk);
	TestFalse(TEXT("error message is set"), Outcome.Error.IsEmpty());
	TestEqual(TEXT("no steps ran"), Outcome.StepsCompleted, 0);
	TestEqual(TEXT("no steps total"), Outcome.StepsTotal, 0);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
