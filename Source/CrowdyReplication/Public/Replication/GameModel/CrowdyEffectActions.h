// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyEffectActions.generated.h"

class UCrowdyEffect;

// Succeeded fires only on a COMMITTED effect apply (reached the server AND passed its rules). A transport
// failure OR a server-side rollback both route to Failed, which carries bSuccess + ErrorMessage, so a
// rolled-back apply (a logic/authority failure, not a network failure) is distinguishable without Succeeded
// ever firing on a rejected mutation. A synchronous resolve/marshal failure also routes to Failed, and so does an
// apply that was still waiting in a coalescing window when the world tore down, which is never sent at all.
// Exactly one of the two always fires, so the node never leaks.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyApplyEffectOutcome, bool, bSuccess, FString, ReturnValueJson, FString, ErrorMessage);

/**
 * Applies an authored Game Model effect to Target (the affected container), optionally caused by Source, with
 * Success/Failed pins. The designer-facing runtime entry (Example 1: Apply(TakeDamage, Victim, Attacker)):
 * Target and Source are any registered participant (an actor, or a Host-owned subsystem enrolled as a
 * participant); it resolves both objects' bound containers off their entity NetIDs, marshals the effect's
 * magnitudes (+ source_id when Source is set) into invoke params, and routes to the subsystem's InvokeAndApply
 * seam. Overrides replaces a magnitude by Name (a JSON-encoded literal). Shares UCrowdyEffects::ApplyInternal
 * with the fire-and-forget library so both paths resolve + marshal identically.
 *
 * An effect that opts into coalescing may have this apply merged with other applies of the same effect to the same
 * target and sent as one server call, so the pins can fire later than they would otherwise: up to the effect's
 * coalesce window, and longer while the server's invoke allowance is under pressure. Every merged apply still gets
 * its own pin fired from that one call's outcome, so several of these nodes can be waiting on the same invoke and
 * all of them complete. Nothing about the pins changes, only when they fire.
 */
UCLASS(meta = (HasDedicatedAsyncNode))
class CROWDYREPLICATION_API UCrowdyApplyEffectAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Effects")
	FCrowdyApplyEffectOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Effects")
	FCrowdyApplyEffectOutcome Failed;

	// DefaultToSelf on Target: applying an effect to the Blueprint that is running is the overwhelmingly common
	// case, so an unwired Target resolves to self rather than to null (which would silently do nothing).
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AutoCreateRefTerm = "Overrides", DefaultToSelf = "Target"), Category = "Crowdy SDK|Game Model|Effects",
		DisplayName = "Apply Crowdy Effect")
	static UCrowdyApplyEffectAction* ApplyEffect(UObject* WorldContext, UCrowdyEffect* Effect, UObject* Target,
		UObject* Source, const TMap<FName, FString>& Overrides, float Level = 1.0f, const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<UCrowdyEffect> Effect;
	TWeakObjectPtr<UObject> Target;
	TWeakObjectPtr<UObject> Source;
	TMap<FName, FString> Overrides;
	float Level = 1.0f;
	FString SessionId;
};
