#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/CrowdyModelChangeActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelChangeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyGameModelTestTarget* MakeChangeTarget()
	{
		return NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	}
}

// The pure listener filter: an explicit Target wins over a ModelId, a destroyed Target (null) matches nothing
// (never silently widening to global), a ModelId matches by string, and no filter passes everything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelListenFilterTest,
	"CrowdySDK.GameModel.ListenFilterDecision", CrowdyModelChangeTestFlags)
bool FCrowdyModelListenFilterTest::RunTest(const FString& Parameters)
{
	UObject* A = MakeChangeTarget();
	UObject* B = MakeChangeTarget();
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
	{
		return false;
	}

	using CrowdyModelListen::PassesFilter;

	// No filter -> global: everything passes, including a null Target (a free container).
	TestTrue(TEXT("global passes a value"), PassesFilter(false, nullptr, FString(), A, TEXT("cid")));
	TestTrue(TEXT("global passes a null-Target free container"), PassesFilter(false, nullptr, FString(), nullptr, TEXT("cid")));

	// Target filter: matches only that exact object.
	TestTrue(TEXT("target matches self"), PassesFilter(true, A, FString(), A, TEXT("cid")));
	TestFalse(TEXT("target rejects another object"), PassesFilter(true, A, FString(), B, TEXT("cid")));
	// A destroyed Target (now null) matches nothing rather than becoming a global listener.
	TestFalse(TEXT("stale target matches nothing"), PassesFilter(true, nullptr, FString(), A, TEXT("cid")));

	// ModelId filter: matches by string.
	TestTrue(TEXT("modelId matches"), PassesFilter(false, nullptr, TEXT("cid-1"), A, TEXT("cid-1")));
	TestFalse(TEXT("modelId rejects a different id"), PassesFilter(false, nullptr, TEXT("cid-1"), A, TEXT("cid-2")));

	// Target takes precedence over a ModelId: the ModelId is ignored when a Target filter is set.
	TestTrue(TEXT("target wins over a mismatched modelId"), PassesFilter(true, A, TEXT("cid-1"), A, TEXT("cid-2")));
	TestFalse(TEXT("target mismatch rejects even when modelId matches"), PassesFilter(true, A, TEXT("cid-1"), B, TEXT("cid-1")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
