// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdyApplySelection.h"
#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelSnapshot.h"
#include "GameModel/CrowdyModelVocabulary.h"
#include "Model/CrowdyStudioTypes.h"

// An attribute is addressed by a lowercased server key and shown under the name it was authored with. These two
// facts have to stay apart: the name reaching a comparison, a lookup or an upsert payload is a wrong-cased key,
// and FString comparison folds case in Unreal, so such a key matches in some places and not in others rather than
// failing outright. Every test here either pins what a reader sees or pins that the key underneath is untouched.

namespace
{
	constexpr EAutomationTestFlags CrowdyDisplayNameTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Named apart from the builders in the other test files of this module: the unity build merges its translation
	// units, so two anonymous-namespace helpers sharing a name redefine each other.

	TSharedPtr<FStudioPropertyDef> DisplayNameAttributeDef(
		const FString& TypeName, const FString& Key, const FString& ValueType = TEXT("int"))
	{
		TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
		Def->ContainerTypeName = TypeName;
		Def->Key = Key;
		Def->ValueType = ValueType;
		return Def;
	}

	// One model declared in the project, with each attribute's server key paired with the spelling it was authored
	// under. An empty authored spelling is how an attribute reflected without one arrives.
	void DisplayNameDeclareType(
		FCrowdyModelSnapshot& Snapshot,
		const FString& TypeName,
		const TArray<TPair<FString, FString>>& KeysAndAuthoredNames)
	{
		FCrowdyModelSnapshotType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.OwningClassPath = TEXT("/Game/Models/BP_Farm.BP_Farm_C");
		for (const TPair<FString, FString>& Entry : KeysAndAuthoredNames)
		{
			FCrowdyModelSnapshotAttribute Attribute;
			Attribute.Key = Entry.Key;
			Attribute.ValueType = TEXT("int");
			Attribute.AuthoredName = Entry.Value;
			Type.Attributes.Add(MoveTemp(Attribute));
		}
		Snapshot.Types.Add(MoveTemp(Type));
	}

	const FCrowdyModelRow* DisplayNameFindRow(const TArray<FCrowdyModelRow>& Rows, const FString& Key)
	{
		return Rows.FindByPredicate([&Key](const FCrowdyModelRow& Row)
		{
			return CrowdyModelSnapshotKeys::KeysMatch(Row.Name, Key);
		});
	}
}

// The reconstruction that stands in when nothing in the project declares an attribute. It is the only thing a
// server-only attribute has, so it has to answer for every shape a server key can take rather than only the tidy
// ones.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeDisplayNameFromKeyTest,
	"CrowdySDK.CrowdyStudio.AttributeDisplayNameFromKey", CrowdyDisplayNameTestFlags)

bool FCrowdyAttributeDisplayNameFromKeyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an underscore is a word break, not a character"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("crop_stage")), FString(TEXT("CropStage")));

	TestEqual(TEXT("a single-word key still gets its capital"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("hp")), FString(TEXT("Hp")));

	TestEqual(TEXT("three words join into one name"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("last_harvest_time")),
		FString(TEXT("LastHarvestTime")));

	// A key that already carries capitals is left alone rather than re-cased: whatever it is, it is already more
	// readable than anything this could make of it.
	TestEqual(TEXT("a key that is already PascalCase is unchanged"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("CropStage")), FString(TEXT("CropStage")));

	TestEqual(TEXT("an empty key yields an empty name rather than anything invented"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(FString()), FString());

	TestEqual(TEXT("a leading underscore adds no word"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("_hp")), FString(TEXT("Hp")));

	TestEqual(TEXT("a trailing underscore adds no word"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("hp_")), FString(TEXT("Hp")));

	TestEqual(TEXT("doubled underscores are one word break"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("crop__stage")), FString(TEXT("CropStage")));

	TestEqual(TEXT("a key of nothing but underscores yields an empty name"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("___")), FString());

	TestEqual(TEXT("a single character is capitalized without running off the end"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("h")), FString(TEXT("H")));

	return true;
}

// The authored spelling is the whole point: reconstructing from the key can only ever guess at capitalization,
// and for a name the key ran together it guesses wrong.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeDisplayNamePrefersAuthoredTest,
	"CrowdySDK.CrowdyStudio.AttributeDisplayNamePrefersAuthored", CrowdyDisplayNameTestFlags)

bool FCrowdyAttributeDisplayNamePrefersAuthoredTest::RunTest(const FString& Parameters)
{
	// The key carries no underscore, so reconstruction can only produce "Hpregen". Anything that answers HPRegen
	// answered from the authored name and from nothing else.
	TestEqual(TEXT("reconstruction alone cannot recover the authored capitalization"),
		CrowdyModelVocabulary::AttributeDisplayNameFromKey(TEXT("hpregen")), FString(TEXT("Hpregen")));

	TestEqual(TEXT("the authored spelling wins over the reconstruction"),
		CrowdyModelVocabulary::AttributeDisplayName(TEXT("hpregen"), TEXT("HPRegen")), FString(TEXT("HPRegen")));

	// A CrowdyKey override pins a key that has nothing to do with the property name, which is the case where the
	// two can differ by more than case.
	TestEqual(TEXT("an overridden key still shows the property it was authored on"),
		CrowdyModelVocabulary::AttributeDisplayName(TEXT("secret"), TEXT("SecretScore")),
		FString(TEXT("SecretScore")));

	TestEqual(TEXT("no authored spelling falls back to the reconstruction"),
		CrowdyModelVocabulary::AttributeDisplayName(TEXT("crop_stage"), FString()), FString(TEXT("CropStage")));

	TestEqual(TEXT("a whitespace-only authored spelling is no spelling at all"),
		CrowdyModelVocabulary::AttributeDisplayName(TEXT("crop_stage"), TEXT("   ")), FString(TEXT("CropStage")));

	return true;
}

// A plan capture is how the authored spelling travels from the class that declares an attribute to the row that
// shows it. A default FName stringifies as "None", which would put that word in front of a reader as if somebody
// had authored it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeDisplayNameSurvivesCaptureTest,
	"CrowdySDK.CrowdyStudio.AttributeDisplayNameSurvivesCapture", CrowdyDisplayNameTestFlags)

bool FCrowdyAttributeDisplayNameSurvivesCaptureTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Desired;
	Desired.TypeName = TEXT("Farm");
	Desired.DisplayName = TEXT("Farm");

	FCrowdyDesiredPropertyDef Authored;
	Authored.PropertyName = FName(TEXT("CropStage"));
	Authored.Key = TEXT("crop_stage");
	Authored.ValueType = TEXT("int");
	Desired.Props.Add(Authored);

	FCrowdyDesiredPropertyDef Nameless;
	Nameless.Key = TEXT("crowdy_rev");
	Nameless.ValueType = TEXT("int");
	Desired.Props.Add(Nameless);

	const TArray<FCrowdyDesiredContainerType> Types = { Desired };

	FCrowdyModelSnapshotPlan Plan;
	Plan.AppId = 42;
	Plan.DesiredTypes = &Types;

	const FCrowdyModelSnapshot Snapshot = CaptureModelSnapshot(Plan);

	const FCrowdyModelSnapshotType* Captured = Snapshot.FindType(TEXT("Farm"));
	if (!Captured)
	{
		AddError(TEXT("the captured plan lost the declared model"));
		return true;
	}

	if (const FCrowdyModelSnapshotAttribute* Attribute = Captured->FindAttribute(TEXT("crop_stage")))
	{
		TestEqual(TEXT("the key is untouched by the capture"), Attribute->Key, FString(TEXT("crop_stage")));
		TestEqual(TEXT("the authored spelling survives the capture"),
			Attribute->AuthoredName, FString(TEXT("CropStage")));
	}
	else
	{
		AddError(TEXT("the captured model lost its attribute"));
	}

	if (const FCrowdyModelSnapshotAttribute* Attribute = Captured->FindAttribute(TEXT("crowdy_rev")))
	{
		TestTrue(TEXT("an attribute with no authored spelling captures none, rather than the word None"),
			Attribute->AuthoredName.IsEmpty());
	}
	else
	{
		AddError(TEXT("the captured model lost its reserved attribute"));
	}

	return true;
}

// The row is where the two forms sit side by side, so it is where they are most easily confused for each other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeRowShowsTheAuthoredNameTest,
	"CrowdySDK.CrowdyStudio.AttributeRowShowsTheAuthoredName", CrowdyDisplayNameTestFlags)

bool FCrowdyAttributeRowShowsTheAuthoredNameTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DisplayNameDeclareType(Snapshot, TEXT("Farm"), {
		{ TEXT("crop_stage"), TEXT("CropStage") },
		{ TEXT("hpregen"), TEXT("HPRegen") }
	});

	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(DisplayNameAttributeDef(TEXT("Farm"), TEXT("crop_stage")));
	Defs.Add(DisplayNameAttributeDef(TEXT("Farm"), TEXT("hpregen")));
	// On the server and declared nowhere in the project, so there is no authored spelling to find.
	Defs.Add(DisplayNameAttributeDef(TEXT("Farm"), TEXT("legacy_yield")));

	const TArray<FCrowdyModelRow> Rows =
		CrowdyModelLedger::BuildAttributeRows(Defs, &Snapshot, TEXT("Farm"));

	TestEqual(TEXT("every attribute becomes a row"), Rows.Num(), 3);

	if (const FCrowdyModelRow* Row = DisplayNameFindRow(Rows, TEXT("crop_stage")))
	{
		TestEqual(TEXT("the row shows the authored spelling"), Row->DisplayLabel(), FString(TEXT("CropStage")));

		// THE INVARIANT. The display string is a label; these two are what address the attribute on the server.
		TestEqual(TEXT("the row's identity is still the lowercase server key"),
			Row->Name, FString(TEXT("crop_stage")));
		TestEqual(TEXT("the sorted and searched form is still the server key"),
			Row->Primary, FString(TEXT("crop_stage")));
		TestTrue(TEXT("the identity still matches the server key case-sensitively"),
			CrowdyModelSnapshotKeys::KeysMatch(Row->Name, TEXT("crop_stage")));
		TestFalse(TEXT("the shown name is not the identity"),
			CrowdyModelSnapshotKeys::KeysMatch(Row->Name, Row->DisplayLabel()));
	}
	else
	{
		AddError(TEXT("the crop_stage row is missing"));
	}

	if (const FCrowdyModelRow* Row = DisplayNameFindRow(Rows, TEXT("hpregen")))
	{
		// Reconstruction would say Hpregen here, so this asserts the authored name reached the row rather than
		// the two routes happening to agree.
		TestEqual(TEXT("a key with no underscore still shows its authored capitalization"),
			Row->DisplayLabel(), FString(TEXT("HPRegen")));
	}
	else
	{
		AddError(TEXT("the hpregen row is missing"));
	}

	if (const FCrowdyModelRow* Row = DisplayNameFindRow(Rows, TEXT("legacy_yield")))
	{
		TestEqual(TEXT("an attribute only the server has falls back to the reconstruction"),
			Row->DisplayLabel(), FString(TEXT("LegacyYield")));
		TestEqual(TEXT("its identity is still the server key"), Row->Name, FString(TEXT("legacy_yield")));
	}
	else
	{
		AddError(TEXT("the legacy_yield row is missing"));
	}

	return true;
}

// A row with no better name to show must keep showing the one it has. Function, automation and live rows share
// the struct with attributes, and a display field left empty on them cannot be allowed to blank their name cell.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowWithoutADisplayKeepsItsNameTest,
	"CrowdySDK.CrowdyStudio.RowWithoutADisplayKeepsItsName", CrowdyDisplayNameTestFlags)

bool FCrowdyRowWithoutADisplayKeepsItsNameTest::RunTest(const FString& Parameters)
{
	FCrowdyModelRow Row;
	Row.Kind = ECrowdyModelRowKind::Function;
	Row.Name = TEXT("goblin_tick");
	Row.Primary = TEXT("goblin_tick");

	TestEqual(TEXT("a row carrying no display string shows its primary label"),
		Row.DisplayLabel(), FString(TEXT("goblin_tick")));

	Row.Display = TEXT("GoblinTick");
	TestEqual(TEXT("a row carrying one shows that instead"), Row.DisplayLabel(), FString(TEXT("GoblinTick")));

	return true;
}

// Filtering is defined over the server key, and a reader who knows an attribute only by the name on screen has to
// be able to type that too. Both have to find the row; neither may replace the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAttributeFilterFindsBothFormsTest,
	"CrowdySDK.CrowdyStudio.AttributeFilterFindsBothForms", CrowdyDisplayNameTestFlags)

bool FCrowdyAttributeFilterFindsBothFormsTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DisplayNameDeclareType(Snapshot, TEXT("Farm"), { { TEXT("crop_stage"), TEXT("CropStage") } });

	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(DisplayNameAttributeDef(TEXT("Farm"), TEXT("crop_stage")));

	const TArray<FCrowdyModelRow> Rows =
		CrowdyModelLedger::BuildAttributeRows(Defs, &Snapshot, TEXT("Farm"));

	TestEqual(TEXT("the server key still finds the row"),
		CrowdyModelLedger::FilterRows(Rows, TEXT("crop_stage")).Num(), 1);
	TestEqual(TEXT("the shown name finds it too"),
		CrowdyModelLedger::FilterRows(Rows, TEXT("CropStage")).Num(), 1);
	TestEqual(TEXT("a word in either form finds it"),
		CrowdyModelLedger::FilterRows(Rows, TEXT("stage")).Num(), 1);
	TestEqual(TEXT("a query matching neither finds nothing"),
		CrowdyModelLedger::FilterRows(Rows, TEXT("harvest")).Num(), 0);

	return true;
}

// The delete sheet names what the reader ticked, and the mutation it issues names the server key. A mark carries
// both, and only one of them is compared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteMarkKeepsTheKeyTest,
	"CrowdySDK.CrowdyStudio.DeleteMarkKeepsTheKey", CrowdyDisplayNameTestFlags)

bool FCrowdyDeleteMarkKeepsTheKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyModelRow Row;
	Row.Kind = ECrowdyModelRowKind::Attribute;
	Row.Name = TEXT("crop_stage");
	Row.OwningType = TEXT("Farm");
	Row.Primary = TEXT("crop_stage");
	Row.Display = TEXT("CropStage");

	const FCrowdyDeleteMark Mark = CrowdyGameModelDelete::MarkFromRow(Row);

	TestEqual(TEXT("the mark deletes the server key"), Mark.Name, FString(TEXT("crop_stage")));
	TestEqual(TEXT("the mark is scoped by the model"), Mark.OwningType, FString(TEXT("Farm")));
	TestEqual(TEXT("the sheet names it the way the row did"), Mark.Display, FString(TEXT("CropStage")));

	// Two marks for one attribute that merely render differently are one entity, so a row marked before a plan
	// landed still reads as marked afterwards.
	const FCrowdyDeleteMark Reconstructed =
		CrowdyGameModelDelete::MarkAttribute(TEXT("Farm"), TEXT("crop_stage"), TEXT("CropStage"));
	const FCrowdyDeleteMark Raw =
		CrowdyGameModelDelete::MarkAttribute(TEXT("Farm"), TEXT("crop_stage"), TEXT("crop_stage"));
	TestTrue(TEXT("the shown name is not part of a mark's identity"), Reconstructed == Raw);

	// A key differing only in case is a different attribute, which is the comparison a display string must never
	// be allowed to stand in for.
	const FCrowdyDeleteMark OtherCase =
		CrowdyGameModelDelete::MarkAttribute(TEXT("Farm"), TEXT("Crop_Stage"), TEXT("CropStage"));
	TestTrue(TEXT("two keys differing only in case are two attributes"), Reconstructed != OtherCase);

	return true;
}

// The apply sheet is the last thing read before a write, so it names attributes the way the browser does while
// the unit it sends stays keyed by the server key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitShowsTheAuthoredNameTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitShowsTheAuthoredName", CrowdyDisplayNameTestFlags)

bool FCrowdyApplyUnitShowsTheAuthoredNameTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdySchemaPropUpsert> Props;

	FCrowdySchemaPropUpsert Authored;
	Authored.ContainerTypeName = TEXT("Farm");
	Authored.Prop.PropertyName = FName(TEXT("CropStage"));
	Authored.Prop.Key = TEXT("crop_stage");
	Authored.Prop.ValueType = TEXT("int");
	Authored.bIsNew = true;
	Props.Add(Authored);

	FCrowdySchemaPropUpsert Nameless;
	Nameless.ContainerTypeName = TEXT("Farm");
	Nameless.Prop.Key = TEXT("legacy_yield");
	Nameless.Prop.ValueType = TEXT("int");
	Props.Add(Nameless);

	FCrowdyApplyPlanInput Input;
	Input.AppId = 42;
	Input.Props = &Props;

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Input);
	TestEqual(TEXT("both attributes become units"), Units.Num(), 2);
	if (Units.Num() != 2)
	{
		return true;
	}

	TestEqual(TEXT("the unit is keyed by the server key"), Units[0].Name, FString(TEXT("crop_stage")));
	TestEqual(TEXT("the line names the attribute as it was authored"),
		Units[0].Display, FString(TEXT("CropStage on Farm")));

	TestEqual(TEXT("an attribute with no authored spelling is reconstructed from its key"),
		Units[1].Display, FString(TEXT("LegacyYield on Farm")));
	TestEqual(TEXT("and is still keyed by the server key"), Units[1].Name, FString(TEXT("legacy_yield")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
