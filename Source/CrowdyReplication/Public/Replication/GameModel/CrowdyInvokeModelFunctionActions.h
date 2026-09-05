// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyInvokeModelFunctionActions.generated.h"

class FJsonObject;

// Succeeded fires only on a COMMITTED call (reached the server AND passed its logic/authority checks). A transport
// failure OR a server-side rollback both route to Failed, which still carries bSuccess + ErrorMessage, so a
// rolled-back call (a logic failure, not a network failure) is distinguishable without Succeeded ever firing on a
// rejected mutation. A synchronous resolve failure also routes to Failed.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyCallModelFunctionOutcome, bool, bSuccess, FString, ReturnValueJson, FString, ErrorMessage);

/**
 * Calls a server function on a Game Model, with Success/Failed pins and typed params. The general-purpose runtime
 * entry when an authored Crowdy Effect is more than you need: name the function and pass its params directly.
 *
 * Target is any registered participant (an actor carrying a UCrowdyEntityComponent, or a UObject enrolled as a
 * participant such as a Host-owned subsystem); its bound model is resolved off the entity NetID. Leave Target empty
 * and pass ContainerId to call a free/data model (an inventory, a quest) by id. On success the confirmed result
 * echoes into the cache + OnRep and peers are notified to re-pull (the fallback model-changed ping).
 *
 * Params maps each function parameter name to a JSON-encoded value. There is no per-parameter type declaration
 * here (unlike a Crowdy Effect's magnitudes), so each value is parsed and typed by its shape: "5" is a number,
 * "true"/"false" a boolean, "null" a null, "\"text\"" a string. A value that is not valid JSON is sent as a
 * string, so a bare word or a bare id round-trips without quotes. To force a value that looks like a number,
 * bool, or null to be a string (an all-digit id, the literal word null), wrap it in quotes: "\"12345\"".
 *
 * This node calls a function directly and adds no model-changed notification of its own. When it invokes an
 * authored effect's function whose notification names the container via the server-injected $self_container_id,
 * that notification still fires; otherwise peers refresh via the fallback ping. For an authored effect with a
 * channel notification, use Apply Crowdy Effect instead.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyInvokeModelFunctionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyCallModelFunctionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyCallModelFunctionOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AutoCreateRefTerm = "Params"), Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Call Model Function")
	static UCrowdyInvokeModelFunctionAction* CallModelFunction(UObject* WorldContext, UObject* Target,
		const FString& ContainerId, const FString& FunctionName, const TMap<FName, FString>& Params,
		const FString& SessionId = FString());

	// The pure marshaller: build the invoke params object from a parameter-name -> JSON-literal map. Each value is
	// parsed as a JSON value; a value that is not valid JSON is emitted as a JSON string, so a bare word or id
	// round-trips without quotes. An entry with a None key is skipped. Never fails (an empty map yields an empty
	// object). Public + static so it is headless-testable with no world, no subsystem, no HTTP.
	static TSharedPtr<FJsonObject> BuildParamsJson(const TMap<FName, FString>& Params);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<UObject> Target;
	FString ContainerId;
	FString FunctionName;
	TMap<FName, FString> Params;
	FString SessionId;
};
