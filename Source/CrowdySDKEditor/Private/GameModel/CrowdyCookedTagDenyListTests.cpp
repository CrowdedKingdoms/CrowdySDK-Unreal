// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCookedTagDenyListTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// One entry as the deny list spells it. The wildcard class is deliberate: these tags are written by a
	// subscription that fires for any asset, not by one class's own override.
	FString CrowdyDenyListEntryFor(const TCHAR* Tag)
	{
		return FString::Printf(TEXT("(Class=*,Tag=%s)"), Tag);
	}
}

/**
 * The plugin ships the cook deny list for its own schema tags, so a project consuming it does not have to copy
 * anything into DefaultEngine.ini to avoid shipping editor-only payloads in a packaged build.
 *
 * This asserts the entries actually reach the engine config hierarchy. A plugin config file that the engine never
 * merges would fail silently and in the worst possible place: nothing misbehaves in the editor, and the tags simply
 * turn up resident in a cooked build that nobody thought to inspect.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCookedTagDenyListReachesEngineConfigTest,
	"CrowdySDK.GameModel.CookedTagDenyListReachesEngineConfig", CrowdyCookedTagDenyListTestFlags)
bool FCrowdyCookedTagDenyListReachesEngineConfigTest::RunTest(const FString& Parameters)
{
	TArray<FString> DenyList;
	GConfig->GetArray(TEXT("AssetRegistry"), TEXT("CookedTagsBlacklist"), DenyList, GEngineIni);

	if (!TestTrue(TEXT("the engine config carries a cooked tag deny list at all"), DenyList.Num() > 0))
	{
		return false;
	}

	const TArray<FString> Required =
	{
		CrowdyDenyListEntryFor(CrowdyGameModelMetaKeys::ScanAssetTag),
		CrowdyDenyListEntryFor(CrowdyGameModelMetaKeys::ContainerTypeAssetTag),
		CrowdyDenyListEntryFor(CrowdyEffectTagKeys::AuthoredSurface),
		CrowdyDenyListEntryFor(CrowdyEffectTagKeys::AuthoredSurfaceVersion),
	};

	for (const FString& Entry : Required)
	{
		// Case-INSENSITIVE on purpose: this compares against text a human typed into an ini, and the engine does
		// not normalize it. What matters is that the entry arrived, not how it was capitalized.
		const bool bPresent = DenyList.ContainsByPredicate(
			[&Entry](const FString& Candidate) { return Candidate.Equals(Entry, ESearchCase::IgnoreCase); });

		TestTrue(FString::Printf(TEXT("the deny list carries %s"), *Entry), bPresent);
	}

	return true;
}

#endif
