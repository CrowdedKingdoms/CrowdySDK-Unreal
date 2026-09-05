#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Replication/GameModel/Kit/CrowdyGameKitDeploy.h"
#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"
#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"

// The editor deploy facade (CrowdyKitDeployLayers) emits the kit layers, builds an admin-token API client, and
// pumps its async completion on the core ticker, reporting one FCrowdyKitDeployOutcome. The network success path
// is covered at the bridge level (CrowdySDK.CrowdyKit.Deploy*, canned transport); these tests lock the facade's
// own contract: a pre-network failure (a bad emit, a missing endpoint) is reported through the same async path,
// delivered on a later tick rather than re-entrantly, and never touches the network.
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

// A well-formed emit with no configured Game API endpoint fails before any network step, reported through the same
// async outcome. A default Combat preset emits cleanly, so the only failure is the missing endpoint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeployFacadeNoEndpointTest,
	"CrowdySDK.CrowdyKit.DeployFacadeNoEndpoint", CrowdyKitDeployFacadeTestFlags)
bool FCrowdyKitDeployFacadeNoEndpointTest::RunTest(const FString& Parameters)
{
	TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers;
	Layers.Add(NewObject<UCrowdyCombatPreset>());

	FCrowdyKitDeployOutcome Outcome;
	bool bFiredBeforeTick = false;
	const bool bFired = RunDeployFacade(Layers, FString(), Outcome, bFiredBeforeTick);

	if (!TestTrue(TEXT("outcome delivered"), bFired))
	{
		return false;
	}
	TestFalse(TEXT("delivery is not re-entrant"), bFiredBeforeTick);
	TestFalse(TEXT("deploy failed"), Outcome.bOk);
	TestTrue(TEXT("error names the missing endpoint"), Outcome.Error.Contains(TEXT("endpoint")));
	TestEqual(TEXT("no steps ran"), Outcome.StepsCompleted, 0);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
