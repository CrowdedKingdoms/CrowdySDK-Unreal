#include "CrowdyKitBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Structural goldens for the kit emit surface. These prove OUR mapping (a UE
// options struct -> crowdy::kit options -> the linked builder -> the merged
// gameModelSeed bundle) and the load-bearing invariants (appId as a JSON string,
// throw containment, cross-genre collision rejection). They deliberately assert
// structure - type names, function names, automation counts - rather than a full
// JSON string, so they do not re-test (or break with) CrowdyCPP's own builders.
namespace
{
	constexpr EAutomationTestFlags CrowdyKitEmitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Collect the "name"-style key of every element of a seed array field (empty when absent).
	TArray<FString> CollectField(const TSharedPtr<FJsonObject>& Seed, const TCHAR* ArrayField, const TCHAR* NameField)
	{
		TArray<FString> Names;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Seed.IsValid() && Seed->TryGetArrayField(ArrayField, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
				FString Name;
				if (Obj.IsValid() && Obj->TryGetStringField(NameField, Name))
				{
					Names.Add(Name);
				}
			}
		}
		return Names;
	}

	// The invokePolicyJson of a named function in the seed (empty when the function is absent).
	FString FunctionPolicy(const TSharedPtr<FJsonObject>& Seed, const FString& FunctionName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Seed.IsValid() && Seed->TryGetArrayField(TEXT("functions"), Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
				FString Name;
				if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), Name) && Name == FunctionName)
				{
					FString Policy;
					Obj->TryGetStringField(TEXT("invokePolicyJson"), Policy);
					return Policy;
				}
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatEmitTest, "CrowdySDK.CrowdyKit.CombatEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitCombatEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Combat;
	Layer.Combat.TypePrefix = TEXT("Goblin");

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(12345, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	TestTrue(TEXT("seed parses"), Seed.IsValid());

	// appId is a BigInt scalar: a JSON string, never a number.
	const TSharedPtr<FJsonValue> AppIdValue = Seed.IsValid() ? Seed->TryGetField(TEXT("appId")) : nullptr;
	TestTrue(TEXT("appId is a string"), AppIdValue.IsValid() && AppIdValue->Type == EJson::String);
	FString AppIdString;
	if (Seed.IsValid())
	{
		Seed->TryGetStringField(TEXT("appId"), AppIdString);
	}
	TestEqual(TEXT("appId value"), AppIdString, TEXT("12345"));

	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	TestTrue(TEXT("Combatant type"), Types.Contains(TEXT("GoblinCombatant")));
	TestTrue(TEXT("StatusEffect type"), Types.Contains(TEXT("GoblinStatusEffect")));

	const TArray<FString> Functions = CollectField(Seed, TEXT("functions"), TEXT("name"));
	TestTrue(TEXT("attack fn"), Functions.Contains(TEXT("goblin_attack")));
	TestTrue(TEXT("apply_effect fn"), Functions.Contains(TEXT("goblin_apply_effect")));
	TestTrue(TEXT("effect_tick fn"), Functions.Contains(TEXT("goblin_effect_tick")));
	TestTrue(TEXT("respawn fn"), Functions.Contains(TEXT("goblin_respawn")));
	// Revive/sync are opt-in and off here.
	TestFalse(TEXT("no revive fn"), Functions.Contains(TEXT("goblin_revive")));
	TestFalse(TEXT("no sync fn"), Functions.Contains(TEXT("goblin_sync_combatant")));

	// One status-effect tick automation.
	TestEqual(TEXT("one automation"), Bundle.NumAutomations, 1);
	TestEqual(TEXT("automation json count"), Bundle.AutomationJsons.Num(), 1);

	// The BigInt appId invariant holds on the emitted automation too, not only the seed.
	const TSharedPtr<FJsonObject> Automation =
		ParseObject(Bundle.AutomationJsons.Num() > 0 ? Bundle.AutomationJsons[0] : FString());
	const TSharedPtr<FJsonValue> AutomationAppId = Automation.IsValid() ? Automation->TryGetField(TEXT("appId")) : nullptr;
	TestTrue(TEXT("automation appId is a string"), AutomationAppId.IsValid() && AutomationAppId->Type == EJson::String);

	// Non-turn-based: the attack policy must not gate on the session turn.
	TestFalse(TEXT("no turn gate"), FunctionPolicy(Seed, TEXT("goblin_attack")).Contains(TEXT("is_current_turn")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatOptionsEmitTest, "CrowdySDK.CrowdyKit.CombatOptionsEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitCombatOptionsEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Combat;
	Layer.Combat.bTurnBased = true;
	Layer.Combat.bHostSynced = true;
	Layer.Combat.bEnableRevive = true;
	Layer.Combat.ReviveGroupId = TEXT("healers");

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Functions = CollectField(Seed, TEXT("functions"), TEXT("name"));
	// Empty prefix -> bare function names.
	TestTrue(TEXT("revive fn present"), Functions.Contains(TEXT("revive")));
	TestTrue(TEXT("sync fn present"), Functions.Contains(TEXT("sync_combatant")));
	// Turn-based gates the attack on the session turn.
	TestTrue(TEXT("turn gate present"), FunctionPolicy(Seed, TEXT("attack")).Contains(TEXT("is_current_turn")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatReviveRequiresGroupTest, "CrowdySDK.CrowdyKit.CombatReviveRequiresGroup",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitCombatReviveRequiresGroupTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Combat;
	Layer.Combat.bEnableRevive = true;
	// ReviveGroupId deliberately empty: an empty group id would emit a degenerate revive policy.

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestFalse(TEXT("emit fails"), Bundle.bOk);
	TestTrue(TEXT("message names the revive group id"), Bundle.ErrorMessage.Contains(TEXT("Revive Group Id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsSeasonEmitTest, "CrowdySDK.CrowdyKit.LeaderboardsSeasonEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitLeaderboardsSeasonEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Leaderboards;
	Layer.Leaderboards.TypePrefix = TEXT("Weekly");
	Layer.Leaderboards.SubmitAuthority = ECrowdyKitSubmitAuthority::Server;
	Layer.Leaderboards.SeasonCron = TEXT("0 0 1 * *");

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	TestTrue(TEXT("entry type"), Types.Contains(TEXT("WeeklyLeaderboardEntry")));

	const TArray<FString> Functions = CollectField(Seed, TEXT("functions"), TEXT("name"));
	TestTrue(TEXT("submit fn"), Functions.Contains(TEXT("weekly_submit_score")));
	TestTrue(TEXT("roll_season fn"), Functions.Contains(TEXT("weekly_roll_season")));

	// The season cron adds exactly one automation.
	TestEqual(TEXT("one automation"), Bundle.NumAutomations, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsNoSeasonEmitTest, "CrowdySDK.CrowdyKit.LeaderboardsNoSeasonEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitLeaderboardsNoSeasonEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Leaderboards;

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Functions = CollectField(Seed, TEXT("functions"), TEXT("name"));
	TestTrue(TEXT("submit fn"), Functions.Contains(TEXT("submit_score")));
	TestFalse(TEXT("no roll_season fn"), Functions.Contains(TEXT("roll_season")));
	// No season means no automation.
	TestEqual(TEXT("no automation"), Bundle.NumAutomations, 0);
	TestEqual(TEXT("no automation json"), Bundle.AutomationJsons.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitGuildRequiresGroupIdTest, "CrowdySDK.CrowdyKit.GuildRequiresGroupId",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitGuildRequiresGroupIdTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Guild;
	// GuildGroupId deliberately empty.

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestFalse(TEXT("emit fails"), Bundle.bOk);
	TestTrue(TEXT("message names the group id"), Bundle.ErrorMessage.Contains(TEXT("Group Id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitGuildEmitTest, "CrowdySDK.CrowdyKit.GuildEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitGuildEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::Guild;
	Layer.Guild.GuildGroupId = TEXT("team-1");

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	// The composite is a hall lockable plus a bank inventory (exact lock/inventory
	// suffixes belong to CrowdyCPP; assert the prefixes we own).
	bool bHasHall = false;
	bool bHasBank = false;
	for (const FString& Type : Types)
	{
		bHasHall |= Type.Contains(TEXT("GuildHall"));
		bHasBank |= Type.Contains(TEXT("GuildBank"));
	}
	TestTrue(TEXT("hall type present"), bHasHall);
	TestTrue(TEXT("bank type present"), bHasBank);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLivingWorldDefaultEmitTest, "CrowdySDK.CrowdyKit.LivingWorldDefaultEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitLivingWorldDefaultEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::LivingWorld;
	// Defaults: time/nodes/crops enabled, waves off.

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	TestTrue(TEXT("world state"), Types.Contains(TEXT("WorldState")));
	TestTrue(TEXT("resource node"), Types.Contains(TEXT("ResourceNode")));
	TestTrue(TEXT("crop"), Types.Contains(TEXT("Crop")));
	TestFalse(TEXT("no wave spawner"), Types.Contains(TEXT("WaveSpawner")));

	// clock + node-regen + crop-growth = 3.
	TestEqual(TEXT("three automations"), Bundle.NumAutomations, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLivingWorldWavesEmitTest, "CrowdySDK.CrowdyKit.LivingWorldWavesEmit",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitLivingWorldWavesEmitTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::LivingWorld;
	Layer.LivingWorld.bEnableWaves = true;

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	TestTrue(TEXT("wave spawner present"), Types.Contains(TEXT("WaveSpawner")));
	// clock + node-regen + crop-growth + wave-spawner = 4.
	TestEqual(TEXT("four automations"), Bundle.NumAutomations, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLivingWorldAllDisabledTest, "CrowdySDK.CrowdyKit.LivingWorldAllDisabled",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitLivingWorldAllDisabledTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Layer;
	Layer.Genre = ECrowdyKitGenre::LivingWorld;
	Layer.LivingWorld.bEnableTime = false;
	Layer.LivingWorld.bEnableNodes = false;
	Layer.LivingWorld.bEnableCrops = false;
	Layer.LivingWorld.bEnableWaves = false;

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {Layer}, FString());
	TestFalse(TEXT("emit fails"), Bundle.bOk);
	TestFalse(TEXT("message is set"), Bundle.ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmptyLayersTest, "CrowdySDK.CrowdyKit.EmptyLayers",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitEmptyLayersTest::RunTest(const FString&)
{
	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {}, FString());
	TestFalse(TEXT("empty fails"), Bundle.bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitDuplicateCollisionTest, "CrowdySDK.CrowdyKit.DuplicateCollision",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitDuplicateCollisionTest::RunTest(const FString&)
{
	// Two combat layers with the same (empty) prefix both define a "Combatant" type.
	FCrowdyKitLayerSpec A;
	A.Genre = ECrowdyKitGenre::Combat;
	FCrowdyKitLayerSpec B;
	B.Genre = ECrowdyKitGenre::Combat;

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(1, {A, B}, FString());
	TestFalse(TEXT("collision fails"), Bundle.bOk);
	// mergeBlueprints reports the redefinition; the contained message carries it.
	TestTrue(TEXT("message names the redefinition"), Bundle.ErrorMessage.Contains(TEXT("redefines")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitMultiGenreMergeTest, "CrowdySDK.CrowdyKit.MultiGenreMerge",
	CrowdyKitEmitTestFlags)
bool FCrowdyKitMultiGenreMergeTest::RunTest(const FString&)
{
	FCrowdyKitLayerSpec Combat;
	Combat.Genre = ECrowdyKitGenre::Combat;
	Combat.Combat.TypePrefix = TEXT("A");

	FCrowdyKitLayerSpec Boards;
	Boards.Genre = ECrowdyKitGenre::Leaderboards;
	Boards.Leaderboards.TypePrefix = TEXT("B");
	// No season -> no automation.

	FCrowdyKitLayerSpec World;
	World.Genre = ECrowdyKitGenre::LivingWorld;
	World.LivingWorld.TypePrefix = TEXT("C");
	// Defaults: 3 automations, waves off.

	const FCrowdyKitBundle Bundle = FCrowdyKitBridge::EmitBundle(999, {Combat, Boards, World}, TEXT("sess-1"));
	TestTrue(TEXT("emit ok"), Bundle.bOk);

	const TSharedPtr<FJsonObject> Seed = ParseObject(Bundle.SeedInputJson);
	TestTrue(TEXT("seed parses"), Seed.IsValid());

	// appId stays a string across the merge; sessionId rides the seed.
	FString AppIdString;
	FString SessionString;
	if (Seed.IsValid())
	{
		Seed->TryGetStringField(TEXT("appId"), AppIdString);
		Seed->TryGetStringField(TEXT("sessionId"), SessionString);
	}
	TestEqual(TEXT("appId value"), AppIdString, TEXT("999"));
	TestEqual(TEXT("sessionId value"), SessionString, TEXT("sess-1"));

	// Combat effect-tick (1) + worldsim clock/node/crop (3) = 4.
	TestEqual(TEXT("merged automations"), Bundle.NumAutomations, 4);

	// Types from all three genres are present (each carries its own prefix, so no collision).
	const TArray<FString> Types = CollectField(Seed, TEXT("containerTypes"), TEXT("typeName"));
	TestTrue(TEXT("combat type"), Types.Contains(TEXT("ACombatant")));
	TestTrue(TEXT("board type"), Types.Contains(TEXT("BLeaderboardEntry")));
	TestTrue(TEXT("world type"), Types.Contains(TEXT("CWorldState")));
	TestTrue(TEXT("counts are additive"), Bundle.NumContainerTypes >= 5);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
