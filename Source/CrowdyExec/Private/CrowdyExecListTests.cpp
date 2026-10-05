#include "CrowdyExecTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyExecListTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The contract's read fixture: Health 0, bDefeated true, Phase Final.
	const TCHAR* const ReadFixtureHex = TEXT("84a8636f6e747261637401a565706f6368cf000001998c91f600a373657107a66669656c647383a64865616c746800a9624465666561746564c3a55068617365a546696e616c");

	FString HexOf(TConstArrayView<uint8> Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
	}

	TArray<uint8> BytesOf(const FString& Hex)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Hex.Len() / 2);
		HexToBytes(Hex, Bytes.GetData());
		return Bytes;
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("test_boss");
		Definition->WatchedFields = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
		return Definition;
	}

	/** The boss with structs: FCrowdyExecTestBossState, and Hit sending and replying FCrowdyExecTestHit. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeStructDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewDefinition();
		Definition->State = FCrowdyExecTestBossState::StaticStruct();
		FCrowdyServerFunction& Hit = Definition->Functions.AddDefaulted_GetRef();
		Hit.Name = TEXT("Hit");
		Hit.Params = FCrowdyExecTestHit::StaticStruct();
		Hit.Reply = FCrowdyExecTestHit::StaticStruct();
		return Definition;
	}

	void AddHitValues(FInstancedPropertyBag& List)
	{
		List.AddProperty(TEXT("Damage"), EPropertyBagPropertyType::Int32);
		List.AddProperty(TEXT("Source"), EPropertyBagPropertyType::Name);
	}

	/** The same boss with Lists holding the same values; Secret starts at 7 and Health at 4000. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeListDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewDefinition();
		Definition->StateForm = ECrowdyServerValuesForm::List;
		FInstancedPropertyBag& State = Definition->StateList;
		State.AddProperty(TEXT("Health"), EPropertyBagPropertyType::Int32);
		State.AddProperty(TEXT("bDefeated"), EPropertyBagPropertyType::Bool);
		State.AddProperty(TEXT("Phase"), EPropertyBagPropertyType::Enum, StaticEnum<ECrowdyExecTestPhase>());
		State.AddProperty(TEXT("Secret"), EPropertyBagPropertyType::Int32);
		State.SetValueInt32(TEXT("Health"), 4000);
		State.SetValueInt32(TEXT("Secret"), 7);
		FCrowdyServerFunction& Hit = Definition->Functions.AddDefaulted_GetRef();
		Hit.Name = TEXT("Hit");
		Hit.ParamsForm = ECrowdyServerValuesForm::List;
		Hit.ReplyForm = ECrowdyServerValuesForm::List;
		AddHitValues(Hit.ParamsList);
		AddHitValues(Hit.ReplyList);
		return Definition;
	}

	bool BakeOrFail(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		Test.TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
		return bBaked;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	TArray<uint8> EncodeList(FAutomationTestBase& Test, const UCrowdyServerObjectDefinition& Definition, const FInstancedPropertyBag& List,
		TConstArrayView<FString> OnlyFields = {})
	{
		TArray<uint8> Bytes;
		FString Error;
		Test.TestTrue(FString::Printf(TEXT("the List encodes (%s)"), *Error),
			CrowdyExec::Encode(Definition, List.GetPropertyBagStruct(), List.GetValue().GetMemory(), Bytes, Error, OnlyFields));
		return Bytes;
	}

	template <typename T>
	TArray<uint8> EncodeStruct(FAutomationTestBase& Test, const UCrowdyServerObjectDefinition& Definition, const T& Value, TConstArrayView<FString> OnlyFields = {})
	{
		TArray<uint8> Bytes;
		FString Error;
		Test.TestTrue(FString::Printf(TEXT("%s encodes (%s)"), *T::StaticStruct()->GetName(), *Error),
			CrowdyExec::Encode(Definition, T::StaticStruct(), &Value, Bytes, Error, OnlyFields));
		return Bytes;
	}

	/** A List value, or Missing when it cannot be read, so a regression fails the test instead of stopping the run. */
	template <typename T>
	T Or(const TValueOrError<T, EPropertyBagResult>& Value, T Missing)
	{
		return Value.HasValue() ? Value.GetValue() : Missing;
	}

	FInstancedPropertyBag HitList(const FInstancedPropertyBag& Defaults, FName DamageName, int32 Damage, FName Source)
	{
		FInstancedPropertyBag List = Defaults;
		List.SetValueInt32(DamageName, Damage);
		List.SetValueName(TEXT("Source"), Source);
		return List;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecListEncodesLikeStructTest, "CrowdySDK.CrowdyExec.ListEncodesLikeTheStruct", CrowdyExecListTests::TestFlags)
bool FCrowdyExecListEncodesLikeStructTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecListTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Structs = MakeStructDefinition();
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Lists = MakeListDefinition();
	if (!BakeOrFail(*this, *Structs) || !BakeOrFail(*this, *Lists))
	{
		return false;
	}
	const FCrowdyServerFunction& Hit = Lists->Functions[0];

	FCrowdyExecTestHit StructHit;
	StructHit.Damage = 25;
	StructHit.Source = TEXT("Sword");
	const TArray<uint8> StructBytes = EncodeStruct(*this, *Structs, StructHit);
	TestEqual(TEXT("params sent as a List are byte for byte the struct's"),
		HexOf(EncodeList(*this, *Lists, HitList(Hit.ParamsList, TEXT("Damage"), 25, TEXT("Sword")))), HexOf(StructBytes));

	FCrowdyExecTestBossState StructState;
	StructState.Health = 0;
	StructState.bDefeated = true;
	StructState.Phase = ECrowdyExecTestPhase::Enraged;
	FInstancedPropertyBag ListState = Lists->StateList;
	ListState.SetValueInt32(TEXT("Health"), 0);
	ListState.SetValueBool(TEXT("bDefeated"), true);
	ListState.SetValueEnum(TEXT("Phase"), ECrowdyExecTestPhase::Enraged);
	const TArray<FString> Watched = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
	TestEqual(TEXT("a List State's watched values are byte for byte the struct's"),
		HexOf(EncodeList(*this, *Lists, ListState, Watched)), HexOf(EncodeStruct(*this, *Structs, StructState, Watched)));

	FInstancedStruct Decoded;
	Decoded.InitializeAs(Hit.GetReplyStruct());
	FString Error;
	TestTrue(FString::Printf(TEXT("the struct's bytes decode into the reply List (%s)"), *Error),
		CrowdyExec::Decode(*Lists, *Hit.GetReplyStruct(), StructBytes, Decoded.GetMutableMemory(), Error));
	const FInstancedPropertyBag Reply = CrowdyExec::ToList(Decoded);
	TestTrue(TEXT("and read back by name"), Or(Reply.GetValueInt32(TEXT("Damage")), -1) == 25
		&& Or(Reply.GetValueName(TEXT("Source")), FName()) == FName(TEXT("Sword")));

	Lists->Functions[0].ParamsList.SetValueInt32(TEXT("Damage"), 3);
	FCrowdyExecTestHit SourceOnly;
	SourceOnly.Source = TEXT("Axe");
	TestTrue(TEXT("a message without Damage decodes"), CrowdyExec::Decode(*Lists, *Hit.GetReplyStruct(),
		EncodeStruct(*this, *Structs, SourceOnly, {TEXT("Source")}), Decoded.GetMutableMemory(), Error));
	TestEqual(TEXT("and a value it does not carry takes the List's starting value"), Or(CrowdyExec::ToList(Decoded).GetValueInt32(TEXT("Damage")), -1), 3);

	FInstancedStruct State;
	Lists->InitializeValues(Lists->GetStateStruct(), State);
	TestEqual(TEXT("a new State starts from the List's values"), Or(CrowdyExec::ToList(State).GetValueInt32(TEXT("Health")), -1), 4000);
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	TestTrue(FString::Printf(TEXT("the contract's read fixture decodes into a List State (%s)"), *Error),
		CrowdyExec::DecodeState(*Lists, ECrowdyExecStateMessage::Read, BytesOf(ReadFixtureHex), State.GetMutableMemory(), Header, Fields, Error));
	const FInstancedPropertyBag Read = CrowdyExec::ToList(State);
	TestTrue(TEXT("with the fixture's values"), Or(Read.GetValueInt32(TEXT("Health")), -1) == 0 && Or(Read.GetValueBool(TEXT("bDefeated")), false)
		&& Or(Read.GetValueEnum<ECrowdyExecTestPhase>(TEXT("Phase")), ECrowdyExecTestPhase::Calm) == ECrowdyExecTestPhase::Final);
	TestEqual(TEXT("a value players never see keeps the List's starting value"), Or(Read.GetValueInt32(TEXT("Secret")), -1), 7);
	TestTrue(TEXT("a struct State reads no List"), CrowdyExec::ToList(FInstancedStruct::Make(StructState)).GetPropertyBagStruct() == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecListBakedByIdTest, "CrowdySDK.CrowdyExec.ListIsBakedByIdAndFoundByName", CrowdyExecListTests::TestFlags)
bool FCrowdyExecListBakedByIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecListTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Lists = MakeListDefinition();
	if (!BakeOrFail(*this, *Lists))
	{
		return false;
	}
	TArray<FString> Names;
	for (const FCrowdyExecBakedStruct& Baked : Lists->BakedStructs)
	{
		TestNull(FString::Printf(TEXT("%s keeps no struct, which is made afresh on every load"), *Baked.List), Baked.Struct.Get());
		Names.Add(Baked.List);
	}
	TestTrue(TEXT("each List is found by its name"), Names == TArray<FString>({TEXT("TestBossState"), TEXT("HitParams")}));
	FCrowdyServerFunction& Hit = Lists->Functions[0];
	TestTrue(TEXT("a reply List of the params' shape shares their struct"), Hit.GetReplyStruct() == Hit.GetParamsStruct());

	const TArray<uint8> Before = EncodeList(*this, *Lists, HitList(Hit.ParamsList, TEXT("Damage"), 25, TEXT("Sword")));
	const TArray<FCrowdyExecBakedStruct> Saved = Lists->BakedStructs;
	Hit.ParamsList.RenameProperty(TEXT("Damage"), TEXT("Hurt"));
	Hit.ParamsList.ReorderProperty(TEXT("Source"), TEXT("Hurt"));
	Lists->BakedStructs = Saved;
	CrowdyExec::NotifyStructsChanged();
	TestEqual(TEXT("tables saved before a rename and a move still send the value under its baked name, in baked order"),
		HexOf(EncodeList(*this, *Lists, HitList(Hit.ParamsList, TEXT("Hurt"), 25, TEXT("Sword")))), HexOf(Before));

	if (!BakeOrFail(*this, *Lists))
	{
		return false;
	}
	TestNotNull(TEXT("baking again sends the new name"), CrowdyExec::FindResolvedPropertyForTest(*Lists, *Hit.GetParamsStruct(), TEXT("Hurt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecListChecksTest, "CrowdySDK.CrowdyExec.ListChecks", CrowdyExecListTests::TestFlags)
bool FCrowdyExecListChecksTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecListTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Empty = NewDefinition();
	Empty->StateForm = ECrowdyServerValuesForm::List;
	Empty->WatchedFields.Reset();
	TestTrue(TEXT("an empty State List is refused"), BakeErrors(*Empty).Contains(TEXT("Add at least one variable, or use a State struct")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Text = MakeListDefinition();
	Text->Functions[0].ParamsList.AddProperty(TEXT("Label"), EPropertyBagPropertyType::Text);
	TestTrue(TEXT("a List value of a refused type is named by List and value"),
		BakeErrors(*Text).Contains(TEXT("HitParams.Label: text is not supported; use FString")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Twins = MakeListDefinition();
	const FCrowdyServerFunction Original = Twins->Functions[0];
	FCrowdyServerFunction& Twin = Twins->Functions.Add_GetRef(Original);
	Twin.ServerName = TEXT("hit_again");
	Twin.ParamsList.Reset();
	AddHitValues(Twin.ParamsList);
	TestTrue(TEXT("two Lists of one name are refused"),
		BakeErrors(*Twins).Contains(TEXT("Two lists are named HitParams; give their functions different names")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Nothing = MakeListDefinition();
	Nothing->Functions[0].ParamsList.RemovePropertiesByName({TEXT("Damage"), TEXT("Source")});
	TestNull(TEXT("a params List emptied value by value sends nothing"), Nothing->Functions[0].GetParamsStruct());
	TestTrue(TEXT("and still bakes"), BakeOrFail(*this, *Nothing));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Switched = MakeListDefinition();
	Switched->Functions[0].ParamsForm = ECrowdyServerValuesForm::Struct;
	Switched->Functions[0].Params = FCrowdyExecTestHit::StaticStruct();
	TestTrue(TEXT("a List left behind by switching to Struct gives way to the struct"),
		Switched->Functions[0].GetParamsStruct() == FCrowdyExecTestHit::StaticStruct());

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Cooked = MakeListDefinition();
	TestTrue(TEXT("a List is found whatever case its function's name reads back in"),
		Cooked->FindListStruct(TEXT("hitparams")) == Cooked->Functions[0].GetParamsStruct());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecListStructInputTest, "CrowdySDK.CrowdyExec.ListHoldsAStructInput", CrowdyExecListTests::TestFlags)
bool FCrowdyExecListStructInputTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecListTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Lists = MakeListDefinition();
	FCrowdyServerFunction& Aim = Lists->Functions.AddDefaulted_GetRef();
	Aim.Name = TEXT("Aim");
	Aim.ParamsForm = ECrowdyServerValuesForm::List;
	Aim.ParamsList.AddProperty(TEXT("Target"), EPropertyBagPropertyType::Struct, FCrowdyExecTestHit::StaticStruct());
	if (!BakeOrFail(*this, *Lists))
	{
		return false;
	}
	TestTrue(TEXT("the struct a List input holds is baked beside the List"),
		Lists->BakedStructs.ContainsByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct.Get() == FCrowdyExecTestHit::StaticStruct(); }));

	FCrowdyExecTestHit Target;
	Target.Damage = 7;
	Target.Source = TEXT("Bow");
	FInstancedPropertyBag Inputs = Aim.ParamsList;
	Inputs.SetValueStruct(TEXT("Target"), FConstStructView::Make(Target));

	TArray<uint8> Expected;
	FCrowdyExecWriter Writer(Expected);
	Writer.MapHeader(1);
	Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>("Target"), 6));
	Expected.Append(EncodeStruct(*this, *Lists, Target));
	TestEqual(TEXT("the List sends the struct nested under its input's name"), HexOf(EncodeList(*this, *Lists, Inputs)), HexOf(Expected));
	return true;
}

#endif
