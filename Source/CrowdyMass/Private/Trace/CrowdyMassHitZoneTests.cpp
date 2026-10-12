#include "Assets/CrowdyMassHitZoneTable.h"
#include "Trace/CrowdyMassTraceBlueprintLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fragments/CrowdyMassFragments.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	constexpr EAutomationTestFlags CrowdyMassHitZoneTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyMassHitZoneEntry MakeEntry(FName BoneName, FName ZoneName, float DamageMultiplier)
	{
		FCrowdyMassHitZoneEntry Entry;
		Entry.BoneName = BoneName;
		Entry.ZoneName = ZoneName;
		Entry.DamageMultiplier = DamageMultiplier;
		return Entry;
	}
}

// A missing trace collision fragment must fail CLOSED: an entity nobody ever gave a trace collision
// fragment to must never block a trace and must never pass a trace filter, the same as an entity that
// explicitly has bEnableTraceCollision = false. The old behaviour treated a missing fragment as
// "blocks everything", which silently made every non-participating archetype a wall the moment
// anything traced through it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTraceCollisionMissingFragmentFailsClosedTest,
	"CrowdySDK.Mass.TraceCollisionMissingFragmentFailsClosed", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassTraceCollisionMissingFragmentFailsClosedTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a null fragment pointer (entity has no fragment at all) does not allow participation"),
		CrowdyMassTrace::DoesTraceCollisionFragmentAllowParticipation(nullptr));

	FCrowdyMassTraceCollisionFragment EnabledFragment;
	EnabledFragment.bEnableTraceCollision = true;
	TestTrue(TEXT("a present, enabled fragment allows participation"),
		CrowdyMassTrace::DoesTraceCollisionFragmentAllowParticipation(&EnabledFragment));

	FCrowdyMassTraceCollisionFragment DisabledFragment;
	DisabledFragment.bEnableTraceCollision = false;
	TestFalse(TEXT("a present, explicitly disabled fragment does not allow participation, matching the missing case"),
		CrowdyMassTrace::DoesTraceCollisionFragmentAllowParticipation(&DisabledFragment));

	return true;
}

// FCrowdyMassResolvedHitZoneTable::Build is the only place allowed to search the authored Entries
// array; Resolve itself must be a single hash lookup so it stays cheap enough to run on every trace
// hit. This exercises a matched bone, an unmatched bone falling back to the table's own default, and
// a null table falling back to the built-in default, without touching the per-hit trace path at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassHitZoneResolutionTest,
	"CrowdySDK.Mass.HitZoneResolution", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassHitZoneResolutionTest::RunTest(const FString& Parameters)
{
	UCrowdyMassHitZoneTable* Table = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
	Table->Entries.Add(MakeEntry(TEXT("head"), TEXT("Head"), 2.5f));
	Table->Entries.Add(MakeEntry(TEXT("spine_01"), TEXT("Torso"), 1.0f));
	Table->DefaultZoneName = TEXT("Limb");
	Table->DefaultDamageMultiplier = 0.75f;

	FCrowdyMassResolvedHitZoneTable Resolved;
	Resolved.Build(Table);

	FName ZoneName;
	float DamageMultiplier = 0.0f;

	Resolved.Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("matched bone reports its authored zone"), ZoneName, FName(TEXT("Head")));
	TestEqual(TEXT("matched bone reports its authored multiplier"), DamageMultiplier, 2.5f);

	Resolved.Resolve(TEXT("thigh_r"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("unmatched bone falls back to the table's default zone"), ZoneName, FName(TEXT("Limb")));
	TestEqual(TEXT("unmatched bone falls back to the table's default multiplier"), DamageMultiplier, 0.75f);

	FCrowdyMassResolvedHitZoneTable NullResolved;
	NullResolved.Build(nullptr);
	NullResolved.Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("a null table falls back to the built-in default zone"),
		ZoneName, FCrowdyMassResolvedHitZoneTable::BuiltInDefaultZoneName());
	TestEqual(TEXT("a null table falls back to the built-in default multiplier"), DamageMultiplier, 1.0f);

	return true;
}

// The first authored row for a bone is the one that takes effect, which is the row an author is told
// about when validation reports a duplicate. Build walks the rows back to front so a plain Add leaves the
// earliest one standing, at one hash per row; mutating that walk to run forwards leaves the LAST row
// standing and turns this red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassHitZoneFirstDuplicateRowWinsTest,
	"CrowdySDK.Mass.HitZoneFirstDuplicateRowWins", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassHitZoneFirstDuplicateRowWinsTest::RunTest(const FString& Parameters)
{
	UCrowdyMassHitZoneTable* Table = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
	Table->Entries.Add(MakeEntry(TEXT("head"), TEXT("HeadFirst"), 2.0f));
	Table->Entries.Add(MakeEntry(TEXT("head"), TEXT("HeadSecond"), 3.0f));

	FCrowdyMassResolvedHitZoneTable Resolved;
	Resolved.Build(Table);

	FName ZoneName;
	float DamageMultiplier = 0.0f;
	Resolved.Resolve(TEXT("head"), ZoneName, DamageMultiplier);

	TestEqual(TEXT("the first authored row for a bone is the one that resolves"), ZoneName, FName(TEXT("HeadFirst")));
	TestEqual(TEXT("and its multiplier with it"), DamageMultiplier, 2.0f);

	return true;
}

// Build is the only clamp a cooked build ever runs: IsDataValid is editor-only, and a table assembled in
// code never sees it at all. An unnamed default zone would report NAME_None for every unmatched bone,
// which reads as "nothing resolved" rather than as the fallback, and a negative multiplier would flip the
// sign of whatever it scales. Mutating either clamp out of Build turns this red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassHitZoneBuildClampsTest,
	"CrowdySDK.Mass.HitZoneBuildClamps", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassHitZoneBuildClampsTest::RunTest(const FString& Parameters)
{
	UCrowdyMassHitZoneTable* Table = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
	Table->Entries.Add(MakeEntry(TEXT("head"), NAME_None, -3.0f));
	Table->DefaultZoneName = NAME_None;
	Table->DefaultDamageMultiplier = -2.0f;

	FCrowdyMassResolvedHitZoneTable Resolved;
	Resolved.Build(Table);

	FName ZoneName;
	float DamageMultiplier = 0.0f;

	Resolved.Resolve(TEXT("thigh_r"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("an unnamed default zone falls back to the built-in one rather than reporting NAME_None"),
		ZoneName, FCrowdyMassResolvedHitZoneTable::BuiltInDefaultZoneName());
	TestEqual(TEXT("a negative default multiplier is clamped to zero"), DamageMultiplier, 0.0f);

	Resolved.Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("an unnamed row zone falls back to the table's default rather than reporting NAME_None"),
		ZoneName, FCrowdyMassResolvedHitZoneTable::BuiltInDefaultZoneName());
	TestEqual(TEXT("a negative row multiplier is clamped to zero"), DamageMultiplier, 0.0f);

	return true;
}

/**
 * The hit zone table belongs to one world.
 *
 * It used to be a function-local static inside the trace library, which is process-wide: two PIE clients
 * shared one table, and a table set in one PIE session was still installed when the next one started,
 * which is a state no cold start can reproduce. Mutating the storage back to a static shared between
 * subsystems turns this red on the second world, which never had a table assigned.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassHitZoneTableIsWorldScopedTest,
	"CrowdySDK.Mass.HitZoneTableIsWorldScoped", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassHitZoneTableIsWorldScopedTest::RunTest(const FString& Parameters)
{
	UCrowdyMassEntitySubsystem* FirstWorld = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
	UCrowdyMassEntitySubsystem* SecondWorld = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());

	UCrowdyMassHitZoneTable* Table = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
	Table->Entries.Add(MakeEntry(TEXT("head"), TEXT("Head"), 2.5f));
	Table->DefaultZoneName = TEXT("Limb");
	Table->DefaultDamageMultiplier = 0.75f;

	FirstWorld->SetHitZoneTable(Table);

	FName ZoneName;
	float DamageMultiplier = 0.0f;

	FirstWorld->GetResolvedHitZoneTable().Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("the world the table was assigned to resolves it"), ZoneName, FName(TEXT("Head")));
	TestEqual(TEXT("with its multiplier"), DamageMultiplier, 2.5f);

	SecondWorld->GetResolvedHitZoneTable().Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("a second world that was never given a table still reports the built-in default zone"),
		ZoneName, FCrowdyMassResolvedHitZoneTable::BuiltInDefaultZoneName());
	TestEqual(TEXT("and the built-in default multiplier"), DamageMultiplier, 1.0f);

	// Clearing one world's table leaves the other's alone too, which is the same claim from the other side.
	SecondWorld->SetHitZoneTable(nullptr);
	FirstWorld->GetResolvedHitZoneTable().Resolve(TEXT("head"), ZoneName, DamageMultiplier);
	TestEqual(TEXT("clearing the second world's table does not clear the first world's"),
		ZoneName, FName(TEXT("Head")));

	return true;
}

#if WITH_EDITOR
// IsDataValid is the only line of defence an author has: nothing at runtime ever reports a table that
// silently degrades to the default zone for a bone that was supposed to be authored, or a duplicate
// row where only the first ever takes effect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassHitZoneTableValidationTest,
	"CrowdySDK.Mass.HitZoneTableValidation", CrowdyMassHitZoneTestFlags)
bool FCrowdyMassHitZoneTableValidationTest::RunTest(const FString& Parameters)
{
	{
		UCrowdyMassHitZoneTable* Empty = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
		FDataValidationContext Context;
		TestTrue(TEXT("an empty table is invalid"), Empty->IsDataValid(Context) == EDataValidationResult::Invalid);
	}

	{
		UCrowdyMassHitZoneTable* DuplicateBone = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
		DuplicateBone->Entries.Add(MakeEntry(TEXT("head"), TEXT("Head"), 2.0f));
		DuplicateBone->Entries.Add(MakeEntry(TEXT("head"), TEXT("HeadAgain"), 3.0f));
		FDataValidationContext Context;
		TestTrue(TEXT("a duplicate bone row is invalid"), DuplicateBone->IsDataValid(Context) == EDataValidationResult::Invalid);
	}

	{
		UCrowdyMassHitZoneTable* NegativeMultiplier = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
		NegativeMultiplier->Entries.Add(MakeEntry(TEXT("head"), TEXT("Head"), -1.0f));
		FDataValidationContext Context;
		TestTrue(TEXT("a negative multiplier is invalid"), NegativeMultiplier->IsDataValid(Context) == EDataValidationResult::Invalid);
	}

	{
		UCrowdyMassHitZoneTable* Unnamed = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
		Unnamed->Entries.Add(MakeEntry(NAME_None, TEXT("Head"), 1.0f));
		FDataValidationContext Context;
		TestTrue(TEXT("a row with no BoneName is invalid"), Unnamed->IsDataValid(Context) == EDataValidationResult::Invalid);
	}

	{
		UCrowdyMassHitZoneTable* Valid = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
		Valid->Entries.Add(MakeEntry(TEXT("head"), TEXT("Head"), 2.0f));
		Valid->Entries.Add(MakeEntry(TEXT("spine_01"), TEXT("Torso"), 1.0f));
		FDataValidationContext Context;
		TestTrue(TEXT("a well-formed table is valid"), Valid->IsDataValid(Context) == EDataValidationResult::Valid);
	}

	return true;
}
#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
