// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * The neutral, engine-side representation of a Game Model automation upsert (the server's UpsertAutomationInput).
 * An automation is a server-driven process that invokes a model function on its own, on a schedule or in reaction
 * to model activity (an NPC tick, a spawner, a world job). It is what an effect that opts into running
 * automatically lowers to, alongside its function input, and what the schema sync (in CrowdyStudio) turns into the
 * actual gameModelUpsertAutomation GraphQL call.
 *
 * It lives in CrowdyReplication, not CrowdyStudio, for the same reason FCrowdyGameModelFunctionInput does: the
 * lowering core is engine-side and reused by every effect front-end; CrowdyStudio depends on CrowdyReplication
 * (one-way), so the sync consumes this without CrowdyReplication learning about GraphQL.
 *
 * Field-for-field this mirrors the UpsertAutomationInput wire shape (see cks-docs game-api). Wire enum fields are
 * plain strings in the exact server vocabulary (targetMode "container|type|global", triggerType
 * "schedule|event|manual", scheduleKind "interval|cron"). The budget fields carry explicit defaults and are always
 * emitted, so a re-sync read-back matches and the diff stays idempotent. JSON-typed fields carry no server
 * byte-stability guarantee, so downstream comparison is semantic (the schema sync canonicalizes before diffing).
 */
struct FCrowdyGameModelAutomationInput
{
	// The automation name, unique per app; the upsert key.
	FString Name;

	FString Description;

	// Whether the automation is eligible to run. Defaults true.
	bool bEnabled = true;

	// "model_function" (invoke a Model function; the only kind this authoring surface emits) or "compute_invoke".
	FString ActionKind = TEXT("model_function");

	// The entry-point function name (must be autonomous_invocable). Required for actionKind=model_function.
	FString FunctionName;

	// "container" | "type" | "global". Defaults to "type" for an effect-authored automation (fan out over every
	// container of the effect's target type).
	FString TargetMode = TEXT("type");

	// For targetMode=container: the specific self container UUID. Empty otherwise.
	FString SelfContainerId;

	// For targetMode=type: the container type to fan out over (the effect's own container type by default).
	FString TargetTypeName;

	// Optional session scope (UUID). Empty = app-global.
	FString SessionId;

	// A JSON object of static params passed to the entry point. Empty = "{}" (the server default).
	FString ParamsJson;

	// A JSON selector resolving candidate refs/scalars over model data into params. Empty when unused. Not authored
	// by this pass (parking lot), but carried so a server-authored selector round-trips.
	FString SelectorJson;

	// "schedule" | "event" | "manual". Defaults to "schedule".
	FString TriggerType = TEXT("schedule");

	// For schedule triggers: "interval" | "cron".
	FString ScheduleKind = TEXT("interval");

	// Interval in ms (scheduleKind=interval).
	int32 IntervalMs = 1000;

	// Cron expression (scheduleKind=cron). Empty for interval.
	FString CronExpr;

	// The safety budget. Explicit defaults, always emitted (idempotency).
	int32 MaxTargets = 50;
	int32 GasLimit = 100000;
	int32 RunTimeoutMs = 2000;
	int32 MaxRunsPerMinute = 120;
	int32 FailureThreshold = 5;
	int32 CooldownMs = 30000;
};

/**
 * The neutral representation of a Game Model automation event trigger (the server's UpsertAutomationTriggerInput):
 * fires an automation in reaction to model activity. Emitted alongside an automation input when an effect chooses
 * the event trigger mode.
 */
struct FCrowdyGameModelAutomationTriggerInput
{
	// The automation (by name) this trigger fires.
	FString AutomationName;

	// The model event: "function_invoked" | "property_changed" | "container_created".
	FString OnEvent;

	// Filter: only this function name. Empty = any.
	FString FunctionName;

	// Filter: only this container type. Empty = any.
	FString ContainerTypeName;

	// Filter: only this property key (for property_changed). Empty = any.
	FString PropertyKey;

	// Filter: which writes count as a change, for property_changed only. "any" | "direct" | "function"; empty omits
	// the field so the server applies its own default. The server rejects a filter the event cannot match, so this
	// stays empty for every other event.
	FString WriteSource;

	// Debounce / coalesce window in ms.
	int32 DebounceMs = 0;
};
