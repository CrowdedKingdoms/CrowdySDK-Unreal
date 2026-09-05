// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * The neutral, engine-side representation of a Game Model function upsert (the server's UpsertFunctionInput).
 * It is what the EffectScript lowering produces and what the schema sync (in CrowdyStudio) turns into the actual
 * gameModelUpsertFunction GraphQL call. It lives in CrowdyReplication, not CrowdyStudio, because the lowering
 * core is engine-side and reused by every effect front-end (text, picker, optional typed-C++); CrowdyStudio
 * depends on CrowdyReplication (one-way), so the sync can consume this without CrowdyReplication ever learning
 * about GraphQL.
 *
 * Field-for-field this mirrors the UpsertFunctionInput wire shape (see cks-docs game-models.md): scalar header
 * fields, a JSON invoke-policy string, and the parameters / mutations / notifications arrays. JSON-typed fields
 * carry no server byte-stability guarantee, so downstream comparison is always semantic (the schema sync
 * canonicalizes before diffing).
 */
struct FCrowdyGameModelFunctionParam
{
	// The parameter name as referenced in expressions with a leading '$' (stored here without the sigil).
	FString Name;

	// "int" | "float" | "bool" | "string" | "container_ref".
	FString ValueType;

	bool bRequired = true;

	// A JSON-encoded default value ("5", "\"text\"", "true"); empty when the parameter is required / has none.
	FString DefaultValueJson;

	FString Description;

	int32 SortOrder = 0;
};

// One declared write the function performs: set Property on Target to the pure expression Expression.
struct FCrowdyGameModelMutation
{
	// "self" or "ref($<param>)" / "ref(\"<uuid>\")" the container this write targets.
	FString Target;

	// The server property key (lowercased attribute name) being written.
	FString Property;

	// The pure DSL expression producing the new value (already clamp-wrapped and key-resolved by lowering).
	FString Expression;
};

// One model-driven change notification a function declares so peers know to re-pull. Shape per cks-docs
// model-driven-notifications.md.
struct FCrowdyGameModelNotificationArg
{
	FString Name;
	FString Expression;
};

struct FCrowdyGameModelNotification
{
	// "spatial" | "channel" | "actor".
	FString Kind;

	// Optional server-side emit-as label; empty when unused.
	FString EmitAs;

	TArray<FCrowdyGameModelNotificationArg> Args;
};

// One parameter bound into a delayed invocation. The expression is evaluated when the timer is ARMED, not when it
// fires, so it captures the value the arming run decided.
struct FCrowdyGameModelTimerParam
{
	FString Name;

	FString Expression;
};

// One declarative delayed invocation a function arms: "run FunctionName again in N ms". The server arms it in the
// same transaction as the function's mutations, so a rolled-back invocation schedules nothing and a committed one
// is guaranteed to fire even across an API restart. The target function must be autonomous-invocable, because the
// fire is headless with no player in the request.
struct FCrowdyGameModelTimer
{
	// The function to invoke when the timer fires. May be the arming function itself.
	FString FunctionName;

	// The container the delayed invocation runs against, e.g. "self" (the default when empty) or "$target_id".
	FString Target;

	// Expression resolving the delay in milliseconds. Model-expression source, not a literal, so a plain number is
	// emitted as its own digits rather than quoted.
	FString DelayMsExpression;

	// Optional expression resolving an app-scoped key. Re-arming the same key REPLACES the pending timer instead of
	// queueing a second fire, which is both the "reset the countdown" behaviour and the guard that stops a hot
	// function flooding the timer queue. Also expression source: a literal key has to arrive already quoted.
	FString DedupeKeyExpression;

	TArray<FCrowdyGameModelTimerParam> Params;
};

// The whole function upsert. Produced by FCrowdyEffectLowering::Lower; consumed by a later schema sync.
struct FCrowdyGameModelFunctionInput
{
	// The function name (from the effect asset).
	FString Name;

	// The container type the function is bound to (from the target attribute's owning CrowdyContainer class).
	FString ContainerTypeName;

	FString Description;

	// The declared type of the value an invocation answers with ("int" | "float" | "bool" | "string"); empty when
	// the function answers with nothing, or when no type could be determined for the value it returns.
	FString ReturnType;

	// "player" (default) | "server" | "internal". Set from the effect's Callable From control; internal means the
	// function is reachable only through a fn: call inside another function's expression.
	FString InvokeScope = TEXT("player");

	// Whether an automation (autonomous process) may run this function "as the server". Off for an ordinary
	// player-invoked effect; set by an effect that opts into running automatically. Mirrors the server's
	// UpsertFunctionInput.autonomousInvocable and is orthogonal to InvokeScope.
	bool bAutonomousInvocable = false;

	// The pure expression whose value becomes the invoke result. The server evaluates it after every mutation has
	// run, so it reads the values this invocation just wrote. Empty when the function answers with nothing.
	FString ReturnExpression;

	// The invoke-policy boolean tree as a JSON string ({"type":"and","rules":[...]} etc.); empty means no
	// policy (any entitled player may invoke). Emitted in the exact server leaf/combinator vocabulary.
	FString InvokePolicyJson;

	TArray<FCrowdyGameModelFunctionParam> Parameters;

	TArray<FCrowdyGameModelMutation> Mutations;

	// The model-changed notifications this function declares. The effect lowering authors these; the schema sync
	// fills any runtime-resolved value (such as a channel id) before sending them.
	TArray<FCrowdyGameModelNotification> Notifications;

	// The delayed invocations this function arms when it commits. Unlike notifications, these need no runtime
	// resolution, so the effect is their sole author and a sync replaces the server's set wholesale.
	TArray<FCrowdyGameModelTimer> Timers;
};
