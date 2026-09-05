// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

struct FCrowdyGameModelFunctionInput;
struct FCrowdyGameModelAutomationInput;
struct FCrowdyGameModelAutomationTriggerInput;
class FJsonObject;

// Shared JSON marshalling for the Game Model function upsert path. Turns the neutral, engine-side
// FCrowdyGameModelFunctionInput into the server's UpsertFunctionInput JSON object. It lives in CrowdyReplication,
// next to the input struct, so every consumer that already depends on CrowdyReplication (the Studio schema sync and
// the editor lowered-payload preview) shares one definition instead of re-declaring it. The output is a pure
// function of its inputs, so it is safe to call off the game thread and easy to golden-test.
namespace CrowdyGameModelMarshalling
{
	// Build one UpsertFunctionInput from a compiled effect's neutral function input, sending every authored field
	// explicitly so the server read-back matches and a follow-up plan is a genuine no-op. The effect owns
	// name/container/params/mutations/policy/return/scope and its model-changed notifications; the caller's diff decides
	// the final notification set and this serializes it as-is. AppId travels as a decimal string (BigInt), never a JSON
	// number.
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> BuildFunctionUpsertInput(const FCrowdyGameModelFunctionInput& Fn, int64 AppId);

	// Build the UpsertAutomationInput object from a compiled effect's neutral automation input (the payload an effect
	// that opts into running itself sends alongside its function). Returns the bare input object, matching
	// BuildFunctionUpsertInput's convention; the Studio sync wraps it in the mutation's { input: ... } variables, and
	// the editor payload preview serializes it directly. AppId travels as a decimal string (BigInt).
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> BuildAutomationUpsertInput(const FCrowdyGameModelAutomationInput& Automation, int64 AppId);

	// Build the UpsertAutomationTriggerInput object from a compiled effect's neutral trigger input (emitted for a
	// property-change automation). Same input-object convention as above.
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> BuildAutomationTriggerUpsertInput(const FCrowdyGameModelAutomationTriggerInput& Trigger, int64 AppId);
}
