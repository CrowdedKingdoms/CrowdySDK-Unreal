// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "GameModel/CrowdyEffectPlanCache.h"
#include "GameModel/CrowdySchemaSync.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectPlanCacheTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Deliberately distinct names from the schema-sync tests' own fixtures: adaptive unity merges this module's
	// translation units, so two anonymous-namespace helpers sharing a name would redefine each other.
	FCrowdyDesiredPropertyDef MakeCacheProp(
		const FString& Key, const FString& ValueType, const FString& DefaultJson = TEXT("0"))
	{
		FCrowdyDesiredPropertyDef Prop;
		Prop.Key = Key;
		Prop.ValueType = ValueType;
		Prop.DefaultValueJson = DefaultJson;
		return Prop;
	}

	FCrowdyDesiredContainerType MakeCacheType(
		const FString& TypeName, const TArray<FCrowdyDesiredPropertyDef>& Props)
	{
		FCrowdyDesiredContainerType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.Props = Props;
		return Type;
	}

	// A record as a previous plan would have stored it: compiled cleanly against TypeName, under the given content
	// key. bReusable mirrors what BuildRecords stamps on anything it stores.
	FCrowdyEffectPlanRecord MakeStoredRecord(
		const FString& AssetPath, const FString& TypeName, const FString& PackageHash, const FString& VocabularyHash)
	{
		FCrowdyEffectPlanRecord Record;
		Record.AssetPath = AssetPath;
		Record.TargetTypeName = TypeName;
		Record.EffectiveFunctionName = TEXT("take_damage");
		Record.Function.Name = TEXT("take_damage");
		Record.Function.ContainerTypeName = TypeName;
		Record.PackageHash = PackageHash;
		Record.VocabularyHash = VocabularyHash;
		Record.bReusable = true;
		return Record;
	}
}

// The saved package hash is the half of the key that covers the asset's own content, including the editor-only node
// graph no field-by-field hash could see. A different hash is a different effect, whatever else matches.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCachePackageHashMovesTheKeyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCachePackageHashMovesTheKey", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCachePackageHashMovesTheKeyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Types =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }) };
	const FString Vocabulary = FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), Types);

	const FCrowdyEffectPlanRecord Record =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), Vocabulary);

	TestTrue(TEXT("An unchanged package with an unchanged vocabulary is reused"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("aaaa"), Vocabulary, /*bPackageDirty*/ false));
	TestFalse(TEXT("A package saved since the record was stored is recompiled"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("bbbb"), Vocabulary, /*bPackageDirty*/ false));
	return true;
}

// The source half of the vocabulary key. An effect that declares a source container type has its source.<attr> reads
// checked against THAT type's attributes, so renaming one there has to invalidate the effect too. Nothing else in the
// key can see it: the effect asset is untouched, and its target type's attributes have not moved, so without this the
// stored compile is served and the sync pushes a function naming an attribute the source no longer has.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheSourceAttributeMovesTheKeyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheSourceAttributeMovesTheKey", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheSourceAttributeMovesTheKeyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Before =
		{ MakeCacheType(TEXT("Adventurer"), { MakeCacheProp(TEXT("hp"), TEXT("int")) }),
		  MakeCacheType(TEXT("Trap"), { MakeCacheProp(TEXT("charge"), TEXT("int")) }) };
	// Only the SOURCE type changed: charge became power. The target type is byte-identical.
	const TArray<FCrowdyDesiredContainerType> After =
		{ MakeCacheType(TEXT("Adventurer"), { MakeCacheProp(TEXT("hp"), TEXT("int")) }),
		  MakeCacheType(TEXT("Trap"), { MakeCacheProp(TEXT("power"), TEXT("int")) }) };

	const FString BeforeHash =
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), TEXT("Trap"), Before);
	const FString AfterHash =
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), TEXT("Trap"), After);

	TestNotEqual(TEXT("Renaming an attribute on the declared source type moves the vocabulary key"),
		BeforeHash, AfterHash);

	// The same rename is invisible to a key that covers the target type alone, which is the defect this guards.
	TestEqual(TEXT("The target-only key cannot see a source-side rename"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), Before),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), After));

	FCrowdyEffectPlanRecord Record =
		MakeStoredRecord(TEXT("/Game/FX/TrapHit.TrapHit"), TEXT("Adventurer"), TEXT("aaaa"), BeforeHash);
	Record.SourceTypeName = TEXT("Trap");
	TestFalse(TEXT("A record compiled against the old source vocabulary is recompiled"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("aaaa"), AfterHash, /*bPackageDirty*/ false));
	return true;
}

// An effect that declares no source type shares its target's schema, and its key must not move because the feature
// exists: every effect stored before source types were authored has to stay reusable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheNoSourceTypeKeysAsBeforeTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheNoSourceTypeKeysAsBefore", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheNoSourceTypeKeysAsBeforeTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Types =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }),
		  MakeCacheType(TEXT("Trap"), { MakeCacheProp(TEXT("charge"), TEXT("int")) }) };

	TestEqual(TEXT("An empty source type name hashes exactly as the target-only key does"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), FString(), Types),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), Types));

	// And declaring one is a different key, so the first plan after an author declares a source type recompiles
	// rather than serving the compile that never checked it.
	TestNotEqual(TEXT("Declaring a source type is a different key"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), FString(), Types),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), TEXT("Trap"), Types));
	return true;
}

// A declared source type that no longer exists in the reflected schema must key differently from one that exists with
// no attributes, for the same reason the target half does: "the type is gone" and "the type is empty" are different
// answers, and the first one has to recompile once the class arrives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheAbsentSourceTypeKeysDistinctlyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheAbsentSourceTypeKeysDistinctly", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheAbsentSourceTypeKeysDistinctlyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> WithoutTrap =
		{ MakeCacheType(TEXT("Adventurer"), { MakeCacheProp(TEXT("hp"), TEXT("int")) }) };
	const TArray<FCrowdyDesiredContainerType> WithEmptyTrap =
		{ MakeCacheType(TEXT("Adventurer"), { MakeCacheProp(TEXT("hp"), TEXT("int")) }),
		  MakeCacheType(TEXT("Trap"), {}) };

	TestNotEqual(TEXT("A missing source type and an attribute-less one are different keys"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), TEXT("Trap"), WithoutTrap),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Adventurer"), TEXT("Trap"), WithEmptyTrap));
	return true;
}

// The vocabulary half. An effect compiles against its target type's attributes, so adding one has to invalidate every
// effect on that type even though no effect asset changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheAddedAttributeMovesTheKeyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheAddedAttributeMovesTheKey", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheAddedAttributeMovesTheKeyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Before =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }) };
	const TArray<FCrowdyDesiredContainerType> After =
		{ MakeCacheType(TEXT("Combatant"),
			{ MakeCacheProp(TEXT("health"), TEXT("int")), MakeCacheProp(TEXT("armour"), TEXT("int")) }) };

	const FString BeforeHash = FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), Before);
	const FString AfterHash = FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), After);

	TestNotEqual(TEXT("Adding an attribute to the target type moves the vocabulary key"), BeforeHash, AfterHash);

	const FCrowdyEffectPlanRecord Record =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), BeforeHash);
	TestFalse(TEXT("A record compiled against the old vocabulary is recompiled"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("aaaa"), AfterHash, /*bPackageDirty*/ false));
	return true;
}

// Retyping an attribute changes what an assignment lowers to (an int assignment rounds, a float does not), so it must
// move the key even though the attribute set is otherwise identical.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheValueTypeMovesTheKeyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheValueTypeMovesTheKey", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheValueTypeMovesTheKeyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> AsInt =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }) };
	const TArray<FCrowdyDesiredContainerType> AsFloat =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("float")) }) };

	TestNotEqual(TEXT("Retyping an attribute moves the vocabulary key"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), AsInt),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), AsFloat));
	return true;
}

// An attribute's native clamp bounds are inherited by every assignment the lowering emits, so they are part of what an
// effect compiled against even though the server schema has no clamp concept and never sees them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheClampBoundsMoveTheKeyTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheClampBoundsMoveTheKey", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheClampBoundsMoveTheKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredPropertyDef Unclamped = MakeCacheProp(TEXT("health"), TEXT("int"));
	FCrowdyDesiredPropertyDef Clamped = Unclamped;
	Clamped.bHasClamp = true;
	Clamped.ClampMin = 0.0;
	Clamped.ClampMax = 100.0;

	FCrowdyDesiredPropertyDef Rebalanced = Clamped;
	Rebalanced.ClampMax = 200.0;

	const FString NoClampHash = FCrowdyEffectPlanCache::ComputeVocabularyHash(
		TEXT("Combatant"), { MakeCacheType(TEXT("Combatant"), { Unclamped }) });
	const FString ClampHash = FCrowdyEffectPlanCache::ComputeVocabularyHash(
		TEXT("Combatant"), { MakeCacheType(TEXT("Combatant"), { Clamped }) });
	const FString RebalancedHash = FCrowdyEffectPlanCache::ComputeVocabularyHash(
		TEXT("Combatant"), { MakeCacheType(TEXT("Combatant"), { Rebalanced }) });

	TestNotEqual(TEXT("Gaining a clamp moves the vocabulary key"), NoClampHash, ClampHash);
	TestNotEqual(TEXT("Widening a clamp moves the vocabulary key"), ClampHash, RebalancedHash);
	return true;
}

// The key must describe the attribute SET, not the order reflection happened to walk it in: a member reorder in a
// container class is not a reason to recompile every effect that targets it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheKeyIsStableAcrossReorderingTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheKeyIsStableAcrossReordering", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheKeyIsStableAcrossReorderingTest::RunTest(const FString& Parameters)
{
	const FCrowdyDesiredPropertyDef Health = MakeCacheProp(TEXT("health"), TEXT("int"), TEXT("100"));
	const FCrowdyDesiredPropertyDef Armour = MakeCacheProp(TEXT("armour"), TEXT("int"), TEXT("5"));
	const FCrowdyDesiredPropertyDef Name = MakeCacheProp(TEXT("name"), TEXT("string"), TEXT("\"\""));

	const TArray<FCrowdyDesiredContainerType> OneOrder =
		{ MakeCacheType(TEXT("Combatant"), { Health, Armour, Name }) };
	const TArray<FCrowdyDesiredContainerType> AnotherOrder =
		{ MakeCacheType(TEXT("Combatant"), { Name, Health, Armour }) };

	TestEqual(TEXT("Reordering the same attribute set leaves the vocabulary key unchanged"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), OneOrder),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), AnotherOrder));
	return true;
}

// A type that has vanished from the reflected schema and a type that merely has no attributes are different
// situations, so they must not key the same: the first has to recompile, the second may not need to.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheAbsentTypeDiffersFromEmptyTypeTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheAbsentTypeDiffersFromEmptyType", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheAbsentTypeDiffersFromEmptyTypeTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> WithEmptyType =
		{ MakeCacheType(TEXT("Combatant"), {}) };

	TestNotEqual(TEXT("An absent type keys differently from a type with no attributes"),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), TArray<FCrowdyDesiredContainerType>()),
		FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), WithEmptyType));
	return true;
}

// Unsaved edits are invisible to the saved package hash, so a resident dirty package is the one case that overrides a
// full key match. Getting this wrong writes a stale function to a live server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheDirtyPackageAlwaysMissesTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheDirtyPackageAlwaysMisses", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheDirtyPackageAlwaysMissesTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Types =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }) };
	const FString Vocabulary = FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), Types);

	const FCrowdyEffectPlanRecord Record =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), Vocabulary);

	TestTrue(TEXT("A clean package with a fully matching key is reused"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("aaaa"), Vocabulary, /*bPackageDirty*/ false));
	TestFalse(TEXT("A dirty package is recompiled even when every hash matches"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, TEXT("aaaa"), Vocabulary, /*bPackageDirty*/ true));
	return true;
}

// A package the registry has no saved hash for cannot be shown to be unchanged, so it is never reused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheUnknownHashMissesTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheUnknownHashMisses", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheUnknownHashMissesTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Types =
		{ MakeCacheType(TEXT("Combatant"), { MakeCacheProp(TEXT("health"), TEXT("int")) }) };
	const FString Vocabulary = FCrowdyEffectPlanCache::ComputeVocabularyHash(TEXT("Combatant"), Types);

	const FCrowdyEffectPlanRecord Record =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), Vocabulary);

	TestFalse(TEXT("A probe with no package hash is recompiled"),
		FCrowdyEffectPlanCache::IsRecordReusable(Record, FString(), Vocabulary, /*bPackageDirty*/ false));

	FCrowdyEffectPlanRecord Hashless = Record;
	Hashless.PackageHash.Reset();
	TestFalse(TEXT("A record stored with no package hash is recompiled"),
		FCrowdyEffectPlanCache::IsRecordReusable(Hashless, TEXT("aaaa"), Vocabulary, /*bPackageDirty*/ false));
	return true;
}

// A record that failed to compile, or that never resolved a container type, is waiting on something outside both
// halves of the key, so remembering it would keep the effect broken after the missing piece arrived.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheDoesNotStoreAnUnresolvedRecordTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheDoesNotStoreAnUnresolvedRecord", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheDoesNotStoreAnUnresolvedRecordTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectPlanRecord Good =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), TEXT("vvvv"));
	TestTrue(TEXT("A cleanly compiled record is worth storing"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(Good, /*bPackageDirty*/ false));

	FCrowdyEffectPlanRecord Broken = Good;
	Broken.bCompileFailed = true;
	TestFalse(TEXT("A record whose compile failed is never stored"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(Broken, /*bPackageDirty*/ false));

	FCrowdyEffectPlanRecord NoType = Good;
	NoType.TargetTypeName.Reset();
	TestFalse(TEXT("A record whose target container type did not resolve is never stored"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(NoType, /*bPackageDirty*/ false));

	FCrowdyEffectPlanRecord NoHash = Good;
	NoHash.PackageHash.Reset();
	TestFalse(TEXT("A record with no package hash to key on is never stored"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(NoHash, /*bPackageDirty*/ false));
	return true;
}

// A compile of an asset with unsaved edits describes content that is in no package on disk, and the only key there is
// to file it under is the SAVED package's hash, which describes the content the designer can still get back. Reloading
// or reverting the asset restores exactly that content and leaves the hash where it was, so a stored record would then
// be served as a valid hit and the sync would ship edits that were thrown away.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheDoesNotStoreADirtyCompileTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheDoesNotStoreADirtyCompile", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheDoesNotStoreADirtyCompileTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectPlanRecord Good =
		MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), TEXT("vvvv"));

	TestTrue(TEXT("The same record compiled from the saved asset is worth storing"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(Good, /*bPackageDirty*/ false));
	TestFalse(TEXT("A record compiled from unsaved edits is never stored, however clean the compile was"),
		FCrowdyEffectPlanCache::ShouldStoreRecord(Good, /*bPackageDirty*/ true));
	return true;
}

// The project settings a compile reads are not part of any single asset's key, so moving one has to drop the whole
// store rather than leave every effect looking unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheSaltChangeEmptiesTheStoreTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheSaltChangeEmptiesTheStore", CrowdyEffectPlanCacheTestFlags)

bool FCrowdyEffectPlanCacheSaltChangeEmptiesTheStoreTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectPlanCache Cache;
	Cache.BeginPlan(TEXT("carrier=1"));
	Cache.Store(MakeStoredRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("aaaa"), TEXT("vvvv")));

	TestNotNull(TEXT("A stored record is found again under the same salt"), Cache.Find(TEXT("/Game/FX/Hit.Hit")));
	Cache.BeginPlan(TEXT("carrier=1"));
	TestNotNull(TEXT("Re-planning under an unchanged salt keeps the store"), Cache.Find(TEXT("/Game/FX/Hit.Hit")));

	Cache.BeginPlan(TEXT("carrier=2"));
	TestNull(TEXT("A changed salt drops every stored record"), Cache.Find(TEXT("/Game/FX/Hit.Hit")));
	TestEqual(TEXT("The store is empty after a salt change"), Cache.Num(), 0);
	return true;
}

#endif
