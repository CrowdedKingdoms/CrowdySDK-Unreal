// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyCombatKitActions.generated.h"

class AActor;
class FJsonObject;
class UCrowdyGameModelSubsystem;

// A parsed, read-only view of a combatant's server-authoritative stats, returned by Get Combatant State.
USTRUCT(BlueprintType)
struct FCrowdyCombatantState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	int32 Hp = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	int32 MaxHp = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	int32 Attack = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	int32 Defense = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	bool bAlive = false;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FString ContainerId;
};

// On success ContainerId names the new combatant and ErrorMessage is empty; on failure ContainerId is empty and
// ErrorMessage explains why (not-registered, transport/server refusal, a rejected stat write).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdySpawnCombatantOutcome, const FString&, ContainerId, const FString&, ErrorMessage);

// On success ReturnValueJson carries the attack function's server return (the target's remaining hp) and
// ErrorMessage is empty; on failure ErrorMessage carries the server envelope's reason (a policy denial reads back
// verbatim).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyCombatAttackOutcome, const FString&, ReturnValueJson, const FString&, ErrorMessage);

// On success State holds the read stats and ErrorMessage is empty; on failure State is defaulted and ErrorMessage
// explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyGetCombatantStateOutcome, const FCrowdyCombatantState&, State, const FString&, ErrorMessage);

// Shared outcome for the self-mutating combatant functions (respawn / revive / sync): on success ReturnValueJson
// carries the function's server return and ErrorMessage is empty; on failure ErrorMessage carries the server
// envelope's reason (a policy denial, or "no such function" when the kit was not deployed with that option).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyCombatantMutationOutcome, const FString&, ReturnValueJson, const FString&, ErrorMessage);

// On success EffectContainerId names the armed effect container, ReturnValueJson carries the apply function's
// return (the effect's ticks_left), and ErrorMessage is empty; on failure EffectContainerId is empty (or the
// created container id when the arm itself was refused) and ErrorMessage explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyApplyStatusEffectOutcome, const FString&, EffectContainerId, const FString&, ReturnValueJson, const FString&, ErrorMessage);

/**
 * Spawn a Combatant for an Actor: create the Combatant container on the server, bind it to the Actor's Game Model
 * entity, and seed its stats. This closes the "spawned standalone with nobody bound" gap by calling the bind once,
 * so a later Combat Attack / Get Combatant State on the same Actor resolves to this container.
 *
 * TypePrefix selects the deployed Combat kit's type name (<Prefix>Combatant); leave it empty for a kit deployed
 * with no prefix. The stat inputs default to the kit's own defaults, so leaving them alone matches a plain spawn.
 * owner_user_id mirrors the local Game Model user id when signed in (best-effort; the server already pins the
 * record owner on create). The Actor must already be a registered Game Model entity.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySpawnCombatantAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdySpawnCombatantOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdySpawnCombatantOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Spawn Combatant")
	static UCrowdySpawnCombatantAction* SpawnCombatant(UObject* WorldContext, AActor* Actor,
		const FString& TypePrefix, int32 Hp = 100, int32 MaxHp = 100, int32 Attack = 10, int32 Defense = 0,
		const FString& SessionId = FString());

	virtual void Activate() override;

private:
	// Queues the stat property writes applied after create + bind. bFatal writes fail the spawn on rejection; the
	// owner_user_id mirror is non-fatal (the record owner is already pinned server-side).
	void BuildPendingWrites();
	// Applies the next queued write, then broadcasts Succeeded once the queue drains.
	void ApplyNextProperty();

	struct FPendingWrite
	{
		FString Key;
		FString ValueType;
		FString ValueJson;
		bool bFatal = false;
	};

	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	int32 StartingHp = 100;
	int32 StartingMaxHp = 100;
	int32 StartingAttack = 10;
	int32 StartingDefense = 0;
	FString SessionId;

	TWeakObjectPtr<UCrowdyGameModelSubsystem> Model;
	FGuid BoundNetID;
	FString CreatedContainerId;
	TArray<FPendingWrite> PendingWrites;
	int32 WriteIndex = 0;
};

/**
 * Attack a target combatant with an attacker's combatant. Both the damage formula (attack vs defense) and the
 * death flip run server-side in one transaction; this node only names the two participants. The attacker and
 * target must both be registered Game Model entities and the target must have a bound combatant container (spawn
 * it first), otherwise Failed carries a clear reason. A policy denial (not your turn, already dead) reads back
 * verbatim from the server on the Failed pin.
 *
 * TypePrefix must match the prefix the target/attacker were spawned under, so the correct attack function is
 * called; leave it empty for a kit deployed with no prefix.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyCombatAttackAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatAttackOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatAttackOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Combat Attack")
	static UCrowdyCombatAttackAction* CombatAttack(UObject* WorldContext, AActor* Attacker, AActor* Target,
		const FString& TypePrefix, const FString& SessionId = FString());

	// The pure param marshaller: the attack function takes target_id as a container_ref, which on the wire is the
	// bare target container-id STRING. Public + static so it is headless-testable with no world/subsystem/HTTP.
	static TSharedPtr<FJsonObject> BuildAttackParams(const FString& TargetContainerId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Attacker;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	FString SessionId;
};

/**
 * Read a combatant's server-authoritative state (hp / max_hp / attack / defense / alive) for an Actor by pulling
 * its bound container. The Actor must be a registered Game Model entity with a bound combatant container; on
 * failure Failed carries a clear reason and a defaulted state.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetCombatantStateAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyGetCombatantStateOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyGetCombatantStateOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Get Combatant State")
	static UCrowdyGetCombatantStateAction* GetCombatantState(UObject* WorldContext, AActor* Actor,
		const FString& SessionId = FString());

	// The pure parser: read the combatant stats out of a pulled container-state property map (key -> value).
	// ContainerId is threaded through onto the returned struct. Public + static so it is headless-testable.
	static FCrowdyCombatantState ParseCombatantState(const TSharedPtr<FJsonObject>& State, const FString& ContainerId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Target;
	FString SessionId;
};

/**
 * Respawn an Actor's downed combatant at full hp (hp -> max_hp, alive -> true), then re-pull its state so the
 * Actor's local combatant OnRep fires. The respawn function mutates the combatant itself and is owner-of-self
 * gated and only permitted while the combatant is down, so a respawn on a still-alive combatant reads back the
 * server's denial on Failed.
 *
 * TypePrefix must match the prefix the Actor's combatant was spawned under; leave it empty for a kit with no
 * prefix. The Actor must be a registered Game Model entity with a bound combatant container.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyRespawnCombatantAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Respawn Combatant")
	static UCrowdyRespawnCombatantAction* RespawnCombatant(UObject* WorldContext, AActor* Actor,
		const FString& TypePrefix, const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	FString SessionId;
};

/**
 * Revive another player's downed combatant at full hp. The revive function mutates the TARGET combatant but is
 * gated by a team/group permission held by the CALLER (a healer role), so the Target actor's bound container is
 * the invoke self and the local caller's permission authorizes it. On success the Target's state is re-pulled so
 * its OnRep fires.
 *
 * revive only exists when the kit was deployed with a revive group; a kit without one returns a server error on
 * Failed. TypePrefix must match the prefix the Target's combatant was spawned under. The Target must be a
 * registered Game Model entity with a bound combatant container.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyReviveCombatantAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Revive Combatant")
	static UCrowdyReviveCombatantAction* ReviveCombatant(UObject* WorldContext, AActor* Target,
		const FString& TypePrefix, const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	FString SessionId;
};

/**
 * Persist an Actor's host-simulated hp to its durable combatant (hp clamped to 0..max_hp, alive re-derived), then
 * re-pull so its OnRep fires. This is the low-frequency durable write-back for fast-twitch combat that runs on the
 * replication plane under host authority.
 *
 * sync_combatant only exists when the kit was deployed hostSynced and is enforced is_host server-side, so a
 * non-host caller reads back the server's denial on Failed. TypePrefix must match the prefix the Actor's combatant
 * was spawned under. The Actor must be a registered Game Model entity with a bound combatant container.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySyncCombatantAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyCombatantMutationOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Sync Combatant")
	static UCrowdySyncCombatantAction* SyncCombatant(UObject* WorldContext, AActor* Actor,
		const FString& TypePrefix, int32 Hp, const FString& SessionId = FString());

	// The pure param marshaller: sync_combatant takes hp as a JSON number. Public + static for headless testing.
	static TSharedPtr<FJsonObject> BuildSyncParams(int32 Hp);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	int32 SyncHp = 0;
	FString SessionId;
};

/**
 * Arm a status effect from a Caster against a Target combatant. Creates a caller-owned StatusEffect container and
 * invokes the apply function, which records the effect's target_key / effect_id / magnitude / ticks; the server's
 * interval automation then applies the damage-over-time tick by tick while the app has a player in it. The tick is
 * presence-gated, and the effect stores REMAINING TICKS rather than an expiry, so an effect armed before the last
 * player left resumes with its full remaining duration instead of having lapsed in the meantime.
 *
 * The Target's combat_key is derived from the Target actor's NetID string (the same value Spawn Combatant seeded),
 * so no extra server read is needed to join the effect to the target. Both Caster and Target must be registered
 * Game Model entities. The DoT plays out server-side; cross-client convergence happens on the next state pull. A
 * live notification carrier for effect application is a deferred follow-up (mirroring the Combat Attack
 * cross-client note).
 *
 * TypePrefix must match the prefix the Target's combatant was spawned under; leave it empty for a kit with no
 * prefix.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyApplyStatusEffectAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyApplyStatusEffectOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Combat")
	FCrowdyApplyStatusEffectOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Combat", DisplayName = "Apply Status Effect")
	static UCrowdyApplyStatusEffectAction* ApplyStatusEffect(UObject* WorldContext, AActor* Caster, AActor* Target,
		const FString& TypePrefix, const FString& EffectId, int32 Magnitude, int32 Ticks,
		const FString& SessionId = FString());

	// The pure param marshaller for apply_effect: target_key/effect_id are JSON strings, magnitude/ticks are JSON
	// numbers. Public + static so it is headless-testable with no world/subsystem/HTTP.
	static TSharedPtr<FJsonObject> BuildApplyEffectParams(const FString& TargetKey, const FString& EffectId,
		int32 Magnitude, int32 Ticks);

	virtual void Activate() override;

private:
	// Seeds owner_user_id on the new effect container (best-effort), then invokes apply_effect and broadcasts.
	void SeedOwnerThenApply();
	void InvokeApply();

	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor> Caster;
	TWeakObjectPtr<AActor> Target;
	FString TypePrefix;
	FString EffectId;
	int32 Magnitude = 0;
	int32 Ticks = 0;
	FString SessionId;

	TWeakObjectPtr<UCrowdyGameModelSubsystem> Model;
	FString TargetKey;
	FString EffectContainerId;
};
