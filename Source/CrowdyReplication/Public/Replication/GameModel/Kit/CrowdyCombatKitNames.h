// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// The Combat kit's server-side names, mirrored from the vendored CrowdyCPP combat blueprint so the Blueprint
// nodes address exactly the type/function/property names a deployed Combat kit created. Kept in one place so a
// node and the deployed schema cannot drift. The runtime nodes compose the existing Game Model subsystem seam
// rather than calling the vendored kit's blocking helpers, so only the names are shared, not the code path.
namespace CrowdyCombatKitNames
{
	// A combatant type name is <TypePrefix>Combatant (empty prefix -> "Combatant").
	inline constexpr const TCHAR* CombatantTypeSuffix = TEXT("Combatant");

	// A status-effect type name is <TypePrefix>StatusEffect (empty prefix -> "StatusEffect").
	inline constexpr const TCHAR* StatusEffectTypeSuffix = TEXT("StatusEffect");

	// The base combatant/effect function names. With a non-empty prefix the deployed function is
	// snake_case(prefix) + "_" + base (for example "fire_attack", "fire_apply_effect").
	inline constexpr const TCHAR* AttackFunctionBase = TEXT("attack");
	inline constexpr const TCHAR* RespawnFunctionBase = TEXT("respawn");
	inline constexpr const TCHAR* ReviveFunctionBase = TEXT("revive");
	inline constexpr const TCHAR* SyncFunctionBase = TEXT("sync_combatant");
	inline constexpr const TCHAR* ApplyEffectFunctionBase = TEXT("apply_effect");

	// Combatant property keys. These are the same regardless of the type prefix (only the type and function names
	// carry the prefix), so a reader never needs the prefix.
	namespace Keys
	{
		inline constexpr const TCHAR* OwnerUserId = TEXT("owner_user_id");
		inline constexpr const TCHAR* CombatKey = TEXT("combat_key");
		inline constexpr const TCHAR* Hp = TEXT("hp");
		inline constexpr const TCHAR* MaxHp = TEXT("max_hp");
		inline constexpr const TCHAR* Attack = TEXT("attack");
		inline constexpr const TCHAR* Defense = TEXT("defense");
		inline constexpr const TCHAR* Alive = TEXT("alive");
	}

	// StatusEffect property keys. Like the combatant keys these carry no prefix (only the type and function names
	// do). owner_user_id is the shared owner mirror (Keys::OwnerUserId). target_key joins an effect to the
	// combatant whose combat_key it matches.
	namespace EffectKeys
	{
		inline constexpr const TCHAR* EffectId = TEXT("effect_id");
		inline constexpr const TCHAR* TargetKey = TEXT("target_key");
		inline constexpr const TCHAR* Magnitude = TEXT("magnitude");
		inline constexpr const TCHAR* TicksLeft = TEXT("ticks_left");
	}

	// Combatant stat defaults, mirrored from combat.hpp's property definitions.
	namespace Defaults
	{
		inline constexpr int32 Hp = 100;
		inline constexpr int32 MaxHp = 100;
		inline constexpr int32 Attack = 10;
		inline constexpr int32 Defense = 0;
	}

	// Mirror of crowdy::kit::toSnakeCase: a space or hyphen becomes a single underscore, an uppercase letter
	// preceded by a lowercase letter or digit gets an underscore before it, and everything is lowercased. Used to
	// derive the prefixed function names, so a prefix maps to the identical name the kit deployment produced.
	inline FString ToSnakeCase(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len() + 4);
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR C = Name[Index];
			if (C == TEXT(' ') || C == TEXT('-'))
			{
				if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
				{
					Out.AppendChar(TEXT('_'));
				}
				continue;
			}
			if (FChar::IsUpper(C))
			{
				if (Index > 0 && (FChar::IsLower(Name[Index - 1]) || FChar::IsDigit(Name[Index - 1])))
				{
					Out.AppendChar(TEXT('_'));
				}
				Out.AppendChar(FChar::ToLower(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	// The full Combatant container type name for a PascalCase prefix: <Prefix>Combatant.
	inline FString CombatantTypeName(const FString& TypePrefix)
	{
		return TypePrefix + CombatantTypeSuffix;
	}

	// The full StatusEffect container type name for a PascalCase prefix: <Prefix>StatusEffect.
	inline FString EffectTypeName(const FString& TypePrefix)
	{
		return TypePrefix + StatusEffectTypeSuffix;
	}

	// The full function name for a base and a prefix: the bare base for an empty prefix, else
	// snake_case(prefix) + "_" + base. This is the identical rule the kit deployment used to name its functions.
	inline FString PrefixedFunctionName(const FString& TypePrefix, const TCHAR* FunctionBase)
	{
		if (TypePrefix.IsEmpty())
		{
			return FunctionBase;
		}
		return ToSnakeCase(TypePrefix) + TEXT("_") + FunctionBase;
	}

	// The full attack function name for a prefix: "attack" for an empty prefix, else snake_case(prefix) + "_attack".
	inline FString AttackFunctionName(const FString& TypePrefix)
	{
		return PrefixedFunctionName(TypePrefix, AttackFunctionBase);
	}

	// respawn: mutates the combatant itself back to full hp (owner-of-self gated, only while downed).
	inline FString RespawnFunctionName(const FString& TypePrefix)
	{
		return PrefixedFunctionName(TypePrefix, RespawnFunctionBase);
	}

	// revive: mutates a downed combatant back to full hp, gated by a team/group permission held by the caller.
	// Only exists when the kit was deployed with a revive group.
	inline FString ReviveFunctionName(const FString& TypePrefix)
	{
		return PrefixedFunctionName(TypePrefix, ReviveFunctionBase);
	}

	// sync_combatant: persists host-simulated hp, is_host gated. Only exists when the kit was deployed hostSynced.
	inline FString SyncFunctionName(const FString& TypePrefix)
	{
		return PrefixedFunctionName(TypePrefix, SyncFunctionBase);
	}

	// apply_effect: arms a status effect against a target combat_key; the interval automation ticks it server-side.
	inline FString ApplyEffectFunctionName(const FString& TypePrefix)
	{
		return PrefixedFunctionName(TypePrefix, ApplyEffectFunctionBase);
	}
}
