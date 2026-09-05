// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelSnapshot.h"
#include "GameModel/CrowdyModelVocabulary.h"
#include "Model/CrowdyStudioTypes.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h" // the row-identity rule a highlight is carried across by
#include "UI/GameModel/SCrowdyProvenanceGlyph.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelClassificationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// These builders are deliberately named apart from the ones in CrowdyModelLedgerTests.cpp: the unity build merges
	// this module's .cpp files into shared translation units, so two free functions of the same name in different
	// files of one module redefine each other.

	TSharedPtr<FStudioContainerType> ClassifiedType(const FString& TypeName)
	{
		TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
		Type->TypeName = TypeName;
		return Type;
	}

	TSharedPtr<FStudioPropertyDef> ClassifiedAttribute(
		const FString& TypeName, const FString& Key, const FString& ValueType = TEXT("int"))
	{
		TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
		Def->ContainerTypeName = TypeName;
		Def->Key = Key;
		Def->ValueType = ValueType;
		return Def;
	}

	TSharedPtr<FStudioFunction> ClassifiedFunction(const FString& TypeName, const FString& Name)
	{
		TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
		Function->ContainerTypeName = TypeName;
		Function->Name = Name;
		return Function;
	}

	TSharedPtr<FStudioAutomation> ClassifiedAutomation(const FString& TypeName, const FString& Name)
	{
		TSharedPtr<FStudioAutomation> Automation = MakeShared<FStudioAutomation>();
		Automation->TargetTypeName = TypeName;
		Automation->Name = Name;
		Automation->TriggerType = TEXT("schedule");
		Automation->ScheduleKind = TEXT("interval");
		Automation->IntervalMs = 60000;
		return Automation;
	}

	// One model declared in code, with the attributes named.
	void DeclareType(FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const TArray<FString>& AttributeKeys,
		const FString& ClassPath = TEXT("/Game/Models/BP_Hero.BP_Hero_C"))
	{
		FCrowdyModelSnapshotType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.OwningClassPath = ClassPath;
		for (const FString& Key : AttributeKeys)
		{
			Type.Attributes.Add({ Key, TEXT("int") });
		}
		Snapshot.Types.Add(MoveTemp(Type));
	}

	void DeclareFunction(FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const FString& Name,
		const FString& AssetPath = TEXT("/Game/Effects/FX_Damage.FX_Damage"))
	{
		FCrowdyModelSnapshotFunction Function;
		Function.TypeName = TypeName;
		Function.Name = Name;
		Function.ReturnType = TEXT("int");
		Function.AssetPath = AssetPath;
		Snapshot.Functions.Add(MoveTemp(Function));
		Snapshot.FunctionAuthors.Add({ TypeName, Name, AssetPath });
	}

	void DeclareAutomation(FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const FString& Name,
		const FString& AssetPath = TEXT("/Game/Effects/FX_Regen.FX_Regen"))
	{
		FCrowdyModelSnapshotAutomation Automation;
		Automation.Name = Name;
		Automation.TargetTypeName = TypeName;
		Automation.AssetPath = AssetPath;
		Automation.TriggerType = TEXT("schedule");
		Automation.ScheduleKind = TEXT("interval");
		Automation.IntervalMs = 300000;
		Snapshot.Automations.Add(MoveTemp(Automation));
		Snapshot.AutomationAuthors.Add({ FString(), Name, AssetPath });
	}

	FString ScopedKey(const FString& TypeName, const FString& Name)
	{
		return FCrowdySchemaSync::ScopedNameKey(TypeName, Name);
	}

	FString AutomationKey(const FString& Name)
	{
		return FCrowdySchemaSync::ScopedNameKey(FString(), Name);
	}

	// One effect asset's compile facts as the sweep would have produced them for an asset that will not compile. Named
	// apart from the fixtures in the other test files of this module: adaptive unity merges its translation units, so
	// two anonymous-namespace helpers sharing a name redefine each other.
	FCrowdyEffectPlanRecord BrokenEffectRecord(
		const FString& AssetPath, const FString& TargetTypeName, const FString& FunctionName)
	{
		FCrowdyEffectPlanRecord Record;
		Record.AssetPath = AssetPath;
		// Resolved before the compile is attempted, so it survives the failure. Empty when the effect's container class
		// could not be resolved at all, which is the case the compile fails on first.
		Record.TargetTypeName = TargetTypeName;
		Record.EffectiveFunctionName = FunctionName;
		Record.bCompileFailed = true;
		Record.FirstCompileError = TEXT("line 2: unknown attribute 'helth'");
		return Record;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationNoSnapshotIsSilentTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationNoSnapshotIsSilent", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationNoSnapshotIsSilentTest::RunTest(const FString& Parameters)
{
	// The state the browser shipped in: nobody has pressed Preview changes, so nothing can be claimed about any row.
	// Anything other than blank here would be an invention, and "server-only" in particular would be a lie about
	// every model the project declares.
	const FCrowdyModelClassification Model = CrowdyModelLedger::ClassifyModel(nullptr, TEXT("Hero"));
	const FCrowdyModelClassification Attribute = CrowdyModelLedger::ClassifyAttribute(nullptr, TEXT("Hero"), TEXT("Health"));
	const FCrowdyModelClassification Function = CrowdyModelLedger::ClassifyFunction(nullptr, TEXT("Hero"), TEXT("DealDamage"));
	const FCrowdyModelClassification Automation = CrowdyModelLedger::ClassifyAutomation(nullptr, TEXT("HeroRegen"));

	TestTrue(TEXT("A model says nothing about where it came from"), Model.Provenance == ECrowdyModelProvenance::Unknown);
	TestTrue(TEXT("An attribute says nothing either"), Attribute.Provenance == ECrowdyModelProvenance::Unknown);
	TestTrue(TEXT("Nor a function"), Function.Provenance == ECrowdyModelProvenance::Unknown);
	TestTrue(TEXT("Nor an automation"), Automation.Provenance == ECrowdyModelProvenance::Unknown);
	TestTrue(TEXT("Nothing claims a difference"), Model.Drift == ECrowdyModelDrift::None
		&& Attribute.Drift == ECrowdyModelDrift::None
		&& Function.Drift == ECrowdyModelDrift::None
		&& Automation.Drift == ECrowdyModelDrift::None);

	// And that has to reach the columns as blank text, not as a word for "unknown".
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(ClassifiedType(TEXT("Hero")));
	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(ClassifiedFunction(TEXT("Hero"), TEXT("DealDamage")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, Functions, {});
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildFunctionRows(Functions, TEXT("Hero"));

	TestEqual(TEXT("Exactly the server's model is listed, with nothing invented"), Models.Num(), 1);
	TestEqual(TEXT("Exactly the server's function is listed"), Rows.Num(), 1);
	if (Models.Num() == 1 && Rows.Num() == 1)
	{
		TestEqual(TEXT("The model's Source cell is blank"), Models[0].ProvenanceText, FString());
		TestEqual(TEXT("The model's Status cell is blank"), Models[0].DriftText, FString());
		TestEqual(TEXT("The row's Source cell is blank"), Rows[0].ProvenanceText, FString());
		TestEqual(TEXT("The row's Status cell is blank"), Rows[0].DriftText, FString());
		TestFalse(TEXT("Nothing was synthesized"), Models[0].bCodeOnly || Rows[0].bCodeOnly);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationCodeSyncedTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationCodeSynced", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationCodeSyncedTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));

	TestTrue(TEXT("A model the code declares with no change planned is in sync"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero")).Provenance == ECrowdyModelProvenance::CodeSynced);
	TestTrue(TEXT("So is its attribute"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("Health")).Provenance == ECrowdyModelProvenance::CodeSynced);
	TestTrue(TEXT("So is its function"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage")).Provenance == ECrowdyModelProvenance::CodeSynced);
	TestTrue(TEXT("So is its automation"),
		CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("HeroRegen")).Provenance == ECrowdyModelProvenance::CodeSynced);

	TestTrue(TEXT("Nothing differs, so the Status column stays silent"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero")).Drift == ECrowdyModelDrift::None
		&& CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage")).Drift == ECrowdyModelDrift::None);

	TestEqual(TEXT("A model names the class that declares it"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero")).CodePath,
		FString(TEXT("/Game/Models/BP_Hero.BP_Hero_C")));
	TestEqual(TEXT("A function names the effect asset that authors it"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage")).CodePath,
		FString(TEXT("/Game/Effects/FX_Damage.FX_Damage")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationCodeDriftedTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationCodeDrifted", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationCodeDriftedTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));

	Snapshot.TypeUpdates.Add(TEXT("Hero"));
	Snapshot.AttributeUpdates.Add(ScopedKey(TEXT("Hero"), TEXT("Health")));
	Snapshot.FunctionUpdates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.AutomationUpdates.Add(AutomationKey(TEXT("HeroRegen")));

	const FCrowdyModelClassification Model = CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero"));
	const FCrowdyModelClassification Attribute = CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("Health"));
	const FCrowdyModelClassification Function = CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	const FCrowdyModelClassification Automation = CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("HeroRegen"));

	TestTrue(TEXT("An update planned against a live entity is a code change the server has not got"),
		Model.Provenance == ECrowdyModelProvenance::CodeDrifted
		&& Attribute.Provenance == ECrowdyModelProvenance::CodeDrifted
		&& Function.Provenance == ECrowdyModelProvenance::CodeDrifted
		&& Automation.Provenance == ECrowdyModelProvenance::CodeDrifted);
	TestTrue(TEXT("And it says so"),
		Model.Drift == ECrowdyModelDrift::ChangedInCode
		&& Attribute.Drift == ECrowdyModelDrift::ChangedInCode
		&& Function.Drift == ECrowdyModelDrift::ChangedInCode
		&& Automation.Drift == ECrowdyModelDrift::ChangedInCode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationCodeNotPushedTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationCodeNotPushed", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationCodeNotPushedTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));

	Snapshot.TypeCreates.Add(TEXT("Hero"));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Health")));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.AutomationCreates.Add(AutomationKey(TEXT("HeroRegen")));

	const FCrowdyModelClassification Model = CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero"));
	const FCrowdyModelClassification Attribute = CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("Health"));
	const FCrowdyModelClassification Function = CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	const FCrowdyModelClassification Automation = CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("HeroRegen"));

	TestTrue(TEXT("A create planned for it means the server has never had it"),
		Model.Provenance == ECrowdyModelProvenance::CodeNotPushed
		&& Attribute.Provenance == ECrowdyModelProvenance::CodeNotPushed
		&& Function.Provenance == ECrowdyModelProvenance::CodeNotPushed
		&& Automation.Provenance == ECrowdyModelProvenance::CodeNotPushed);
	TestTrue(TEXT("And it says so"),
		Model.Drift == ECrowdyModelDrift::NotOnServerYet
		&& Attribute.Drift == ECrowdyModelDrift::NotOnServerYet
		&& Function.Drift == ECrowdyModelDrift::NotOnServerYet
		&& Automation.Drift == ECrowdyModelDrift::NotOnServerYet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationServerOnlyTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationServerOnly", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationServerOnlyTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));

	TestTrue(TEXT("A model no class declares lives only on the server"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("LegacyChest")).Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("An attribute no class declares does too, even on a model that is in code"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("LegacyMana")).Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("So does a function no effect authors"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("legacy_heal")).Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("So does an automation no effect authors"),
		CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("legacy_tick")).Provenance == ECrowdyModelProvenance::ServerOnly);

	TestTrue(TEXT("Each says why"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("LegacyChest")).Drift == ECrowdyModelDrift::OnlyOnServer
		&& CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("legacy_heal")).Drift == ECrowdyModelDrift::OnlyOnServer);
	TestEqual(TEXT("Nothing in the project declares it, so there is no path to open"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("LegacyChest")).CodePath, FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationKitOwnedIsNotCodeTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationKitOwnedIsNotCode", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationKitOwnedIsNotCodeTest::RunTest(const FString& Parameters)
{
	// Whether an entity is authored in code must be answered from the code side, not by asking whether the plan left
	// it off its server-only prune list. Kit-deployed schema is exactly the case that separates the two: it is kept
	// off that list on purpose, so an inversion would report every kit type as code-synced.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	Snapshot.RecognizedKitTypePrefixes.Add(TEXT("CK"));
	Snapshot.RecognizedKitTypeNames.Add(TEXT("Combatant"));
	Snapshot.RecognizedKitFunctionNames.Add(TEXT("advance_time"));

	const FCrowdyModelClassification ByPrefix = CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("CKArena"));
	const FCrowdyModelClassification ByExactName = CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Combatant"));

	TestTrue(TEXT("A type carrying a deployed kit's prefix is the kit's"),
		ByPrefix.Provenance == ECrowdyModelProvenance::KitOwned);
	TestTrue(TEXT("So is one whose bare name the deploy seeded, which no prefix rule could recognize"),
		ByExactName.Provenance == ECrowdyModelProvenance::KitOwned);
	TestFalse(TEXT("Neither is claimed as code, which is what inverting the prune list would have said"),
		ByPrefix.Provenance == ECrowdyModelProvenance::CodeSynced
		|| ByExactName.Provenance == ECrowdyModelProvenance::CodeSynced);
	TestTrue(TEXT("A kit owns its namespace, so this is not drift"),
		ByPrefix.Drift == ECrowdyModelDrift::None && ByExactName.Drift == ECrowdyModelDrift::None);

	TestTrue(TEXT("A kit function is recognized by its snake-cased prefix"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("CKArena"), TEXT("ck_arena_start")).Provenance
			== ECrowdyModelProvenance::KitOwned);
	TestTrue(TEXT("And by the exact name a deploy seeded"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Combatant"), TEXT("advance_time")).Provenance
			== ECrowdyModelProvenance::KitOwned);
	TestTrue(TEXT("A verb-first kit function is recognized by the kit type it lives on"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("CKArena"), TEXT("open_guild_hall")).Provenance
			== ECrowdyModelProvenance::KitOwned);
	TestTrue(TEXT("An attribute on a kit type belongs to the kit too"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("CKArena"), TEXT("Rounds")).Provenance
			== ECrowdyModelProvenance::KitOwned);
	TestTrue(TEXT("An automation named from a kit function belongs to the kit"),
		CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("ck_arena_tick")).Provenance
			== ECrowdyModelProvenance::KitOwned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationSkippedAuthorIsNotServerOnlyTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationSkippedAuthorIsNotServerOnly", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationSkippedAuthorIsNotServerOnlyTest::RunTest(const FString& Parameters)
{
	// The other half of the same trap, in the opposite direction: an effect that would not compile owns a live server
	// function, and the plan keeps that function off the prune list precisely so it is not lost. Reading the prune
	// list backwards would call it code-synced; reading only the desired schema would call it server-only. It is
	// neither, because this plan could not produce the code side at all.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	Snapshot.UncheckableFunctionKeys.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.FunctionAuthors.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_Broken.FX_Broken") });
	Snapshot.UncheckableAutomationKeys.Add(AutomationKey(TEXT("HeroRegen")));
	Snapshot.AutomationAuthors.Add({ FString(), TEXT("HeroRegen"), TEXT("/Game/Effects/FX_Broken.FX_Broken") });

	const FCrowdyModelClassification Function = CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	const FCrowdyModelClassification Automation = CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("HeroRegen"));

	TestTrue(TEXT("Nothing can be said about where the live copy stands"),
		Function.Provenance == ECrowdyModelProvenance::Unknown);
	TestFalse(TEXT("It is emphatically not an orphan, which is what a prune would act on"),
		Function.Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("The Status column says so in words"), Function.Drift == ECrowdyModelDrift::CannotBeChecked);
	TestEqual(TEXT("The asset that claims it is still nameable, which is where the fix is"),
		Function.CodePath, FString(TEXT("/Game/Effects/FX_Broken.FX_Broken")));

	TestTrue(TEXT("An automation of a passed-over effect reads the same way"),
		Automation.Provenance == ECrowdyModelProvenance::Unknown
		&& Automation.Drift == ECrowdyModelDrift::CannotBeChecked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationConflictNeedsAFixTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationConflictNeedsAFix", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationConflictNeedsAFixTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	Snapshot.NeedsAFixFunctionKeys.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.FunctionAuthors.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_A.FX_A") });
	Snapshot.FunctionAuthors.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_B.FX_B") });
	Snapshot.NeedsAFixAutomationKeys.Add(AutomationKey(TEXT("HeroRegen")));
	Snapshot.AutomationAuthors.Add({ FString(), TEXT("HeroRegen"), TEXT("/Game/Effects/FX_A.FX_A") });

	const FCrowdyModelClassification Function = CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	const FCrowdyModelClassification Automation = CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("HeroRegen"));

	TestTrue(TEXT("A name two assets claim needs a decision before anything can move"),
		Function.Drift == ECrowdyModelDrift::NeedsAFix && Automation.Drift == ECrowdyModelDrift::NeedsAFix);
	TestFalse(TEXT("It came from the project, so it is never reported as server-only"),
		Function.Provenance == ECrowdyModelProvenance::ServerOnly
		|| Automation.Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("Nothing was synced for it, so the live copy cannot be claimed to match code"),
		Function.Provenance == ECrowdyModelProvenance::CodeDrifted);
	TestEqual(TEXT("One of the claiming assets is named; the plan report lists them all"),
		Function.CodePath, FString(TEXT("/Game/Effects/FX_A.FX_A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationIdentityIsCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationIdentityIsCaseSensitive", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationIdentityIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// A type name is a server key, and both FString comparison and TSet<FString> fold case. Two types differing only
	// in case are two different models with two different server schemas, and confusing them hands one of them the
	// other's verdict.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	Snapshot.TypeUpdates.Add(TEXT("Hero"));
	Snapshot.FunctionUpdates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));

	TestTrue(TEXT("The declared model is the one the update was planned for"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("Hero")).Provenance == ECrowdyModelProvenance::CodeDrifted);
	TestTrue(TEXT("A differently-cased type name is a different model the project does not declare"),
		CrowdyModelLedger::ClassifyModel(&Snapshot, TEXT("hero")).Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("A differently-cased attribute key is a different attribute"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("health")).Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("A differently-cased function name is a different function"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("dealdamage")).Provenance == ECrowdyModelProvenance::ServerOnly);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationAttributeIsScopedByItsModelTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationAttributeIsScopedByItsModel", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationAttributeIsScopedByItsModelTest::RunTest(const FString& Parameters)
{
	// The same attribute key on two models is two server property definitions, so each has to answer for itself.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareType(Snapshot, TEXT("Goblin"), { TEXT("Health") });
	Snapshot.AttributeUpdates.Add(FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("Health")));

	TestTrue(TEXT("The model the change was planned for reports the change"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Hero"), TEXT("Health")).Drift == ECrowdyModelDrift::ChangedInCode);
	TestTrue(TEXT("The other model's identically-named attribute is untouched"),
		CrowdyModelLedger::ClassifyAttribute(&Snapshot, TEXT("Goblin"), TEXT("Health")).Drift == ECrowdyModelDrift::None);

	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(ClassifiedAttribute(TEXT("Hero"), TEXT("Health")));
	Defs.Add(ClassifiedAttribute(TEXT("Goblin"), TEXT("Health")));
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAttributeRows(Defs, &Snapshot);

	TestEqual(TEXT("Both attributes become rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		// Both rows are named Health, so the row order is settled by the model each belongs to.
		const FCrowdyModelRow& First = Rows[0].OwningType == TEXT("Goblin") ? Rows[0] : Rows[1];
		const FCrowdyModelRow& Second = Rows[0].OwningType == TEXT("Goblin") ? Rows[1] : Rows[0];
		TestEqual(TEXT("The untouched model's row says nothing"), First.DriftText, FString());
		TestEqual(TEXT("The changed model's row says what changed"), Second.DriftText, FString(TEXT("Changed in code")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSynthesisCodeOnlyEntitiesAppearTest,
	"CrowdySDK.CrowdyStudio.ModelSynthesisCodeOnlyEntitiesAppear", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSynthesisCodeOnlyEntitiesAppearTest::RunTest(const FString& Parameters)
{
	// Every row the browser has ever shown came from a server read, so an entity the project declares and the server
	// has never seen had no row at all and "Not on server yet" could never appear anywhere.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health"), TEXT("Mana") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));
	Snapshot.TypeCreates.Add(TEXT("Hero"));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Health")));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Mana")));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.AutomationCreates.Add(AutomationKey(TEXT("HeroRegen")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList({}, {}, {}, &Snapshot);
	TestEqual(TEXT("The model the project declares is listed even with nothing read from the server"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("It is named after the type"), Models[0].TypeName, FString(TEXT("Hero")));
		TestTrue(TEXT("It is marked as built from the project, so nothing tries to read it from the server"),
			Models[0].bCodeOnly);
		TestEqual(TEXT("Its Source cell says where it came from"), Models[0].ProvenanceText, FString(TEXT("code-not-pushed")));
		TestEqual(TEXT("Its Status cell says what is missing"), Models[0].DriftText, FString(TEXT("Not on server yet")));
		TestEqual(TEXT("Its function count is what its table will show"), Models[0].FunctionCount, 1);
		TestEqual(TEXT("Its automation count is too"), Models[0].AutomationCount, 1);
	}

	const TArray<FCrowdyModelRow> Attributes = CrowdyModelLedger::BuildAttributeRows({}, &Snapshot, TEXT("Hero"));
	const TArray<FCrowdyModelRow> Functions = CrowdyModelLedger::BuildFunctionRows({}, TEXT("Hero"), &Snapshot);
	const TArray<FCrowdyModelRow> Automations = CrowdyModelLedger::BuildAutomationRows({}, {}, TEXT("Hero"), &Snapshot);

	TestEqual(TEXT("Both declared attributes become rows"), Attributes.Num(), 2);
	TestEqual(TEXT("The declared function becomes a row"), Functions.Num(), 1);
	TestEqual(TEXT("The declared automation becomes a row"), Automations.Num(), 1);
	if (Attributes.Num() == 2 && Functions.Num() == 1 && Automations.Num() == 1)
	{
		TestEqual(TEXT("Rows are still in one alphabetical order"), Attributes[0].Name, FString(TEXT("Health")));
		TestEqual(TEXT("An attribute row carries its value type from the class"), Attributes[0].Secondary, FString(TEXT("Number")));
		TestEqual(TEXT("A function row carries its return type"), Functions[0].Secondary, FString(TEXT("Number")));
		TestEqual(TEXT("An automation row says when it runs"), Automations[0].Secondary, FString(TEXT("Runs every 5 minutes")));

		TestTrue(TEXT("Every synthesized row is marked as one"),
			Attributes[0].bCodeOnly && Functions[0].bCodeOnly && Automations[0].bCodeOnly);
		TestEqual(TEXT("An attribute row says what is missing"), Attributes[0].DriftText, FString(TEXT("Not on server yet")));
		TestEqual(TEXT("A function row does too"), Functions[0].DriftText, FString(TEXT("Not on server yet")));
		TestEqual(TEXT("An automation row does too"), Automations[0].DriftText, FString(TEXT("Not on server yet")));
		TestEqual(TEXT("A function row names the effect asset that authors it"),
			Functions[0].CodePath, FString(TEXT("/Game/Effects/FX_Damage.FX_Damage")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSynthesisExcludesReservedPlumbingTest,
	"CrowdySDK.CrowdyStudio.ModelSynthesisExcludesReservedPlumbing", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSynthesisExcludesReservedPlumbingTest::RunTest(const FString& Parameters)
{
	// The SDK provisions a revision attribute and a per-type touch function on EVERY model, and both land in the
	// plan's own desired schema. Building rows from the project side therefore puts back exactly the two rows per
	// model the browser has always excluded: at eighty models that is a hundred and sixty rows of wiring nobody
	// authored, in a list meant to show what a designer wrote.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health"), TEXT("crowdy_rev") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("__crowdy_touch_hero"));
	Snapshot.TypeCreates.Add(TEXT("Hero"));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Health")));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("crowdy_rev")));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("__crowdy_touch_hero")));

	const TArray<FCrowdyModelRow> Attributes = CrowdyModelLedger::BuildAttributeRows({}, &Snapshot, TEXT("Hero"));
	const TArray<FCrowdyModelRow> Functions = CrowdyModelLedger::BuildFunctionRows({}, TEXT("Hero"), &Snapshot);

	TestEqual(TEXT("The reserved revision attribute is not a row"), Attributes.Num(), 1);
	if (Attributes.Num() == 1)
	{
		TestEqual(TEXT("Only the designer attribute survives"), Attributes[0].Name, FString(TEXT("Health")));
	}
	TestEqual(TEXT("The reserved touch function is not a row"), Functions.Num(), 1);
	if (Functions.Num() == 1)
	{
		TestEqual(TEXT("Only the designer function survives"), Functions[0].Name, FString(TEXT("DealDamage")));
	}

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList({}, {}, {}, &Snapshot);
	TestEqual(TEXT("One model is built"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("The touch function does not count as a function"), Models[0].FunctionCount, 1);
		TestEqual(TEXT("The revision attribute does not count as an attribute"), Models[0].AttributeCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSynthesisCodeOnlyModelKnowsItsAttributeCountTest,
	"CrowdySDK.CrowdyStudio.ModelSynthesisCodeOnlyModelKnowsItsAttributeCount", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSynthesisCodeOnlyModelKnowsItsAttributeCountTest::RunTest(const FString& Parameters)
{
	// Attributes are read from the server one model at a time, so a count of "not loaded yet" means "ask for it".
	// There is nothing to ask for on a model the server has never had, so leaving that answer in place would park the
	// model on a loading state forever.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health"), TEXT("Mana"), TEXT("crowdy_rev") });
	DeclareType(Snapshot, TEXT("Goblin"), { TEXT("Health") });
	Snapshot.TypeCreates.Add(TEXT("Hero"));

	TArray<TSharedPtr<FStudioContainerType>> ServerTypes;
	ServerTypes.Add(ClassifiedType(TEXT("Goblin")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(ServerTypes, {}, {}, &Snapshot);
	TestEqual(TEXT("The server's model and the project-only one are both listed"), Models.Num(), 2);
	if (Models.Num() == 2)
	{
		const FCrowdyModelSummary& Goblin = Models[0].TypeName == TEXT("Goblin") ? Models[0] : Models[1];
		const FCrowdyModelSummary& Hero = Models[0].TypeName == TEXT("Goblin") ? Models[1] : Models[0];

		TestEqual(TEXT("A project-only model already knows every attribute it has"), Hero.AttributeCount, 2);
		TestTrue(TEXT("Which is a real count, not a request to go and read one"), Hero.AttributeCount != INDEX_NONE);

		// The contrast matters: a model the server does have still has to be read, and claiming a count for it here
		// would be claiming an answer nobody has asked the server for.
		TestEqual(TEXT("A model the server has still reports not-loaded-yet"), Goblin.AttributeCount, INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSynthesisNeverDuplicatesAServerRowTest,
	"CrowdySDK.CrowdyStudio.ModelSynthesisNeverDuplicatesAServerRow", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSynthesisNeverDuplicatesAServerRowTest::RunTest(const FString& Parameters)
{
	// A snapshot is only as fresh as the last plan. Apply the plan and the server now has what the snapshot still
	// lists as a create, so a build that trusted the snapshot alone would show every applied entity twice.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));
	Snapshot.TypeCreates.Add(TEXT("Hero"));
	Snapshot.AttributeCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Health")));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));
	Snapshot.AutomationCreates.Add(AutomationKey(TEXT("HeroRegen")));

	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(ClassifiedType(TEXT("Hero")));
	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(ClassifiedAttribute(TEXT("Hero"), TEXT("Health")));
	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(ClassifiedFunction(TEXT("Hero"), TEXT("DealDamage")));
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(ClassifiedAutomation(TEXT("Hero"), TEXT("HeroRegen")));

	TestEqual(TEXT("The model is listed once"),
		CrowdyModelLedger::BuildModelList(Types, Functions, Automations, &Snapshot).Num(), 1);
	TestEqual(TEXT("The attribute is listed once"),
		CrowdyModelLedger::BuildAttributeRows(Defs, &Snapshot, TEXT("Hero")).Num(), 1);
	TestEqual(TEXT("The function is listed once"),
		CrowdyModelLedger::BuildFunctionRows(Functions, TEXT("Hero"), &Snapshot).Num(), 1);
	TestEqual(TEXT("The automation is listed once"),
		CrowdyModelLedger::BuildAutomationRows(Automations, {}, TEXT("Hero"), &Snapshot).Num(), 1);

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, Functions, Automations, &Snapshot);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("And counted once"), Models[0].FunctionCount, 1);
		TestEqual(TEXT("Both of them"), Models[0].AutomationCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSynthesisCountsMatchTheTablesTest,
	"CrowdySDK.CrowdyStudio.ModelSynthesisCountsMatchTheTables", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSynthesisCountsMatchTheTablesTest::RunTest(const FString& Parameters)
{
	// The list shows a count and opening the model shows the rows. A count that leaves out the rows the project has
	// not synced yet contradicts the table it opens onto, and there is nothing on screen to say which is right.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeclareFunction(Snapshot, TEXT("Hero"), TEXT("Heal"), TEXT("/Game/Effects/FX_Heal.FX_Heal"));
	DeclareAutomation(Snapshot, TEXT("Hero"), TEXT("HeroRegen"));
	Snapshot.FunctionCreates.Add(ScopedKey(TEXT("Hero"), TEXT("Heal")));
	Snapshot.AutomationCreates.Add(AutomationKey(TEXT("HeroRegen")));

	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(ClassifiedType(TEXT("Hero")));
	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(ClassifiedFunction(TEXT("Hero"), TEXT("DealDamage")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, Functions, {}, &Snapshot);
	const TArray<FCrowdyModelRow> FunctionRows = CrowdyModelLedger::BuildFunctionRows(Functions, TEXT("Hero"), &Snapshot);
	const TArray<FCrowdyModelRow> AutomationRows = CrowdyModelLedger::BuildAutomationRows({}, {}, TEXT("Hero"), &Snapshot);

	TestEqual(TEXT("One model is built"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("The function count is the number of function rows the model opens onto"),
			Models[0].FunctionCount, FunctionRows.Num());
		TestEqual(TEXT("It counts the live function and the one waiting to be synced"), Models[0].FunctionCount, 2);
		TestEqual(TEXT("The automation count is the number of automation rows"),
			Models[0].AutomationCount, AutomationRows.Num());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSourceFilterPartitionsAndComposesTest,
	"CrowdySDK.CrowdyStudio.ModelSourceFilterPartitionsAndComposes", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSourceFilterPartitionsAndComposesTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("HeroSynced"), { TEXT("Health") });
	DeclareType(Snapshot, TEXT("HeroPending"), { TEXT("Health") });
	DeclareType(Snapshot, TEXT("HeroChanged"), { TEXT("Health") });
	Snapshot.TypeCreates.Add(TEXT("HeroPending"));
	Snapshot.TypeUpdates.Add(TEXT("HeroChanged"));
	Snapshot.RecognizedKitTypeNames.Add(TEXT("HeroKit"));

	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(ClassifiedType(TEXT("HeroSynced")));
	Types.Add(ClassifiedType(TEXT("HeroChanged")));
	Types.Add(ClassifiedType(TEXT("HeroKit")));
	Types.Add(ClassifiedType(TEXT("GoblinLegacy")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {}, &Snapshot);
	TestEqual(TEXT("Three server models plus the project-only one"), Models.Num(), 5);

	const TArray<FCrowdyModelSummary> All =
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::All);
	const TArray<FCrowdyModelSummary> InCode =
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::InCode);
	const TArray<FCrowdyModelSummary> OnServer =
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::OnlyOnServer);

	TestEqual(TEXT("All shows everything"), All.Num(), 5);
	TestEqual(TEXT("In code shows the synced, the changed and the not-yet-pushed"), InCode.Num(), 3);
	TestEqual(TEXT("Only on server shows the kit's and the orphan"), OnServer.Num(), 2);
	TestEqual(TEXT("Between them the two settings account for every classified model exactly once"),
		InCode.Num() + OnServer.Num(), All.Num());

	// The search box and the source setting narrow independently, so one must never undo the other.
	const TArray<FCrowdyModelSummary> SearchedInCode =
		CrowdyModelLedger::FilterModels(Models, TEXT("Hero"), ECrowdyModelSourceFilter::InCode);
	TestEqual(TEXT("A query narrows what the source setting already allows"), SearchedInCode.Num(), 3);

	const TArray<FCrowdyModelSummary> SearchedOnServer =
		CrowdyModelLedger::FilterModels(Models, TEXT("Hero"), ECrowdyModelSourceFilter::OnlyOnServer);
	TestEqual(TEXT("And the source setting narrows what the query allows"), SearchedOnServer.Num(), 1);
	if (SearchedOnServer.Num() == 1)
	{
		TestEqual(TEXT("The one left is the kit's, which matches both"), SearchedOnServer[0].TypeName, FString(TEXT("HeroKit")));
	}

	const TArray<FCrowdyModelSummary> NoMatch =
		CrowdyModelLedger::FilterModels(Models, TEXT("Dragon"), ECrowdyModelSourceFilter::InCode);
	TestEqual(TEXT("A query nothing matches still returns nothing"), NoMatch.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSourceFilterKeepsUnclassifiedVisibleTest,
	"CrowdySDK.CrowdyStudio.ModelSourceFilterKeepsUnclassifiedVisible", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSourceFilterKeepsUnclassifiedVisibleTest::RunTest(const FString& Parameters)
{
	// With no plan run, every row is unclassified. A setting that hid those would empty the entire page the first
	// time anyone touched it, which reads as a page that failed to load rather than as a filter working.
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(ClassifiedType(TEXT("Hero")));
	Types.Add(ClassifiedType(TEXT("Goblin")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("All shows both"),
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::All).Num(), 2);
	TestEqual(TEXT("In code still shows both, because nothing is known about either"),
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::InCode).Num(), 2);
	TestEqual(TEXT("Only on server does too, for the same reason"),
		CrowdyModelLedger::FilterModels(Models, FString(), ECrowdyModelSourceFilter::OnlyOnServer).Num(), 2);

	// The same holds for one entity nothing could be checked: it is not evidence for either side.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	Snapshot.UncheckableFunctionKeys.Add(ScopedKey(TEXT("Hero"), TEXT("DealDamage")));

	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(ClassifiedFunction(TEXT("Hero"), TEXT("DealDamage")));
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildFunctionRows(Functions, TEXT("Hero"), &Snapshot);

	TestEqual(TEXT("The uncheckable row survives In code"),
		CrowdyModelLedger::FilterRows(Rows, FString(), ECrowdyModelSourceFilter::InCode).Num(), 1);
	TestEqual(TEXT("And Only on server"),
		CrowdyModelLedger::FilterRows(Rows, FString(), ECrowdyModelSourceFilter::OnlyOnServer).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelVocabularyIsTheFixedSetTest,
	"CrowdySDK.CrowdyStudio.ModelVocabularyIsTheFixedSet", CrowdyModelClassificationTestFlags)

bool FCrowdyModelVocabularyIsTheFixedSetTest::RunTest(const FString& Parameters)
{
	// These exact words are also what the Crowdy Effect asset's own toolbar says. Two surfaces describing one
	// condition in two vocabularies is two conditions as far as the reader is concerned.
	TestEqual(TEXT("Not on server yet"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::NotOnServerYet), FString(TEXT("Not on server yet")));
	TestEqual(TEXT("Changed in code"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::ChangedInCode), FString(TEXT("Changed in code")));
	TestEqual(TEXT("Only on server"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::OnlyOnServer), FString(TEXT("Only on server")));
	TestEqual(TEXT("Needs a fix"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::NeedsAFix), FString(TEXT("Needs a fix")));
	TestEqual(TEXT("Cannot be checked"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::CannotBeChecked), FString(TEXT("Cannot be checked")));
	TestEqual(TEXT("A healthy row says nothing at all"),
		CrowdyModelVocabulary::DriftLabel(ECrowdyModelDrift::None), FString());

	TestEqual(TEXT("code-synced"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::CodeSynced), FString(TEXT("code-synced")));
	TestEqual(TEXT("code-not-pushed"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::CodeNotPushed), FString(TEXT("code-not-pushed")));
	TestEqual(TEXT("code-drifted"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::CodeDrifted), FString(TEXT("code-drifted")));
	TestEqual(TEXT("server-only"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::ServerOnly), FString(TEXT("server-only")));
	TestEqual(TEXT("kit-owned"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::KitOwned), FString(TEXT("kit-owned")));
	TestEqual(TEXT("An unclassified row says nothing at all"),
		CrowdyModelVocabulary::ProvenanceLabel(ECrowdyModelProvenance::Unknown), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSnapshotCaptureFlattensThePlanTest,
	"CrowdySDK.CrowdyStudio.ModelSnapshotCaptureFlattensThePlan", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSnapshotCaptureFlattensThePlanTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyDesiredContainerType> DesiredTypes;
	{
		FCrowdyDesiredContainerType Type;
		Type.TypeName = TEXT("Hero");
		Type.DisplayName = TEXT("Hero");
		Type.OwningClassPath = TEXT("/Game/Models/BP_Hero.BP_Hero_C");
		FCrowdyDesiredPropertyDef Prop;
		Prop.Key = TEXT("Health");
		Prop.ValueType = TEXT("int");
		Type.Props.Add(MoveTemp(Prop));
		DesiredTypes.Add(MoveTemp(Type));
	}

	TArray<FCrowdyGameModelFunctionInput> DesiredFunctions;
	{
		FCrowdyGameModelFunctionInput Function;
		Function.Name = TEXT("DealDamage");
		Function.ContainerTypeName = TEXT("Hero");
		Function.ReturnType = TEXT("int");
		DesiredFunctions.Add(MoveTemp(Function));
	}

	TArray<FCrowdyGameModelAutomationInput> DesiredAutomations;
	{
		FCrowdyGameModelAutomationInput Automation;
		Automation.Name = TEXT("HeroRegen");
		Automation.TargetTypeName = TEXT("Hero");
		Automation.TriggerType = TEXT("event");
		DesiredAutomations.Add(MoveTemp(Automation));
	}

	TArray<FCrowdyGameModelAutomationTriggerInput> DesiredTriggers;
	{
		FCrowdyGameModelAutomationTriggerInput Trigger;
		Trigger.AutomationName = TEXT("HeroRegen");
		Trigger.OnEvent = TEXT("property_changed");
		Trigger.PropertyKey = TEXT("Health");
		DesiredTriggers.Add(MoveTemp(Trigger));
	}

	TArray<FCrowdySchemaAuthorship> FunctionAuthorship;
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_Damage.FX_Damage"), false, false });
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("Stun"), TEXT("/Game/Effects/FX_Broken.FX_Broken"), true, false });
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("Slow"), TEXT("/Game/Effects/FX_One.FX_One"), true, true });
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("Slow"), TEXT("/Game/Effects/FX_Two.FX_Two"), true, true });

	TArray<FCrowdySchemaAuthorship> AutomationAuthorship;
	AutomationAuthorship.Add({ FString(), TEXT("HeroRegen"), TEXT("/Game/Effects/FX_Regen.FX_Regen"), false, false });

	FCrowdySchemaDelta Delta;
	Delta.TypeUpserts.Add({ DesiredTypes[0], /*bIsNew*/ true });
	Delta.PropUpserts.Add({ TEXT("Hero"), DesiredTypes[0].Props[0], /*bIsNew*/ false });
	Delta.FunctionUpserts.Add({ DesiredFunctions[0], /*bIsNew*/ true });
	Delta.AutomationUpserts.Add({ DesiredAutomations[0], /*bIsNew*/ false });

	FCrowdyModelSnapshotPlan Plan;
	Plan.AppId = 4242;
	Plan.DesiredTypes = &DesiredTypes;
	Plan.DesiredFunctions = &DesiredFunctions;
	Plan.DesiredAutomations = &DesiredAutomations;
	Plan.DesiredTriggers = &DesiredTriggers;
	Plan.FunctionAuthorship = &FunctionAuthorship;
	Plan.AutomationAuthorship = &AutomationAuthorship;
	Plan.Delta = &Delta;

	const FCrowdyModelSnapshot Snapshot = CaptureModelSnapshot(Plan);

	TestTrue(TEXT("The capture is pinned to the app it was planned for"), Snapshot.AppId == 4242);
	TestTrue(TEXT("And stamped with when it was taken"), Snapshot.CapturedAt.GetTicks() > 0);

	TestTrue(TEXT("A create and an update are told apart for a model"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.TypeCreates, TEXT("Hero"))
		&& !CrowdyModelSnapshotKeys::Contains(Snapshot.TypeUpdates, TEXT("Hero")));
	TestTrue(TEXT("And for an attribute, keyed by its model"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.AttributeUpdates, ScopedKey(TEXT("Hero"), TEXT("Health"))));
	TestTrue(TEXT("And for a function, keyed by its model"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.FunctionCreates, ScopedKey(TEXT("Hero"), TEXT("DealDamage"))));
	TestTrue(TEXT("And for an automation, keyed by its name alone"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.AutomationUpdates, AutomationKey(TEXT("HeroRegen"))));

	TestTrue(TEXT("A passed-over effect's name cannot be checked"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.UncheckableFunctionKeys, ScopedKey(TEXT("Hero"), TEXT("Stun"))));
	TestTrue(TEXT("A name two effects claim needs a fix"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.NeedsAFixFunctionKeys, ScopedKey(TEXT("Hero"), TEXT("Slow"))));
	TestFalse(TEXT("And is never also filed as merely uncheckable, though the gather marks it skipped too"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.UncheckableFunctionKeys, ScopedKey(TEXT("Hero"), TEXT("Slow"))));

	if (const FCrowdyModelSnapshotFunction* Function = Snapshot.FindFunction(TEXT("Hero"), TEXT("DealDamage")))
	{
		TestEqual(TEXT("A synced function still names the asset that authors it"),
			Function->AssetPath, FString(TEXT("/Game/Effects/FX_Damage.FX_Damage")));
	}
	else
	{
		AddError(TEXT("The desired function did not reach the capture"));
	}

	if (const FCrowdyModelSnapshotAutomation* Automation = Snapshot.FindAutomation(TEXT("HeroRegen")))
	{
		TestEqual(TEXT("An automation carries the event it reacts to, so its row reads the same before and after a sync"),
			Automation->OnEventPropertyKey, FString(TEXT("Health")));
		TestEqual(TEXT("And names its asset"), Automation->AssetPath, FString(TEXT("/Game/Effects/FX_Regen.FX_Regen")));
	}
	else
	{
		AddError(TEXT("The desired automation did not reach the capture"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSnapshotUncheckableExcludesWhatThePlanProducedTest,
	"CrowdySDK.CrowdyStudio.ModelSnapshotUncheckableExcludesWhatThePlanProduced", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSnapshotUncheckableExcludesWhatThePlanProducedTest::RunTest(const FString& Parameters)
{
	// Two effects can name the same function without it being a conflict: one is passed over before the duplicate
	// check ever sees it (an unmigrated or non-compiling asset), so the other syncs normally. The plan DID produce
	// that function, so it is checkable, and letting the broken sibling drag it down to "cannot be checked" would
	// hide a real verdict behind an unrelated asset.
	TArray<FCrowdyGameModelFunctionInput> DesiredFunctions;
	{
		FCrowdyGameModelFunctionInput Function;
		Function.Name = TEXT("DealDamage");
		Function.ContainerTypeName = TEXT("Hero");
		DesiredFunctions.Add(MoveTemp(Function));
	}

	TArray<FCrowdySchemaAuthorship> FunctionAuthorship;
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_Good.FX_Good"), false, false });
	FunctionAuthorship.Add({ TEXT("Hero"), TEXT("DealDamage"), TEXT("/Game/Effects/FX_Unmigrated.FX_Unmigrated"), true, false });

	FCrowdyModelSnapshotPlan Plan;
	Plan.AppId = 7;
	Plan.DesiredFunctions = &DesiredFunctions;
	Plan.FunctionAuthorship = &FunctionAuthorship;

	const FCrowdyModelSnapshot Snapshot = CaptureModelSnapshot(Plan);

	TestFalse(TEXT("A name the plan actually produced is never filed as uncheckable"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.UncheckableFunctionKeys, ScopedKey(TEXT("Hero"), TEXT("DealDamage"))));
	TestTrue(TEXT("So it classifies on the code rules, in sync with nothing planned against it"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("DealDamage")).Provenance
			== ECrowdyModelProvenance::CodeSynced);

	// And the exclusion has to be about THIS name, or it would just be switched off: a name nothing produced still
	// has to come back uncheckable.
	TArray<FCrowdySchemaAuthorship> OnlyBroken;
	OnlyBroken.Add({ TEXT("Hero"), TEXT("Stun"), TEXT("/Game/Effects/FX_Unmigrated.FX_Unmigrated"), true, false });
	Plan.FunctionAuthorship = &OnlyBroken;
	const FCrowdyModelSnapshot WithoutTheGoodOne = CaptureModelSnapshot(Plan);

	TestTrue(TEXT("A name only a passed-over effect claims is still uncheckable"),
		CrowdyModelSnapshotKeys::Contains(WithoutTheGoodOne.UncheckableFunctionKeys, ScopedKey(TEXT("Hero"), TEXT("Stun"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationBrokenAutomationAuthorIsNotServerOnlyTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationBrokenAutomationAuthorIsNotServerOnly", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationBrokenAutomationAuthorIsNotServerOnlyTest::RunTest(const FString& Parameters)
{
	// Driven through the real selection rather than a hand-written snapshot, because the gap this covers was in the
	// selection: an effect that runs itself and then stops compiling produced no attribution at all, so its live
	// automation arrived at the browser looking like one no asset in the project claims. The plan disagrees with that
	// reading: it records the same name as recognized before any skip, precisely so the prune never offers it.
	const TArray<FCrowdyEffectPlanRecord> Records =
		{ BrokenEffectRecord(TEXT("/Game/FX/HeroRegen.HeroRegen"), TEXT("Hero"), TEXT("hero_regen")) };

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdyGameModelAutomationTriggerInput> Triggers;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelAutomationInput> Automations =
		FCrowdySchemaSync::SelectDesiredAutomations(Records, Warnings, RecognizedNames, Triggers, Authorship);

	TestEqual(TEXT("Nothing is planned for an effect that will not compile"), Automations.Num(), 0);
	TestTrue(TEXT("Its name is still protected from the prune"), RecognizedNames.Contains(TEXT("hero_regen")));
	if (TestEqual(TEXT("And it is still attributed to the asset that claims it"), Authorship.Num(), 1))
	{
		TestTrue(TEXT("Marked as one the plan could not act on"), Authorship[0].bSkipped);
		TestEqual(TEXT("Named by the function its automation defaults to"),
			Authorship[0].Name, FString(TEXT("hero_regen")));
		TestTrue(TEXT("An automation name is app-wide, so it carries no scope"), Authorship[0].Scope.IsEmpty());
		TestEqual(TEXT("Pointing at the asset to fix"),
			Authorship[0].AssetPath, FString(TEXT("/Game/FX/HeroRegen.HeroRegen")));
	}

	FCrowdyModelSnapshotPlan Plan;
	Plan.AppId = 11;
	Plan.AutomationAuthorship = &Authorship;
	const FCrowdyModelSnapshot Snapshot = CaptureModelSnapshot(Plan);

	const FCrowdyModelClassification Verdict = CrowdyModelLedger::ClassifyAutomation(&Snapshot, TEXT("hero_regen"));
	TestFalse(TEXT("The live automation is never reported as an orphan the prune would act on"),
		Verdict.Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("Nothing can be said about how it compares to code, and it says exactly that"),
		Verdict.Drift == ECrowdyModelDrift::CannotBeChecked);
	TestEqual(TEXT("The asset to fix is nameable from the row"),
		Verdict.CodePath, FString(TEXT("/Game/FX/HeroRegen.HeroRegen")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationAuthorWithNoModelIsNotServerOnlyTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationAuthorWithNoModelIsNotServerOnly", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationAuthorWithNoModelIsNotServerOnlyTest::RunTest(const FString& Parameters)
{
	// An effect is scoped by the container class it targets, so an effect whose class was deleted, untagged or left
	// unloaded resolves no model at all and is filed under an empty scope. The server's copy of its function lives on
	// a real model, so the two keys can never meet, and the row fell through to the deletion-candidate reading for a
	// function whose author is sitting broken in the project. Driven through the real selection and capture, since an
	// empty scope is something only they produce.
	const TArray<FCrowdyEffectPlanRecord> Records =
		{ BrokenEffectRecord(TEXT("/Game/FX/Hit.Hit"), FString(), TEXT("take_damage")) };

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	FCrowdySchemaSync::SelectDesiredFunctions(Records, Warnings, RecognizedNames, Authorship);

	if (TestEqual(TEXT("The broken effect is attributed"), Authorship.Num(), 1))
	{
		TestTrue(TEXT("With no model to scope it by, because its class did not resolve"), Authorship[0].Scope.IsEmpty());
	}

	FCrowdyModelSnapshotPlan Plan;
	Plan.AppId = 12;
	Plan.FunctionAuthorship = &Authorship;
	const FCrowdyModelSnapshot Snapshot = CaptureModelSnapshot(Plan);

	TestTrue(TEXT("The capture files it with no scope, which no model's key can match"),
		CrowdyModelSnapshotKeys::Contains(Snapshot.UncheckableFunctionKeys, ScopedKey(FString(), TEXT("take_damage"))));

	const FCrowdyModelClassification Verdict =
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Combatant"), TEXT("take_damage"));

	TestFalse(TEXT("The live function is never reported as an orphan the prune would act on"),
		Verdict.Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("It says that nothing could be checked about it"),
		Verdict.Drift == ECrowdyModelDrift::CannotBeChecked);
	TestEqual(TEXT("And still names the asset to fix"), Verdict.CodePath, FString(TEXT("/Game/FX/Hit.Hit")));

	// The fallback answers for the name, not for everything: a function nothing claims still reads as server-only.
	TestTrue(TEXT("An unrelated server function is untouched by it"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Combatant"), TEXT("legacy_heal")).Provenance
			== ECrowdyModelProvenance::ServerOnly);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelClassificationReboundFunctionIsNotServerOnlyTest,
	"CrowdySDK.CrowdyStudio.ModelClassificationReboundFunctionIsNotServerOnly", CrowdyModelClassificationTestFlags)

bool FCrowdyModelClassificationReboundFunctionIsNotServerOnlyTest::RunTest(const FString& Parameters)
{
	// An effect repointed at another model. The plan matches the server function that still bears the name and plans an
	// ordinary update that rebinds it, so the row on the model it is moving off is code-authored and drifting. Reading
	// it as server-only would have the page offer up for deletion the very entity the plan is about to move.
	FCrowdyModelSnapshot Snapshot;
	DeclareType(Snapshot, TEXT("Goblin"), { TEXT("Health") });
	DeclareFunction(Snapshot, TEXT("Goblin"), TEXT("take_damage"), TEXT("/Game/FX/Hit.Hit"));
	Snapshot.FunctionUpdates.Add(ScopedKey(TEXT("Goblin"), TEXT("take_damage")));

	const FCrowdyModelClassification OldModel =
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Hero"), TEXT("take_damage"));

	TestFalse(TEXT("The function being moved is not an orphan on the model it is leaving"),
		OldModel.Provenance == ECrowdyModelProvenance::ServerOnly);
	TestTrue(TEXT("It came from code and the code has moved it"),
		OldModel.Provenance == ECrowdyModelProvenance::CodeDrifted
		&& OldModel.Drift == ECrowdyModelDrift::ChangedInCode);
	TestEqual(TEXT("Naming the effect that moved it"), OldModel.CodePath, FString(TEXT("/Game/FX/Hit.Hit")));

	// The name alone is not enough. With nothing planned for the declaration it leads to, this is a stale server copy
	// sitting beside a function code declares elsewhere, and claiming a change is coming for it would be an invention.
	FCrowdyModelSnapshot Settled;
	DeclareType(Settled, TEXT("Goblin"), { TEXT("Health") });
	DeclareFunction(Settled, TEXT("Goblin"), TEXT("take_damage"), TEXT("/Game/FX/Hit.Hit"));
	TestTrue(TEXT("A name with no planned update says nothing new"),
		CrowdyModelLedger::ClassifyFunction(&Settled, TEXT("Hero"), TEXT("take_damage")).Provenance
			== ECrowdyModelProvenance::ServerOnly);

	// Nor is an ambiguous one: with two models declaring the name, nothing can say which of them this row becomes,
	// which is the same reason the diff's own rebind fallback requires a single answer.
	FCrowdyModelSnapshot Ambiguous;
	DeclareType(Ambiguous, TEXT("Goblin"), { TEXT("Health") });
	DeclareType(Ambiguous, TEXT("Orc"), { TEXT("Health") });
	DeclareFunction(Ambiguous, TEXT("Goblin"), TEXT("take_damage"), TEXT("/Game/FX/Hit.Hit"));
	DeclareFunction(Ambiguous, TEXT("Orc"), TEXT("take_damage"), TEXT("/Game/FX/Bash.Bash"));
	Ambiguous.FunctionUpdates.Add(ScopedKey(TEXT("Goblin"), TEXT("take_damage")));
	Ambiguous.FunctionUpdates.Add(ScopedKey(TEXT("Orc"), TEXT("take_damage")));
	TestTrue(TEXT("Two declarations of one name leave the old row unclaimed"),
		CrowdyModelLedger::ClassifyFunction(&Ambiguous, TEXT("Hero"), TEXT("take_damage")).Provenance
			== ECrowdyModelProvenance::ServerOnly);

	// And the model the function is being moved ONTO still classifies from its own scoped key.
	TestTrue(TEXT("The new model reports the planned change on its own row"),
		CrowdyModelLedger::ClassifyFunction(&Snapshot, TEXT("Goblin"), TEXT("take_damage")).Drift
			== ECrowdyModelDrift::ChangedInCode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelSectionRowIdentityIsPerEntityTest,
	"CrowdySDK.CrowdyStudio.ModelSectionRowIdentityIsPerEntity", CrowdyModelClassificationTestFlags)

bool FCrowdyModelSectionRowIdentityIsPerEntityTest::RunTest(const FString& Parameters)
{
	// What carries a highlight across a table's rows being replaced. Two rows are the same row to a reader when they
	// name the same thing on the same model, and never otherwise: an attribute and a function may share a name, the
	// same name on two models is two entities, and these are server keys, so case is part of the name.
	FCrowdyModelRow Function;
	Function.Kind = ECrowdyModelRowKind::Function;
	Function.Name = TEXT("take_damage");
	Function.OwningType = TEXT("Hero");

	FCrowdyModelRow Rebuilt = Function;
	Rebuilt.ProvenanceText = TEXT("code-drifted");
	Rebuilt.DriftText = TEXT("Changed in code");

	TestTrue(TEXT("The same entity rebuilt with a new verdict is still the same row"),
		SCrowdyModelSectionTable::IsSameEntity(Function, Rebuilt));

	FCrowdyModelRow OtherModel = Function;
	OtherModel.OwningType = TEXT("Goblin");
	TestFalse(TEXT("The same name on another model is another entity"),
		SCrowdyModelSectionTable::IsSameEntity(Function, OtherModel));

	FCrowdyModelRow OtherKind = Function;
	OtherKind.Kind = ECrowdyModelRowKind::Attribute;
	TestFalse(TEXT("An attribute is not the function that shares its name"),
		SCrowdyModelSectionTable::IsSameEntity(Function, OtherKind));

	FCrowdyModelRow OtherCase = Function;
	OtherCase.Name = TEXT("Take_Damage");
	TestFalse(TEXT("A differently-cased server key is a different entity"),
		SCrowdyModelSectionTable::IsSameEntity(Function, OtherCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProvenanceGlyphScalesWithItsExtentTest,
	"CrowdySDK.CrowdyStudio.ProvenanceGlyphScalesWithItsExtent", CrowdyModelClassificationTestFlags)

bool FCrowdyProvenanceGlyphScalesWithItsExtentTest::RunTest(const FString& /*Parameters*/)
{
	// The painted shapes are fractions of a fixed base unit multiplied by this. Scale it against the DEFAULT size
	// instead and the ratio comes out at one for every table row, so the shape keeps its original size inside a box
	// that grew around it: the glyph looks unchanged however large the default is set, and nothing reports an error.
	// That went unnoticed across four separate size changes, which is the whole reason this is worth a test.
	const float Default = SCrowdyProvenanceGlyph::GlyphExtent();
	TestTrue(TEXT("A glyph at the default extent is drawn larger than the base unit it is proportioned from"),
		SCrowdyProvenanceGlyph::ShapeScale(Default) > 1.0f);

	// Twice the room draws twice the shape. This is the property a gutter and a legend sharing one drawing rely on.
	TestTrue(TEXT("Doubling the extent doubles the shape"),
		FMath::IsNearlyEqual(SCrowdyProvenanceGlyph::ShapeScale(2.0f * Default),
			2.0f * SCrowdyProvenanceGlyph::ShapeScale(Default)));

	TestTrue(TEXT("A larger extent always draws a larger shape"),
		SCrowdyProvenanceGlyph::ShapeScale(Default + 6.0f) > SCrowdyProvenanceGlyph::ShapeScale(Default));

	// Zero means "no size given", which every table row passes, and it must land on the default rather than on
	// nothing at all.
	TestTrue(TEXT("An unset extent is drawn at the default size"),
		FMath::IsNearlyEqual(SCrowdyProvenanceGlyph::ShapeScale(0.0f), SCrowdyProvenanceGlyph::ShapeScale(Default)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelVocabularyListsEveryStateTest,
	"CrowdySDK.CrowdyStudio.VocabularyListsEveryState", CrowdyModelClassificationTestFlags)

bool FCrowdyModelVocabularyListsEveryStateTest::RunTest(const FString& /*Parameters*/)
{
	// The legend is built by walking these two lists, so a state missing from one of them is a mark the page paints
	// and never explains. Nothing else notices: the row still renders, the glyph still draws, and the only symptom is
	// a reader who cannot look the shape up. Counting against the enum is what makes adding a state to the enum and
	// forgetting the list a failure here rather than a gap someone finds on the page.
	const TArray<ECrowdyModelProvenance>& Provenances = CrowdyModelVocabulary::AllProvenances();
	TestEqual(TEXT("Every provenance the ledger can assign is listed for the legend"),
		Provenances.Num(), static_cast<int32>(ECrowdyModelProvenance::KitOwned) + 1);

	for (int32 Value = 0; Value <= static_cast<int32>(ECrowdyModelProvenance::KitOwned); ++Value)
	{
		const ECrowdyModelProvenance Provenance = static_cast<ECrowdyModelProvenance>(Value);
		TestTrue(FString::Printf(TEXT("Provenance %d is listed"), Value), Provenances.Contains(Provenance));
		TestFalse(FString::Printf(TEXT("Provenance %d has a meaning to show"), Value),
			CrowdyModelVocabulary::ProvenanceMeaning(Provenance).IsEmpty());
	}

	const TArray<ECrowdyModelDrift>& Drifts = CrowdyModelVocabulary::AllDrifts();
	TestEqual(TEXT("Every drift the ledger can assign is listed for the legend"),
		Drifts.Num(), static_cast<int32>(ECrowdyModelDrift::CannotBeChecked) + 1);

	for (int32 Value = 0; Value <= static_cast<int32>(ECrowdyModelDrift::CannotBeChecked); ++Value)
	{
		const ECrowdyModelDrift Drift = static_cast<ECrowdyModelDrift>(Value);
		TestTrue(FString::Printf(TEXT("Drift %d is listed"), Value), Drifts.Contains(Drift));
		TestFalse(FString::Printf(TEXT("Drift %d has a meaning to show"), Value),
			CrowdyModelVocabulary::DriftMeaning(Drift).IsEmpty());
	}

	// The two states that show nothing are exactly the ones a reader cannot look up from the page, so the legend has
	// to carry them even though neither has a word or a shape of its own.
	TestTrue(TEXT("The unmarked provenance is explained"),
		Provenances.Contains(ECrowdyModelProvenance::Unknown));
	TestTrue(TEXT("The blank status is explained"), Drifts.Contains(ECrowdyModelDrift::None));
	return true;
}

#endif
