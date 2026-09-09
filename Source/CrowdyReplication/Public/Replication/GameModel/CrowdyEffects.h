// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyEffects.generated.h"

class UCrowdyEffect;
class FJsonObject;
struct FCrowdyInvokeResult;

/**
 * Blueprint access to APPLYING an authored Game Model effect, mirroring UCrowdyModel: a function library that
 * takes the objects you already have, never a new component. Target and Source are any registered participant -
 * an actor carrying a UCrowdyEntityComponent, or a UObject enrolled as a participant (a Host-owned subsystem).
 * Apply(Effect, Target, Source) resolves both objects' bound containers off their entity NetIDs, marshals the
 * effect's tuning magnitudes (+ source_id when a Source is given) into invoke params, and routes to the
 * subsystem's InvokeAndApply seam. The server evaluates the effect's rules transactionally; on success the
 * confirmed result echoes into the cache + OnRep here and peers are notified to re-pull. Fire-and-forget; the
 * latent node UCrowdyApplyEffectAction carries Success/Failed.
 *
 * An effect that opts into coalescing is not sent immediately: repeated applies to the same target inside its
 * window are summed and sent as one server call, because the server admits only a limited number of Game Model
 * calls per player per app and an autofire weapon spends that allowance in seconds. Coalescing is off by default,
 * so every effect that does not ask for it behaves exactly as it always has. See UCrowdyEffect::bCoalescable for
 * what has to match before two applies merge, and ApplyInternal for what changes about timing and retries.
 *
 * source_id is supplied iff a Source object is passed. An effect that reads source.<attr> needs it; a pure-self
 * effect does not. The asset records this as bRequiresSource (recomputed from the lowering at author time), so
 * BuildInvokeParams rejects a missing Source client-side with a clear error instead of letting the server fail on
 * the absent required param. Apply still never compiles on the hot path -- the persisted flag + the function name +
 * magnitudes + the fixed source_id rule fully determine the invoke.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyEffects : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Encode a single typed value as the JSON-literal form an Overrides entry expects: an int/float becomes a bare
	// number, a bool becomes true/false, and a string becomes a JSON-quoted (escaped) string. These are the pieces
	// the smart Apply Crowdy Effect node uses to assemble its Overrides map from typed input pins, so a designer
	// never types JSON by hand. Pure; safe on any thread.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString JsonFromInt(int32 Value);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString JsonFromFloat(double Value);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString JsonFromBool(bool Value);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString JsonFromString(const FString& Value);

	// Decode an invoke's ReturnValueJson back to the typed value the effect declared it answers with: the exact
	// inverse of the JsonFrom* encoders above, and the pieces the smart Apply Crowdy Effect node splices behind its
	// typed Return Value pin. Each treats the payload as forged and answers with the type's zero value when it is
	// empty, malformed, too deeply nested, or a different JSON type, so a server that answered nothing and a server
	// that answered zero read alike here; the node's raw Return Value Json pin is what distinguishes them. Pure;
	// safe on any thread.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static int32 JsonToInt(const FString& ValueJson);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static double JsonToFloat(const FString& ValueJson);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static bool JsonToBool(const FString& ValueJson);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString JsonToString(const FString& ValueJson);

	// Resolve a registered participant to its bound Game Model container id, for a container_ref Override entry.
	// Returns empty when Object is null, has no world, no Game Model subsystem, or is not a bound entity. The bare
	// id (not a JSON string) is what an Overrides value for a container_ref magnitude carries; the marshaller
	// stringifies it. Pure; game-thread (reads the subsystem's entity binding).
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Effects",
		meta = (BlueprintInternalUseOnly = "true"))
	static FString GetContainerIdFor(UObject* Object);

	// Apply Effect to Target (the affected container), optionally caused by Source. Overrides replaces a
	// magnitude's authored default by its Name (value is a JSON-encoded literal: "5", "true", "\"text\"", a
	// container-id string). Level samples any curve-bound magnitude (default 1). SessionId is optional (empty =
	// app-global). Fire-and-forget: nothing here reports the outcome, and a coalescing effect may be merged with
	// other applies and sent later (see ApplyInternal).
	// BlueprintInternalUseOnly: the "Apply Crowdy Effect (Fire and Forget)" K2 node places this call itself and
	// exposes one typed pin per magnitude, so the plain function must not also appear in the palette under the
	// same label with a raw Overrides map pin.
	// DefaultToSelf on Target: applying an effect to the Blueprint that is running is the overwhelmingly common
	// case, so an unwired Target resolves to self rather than to null (which would silently do nothing).
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Effects",
		meta = (AutoCreateRefTerm = "Overrides", BlueprintInternalUseOnly = "true", DefaultToSelf = "Target"),
		DisplayName = "Apply Crowdy Effect (Fire and Forget)")
	static void Apply(UCrowdyEffect* Effect, UObject* Target, UObject* Source,
		const TMap<FName, FString>& Overrides, float Level = 1.0f, const FString& SessionId = TEXT(""));

	// Free/data container variant (no actor): apply Effect to a container addressed by id (inventories,
	// quests). WorldContext supplies the world; Source, when set, still injects source_id. Level samples any
	// curve-bound magnitude (default 1).
	// BlueprintInternalUseOnly: the "Apply Crowdy Effect to Model (by Id)" K2 node places this call itself and
	// exposes one typed pin per magnitude, so the plain function must not also appear in the palette under the
	// same label with a raw Overrides map pin.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Effects",
		meta = (WorldContext = "WorldContext", AutoCreateRefTerm = "Overrides", BlueprintInternalUseOnly = "true"),
		DisplayName = "Apply Crowdy Effect to Model (by Id)")
	static void ApplyToContainer(UObject* WorldContext, const FString& ContainerId, UCrowdyEffect* Effect,
		UObject* Source, const TMap<FName, FString>& Overrides, float Level = 1.0f, const FString& SessionId = TEXT(""));

	// The pure marshaller: build the invoke params object from Effect's magnitudes + the source_id rule. One key
	// per magnitude (key = Name, value = the Override for that Name, else a curve sampled at Level, else the authored
	// DefaultValueJson, which a Required magnitude does not have). An optional magnitude that resolves to nothing is
	// omitted, so the server applies the parameter's own default. Typing is value-type aware: a string/container_ref
	// magnitude is ALWAYS emitted as a JSON string (a bare word or a numeric-looking id is stringified, not
	// mis-typed), while an int/float/bool magnitude must be valid JSON of its own shape or it is rejected (a
	// non-JSON numeric is a typo, caught here). Adds source_id (a string) iff bHasSource. Returns null + OutError
	// when the effect requires a Source but bHasSource
	// is false, a Required magnitude with no curve has no override, a curve is bound to a non-numeric
	// magnitude, a numeric/bool value is not valid JSON, or bHasSource with an empty SourceContainerId. Public +
	// static so tests drive it with no world, no subsystem, no HTTP.
	static TSharedPtr<FJsonObject> BuildInvokeParams(const UCrowdyEffect* Effect, const TMap<FName, FString>& Overrides,
		float Level, bool bHasSource, const FString& SourceContainerId, FString& OutError);

	// Everything besides the target container, the function and the session that two applies of a coalescing effect
	// must agree on before their magnitudes may be summed into one server call, reduced to one 64-bit value. Built
	// from what DETERMINES the invoke parameters - the effect asset, the level any curve is sampled at, the Source's
	// container, and every override except the one being summed - rather than from the marshalled parameters, so it
	// never depends on how a JSON object enumerates its keys.
	//
	// It is a hash rather than a string because this runs on every apply of an autofire effect: the string form was a
	// several-hundred-byte concatenation, built and then hashed again by the map that used it. The overrides are
	// folded in order-independently (their names are unique, so no two entries can cancel), which removes the sort and
	// the per-override lowercased copy the string form needed to be stable.
	//
	// Level is included whether or not any magnitude actually reads a curve, because that is a property of the asset
	// which can change without this code seeing it. Including it can split a window that could have merged, costing
	// some of the invoke allowance; leaving it out could merge two applies whose curve-sampled magnitudes differ,
	// which would send a value neither caller asked for. Public + static so "these two must not merge" is testable
	// with no world and no server.
	static uint64 BuildMergeDiscriminator(const UCrowdyEffect* Effect, const TMap<FName, FString>& Overrides,
		float Level, const FString& SourceContainerId, const FString& AccumulateParam);

	// Shared resolve + marshal + dispatch, reused by the library entries (OnDone null) and the latent node (OnDone
	// broadcasts). Exactly one of Target (actor-bound) or ContainerId (free/data) is used; the other is null/empty.
	// Returns false + OutError on a SYNCHRONOUS failure (no world/subsystem, Target not a registered entity, a
	// marshal error) before any invoke is dispatched; true once the apply has been accepted, after which OnDone runs
	// exactly once, later, on the game thread.
	//
	// One call here is NOT necessarily one gameModelInvoke, and the invoke is not necessarily sent now. Which of the
	// two behaviours below applies is decided entirely by the effect asset, so a caller never has to choose.
	//
	// An effect with coalescing OFF (the default, and every effect authored before it existed) behaves exactly as it
	// always has: one gameModelInvoke, dispatched immediately, its raw result handed to OnDone.
	//
	// An effect with coalescing ON is offered to the subsystem's merge window instead. Repeated applies to the SAME
	// target that agree on the function, the session and every parameter except the accumulated one are summed and
	// sent as ONE invoke when the window closes, and every merged caller receives its own copy of that one outcome.
	// The apply is therefore delayed by up to the window, and a world torn down while a window is open fails every
	// caller waiting in it rather than sending anything.
	//
	// Either way, a refusal the server attributes to its own per-player invoke rate limit is retried, after a bounded
	// backoff, by the subsystem's governor. That is the ONLY failure ever repeated, because it is the only one the
	// server decides at its gate before running the function, so the refused call committed nothing and repeating it
	// cannot apply a write twice. No other failure is retried here, an unattributed transport failure least of all:
	// it may have committed before the failure was reported. A caller that wants retry-on-platform-fault still owns
	// that itself, and the exactly-once idiom documented on Append and Remove At still applies: reissuing an
	// already-committed invoke is only safe when the effect's own writes are idempotent.
	//
	// If you do own it, branch on Blame and not on FCrowdyInvokeResult::bRetryable alone. bRetryable is NOT limited to
	// Blame::Platform: the server defaults extensions.retryable to true, so an Author fault (which will fail
	// identically however often it is repeated) and a Budget refusal (already being retried here) both report it true.
	// Blame::Platform with bRetryable is the pair that means what a retry loop wants.
	static bool ApplyInternal(UObject* WorldContext, UObject* Target, const FString& ContainerId, UCrowdyEffect* Effect,
		UObject* Source, const TMap<FName, FString>& Overrides, float Level, const FString& SessionId,
		TFunction<void(FCrowdyInvokeResult)> OnDone, FString& OutError);
};
