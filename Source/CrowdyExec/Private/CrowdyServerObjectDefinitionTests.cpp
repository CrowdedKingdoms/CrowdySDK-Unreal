#include "CrowdyExecTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectDefinitionTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** A tip jar whose State is a List holding Gold. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewTipJar()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("test_tips");
		Definition->StateForm = ECrowdyServerValuesForm::List;
		Definition->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
		return Definition;
	}

	FCrowdyServerFunction& AddFunction(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name)
	{
		FCrowdyServerFunction& Function = Definition.Functions.AddDefaulted_GetRef();
		Function.Name = Name;
		return Function;
	}

	bool BakeOrFail(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const TCHAR* What)
	{
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), What, *FString::Join(Errors, TEXT("; "))), bBaked);
		return bBaked;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	/** The server name the tables give the State List's value called Property; empty when they have none. */
	FString StateServerName(const UCrowdyServerObjectDefinition& Definition, FName Property)
	{
		const FCrowdyExecBakedStruct* State = Definition.BakedStructs.FindByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.List == TEXT("TestTipsState"); });
		const FCrowdyExecBakedField* Field = State ? State->Fields.FindByPredicate([Property](const FCrowdyExecBakedField& Candidate) { return Candidate.Property == Property; }) : nullptr;
		return Field ? Field->ServerName : FString();
	}

	/** The tip jar with Silver beside Gold, and Gold renamed Coins; GoldId is the renamed value's id. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewRenamedTipJar(FGuid& OutGoldId)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewTipJar();
		Definition->StateList.AddProperty(TEXT("Silver"), EPropertyBagPropertyType::Int32);
		const FPropertyBagPropertyDesc* Gold = Definition->StateList.FindPropertyDescByName(TEXT("Gold"));
		OutGoldId = Gold ? Gold->ID : FGuid();
		Definition->StateList.RenameProperty(TEXT("Gold"), TEXT("Coins"));
		return Definition;
	}

	/** The List value called Name in Values, or -1 when there is none. */
	int32 IntOf(const FInstancedStruct& Values, FName Name)
	{
		const TValueOrError<int32, EPropertyBagResult> Value = CrowdyExec::ToList(Values).GetValueInt32(Name);
		return Value.HasValue() ? Value.GetValue() : -1;
	}

	/** A tip jar with Gold, Silver and Bronze, all visible to players, that keeps a record per entity; Gold is owner only and Silver team only. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewRecordsJar()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewTipJar();
		Definition->StateList.AddProperty(TEXT("Silver"), EPropertyBagPropertyType::Int32);
		Definition->StateList.AddProperty(TEXT("Bronze"), EPropertyBagPropertyType::Int32);
		Definition->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Definition->WatchedFields = {TEXT("Gold"), TEXT("Silver"), TEXT("Bronze")};
		Definition->OwnerFields = {TEXT("Gold")};
		Definition->TeamFields = {TEXT("Silver")};
		return Definition;
	}

	bool HasError(const TArray<FString>& Errors, const TCHAR* Part)
	{
		return Errors.ContainsByPredicate([Part](const FString& Error) { return Error.Contains(Part); });
	}

	TOptional<ECrowdyServerFieldAudience> BakedAudience(const UCrowdyServerObjectDefinition& Definition, FName Property)
	{
		const FCrowdyExecBakedStruct* State = Definition.BakedStructs.FindByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.List == TEXT("TestTipsState"); });
		const FCrowdyExecBakedField* Field = State ? State->Fields.FindByPredicate([Property](const FCrowdyExecBakedField& Candidate) { return Candidate.Property == Property; }) : nullptr;
		return Field ? TOptional<ECrowdyServerFieldAudience>(Field->Audience) : TOptional<ECrowdyServerFieldAudience>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDefinitionMethodNamesTest, "CrowdySDK.CrowdyExec.DefinitionBakesMethodNames", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectDefinitionMethodNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewTipJar();
	AddFunction(*Definition, TEXT("AddTip"));
	AddFunction(*Definition, TEXT("GiveTip")).ServerName = TEXT("hand_over");
	TestTrue(TEXT("an unbaked function has no baked method"), Definition->Functions[0].BakedMethodName.IsEmpty());
	TestEqualSensitive(TEXT("and works its method out from its name"), *Definition->Functions[0].GetMethodName(), TEXT("add_tip"));
	if (!BakeOrFail(*this, *Definition, TEXT("the tip jar")))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a bake writes down the name in snake case"), *Definition->Functions[0].BakedMethodName, TEXT("add_tip"));
	TestEqualSensitive(TEXT("or the server name"), *Definition->Functions[1].BakedMethodName, TEXT("hand_over"));

	Definition->Functions[0].Name = TEXT("Tip");
	TestEqualSensitive(TEXT("the baked method is sent, not one worked out from how the name reads now"), *Definition->Functions[0].GetMethodName(), TEXT("add_tip"));
	if (!BakeOrFail(*this, *Definition, TEXT("the renamed tip jar")))
	{
		return false;
	}
	TestEqualSensitive(TEXT("baking again writes down the new name"), *Definition->Functions[0].GetMethodName(), TEXT("tip"));

#if WITH_EDITOR
	Definition->Functions[0].Name = TEXT("Read");
	TArray<FCrowdyExecBakedStruct> Structs;
	TArray<FCrowdyExecBakedEnum> Enums;
	TArray<FString> Errors;
	Definition->BuildTables(Structs, Enums, Errors);
	TestTrue(FString::Printf(TEXT("checks judge the name as it is now, not the baked one (got: %s)"), *FString::Join(Errors, TEXT(" | "))),
		Errors.Contains(TEXT("Function Read: 'read' is reserved; rename the function or set its Server Name")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDefinitionCopiedListsTest, "CrowdySDK.CrowdyExec.DefinitionCopiedFunctionKeepsItsOwnValues", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectDefinitionCopiedListsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewTipJar();
	FCrowdyServerFunction& Tip = AddFunction(*Definition, TEXT("Tip"));
	Tip.ParamsForm = ECrowdyServerValuesForm::List;
	Tip.ReplyForm = ECrowdyServerValuesForm::List;
	Tip.ParamsList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	Tip.ParamsList.SetValueInt32(TEXT("Amount"), 5);
	Tip.ReplyList.AddProperty(TEXT("Total"), EPropertyBagPropertyType::Int32);
	Tip.ReplyList.SetValueInt32(TEXT("Total"), 50);
	const FCrowdyServerFunction Original = Definition->Functions[0];
	FCrowdyServerFunction& Copy = Definition->Functions.Add_GetRef(Original);
	Copy.Name = TEXT("TipCopy");
	Copy.ParamsList.SetValueInt32(TEXT("Amount"), 9);
	Copy.ReplyList.SetValueInt32(TEXT("Total"), 90);
	if (!BakeOrFail(*this, *Definition, TEXT("the tip jar with a copied function")))
	{
		return false;
	}
	const FCrowdyServerFunction& First = Definition->Functions[0];
	const FCrowdyServerFunction& Second = Definition->Functions[1];
	TestTrue(TEXT("Lists of one shape share one struct"), First.GetParamsStruct() == Second.GetParamsStruct() && First.GetReplyStruct() == Second.GetReplyStruct());

	TestEqual(TEXT("Make Inputs gives the original its own defaults"), IntOf(UCrowdyServerObjectLibrary::MakeFunctionInputs(Definition.Get(), TEXT("Tip")), TEXT("Amount")), 5);
	TestEqual(TEXT("and the copy its own"), IntOf(UCrowdyServerObjectLibrary::MakeFunctionInputs(Definition.Get(), TEXT("TipCopy")), TEXT("Amount")), 9);
	FInstancedStruct Reply;
	Second.InitializeReply(Reply);
	TestEqual(TEXT("the copy's outputs start from its own values"), IntOf(Reply, TEXT("Total")), 90);

	const uint8 EmptyMap[] = {0x80};
	FInstancedStruct Decoded;
	Decoded.InitializeAs(Second.GetReplyStruct());
	FString Error;
	TestTrue(FString::Printf(TEXT("an empty reply decodes (%s)"), *Error),
		CrowdyExec::Decode(*Definition, *Second.GetReplyStruct(), MakeArrayView(EmptyMap), Decoded.GetMutableMemory(), Error, Reply.GetMemory()));
	TestEqual(TEXT("and a value it leaves out takes the called function's own value"), IntOf(Decoded, TEXT("Total")), 90);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDefinitionListValueNamesTest, "CrowdySDK.CrowdyExec.DefinitionListValueNames", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectDefinitionListValueNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	FGuid GoldId;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRenamedTipJar(GoldId);
	if (!TestTrue(TEXT("Gold has an id"), GoldId.IsValid()))
	{
		return false;
	}
	FCrowdyServerListValueName& Entry = Definition->ListValueNames.AddDefaulted_GetRef();
	Entry.List = TEXT("TestTipsState");
	Entry.ValueId = GoldId;
	Entry.ServerName = TEXT("Gold");
	if (!BakeOrFail(*this, *Definition, TEXT("the tip jar keeping Gold")))
	{
		return false;
	}
	TestEqualSensitive(TEXT("an entry keeps a renamed value's server name"), *StateServerName(*Definition, TEXT("Coins")), TEXT("Gold"));

	Definition->ListValueNames[0].List = TEXT("TestTipsStateOld");
	BakeOrFail(*this, *Definition, TEXT("the tip jar with an entry for another List"));
	TestEqualSensitive(TEXT("an entry for another List does not apply"), *StateServerName(*Definition, TEXT("Coins")), TEXT("Coins"));

	Definition->ListValueNames[0].List = TEXT("TestTipsState");
	Definition->ListValueNames[0].ValueId = FGuid::NewGuid();
	BakeOrFail(*this, *Definition, TEXT("the tip jar with an entry for another value"));
	TestEqualSensitive(TEXT("nor one for another value"), *StateServerName(*Definition, TEXT("Coins")), TEXT("Coins"));

	Definition->ListValueNames[0].ValueId = GoldId;
	Definition->ListValueNames[0].ServerName = TEXT("Silver");
	TestTrue(TEXT("a kept name another value has fails the bake as a field's would"),
		BakeErrors(*Definition).Contains(TEXT("TestTipsState.Silver: another field of TestTipsState is also named 'Silver'; server names must differ in more than case")));
	Definition->ListValueNames[0].ServerName = TEXT("9Gold");
	TestTrue(TEXT("and so does a name the server cannot use"), BakeErrors(*Definition).Contains(
		TEXT("TestTipsState.Coins: '9Gold' cannot be its server name, which must be letters, digits and underscores, not start with a digit, and be at most 64 characters; set one under Server Names")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsBakeFieldAudienceTest, "CrowdySDK.CrowdyExec.ServerObjectRecordsBakeFieldAudience", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectRecordsBakeFieldAudienceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
	if (!BakeOrFail(*this, *Definition, TEXT("the records jar")))
	{
		return false;
	}
	TestTrue(TEXT("an owner-only variable bakes Owner"), BakedAudience(*Definition, TEXT("Gold")) == ECrowdyServerFieldAudience::Owner);
	TestTrue(TEXT("a team-only variable bakes Team"), BakedAudience(*Definition, TEXT("Silver")) == ECrowdyServerFieldAudience::Team);
	TestTrue(TEXT("any other visible variable bakes Everyone"), BakedAudience(*Definition, TEXT("Bronze")) == ECrowdyServerFieldAudience::Everyone);

	Definition->Holds = ECrowdyServerObjectHolds::OneSet;
	if (!BakeOrFail(*this, *Definition, TEXT("the same lists holding one set")))
	{
		return false;
	}
	TestTrue(TEXT("one set ignores the owner list"), BakedAudience(*Definition, TEXT("Gold")) == ECrowdyServerFieldAudience::Everyone);
	TestTrue(TEXT("and the team list"), BakedAudience(*Definition, TEXT("Silver")) == ECrowdyServerFieldAudience::Everyone);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsRulesTest, "CrowdySDK.CrowdyExec.ServerObjectRecordsRules", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectRecordsRulesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
	TestTrue(TEXT("a valid records definition bakes"), BakeErrors(*Definition).IsEmpty());

	Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	TestTrue(TEXT("Readable By Owner Only is refused"), HasError(BakeErrors(*Definition), TEXT("A Record per Entity needs Readable By Every Player")));
	Definition->Visibility = ECrowdyServerObjectVisibility::Members;
	TestTrue(TEXT("and so is Members"), HasError(BakeErrors(*Definition), TEXT("A Record per Entity needs Readable By Every Player")));
	Definition->Visibility = ECrowdyServerObjectVisibility::Public;

	Definition->MaxRecords = 0;
	TestTrue(TEXT("Max Records 0 is refused"), HasError(BakeErrors(*Definition), TEXT("Max Records is 0; it must be from 1 to 4096")));
	Definition->MaxRecords = 4097;
	TestTrue(TEXT("Max Records 4097 is refused"), HasError(BakeErrors(*Definition), TEXT("Max Records is 4097; it must be from 1 to 4096")));
	Definition->MaxRecords = 4096;
	TestTrue(TEXT("Max Records 4096 is allowed"), BakeErrors(*Definition).IsEmpty());

	Definition->PushIntervalMs = 49;
	TestTrue(TEXT("Send Changes Every 49 is refused"), HasError(BakeErrors(*Definition), TEXT("Send Changes Every is 49 ms; it must be from 50 to 5000 ms")));
	Definition->PushIntervalMs = 5001;
	TestTrue(TEXT("Send Changes Every 5001 is refused"), HasError(BakeErrors(*Definition), TEXT("Send Changes Every is 5001 ms; it must be from 50 to 5000 ms")));
	Definition->PushIntervalMs = 50;
	TestTrue(TEXT("Send Changes Every 50 is allowed"), BakeErrors(*Definition).IsEmpty());

	Definition->TeamFields.Add(TEXT("Gold"));
	TestTrue(TEXT("a variable in both lists is refused"), HasError(BakeErrors(*Definition), TEXT("'Gold' is both Owner Only and Team Only; choose one")));
	Definition->TeamFields = {TEXT("Silver")};

	Definition->WatchedFields.Remove(TEXT("Gold"));
	TestTrue(TEXT("an owner-only variable that is not visible to players is refused"), HasError(BakeErrors(*Definition), TEXT("Owner Only variable 'Gold' is not Visible to Players; choose Owner Only for it again")));
	Definition->WatchedFields.Add(TEXT("Gold"));

	Definition->OwnerFields.Add(TEXT("Platinum"));
	TestTrue(TEXT("an owner-only name that is not a field is refused"), HasError(BakeErrors(*Definition), TEXT("Owner Only variable 'Platinum' is not a field of")));
	Definition->OwnerFields = {TEXT("Gold")};
	Definition->TeamFields.Add(TEXT("Platinum"));
	TestTrue(TEXT("and so is a team-only one"), HasError(BakeErrors(*Definition), TEXT("Team Only variable 'Platinum' is not a field of")));
	Definition->TeamFields = {TEXT("Silver")};
	TestTrue(TEXT("the restored definition bakes"), BakeErrors(*Definition).IsEmpty());

	Definition->Holds = ECrowdyServerObjectHolds::OneSet;
	Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	Definition->MaxRecords = 0;
	Definition->PushIntervalMs = 0;
	Definition->OwnerFields = {TEXT("Platinum")};
	Definition->TeamFields = {TEXT("Platinum")};
	const TArray<FString> Errors = BakeErrors(*Definition);
	TestFalse(TEXT("one set refuses nothing about records"), HasError(Errors, TEXT("Record")) || HasError(Errors, TEXT("Max Records")) || HasError(Errors, TEXT("Send Changes Every")) || HasError(Errors, TEXT("Platinum")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAreaRulesTest, "CrowdySDK.CrowdyExec.ServerObjectAreaRules", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectAreaRulesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	for (const ECrowdyServerObjectHolds Holds : {ECrowdyServerObjectHolds::OneSet, ECrowdyServerObjectHolds::RecordPerEntity})
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewTipJar();
		Definition->Holds = Holds;
		TestTrue(TEXT("the default areas bake"), BakeErrors(*Definition).IsEmpty());

		Definition->AreaSizeChunks = 0;
		TestTrue(TEXT("Area Size 0 is refused"), HasError(BakeErrors(*Definition), TEXT("Area Size is 0; it must be from 1 to 64")));
		Definition->AreaSizeChunks = 65;
		TestTrue(TEXT("Area Size 65 is refused"), HasError(BakeErrors(*Definition), TEXT("Area Size is 65; it must be from 1 to 64")));
		Definition->AreaSizeChunks = 64;
		TestTrue(TEXT("Area Size 64 is allowed"), BakeErrors(*Definition).IsEmpty());

		Definition->InterestRadiusAreas = -1;
		TestTrue(TEXT("a radius of -1 is refused"), HasError(BakeErrors(*Definition), TEXT("Areas Followed Around a Player is -1; it must be from 0 to 3")));
		Definition->InterestRadiusAreas = 4;
		TestTrue(TEXT("a radius of 4 is refused"), HasError(BakeErrors(*Definition), TEXT("Areas Followed Around a Player is 4; it must be from 0 to 3")));
		Definition->InterestRadiusAreas = 0;
		TestTrue(TEXT("a radius of 0 is allowed"), BakeErrors(*Definition).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAreaKeyFloorsTest, "CrowdySDK.CrowdyExec.ServerObjectAreaKeyFloors", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectAreaKeyFloorsTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("the origin"), *UCrowdyServerObjectDefinition::MakeAreaKey(0, 0, 4), TEXT("0,0"));
	TestEqualSensitive(TEXT("the far corner of the first area"), *UCrowdyServerObjectDefinition::MakeAreaKey(3, 3, 4), TEXT("0,0"));
	TestEqualSensitive(TEXT("the next area along x, one below in y"), *UCrowdyServerObjectDefinition::MakeAreaKey(4, -1, 4), TEXT("1,-1"));
	TestEqualSensitive(TEXT("an exact multiple below zero"), *UCrowdyServerObjectDefinition::MakeAreaKey(-4, -5, 4), TEXT("-1,-2"));
	TestEqualSensitive(TEXT("negatives round down, not toward zero"), *UCrowdyServerObjectDefinition::MakeAreaKey(7, -8, 4), TEXT("1,-2"));
	TestEqualSensitive(TEXT("a size below 1 counts as 1"), *UCrowdyServerObjectDefinition::MakeAreaKey(5, -5, 0), TEXT("5,-5"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = CrowdyServerObjectDefinitionTests::NewTipJar();
	Definition->AreaSizeChunks = 8;
	TestEqualSensitive(TEXT("an instance uses its own Area Size"), *Definition->GetAreaKey(9, -1), TEXT("1,-1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsRefuseMembersTest, "CrowdySDK.CrowdyExec.ServerObjectRecordsRefuseMembersFrom", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectRecordsRefuseMembersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	const TCHAR* const Refusal = TEXT("A Record per Entity cannot use Members From; each record carries its own owner and team");
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestTrue(TEXT("members kept by the object are refused"), BakeErrors(*Definition).Contains(Refusal));
	Definition->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestTrue(TEXT("and so are a Crowdy Team's"), BakeErrors(*Definition).Contains(Refusal));
	Definition->MembersFrom = ECrowdyServerMembersSource::None;
	TestTrue(TEXT("no members bakes"), BakeErrors(*Definition).IsEmpty());

	Definition->Holds = ECrowdyServerObjectHolds::OneSet;
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestFalse(TEXT("one set keeps its members"), BakeErrors(*Definition).Contains(Refusal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsTeamFromTest, "CrowdySDK.CrowdyExec.ServerObjectRecordsTeamFrom", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectRecordsTeamFromTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
	TestEqual(TEXT("a record's team is its own by default"), Definition->TeamFrom, ECrowdyServerTeamSource::ThisObjectsRecords);
	TestFalse(TEXT("so its teams are not Crowdy Teams"), Definition->HasCrowdyTeamRecords());
	Definition->TeamFrom = ECrowdyServerTeamSource::CrowdyTeam;
	TestTrue(TEXT("Crowdy Team makes them Crowdy Teams"), Definition->HasCrowdyTeamRecords());
	TestTrue(TEXT("and bakes with nothing more"), BakeErrors(*Definition).IsEmpty());
	Definition->Holds = ECrowdyServerObjectHolds::OneSet;
	TestFalse(TEXT("one set has no records to take teams from"), Definition->HasCrowdyTeamRecords());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsReserveChangesTest, "CrowdySDK.CrowdyExec.ServerObjectRecordsReserveChanges", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectRecordsReserveChangesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	const TCHAR* const Refusal = TEXT("Function Changes: 'changes' is reserved in a Record per Entity; rename the function");
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
	AddFunction(*Definition, TEXT("Changes"));
	TestTrue(TEXT("a function named changes is refused"), BakeErrors(*Definition).Contains(Refusal));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Renamed = NewRecordsJar();
	AddFunction(*Renamed, TEXT("Changes")).ServerName = TEXT("list_changes");
	TestTrue(TEXT("under another server name it bakes"), BakeErrors(*Renamed).IsEmpty());

	TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = NewRecordsJar();
	OneSet->Holds = ECrowdyServerObjectHolds::OneSet;
	AddFunction(*OneSet, TEXT("Changes"));
	TestFalse(TEXT("one set has no changes method to keep"), BakeErrors(*OneSet).Contains(Refusal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFlushTimerReservedTest, "CrowdySDK.CrowdyExec.ServerObjectFlushTimerIsReserved", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectFlushTimerReservedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	for (const ECrowdyServerObjectHolds Holds : {ECrowdyServerObjectHolds::OneSet, ECrowdyServerObjectHolds::RecordPerEntity})
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRecordsJar();
		Definition->Holds = Holds;
		FCrowdyServerTimer& Timer = Definition->Timers.AddDefaulted_GetRef();
		Timer.Name = TEXT("crowdy_flush");
		TestTrue(TEXT("a timer named crowdy_flush is refused"), BakeErrors(*Definition).Contains(TEXT("Timers: crowdy_flush: 'crowdy_flush' is reserved in the server code; rename the timer")));
		Timer.Name = TEXT("CrowdyFlush");
		TestTrue(TEXT("and so is one whose function would be crowdy_flush"), BakeErrors(*Definition).Contains(TEXT("Timers: CrowdyFlush: 'crowdy_flush' is reserved in the server code; rename the timer")));
		Timer.Name = TEXT("Flush");
		TestTrue(TEXT("another name bakes"), BakeErrors(*Definition).IsEmpty());
	}
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDefinitionKeepListValueNameTest, "CrowdySDK.CrowdyExec.DefinitionKeepListValueServerName", CrowdyServerObjectDefinitionTests::TestFlags)
bool FCrowdyServerObjectDefinitionKeepListValueNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectDefinitionTests;
	FGuid GoldId;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewRenamedTipJar(GoldId);
	Definition->KeepListValueServerName(TEXT("TestTipsState"), GoldId, TEXT("Gold"));
	TestEqual(TEXT("keeping a name adds one entry"), Definition->ListValueNames.Num(), 1);
	TestEqualSensitive(TEXT("and bakes it"), *StateServerName(*Definition, TEXT("Coins")), TEXT("Gold"));

	Definition->KeepListValueServerName(TEXT("TestTipsState"), GoldId, TEXT("Treasure"));
	TestEqual(TEXT("keeping another name for the same value updates that entry"), Definition->ListValueNames.Num(), 1);
	TestEqualSensitive(TEXT("to the new name"), *Definition->ListValueNames[0].ServerName, TEXT("Treasure"));
	TestEqualSensitive(TEXT("and bakes it"), *StateServerName(*Definition, TEXT("Coins")), TEXT("Treasure"));
	return true;
}
#endif

#endif
