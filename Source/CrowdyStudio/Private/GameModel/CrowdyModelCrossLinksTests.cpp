// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdyModelCrossLinks.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelCrossLinksTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every fixture here carries the CrossLinkTest prefix. Adaptive unity merges this module's .cpp files into
	// shared translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	TSharedPtr<FStudioFunction> CrossLinkTestFunction(const FString& OwningType, const FString& Name)
	{
		TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
		Function->ContainerTypeName = OwningType;
		Function->Name = Name;
		return Function;
	}

	void CrossLinkTestAddMutation(
		const TSharedPtr<FStudioFunction>& Function, const FString& Target, const FString& Property,
		const FString& Expression)
	{
		FStudioFunctionMutation Mutation;
		Mutation.Target = Target;
		Mutation.Property = Property;
		Mutation.Expression = Expression;
		Function->Mutations.Add(MoveTemp(Mutation));
	}

	TSharedPtr<FStudioAutomation> CrossLinkTestAutomation(
		const FString& Name, const FString& FunctionName, const FString& TargetTypeName)
	{
		TSharedPtr<FStudioAutomation> Automation = MakeShared<FStudioAutomation>();
		Automation->Name = Name;
		Automation->FunctionName = FunctionName;
		Automation->TargetMode = TEXT("type");
		Automation->TargetTypeName = TargetTypeName;
		return Automation;
	}

	TSharedPtr<FStudioAutomationTrigger> CrossLinkTestTrigger(
		const FString& AutomationName, const FString& ContainerTypeName, const FString& PropertyKey)
	{
		TSharedPtr<FStudioAutomationTrigger> Trigger = MakeShared<FStudioAutomationTrigger>();
		Trigger->AutomationName = AutomationName;
		Trigger->OnEvent = TEXT("property_changed");
		Trigger->ContainerTypeName = ContainerTypeName;
		Trigger->PropertyKey = PropertyKey;
		return Trigger;
	}

	const FCrowdyModelLink* CrossLinkTestFind(
		const TArray<FCrowdyModelLink>& Links, ECrowdyModelRowKind Kind, const FString& TargetModel,
		const FString& TargetName)
	{
		for (const FCrowdyModelLink& Link : Links)
		{
			if (Link.Kind == Kind
				&& Link.TargetModel.Equals(TargetModel, ESearchCase::CaseSensitive)
				&& Link.TargetName.Equals(TargetName, ESearchCase::CaseSensitive))
			{
				return &Link;
			}
		}
		return nullptr;
	}

	int32 CrossLinkTestCountRelation(const TArray<FCrowdyModelLink>& Links, const FString& Relation)
	{
		int32 Count = 0;
		for (const FCrowdyModelLink& Link : Links)
		{
			if (Link.Relation.Equals(Relation, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksFindTheFunctionsThatWriteAnAttributeTest,
	"CrowdySDK.CrowdyStudio.CrossLinksFindTheFunctionsThatWriteAnAttribute", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksFindTheFunctionsThatWriteAnAttributeTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> TakeDamage = CrossLinkTestFunction(TEXT("Knight"), TEXT("take_damage"));
	CrossLinkTestAddMutation(TakeDamage, TEXT("self"), TEXT("hp"), TEXT("self.hp - 10"));
	Functions.Add(TakeDamage);

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), Functions, {}, {});

	TestEqual(TEXT("the function that writes the key is the one link"), Links.Num(), 1);
	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Function, TEXT("Knight"), TEXT("take_damage"));
	if (!Link)
	{
		AddError(TEXT("the function that writes hp is not in the links"));
		return false;
	}

	// A write is not a read. Reporting it as the weaker relationship is the same answer a reader cannot act on.
	TestEqual(TEXT("the relationship is the write"), Link->Relation, FString(TEXT("writes it")));
	TestEqual(TEXT("the link opens the functions section"), Link->TargetSection, FString(TEXT("functions")));
	TestTrue(TEXT("the link can be followed"), Link->bNavigable);
	TestTrue(TEXT("the line names the function"),
		Link->Display.Contains(TEXT("take_damage"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksMatchWholeIdentifiersOnlyTest,
	"CrowdySDK.CrowdyStudio.CrossLinksMatchWholeIdentifiersOnly", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksMatchWholeIdentifiersOnlyTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Regen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	CrossLinkTestAddMutation(Regen, TEXT("self"), TEXT("hp_max"), TEXT("self.hp_max + 1"));
	Functions.Add(Regen);

	// The control: the key the function really does name is reported, so an empty answer below is the rule working
	// rather than a fixture that names nothing.
	const TArray<FCrowdyModelLink> ForLongKey = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp_max"), Functions, {}, {});
	TestEqual(TEXT("the key the function writes is reported"), ForLongKey.Num(), 1);

	const TArray<FCrowdyModelLink> ForShortKey = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), Functions, {}, {});
	TestEqual(TEXT("a longer key starting with the same letters is not reported"), ForShortKey.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksAreCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.CrossLinksAreCaseSensitive", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksAreCaseSensitiveTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Heal = CrossLinkTestFunction(TEXT("Knight"), TEXT("heal"));
	CrossLinkTestAddMutation(Heal, TEXT("self"), TEXT("HP"), TEXT("1"));
	Functions.Add(Heal);

	const TArray<FCrowdyModelLink> ForUpper = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("HP"), Functions, {}, {});
	TestEqual(TEXT("the key the function writes is reported"), ForUpper.Num(), 1);

	const TArray<FCrowdyModelLink> ForLower = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), Functions, {}, {});
	TestEqual(TEXT("two keys differing only in case are two keys"), ForLower.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksFindTheAutomationsThatRunAFunctionTest,
	"CrowdySDK.CrowdyStudio.CrossLinksFindTheAutomationsThatRunAFunction", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksFindTheAutomationsThatRunAFunctionTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Regen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	Functions.Add(Regen);

	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(CrossLinkTestAutomation(TEXT("knight_tick"), TEXT("regen"), TEXT("Knight")));
	Automations.Add(CrossLinkTestAutomation(TEXT("goblin_tick"), TEXT("decay"), TEXT("Goblin")));

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForFunction(*Regen, Functions, Automations, {});

	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Automation, TEXT("Knight"), TEXT("knight_tick"));
	if (!Link)
	{
		AddError(TEXT("the automation that runs the function is not in the links"));
		return false;
	}
	TestEqual(TEXT("the relationship is the run"), Link->Relation, FString(TEXT("runs it")));
	TestEqual(TEXT("the link opens the automations section"), Link->TargetSection, FString(TEXT("automations")));
	TestTrue(TEXT("the link can be followed"), Link->bNavigable);
	TestTrue(TEXT("an automation running a different function is not reported"),
		CrossLinkTestFind(Links, ECrowdyModelRowKind::Automation, TEXT("Goblin"), TEXT("goblin_tick")) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksNameEveryScopeCarryingAFunctionNameTest,
	"CrowdySDK.CrowdyStudio.CrossLinksNameEveryScopeCarryingAFunctionName", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksNameEveryScopeCarryingAFunctionNameTest::RunTest(const FString& Parameters)
{
	// Three models carry one name. Standing on any one of them, the other two are what the reader has not been told
	// about anywhere else, and the server resolves some calls to this name without a model.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> KnightRegen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	Functions.Add(KnightRegen);
	Functions.Add(CrossLinkTestFunction(TEXT("Goblin"), TEXT("regen")));
	Functions.Add(CrossLinkTestFunction(TEXT("Orc"), TEXT("regen")));

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForFunction(*KnightRegen, Functions, {}, {});

	TestEqual(TEXT("both other models carrying the name are reported"),
		CrossLinkTestCountRelation(Links, TEXT("same name")), 2);
	TestTrue(TEXT("the copy on Goblin is named"),
		CrossLinkTestFind(Links, ECrowdyModelRowKind::Function, TEXT("Goblin"), TEXT("regen")) != nullptr);
	TestTrue(TEXT("the copy on Orc is named"),
		CrossLinkTestFind(Links, ECrowdyModelRowKind::Function, TEXT("Orc"), TEXT("regen")) != nullptr);
	TestTrue(TEXT("the copy the reader is standing on is not named"),
		CrossLinkTestFind(Links, ECrowdyModelRowKind::Function, TEXT("Knight"), TEXT("regen")) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksMarkAnUndeterminedTargetUnnavigableTest,
	"CrowdySDK.CrowdyStudio.CrossLinksMarkAnUndeterminedTargetUnnavigable", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksMarkAnUndeterminedTargetUnnavigableTest::RunTest(const FString& Parameters)
{
	// The automation the trigger fires runs against no model, so it is listed under the app itself and there is no
	// model to open it on. Dropping the line hides a real dependency and guessing a model sends the reader
	// somewhere wrong, so it is shown and cannot be followed.
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(CrossLinkTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString()));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(CrossLinkTestTrigger(TEXT("goblin_tick"), FString(), TEXT("hp")));

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), {}, Automations, Triggers);

	TestEqual(TEXT("the trigger is reported"), Links.Num(), 1);
	if (Links.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("the link is to the automation the trigger fires"),
		Links[0].TargetName, FString(TEXT("goblin_tick")));
	TestEqual(TEXT("the relationship is the wait"), Links[0].Relation, FString(TEXT("waits on it")));
	TestFalse(TEXT("the link cannot be followed"), Links[0].bNavigable);
	TestTrue(TEXT("the line says why it cannot be followed"),
		Links[0].Display.Contains(TEXT("cannot be opened"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinkSummaryDistinguishesUnreadFromNoneTest,
	"CrowdySDK.CrowdyStudio.CrossLinkSummaryDistinguishesUnreadFromNone", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinkSummaryDistinguishesUnreadFromNoneTest::RunTest(const FString& Parameters)
{
	const FString GenuineZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded);
	const FString FailedZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Failed, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded);
	const FString UnreadZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::NeverRequested, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded);

	TestTrue(TEXT("a genuine zero says nothing names this"), !GenuineZero.IsEmpty());
	TestNotEqual(TEXT("a zero over a failed list is not a zero"), FailedZero, GenuineZero);
	TestNotEqual(TEXT("a zero over an unread list is not a zero"), UnreadZero, GenuineZero);
	TestNotEqual(TEXT("a failed read and a read nobody made are two answers"), FailedZero, UnreadZero);
	TestTrue(TEXT("the failed sentence says the read failed"),
		FailedZero.Contains(TEXT("could not be read"), ESearchCase::CaseSensitive));

	// A count is still reported when a list is missing, because the links found are facts; the sentence says the
	// total is a floor rather than claiming it is complete.
	const FString CountedOverAFailedList = CrowdyModelCrossLinks::SummaryLine(
		2, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Failed, ECrowdyModelLoadState::Loaded);
	TestTrue(TEXT("the count is named"),
		CountedOverAFailedList.Contains(TEXT("2 things name this"), ESearchCase::CaseSensitive));
	TestNotEqual(TEXT("a count over a failed list is not the plain count"),
		CountedOverAFailedList,
		CrowdyModelCrossLinks::SummaryLine(
			2, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinkSummaryAnswersForAFailedTriggerReadTest,
	"CrowdySDK.CrowdyStudio.CrossLinkSummaryAnswersForAFailedTriggerRead", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinkSummaryAnswersForAFailedTriggerReadTest::RunTest(const FString& Parameters)
{
	// The event triggers are a second read chained behind the automations and they can fail on their own, leaving
	// the automations present and every "waits on it" line gone. A summary that folded them into the automations
	// state would report that as a genuine zero.
	const FString GenuineZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded);
	const FString TriggersFailedZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Failed);

	TestNotEqual(TEXT("a zero over a failed trigger read is not a zero"), TriggersFailedZero, GenuineZero);
	TestTrue(TEXT("the sentence names the triggers"),
		TriggersFailedZero.Contains(TEXT("automation triggers"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the sentence says the read failed"),
		TriggersFailedZero.Contains(TEXT("could not be read"), ESearchCase::CaseSensitive));

	// A read nobody made and a read that failed are two answers here too, and neither is the plain zero.
	const FString TriggersUnreadZero = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::Loaded, ECrowdyModelLoadState::NeverRequested);
	TestNotEqual(TEXT("an unread trigger list is not a zero"), TriggersUnreadZero, GenuineZero);
	TestNotEqual(TEXT("a failed trigger read and one nobody made are two answers"),
		TriggersUnreadZero, TriggersFailedZero);

	// All three missing at once is one sentence naming all three, not a clause that swallows the others.
	const FString AllThreeFailed = CrowdyModelCrossLinks::SummaryLine(
		0, ECrowdyModelLoadState::Failed, ECrowdyModelLoadState::Failed, ECrowdyModelLoadState::Failed);
	TestTrue(TEXT("the functions are named"),
		AllThreeFailed.Contains(TEXT("functions"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the automations are named"),
		AllThreeFailed.Contains(TEXT("automations"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the triggers are named"),
		AllThreeFailed.Contains(TEXT("automation triggers"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksOpenAnAutomationOnItsOwnModelTest,
	"CrowdySDK.CrowdyStudio.CrossLinksOpenAnAutomationOnItsOwnModel", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksOpenAnAutomationOnItsOwnModelTest::RunTest(const FString& Parameters)
{
	// The trigger filters on Knight, the automation it fires runs against Goblin, and Goblin is the model whose
	// automations list holds it. A link built from the trigger's filter opens Knight and highlights nothing.
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(CrossLinkTestAutomation(TEXT("goblin_tick"), TEXT("regen"), TEXT("Goblin")));

	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(CrossLinkTestTrigger(TEXT("goblin_tick"), TEXT("Knight"), TEXT("hp")));

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), {}, Automations, Triggers);

	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Automation, TEXT("Goblin"), TEXT("goblin_tick"));
	if (!Link)
	{
		AddError(TEXT("the automation the trigger fires is not linked on the model that lists it"));
		return false;
	}
	TestEqual(TEXT("the relationship is the wait"), Link->Relation, FString(TEXT("waits on it")));
	TestTrue(TEXT("the link can be followed"), Link->bNavigable);
	TestTrue(TEXT("the trigger's filter model is not the target"),
		CrossLinkTestFind(Links, ECrowdyModelRowKind::Automation, TEXT("Knight"), TEXT("goblin_tick")) == nullptr);

	// The same rule on the function side: a trigger naming a function links to the automation on the automation's
	// own model, not on whatever the trigger filters.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> TakeDamage = CrossLinkTestFunction(TEXT("Knight"), TEXT("take_damage"));
	Functions.Add(TakeDamage);
	Triggers[0]->FunctionName = TEXT("take_damage");

	const TArray<FCrowdyModelLink> FunctionLinks =
		CrowdyModelCrossLinks::ForFunction(*TakeDamage, Functions, Automations, Triggers);
	TestTrue(TEXT("the trigger's automation is linked on its own model"),
		CrossLinkTestFind(FunctionLinks, ECrowdyModelRowKind::Automation, TEXT("Goblin"), TEXT("goblin_tick"))
			!= nullptr);
	TestTrue(TEXT("the trigger's filter model is not the target here either"),
		CrossLinkTestFind(FunctionLinks, ECrowdyModelRowKind::Automation, TEXT("Knight"), TEXT("goblin_tick"))
			== nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksShowAnUnreportedAutomationUnnavigableTest,
	"CrowdySDK.CrowdyStudio.CrossLinksShowAnUnreportedAutomationUnnavigable", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksShowAnUnreportedAutomationUnnavigableTest::RunTest(const FString& Parameters)
{
	// The trigger names an automation this app's list does not carry, so nothing can say which model lists it. The
	// line is still shown, because dropping it would hide a real dependency, and it says why it cannot be opened.
	TArray<TSharedPtr<FStudioAutomationTrigger>> Triggers;
	Triggers.Add(CrossLinkTestTrigger(TEXT("goblin_tick"), TEXT("Knight"), TEXT("hp")));

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForAttribute(
		TEXT("Knight"), TEXT("hp"), {}, {}, Triggers);

	TestEqual(TEXT("the trigger is still reported"), Links.Num(), 1);
	if (Links.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("the link names the automation"), Links[0].TargetName, FString(TEXT("goblin_tick")));
	TestTrue(TEXT("no model is guessed at"), Links[0].TargetModel.IsEmpty());
	TestFalse(TEXT("the link cannot be followed"), Links[0].bNavigable);
	TestTrue(TEXT("the line says why"),
		Links[0].Display.Contains(TEXT("cannot be opened"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksAttributeLinkUsesAuthoredNameTest,
	"CrowdySDK.CrowdyStudio.CrossLinksAttributeLinkUsesAuthoredName", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksAttributeLinkUsesAuthoredNameTest::RunTest(const FString& Parameters)
{
	// "hpregen" reconstructs to "Hpregen" with no authored name in scope, so a display of "HPRegen" is reachable
	// only through the map. A key whose synthesized form happened to match the authored one could pass this test
	// by accident, which is exactly what this key is chosen to rule out.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Regen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	CrossLinkTestAddMutation(Regen, TEXT("self"), TEXT("hpregen"), TEXT("self.hpregen + 1"));
	Functions.Add(Regen);

	TMap<FString, FString> AttributeDisplayNames;
	AttributeDisplayNames.Add(TEXT("hpregen"), TEXT("HPRegen"));

	const TArray<FCrowdyModelLink> Links =
		CrowdyModelCrossLinks::ForFunction(*Regen, Functions, {}, {}, AttributeDisplayNames);

	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Attribute, TEXT("Knight"), TEXT("hpregen"));
	if (!Link)
	{
		AddError(TEXT("the attribute the function writes is not in the links"));
		return false;
	}

	// TargetName is identity: navigation opens the row by it, so it must stay the raw server key even though the
	// map supplies a different spelling to show.
	TestEqual(TEXT("the target name stays the raw server key"), Link->TargetName, FString(TEXT("hpregen")));
	TestTrue(TEXT("the display uses the authored spelling"),
		Link->Display.Contains(TEXT("HPRegen"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksAttributeLinkFallsBackWithNoMapEntryTest,
	"CrowdySDK.CrowdyStudio.CrossLinksAttributeLinkFallsBackWithNoMapEntry", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksAttributeLinkFallsBackWithNoMapEntryTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Regen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	CrossLinkTestAddMutation(Regen, TEXT("self"), TEXT("hpregen"), TEXT("self.hpregen + 1"));
	Functions.Add(Regen);

	// The map has entries, just not for this key, so a miss has to fall through to the key reconstructed into
	// words rather than leaving the display blank or borrowing another key's spelling.
	TMap<FString, FString> AttributeDisplayNames;
	AttributeDisplayNames.Add(TEXT("crop_stage"), TEXT("CropStage"));

	const TArray<FCrowdyModelLink> Links =
		CrowdyModelCrossLinks::ForFunction(*Regen, Functions, {}, {}, AttributeDisplayNames);

	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Attribute, TEXT("Knight"), TEXT("hpregen"));
	if (!Link)
	{
		AddError(TEXT("the attribute the function writes is not in the links"));
		return false;
	}

	TestEqual(TEXT("the target name stays the raw server key"), Link->TargetName, FString(TEXT("hpregen")));
	TestTrue(TEXT("the display falls back to the key reconstructed into words"),
		Link->Display.Contains(TEXT("Hpregen"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("the display does not borrow another key's authored spelling"),
		Link->Display.Contains(TEXT("CropStage"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksAuthoredNamesDoNotLeakToOtherLinkKindsTest,
	"CrowdySDK.CrowdyStudio.CrossLinksAuthoredNamesDoNotLeakToOtherLinkKinds", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksAuthoredNamesDoNotLeakToOtherLinkKindsTest::RunTest(const FString& Parameters)
{
	// The transform is meant for Attribute-kind links only. A map keyed by the function's own name makes a leak
	// visible: if it reached the Function or Automation link below, "regen" would resolve through this entry the
	// same way an attribute key does.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> KnightRegen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	Functions.Add(KnightRegen);
	Functions.Add(CrossLinkTestFunction(TEXT("Goblin"), TEXT("regen")));

	TArray<TSharedPtr<FStudioAutomation>> Automations;
	Automations.Add(CrossLinkTestAutomation(TEXT("knight_tick"), TEXT("regen"), TEXT("Knight")));

	TMap<FString, FString> AttributeDisplayNames;
	AttributeDisplayNames.Add(TEXT("regen"), TEXT("Regenerate"));

	const TArray<FCrowdyModelLink> Unmapped =
		CrowdyModelCrossLinks::ForFunction(*KnightRegen, Functions, Automations, {});
	const TArray<FCrowdyModelLink> Mapped =
		CrowdyModelCrossLinks::ForFunction(*KnightRegen, Functions, Automations, {}, AttributeDisplayNames);

	const FCrowdyModelLink* FunctionLinkUnmapped = CrossLinkTestFind(
		Unmapped, ECrowdyModelRowKind::Function, TEXT("Goblin"), TEXT("regen"));
	const FCrowdyModelLink* FunctionLinkMapped = CrossLinkTestFind(
		Mapped, ECrowdyModelRowKind::Function, TEXT("Goblin"), TEXT("regen"));
	const FCrowdyModelLink* AutomationLinkUnmapped = CrossLinkTestFind(
		Unmapped, ECrowdyModelRowKind::Automation, TEXT("Knight"), TEXT("knight_tick"));
	const FCrowdyModelLink* AutomationLinkMapped = CrossLinkTestFind(
		Mapped, ECrowdyModelRowKind::Automation, TEXT("Knight"), TEXT("knight_tick"));

	if (!FunctionLinkUnmapped || !FunctionLinkMapped || !AutomationLinkUnmapped || !AutomationLinkMapped)
	{
		AddError(TEXT("a function or automation link is missing from one of the two runs"));
		return false;
	}

	TestEqual(TEXT("the function link's text is unchanged by the map"),
		FunctionLinkMapped->Display, FunctionLinkUnmapped->Display);
	TestEqual(TEXT("the automation link's text is unchanged by the map"),
		AutomationLinkMapped->Display, AutomationLinkUnmapped->Display);
	TestFalse(TEXT("the function link does not pick up the authored spelling"),
		FunctionLinkMapped->Display.Contains(TEXT("Regenerate"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("the automation link does not pick up the authored spelling"),
		AutomationLinkMapped->Display.Contains(TEXT("Regenerate"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrossLinksDefaultMapLeavesAttributesSynthesizedTest,
	"CrowdySDK.CrowdyStudio.CrossLinksDefaultMapLeavesAttributesSynthesized", CrowdyModelCrossLinksTestFlags)

bool FCrowdyCrossLinksDefaultMapLeavesAttributesSynthesizedTest::RunTest(const FString& Parameters)
{
	// No map argument at all, the same call shape every other test in this file uses. This is the contract that
	// has to hold for them: an attribute link is still shown, on the key reconstructed into words, never blank and
	// never an error, so their behaviour is a defined case rather than an accident of the default.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	const TSharedPtr<FStudioFunction> Regen = CrossLinkTestFunction(TEXT("Knight"), TEXT("regen"));
	CrossLinkTestAddMutation(Regen, TEXT("self"), TEXT("crop_stage"), TEXT("self.crop_stage + 1"));
	Functions.Add(Regen);

	const TArray<FCrowdyModelLink> Links = CrowdyModelCrossLinks::ForFunction(*Regen, Functions, {}, {});

	const FCrowdyModelLink* Link = CrossLinkTestFind(
		Links, ECrowdyModelRowKind::Attribute, TEXT("Knight"), TEXT("crop_stage"));
	if (!Link)
	{
		AddError(TEXT("the attribute the function writes is not in the links"));
		return false;
	}
	TestEqual(TEXT("the target name stays the raw server key"), Link->TargetName, FString(TEXT("crop_stage")));
	TestTrue(TEXT("the display is the key reconstructed into words"),
		Link->Display.Contains(TEXT("CropStage"), ESearchCase::CaseSensitive));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
