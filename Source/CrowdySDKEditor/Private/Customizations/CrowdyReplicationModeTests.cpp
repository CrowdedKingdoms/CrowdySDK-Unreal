// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyReplicationMode.h"
#include "Replication/State/CrowdyStateMetaKeys.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyReplicationModeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	bool Contains(const TArray<const TCHAR*>& Keys, const TCHAR* Key)
	{
		return Keys.ContainsByPredicate([Key](const TCHAR* Other) { return FCString::Strcmp(Other, Key) == 0; });
	}
}

// Leaving Replicated scrubs exactly the CrowdyState-owned keys, plus CrowdyOnRep when the variable leaves Crowdy
// entirely. Pure, so it is asserted here without the editor Slate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReplicationModeScrubTest,
	"CrowdySDK.Editor.ReplicationModeScrub", CrowdyReplicationModeTestFlags)
bool FCrowdyReplicationModeScrubTest::RunTest(const FString& Parameters)
{
	using EMode = ECrowdyReplicationMode;
	using namespace CrowdyReplicationModeDecision;

	// Replicated -> None: marker + owner-only + manual-dirty + heartbeat + OnRep.
	{
		const TArray<const TCHAR*> Scrub = KeysToScrubOnSwitch(EMode::Replicated, EMode::None);
		TestEqual(TEXT("five CrowdyState keys scrubbed on leaving to None"), Scrub.Num(), 5);
		TestTrue(TEXT("None scrubs the CrowdyState marker"), Contains(Scrub, CrowdyStateMetaKeys::Replicate));
		TestTrue(TEXT("None scrubs CrowdyOwnerOnly"), Contains(Scrub, CrowdyStateMetaKeys::OwnerOnly));
		TestTrue(TEXT("None scrubs CrowdyManualDirty"), Contains(Scrub, CrowdyStateMetaKeys::ManualDirty));
		TestTrue(TEXT("None scrubs CrowdyHeartbeat"), Contains(Scrub, CrowdyStateMetaKeys::Heartbeat));
		TestTrue(TEXT("None scrubs CrowdyOnRep"), Contains(Scrub, CrowdyStateMetaKeys::OnRep));
	}

	// Re-picking the same mode scrubs nothing (no spurious churn / no accidental clear).
	{
		TestEqual(TEXT("Replicated -> Replicated scrubs nothing"),
			KeysToScrubOnSwitch(EMode::Replicated, EMode::Replicated).Num(), 0);
		TestEqual(TEXT("None -> Replicated scrubs nothing (None owns no keys)"),
			KeysToScrubOnSwitch(EMode::None, EMode::Replicated).Num(), 0);
		TestEqual(TEXT("None -> None scrubs nothing"),
			KeysToScrubOnSwitch(EMode::None, EMode::None).Num(), 0);
	}

	return true;
}

// A Crowdy RepNotify names its function in metadata, so a renamed function graph has to drag its bindings along or
// they name a function that no longer exists. This is the rule that decides which bindings move.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOnRepFollowsGraphRenameTest,
	"CrowdySDK.Editor.OnRepFollowsGraphRename", CrowdyReplicationModeTestFlags)
bool FCrowdyOnRepFollowsGraphRenameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationModeDecision;

	TestTrue(TEXT("a binding naming the renamed graph follows it"),
		OnRepFollowsGraphRename(TEXT("OnRep_Health"), TEXT("OnRep_Health"), TEXT("OnRep_Hp")));

	// The notify resolves through UClass::FindFunctionByName, which is an FName lookup, so a binding that differs
	// only in case names the same function and must move too. Left behind, it would resolve to nothing.
	TestTrue(TEXT("a differently-cased binding names the same function and follows"),
		OnRepFollowsGraphRename(TEXT("onrep_health"), TEXT("OnRep_Health"), TEXT("OnRep_Hp")));

	TestFalse(TEXT("a binding naming another function stays put"),
		OnRepFollowsGraphRename(TEXT("OnRep_Shield"), TEXT("OnRep_Health"), TEXT("OnRep_Hp")));
	TestFalse(TEXT("a variable with no notify has nothing to move"),
		OnRepFollowsGraphRename(FString(), TEXT("OnRep_Health"), TEXT("OnRep_Hp")));

	// A rename that changes nothing must not stamp metadata: the stamp recompiles the skeleton, so a no-op rename
	// would cost a compile per graph touched.
	TestFalse(TEXT("a rename to the same name moves nothing"),
		OnRepFollowsGraphRename(TEXT("OnRep_Health"), TEXT("OnRep_Health"), TEXT("OnRep_Health")));

	// Neither end of the rename can be None: a nameless graph is not something a binding can name, and repointing a
	// binding AT None would clear it rather than move it.
	TestFalse(TEXT("no old name means no rename to follow"),
		OnRepFollowsGraphRename(TEXT("OnRep_Health"), NAME_None, TEXT("OnRep_Hp")));
	TestFalse(TEXT("no new name is never a target"),
		OnRepFollowsGraphRename(TEXT("OnRep_Health"), TEXT("OnRep_Health"), NAME_None));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
