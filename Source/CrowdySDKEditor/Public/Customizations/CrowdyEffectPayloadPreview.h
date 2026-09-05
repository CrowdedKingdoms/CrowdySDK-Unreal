// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

struct FCrowdyEffectLoweringResult;

/**
 * Pure text rendering of a compiled effect's deploy payload: what this asset actually sends to the Game Model server
 * on a schema sync. It is the asset's full authored upsert set (the function, and, when the effect runs itself, the
 * automation and its event trigger), NOT a server diff - it makes no server call and is deterministic from the
 * lowering result alone, so it is headless-testable.
 *
 * Two modes: the exact wire JSON (the pretty-printed gameModelUpsert* input objects, byte-for-byte what deploy sends,
 * built through the same shared marshaller the Studio sync uses) and a readable summary (function name, params,
 * mutations, notifications, automation) for a quick scan. appId is resolved at deploy time, not authoring time, so the
 * preview shows a "<appId>" placeholder.
 */
namespace CrowdyEffectPayloadPreview
{
	enum class EMode : uint8
	{
		Summary,
		WireJson,
	};

	// The placeholder shown wherever the real appId is filled in at deploy time.
	extern const TCHAR* const AppIdPlaceholder;

	// Render the payload preview for a compiled effect. When the effect has compile errors, returns the diagnostics in
	// place of a payload (there is nothing shippable to preview). EffectiveFunctionName is only used for labels; the
	// payload contents come entirely from Result.
	FString BuildPayloadPreview(const FCrowdyEffectLoweringResult& Result, const FString& EffectiveFunctionName, EMode Mode);
}
