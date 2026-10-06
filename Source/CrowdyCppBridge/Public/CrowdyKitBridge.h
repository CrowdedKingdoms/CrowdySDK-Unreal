#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FCrowdyCppClient;

/**
 * Options for the Game Kit blueprints. Game Models are deprecated: EmitBundle and DeployBundle both fail at once with
 * CrowdyCppGameModelDeprecatedMessage.
 *
 * These are plain structs, not USTRUCTs. The editor preset layer mirrors these as USTRUCTs and converts into them.
 */

/** Owner-mirror property typing (kit convention: expressions cannot read container ownership). */
enum class ECrowdyKitOwnerIdKind : uint8
{
	Int,
	String
};

/** Who may instantiate a container type. */
enum class ECrowdyKitInstantiableBy : uint8
{
	Member,
	Admin
};

/** Who may call a trusted reward-granting mutation (anti-cheat: never a plain player). */
enum class ECrowdyKitSubmitAuthority : uint8
{
	Server,
	Host,
	Automation,
	Owner
};

/** Which genre builder a layer selects. */
enum class ECrowdyKitGenre : uint8
{
	Combat,
	Leaderboards,
	Guild,
	LivingWorld
};

/** Server-authoritative combat: Combatant/StatusEffect containers, an attack damage formula, DoT tick automation, respawn, optional revive/host-sync. */
struct FCrowdyKitCombatOptions
{
	FString TypePrefix;
	/** Adds is_current_turn to the actor policies (session-turn play). */
	bool bTurnBased = false;
	/** Adds an is_host-gated sync function so the elected host can run fast combat and sync durable hp. */
	bool bHostSynced = false;
	int32 EffectTickIntervalMs = 5000;
	ECrowdyKitInstantiableBy CombatantInstantiableBy = ECrowdyKitInstantiableBy::Member;
	/** Adds a group-permission-gated revive usable on any downed combatant. */
	bool bEnableRevive = false;
	FString ReviveGroupId;
	/** Empty admits any member of the revive group. */
	FString RevivePermission;
	ECrowdyKitOwnerIdKind OwnerIdKind = ECrowdyKitOwnerIdKind::Int;
};

/** Per-player leaderboard entries written only through a trusted submit, with an optional cron season roll. */
struct FCrowdyKitLeaderboardsOptions
{
	FString TypePrefix;
	ECrowdyKitSubmitAuthority SubmitAuthority = ECrowdyKitSubmitAuthority::Host;
	/** True keeps the best score; false overwrites. */
	bool bKeepBest = true;
	/** Empty disables the season roll; otherwise a cron expression (e.g. "0 0 1 * *"). */
	FString SeasonCron;
	ECrowdyKitOwnerIdKind OwnerIdKind = ECrowdyKitOwnerIdKind::Int;
};

/** A guild's shared assets: a group-gated GuildHall lockable plus an optional guild-bank inventory. Requires a pre-existing guild team. */
struct FCrowdyKitGuildOptions
{
	FString TypePrefix = TEXT("Guild");
	/** The guild team's group id. Required: the hall's policy checks membership of this group. */
	FString GuildGroupId;
	/** Optional group-permission key the hall requires; empty admits every guild member. */
	FString HallPermission;
	/** Also compose a <prefix>Bank inventory for shared storage. */
	bool bBank = true;
};

/** Day/night and weather singleton driven by an interval automation. */
struct FCrowdyKitWorldsimTime
{
	int32 IntervalMs = 60000;
	int32 HoursPerDay = 24;
	bool bWeather = true;
	/** Replication radius of the time-changed spatial ping (chunks, 0-8). */
	int32 NotifyDistance = 8;
};

/** Resource-node regeneration driven by an interval automation. */
struct FCrowdyKitWorldsimNodes
{
	int32 IntervalMs = 60000;
};

/** Crop growth driven by an interval automation. */
struct FCrowdyKitWorldsimCrops
{
	int32 IntervalMs = 60000;
};

/** Wave-spawner counters advanced by an interval automation (entity spawning stays host-side). */
struct FCrowdyKitWorldsimWaves
{
	int32 IntervalMs = 60000;
	/** Added to next_wave_size each wave. */
	int32 Growth = 1;
};

/** World simulation. At least one section must be enabled. */
struct FCrowdyKitLivingWorldOptions
{
	FString TypePrefix;
	bool bEnableTime = true;
	FCrowdyKitWorldsimTime Time;
	bool bEnableNodes = true;
	FCrowdyKitWorldsimNodes Nodes;
	bool bEnableCrops = true;
	FCrowdyKitWorldsimCrops Crops;
	bool bEnableWaves = false;
	FCrowdyKitWorldsimWaves Waves;
	ECrowdyKitOwnerIdKind OwnerIdKind = ECrowdyKitOwnerIdKind::Int;
};

/**
 * One kit layer in a deploy request. Genre selects which options member the
 * emit reads; the rest are ignored. All selected layers are merged in one pass
 * so cross-genre name collisions are rejected together.
 */
struct FCrowdyKitLayerSpec
{
	ECrowdyKitGenre Genre = ECrowdyKitGenre::Combat;
	FCrowdyKitCombatOptions Combat;
	FCrowdyKitLeaderboardsOptions Leaderboards;
	FCrowdyKitGuildOptions Guild;
	FCrowdyKitLivingWorldOptions LivingWorld;
};

/**
 * The emitted, deployable kit bundle. On success the JSON strings are ready for
 * the deploy step: SeedInputJson is the gameModelSeed variables object, and each
 * automation/trigger JSON is one upsert input. On failure bOk is false and
 * ErrorMessage explains why (a validation error, a cross-genre name collision, or
 * a contained builder exception). All strings are compact JSON.
 */
struct FCrowdyKitBundle
{
	bool bOk = false;
	FString ErrorMessage;
	FString SeedInputJson;
	TArray<FString> AutomationJsons;
	TArray<FString> TriggerJsons;
	int32 NumContainerTypes = 0;
	int32 NumPropertyDefs = 0;
	int32 NumFunctions = 0;
	int32 NumAutomations = 0;
};

/**
 * The aggregate outcome of deploying a kit bundle. bOk is true only when every step (the seed, then each
 * automation, then each trigger) succeeded. On the first failing step deploy stops, bOk is false, ErrorMessage
 * names the failing step and carries its server reason, and StepsCompleted is how many steps had already
 * succeeded. StepsTotal is 1 (the seed) plus the automation and trigger counts.
 */
struct FCrowdyKitDeployResult
{
	bool bOk = false;
	FString ErrorMessage;
	int32 StepsCompleted = 0;
	int32 StepsTotal = 0;
};

class CROWDYCPPBRIDGE_API FCrowdyKitBridge
{
public:
	// Returns a failed bundle (bOk false) carrying CrowdyCppGameModelDeprecatedMessage.
	static FCrowdyKitBundle EmitBundle(int64 AppId, const TArray<FCrowdyKitLayerSpec>& Layers,
		const FString& SessionId);

	// Runs OnDone inline, exactly once, with bOk false and CrowdyCppGameModelDeprecatedMessage. Sends nothing.
	static void DeployBundle(const TSharedRef<FCrowdyCppClient>& Client, const FCrowdyKitBundle& Bundle,
		TFunction<void(FCrowdyKitDeployResult)> OnDone);
};
