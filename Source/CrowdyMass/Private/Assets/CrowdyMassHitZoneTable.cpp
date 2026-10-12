#include "Assets/CrowdyMassHitZoneTable.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

void FCrowdyMassResolvedHitZoneTable::Build(const UCrowdyMassHitZoneTable* Table)
{
	BoneToZone.Reset();
	DefaultZoneName = BuiltInDefaultZoneName();
	DefaultDamageMultiplier = 1.0f;

	if (!Table)
	{
		return;
	}

	// Clamped rather than copied. The asset's validation is editor-only, so a cooked build and a table
	// assembled in code both arrive here unchecked. An unnamed default zone would report NAME_None for
	// every unmatched bone, which reads as "nothing resolved" rather than as the fallback it is, and a
	// negative multiplier would flip the sign of whatever cosmetic preview is scaled by it.
	DefaultZoneName = Table->DefaultZoneName.IsNone() ? BuiltInDefaultZoneName() : Table->DefaultZoneName;
	DefaultDamageMultiplier = FMath::Max(Table->DefaultDamageMultiplier, 0.0f);

	BoneToZone.Reserve(Table->Entries.Num());

	// Walked back to front because Add overwrites: writing the earliest row last leaves the FIRST
	// authored row for a bone standing, which is the row IsDataValid reports a duplicate against, and it
	// costs one hash per row instead of a Contains probe followed by an Add.
	for (int32 EntryIndex = Table->Entries.Num() - 1; EntryIndex >= 0; --EntryIndex)
	{
		const FCrowdyMassHitZoneEntry& Entry = Table->Entries[EntryIndex];
		if (Entry.BoneName.IsNone())
		{
			continue;
		}

		FResolvedZone Zone;
		Zone.ZoneName = Entry.ZoneName.IsNone() ? DefaultZoneName : Entry.ZoneName;
		Zone.DamageMultiplier = FMath::Max(Entry.DamageMultiplier, 0.0f);
		BoneToZone.Add(Entry.BoneName, Zone);
	}
}

void FCrowdyMassResolvedHitZoneTable::Resolve(const FName BoneName, FName& OutZoneName, float& OutDamageMultiplier) const
{
	if (const FResolvedZone* Zone = BoneToZone.Find(BoneName))
	{
		OutZoneName = Zone->ZoneName;
		OutDamageMultiplier = Zone->DamageMultiplier;
		return;
	}

	OutZoneName = DefaultZoneName;
	OutDamageMultiplier = DefaultDamageMultiplier;
}

#if WITH_EDITOR
EDataValidationResult UCrowdyMassHitZoneTable::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (Entries.IsEmpty())
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(TEXT("Hit zone table has no entries; every bone will report the default zone. Author at least one entry or remove this table.")));
	}

	TSet<FName> SeenBones;
	for (const FCrowdyMassHitZoneEntry& Entry : Entries)
	{
		if (Entry.BoneName.IsNone())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(TEXT("Entries has a row with no BoneName; it can never match a hit and is ignored.")));
			continue;
		}

		if (SeenBones.Contains(Entry.BoneName))
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entries has more than one row for bone '%s'; only the first is ever used."),
				*Entry.BoneName.ToString())));
		}
		SeenBones.Add(Entry.BoneName);

		if (Entry.DamageMultiplier < 0.0f)
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entries row for bone '%s' has a negative DamageMultiplier (%f)."),
				*Entry.BoneName.ToString(), Entry.DamageMultiplier)));
		}
	}

	if (DefaultDamageMultiplier < 0.0f)
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("DefaultDamageMultiplier is negative (%f)."), DefaultDamageMultiplier)));
	}

	// UObject's own IsDataValid answers NotValidated, so a table that passed every check above would otherwise
	// report the same result as one nothing looked at. Promote it, and never promote away an Invalid.
	if (Result != EDataValidationResult::Invalid)
	{
		Result = EDataValidationResult::Valid;
	}

	return Result;
}
#endif
