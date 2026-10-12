#include "Trace/CrowdyMassTraceBlueprintLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Assets/CrowdyMassHitZoneTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Mass/EntityFragments.h"
#include "MassRepresentationFragments.h"
#include "Misc/AutomationTest.h"

// An entity close enough to be drawn as an actor of its own is hit through that actor by the world query,
// and as a row by the Mass query, and nothing downstream can merge two results into one player. These
// cases pin the functions that keep such an entity hittable exactly once and named from its own fragment,
// all of which are pure functions of the results and the fragments and need no world, no renderer and no
// spawned actor.
//
// The whole of the row decision is what the world query REPORTED, which is recorded from that query's own
// finished results, so the cases below drive the real recording and the real decision one after the other
// rather than asserting a flag they set themselves.
static constexpr EAutomationTestFlags CrowdyMassNearTierTraceTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassNearTierTraceTestFixture
{
	static FMassRepresentationFragment MakeRepresentation(const EMassRepresentationType Current,
		const EMassRepresentationType Prev, const bool bWaitingForVisualReadiness)
	{
		FMassRepresentationFragment Representation;
		Representation.CurrentRepresentation = Current;
		Representation.PrevRepresentation = Prev;
		Representation.SetIsWaitingForActorVisualReadiness(bWaitingForVisualReadiness);
		return Representation;
	}

	static FCrowdyRemoteIdentityFragment MakeIdentity()
	{
		FCrowdyRemoteIdentityFragment Identity;
		Identity.UUID = FGuid::NewGuid();
		return Identity;
	}

	/**
	 * A finished world trace result that landed on an entity's own actor and was named from that entity's
	 * fragment, built through the join the trace path itself uses so the shape cannot drift from it.
	 */
	static FCrowdyMassTraceHit MakeNamedWorldHit(const FMassEntityHandle Entity,
		const FCrowdyRemoteIdentityFragment& Identity)
	{
		FCrowdyMassTraceHit Hit;
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, nullptr, Hit);
		return Hit;
	}

	/**
	 * A world result carrying the handle of the entity whose actor it touched, without that entity having
	 * been named.
	 *
	 * Built by hand: today the join writes the handle and the naming together, so nothing produces this
	 * shape. It is here because the recording rule is what the handle is worth, not what it names, and
	 * anything that later fills the handle in from the actor-to-entity map without naming the entity would
	 * start suppressing rows for entities no caller was ever told about.
	 */
	static FCrowdyMassTraceHit MakeUnnamedWorldHitOnActorOf(const FMassEntityHandle Entity)
	{
		FCrowdyMassTraceHit Hit;
		Hit.Entity = Entity;
		Hit.bHitMassEntity = false;
		return Hit;
	}

	/**
	 * The multi-hit path's own prune, mirrored here so a case can show what it does and does not cover.
	 *
	 * It is a mirror rather than the function itself, which is file-local to the trace path; nothing below
	 * asserts this mirror is correct, it is only used to show that pruning cannot be what stops one entity
	 * being reported twice.
	 */
	static void PruneAfterFirstBlockingHitAsTheTraceDoes(TArray<FCrowdyMassTraceHit>& Hits)
	{
		for (int32 Index = 0; Index < Hits.Num(); ++Index)
		{
			if (Hits[Index].bBlockingHit)
			{
				Hits.SetNum(Index + 1);
				return;
			}
		}
	}

	static int32 CountHitsNaming(const TArray<FCrowdyMassTraceHit>& Hits, const FGuid& UUID)
	{
		int32 Count = 0;
		for (const FCrowdyMassTraceHit& Hit : Hits)
		{
			if (Hit.bHitMassEntity && Hit.UUID == UUID)
			{
				++Count;
			}
		}
		return Count;
	}
};

/**
 * Whether an entity is still tested as an instanced row, over the cross product of what it is being drawn
 * as and whether the world query reported that entity.
 *
 * The load-bearing cell is an entity being drawn by an actor that the world query did NOT report. That is
 * not an edge case: the actor is a pooled visual shell, so an object type it does not answer, a class
 * filter it does not satisfy, or the caller's ignore list all drop it, and an actor is only ever given to
 * the entities standing nearest the query. Answering from the representation alone, or from whether an
 * actor merely exists, makes exactly those entities the ones nothing can hit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierRowTracePredicateTest,
	"CrowdySDK.Mass.NearTierRowTracePredicate", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierRowTracePredicateTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierTraceTestFixture;

	auto Check = [this](const TCHAR* What, const EMassRepresentationType Current, const EMassRepresentationType Prev,
		const bool bReported, const bool bExpected)
	{
		const FMassRepresentationFragment Representation = FFixture::MakeRepresentation(Current, Prev, /*bWaiting=*/false);
		TestEqual(What, CrowdyMassTrace::ShouldTraceEntityAsRow(Representation, bReported), bExpected);
	};

	// Drawn as a row, which is the ordinary far crowd. Nothing reported it, because a row is not an actor
	// and the world query cannot see one at all.
	Check(TEXT("a row nothing reported traces as a row"),
		EMassRepresentationType::SkinnedMeshInstance, EMassRepresentationType::None, false, true);
	Check(TEXT("a row whose entity the world query already reported is left to that hit"),
		EMassRepresentationType::SkinnedMeshInstance, EMassRepresentationType::None, true, false);

	// Promoted, with the row it is replacing still the previous representation. This is the whole of an
	// asynchronous promotion and the frames after the actor arrives while it is held disabled.
	Check(TEXT("an entity being given an actor the world query did not report still traces as its row"),
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, false, true);
	Check(TEXT("and is left to that hit once the world query has reported it"),
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, true, false);

	// No row exists at all: neither representation is an instanced one, so there is nothing to test.
	Check(TEXT("an actor-drawn entity with no row behind it has nothing to trace as a row"),
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::HighResSpawnedActor, false, false);
	Check(TEXT("nor once it has been reported"),
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::HighResSpawnedActor, true, false);

	Check(TEXT("a low resolution actor with no row behind it is not a row either"),
		EMassRepresentationType::LowResSpawnedActor, EMassRepresentationType::None, false, false);
	Check(TEXT("a low resolution actor demoted from a row is still tested as that row"),
		EMassRepresentationType::LowResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, false, true);
	Check(TEXT("and is left to its actor's hit once the world query reported it"),
		EMassRepresentationType::LowResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, true, false);

	Check(TEXT("an entity being drawn as nothing traces as nothing"),
		EMassRepresentationType::None, EMassRepresentationType::None, false, false);
	Check(TEXT("an entity being drawn as nothing traces as nothing even unreported"),
		EMassRepresentationType::None, EMassRepresentationType::None, true, false);

	// A static mesh instance is a representation this plugin never selects, and it is not a Crowdy Mass row:
	// it must not be treated as one just for being an instanced representation.
	Check(TEXT("a static mesh instance is not a Crowdy Mass row"),
		EMassRepresentationType::StaticMeshInstance, EMassRepresentationType::None, false, false);

	// Being held for visual readiness is not part of the question. An actor waiting to be shown carries no
	// collision, so the world query cannot have reported it, and the unreported answer is what covers it;
	// reading the flag as well would be a second source for one decision.
	const FMassRepresentationFragment WaitingRepresentation = FFixture::MakeRepresentation(
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance,
		/*bWaitingForVisualReadiness=*/true);
	TestTrue(TEXT("an entity whose actor is held for visual readiness traces as the row still on screen"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(WaitingRepresentation, /*bReportedByWorldQuery=*/false));
	TestFalse(TEXT("and the flag changes nothing: what was reported is the whole of the answer"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(WaitingRepresentation, /*bReportedByWorldQuery=*/true));

	return true;
}

/**
 * A hit through an entity's own actor is the same hit as a hit on its row, for everything a caller routes
 * on: the same flags, the same id, the same handle, the same send target.
 *
 * The row side is authored here by hand rather than produced by the row fill, which is file-local to the
 * trace path. That makes it an independent statement of the shape rather than a comparison of one function
 * against itself, and any field the join stops writing turns this red.
 *
 * InstanceIndex is the one field that must NOT match: it is what says which row was hit, and a hit through
 * an actor has no row.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierPromotedHitIdentityShapeTest,
	"CrowdySDK.Mass.NearTierPromotedHitIdentityShape", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierPromotedHitIdentityShapeTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	const FMassEntityHandle Entity(12, 3);

	FCrowdyRemoteIdentityFragment Identity;
	Identity.UUID = FGuid::NewGuid();

	FCrowdyMassTraceHit PromotedHit;
	const bool bNamed = CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, World, PromotedHit);

	// What the row path writes for the same entity, stated independently.
	FCrowdyMassTraceHit RowHit;
	RowHit.bHitMassEntity = true;
	RowHit.bHitCrowdyEntity = true;
	RowHit.UUID = Identity.UUID;
	RowHit.Entity = Entity;
	RowHit.Target.Entity = Entity;
	RowHit.Target.World = World;
	RowHit.InstanceIndex = 7;

	TestTrue(TEXT("an entity carrying a valid id is named"), bNamed);
	TestEqual(TEXT("both report a Mass entity was hit"), PromotedHit.bHitMassEntity, RowHit.bHitMassEntity);
	TestEqual(TEXT("both report a Crowdy entity was hit"), PromotedHit.bHitCrowdyEntity, RowHit.bHitCrowdyEntity);
	TestEqual(TEXT("both carry the same id"), PromotedHit.UUID, RowHit.UUID);
	TestTrue(TEXT("both carry the same entity handle"), PromotedHit.Entity == RowHit.Entity);
	TestTrue(TEXT("both address the same entity as a send target"), PromotedHit.Target.Entity == RowHit.Target.Entity);
	TestSamePtr(TEXT("both carry the world the query ran in"), PromotedHit.Target.World.Get(), RowHit.Target.World.Get());
	TestTrue(TEXT("the promoted hit is addressable, which is what makes it usable as a send target"),
		PromotedHit.Target.IsSet());

	TestEqual(TEXT("a hit through an actor names no instanced row"), PromotedHit.InstanceIndex, INDEX_NONE);
	TestNotEqual(TEXT("which is the one thing that differs from a row hit"),
		PromotedHit.InstanceIndex, RowHit.InstanceIndex);

	// A world is not required to name the entity: the handle half of the target is filled either way, and
	// only the world half is left out. This is what lets the join be driven with no world at all.
	FCrowdyMassTraceHit WorldlessHit;
	TestTrue(TEXT("the entity is still named without a world"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, nullptr, WorldlessHit));
	TestTrue(TEXT("and still carries the handle"), WorldlessHit.Entity == Entity);
	TestFalse(TEXT("but its target is incomplete rather than pointing at some other world"),
		WorldlessHit.Target.IsSet());

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);

	return true;
}

/**
 * The id on a promoted hit comes from the entity and overwrites whatever was read off the actor.
 *
 * The actors are pooled: one is handed to the next entity that needs it, so an id carried on the actor can
 * name a player who is no longer standing there. The world fill reads the actor first and the join runs
 * after it, so the join declining to overwrite an id that is already set is the whole of the defect, and
 * it looks entirely healthy from the outside.
 *
 * The refusal half is here too: an entity that cannot be named leaves the hit exactly as it was rather than
 * claiming a Mass entity nobody can address.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierPromotedHitIdentityComesFromTheEntityTest,
	"CrowdySDK.Mass.NearTierPromotedHitIdentityComesFromTheEntity", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierPromotedHitIdentityComesFromTheEntityTest::RunTest(const FString& Parameters)
{
	const FGuid ActorLentUUID = FGuid::NewGuid();
	const FMassEntityHandle Entity(4, 2);

	FCrowdyRemoteIdentityFragment Identity;
	Identity.UUID = FGuid::NewGuid();

	// The state the actor-side population leaves behind: a pooled shell still answering with the id of the
	// entity it stood for last.
	FCrowdyMassTraceHit Hit;
	Hit.UUID = ActorLentUUID;
	Hit.bHitCrowdyEntity = true;

	TestTrue(TEXT("the entity is named"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, nullptr, Hit));
	TestEqual(TEXT("the id is the entity's own"), Hit.UUID, Identity.UUID);
	TestNotEqual(TEXT("and not the one the pooled actor was still carrying"), Hit.UUID, ActorLentUUID);

	// Fails closed. An entity whose identity fragment names nothing, an entity with no identity fragment at
	// all, and an unset handle are all left as an ordinary hit: whatever the actor said stands, and nothing
	// claims a Mass entity that cannot be addressed.
	FCrowdyRemoteIdentityFragment BlankIdentity;

	FCrowdyMassTraceHit UnnamedHit;
	UnnamedHit.UUID = ActorLentUUID;
	UnnamedHit.bHitCrowdyEntity = true;

	TestFalse(TEXT("an identity fragment carrying no valid id names nothing"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &BlankIdentity, nullptr, UnnamedHit));
	TestFalse(TEXT("so the hit is not reported as a Mass entity"), UnnamedHit.bHitMassEntity);
	TestEqual(TEXT("and the actor's own id is left alone"), UnnamedHit.UUID, ActorLentUUID);

	FCrowdyMassTraceHit NoFragmentHit;
	TestFalse(TEXT("an entity with no identity fragment names nothing"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, nullptr, nullptr, NoFragmentHit));
	TestFalse(TEXT("and reports no Mass entity"), NoFragmentHit.bHitMassEntity);

	FCrowdyMassTraceHit UnsetEntityHit;
	TestFalse(TEXT("an unset handle names nothing even with a perfectly good identity"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(FMassEntityHandle(), &Identity, nullptr, UnsetEntityHit));
	TestFalse(TEXT("and reports no Mass entity"), UnsetEntityHit.bHitMassEntity);

	return true;
}

/**
 * Naming the entity a hit landed on must leave the hit itself alone.
 *
 * Everything a caller does with a hit other than route it comes from the world query: where it landed, what
 * it landed on, and the bone the hit zone is resolved from. The join runs in the middle of filling that in,
 * so a join that reset or rebuilt the result would take the impact and the bone with it, and the entity
 * would then be correctly named at a zone of None.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierPromotedHitKeepsItsWorldFieldsTest,
	"CrowdySDK.Mass.NearTierPromotedHitKeepsItsWorldFields", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierPromotedHitKeepsItsWorldFieldsTest::RunTest(const FString& Parameters)
{
	UCrowdyMassHitZoneTable* Table = NewObject<UCrowdyMassHitZoneTable>(GetTransientPackage(), NAME_None, RF_Transient);
	FCrowdyMassHitZoneEntry HeadEntry;
	HeadEntry.BoneName = TEXT("head");
	HeadEntry.ZoneName = TEXT("Head");
	HeadEntry.DamageMultiplier = 2.5f;
	Table->Entries.Add(HeadEntry);
	Table->DefaultZoneName = TEXT("Limb");
	Table->DefaultDamageMultiplier = 0.75f;

	FCrowdyMassResolvedHitZoneTable Resolved;
	Resolved.Build(Table);

	// A hit filled in the way the world path fills one, down to the bone and the zone it resolves to.
	FHitResult WorldHit;
	WorldHit.bBlockingHit = true;
	WorldHit.Distance = 250.0f;
	WorldHit.BoneName = TEXT("head");

	FCrowdyMassTraceHit Hit;
	Hit.SetHitResult(WorldHit);
	Hit.BoneName = WorldHit.BoneName;
	Resolved.Resolve(Hit.BoneName, Hit.HitZoneName, Hit.DamageMultiplier);

	const FMassEntityHandle Entity(9, 1);
	FCrowdyRemoteIdentityFragment Identity;
	Identity.UUID = FGuid::NewGuid();

	TestTrue(TEXT("the entity is named"),
		CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, nullptr, Hit));

	TestTrue(TEXT("the impact is still a blocking hit"), Hit.bBlockingHit);
	TestEqual(TEXT("the distance survives"), Hit.HitResult.Distance, 250.0f);
	TestEqual(TEXT("the bone the zone is keyed on survives"), Hit.BoneName, FName(TEXT("head")));

	// The same bone resolves to the same zone whichever representation was hit, because both fills key the
	// lookup on the bone the query reported and resolve it against the one table this world holds.
	FName RowZoneName;
	float RowDamageMultiplier = 0.0f;
	Resolved.Resolve(TEXT("head"), RowZoneName, RowDamageMultiplier);

	TestEqual(TEXT("a hit through the actor resolves the zone a hit on the row would"), Hit.HitZoneName, RowZoneName);
	TestEqual(TEXT("with the same multiplier"), Hit.DamageMultiplier, RowDamageMultiplier);
	TestEqual(TEXT("which is the authored zone rather than the table's default"), Hit.HitZoneName, FName(TEXT("Head")));

	return true;
}

/**
 * What may suppress an entity's row is what the world query REPORTED, and reported means named.
 *
 * This is the whole round trip, driven in the order the trace path drives it: a finished world result is
 * recorded, and the row decision for each entity is then taken against that record. An entity the world
 * pass named is left to that hit; an entity it did not name is still tested as a row, whatever it is being
 * drawn by and whether or not an actor for it exists.
 *
 * The cell that matters is the second: an entity's actor is a pooled visual shell, so an object type it
 * does not answer, a class filter for a game's own pawn, or an ignore list all drop it from the world
 * query, and only the entities nearest the query are given an actor at all. Suppressing those rows loses
 * the two crowd members standing closest to the shot and loses them silently, because every other entity
 * still answers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierWorldReportedCoverageTest,
	"CrowdySDK.Mass.NearTierWorldReportedCoverage", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierWorldReportedCoverageTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierTraceTestFixture;

	// Both are fully promoted and identical in every respect the crowd pass can see. The only difference is
	// what the world query did with each one's actor.
	const FMassRepresentationFragment Promoted = FFixture::MakeRepresentation(
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, /*bWaiting=*/false);

	const FMassEntityHandle ReportedEntity(11, 1);
	const FMassEntityHandle FilteredEntity(12, 1);
	const FCrowdyRemoteIdentityFragment ReportedIdentity = FFixture::MakeIdentity();

	CrowdyMassTrace::FCrowdyMassWorldReportedEntities Reported;
	TestTrue(TEXT("nothing is reported before a world result has been recorded"), Reported.IsEmpty());

	const FCrowdyMassTraceHit NamedHit = FFixture::MakeNamedWorldHit(ReportedEntity, ReportedIdentity);
	if (!TestTrue(TEXT("the fixture's world hit really was named, so the recording below has something to record"),
		NamedHit.bHitMassEntity))
	{
		return false;
	}

	CrowdyMassTrace::RecordWorldReportedEntity(NamedHit, Reported);

	TestEqual(TEXT("the named result recorded exactly one entity"), Reported.Num(), 1);
	TestTrue(TEXT("and it is the entity that result named"), Reported.Contains(ReportedEntity));
	TestFalse(TEXT("the entity the world query never reached is not in the record"), Reported.Contains(FilteredEntity));

	TestFalse(TEXT("the reported entity is left to its world hit rather than tested as a row as well"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(Promoted, Reported.Contains(ReportedEntity)));
	TestTrue(TEXT("the identically promoted entity the world query dropped is still tested as a row, which is the only way anything can hit it"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(Promoted, Reported.Contains(FilteredEntity)));

	// A result that carries an entity handle without having named that entity is handed back as an ordinary
	// actor hit, and nothing downstream can route it to a player. Recording it would suppress the row of an
	// entity no caller was told about, which is the same hole as the filtered shell above reached by a
	// different route: what the record means is REPORTED, and a handle is not a report.
	const FMassEntityHandle UnnamedEntity(13, 1);
	CrowdyMassTrace::RecordWorldReportedEntity(FFixture::MakeUnnamedWorldHitOnActorOf(UnnamedEntity), Reported);

	TestFalse(TEXT("a world hit that could not be named records no entity"), Reported.Contains(UnnamedEntity));
	TestEqual(TEXT("so the record is no larger than it was"), Reported.Num(), 1);
	TestTrue(TEXT("and that entity is still tested as a row"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(Promoted, Reported.Contains(UnnamedEntity)));

	// A shot passing through two of one actor's components reports that entity twice, and the record is a
	// set: it answers whether the entity was reported, never how often.
	CrowdyMassTrace::RecordWorldReportedEntity(FFixture::MakeNamedWorldHit(ReportedEntity, ReportedIdentity), Reported);
	TestEqual(TEXT("one entity reported twice is one entry"), Reported.Num(), 1);

	// The overlap half of the same rule. A world overlap is filtered by class, and the pooled shell an entity
	// is given satisfies almost no gameplay class filter, so this is the path that loses entities routinely.
	CrowdyMassTrace::FCrowdyMassWorldReportedEntities OverlapReported;

	const FMassEntityHandle OverlappedEntity(21, 2);
	FCrowdyMassOverlapResult JoinedOverlap;
	JoinedOverlap.bOverlapMassEntity = true;
	JoinedOverlap.Entity = OverlappedEntity;
	CrowdyMassTrace::RecordWorldReportedEntity(JoinedOverlap, OverlapReported);

	TestTrue(TEXT("an overlap joined to an entity records it"), OverlapReported.Contains(OverlappedEntity));
	TestFalse(TEXT("so that entity is left to the overlap rather than reported again as a row"),
		CrowdyMassTrace::ShouldTraceEntityAsRow(Promoted, OverlapReported.Contains(OverlappedEntity)));

	// The same rule on the overlap side: a result carrying a handle it never named reports an actor and not
	// a player, so it may not suppress anything either.
	const FMassEntityHandle UnjoinedEntity(22, 2);
	FCrowdyMassOverlapResult UnjoinedOverlap;
	UnjoinedOverlap.bOverlapMassEntity = false;
	UnjoinedOverlap.Entity = UnjoinedEntity;
	CrowdyMassTrace::RecordWorldReportedEntity(UnjoinedOverlap, OverlapReported);

	TestFalse(TEXT("an overlap that named no entity records none"), OverlapReported.Contains(UnjoinedEntity));
	TestEqual(TEXT("and leaves the record as it was"), OverlapReported.Num(), 1);

	// An unset handle is what a result carries when it resolved to no entity at all, and is not a member of
	// anything: an entity walk asking about it must not find the record answering yes to everyone.
	CrowdyMassTrace::FCrowdyMassWorldReportedEntities EmptyRecord;
	EmptyRecord.Add(FMassEntityHandle());
	TestTrue(TEXT("an unset handle records nothing"), EmptyRecord.IsEmpty());
	TestFalse(TEXT("and an empty record reports nobody"), EmptyRecord.Contains(ReportedEntity));

	return true;
}

/**
 * One shot, one entity, one hit.
 *
 * The world query runs first and unconditionally, and a Mass hit is always synthesised as blocking, so a
 * promoted entity found by both queries arrives as two results. The prune the multi path applies keeps
 * every result BEFORE the first blocking one, so a non-blocking world hit followed by a blocking row hit
 * survives it intact: excluding the row is the only thing that makes such an entity hittable once, and the
 * counterfactual below is what shows the prune cannot stand in for it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierEntityIsReportedOnceTest,
	"CrowdySDK.Mass.NearTierEntityIsReportedOnce", CrowdyMassNearTierTraceTestFlags)
bool FCrowdyMassNearTierEntityIsReportedOnceTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierTraceTestFixture;

	const FMassEntityHandle Entity(21, 5);
	FCrowdyRemoteIdentityFragment Identity;
	Identity.UUID = FGuid::NewGuid();

	// Fully promoted: the actor is up, standing, and carrying collision.
	const FMassRepresentationFragment Representation = FFixture::MakeRepresentation(
		EMassRepresentationType::HighResSpawnedActor, EMassRepresentationType::SkinnedMeshInstance, /*bWaiting=*/false);

	// The world query found the actor and grazed it, which is a non-blocking hit: the shot carries on and
	// whatever is behind is reported after it.
	FCrowdyMassTraceHit WorldHit;
	FHitResult GrazingHit;
	GrazingHit.bBlockingHit = false;
	GrazingHit.Distance = 100.0f;
	WorldHit.SetHitResult(GrazingHit);
	CrowdyMassTrace::JoinEntityIdentityToTraceHit(Entity, &Identity, nullptr, WorldHit);

	// Recorded from the finished result, which is the order the trace path fills these in: every world hit
	// is named before the crowd is walked, so the record exists by the time the row decision is taken.
	CrowdyMassTrace::FCrowdyMassWorldReportedEntities Reported;
	CrowdyMassTrace::RecordWorldReportedEntity(WorldHit, Reported);

	// The row the Mass query would have synthesised for the same entity, always blocking.
	FCrowdyMassTraceHit RowHit;
	FHitResult SyntheticRowHit;
	SyntheticRowHit.bBlockingHit = true;
	SyntheticRowHit.Distance = 105.0f;
	RowHit.SetHitResult(SyntheticRowHit);
	RowHit.bHitMassEntity = true;
	RowHit.bHitCrowdyEntity = true;
	RowHit.UUID = Identity.UUID;
	RowHit.Entity = Entity;
	RowHit.InstanceIndex = 3;

	TArray<FCrowdyMassTraceHit> Hits;
	Hits.Add(WorldHit);
	if (CrowdyMassTrace::ShouldTraceEntityAsRow(Representation, Reported.Contains(Entity)))
	{
		Hits.Add(RowHit);
	}
	FFixture::PruneAfterFirstBlockingHitAsTheTraceDoes(Hits);

	TestEqual(TEXT("the shot reports the entity exactly once"),
		FFixture::CountHitsNaming(Hits, Identity.UUID), 1);
	if (TestEqual(TEXT("and reports nothing else"), Hits.Num(), 1))
	{
		TestEqual(TEXT("the one result is the hit on the actor, which names no row"),
			Hits[0].InstanceIndex, INDEX_NONE);
	}

	// The same two results with the row NOT excluded, to show what the prune does with them. It keeps
	// everything before the first blocking hit, so both survive and one player is reported twice: pruning
	// is not, and cannot be, what stops this.
	TArray<FCrowdyMassTraceHit> UnfilteredHits;
	UnfilteredHits.Add(WorldHit);
	UnfilteredHits.Add(RowHit);
	FFixture::PruneAfterFirstBlockingHitAsTheTraceDoes(UnfilteredHits);

	TestEqual(TEXT("without the exclusion the same entity is reported twice, and the prune keeps both"),
		FFixture::CountHitsNaming(UnfilteredHits, Identity.UUID), 2);

	// The same promoted entity, in a shot whose world query dropped its actor: a class filter, an object
	// type the pooled shell does not answer, or an ignore list. Nothing was recorded for it, so the row is
	// added, and the entity is reported exactly once again, this time as the row. Once is the requirement in
	// both directions, and the entity that is never reported at all is the harder half to notice, because
	// every other entity in the same shot still answers.
	CrowdyMassTrace::FCrowdyMassWorldReportedEntities NothingReported;

	TArray<FCrowdyMassTraceHit> FilteredShotHits;
	if (CrowdyMassTrace::ShouldTraceEntityAsRow(Representation, NothingReported.Contains(Entity)))
	{
		FilteredShotHits.Add(RowHit);
	}
	FFixture::PruneAfterFirstBlockingHitAsTheTraceDoes(FilteredShotHits);

	TestEqual(TEXT("an entity whose actor the world query dropped is still reported exactly once"),
		FFixture::CountHitsNaming(FilteredShotHits, Identity.UUID), 1);
	if (TestEqual(TEXT("by the one result the shot produced"), FilteredShotHits.Num(), 1))
	{
		TestTrue(TEXT("which is its row, since nothing reported the actor"),
			FilteredShotHits[0].InstanceIndex != INDEX_NONE);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
