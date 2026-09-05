// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// The Worldsim kit's server-side names, mirrored from the vendored CrowdyCPP worldsim blueprint so the Blueprint
// nodes address exactly the type/function/property names a deployed Worldsim kit created. Kept in one place so a
// node and the deployed schema cannot drift. The runtime nodes compose the existing Game Model subsystem seam
// rather than calling the vendored kit's blocking helpers, so only the names are shared, not the code path.
namespace CrowdyWorldsimKitNames
{
	// Container type name suffixes. A type name is <TypePrefix><Suffix> (empty prefix -> the bare suffix).
	inline constexpr const TCHAR* WorldStateTypeSuffix = TEXT("WorldState");
	inline constexpr const TCHAR* ResourceNodeTypeSuffix = TEXT("ResourceNode");
	inline constexpr const TCHAR* CropTypeSuffix = TEXT("Crop");
	inline constexpr const TCHAR* WaveSpawnerTypeSuffix = TEXT("WaveSpawner");

	// Base function names. With a non-empty prefix the deployed function is snake_case(prefix) + "_" + base.
	inline constexpr const TCHAR* AdvanceTimeFunctionBase = TEXT("advance_time");
	inline constexpr const TCHAR* SetWeatherFunctionBase = TEXT("set_weather");
	inline constexpr const TCHAR* RegenNodeFunctionBase = TEXT("regen_node");
	inline constexpr const TCHAR* GatherNodeFunctionBase = TEXT("gather_node");
	inline constexpr const TCHAR* GrowCropFunctionBase = TEXT("grow_crop");
	inline constexpr const TCHAR* HarvestFunctionBase = TEXT("harvest");
	inline constexpr const TCHAR* SpawnWaveFunctionBase = TEXT("spawn_wave");

	// Property keys. These are the same regardless of the type prefix (only the type and function names carry the
	// prefix), so a reader never needs the prefix.
	namespace Keys
	{
		// WorldState singleton.
		inline constexpr const TCHAR* TimeOfDay = TEXT("time_of_day");
		inline constexpr const TCHAR* Day = TEXT("day");
		inline constexpr const TCHAR* Weather = TEXT("weather");
		inline constexpr const TCHAR* Cx = TEXT("cx");
		inline constexpr const TCHAR* Cy = TEXT("cy");
		inline constexpr const TCHAR* Cz = TEXT("cz");

		// ResourceNode.
		inline constexpr const TCHAR* NodeId = TEXT("node_id");
		inline constexpr const TCHAR* ResourceItemId = TEXT("resource_item_id");
		inline constexpr const TCHAR* Amount = TEXT("amount");
		inline constexpr const TCHAR* MaxAmount = TEXT("max_amount");
		inline constexpr const TCHAR* RegenRate = TEXT("regen_rate");
		inline constexpr const TCHAR* X = TEXT("x");
		inline constexpr const TCHAR* Y = TEXT("y");
		inline constexpr const TCHAR* Z = TEXT("z");

		// Crop.
		inline constexpr const TCHAR* OwnerUserId = TEXT("owner_user_id");
		inline constexpr const TCHAR* Stage = TEXT("stage");
		inline constexpr const TCHAR* MaxStage = TEXT("max_stage");
		inline constexpr const TCHAR* OutputItemId = TEXT("output_item_id");
		inline constexpr const TCHAR* OutputQty = TEXT("output_qty");

		// WaveSpawner.
		inline constexpr const TCHAR* Wave = TEXT("wave");
		inline constexpr const TCHAR* NextWaveSize = TEXT("next_wave_size");
	}

	// Invoke parameter names for the player-facing functions.
	namespace Params
	{
		inline constexpr const TCHAR* Amount = TEXT("amount");
		inline constexpr const TCHAR* ToStackId = TEXT("to_stack_id");
	}

	// Property defaults, mirrored from worldsim.hpp's property definitions.
	namespace Defaults
	{
		inline constexpr int32 MaxAmount = 100;
		inline constexpr int32 RegenRate = 1;
		inline constexpr int32 MaxStage = 3;
		inline constexpr int32 OutputQty = 1;
		inline constexpr int32 NextWaveSize = 5;
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

	// A function name for a prefix: the bare base for an empty prefix, else snake_case(prefix) + "_" + base.
	inline FString FunctionNameForPrefix(const FString& TypePrefix, const TCHAR* FunctionBase)
	{
		if (TypePrefix.IsEmpty())
		{
			return FString(FunctionBase);
		}
		return ToSnakeCase(TypePrefix) + TEXT("_") + FunctionBase;
	}

	// Full container type names for a PascalCase prefix: <Prefix><Suffix>.
	inline FString WorldStateTypeName(const FString& TypePrefix)
	{
		return TypePrefix + WorldStateTypeSuffix;
	}

	inline FString ResourceNodeTypeName(const FString& TypePrefix)
	{
		return TypePrefix + ResourceNodeTypeSuffix;
	}

	inline FString CropTypeName(const FString& TypePrefix)
	{
		return TypePrefix + CropTypeSuffix;
	}

	inline FString WaveSpawnerTypeName(const FString& TypePrefix)
	{
		return TypePrefix + WaveSpawnerTypeSuffix;
	}

	// Full player-facing function names for a prefix.
	inline FString GatherNodeFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, GatherNodeFunctionBase);
	}

	inline FString HarvestFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, HarvestFunctionBase);
	}

	// The automation-only / admin function names, provided for completeness and name-parity tests. The player
	// nodes never call these (they are automation- or admin-gated server-side).
	inline FString AdvanceTimeFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, AdvanceTimeFunctionBase);
	}

	inline FString SetWeatherFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, SetWeatherFunctionBase);
	}

	inline FString RegenNodeFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, RegenNodeFunctionBase);
	}

	inline FString GrowCropFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, GrowCropFunctionBase);
	}

	inline FString SpawnWaveFunctionName(const FString& TypePrefix)
	{
		return FunctionNameForPrefix(TypePrefix, SpawnWaveFunctionBase);
	}
}
