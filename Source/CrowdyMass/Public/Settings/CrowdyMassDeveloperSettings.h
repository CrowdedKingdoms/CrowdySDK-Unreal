#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MassEntityConfigAsset.h"
#include "CrowdyMassDeveloperSettings.generated.h"

UENUM(BlueprintType)
enum class ECrowdyMassSpatialGridMode : uint8
{
	TwoDimensional UMETA(DisplayName="2D (X/Y)"),
	ThreeDimensional UMETA(DisplayName="3D (X/Y/Z)")
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Crowdy Mass"))
class CROWDYMASS_API UCrowdyMassDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return "Plugins"; }
	virtual FName GetSectionName() const override { return "Crowdy Mass"; }

	/** Mass Entity Config used when CrowdyMass spawns remote entities through the entity subsystem or rendering backend. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities",	meta=(DisplayName="Remote Entity Config", ToolTip="Mass Entity Config used when Crowdy spawns remote entities. Add the Crowdy Remote Entity trait, plus any project-specific traits needed for rendering or gameplay."))
	TSoftObjectPtr<UMassEntityConfigAsset> RemoteEntityConfig;

	/**
	 * Transform smoothing duration used before any arrival interval has been observed, which is every
	 * remote entity's first update, and for every update when Adaptive Smoothing is off. Steady-state
	 * motion is governed by the measured interval instead, not by this.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities",	meta=(DisplayName="Default Smoothing Duration", ClampMin="0.0", UIMin="0.0", Units="s", ToolTip="Smoothing duration used until an arrival interval has been observed, and for every update when Adaptive Smoothing is disabled. Steady-state motion is driven by the measured interval between arrivals instead."))
	float DefaultSmoothingDuration = 0.1f;

	/**
	 * Replays each segment over the interval actually observed between arrivals rather than over a
	 * fixed span. Disable to restore the fixed Default Smoothing Duration for every update, which is
	 * useful for comparing the two directly.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities", meta=(DisplayName="Adaptive Smoothing", ToolTip="Replays each segment over the observed interval between arrivals instead of a fixed duration. Disable to compare against the fixed-duration behaviour."))
	bool bAdaptiveSmoothing = true;

	/**
	 * How much longer than the observed interval each segment is replayed over.
	 *
	 * Above 1 the next update lands while the entity is still moving and simply retargets it, which is
	 * seamless; below 1 the entity arrives early and stands still until the next update. 1.0 is not
	 * the neutral setting it appears to be, since half of all intervals are longer than the typical
	 * one and those halves stall. Overshooting costs a little extra distance behind the sender.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities", meta=(DisplayName="Smoothing Interval Overshoot", ClampMin="0.5", UIMin="0.5", ClampMax="3.0", UIMax="3.0", EditCondition="bAdaptiveSmoothing", ToolTip="How much longer than the observed arrival interval each segment is replayed over. Above 1 removes the stall at the cost of sitting slightly further behind; below 1 reintroduces it."))
	float SmoothingIntervalOvershoot = 1.2f;

	/** Shortest span a measured segment may be replayed over, so a burst of arrivals cannot make one snap. Does not apply to a duration a sender asked for outright, since a sender may legitimately want a snap. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities", meta=(DisplayName="Min Smoothing Duration", ClampMin="0.0", UIMin="0.0", Units="s", EditCondition="bAdaptiveSmoothing", ToolTip="Shortest span a measured segment may be replayed over. Ignored unless Adaptive Smoothing is on."))
	float MinSmoothingDuration = 0.05f;

	/** Longest span any segment may be replayed over, so neither a stalled sender nor a forged duration can leave an entity gliding or frozen for seconds. Applies to sender-declared durations as well. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Remote Entities", meta=(DisplayName="Max Smoothing Duration", ClampMin="0.0", UIMin="0.0", Units="s", ToolTip="Longest span any segment may be replayed over, including a duration a sender declared."))
	float MaxSmoothingDuration = 0.5f;

	/**
	 * How close a remote entity has to be, while off screen or behind the camera, before it is promoted
	 * from one row of the crowd to a real actor.
	 *
	 * This and the two settings below decide the entire cost of promotion, so they are named here rather
	 * than inherited from whatever Mass happens to default to. They start at exactly those defaults, so a
	 * project that never opens this page behaves as it always did.
	 *
	 * Only entities of a class that declares a near tier shell are ever promoted, so raising this changes
	 * nothing for a project that declares none.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Near Tier", meta=(DisplayName="Near Tier Distance", ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="How close a remote entity must be before it is promoted from a crowd row to a real actor, measured while it is not on screen. Only classes that declare a near tier shell are ever promoted."))
	float NearTierDistance = 1000.0f;

	/**
	 * The same distance, for an entity the camera can actually see.
	 *
	 * Two numbers rather than one because an entity in front of the player earns a real actor from further
	 * away than one behind them does, and collapsing them would quietly halve the range at which a visible
	 * crowd gains actors.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Near Tier", meta=(DisplayName="Near Tier Visible Distance", ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="How close a remote entity the camera can see must be before it is promoted from a crowd row to a real actor. Normally larger than Near Tier Distance, since a visible crowd is worth more."))
	float NearTierVisibleDistance = 2000.0f;

	/**
	 * The hard ceiling on how many entities may hold a near tier actor at once.
	 *
	 * The nearest entities win it and the rest stay crowd rows however close they are, so this is what
	 * bounds the cost of a crowd walking into the camera all at once. It is a count of actors alive at the
	 * same time, not a rate: spreading spawns over more frames does not raise it.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Near Tier", meta=(DisplayName="Max Near Tier Actors", ClampMin="0", UIMin="0", ToolTip="Ceiling on how many remote entities may hold a near tier actor at the same time. The nearest win; the rest stay crowd rows however close they get."))
	int32 MaxNearTierActors = 50;

	/** Enables the Crowdy Mass trace broad phase. When disabled, traces scan all tracked remote Mass entities. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Enable Spatial Grid", ToolTip="Uses a spatial grid to narrow which remote entities a trace tests. When off, every trace scans all tracked remote entities and the grid settings below are unused."))
	bool bEnableSpatialGrid = true;

	/** Chooses whether the trace grid partitions X/Y only, or X/Y/Z. 2D is usually best for ground-based player crowds. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Mode", EditCondition="bEnableSpatialGrid", ToolTip="Whether the trace grid partitions X/Y only (2D) or X/Y/Z (3D). 2D is usually best for ground-based crowds."))
	ECrowdyMassSpatialGridMode SpatialGridMode = ECrowdyMassSpatialGridMode::TwoDimensional;

	/** Size of each spatial grid cell in centimeters. Smaller cells reduce candidates but make long traces visit more cells. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Cell Size", ClampMin="100.0", UIMin="100.0", Units="cm", EditCondition="bEnableSpatialGrid", ToolTip="Size of each trace grid cell. Smaller cells give fewer candidates but make long traces visit more cells."))
	float SpatialGridCellSize = 1600.0f;

	/** Conservative radius used to place each remote entity into grid cells; exact mesh bounds and physics asset tests still decide hits. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Entity Bounds Radius", ClampMin="0.0", UIMin="0.0", Units="cm", EditCondition="bEnableSpatialGrid", ToolTip="Broad-phase radius used to place remote Mass entities in the trace grid. Exact mesh bounds and physics asset tests still decide hits."))
	float SpatialGridEntityBoundsRadius = 250.0f;

	/** Extra radius around the line trace when collecting grid cells. Use this only as broad-phase padding. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Trace Query Radius", ClampMin="0.0", UIMin="0.0", Units="cm", EditCondition="bEnableSpatialGrid", ToolTip="Extra radius around the ray when collecting grid cells. This is broad-phase only and should be conservative."))
	float SpatialGridTraceQueryRadius = 0.0f;

	/** Safety limit for cells visited by one trace or overlap, or covered by one remote entity, which is then placed with the largest radius that fits. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Max Ray Cells", ClampMin="1", UIMin="1", EditCondition="bEnableSpatialGrid", ToolTip="Maximum grid cells one Crowdy Mass trace or overlap may visit (an overlap past it scans tracked entities instead), and the most one remote entity may cover; an entity that would cover more is placed with the largest radius that fits, so lowering this also shrinks how far a fast entity's placement reaches."))
	int32 SpatialGridMaxRayCells = 4096;

	/**
	 * Ceiling on the swept radius a single remote update may claim in the grid.
	 *
	 * The radius grows with how far an entity says it moved since its last update, and that distance comes off
	 * the wire, so without a ceiling one packet reporting a large jump makes the covered cell box grow with the
	 * square of the distance. 12800 is eight cells at the default cell size, which covers any believable
	 * movement in one update and bounds the box at a few hundred cells in 2D. Spatial Grid Max Ray Cells caps it in any mode.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace", meta=(DisplayName="Spatial Grid Max Entity Sweep Radius", ClampMin="0.0", UIMin="0.0", Units="cm", EditCondition="bEnableSpatialGrid", ToolTip="Ceiling on the swept radius one remote entity update may claim in the trace grid. The radius is derived from a wire-supplied position delta, so this is what stops a single forged jump enumerating an unbounded number of cells."))
	float SpatialGridMaxEntitySweepRadius = 12800.0f;

	/**
	 * Caches per-entity Mass visual instance lookup data used by Crowdy Mass traces.
	 * This reduces repeated representation, component, instance, and asset lookups during frequent
	 * traces, at the cost of a small amount of memory per tracked entity. Entries are validated and
	 * rebuilt when representation, LOD, component, instance id, or asset data changes.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Cache", meta=(DisplayName="Enable Trace Instance Cache", ToolTip="Caches per-entity Mass visual instance lookup data used by Crowdy Mass traces. This reduces repeated representation, component, instance, and asset lookups during frequent traces, at the cost of a small amount of memory per tracked entity."))
	bool bEnableTraceInstanceCache = false;

	/**
	 * Caches shared trace metadata for skinned asset + physics asset pairs.
	 * This reduces repeated physics asset body setup walking and bone/shape metadata resolution for
	 * crowds that share character assets. The cache uses memory per unique asset pair and may need to
	 * be disabled while debugging live physics asset authoring changes.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Cache", meta=(DisplayName="Enable Physics Asset Trace Metadata Cache", ToolTip="Caches shared trace metadata per skinned asset + physics asset pair. This reduces repeated physics asset body setup walking and bone/shape metadata resolution for crowds that share character assets. Uses memory per unique asset pair."))
	bool bEnablePhysicsAssetTraceMetadataCache = false;

	/** Color used when Crowdy Mass trace debug draws visited spatial grid cells. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Debug", meta=(DisplayName="Spatial Grid Debug Color", ToolTip="Color used for the wire boxes drawn by Crowdy Mass trace nodes when Draw Spatial Grid Cells is enabled."))
	FColor SpatialGridDebugColor = FColor::Red;

	/** Line thickness used when Crowdy Mass trace debug draws visited spatial grid cells. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Debug", meta=(DisplayName="Spatial Grid Debug Line Thickness", ClampMin="0.0", UIMin="0.0", ToolTip="Line thickness for the wire boxes drawn by Crowdy Mass trace nodes when Draw Spatial Grid Cells is enabled."))
	float SpatialGridDebugLineThickness = 1.0f;

	/** Extra height above and below the trace used when producing 2D spatial grid debug cell bounds. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Debug", meta=(DisplayName="Spatial Grid Debug 2D Height Padding", ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="Extra height above and below the trace used when producing 2D spatial grid debug cell bounds. This affects visuals only; it does not change trace candidates."))
	float SpatialGridDebugTwoDimensionalHeightPadding = 0.0f;

	/** Vertical guide length above and below the possessed pawn root plane for 2D spatial grid debug cell corners. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy Mass|Trace|Debug", meta=(DisplayName="Spatial Grid Debug 2D Corner Line Half Length", ClampMin="0.0", UIMin="0.0", Units="cm", ToolTip="Vertical guide length above and below the possessed pawn root plane for 2D spatial grid debug cell corners. This affects visuals only; it does not change trace candidates."))
	float SpatialGridDebugTwoDimensionalCornerLineHalfLength = 100000.0f;
};
