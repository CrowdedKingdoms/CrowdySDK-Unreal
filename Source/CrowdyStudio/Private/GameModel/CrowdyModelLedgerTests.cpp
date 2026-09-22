// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelVocabulary.h"
#include "Model/CrowdyStudioTypes.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelLedgerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FStudioPropertyDef> MakeAttributeDef(
		const FString& Key, const FString& TypeName, const FString& ValueType = TEXT("string"), const FString& Description = FString())
	{
		TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
		Def->Key = Key;
		Def->ContainerTypeName = TypeName;
		Def->ValueType = ValueType;
		Def->Description = Description;
		return Def;
	}

	TSharedPtr<FStudioFunction> MakeFunction(const FString& Name, const FString& TypeName, const FString& ReturnType = FString())
	{
		TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
		Function->Name = Name;
		Function->ContainerTypeName = TypeName;
		Function->ReturnType = ReturnType;
		return Function;
	}

	TSharedPtr<FStudioContainerType> MakeType(
		const FString& TypeName, const FString& DisplayName = FString(), const FString& Description = FString())
	{
		TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
		Type->TypeName = TypeName;
		Type->DisplayName = DisplayName;
		Type->Description = Description;
		return Type;
	}

	TSharedPtr<FStudioAutomation> MakeAutomation(
		const FString& Name,
		const FString& TargetTypeName,
		const FString& TriggerType = TEXT("schedule"),
		const FString& ScheduleKind = TEXT("interval"),
		int32 IntervalMs = 300000)
	{
		TSharedPtr<FStudioAutomation> Automation = MakeShared<FStudioAutomation>();
		Automation->Name = Name;
		Automation->TargetTypeName = TargetTypeName;
		Automation->TriggerType = TriggerType;
		Automation->ScheduleKind = ScheduleKind;
		Automation->IntervalMs = IntervalMs;
		return Automation;
	}

	TSharedPtr<FStudioContainer> MakeContainer(
		const FString& ContainerId,
		const FString& TypeName,
		const FString& DisplayName = FString(),
		int64 OwnerUserId = 0,
		const FString& SessionId = FString(),
		const FString& BindingKey = FString())
	{
		TSharedPtr<FStudioContainer> Container = MakeShared<FStudioContainer>();
		Container->ContainerId = ContainerId;
		Container->TypeName = TypeName;
		Container->DisplayName = DisplayName;
		Container->OwnerUserId = OwnerUserId;
		Container->SessionId = SessionId;
		Container->BindingKey = BindingKey;
		return Container;
	}

	TSharedPtr<FStudioAutomationTrigger> MakeTrigger(
		const FString& AutomationName,
		const FString& OnEvent,
		const FString& PropertyKey = FString(),
		const FString& FunctionName = FString())
	{
		TSharedPtr<FStudioAutomationTrigger> Trigger = MakeShared<FStudioAutomationTrigger>();
		Trigger->AutomationName = AutomationName;
		Trigger->OnEvent = OnEvent;
		Trigger->PropertyKey = PropertyKey;
		Trigger->FunctionName = FunctionName;
		return Trigger;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerReservedAttributeExcludedTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerReservedAttributeExcluded", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerReservedAttributeExcludedTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(MakeAttributeDef(TEXT("crowdy_rev"), TEXT("Hero")));
	Defs.Add(MakeAttributeDef(TEXT("Health"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAttributeRows(Defs);

	TestEqual(TEXT("The reserved revision key never becomes a row"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("Only the designer attribute survives"), Rows[0].Name, FString(TEXT("Health")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerReservedFunctionExcludedTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerReservedFunctionExcluded", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerReservedFunctionExcludedTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(MakeFunction(TEXT("__crowdy_touch_hero"), TEXT("Hero")));
	Functions.Add(MakeFunction(TEXT("DealDamage"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildFunctionRows(Functions, TEXT("Hero"));

	TestEqual(TEXT("The reserved touch function never becomes a row"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("Only the designer function survives"), Rows[0].Name, FString(TEXT("DealDamage")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerReservedFunctionNotCountedTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerReservedFunctionNotCounted", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerReservedFunctionNotCountedTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));

	TArray<TSharedPtr<FStudioFunction>> Functions;
	Functions.Add(MakeFunction(TEXT("__crowdy_touch_hero"), TEXT("Hero")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, Functions, {});

	TestEqual(TEXT("A model whose only function is its touch function is still one model"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("The touch function does not count as a function"), Models[0].FunctionCount, 0);
	}

	// The zero above is also what a model that counts nothing at all reports, so a designer function has to be
	// counted here too: on its own, "the reserved one is excluded" cannot tell the exclusion from a count that
	// never happens.
	Functions.Add(MakeFunction(TEXT("DealDamage"), TEXT("Hero")));
	const TArray<FCrowdyModelSummary> WithDesignerFunction = CrowdyModelLedger::BuildModelList(Types, Functions, {});

	TestEqual(TEXT("The model is still one model"), WithDesignerFunction.Num(), 1);
	if (WithDesignerFunction.Num() == 1)
	{
		TestEqual(TEXT("A designer function is counted, so the exclusion above is an exclusion and not a dead count"),
			WithDesignerFunction[0].FunctionCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerAutomationsCountedPerModelTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerAutomationsCountedPerModel", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerAutomationsCountedPerModelTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));

	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("HeroRegen"), TEXT("Hero")));
	Automations.Add(MakeAutomation(TEXT("HeroDecay"), TEXT("Hero")));
	Automations.Add(MakeAutomation(TEXT("NightlyCleanup"), FString()));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, Automations);

	TestEqual(TEXT("One model is built"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("A model counts its own automations"), Models[0].AutomationCount, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerNearReservedAttributeKeptTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerNearReservedAttributeKept", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerNearReservedAttributeKeptTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(MakeAttributeDef(TEXT("crowdy_revamp"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAttributeRows(Defs);

	TestEqual(TEXT("A key that only starts like the reserved key is not the reserved key"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("The near-miss attribute is kept verbatim"), Rows[0].Name, FString(TEXT("crowdy_revamp")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerDisplayFallsBackToTypeNameTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerDisplayFallsBackToTypeName", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerDisplayFallsBackToTypeNameTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero"), FString()));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("One model is built"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("A blank display name falls back to the type name"), Models[0].Display, FString(TEXT("Hero")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerSearchMatchesTypeDisplayDescriptionTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerSearchMatchesTypeDisplayDescription", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerSearchMatchesTypeDisplayDescriptionTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("HeroCharacter"), TEXT("Player Hero"), TEXT("The player's controlled avatar")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("Searching by the raw type name finds the model"),
		CrowdyModelLedger::FilterModels(Models, TEXT("HeroCharacter")).Num(), 1);
	TestEqual(TEXT("Searching by the display name finds the model"),
		CrowdyModelLedger::FilterModels(Models, TEXT("Player Hero")).Num(), 1);
	TestEqual(TEXT("Searching by the description finds the model"),
		CrowdyModelLedger::FilterModels(Models, TEXT("avatar")).Num(), 1);
	TestEqual(TEXT("An unrelated query finds nothing"),
		CrowdyModelLedger::FilterModels(Models, TEXT("Goblin")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerSearchCaseInsensitiveTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerSearchCaseInsensitive", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerSearchCaseInsensitiveTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("An uppercase query matches a lowercase key"), CrowdyModelLedger::FilterModels(Models, TEXT("HERO")).Num(), 1);
	TestEqual(TEXT("A mixed-case query matches too"), CrowdyModelLedger::FilterModels(Models, TEXT("HeRo")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerEmptyQueryReturnsAllTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerEmptyQueryReturnsAll", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerEmptyQueryReturnsAllTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));
	Types.Add(MakeType(TEXT("Goblin")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("An empty query returns every model"), CrowdyModelLedger::FilterModels(Models, FString()).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerWhitespaceQueryReturnsAllTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerWhitespaceQueryReturnsAll", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerWhitespaceQueryReturnsAllTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));
	Types.Add(MakeType(TEXT("Goblin")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("A whitespace-only query returns every model"), CrowdyModelLedger::FilterModels(Models, TEXT("   ")).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerAppWideAutomationRetainedTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerAppWideAutomationRetained", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerAppWideAutomationRetainedTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("NightlyCleanup"), FString()));
	Automations.Add(MakeAutomation(TEXT("HeroRegen"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> AppWideRows = CrowdyModelLedger::BuildAutomationRows(Automations, {}, FString());
	const TArray<FCrowdyModelRow> HeroRows = CrowdyModelLedger::BuildAutomationRows(Automations, {}, TEXT("Hero"));

	TestEqual(TEXT("Exactly the app-wide automation is returned for the empty type"), AppWideRows.Num(), 1);
	if (AppWideRows.Num() == 1)
	{
		TestEqual(TEXT("It is the automation with no target type"), AppWideRows[0].Name, FString(TEXT("NightlyCleanup")));
	}

	TestEqual(TEXT("A specific model's query returns only its own automation"), HeroRows.Num(), 1);
	if (HeroRows.Num() == 1)
	{
		TestEqual(TEXT("The app-wide automation does not leak into a model's list"), HeroRows[0].Name, FString(TEXT("HeroRegen")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerAutomationNoTriggerRendersScheduleTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerAutomationNoTriggerRendersSchedule", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerAutomationNoTriggerRendersScheduleTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("HeroRegen"), TEXT("Hero"), TEXT("schedule"), TEXT("interval"), 300000));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAutomationRows(Automations, {}, TEXT("Hero"));

	TestEqual(TEXT("One automation row is built"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("An automation with no event trigger still renders its schedule phrase"),
			Rows[0].Secondary, FString(TEXT("Runs every 5 minutes")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerEventTriggerRendersPlainPhraseTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerEventTriggerRendersPlainPhrase", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerEventTriggerRendersPlainPhraseTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("HeroRegen"), TEXT("Hero"), TEXT("event")));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(MakeTrigger(TEXT("HeroRegen"), TEXT("property_changed"), TEXT("Health")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAutomationRows(Automations, Triggers, TEXT("Hero"));

	TestEqual(TEXT("One automation row is built"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		// The raw server token never reaches the column: "property_changed" is wire vocabulary.
		TestEqual(TEXT("An event trigger reads as the thing it watches, in plain words"),
			Rows[0].Secondary, FString(TEXT("Runs when Health changes")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerSeveralTriggersRenderInOneStableOrderTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerSeveralTriggersRenderInOneStableOrder", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerSeveralTriggersRenderInOneStableOrderTest::RunTest(const FString& Parameters)
{
	// One automation may carry several triggers: the server identifies a trigger by its event and its filters, not
	// by its automation. The list order it answers in is not promised, so both orders must render identically.
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("SpawnGuard"), TEXT("Hero"), TEXT("event")));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(MakeTrigger(TEXT("SpawnGuard"), TEXT("property_changed"), TEXT("Health")));
	Triggers.Add(MakeTrigger(TEXT("SpawnGuard"), TEXT("function_invoked"), FString(), TEXT("Heal")));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Reversed;
	Reversed.Add(Triggers[1]);
	Reversed.Add(Triggers[0]);

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAutomationRows(Automations, Triggers, TEXT("Hero"));
	const TArray<FCrowdyModelRow> ReversedRows = CrowdyModelLedger::BuildAutomationRows(Automations, Reversed, TEXT("Hero"));

	TestEqual(TEXT("One automation row is built"), Rows.Num(), 1);
	TestEqual(TEXT("One automation row is built from the reversed read"), ReversedRows.Num(), 1);
	if (Rows.Num() == 1 && ReversedRows.Num() == 1)
	{
		TestEqual(TEXT("Every event the automation reacts to is shown, not just the first one read"),
			Rows[0].Secondary, FString(TEXT("Runs after Heal runs; Runs when Health changes")));
		TestEqual(TEXT("The order the server answered in does not change the row"),
			ReversedRows[0].Secondary, Rows[0].Secondary);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerTriggerOwnershipIsCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerTriggerOwnershipIsCaseSensitive", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerTriggerOwnershipIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// An automation name is a server key. A trigger naming a differently-cased name names a different automation,
	// and attaching it here would describe one automation with another's events.
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(MakeAutomation(TEXT("HeroRegen"), TEXT("Hero"), TEXT("schedule"), TEXT("interval"), 300000));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(MakeTrigger(TEXT("heroregen"), TEXT("property_changed"), TEXT("Health")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAutomationRows(Automations, Triggers, TEXT("Hero"));

	TestEqual(TEXT("One automation row is built"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("A trigger belonging to a differently-cased name is not this automation's"),
			Rows[0].Secondary, FString(TEXT("Runs every 5 minutes")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerAttributeCountDistinctFromZeroTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerAttributeCountDistinctFromZero", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerAttributeCountDistinctFromZeroTest::RunTest(const FString& Parameters)
{
	const FCrowdyModelSummary Default;
	TestEqual(TEXT("An unloaded summary reports not-loaded-yet"), Default.AttributeCount, INDEX_NONE);

	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Hero")));
	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("One model is built"), Models.Num(), 1);
	if (Models.Num() == 1)
	{
		TestEqual(TEXT("Building the model list never touches AttributeCount, so an unopened model stays not-loaded-yet"),
			Models[0].AttributeCount, INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerRowOrderingStableAlphabeticalTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerRowOrderingStableAlphabetical", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerRowOrderingStableAlphabeticalTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(MakeAttributeDef(TEXT("Zeal"), TEXT("Hero")));
	Defs.Add(MakeAttributeDef(TEXT("armor"), TEXT("Hero")));
	Defs.Add(MakeAttributeDef(TEXT("Health"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAttributeRows(Defs);

	TestEqual(TEXT("All three attributes become rows"), Rows.Num(), 3);
	if (Rows.Num() == 3)
	{
		TestEqual(TEXT("The first row is alphabetically first, case-insensitively"), Rows[0].Name, FString(TEXT("armor")));
		TestEqual(TEXT("The second row is alphabetically second"), Rows[1].Name, FString(TEXT("Health")));
		TestEqual(TEXT("The third row is alphabetically last"), Rows[2].Name, FString(TEXT("Zeal")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerModelOrderingStableAlphabeticalTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerModelOrderingStableAlphabetical", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerModelOrderingStableAlphabeticalTest::RunTest(const FString& Parameters)
{
	// Each display name deliberately sorts differently from its own type name, so an order taken from the type name
	// rather than from the name actually rendered puts these in the wrong places.
	TArray<TSharedPtr<FStudioContainerType>> Types;
	Types.Add(MakeType(TEXT("Zombie"), TEXT("apple")));
	Types.Add(MakeType(TEXT("apple"), TEXT("Zealot")));
	Types.Add(MakeType(TEXT("Bandit"), TEXT("Bandit")));

	const TArray<FCrowdyModelSummary> Models = CrowdyModelLedger::BuildModelList(Types, {}, {});

	TestEqual(TEXT("All three models are built"), Models.Num(), 3);
	if (Models.Num() == 3)
	{
		TestEqual(TEXT("Models sort case-insensitively by the name that is rendered"), Models[0].Display, FString(TEXT("apple")));
		TestEqual(TEXT("The second model follows alphabetically"), Models[1].Display, FString(TEXT("Bandit")));
		TestEqual(TEXT("The last model sorts last"), Models[2].Display, FString(TEXT("Zealot")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerUnrecognizedValueTypeFallsThroughTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerUnrecognizedValueTypeFallsThrough", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerUnrecognizedValueTypeFallsThroughTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("An unrecognized value type is shown verbatim, never blank and never guessed"),
		CrowdyModelVocabulary::ValueTypeLabel(TEXT("vector3")), FString(TEXT("vector3")));

	TArray<TSharedPtr<FStudioPropertyDef>> Defs;
	Defs.Add(MakeAttributeDef(TEXT("Position"), TEXT("Hero"), TEXT("vector3")));
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildAttributeRows(Defs);

	TestEqual(TEXT("One attribute row is built"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("The row's detail column carries the raw server type, not a blank"), Rows[0].Secondary, FString(TEXT("vector3")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerLiveRowsTypeFilterIsCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerLiveRowsTypeFilterIsCaseSensitive", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerLiveRowsTypeFilterIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// A type name is a server key. Two types differing only in case are two different models, and folding them
	// together would list one model's live instances under another's name.
	TArray<TSharedPtr<FStudioContainer>> Containers;
	Containers.Add(MakeContainer(TEXT("c-1"), TEXT("Hero")));
	Containers.Add(MakeContainer(TEXT("c-2"), TEXT("hero")));
	Containers.Add(MakeContainer(TEXT("c-3"), TEXT("Goblin")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildLiveRows(Containers, TEXT("Hero"));

	TestEqual(TEXT("Only the exactly-matching type's instances are listed"), Rows.Num(), 1);
	if (Rows.Num() == 1)
	{
		TestEqual(TEXT("The differently-cased type's instance is not this model's"), Rows[0].Name, FString(TEXT("c-1")));
		TestTrue(TEXT("A live instance is marked as one"), Rows[0].Kind == ECrowdyModelRowKind::LiveInstance);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerLiveRowTitleFallsBackToIdTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerLiveRowTitleFallsBackToId", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerLiveRowTitleFallsBackToIdTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainer>> Containers;
	Containers.Add(MakeContainer(TEXT("c-unnamed"), TEXT("Hero"), TEXT("   ")));
	Containers.Add(MakeContainer(TEXT("c-named"), TEXT("Hero"), TEXT("Knight of Ash")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildLiveRows(Containers, TEXT("Hero"));

	TestEqual(TEXT("Both instances become rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestEqual(TEXT("An unnamed instance shows its id rather than a blank title"), Rows[0].Primary, FString(TEXT("c-unnamed")));

		// Without this the fallback above is indistinguishable from a title cell that always holds the id.
		TestEqual(TEXT("A named instance shows its name"), Rows[1].Primary, FString(TEXT("Knight of Ash")));
		TestEqual(TEXT("The id has its own cell either way"), Rows[1].Secondary, FString(TEXT("c-named")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerLiveRowOwnerReadsPlainlyTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerLiveRowOwnerReadsPlainly", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerLiveRowOwnerReadsPlainlyTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainer>> Containers;
	Containers.Add(MakeContainer(TEXT("c-1"), TEXT("Hero"), FString(), 42));
	Containers.Add(MakeContainer(TEXT("c-2"), TEXT("Hero"), FString(), 0));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildLiveRows(Containers, TEXT("Hero"));

	TestEqual(TEXT("Both instances become rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestEqual(TEXT("An owned instance names its owner"), Rows[0].Owner, FString(TEXT("owner #42")));
		TestEqual(TEXT("An unowned instance says so rather than showing a zero"), Rows[1].Owner, FString(TEXT("unowned")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerLiveRowCarriesSessionAndBindingTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerLiveRowCarriesSessionAndBinding", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerLiveRowCarriesSessionAndBindingTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioContainer>> Containers;
	Containers.Add(MakeContainer(TEXT("c-1"), TEXT("Hero"), FString(), 0, TEXT("sess-77"), TEXT("hero:player-3")));
	Containers.Add(MakeContainer(TEXT("c-2"), TEXT("Hero")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildLiveRows(Containers, TEXT("Hero"));

	TestEqual(TEXT("Both instances become rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestEqual(TEXT("The session cell carries the session the instance belongs to"), Rows[0].Session, FString(TEXT("sess-77")));

		// The binding key is the reason a deleted instance can come back, so it must reach the column verbatim.
		TestEqual(TEXT("The binding cell carries the key the instance was ensured under"), Rows[0].Binding, FString(TEXT("hero:player-3")));
		TestEqual(TEXT("An app-global instance has no session"), Rows[1].Session, FString());
		TestEqual(TEXT("An instance created outright has no binding key"), Rows[1].Binding, FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerLiveRowsKeepServerOrderTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerLiveRowsKeepServerOrder", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerLiveRowsKeepServerOrderTest::RunTest(const FString& Parameters)
{
	// The server pages over its own ordering. Re-ordering the rows here would mean the page a reader is looking
	// at is not the page that was fetched, and loading the next one would reshuffle the rows already on screen.
	TArray<TSharedPtr<FStudioContainer>> Containers;
	Containers.Add(MakeContainer(TEXT("c-1"), TEXT("Hero"), TEXT("Zeal")));
	Containers.Add(MakeContainer(TEXT("c-2"), TEXT("Hero"), TEXT("Armor")));
	Containers.Add(MakeContainer(TEXT("c-3"), TEXT("Hero"), TEXT("Mist")));

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildLiveRows(Containers, TEXT("Hero"));

	TestEqual(TEXT("All three instances become rows"), Rows.Num(), 3);
	if (Rows.Num() == 3)
	{
		TestEqual(TEXT("The first row is the one the server answered first"), Rows[0].Primary, FString(TEXT("Zeal")));
		TestEqual(TEXT("The second row keeps its place"), Rows[1].Primary, FString(TEXT("Armor")));
		TestEqual(TEXT("The last row keeps its place"), Rows[2].Primary, FString(TEXT("Mist")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryReadsAsASentenceTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryReadsAsASentence", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryReadsAsASentenceTest::RunTest(const FString& Parameters)
{
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(
		TEXT("{\"hp\":84,\"mana\":50,\"stamina\":100,\"level\":7}"), 6);

	TestEqual(TEXT("The properties read as one plain line, in key order"),
		Summary, FString(TEXT("hp 84, level 7, mana 50, stamina 100")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryStripsTrailingZeroTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryStripsTrailingZero", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryStripsTrailingZeroTest::RunTest(const FString& Parameters)
{
	// Every number arrives as a JSON double, so a whole one must not read as 84.000000 and look like a precision
	// problem. A genuine fraction still has to survive, or "stripped" would just mean "truncated".
	TestEqual(TEXT("A whole number reads as a whole number"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"hp\":84.0}"), 6), FString(TEXT("hp 84")));
	TestEqual(TEXT("A fraction keeps the digits it needs"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"ratio\":0.25}"), 6), FString(TEXT("ratio 0.25")));
	TestEqual(TEXT("A negative whole number reads plainly too"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"drift\":-3}"), 6), FString(TEXT("drift -3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryBooleansReadYesNoTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryBooleansReadYesNo", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryBooleansReadYesNoTest::RunTest(const FString& Parameters)
{
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(TEXT("{\"alive\":true,\"stunned\":false}"), 6);

	TestEqual(TEXT("A flag reads as a plain answer, not as a wire literal"),
		Summary, FString(TEXT("alive yes, stunned no")));
	TestFalse(TEXT("The wire literals never reach the line"),
		Summary.Contains(TEXT("true"), ESearchCase::CaseSensitive) || Summary.Contains(TEXT("false"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryStringsAreBareTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryStringsAreBare", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryStringsAreBareTest::RunTest(const FString& Parameters)
{
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(TEXT("{\"title\":\"Knight of Ash\"}"), 6);

	TestEqual(TEXT("A text value reads as the text itself"), Summary, FString(TEXT("title Knight of Ash")));
	TestFalse(TEXT("No JSON quoting survives into the line"), Summary.Contains(TEXT("\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryLongValuesAreCutShortTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryLongValuesAreCutShort", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryLongValuesAreCutShortTest::RunTest(const FString& Parameters)
{
	// Capping how many properties the line names bounds nothing on its own: one designer attribute holding a
	// paragraph of prose is enough to fill a line that is one row tall and does not scroll, and every property
	// after it would be off the end with nothing to say so. A key sorting early is the worst case, so use one.
	FString Paragraph;
	for (int32 Index = 0; Index < 400; ++Index)
	{
		Paragraph.AppendChar(TEXT('a'));
	}

	const FString Summary = CrowdyModelLedger::FormatPropertySummary(
		FString::Printf(TEXT("{\"bio\":\"%s\",\"hp\":84}"), *Paragraph), 6);

	TestTrue(TEXT("The line stays short enough to read"), Summary.Len() < 120);
	TestTrue(TEXT("The properties after the long one still reach the line"),
		Summary.Contains(TEXT("hp 84"), ESearchCase::CaseSensitive));

	// A value that fits must come through untouched, or "bounded" would just mean "truncated".
	TestEqual(TEXT("A value within the budget is left alone"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"title\":\"Knight of Ash\"}"), 6),
		FString(TEXT("title Knight of Ash")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryNestedValuesAreNamedTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryNestedValuesAreNamed", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryNestedValuesAreNamedTest::RunTest(const FString& Parameters)
{
	// This line is one row tall and does not scroll. A nested value flattened into it is both unreadable and
	// unbounded, so it is named by what it is instead.
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(
		TEXT("{\"gear\":{\"weapon\":\"axe\",\"armor\":\"mail\"},\"tags\":[\"undead\",\"boss\"]}"), 6);

	TestEqual(TEXT("A nested value is named by its kind"), Summary, FString(TEXT("gear structured data, tags list")));
	TestFalse(TEXT("No nested blob is flattened onto the line"),
		Summary.Contains(TEXT("{")) || Summary.Contains(TEXT("[")) || Summary.Contains(TEXT("axe")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryOrderIsDeterministicTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryOrderIsDeterministic", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryOrderIsDeterministicTest::RunTest(const FString& Parameters)
{
	// A JSON object promises no key order. Two reads of the same instance must still produce the same line, or
	// the strip reshuffles under the reader every refresh.
	const FString First = CrowdyModelLedger::FormatPropertySummary(
		TEXT("{\"stamina\":100,\"hp\":84,\"Level\":7}"), 6);
	const FString Second = CrowdyModelLedger::FormatPropertySummary(
		TEXT("{\"Level\":7,\"stamina\":100,\"hp\":84}"), 6);

	TestEqual(TEXT("The key order the server answered in does not change the line"), Second, First);
	TestEqual(TEXT("Keys are ordered case-insensitively, so casing does not sort a key to an odd place"),
		First, FString(TEXT("hp 84, Level 7, stamina 100")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryStopsAtTheCapTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryStopsAtTheCap", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryStopsAtTheCapTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}");

	TestEqual(TEXT("Beyond the cap the line stops and says how many it left out"),
		CrowdyModelLedger::FormatPropertySummary(Json, 3), FString(TEXT("a 1, b 2, c 3, and 2 more")));
	TestEqual(TEXT("A cap the instance does not reach adds nothing"),
		CrowdyModelLedger::FormatPropertySummary(Json, 5), FString(TEXT("a 1, b 2, c 3, d 4, e 5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryUnreadableJsonIsEmptyTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryUnreadableJsonIsEmpty", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryUnreadableJsonIsEmptyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("No state at all reads as nothing"),
		CrowdyModelLedger::FormatPropertySummary(FString(), 6), FString());
	TestEqual(TEXT("Whitespace-only state reads as nothing"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("   "), 6), FString());
	TestEqual(TEXT("A truncated object reads as nothing, not as a parse error"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"hp\":"), 6), FString());

	// The raw input is exactly what this line replaces, so falling back to it would undo the whole strip.
	TestEqual(TEXT("Text that is not JSON at all is not echoed back"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("not json at all"), 6), FString());
	TestEqual(TEXT("An object with no properties reads as nothing"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{}"), 6), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertySummaryExcludesReservedKeyTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertySummaryExcludesReservedKey", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertySummaryExcludesReservedKeyTest::RunTest(const FString& Parameters)
{
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(
		TEXT("{\"crowdy_rev\":12,\"hp\":84,\"mana\":50}"), 6);

	TestEqual(TEXT("The runtime's own bookkeeping key never reaches the line"),
		Summary, FString(TEXT("hp 84, mana 50")));
	TestFalse(TEXT("Nothing of the reserved key survives"), Summary.Contains(TEXT("crowdy_rev")));

	// An excluded key is not a shown key, so it must not consume a slot or be counted as left out either.
	TestEqual(TEXT("An excluded key does not count against the cap"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"crowdy_rev\":12,\"hp\":84,\"mana\":50}"), 2),
		FString(TEXT("hp 84, mana 50")));

	// The exclusion must be the reserved key and not a prefix, or a designer attribute that merely starts like it
	// would vanish with no explanation.
	TestEqual(TEXT("A key that only starts like the reserved key is kept"),
		CrowdyModelLedger::FormatPropertySummary(TEXT("{\"crowdy_revamp\":3}"), 6), FString(TEXT("crowdy_revamp 3")));
	return true;
}

namespace
{
	// The defs of one small model, in the order the server would answer in rather than in the order the rows come
	// out: a test that feeds sorted input cannot tell whether the sort ran.
	TArray<TSharedPtr<FStudioPropertyDef>> MakeHeroDefs()
	{
		return TArray<TSharedPtr<FStudioPropertyDef>>{
			MakeAttributeDef(TEXT("mana"), TEXT("Hero"), TEXT("int")),
			MakeAttributeDef(TEXT("hp"), TEXT("Hero"), TEXT("int"), TEXT("What is left before the hero falls.")),
			MakeAttributeDef(TEXT("title"), TEXT("Hero"), TEXT("string"))
		};
	}

	const FCrowdyModelRow* FindPropertyRow(const TArray<FCrowdyModelRow>& Rows, const TCHAR* Key)
	{
		return Rows.FindByPredicate([Key](const FCrowdyModelRow& Row)
		{
			return Row.Name.Equals(Key, ESearchCase::CaseSensitive);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsPairValuesWithAttributesTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsPairValuesWithAttributes", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsPairValuesWithAttributesTest::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		TEXT("{\"mana\":50,\"hp\":84,\"title\":\"Knight of Ash\"}"), &Defs);

	if (!TestEqual(TEXT("Every stored value becomes its own row"), Rows.Num(), 3))
	{
		return false;
	}

	// One row per attribute, each carrying the value beside it rather than a line the reader has to parse.
	TestEqual(TEXT("Rows read in name order"), Rows[0].Name, FString(TEXT("hp")));
	TestEqual(TEXT("A number reads as a number"), Rows[0].Value, FString(TEXT("84")));
	TestEqual(TEXT("The declared type is what the model says, not what the value looks like"),
		Rows[0].Secondary, FString(TEXT("Number")));
	TestEqual(TEXT("The attribute's description travels with it"),
		Rows[0].Detail, FString(TEXT("What is left before the hero falls.")));
	TestEqual(TEXT("The second row is the next attribute by name"), Rows[1].Name, FString(TEXT("mana")));
	TestEqual(TEXT("Text reads as the text itself"), Rows[2].Value, FString(TEXT("Knight of Ash")));
	TestEqual(TEXT("The clipboard gets the value as the server holds it"),
		Rows[2].RawValue, FString(TEXT("Knight of Ash")));
	TestTrue(TEXT("A row built from a stored value says it carries one"), Rows[0].bHasValue);
	TestEqual(TEXT("A property row says which model it belongs to"), Rows[0].OwningType, FString(TEXT("Hero")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsShowDeclaredButUnsetTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsShowDeclaredButUnset", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsShowDeclaredButUnsetTest::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(TEXT("{\"hp\":84}"), &Defs);

	if (!TestEqual(TEXT("An attribute with no value is still a row"), Rows.Num(), 3))
	{
		return false;
	}

	// What the instance holds comes first: a reader opens this to see the values, and burying them among the
	// declared-but-empty rows answers a question nobody asked first.
	TestEqual(TEXT("The row carrying a value leads"), Rows[0].Name, FString(TEXT("hp")));
	TestEqual(TEXT("Then the unset ones, in name order"), Rows[1].Name, FString(TEXT("mana")));
	TestEqual(TEXT("An attribute with no value says so in plain words"), Rows[1].Value, FString(TEXT("not set")));
	TestTrue(TEXT("And copying it copies nothing rather than the words"), Rows[1].RawValue.IsEmpty());
	// The counts under the table and the Copy value button both read this rather than the words in the cell.
	TestTrue(TEXT("The row carrying a value says so"), Rows[0].bHasValue);
	TestFalse(TEXT("The unset one says it carries none"), Rows[1].bHasValue);
	TestEqual(TEXT("It still shows what the model declares it to be"), Rows[1].Secondary, FString(TEXT("Number")));
	TestEqual(TEXT("The last unset attribute is there too"), Rows[2].Name, FString(TEXT("title")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsClaimNothingWithoutDefsTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsClaimNothingWithoutDefs", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsClaimNothingWithoutDefsTest::RunTest(const FString& Parameters)
{
	// Nobody has read this model's attributes. An attribute the model does not declare and an attribute nobody
	// looked up are the same thing from here, so neither claim may be made.
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		TEXT("{\"hp\":84,\"ratio\":0.25}"), nullptr);

	if (!TestEqual(TEXT("Only what the instance carries becomes a row"), Rows.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("Nothing is said about what the model declares"), Rows[0].Detail.IsEmpty());
	// The type still has to read as something, and the one honest source left is the value that arrived.
	TestEqual(TEXT("A whole number is described as a whole number"), Rows[0].Secondary, FString(TEXT("Number")));
	TestEqual(TEXT("A fractional one is not"), Rows[1].Secondary, FString(TEXT("Decimal")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsNameUndeclaredValuesTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsNameUndeclaredValues", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsNameUndeclaredValuesTest::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		TEXT("{\"hp\":84,\"stowaway\":7}"), &Defs);

	const FCrowdyModelRow* Stray = FindPropertyRow(Rows, TEXT("stowaway"));
	if (!TestNotNull(TEXT("A stored value the model does not declare is still shown"), Stray))
	{
		return false;
	}

	TestEqual(TEXT("And it is named as one"), Stray->Detail,
		FString(TEXT("This model does not declare this attribute.")));

	const FCrowdyModelRow* Declared = FindPropertyRow(Rows, TEXT("hp"));
	if (!TestNotNull(TEXT("A declared attribute is present as well"), Declared))
	{
		return false;
	}
	TestFalse(TEXT("A declared one is never labelled undeclared"),
		Declared->Detail.Contains(TEXT("does not declare")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsRenderEveryValueKindTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsRenderEveryValueKind", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsRenderEveryValueKindTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		TEXT("{\"alive\":true,\"blank\":\"\",\"gear\":[\"axe\",\"rope\"],\"missing\":null,\"stats\":{\"str\":3}}"),
		nullptr);

	const FCrowdyModelRow* Alive = FindPropertyRow(Rows, TEXT("alive"));
	const FCrowdyModelRow* Blank = FindPropertyRow(Rows, TEXT("blank"));
	const FCrowdyModelRow* Gear = FindPropertyRow(Rows, TEXT("gear"));
	const FCrowdyModelRow* Missing = FindPropertyRow(Rows, TEXT("missing"));
	const FCrowdyModelRow* Stats = FindPropertyRow(Rows, TEXT("stats"));
	if (!TestTrue(TEXT("Every kind of stored value became a row"),
		Alive && Blank && Gear && Missing && Stats))
	{
		return false;
	}

	TestEqual(TEXT("A flag reads as a plain answer"), Alive->Value, FString(TEXT("yes")));
	TestEqual(TEXT("And copies as what the server holds"), Alive->RawValue, FString(TEXT("true")));
	// Three different kinds of nothing, told apart: an empty text, a stored null, and an attribute with no value
	// at all. A blank cell for any of them would leave the reader unable to say which they were looking at.
	TestEqual(TEXT("An empty text says it is empty"), Blank->Value, FString(TEXT("empty")));
	// And it is a value the instance holds, which is why nothing reads that back off an empty cell or an empty
	// clipboard string: both are empty for this row and neither means the attribute is unset.
	TestTrue(TEXT("An empty text is still a stored value"), Blank->bHasValue);
	TestEqual(TEXT("A stored null is a value the instance holds"), Missing->Value, FString(TEXT("none")));
	// A structure is shown as itself: a reader opening a live model wants what is inside it.
	TestEqual(TEXT("A list shows its items"), Gear->Value, FString(TEXT("[\"axe\",\"rope\"]")));
	TestEqual(TEXT("A structure shows its fields"), Stats->Value, FString(TEXT("{\"str\":3}")));
	TestEqual(TEXT("A list is described as a list"), Gear->Secondary, FString(TEXT("List")));
	TestEqual(TEXT("A structure is described as structured data"), Stats->Secondary, FString(TEXT("Structured data")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsBoundLongValuesTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsBoundLongValues", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsBoundLongValuesTest::RunTest(const FString& Parameters)
{
	FString Paragraph;
	for (int32 Index = 0; Index < 2000; ++Index)
	{
		Paragraph.AppendChar(TEXT('a'));
	}

	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		FString::Printf(TEXT("{\"bio\":\"%s\"}"), *Paragraph), nullptr);

	if (!TestEqual(TEXT("The long value became a row"), Rows.Num(), 1))
	{
		return false;
	}

	// The cell's tooltip repeats the cell, so an unbounded value is an unbounded tooltip. The clipboard is the
	// way to read the whole of one, and it is never cut.
	TestTrue(TEXT("What a cell shows is bounded"), Rows[0].Value.Len() < 500);
	TestEqual(TEXT("What the clipboard gets is whole"), Rows[0].RawValue.Len(), Paragraph.Len());
	TestTrue(TEXT("And the cut is visible rather than silent"), Rows[0].Value.EndsWith(TEXT("...")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsHideReservedKeysTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsHideReservedKeys", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsHideReservedKeysTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"crowdy_rev\":12,\"hp\":84}");

	const TArray<FCrowdyModelRow> Hidden = CrowdyModelLedger::BuildPropertyRows(Json, nullptr);
	TestEqual(TEXT("The runtime's own bookkeeping stays out of the way"), Hidden.Num(), 1);
	TestNull(TEXT("Nothing of the reserved key is shown"), FindPropertyRow(Hidden, TEXT("crowdy_rev")));

	// It is hidden, not unreachable: a reader chasing a replication problem has to be able to see it.
	const TArray<FCrowdyModelRow> Shown =
		CrowdyModelLedger::BuildPropertyRows(Json, nullptr, nullptr, /*bIncludeReserved*/ true);
	TestEqual(TEXT("Asked for, it is there"), Shown.Num(), 2);
	TestNotNull(TEXT("Under its own key"), FindPropertyRow(Shown, TEXT("crowdy_rev")));

	// A reserved attribute the model happens to declare must not come back as an unset row either.
	TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();
	Defs.Add(MakeAttributeDef(TEXT("crowdy_rev"), TEXT("Hero"), TEXT("int")));
	const TArray<FCrowdyModelRow> WithDefs = CrowdyModelLedger::BuildPropertyRows(TEXT("{}"), &Defs);
	TestNull(TEXT("A reserved attribute is not offered as unset"), FindPropertyRow(WithDefs, TEXT("crowdy_rev")));
	TestEqual(TEXT("The designer attributes still are"), WithDefs.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsRefuseUnreadablePayloadTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsRefuseUnreadablePayload", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsRefuseUnreadablePayloadTest::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();

	// A payload that cannot be read says nothing about the instance, the declared attributes included: reporting
	// every one of them as carrying no value would be a claim made from a read that never landed.
	const TArray<FCrowdyModelRow> Broken = CrowdyModelLedger::BuildPropertyRows(TEXT("{\"hp\":"), &Defs);
	TestEqual(TEXT("A payload that will not parse yields no rows"), Broken.Num(), 0);

	FString TooDeep;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		TooDeep += TEXT("{\"a\":");
	}
	TooDeep += TEXT("1");
	for (int32 Index = 0; Index < 200; ++Index)
	{
		TooDeep += TEXT("}");
	}
	TestEqual(TEXT("So does one nested past the parser's guard"),
		CrowdyModelLedger::BuildPropertyRows(TooDeep, &Defs).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertiesReadStateTellsEmptyFromUnreadableTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertiesReadStateTellsEmptyFromUnreadable", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertiesReadStateTellsEmptyFromUnreadableTest::RunTest(const FString& Parameters)
{
	// An empty list of rows means four different things, and a panel that cannot tell them apart reports a server
	// error as an instance with nothing in it.
	TestTrue(TEXT("Nothing read yet is not an answer about the instance"),
		CrowdyModelLedger::ClassifyPropertiesJson(FString()) == ECrowdyPropertyReadState::NoValues);
	TestTrue(TEXT("An instance that genuinely holds nothing says so"),
		CrowdyModelLedger::ClassifyPropertiesJson(TEXT("{}")) == ECrowdyPropertyReadState::NoValues);
	TestTrue(TEXT("An instance with values reads as readable"),
		CrowdyModelLedger::ClassifyPropertiesJson(TEXT("{\"hp\":84}")) == ECrowdyPropertyReadState::Ok);
	TestTrue(TEXT("A payload that will not parse is its own answer"),
		CrowdyModelLedger::ClassifyPropertiesJson(TEXT("not json at all")) == ECrowdyPropertyReadState::Unreadable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelLedgerPropertyRowsAreSearchableByValueTest,
	"CrowdySDK.CrowdyStudio.ModelLedgerPropertyRowsAreSearchableByValue", CrowdyModelLedgerTestFlags)

bool FCrowdyModelLedgerPropertyRowsAreSearchableByValueTest::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FStudioPropertyDef>> Defs = MakeHeroDefs();
	const TArray<FCrowdyModelRow> Rows = CrowdyModelLedger::BuildPropertyRows(
		TEXT("{\"hp\":84,\"title\":\"Knight of Ash\"}"), &Defs);

	// The search box over a property list is used to find a value as often as to find an attribute, so the value
	// is in the haystack the filter reads.
	const TArray<FCrowdyModelRow> ByValue = CrowdyModelLedger::FilterRows(Rows, TEXT("knight"));
	if (!TestEqual(TEXT("Typing a value finds the attribute holding it"), ByValue.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("And it is the right one"), ByValue[0].Name, FString(TEXT("title")));

	const TArray<FCrowdyModelRow> ByKey = CrowdyModelLedger::FilterRows(Rows, TEXT("hp"));
	TestEqual(TEXT("Typing an attribute still finds it"), ByKey.Num(), 1);
	return true;
}

#endif
