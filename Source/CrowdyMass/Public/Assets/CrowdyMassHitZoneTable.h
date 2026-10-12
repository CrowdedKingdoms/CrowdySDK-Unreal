#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CrowdyMassHitZoneTable.generated.h"

/**
 * One authored bone-to-zone mapping. BoneName is matched exactly against the bone name a trace hit
 * carries; author one row per bone that should report something other than the table's default zone.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassHitZoneEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones")
	FName BoneName = NAME_None;

	/** Name reported in FCrowdyMassTraceHit::HitZoneName for a trace landing on BoneName, for example "Head" or "Torso". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones")
	FName ZoneName = NAME_None;

	/**
	 * Cosmetic multiplier for this zone. See FCrowdyMassTraceHit::DamageMultiplier: this is client-
	 * side prediction only, read by whichever client happened to run the trace, and must never be used
	 * to compute an actual damage amount. The server holds its own authoritative zone table.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones", meta=(ClampMin="0.0"))
	float DamageMultiplier = 1.0f;
};

/**
 * Maps a skeleton's bone names to named hit zones and a cosmetic damage multiplier. Assign one
 * through UCrowdyMassTraceBlueprintLibrary::SetHitZoneTable; every Crowdy Mass trace afterward
 * resolves its hit bone against it.
 *
 * A bone that matches no entry reports DefaultZoneName and DefaultDamageMultiplier instead of leaving
 * the hit fields blank, so an unauthored or renamed bone degrades to a sane zone rather than to
 * nothing at all.
 */
UCLASS(BlueprintType)
class CROWDYMASS_API UCrowdyMassHitZoneTable : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones")
	TArray<FCrowdyMassHitZoneEntry> Entries;

	/** Zone reported for a bone that matches no row in Entries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones")
	FName DefaultZoneName = FName(TEXT("Default"));

	/** Multiplier reported for a bone that matches no row in Entries. Cosmetic only, see FCrowdyMassHitZoneEntry::DamageMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Hit Zones", meta=(ClampMin="0.0"))
	float DefaultDamageMultiplier = 1.0f;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/**
 * One hit zone table already resolved into a flat bone-name lookup. Trace hits are frequent and this
 * is read on every one of them, so the table is walked into this shape once when it is assigned (see
 * UCrowdyMassTraceBlueprintLibrary::SetHitZoneTable), never per hit and never per bone per hit.
 *
 * A null source table (or one that has never been built) resolves every bone to the built-in default
 * zone, so callers never need a null check of their own before calling Resolve.
 *
 * Everything this produces is COSMETIC CLIENT-SIDE PREDICTION. The multiplier is whatever the local
 * machine happens to have authored; the server holds its own authoritative zone table and never reads
 * this one, so no damage amount may be computed from what Resolve reports.
 */
struct CROWDYMASS_API FCrowdyMassResolvedHitZoneTable
{
	// The zone reported when nothing has been built and when a table leaves its own default unnamed.
	// Named here so the string exists in one place rather than in every site that has to fall back.
	static FName BuiltInDefaultZoneName() { return FName(TEXT("Default")); }

	/**
	 * Rebuilds the flat lookup from Table. Safe to call with null, which clears the lookup back to
	 * reporting the built-in default zone for every bone.
	 *
	 * Values are clamped here rather than only in the asset's editor-time validation: a cooked build and
	 * a table built in code both reach this with whatever they were given, and neither runs IsDataValid.
	 */
	void Build(const UCrowdyMassHitZoneTable* Table);

	// Zone name and cosmetic multiplier for BoneName, falling back to the table's default (or the
	// built-in default if no table was ever built) for a bone the table does not name.
	void Resolve(FName BoneName, FName& OutZoneName, float& OutDamageMultiplier) const;

private:
	// Only what a hit reports. The bone name is already the key, so keeping a second copy of it inside
	// the value would widen every row of a table that is read on the trace path for nothing.
	struct FResolvedZone
	{
		FName ZoneName;
		float DamageMultiplier = 1.0f;
	};

	TMap<FName, FResolvedZone> BoneToZone;
	FName DefaultZoneName = BuiltInDefaultZoneName();
	float DefaultDamageMultiplier = 1.0f;
};
