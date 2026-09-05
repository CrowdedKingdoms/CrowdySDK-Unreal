#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyKitBridge.h"
#include "Misc/AutomationTest.h"

// The kit deploy sequences the emitted bundle over the async client: the seed, then each automation, then each
// trigger. These tests drive the sequence against the canned transport (no network) and assert the step accounting
// and the stop-on-first-failure contract.
namespace
{
	constexpr EAutomationTestFlags CrowdyKitDeployTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A multi-automation bundle: Living World enables time/nodes/crops interval automations by default, so a deploy
	// exercises the seed step AND the sequential automation loop, not just the seed.
	FCrowdyKitBundle MakeLivingWorldBundle()
	{
		FCrowdyKitLayerSpec Layer;
		Layer.Genre = ECrowdyKitGenre::LivingWorld;
		return FCrowdyKitBridge::EmitBundle(1, { Layer }, FString());
	}

	// Drive a deploy synchronously against the canned transport: every request resolves to Body, and Poll() is
	// pumped until the aggregate OnDone fires (each step is issued from the previous step's callback).
	FCrowdyKitDeployResult DeployViaCrowdyCpp(const FCrowdyKitBundle& Bundle, const FString& Body, int32 HttpStatus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, HttpStatus);
		FCrowdyKitDeployResult Captured;
		bool bCalled = false;
		if (Client.IsValid())
		{
			FCrowdyKitBridge::DeployBundle(Client.ToSharedRef(), Bundle,
				[&Captured, &bCalled](FCrowdyKitDeployResult Result)
				{
					Captured = MoveTemp(Result);
					bCalled = true;
				});
			for (int32 Iteration = 0; Iteration < 128 && !bCalled; ++Iteration)
			{
				Client->Poll();
			}
		}
		check(bCalled);
		return Captured;
	}
}

// A well-formed bundle deploys every step when the server accepts each op; StepsCompleted reaches StepsTotal, which
// is 1 (the seed) plus the automation and trigger counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeploySuccessTest,
	"CrowdySDK.CrowdyKit.DeploySuccess", CrowdyKitDeployTestFlags)
bool FCrowdyKitDeploySuccessTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitBundle Bundle = MakeLivingWorldBundle();
	if (!TestTrue(TEXT("bundle emitted"), Bundle.bOk))
	{
		return false;
	}
	if (!TestTrue(TEXT("bundle has automations to deploy"), Bundle.AutomationJsons.Num() > 0))
	{
		return false;
	}

	// Any 2xx response with no errors[] is a per-step success, so the same body serves every step.
	const FString Body = TEXT("{\"data\":{\"gameModelSeed\":{\"ok\":true}}}");
	const FCrowdyKitDeployResult Result = DeployViaCrowdyCpp(Bundle, Body, 200);

	TestTrue(TEXT("deploy ok"), Result.bOk);
	TestEqual(TEXT("steps total is 1 seed + automations + triggers"),
		Result.StepsTotal, 1 + Bundle.AutomationJsons.Num() + Bundle.TriggerJsons.Num());
	TestEqual(TEXT("every step completed"), Result.StepsCompleted, Result.StepsTotal);
	return true;
}

// A server rejection on the first step (the seed) stops the chain: no automations run, bOk is false, and the error
// names the failing step and carries the server reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeploySeedFailureTest,
	"CrowdySDK.CrowdyKit.DeploySeedFailure", CrowdyKitDeployTestFlags)
bool FCrowdyKitDeploySeedFailureTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitBundle Bundle = MakeLivingWorldBundle();
	if (!TestTrue(TEXT("bundle emitted"), Bundle.bOk))
	{
		return false;
	}

	const FString Body = TEXT("{\"errors\":[{\"message\":\"not authorized\"}]}");
	const FCrowdyKitDeployResult Result = DeployViaCrowdyCpp(Bundle, Body, 200);

	TestFalse(TEXT("deploy failed"), Result.bOk);
	TestEqual(TEXT("no steps completed"), Result.StepsCompleted, 0);
	TestTrue(TEXT("error names the seed step"), Result.ErrorMessage.Contains(TEXT("seed")));
	TestTrue(TEXT("error carries the server reason"), Result.ErrorMessage.Contains(TEXT("not authorized")));
	return true;
}

// A non-deployable bundle (a failed emit) is rejected synchronously with no network: the bundle guard short-circuits
// before the seed, so no step runs even though the canned body would have reported success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeployRejectsFailedBundleTest,
	"CrowdySDK.CrowdyKit.DeployRejectsFailedBundle", CrowdyKitDeployTestFlags)
bool FCrowdyKitDeployRejectsFailedBundleTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, TArray<FCrowdyKitLayerSpec>(), FString());
	if (!TestFalse(TEXT("empty layers is a failed bundle"), Bundle.bOk))
	{
		return false;
	}

	const FString Body = TEXT("{\"data\":{\"gameModelSeed\":{\"ok\":true}}}");
	const FCrowdyKitDeployResult Result = DeployViaCrowdyCpp(Bundle, Body, 200);

	TestFalse(TEXT("deploy of a failed bundle fails"), Result.bOk);
	TestEqual(TEXT("no steps completed"), Result.StepsCompleted, 0);
	TestFalse(TEXT("error message is set"), Result.ErrorMessage.IsEmpty());
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
