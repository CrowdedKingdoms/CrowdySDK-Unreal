#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyKitLayerPreset.generated.h"

/**
 * The authoring surface for a CrowdyCPP GameKit deploy. Each preset is one genre
 * layer with typed pins; a UCrowdyGameKitConfig holds a list of them. Converting a
 * preset to the deployable bundle happens in the bridge (CrowdyGameKitEmit), which
 * translates these plain-typed fields into the vendored kit builders. These headers
 * stay free of any CrowdyCPP type so the editor and Studio can consume them without
 * the bridge dependency.
 *
 * The enums mirror the bridge's plain enums (ECrowdyKit*Kind in CrowdyKitBridge.h);
 * the emit step maps between them. They are deliberately named distinctly so both
 * can coexist in the one translation unit that does the conversion.
 */

/** How the owner id is stored on kit containers (expressions cannot read container ownership, so it is mirrored to a property). */
UENUM(BlueprintType)
enum class ECrowdyKitOwnerId : uint8
{
	Int,
	String
};

/** Who may instantiate a kit container type. Mirrors the bridge's ECrowdyKitInstantiableBy under a distinct name. */
UENUM(BlueprintType)
enum class ECrowdyKitCreator : uint8
{
	Member,
	Admin
};

/**
 * Who may call a trusted reward-granting mutation (anti-cheat: never a plain player). Mirrors the bridge's
 * ECrowdyKitSubmitAuthority under a distinct name so both can coexist in the conversion translation unit.
 */
UENUM(BlueprintType)
enum class ECrowdyKitAuthority : uint8
{
	Server,
	Host,
	Automation,
	Owner
};

/**
 * Base class for one kit layer. Abstract and instanced so a config's layer array
 * offers the concrete genre presets in its add dropdown. Subclasses carry the
 * genre's typed options; the emit step reads them by concrete type.
 */
UCLASS(Abstract, EditInlineNew, DefaultToInstanced, CollapseCategories)
class CROWDYREPLICATION_API UCrowdyKitLayerPreset : public UObject
{
	GENERATED_BODY()

public:
	/** A short label for this layer, shown in the config's deploy preview. */
	virtual FText GetLayerDisplayName() const;
};

/**
 * Server-authoritative combat: Combatant/StatusEffect containers, an attack damage
 * formula, a damage-over-time tick automation, respawn, and optional revive/host-sync.
 */
UCLASS(EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Combat"))
class CROWDYREPLICATION_API UCrowdyCombatPreset : public UCrowdyKitLayerPreset
{
	GENERATED_BODY()

public:
	/** Prefixes every type and function name (e.g. "Goblin" -> GoblinCombatant). Empty keeps the bare kit names. */
	UPROPERTY(EditAnywhere, Category = "Combat")
	FString TypePrefix;

	/** Gates the attack on the session turn (turn-based play). */
	UPROPERTY(EditAnywhere, Category = "Combat")
	bool bTurnBased = false;

	/** Adds an is_host-gated sync function so the elected host can run fast combat and sync durable hp. */
	UPROPERTY(EditAnywhere, Category = "Combat")
	bool bHostSynced = false;

	/** How often the damage-over-time automation ticks status effects, in milliseconds. */
	UPROPERTY(EditAnywhere, Category = "Combat", meta = (ClampMin = "100", UIMin = "100"))
	int32 EffectTickIntervalMs = 5000;

	/** Who may create a Combatant container. */
	UPROPERTY(EditAnywhere, Category = "Combat")
	ECrowdyKitCreator CombatantInstantiableBy = ECrowdyKitCreator::Member;

	/** Adds a group-permission-gated revive usable on any downed combatant. */
	UPROPERTY(EditAnywhere, Category = "Combat|Revive")
	bool bEnableRevive = false;

	/** The group whose members may revive. Required when revive is enabled: an empty id emits a degenerate policy. */
	UPROPERTY(EditAnywhere, Category = "Combat|Revive",
		meta = (EditCondition = "bEnableRevive", EditConditionHides))
	FString ReviveGroupId;

	/** An optional group-permission key the revive requires; empty admits any member of the revive group. */
	UPROPERTY(EditAnywhere, Category = "Combat|Revive",
		meta = (EditCondition = "bEnableRevive", EditConditionHides))
	FString RevivePermission;

	UPROPERTY(EditAnywhere, Category = "Combat")
	ECrowdyKitOwnerId OwnerIdKind = ECrowdyKitOwnerId::Int;

	virtual FText GetLayerDisplayName() const override;
};

/**
 * Per-player leaderboard entries written only through a trusted submit, with an
 * optional cron season roll.
 */
UCLASS(EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Leaderboards"))
class CROWDYREPLICATION_API UCrowdyLeaderboardsPreset : public UCrowdyKitLayerPreset
{
	GENERATED_BODY()

public:
	/** Prefixes the entry type and functions (e.g. "Weekly" -> WeeklyLeaderboardEntry). */
	UPROPERTY(EditAnywhere, Category = "Leaderboards")
	FString TypePrefix;

	/** Who may submit a score. Never Owner for a competitive board unless scores are non-sensitive. */
	UPROPERTY(EditAnywhere, Category = "Leaderboards")
	ECrowdyKitAuthority SubmitAuthority = ECrowdyKitAuthority::Host;

	/** True keeps the best score; false overwrites with the latest. */
	UPROPERTY(EditAnywhere, Category = "Leaderboards")
	bool bKeepBest = true;

	/** A cron expression (e.g. "0 0 1 * *") that rolls the season; empty means no season roll. */
	UPROPERTY(EditAnywhere, Category = "Leaderboards")
	FString SeasonCron;

	UPROPERTY(EditAnywhere, Category = "Leaderboards")
	ECrowdyKitOwnerId OwnerIdKind = ECrowdyKitOwnerId::Int;

	virtual FText GetLayerDisplayName() const override;
};

/**
 * A guild's shared assets: a group-gated hall lockable plus an optional guild-bank
 * inventory. Requires a pre-existing guild team (its group id).
 */
UCLASS(EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Guild"))
class CROWDYREPLICATION_API UCrowdyGuildPreset : public UCrowdyKitLayerPreset
{
	GENERATED_BODY()

public:
	/** Prefixes the composed types (e.g. "Guild" -> GuildHall, GuildBank). */
	UPROPERTY(EditAnywhere, Category = "Guild")
	FString TypePrefix = TEXT("Guild");

	/** The guild team's group id. Required: the hall's policy checks membership of this group. Create the team first. */
	UPROPERTY(EditAnywhere, Category = "Guild")
	FString GuildGroupId;

	/** An optional group-permission key the hall requires; empty admits every guild member. */
	UPROPERTY(EditAnywhere, Category = "Guild")
	FString HallPermission;

	/** Also compose a <prefix>Bank inventory for shared storage. */
	UPROPERTY(EditAnywhere, Category = "Guild")
	bool bBank = true;

	virtual FText GetLayerDisplayName() const override;
};

/**
 * World simulation driven by interval automations: day/night and weather, resource-node
 * regeneration, crop growth, and optional wave spawner counters. At least one section
 * must be enabled.
 */
UCLASS(EditInlineNew, DefaultToInstanced, meta = (DisplayName = "Living World"))
class CROWDYREPLICATION_API UCrowdyLivingWorldPreset : public UCrowdyKitLayerPreset
{
	GENERATED_BODY()

public:
	/** Prefixes every world type and function. */
	UPROPERTY(EditAnywhere, Category = "Living World")
	FString TypePrefix;

	/** Day/night and weather singleton. */
	UPROPERTY(EditAnywhere, Category = "Living World|Time")
	bool bEnableTime = true;

	UPROPERTY(EditAnywhere, Category = "Living World|Time",
		meta = (EditCondition = "bEnableTime", EditConditionHides, ClampMin = "100", UIMin = "100"))
	int32 TimeIntervalMs = 60000;

	UPROPERTY(EditAnywhere, Category = "Living World|Time",
		meta = (EditCondition = "bEnableTime", EditConditionHides, ClampMin = "1", UIMin = "1"))
	int32 HoursPerDay = 24;

	UPROPERTY(EditAnywhere, Category = "Living World|Time",
		meta = (EditCondition = "bEnableTime", EditConditionHides))
	bool bWeather = true;

	/** Replication radius of the time-changed spatial ping, in chunks (0-8). */
	UPROPERTY(EditAnywhere, Category = "Living World|Time",
		meta = (EditCondition = "bEnableTime", EditConditionHides, ClampMin = "0", ClampMax = "8", UIMin = "0", UIMax = "8"))
	int32 NotifyDistance = 8;

	/** Resource-node regeneration. */
	UPROPERTY(EditAnywhere, Category = "Living World|Nodes")
	bool bEnableNodes = true;

	UPROPERTY(EditAnywhere, Category = "Living World|Nodes",
		meta = (EditCondition = "bEnableNodes", EditConditionHides, ClampMin = "100", UIMin = "100"))
	int32 NodesIntervalMs = 60000;

	/** Crop growth. */
	UPROPERTY(EditAnywhere, Category = "Living World|Crops")
	bool bEnableCrops = true;

	UPROPERTY(EditAnywhere, Category = "Living World|Crops",
		meta = (EditCondition = "bEnableCrops", EditConditionHides, ClampMin = "100", UIMin = "100"))
	int32 CropsIntervalMs = 60000;

	/** Wave-spawner counters (entity spawning stays host-side). Off by default. */
	UPROPERTY(EditAnywhere, Category = "Living World|Waves")
	bool bEnableWaves = false;

	UPROPERTY(EditAnywhere, Category = "Living World|Waves",
		meta = (EditCondition = "bEnableWaves", EditConditionHides, ClampMin = "100", UIMin = "100"))
	int32 WavesIntervalMs = 60000;

	/** Added to next_wave_size each wave. */
	UPROPERTY(EditAnywhere, Category = "Living World|Waves",
		meta = (EditCondition = "bEnableWaves", EditConditionHides, ClampMin = "0", UIMin = "0"))
	int32 WaveGrowth = 1;

	UPROPERTY(EditAnywhere, Category = "Living World")
	ECrowdyKitOwnerId OwnerIdKind = ECrowdyKitOwnerId::Int;

	virtual FText GetLayerDisplayName() const override;
};
