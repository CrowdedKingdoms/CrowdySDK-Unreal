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
		Errors.Contains(TEXT("Function Read: 'read' is reserved")));
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
