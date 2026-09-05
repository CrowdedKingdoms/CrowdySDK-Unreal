#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Kit/CrowdyGameKitConfig.h"
#include "Replication/GameModel/Kit/CrowdyGameKitEmit.h"
#include "Replication/GameModel/Kit/CrowdyGameKitPreview.h"
#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"
#include "UObject/Package.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

// The editor-authored preset layer must convert to the same deployable bundle the
// direct bridge spec produces. These tests build a preset, run it through
// CrowdyKitEmitFromPresets, and assert the emitted bundle is byte-identical to the one
// the equivalent hand-built FCrowdyKitLayerSpec yields through the bridge. That proves
// the preset-to-spec field mapping without re-testing the underlying kit builders,
// which have their own structural coverage.
namespace
{
	constexpr EAutomationTestFlags CrowdyKitPresetTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	template <typename T>
	T* MakePreset()
	{
		return NewObject<T>(GetTransientPackage());
	}

	// Compare two emitted bundles field for field so a parity mismatch names the field.
	bool BundlesMatch(FAutomationTestBase& Test, const FCrowdyKitBundle& A, const FCrowdyKitBundle& B)
	{
		bool bOk = true;
		bOk &= Test.TestTrue(TEXT("bOk matches"), A.bOk == B.bOk);
		bOk &= Test.TestEqual(TEXT("SeedInputJson"), A.SeedInputJson, B.SeedInputJson);
		bOk &= Test.TestEqual(TEXT("automation count"), A.AutomationJsons.Num(), B.AutomationJsons.Num());
		if (A.AutomationJsons.Num() == B.AutomationJsons.Num())
		{
			for (int32 i = 0; i < A.AutomationJsons.Num(); ++i)
			{
				bOk &= Test.TestEqual(*FString::Printf(TEXT("automation[%d]"), i), A.AutomationJsons[i], B.AutomationJsons[i]);
			}
		}
		bOk &= Test.TestEqual(TEXT("trigger count"), A.TriggerJsons.Num(), B.TriggerJsons.Num());
		if (A.TriggerJsons.Num() == B.TriggerJsons.Num())
		{
			for (int32 i = 0; i < A.TriggerJsons.Num(); ++i)
			{
				bOk &= Test.TestEqual(*FString::Printf(TEXT("trigger[%d]"), i), A.TriggerJsons[i], B.TriggerJsons[i]);
			}
		}
		bOk &= Test.TestEqual(TEXT("NumContainerTypes"), A.NumContainerTypes, B.NumContainerTypes);
		bOk &= Test.TestEqual(TEXT("NumPropertyDefs"), A.NumPropertyDefs, B.NumPropertyDefs);
		bOk &= Test.TestEqual(TEXT("NumFunctions"), A.NumFunctions, B.NumFunctions);
		bOk &= Test.TestEqual(TEXT("NumAutomations"), A.NumAutomations, B.NumAutomations);
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetCombatParityTest, "CrowdySDK.CrowdyKit.PresetCombatParity",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetCombatParityTest::RunTest(const FString&)
{
	UCrowdyCombatPreset* Preset = MakePreset<UCrowdyCombatPreset>();
	Preset->TypePrefix = TEXT("Goblin");
	Preset->bTurnBased = true;
	Preset->bHostSynced = true;
	Preset->EffectTickIntervalMs = 3000;
	Preset->CombatantInstantiableBy = ECrowdyKitCreator::Admin;
	Preset->bEnableRevive = true;
	Preset->ReviveGroupId = TEXT("healers");
	Preset->RevivePermission = TEXT("medic");
	Preset->OwnerIdKind = ECrowdyKitOwnerId::String;

	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Preset};
	const FCrowdyKitBundle FromPreset = CrowdyKitEmitFromPresets(Layers, 12345, FString());

	FCrowdyKitLayerSpec Spec;
	Spec.Genre = ECrowdyKitGenre::Combat;
	Spec.Combat.TypePrefix = TEXT("Goblin");
	Spec.Combat.bTurnBased = true;
	Spec.Combat.bHostSynced = true;
	Spec.Combat.EffectTickIntervalMs = 3000;
	Spec.Combat.CombatantInstantiableBy = ECrowdyKitInstantiableBy::Admin;
	Spec.Combat.bEnableRevive = true;
	Spec.Combat.ReviveGroupId = TEXT("healers");
	Spec.Combat.RevivePermission = TEXT("medic");
	Spec.Combat.OwnerIdKind = ECrowdyKitOwnerIdKind::String;
	const FCrowdyKitBundle Direct = FCrowdyKitBridge::EmitBundle(12345, {Spec}, FString());

	TestTrue(TEXT("preset emit ok"), FromPreset.bOk);
	return BundlesMatch(*this, FromPreset, Direct);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetLeaderboardsParityTest, "CrowdySDK.CrowdyKit.PresetLeaderboardsParity",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetLeaderboardsParityTest::RunTest(const FString&)
{
	UCrowdyLeaderboardsPreset* Preset = MakePreset<UCrowdyLeaderboardsPreset>();
	Preset->TypePrefix = TEXT("Weekly");
	Preset->SubmitAuthority = ECrowdyKitAuthority::Server;
	Preset->bKeepBest = false;
	Preset->SeasonCron = TEXT("0 0 1 * *");
	Preset->OwnerIdKind = ECrowdyKitOwnerId::String;

	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Preset};
	const FCrowdyKitBundle FromPreset = CrowdyKitEmitFromPresets(Layers, 7, FString());

	FCrowdyKitLayerSpec Spec;
	Spec.Genre = ECrowdyKitGenre::Leaderboards;
	Spec.Leaderboards.TypePrefix = TEXT("Weekly");
	Spec.Leaderboards.SubmitAuthority = ECrowdyKitSubmitAuthority::Server;
	Spec.Leaderboards.bKeepBest = false;
	Spec.Leaderboards.SeasonCron = TEXT("0 0 1 * *");
	Spec.Leaderboards.OwnerIdKind = ECrowdyKitOwnerIdKind::String;
	const FCrowdyKitBundle Direct = FCrowdyKitBridge::EmitBundle(7, {Spec}, FString());

	TestTrue(TEXT("preset emit ok"), FromPreset.bOk);
	return BundlesMatch(*this, FromPreset, Direct);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetLivingWorldParityTest, "CrowdySDK.CrowdyKit.PresetLivingWorldParity",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetLivingWorldParityTest::RunTest(const FString&)
{
	UCrowdyLivingWorldPreset* Preset = MakePreset<UCrowdyLivingWorldPreset>();
	Preset->TypePrefix = TEXT("Realm");
	Preset->bEnableTime = true;
	Preset->TimeIntervalMs = 30000;
	Preset->HoursPerDay = 12;
	Preset->bWeather = false;
	Preset->NotifyDistance = 4;
	Preset->bEnableNodes = true;
	Preset->NodesIntervalMs = 45000;
	Preset->bEnableCrops = false;
	Preset->bEnableWaves = true;
	Preset->WavesIntervalMs = 20000;
	Preset->WaveGrowth = 3;
	Preset->OwnerIdKind = ECrowdyKitOwnerId::Int;

	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Preset};
	const FCrowdyKitBundle FromPreset = CrowdyKitEmitFromPresets(Layers, 99, FString());

	FCrowdyKitLayerSpec Spec;
	Spec.Genre = ECrowdyKitGenre::LivingWorld;
	Spec.LivingWorld.TypePrefix = TEXT("Realm");
	Spec.LivingWorld.bEnableTime = true;
	Spec.LivingWorld.Time.IntervalMs = 30000;
	Spec.LivingWorld.Time.HoursPerDay = 12;
	Spec.LivingWorld.Time.bWeather = false;
	Spec.LivingWorld.Time.NotifyDistance = 4;
	Spec.LivingWorld.bEnableNodes = true;
	Spec.LivingWorld.Nodes.IntervalMs = 45000;
	Spec.LivingWorld.bEnableCrops = false;
	Spec.LivingWorld.bEnableWaves = true;
	Spec.LivingWorld.Waves.IntervalMs = 20000;
	Spec.LivingWorld.Waves.Growth = 3;
	Spec.LivingWorld.OwnerIdKind = ECrowdyKitOwnerIdKind::Int;
	const FCrowdyKitBundle Direct = FCrowdyKitBridge::EmitBundle(99, {Spec}, FString());

	TestTrue(TEXT("preset emit ok"), FromPreset.bOk);
	return BundlesMatch(*this, FromPreset, Direct);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetMultiGenreParityTest, "CrowdySDK.CrowdyKit.PresetMultiGenreParity",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetMultiGenreParityTest::RunTest(const FString&)
{
	UCrowdyCombatPreset* Combat = MakePreset<UCrowdyCombatPreset>();
	Combat->TypePrefix = TEXT("A");
	UCrowdyLeaderboardsPreset* Boards = MakePreset<UCrowdyLeaderboardsPreset>();
	Boards->TypePrefix = TEXT("B");
	UCrowdyLivingWorldPreset* World = MakePreset<UCrowdyLivingWorldPreset>();
	World->TypePrefix = TEXT("C");

	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Combat, Boards, World};
	const FCrowdyKitBundle FromPreset = CrowdyKitEmitFromPresets(Layers, 999, TEXT("sess-1"));

	FCrowdyKitLayerSpec SpecCombat;
	SpecCombat.Genre = ECrowdyKitGenre::Combat;
	SpecCombat.Combat.TypePrefix = TEXT("A");
	FCrowdyKitLayerSpec SpecBoards;
	SpecBoards.Genre = ECrowdyKitGenre::Leaderboards;
	SpecBoards.Leaderboards.TypePrefix = TEXT("B");
	FCrowdyKitLayerSpec SpecWorld;
	SpecWorld.Genre = ECrowdyKitGenre::LivingWorld;
	SpecWorld.LivingWorld.TypePrefix = TEXT("C");
	const FCrowdyKitBundle Direct = FCrowdyKitBridge::EmitBundle(999, {SpecCombat, SpecBoards, SpecWorld}, TEXT("sess-1"));

	TestTrue(TEXT("preset emit ok"), FromPreset.bOk);
	return BundlesMatch(*this, FromPreset, Direct);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetNullLayerSkippedTest, "CrowdySDK.CrowdyKit.PresetNullLayerSkipped",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetNullLayerSkippedTest::RunTest(const FString&)
{
	// A null slot (an added-but-unconfigured layer) is skipped; the emit runs on the configured layers.
	UCrowdyCombatPreset* Combat = MakePreset<UCrowdyCombatPreset>();
	Combat->TypePrefix = TEXT("A");
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {nullptr, Combat, nullptr};

	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, 1, FString());
	TestTrue(TEXT("emit ok with null slots"), Bundle.bOk);
	TestTrue(TEXT("has a combatant type"), Bundle.NumContainerTypes > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPresetGuildRequiresGroupTest, "CrowdySDK.CrowdyKit.PresetGuildRequiresGroup",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPresetGuildRequiresGroupTest::RunTest(const FString&)
{
	UCrowdyGuildPreset* Guild = MakePreset<UCrowdyGuildPreset>();
	// GuildGroupId deliberately empty.
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Guild};

	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, 1, FString());
	TestFalse(TEXT("emit fails"), Bundle.bOk);
	TestTrue(TEXT("message names the group id"), Bundle.ErrorMessage.Contains(TEXT("Group Id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPreviewCountsTest, "CrowdySDK.CrowdyKit.PreviewCounts",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPreviewCountsTest::RunTest(const FString&)
{
	// The bridge-free preview must report the same success + artifact counts the full emit produces, so an editor
	// surface can validate/preview a kit without the bridge type.
	UCrowdyLivingWorldPreset* World = MakePreset<UCrowdyLivingWorldPreset>();
	World->bEnableWaves = true;
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {World};

	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, 1, FString());
	const FCrowdyKitPreview Preview = CrowdyKitPreviewLayers(Layers, 1, FString());

	TestTrue(TEXT("preview ok matches bundle"), Preview.bOk == Bundle.bOk);
	TestTrue(TEXT("preview ok"), Preview.bOk);
	TestEqual(TEXT("container types"), Preview.NumContainerTypes, Bundle.NumContainerTypes);
	TestEqual(TEXT("property defs"), Preview.NumPropertyDefs, Bundle.NumPropertyDefs);
	TestEqual(TEXT("functions"), Preview.NumFunctions, Bundle.NumFunctions);
	TestEqual(TEXT("automations"), Preview.NumAutomations, Bundle.NumAutomations);
	// clock + node-regen + crop-growth + wave-spawner = 4.
	TestEqual(TEXT("waves-on automations"), Preview.NumAutomations, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDeployedNamesCombatTest, "CrowdySDK.CrowdyKit.DeployedNamesCombat",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitDeployedNamesCombatTest::RunTest(const FString&)
{
	// The exact-names facade reads the deployed container-type + function names out of the emitted seed. A Combat
	// layer with an EMPTY prefix produces bare names ("Combatant"/"StatusEffect", "attack"/"apply_effect") that no
	// prefix rule could recognize; this is exactly the case exact-name prune-protection exists to cover.
	UCrowdyCombatPreset* Preset = MakePreset<UCrowdyCombatPreset>();
	// TypePrefix deliberately empty.
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Preset};

	TArray<FString> TypeNames;
	TArray<FString> FunctionNames;
	const bool bOk = CrowdyKitDeployedNames(Layers, 12345, FString(), TypeNames, FunctionNames);

	TestTrue(TEXT("deployed-names emit ok"), bOk);
	TestTrue(TEXT("Combatant type present"), TypeNames.Contains(TEXT("Combatant")));
	TestTrue(TEXT("StatusEffect type present"), TypeNames.Contains(TEXT("StatusEffect")));
	TestTrue(TEXT("attack function present"), FunctionNames.Contains(TEXT("attack")));
	TestTrue(TEXT("apply_effect function present"), FunctionNames.Contains(TEXT("apply_effect")));

	// A failed emit yields no names (a Guild layer with no group id fails validation).
	UCrowdyGuildPreset* Guild = MakePreset<UCrowdyGuildPreset>();
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> GuildLayers = {Guild};
	TArray<FString> FailTypes;
	TArray<FString> FailFns;
	const bool bFailOk = CrowdyKitDeployedNames(GuildLayers, 1, FString(), FailTypes, FailFns);
	TestFalse(TEXT("a failed emit returns false"), bFailOk);
	TestEqual(TEXT("a failed emit yields no type names"), FailTypes.Num(), 0);
	TestEqual(TEXT("a failed emit yields no function names"), FailFns.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPreviewFailureTest, "CrowdySDK.CrowdyKit.PreviewFailure",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitPreviewFailureTest::RunTest(const FString&)
{
	UCrowdyGuildPreset* Guild = MakePreset<UCrowdyGuildPreset>();
	// GuildGroupId deliberately empty: the preview must carry the failure and its message.
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = {Guild};

	const FCrowdyKitPreview Preview = CrowdyKitPreviewLayers(Layers, 1, FString());
	TestFalse(TEXT("preview fails"), Preview.bOk);
	TestTrue(TEXT("error names the group id"), Preview.Error.Contains(TEXT("Group Id")));
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitConfigValidEmptyTest, "CrowdySDK.CrowdyKit.ConfigValidEmpty",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitConfigValidEmptyTest::RunTest(const FString&)
{
	// An empty (or only-unconfigured) config is not-yet-authored, not invalid.
	UCrowdyGameKitConfig* Config = MakePreset<UCrowdyGameKitConfig>();
	FDataValidationContext Context;
	TestTrue(TEXT("empty config is not invalid"), Config->IsDataValid(Context) != EDataValidationResult::Invalid);

	Config->Layers.Add(nullptr);
	FDataValidationContext ContextNull;
	TestTrue(TEXT("only-null config is not invalid"), Config->IsDataValid(ContextNull) != EDataValidationResult::Invalid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitConfigRejectsCollisionTest, "CrowdySDK.CrowdyKit.ConfigRejectsCollision",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitConfigRejectsCollisionTest::RunTest(const FString&)
{
	// Two combat layers with the same (empty) prefix both define a Combatant type.
	UCrowdyGameKitConfig* Config = MakePreset<UCrowdyGameKitConfig>();
	Config->Layers.Add(MakePreset<UCrowdyCombatPreset>());
	Config->Layers.Add(MakePreset<UCrowdyCombatPreset>());

	FDataValidationContext Context;
	TestTrue(TEXT("collision invalidates"), Config->IsDataValid(Context) == EDataValidationResult::Invalid);
	TestTrue(TEXT("an error was reported"), Context.GetNumErrors() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitConfigRejectsGuildNoGroupTest, "CrowdySDK.CrowdyKit.ConfigRejectsGuildNoGroup",
	CrowdyKitPresetTestFlags)
bool FCrowdyKitConfigRejectsGuildNoGroupTest::RunTest(const FString&)
{
	UCrowdyGameKitConfig* Config = MakePreset<UCrowdyGameKitConfig>();
	Config->Layers.Add(MakePreset<UCrowdyGuildPreset>());

	FDataValidationContext Context;
	TestTrue(TEXT("empty guild group invalidates"), Config->IsDataValid(Context) == EDataValidationResult::Invalid);
	return true;
}
#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
