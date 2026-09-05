// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyCombatKitActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Kit/CrowdyCombatKitNames.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCombatKitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString CrowdyCombatSerializeObject(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}
}

// The Combatant type name and the attack function name are derived from the type prefix exactly as the vendored
// combat blueprint derives them: <Prefix>Combatant for the type, and "attack" (no prefix) or snake_case(prefix) +
// "_attack" (with a prefix). A drift here would call a function/type name the deployed kit never created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatTypeNameFromPrefixTest,
	"CrowdySDK.Kit.CombatTypeNameFromPrefix", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatTypeNameFromPrefixTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty prefix -> Combatant"),
		CrowdyCombatKitNames::CombatantTypeName(FString()), FString(TEXT("Combatant")));
	TestEqual(TEXT("PascalCase prefix concats"),
		CrowdyCombatKitNames::CombatantTypeName(TEXT("Fire")), FString(TEXT("FireCombatant")));

	TestEqual(TEXT("empty prefix -> attack"),
		CrowdyCombatKitNames::AttackFunctionName(FString()), FString(TEXT("attack")));
	TestEqual(TEXT("single-word prefix -> lower_attack"),
		CrowdyCombatKitNames::AttackFunctionName(TEXT("Fire")), FString(TEXT("fire_attack")));
	TestEqual(TEXT("PascalCase prefix -> snake_case_attack"),
		CrowdyCombatKitNames::AttackFunctionName(TEXT("MobEngine")), FString(TEXT("mob_engine_attack")));

	// toSnakeCase parity with the kit: hyphen/space collapse to one underscore.
	TestEqual(TEXT("hyphen collapses"),
		CrowdyCombatKitNames::ToSnakeCase(TEXT("Fire-Storm")), FString(TEXT("fire_storm")));

	return true;
}

// The attack function's target_id is a container_ref: on the wire it is the bare target container-id STRING, not
// a number and not a nested object. This is the load-bearing param shape for a Combat Attack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatAttackParamsJsonTest,
	"CrowdySDK.Kit.CombatAttackParamsJson", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatAttackParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = UCrowdyCombatAttackAction::BuildAttackParams(TEXT("container-77"));
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> TargetId = Params->TryGetField(TEXT("target_id"));
	if (TestNotNull(TEXT("target_id present"), TargetId.Get()))
	{
		TestEqual(TEXT("target_id is a JSON string"),
			static_cast<int32>(TargetId->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("target_id value"), TargetId->AsString(), FString(TEXT("container-77")));
	}

	// An all-digit id still round-trips as a string (never coerced to a number).
	const TSharedPtr<FJsonObject> NumericLikeId = UCrowdyCombatAttackAction::BuildAttackParams(TEXT("12345"));
	TestEqual(TEXT("serialized shape"), CrowdyCombatSerializeObject(NumericLikeId), FString(TEXT("{\"target_id\":\"12345\"}")));

	return true;
}

// The property keys and stat defaults must match the vendored combat blueprint so the nodes read/write the exact
// properties the deployed kit created. A rename on either side would silently no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatNamesMatchKitTest,
	"CrowdySDK.Kit.CombatNamesMatchKit", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatNamesMatchKitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("owner_user_id key"), FString(CrowdyCombatKitNames::Keys::OwnerUserId), FString(TEXT("owner_user_id")));
	TestEqual(TEXT("combat_key key"), FString(CrowdyCombatKitNames::Keys::CombatKey), FString(TEXT("combat_key")));
	TestEqual(TEXT("hp key"), FString(CrowdyCombatKitNames::Keys::Hp), FString(TEXT("hp")));
	TestEqual(TEXT("max_hp key"), FString(CrowdyCombatKitNames::Keys::MaxHp), FString(TEXT("max_hp")));
	TestEqual(TEXT("attack key"), FString(CrowdyCombatKitNames::Keys::Attack), FString(TEXT("attack")));
	TestEqual(TEXT("defense key"), FString(CrowdyCombatKitNames::Keys::Defense), FString(TEXT("defense")));
	TestEqual(TEXT("alive key"), FString(CrowdyCombatKitNames::Keys::Alive), FString(TEXT("alive")));

	TestEqual(TEXT("combatant type suffix"), FString(CrowdyCombatKitNames::CombatantTypeSuffix), FString(TEXT("Combatant")));

	TestEqual(TEXT("hp default"), CrowdyCombatKitNames::Defaults::Hp, 100);
	TestEqual(TEXT("max_hp default"), CrowdyCombatKitNames::Defaults::MaxHp, 100);
	TestEqual(TEXT("attack default"), CrowdyCombatKitNames::Defaults::Attack, 10);
	TestEqual(TEXT("defense default"), CrowdyCombatKitNames::Defaults::Defense, 0);

	return true;
}

// The state parser reads hp/max_hp/attack/defense/alive out of a flat pulled property map and threads the
// container id through. Missing keys leave the struct defaults untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatParseStateTest,
	"CrowdySDK.Kit.CombatParseState", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatParseStateTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("hp"), 42);
	State->SetNumberField(TEXT("max_hp"), 120);
	State->SetNumberField(TEXT("attack"), 15);
	State->SetNumberField(TEXT("defense"), 3);
	State->SetBoolField(TEXT("alive"), true);

	const FCrowdyCombatantState Parsed = UCrowdyGetCombatantStateAction::ParseCombatantState(State, TEXT("c-9"));
	TestEqual(TEXT("hp"), Parsed.Hp, 42);
	TestEqual(TEXT("max_hp"), Parsed.MaxHp, 120);
	TestEqual(TEXT("attack"), Parsed.Attack, 15);
	TestEqual(TEXT("defense"), Parsed.Defense, 3);
	TestTrue(TEXT("alive"), Parsed.bAlive);
	TestEqual(TEXT("container id threaded"), Parsed.ContainerId, FString(TEXT("c-9")));

	// A dead combatant with only hp/alive present; the unread stats stay at their struct defaults.
	TSharedPtr<FJsonObject> Partial = MakeShared<FJsonObject>();
	Partial->SetNumberField(TEXT("hp"), 0);
	Partial->SetBoolField(TEXT("alive"), false);
	const FCrowdyCombatantState Dead = UCrowdyGetCombatantStateAction::ParseCombatantState(Partial, TEXT("c-10"));
	TestEqual(TEXT("hp zero"), Dead.Hp, 0);
	TestFalse(TEXT("not alive"), Dead.bAlive);
	TestEqual(TEXT("max_hp default when absent"), Dead.MaxHp, 0);

	return true;
}

// A forged/out-of-range numeric value must saturate to the int32 bounds, not invoke undefined double-to-int
// narrowing (a garbage sentinel on MSVC). The server never sends these; a hostile/broken peer or replay could.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatParseStateSaturatesTest,
	"CrowdySDK.Kit.CombatParseStateSaturates", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatParseStateSaturatesTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Huge = MakeShared<FJsonObject>();
	Huge->SetNumberField(TEXT("hp"), 5.0e18);       // far above INT32_MAX
	Huge->SetNumberField(TEXT("max_hp"), -5.0e18);  // far below INT32_MIN
	Huge->SetNumberField(TEXT("attack"), 3.0e9);    // above INT32_MAX (2147483647)
	Huge->SetNumberField(TEXT("defense"), -3.0e9);  // below INT32_MIN (-2147483648)

	const FCrowdyCombatantState Parsed = UCrowdyGetCombatantStateAction::ParseCombatantState(Huge, TEXT("c-forged"));
	TestEqual(TEXT("hp saturates to int32 max"), Parsed.Hp, MAX_int32);
	TestEqual(TEXT("max_hp saturates to int32 min"), Parsed.MaxHp, MIN_int32);
	TestEqual(TEXT("attack saturates to int32 max"), Parsed.Attack, MAX_int32);
	TestEqual(TEXT("defense saturates to int32 min"), Parsed.Defense, MIN_int32);

	// An in-range value is unaffected by the clamp.
	TSharedPtr<FJsonObject> InRange = MakeShared<FJsonObject>();
	InRange->SetNumberField(TEXT("hp"), 250);
	const FCrowdyCombatantState Ok = UCrowdyGetCombatantStateAction::ParseCombatantState(InRange, TEXT("c-ok"));
	TestEqual(TEXT("in-range hp passes through"), Ok.Hp, 250);

	return true;
}

// The respawn/revive/sync/apply_effect function names and the StatusEffect type name are derived from the type
// prefix exactly as the vendored combat blueprint's combatNames() derives them: a bare base for an empty prefix,
// else snake_case(prefix) + "_" + base; the effect type is <Prefix>StatusEffect. A drift here would call a
// function/type name the deployed kit never created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatFollowUpNamesTest,
	"CrowdySDK.Kit.CombatFollowUpNames", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatFollowUpNamesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty prefix -> respawn"),
		CrowdyCombatKitNames::RespawnFunctionName(FString()), FString(TEXT("respawn")));
	TestEqual(TEXT("empty prefix -> revive"),
		CrowdyCombatKitNames::ReviveFunctionName(FString()), FString(TEXT("revive")));
	TestEqual(TEXT("empty prefix -> sync_combatant"),
		CrowdyCombatKitNames::SyncFunctionName(FString()), FString(TEXT("sync_combatant")));
	TestEqual(TEXT("empty prefix -> apply_effect"),
		CrowdyCombatKitNames::ApplyEffectFunctionName(FString()), FString(TEXT("apply_effect")));
	TestEqual(TEXT("empty prefix -> StatusEffect"),
		CrowdyCombatKitNames::EffectTypeName(FString()), FString(TEXT("StatusEffect")));

	TestEqual(TEXT("PascalCase prefix -> fire_respawn"),
		CrowdyCombatKitNames::RespawnFunctionName(TEXT("Fire")), FString(TEXT("fire_respawn")));
	TestEqual(TEXT("PascalCase prefix -> fire_revive"),
		CrowdyCombatKitNames::ReviveFunctionName(TEXT("Fire")), FString(TEXT("fire_revive")));
	TestEqual(TEXT("PascalCase prefix -> fire_sync_combatant"),
		CrowdyCombatKitNames::SyncFunctionName(TEXT("Fire")), FString(TEXT("fire_sync_combatant")));
	TestEqual(TEXT("PascalCase prefix -> fire_apply_effect"),
		CrowdyCombatKitNames::ApplyEffectFunctionName(TEXT("Fire")), FString(TEXT("fire_apply_effect")));
	TestEqual(TEXT("PascalCase prefix -> FireStatusEffect"),
		CrowdyCombatKitNames::EffectTypeName(TEXT("Fire")), FString(TEXT("FireStatusEffect")));

	// A multi-word prefix snakes the same way combatNames() does for the function names, while the type name keeps
	// the PascalCase prefix verbatim.
	TestEqual(TEXT("multi-word prefix -> mob_engine_apply_effect"),
		CrowdyCombatKitNames::ApplyEffectFunctionName(TEXT("MobEngine")), FString(TEXT("mob_engine_apply_effect")));
	TestEqual(TEXT("multi-word prefix -> MobEngineStatusEffect"),
		CrowdyCombatKitNames::EffectTypeName(TEXT("MobEngine")), FString(TEXT("MobEngineStatusEffect")));

	return true;
}

// The StatusEffect property keys must match the vendored combat blueprint so an armed effect writes the exact
// properties the deployed kit created and the tick automation selects on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatEffectKeysMatchKitTest,
	"CrowdySDK.Kit.CombatEffectKeysMatchKit", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatEffectKeysMatchKitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("effect_id key"),
		FString(CrowdyCombatKitNames::EffectKeys::EffectId), FString(TEXT("effect_id")));
	TestEqual(TEXT("target_key key"),
		FString(CrowdyCombatKitNames::EffectKeys::TargetKey), FString(TEXT("target_key")));
	TestEqual(TEXT("magnitude key"),
		FString(CrowdyCombatKitNames::EffectKeys::Magnitude), FString(TEXT("magnitude")));
	TestEqual(TEXT("ticks_left key"),
		FString(CrowdyCombatKitNames::EffectKeys::TicksLeft), FString(TEXT("ticks_left")));

	TestEqual(TEXT("status effect type suffix"),
		FString(CrowdyCombatKitNames::StatusEffectTypeSuffix), FString(TEXT("StatusEffect")));

	return true;
}

// apply_effect's params: target_key and effect_id are JSON strings, magnitude and ticks are JSON numbers. The
// automation joins on target_key, so it must not be coerced to a number even when the value is all digits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatApplyEffectParamsJsonTest,
	"CrowdySDK.Kit.CombatApplyEffectParamsJson", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatApplyEffectParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params =
		UCrowdyApplyStatusEffectAction::BuildApplyEffectParams(TEXT("ck-42"), TEXT("poison"), 7, 3);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> TargetKey = Params->TryGetField(TEXT("target_key"));
	if (TestNotNull(TEXT("target_key present"), TargetKey.Get()))
	{
		TestEqual(TEXT("target_key is a JSON string"),
			static_cast<int32>(TargetKey->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("target_key value"), TargetKey->AsString(), FString(TEXT("ck-42")));
	}

	const TSharedPtr<FJsonValue> EffectId = Params->TryGetField(TEXT("effect_id"));
	if (TestNotNull(TEXT("effect_id present"), EffectId.Get()))
	{
		TestEqual(TEXT("effect_id is a JSON string"),
			static_cast<int32>(EffectId->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("effect_id value"), EffectId->AsString(), FString(TEXT("poison")));
	}

	const TSharedPtr<FJsonValue> Magnitude = Params->TryGetField(TEXT("magnitude"));
	if (TestNotNull(TEXT("magnitude present"), Magnitude.Get()))
	{
		TestEqual(TEXT("magnitude is a JSON number"),
			static_cast<int32>(Magnitude->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("magnitude value"), static_cast<int32>(Magnitude->AsNumber()), 7);
	}

	const TSharedPtr<FJsonValue> Ticks = Params->TryGetField(TEXT("ticks"));
	if (TestNotNull(TEXT("ticks present"), Ticks.Get()))
	{
		TestEqual(TEXT("ticks is a JSON number"),
			static_cast<int32>(Ticks->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("ticks value"), static_cast<int32>(Ticks->AsNumber()), 3);
	}

	// An all-digit combat_key still round-trips as a string, never coerced to a number.
	const TSharedPtr<FJsonObject> NumericLikeKey =
		UCrowdyApplyStatusEffectAction::BuildApplyEffectParams(TEXT("12345"), TEXT("burn"), 1, 1);
	const TSharedPtr<FJsonValue> NumericTargetKey = NumericLikeKey->TryGetField(TEXT("target_key"));
	if (TestNotNull(TEXT("numeric-like target_key present"), NumericTargetKey.Get()))
	{
		TestEqual(TEXT("all-digit target_key stays a string"),
			static_cast<int32>(NumericTargetKey->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("all-digit target_key value"), NumericTargetKey->AsString(), FString(TEXT("12345")));
	}

	return true;
}

// sync_combatant's hp param is a JSON number, not a string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCombatSyncParamsJsonTest,
	"CrowdySDK.Kit.CombatSyncParamsJson", CrowdyCombatKitTestFlags)
bool FCrowdyKitCombatSyncParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = UCrowdySyncCombatantAction::BuildSyncParams(88);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Hp = Params->TryGetField(TEXT("hp"));
	if (TestNotNull(TEXT("hp present"), Hp.Get()))
	{
		TestEqual(TEXT("hp is a JSON number"),
			static_cast<int32>(Hp->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("hp value"), static_cast<int32>(Hp->AsNumber()), 88);
	}

	TestEqual(TEXT("serialized shape"), CrowdyCombatSerializeObject(Params), FString(TEXT("{\"hp\":88}")));

	return true;
}

#endif
