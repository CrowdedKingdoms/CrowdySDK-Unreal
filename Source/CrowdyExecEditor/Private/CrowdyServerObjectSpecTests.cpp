#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecEditorTestTypes.h"
#include "CrowdyGameKitStarters.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyServerObjectSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace CrowdyServerObjectSpecTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* const RecordPath = TEXT("/Script/CrowdyExecEditor.CrowdyExecTestRecord");

	using FDefinitionPtr = TStrongObjectPtr<UCrowdyServerObjectDefinition>;

	FDefinitionPtr NewDefinition(const TCHAR* TypeName)
	{
		FDefinitionPtr Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		return Definition;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}

	FString Print(const TSharedRef<FJsonObject>& Json)
	{
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	TArray<UCrowdyServerObjectDefinition*> NoProject()
	{
		return {};
	}

	/** The definition as a saved and loaded List is: rebuilt from its own descs, which carry their Ids, so equal Lists share one struct. */
	void Reload(FInstancedPropertyBag& List)
	{
		if (const UPropertyBag* Bag = List.GetPropertyBagStruct())
		{
			List.MigrateToNewBagStruct(UPropertyBag::GetOrCreateFromDescs(Bag->GetPropertyDescs()));
		}
	}

	bool HasError(const TArray<FString>& Errors, const TCHAR* Part)
	{
		return Errors.ContainsByPredicate([Part](const FString& Each) { return Each.Contains(Part); });
	}

	void ExpectError(FAutomationTestBase& Test, const TArray<FString>& Errors, const TCHAR* Part)
	{
		Test.TestTrue(FString::Printf(TEXT("an error says \"%s\" (got: %s)"), Part, *FString::Join(Errors, TEXT(" | "))), HasError(Errors, Part));
	}

	/** What reading Json into a fresh definition complains of. */
	TArray<FString> ReadErrors(FAutomationTestBase& Test, const TCHAR* Json, TArray<UCrowdyServerObjectDefinition*> Project = {})
	{
		TArray<FString> Errors;
		const TSharedPtr<FJsonObject> Spec = Parse(Json);
		if (!Test.TestTrue(TEXT("the test's JSON parses"), Spec.IsValid()))
		{
			return Errors;
		}
		FDefinitionPtr Out = NewDefinition(TEXT(""));
		CrowdyServerObjectSpec::FromJson(*Spec, *Out, [&Project](const FString& Name) -> UCrowdyServerObjectDefinition*
		{
			UCrowdyServerObjectDefinition* const* Found = Project.FindByPredicate([&Name](const UCrowdyServerObjectDefinition* Each) { return Each->TypeName == Name; });
			return Found ? *Found : nullptr;
		}, Errors);
		return Errors;
	}

	/** Exports Source, reads the text back into a second definition, bakes it, and expects every property and a second export to match. */
	void RoundTrip(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Source, TArray<UCrowdyServerObjectDefinition*> Project = {})
	{
		TArray<FString> Errors;
		const bool bSourceBakes = Source.Bake(Errors);
		if (!Test.TestTrue(FString::Printf(TEXT("the source bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bSourceBakes))
		{
			return;
		}
		const FString First = Print(CrowdyServerObjectSpec::ToJson(Source));
		const TSharedPtr<FJsonObject> Parsed = Parse(First);
		if (!Test.TestTrue(TEXT("the export is JSON"), Parsed.IsValid()))
		{
			return;
		}
		FDefinitionPtr Copy = NewDefinition(TEXT(""));
		Errors.Reset();
		const bool bRead = CrowdyServerObjectSpec::FromJson(*Parsed, *Copy, [&Project](const FString& Name) -> UCrowdyServerObjectDefinition*
		{
			UCrowdyServerObjectDefinition* const* Found = Project.FindByPredicate([&Name](const UCrowdyServerObjectDefinition* Each) { return Each->TypeName == Name; });
			return Found ? *Found : nullptr;
		}, Errors);
		Test.TestTrue(FString::Printf(TEXT("the export reads back (%s)"), *FString::Join(Errors, TEXT("; "))), bRead);
		Errors.Reset();
		const bool bCopyBakes = Copy->Bake(Errors);
		Test.TestTrue(FString::Printf(TEXT("the copy bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bCopyBakes);
		for (TFieldIterator<FProperty> It(UCrowdyServerObjectDefinition::StaticClass(), EFieldIterationFlags::None); It; ++It)
		{
			Test.TestTrue(FString::Printf(TEXT("%s is the same after the round trip"), *It->GetName()), It->Identical_InContainer(&Source, Copy.Get()));
		}
		Test.TestEqual(TEXT("a second export is the same text"), Print(CrowdyServerObjectSpec::ToJson(*Copy)), First);
	}

	/** A struct State with watched fields, struct inputs and outputs, every timer kind, members, scaling and limits, and server names. */
	FDefinitionPtr MakeStructState()
	{
		FDefinitionPtr Definition = NewDefinition(TEXT("round_struct"));
		Definition->Description = TEXT("A type that holds a struct");
		Definition->State = FCrowdyExecTestRecord::StaticStruct();
		Definition->WatchedFields = {TEXT("Health"), TEXT("Label")};
		Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
		Definition->MaxMembers = 8;
		Definition->bRemoveMembersWhoLeave = false;
		Definition->bOnPlayerJoined = true;
		Definition->MemoryMegabytes = 128;
		Definition->CallTimeLimitMs = 500;
		Definition->FuelPerCallMillions = 20;
		Definition->MaxWaitingCalls = 100;
		Definition->SaveIntervalSeconds = 20;
		Definition->IdleTimeoutSeconds = 120;
		Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
		Definition->LogicFile.FilePath = TEXT("Server/round_struct.rs");
		FCrowdyServerFunction& Hit = Definition->Functions.AddDefaulted_GetRef();
		Hit.Name = TEXT("Hit");
		Hit.Params = FCrowdyExecTestRecordHit::StaticStruct();
		Hit.Reply = FCrowdyExecTestRecordHit::StaticStruct();
		Hit.CooldownSeconds = 1.5f;
		Hit.RequiredFeature = TEXT("vip");
		FCrowdyServerFunction& Boot = Definition->Functions.AddDefaulted_GetRef();
		Boot.Name = TEXT("Boot");
		Boot.WhoCanCall = ECrowdyServerFunctionCaller::Leader;
		FCrowdyServerFunction& Refresh = Definition->Functions.AddDefaulted_GetRef();
		Refresh.Name = TEXT("Refresh");
		Refresh.WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
		FCrowdyServerMessage& Hurt = Definition->Messages.AddDefaulted_GetRef();
		Hurt.Name = TEXT("Hurt");
		Hurt.Payload = FCrowdyExecTestRecordHit::StaticStruct();
		FCrowdyServerTimer& Tick = Definition->Timers.AddDefaulted_GetRef();
		Tick.Name = TEXT("tick");
		Tick.Seconds = 30.f;
		Tick.bStartAutomatically = false;
		FCrowdyServerTimer& Nightly = Definition->Timers.AddDefaulted_GetRef();
		Nightly.Name = TEXT("nightly");
		Nightly.Repeat = ECrowdyServerTimerRepeat::Schedule;
		Nightly.Cron = TEXT("0 0 * * *");
		Nightly.UtcOffsetMinutes = 60;
		FCrowdyServerTimer& Once = Definition->Timers.AddDefaulted_GetRef();
		Once.Name = TEXT("once");
		Once.Repeat = ECrowdyServerTimerRepeat::After;
		Once.Seconds = 5.f;
		FCrowdyServerFieldName& Caption = Definition->FieldNames.AddDefaulted_GetRef();
		Caption.Struct = FCrowdyExecTestRecord::StaticStruct();
		Caption.Field = TEXT("Label");
		Caption.ServerName = TEXT("caption");
		FCrowdyServerEnumValueName& Short = Definition->EnumValueNames.AddDefaulted_GetRef();
		Short.Enum = StaticEnum<ECrowdyServerObjectKind>();
		Short.Value = TEXT("ServerObject");
		Short.ServerName = TEXT("so");
		return Definition;
	}

	void AddStateVariables(FInstancedPropertyBag& List)
	{
		List.AddProperties({
			FPropertyBagPropertyDesc(TEXT("Gold"), EPropertyBagPropertyType::Int32),
			FPropertyBagPropertyDesc(TEXT("Label"), EPropertyBagPropertyType::String),
			FPropertyBagPropertyDesc(TEXT("Big"), EPropertyBagPropertyType::Int64),
			FPropertyBagPropertyDesc(TEXT("Ratio"), EPropertyBagPropertyType::Float),
			FPropertyBagPropertyDesc(TEXT("Mode"), EPropertyBagPropertyType::Enum, StaticEnum<ECrowdyServerObjectKind>()),
			FPropertyBagPropertyDesc(TEXT("Spot"), EPropertyBagPropertyType::Struct, TBaseStructure<FVector>::Get()),
			FPropertyBagPropertyDesc(TEXT("Tags"), EPropertyBagContainerType::Array, EPropertyBagPropertyType::String),
			FPropertyBagPropertyDesc(TEXT("Scores"), EPropertyBagContainerType::Map, EPropertyBagPropertyType::Int32, nullptr, CPF_Edit, EPropertyBagPropertyType::String)});
		Reload(List);
		List.SetValueInt32(TEXT("Gold"), 5);
		List.SetValueString(TEXT("Label"), TEXT("hero"));
		List.SetValueInt64(TEXT("Big"), 9007199254740993LL);
		List.SetValueFloat(TEXT("Ratio"), 0.1f);
		List.SetValueEnum(TEXT("Mode"), static_cast<int64>(ECrowdyServerObjectKind::FunctionLibrary), StaticEnum<ECrowdyServerObjectKind>());
		List.SetValueStruct(TEXT("Spot"), FVector(1.0, 2.0, 3.0));
		const FPropertyBagPropertyDesc* Tags = List.FindPropertyDescByName(TEXT("Tags"));
		FScriptArrayHelper TagArray(CastField<FArrayProperty>(Tags->CachedProperty), List.GetMutableValue().GetMemory() + Tags->CachedProperty->GetOffset_ForInternal());
		TagArray.AddValues(2);
		*reinterpret_cast<FString*>(TagArray.GetRawPtr(0)) = TEXT("a");
		*reinterpret_cast<FString*>(TagArray.GetRawPtr(1)) = TEXT("b");
		const FPropertyBagPropertyDesc* Scores = List.FindPropertyDescByName(TEXT("Scores"));
		FScriptMapHelper ScoreMap(CastField<FMapProperty>(Scores->CachedProperty), List.GetMutableValue().GetMemory() + Scores->CachedProperty->GetOffset_ForInternal());
		const int32 First = ScoreMap.AddDefaultValue_Invalid_NeedsRehash();
		*reinterpret_cast<FString*>(ScoreMap.GetKeyPtr(First)) = TEXT("x");
		*reinterpret_cast<int32*>(ScoreMap.GetValuePtr(First)) = 1;
		const int32 Second = ScoreMap.AddDefaultValue_Invalid_NeedsRehash();
		*reinterpret_cast<FString*>(ScoreMap.GetKeyPtr(Second)) = TEXT("y");
		*reinterpret_cast<int32*>(ScoreMap.GetValuePtr(Second)) = 2;
		ScoreMap.Rehash();
	}

	/** A Record per Entity type whose variables, function inputs and outputs and message payload are Lists. */
	FDefinitionPtr MakeListState()
	{
		FDefinitionPtr Definition = NewDefinition(TEXT("round_list"));
		Definition->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Definition->MaxRecords = 100;
		Definition->PushIntervalMs = 100;
		Definition->StateForm = ECrowdyServerValuesForm::List;
		AddStateVariables(Definition->StateList);
		Definition->WatchedFields = {TEXT("Gold"), TEXT("Ratio")};
		Definition->OwnerFields = {TEXT("Gold")};
		Definition->TeamFields = {TEXT("Ratio")};
		FCrowdyServerFunction& Tip = Definition->Functions.AddDefaulted_GetRef();
		Tip.Name = TEXT("AddTip");
		Tip.ParamsForm = ECrowdyServerValuesForm::List;
		Tip.ReplyForm = ECrowdyServerValuesForm::List;
		FPropertyBagPropertyDesc Amount(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		CrowdyServerValueRange::SetBound(Amount, TEXT("ClampMin"), TEXT("0"));
		CrowdyServerValueRange::SetBound(Amount, TEXT("ClampMax"), TEXT("100"));
		Tip.ParamsList.AddProperties({Amount, FPropertyBagPropertyDesc(TEXT("Note"), EPropertyBagPropertyType::String)});
		Tip.ReplyList.AddProperties({FPropertyBagPropertyDesc(TEXT("Total"), EPropertyBagPropertyType::Int64)});
		Reload(Tip.ParamsList);
		Reload(Tip.ReplyList);
		FCrowdyServerMessage& Tipped = Definition->Messages.AddDefaulted_GetRef();
		Tipped.Name = TEXT("Tipped");
		Tipped.PayloadForm = ECrowdyServerValuesForm::List;
		Tipped.PayloadList.AddProperties({FPropertyBagPropertyDesc(TEXT("Who"), EPropertyBagPropertyType::String)});
		Reload(Tipped.PayloadList);
		FCrowdyServerListValueName& Kept = Definition->ListValueNames.AddDefaulted_GetRef();
		Kept.List = TEXT("AddTipParams");
		Kept.ValueId = Tip.ParamsList.FindPropertyDescByName(TEXT("Amount"))->ID;
		Kept.ServerName = TEXT("amt");
		return Definition;
	}

	FDefinitionPtr MakeCandidateFor(const TSharedRef<FJsonObject>& Spec, const UCrowdyServerObjectDefinition& Existing, TArray<FString>& Errors)
	{
		TArray<UCrowdyServerObjectDefinition*> Project;
		Project.Add(const_cast<UCrowdyServerObjectDefinition*>(&Existing));
		return CrowdyServerObjectSpec::MakeEdited(*Spec, Existing, Project, Errors);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecRoundTripStructStateTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.RoundTrip.StructState", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecRoundTripStructStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Definition = MakeStructState();
	RoundTrip(*this, *Definition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecRoundTripListStateTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.RoundTrip.ListState", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecRoundTripListStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Definition = MakeListState();
	RoundTrip(*this, *Definition);
	const TSharedRef<FJsonObject> Spec = CrowdyServerObjectSpec::ToJson(*Definition);
	const TArray<TSharedPtr<FJsonValue>>& Variables = Spec->GetArrayField(TEXT("StateList"));
	FString Big;
	for (const TSharedPtr<FJsonValue>& Variable : Variables)
	{
		if (Variable->AsObject()->GetStringField(TEXT("Name")) == TEXT("Big"))
		{
			Variable->AsObject()->TryGetStringField(TEXT("Default"), Big);
		}
	}
	TestEqual(TEXT("an Int64 default travels as text, so no digit is lost"), Big, FString(TEXT("9007199254740993")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecRoundTripGameKitStartersTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.RoundTrip.GameKitStarters", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecRoundTripGameKitStartersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	for (const CrowdyGameKitStarters::FStarter& Starter : CrowdyGameKitStarters::GetStarters())
	{
		UCrowdyServerObjectDefinition* Made = CrowdyGameKitStarters::MakeDefinition(Starter, GetTransientPackage(), NAME_None, RF_Transient);
		if (!TestNotNull(FString::Printf(TEXT("the %s starter's definition is made and bakes"), Starter.TypeName), Made))
		{
			continue;
		}
		FDefinitionPtr Definition(Made);
		Definition->KitConfig = TEXT("{\"note\": 1}");
		RoundTrip(*this, *Definition);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecCanCallByTypeNameTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.CanCallByTypeName", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecCanCallByTypeNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Other = NewDefinition(TEXT("other_type"));
	Other->State = FCrowdyExecTestRecord::StaticStruct();
	FDefinitionPtr Caller = NewDefinition(TEXT("caller_type"));
	TArray<UCrowdyServerObjectDefinition*> Project = {Other.Get()};
	const TSharedPtr<FJsonObject> Spec = Parse(FString::Printf(TEXT(R"({"TypeName": "caller_type", "State": "%s", "CanCall": ["other_type"], "ListenTo": ["other_type"]})"), RecordPath));
	if (!TestTrue(TEXT("the test's JSON parses"), Spec.IsValid()))
	{
		return false;
	}
	TArray<FString> Errors;
	const FDefinitionPtr Made = CrowdyServerObjectSpec::MakeNew(*Spec, Project, Errors);
	TestTrue(FString::Printf(TEXT("a type named in CanCall and ListenTo resolves (%s)"), *FString::Join(Errors, TEXT("; "))), Made.IsValid());
	if (Made.IsValid())
	{
		TestTrue(TEXT("CanCall holds the project's own definition"), Made->CanCall.Num() == 1 && Made->CanCall[0] == Other.Get());
		TestTrue(TEXT("ListenTo holds the project's own definition"), Made->ListenTo.Num() == 1 && Made->ListenTo[0] == Other.Get());
		TestEqual(TEXT("an export names the type again"), CrowdyServerObjectSpec::ToJson(*Made)->GetArrayField(TEXT("CanCall"))[0]->AsString(), FString(TEXT("other_type")));
	}
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"CanCall": ["missing_type"]})"), Project), TEXT("CanCall[0] is \"missing_type\", which is not a Server Object type of the project"));
	Caller->State = FCrowdyExecTestRecord::StaticStruct();
	TArray<UCrowdyServerObjectDefinition*> WithSelf = {Caller.Get()};
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"TypeName": "Caller_Type", "CanCall": ["caller_type"]})"), WithSelf), TEXT("CanCall[0] names this spec's own TypeName"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"TypeName": "caller_type", "ListenTo": ["caller_type"]})"), WithSelf), TEXT("ListenTo[0] names this spec's own TypeName"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecExportKeysTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.ExportHoldsExactlyTheAuthoredKeys", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecExportKeysTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Definition = NewDefinition(TEXT("template_type"));
	const TSharedRef<FJsonObject> Spec = CrowdyServerObjectSpec::ToJson(*Definition);
	const TArray<FString> Keys = CrowdyServerObjectSpec::GetAuthoredKeys();
	for (const FString& Key : Keys)
	{
		TestTrue(FString::Printf(TEXT("the export holds %s"), *Key), Spec->HasField(Key));
	}
	TestEqual(TEXT("the export holds nothing else"), Spec->Values.Num(), Keys.Num());
	for (const TCHAR* Baked : {TEXT("BakedStructs"), TEXT("BakedEnums"), TEXT("NativeClass")})
	{
		TestFalse(FString::Printf(TEXT("the export leaves out %s"), Baked), Spec->HasField(Baked));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecMissingKeysTakeDefaultsTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.NewTypeKeysLeftOutTakeTheirDefaults", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecMissingKeysTakeDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	const TSharedPtr<FJsonObject> Spec = Parse(FString::Printf(TEXT(R"({"TypeName": "tiny", "State": "%s", "Timers": [{"Name": "tick"}]})"), RecordPath));
	TArray<FString> Errors;
	const FDefinitionPtr Made = CrowdyServerObjectSpec::MakeNew(*Spec, NoProject(), Errors);
	TestTrue(FString::Printf(TEXT("a spec of three keys makes a type (%s)"), *FString::Join(Errors, TEXT("; "))), Made.IsValid());
	if (Made.IsValid())
	{
		TestEqual(TEXT("a setting left out keeps the class default"), Made->MemoryMegabytes, GetDefault<UCrowdyServerObjectDefinition>()->MemoryMegabytes);
		TestEqual(TEXT("a setting left out of a nested object keeps its default"), Made->Timers[0].Seconds, FCrowdyServerTimer().Seconds);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecUnknownKeyTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.UnknownKey", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecUnknownKeyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"NotASetting": 1})")), TEXT("NotASetting is not a setting; the settings are "));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Functions": [{"Name": "Hit"}, {"Speed": 1}]})")), TEXT("Functions[1].Speed is not a setting"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecCaseKeyTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.CaseMismatchedKey", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecCaseKeyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"typename": "x"})")), TEXT("typename is not a setting; it is spelled TypeName"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecBakedKeyTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.BakedKey", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecBakedKeyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"BakedStructs": []})")), TEXT("BakedStructs is filled in by the bake, not authored; leave it out"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Functions": [{"BakedMethodName": "hit"}]})")), TEXT("Functions[0].BakedMethodName is filled in by the bake"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecWrongCaseEnumTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.WrongCaseEnum", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecWrongCaseEnumTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Kind": "functionlibrary"})")), TEXT("Kind is \"functionlibrary\", which is spelled FunctionLibrary"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Kind": "Nonsense"})")), TEXT("Kind needs one of ServerObject, FunctionLibrary but got the text \"Nonsense\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecIntStringTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.IntGivenText", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecIntStringTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"MaxRecords": "5"})")), TEXT("MaxRecords needs a whole number but got the text \"5\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecIntFractionTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.IntGivenFraction", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecIntFractionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"MaxRecords": 2.5})")), TEXT("MaxRecords needs a whole number but got the number 2.5"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecOverClampTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.ValueOutsideItsClamp", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecOverClampTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"MemoryMegabytes": 600})")), TEXT("MemoryMegabytes is 600, outside its range of 1 to 512"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Timers": [{"Seconds": 0}]})")), TEXT("Timers[0].Seconds is 0, outside its range of at least 0.01"));
	TestEqual(TEXT("a value on the limit is taken"), ReadErrors(*this, TEXT(R"({"MemoryMegabytes": 512})")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecBadStructPathTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.BadStructPath", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecBadStructPathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"State": "/Script/CrowdyExec.NoSuchStruct"})")), TEXT("State is \"/Script/CrowdyExec.NoSuchStruct\", which is not a ScriptStruct that can be loaded"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"State": "not a path"})")), TEXT("State is \"not a path\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecBadBagTypeTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.BagTypeTheServerDoesNotTake", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecBadBagTypeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Text"}]})")), TEXT("StateList[0].Type is a type the server does not take"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "None"}]})")), TEXT("StateList[0].Type needs a type such as Int32"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecStructWithoutObjectTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.StructVariableWithoutTypeObject", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecStructWithoutObjectTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Struct"}]})")), TEXT("StateList[0].TypeObject is missing; a Struct variable needs the path of its ScriptStruct"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32", "TypeObject": "/Script/CoreUObject.Vector"}]})")), TEXT("StateList[0].TypeObject is only for Enum, Struct"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecDuplicateVariableTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.DuplicateVariableName", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecDuplicateVariableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32"}, {"Name": "a", "Type": "String"}]})")),
		TEXT("StateList[1].Name is \"a\", which an earlier variable already has"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecWrongKindDefaultTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.DefaultOfTheWrongKind", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecWrongKindDefaultTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "n", "Type": "Int32", "Default": "abc"}]})")), TEXT("StateList[0].Default needs a whole number but got the text \"abc\""));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "n", "Type": "Bool", "Default": 1}]})")), TEXT("StateList[0].Default needs true or false but got the number 1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecKitActorsTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.KitActorThatIsNoActorClass", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecKitActorsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"KitActors": {"mob": "/Script/Engine.Texture2D"}})")), TEXT("KitActors[mob] is \"/Script/Engine.Texture2D\", which is not an Actor class that can be loaded"));
	TestEqual(TEXT("an Actor class is taken"), ReadErrors(*this, TEXT(R"({"KitActors": {"mob": "/Script/Engine.Actor"}})")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecKitConfigTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.KitConfigThatIsNoJsonObject", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecKitConfigTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"CodeSource": "GameKitStarter", "KitConfig": "not json"})")), TEXT("KitConfig is not a JSON object"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecBakeFailureTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.WhatTheBakeRefuses", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecBakeFailureTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	const TSharedPtr<FJsonObject> Spec = Parse(FString::Printf(TEXT(R"({"TypeName": "bake_fail", "State": "%s", "WatchedFields": ["Nope"]})"), RecordPath));
	TArray<FString> Errors;
	const FDefinitionPtr Made = CrowdyServerObjectSpec::MakeNew(*Spec, NoProject(), Errors);
	TestFalse(TEXT("a spec the bake refuses makes no definition"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("Watched field 'Nope' is not a field of"));
	TestTrue(TEXT("no package was made for it"), FindPackage(nullptr, *CrowdyServerObjectSpec::MakePackageName(TEXT("/Game/ServerObjects"), *NewDefinition(TEXT("bake_fail")))) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecTakenTypeNameTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.NewTypeNameAProjectTypeHas", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecTakenTypeNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Other = NewDefinition(TEXT("other_type"));
	Other->State = FCrowdyExecTestRecord::StaticStruct();
	const TSharedPtr<FJsonObject> Spec = Parse(FString::Printf(TEXT(R"({"TypeName": "Other_Type", "State": "%s"})"), RecordPath));
	TArray<FString> Errors;
	const FDefinitionPtr Made = CrowdyServerObjectSpec::MakeNew(*Spec, {Other.Get()}, Errors);
	TestFalse(TEXT("a name a project type has, in other capitals, makes no definition"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("TypeName is \"Other_Type\", which"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecNewStarterTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.NewTypeThatIsAGameKitStarter", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecNewStarterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	const TSharedPtr<FJsonObject> Spec = Parse(FString::Printf(TEXT(R"({"TypeName": "my_starter", "State": "%s", "CodeSource": "GameKitStarter"})"), RecordPath));
	TArray<FString> Errors;
	const FDefinitionPtr Made = CrowdyServerObjectSpec::MakeNew(*Spec, NoProject(), Errors);
	TestFalse(TEXT("a new type cannot be a Game Kit starter"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("CodeSource is GameKitStarter"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecEditMissingKeyTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.EditSpecThatLeavesOutASetting", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecEditMissingKeyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Existing = MakeStructState();
	const TSharedRef<FJsonObject> Spec = CrowdyServerObjectSpec::ToJson(*Existing);
	Spec->RemoveField(TEXT("Functions"));
	TArray<FString> Errors;
	const FDefinitionPtr Made = MakeCandidateFor(Spec, *Existing, Errors);
	TestFalse(TEXT("a spec without Functions makes no edit, so it cannot wipe them"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("The spec leaves out Functions."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecEditRenameTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.EditThatChangesTheTypeName", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecEditRenameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Existing = MakeStructState();
	const TSharedRef<FJsonObject> Spec = CrowdyServerObjectSpec::ToJson(*Existing);
	Spec->SetStringField(TEXT("TypeName"), TEXT("renamed_type"));
	TArray<FString> Errors;
	const FDefinitionPtr Made = MakeCandidateFor(Spec, *Existing, Errors);
	TestFalse(TEXT("a changed TypeName makes no edit"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("TypeName is \"renamed_type\" but the type is \"round_struct\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecEditStarterSwitchTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.EditThatMakesTheTypeAGameKitStarter", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecEditStarterSwitchTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Existing = MakeStructState();
	const TSharedRef<FJsonObject> Spec = CrowdyServerObjectSpec::ToJson(*Existing);
	Spec->SetStringField(TEXT("CodeSource"), TEXT("GameKitStarter"));
	TArray<FString> Errors;
	const FDefinitionPtr Made = MakeCandidateFor(Spec, *Existing, Errors);
	TestFalse(TEXT("a switch to a Game Kit starter makes no edit"), Made.IsValid());
	ExpectError(*this, Errors, TEXT("CodeSource cannot change to or from GameKitStarter"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecEditChangesTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.EditReportsWhatChanged", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecEditChangesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Existing = MakeListState();
	TArray<FString> Errors;
	if (!TestTrue(TEXT("the existing type bakes"), Existing->Bake(Errors)))
	{
		return false;
	}
	const TSharedRef<FJsonObject> Same = CrowdyServerObjectSpec::ToJson(*Existing);
	const FDefinitionPtr Unchanged = MakeCandidateFor(Same, *Existing, Errors);
	if (!TestTrue(FString::Printf(TEXT("the unchanged spec makes an edit (%s)"), *FString::Join(Errors, TEXT("; "))), Unchanged.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("an unchanged spec changes no setting"), CrowdyServerObjectSpec::FindChangedKeys(*Existing, *Unchanged).Num(), 0);
	TestEqual(TEXT("an unchanged spec renames no List value"), CrowdyServerObjectSpec::FindRenamedListValues(*Existing, *Unchanged).Num(), 0);

	const TSharedRef<FJsonObject> Renamed = CrowdyServerObjectSpec::ToJson(*Existing);
	Renamed->SetStringField(TEXT("Description"), TEXT("Changed"));
	for (const TSharedPtr<FJsonValue>& Variable : Renamed->GetArrayField(TEXT("StateList")))
	{
		if (Variable->AsObject()->GetStringField(TEXT("Name")) == TEXT("Label"))
		{
			Variable->AsObject()->SetStringField(TEXT("Name"), TEXT("Title"));
		}
	}
	Errors.Reset();
	const FDefinitionPtr Edited = MakeCandidateFor(Renamed, *Existing, Errors);
	if (!TestTrue(FString::Printf(TEXT("the edited spec makes an edit (%s)"), *FString::Join(Errors, TEXT("; "))), Edited.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the settings that differ"), FString::Join(CrowdyServerObjectSpec::FindChangedKeys(*Existing, *Edited), TEXT(",")), FString(TEXT("StateList,Description")));
	const TArray<CrowdyServerObjectSpec::FRenamedListValue> Moved = CrowdyServerObjectSpec::FindRenamedListValues(*Existing, *Edited);
	if (TestEqual(TEXT("one List value changed name"), Moved.Num(), 1))
	{
		TestEqual(TEXT("it was called Label"), Moved[0].From, FString(TEXT("Label")));
		TestEqual(TEXT("it is called Title"), Moved[0].To, FString(TEXT("Title")));
		TestEqual(TEXT("in the state List"), Moved[0].List, FString(TEXT("RoundListState")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecControlCharacterTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.ControlCharactersInNamesAndText", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecControlCharacterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Messages": [{"Name": "Evil\npub fn injected() {}"}]})")), TEXT("Messages[0].Name holds a control character"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"TypeName": "a\u007fb"})")), TEXT("TypeName holds a control character"));
	TestEqual(TEXT("a Description may hold line breaks"), ReadErrors(*this, TEXT(R"({"Description": "two\nlines\tand a tab"})")).Num(), 0);
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"Description": "bell\u0007"})")), TEXT("Description holds a control character"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecLogicFileTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.LogicFileOutsideTheProject", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecLogicFileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"LogicFile": {"FilePath": "/outside/logic.rs"}})")), TEXT("LogicFile.FilePath is \"/outside/logic.rs\"; it must be a .rs file inside the project"));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"LogicFile": {"FilePath": "../../escape.rs"}})")), TEXT("LogicFile.FilePath is \"../../escape.rs\""));
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"LogicFile": {"FilePath": "Server/logic.txt"}})")), TEXT("LogicFile.FilePath is \"Server/logic.txt\""));
	TestEqual(TEXT("a .rs file under the project is taken"), ReadErrors(*this, TEXT(R"({"LogicFile": {"FilePath": "Server/logic.rs"}})")).Num(), 0);
	TestEqual(TEXT("so is no file"), ReadErrors(*this, TEXT(R"({"LogicFile": {"FilePath": ""}})")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecPropertyFlagsTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.VariableFlagsTheEditorNeverSets", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecPropertyFlagsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	const FString Visible = FString::Printf(TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32", "PropertyFlags": "%llu"}]})"), static_cast<uint64>(CPF_Edit | CPF_BlueprintVisible));
	ExpectError(*this, ReadErrors(*this, *Visible), TEXT("StateList[0].PropertyFlags is 5, which sets flags the Server Object editor never does"));
	const uint64 Advanced = CPF_Edit | CPF_AdvancedDisplay | CPF_NativeAccessSpecifierPublic;
	const FString Taken = FString::Printf(TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32", "PropertyFlags": "%llu"}]})"), Advanced);
	TestEqual(TEXT("the flags the editor sets are taken"), ReadErrors(*this, *Taken).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecFloatBoundsTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.RoundTrip.FloatBoundsAreHeldAsFloats", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecFloatBoundsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Definition = NewDefinition(TEXT("round_float"));
	Definition->StateForm = ECrowdyServerValuesForm::List;
	FPropertyBagPropertyDesc Ratio(TEXT("Ratio"), EPropertyBagPropertyType::Float);
	CrowdyServerValueRange::SetBound(Ratio, TEXT("ClampMax"), TEXT("0.3"));
	Definition->StateList.AddProperties({Ratio});
	Reload(Definition->StateList);
	Definition->StateList.SetValueFloat(TEXT("Ratio"), 0.3f);
	FCrowdyServerTimer& Tick = Definition->Timers.AddDefaulted_GetRef();
	Tick.Name = TEXT("tick");
	Tick.Seconds = 0.01f;
	RoundTrip(*this, *Definition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecSoftPathTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.SoftPathThatIsNoPath", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecSoftPathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"KitActors": {"mob": "Actor"}})")), TEXT("KitActors[mob] is \"Actor\", which is not an object path"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecBoundKeyCaseTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.ValueRangeKeyInTheWrongCase", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecBoundKeyCaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	ExpectError(*this, ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32", "MetaData": {"clampmax": "5"}}]})")), TEXT("StateList[0].MetaData.clampmax is spelled ClampMax (capitals matter)"));
	TestEqual(TEXT("the right spelling is taken"), ReadErrors(*this, TEXT(R"({"StateList": [{"Name": "a", "Type": "Int32", "MetaData": {"ClampMax": "5"}}]})")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecFailedCreateLeavesNothingTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.CreateAssetThatFailsLeavesNoPackage", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecFailedCreateLeavesNothingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Unbakeable = NewDefinition(TEXT("create_fail"));
	Unbakeable->State = FCrowdyExecTestRecord::StaticStruct();
	Unbakeable->WatchedFields = {TEXT("Nope")};
	const FString ContentPath = TEXT("/Game/CrowdySpecTestScratch");
	FString Error;
	TestNull(TEXT("a definition that does not bake makes no asset"), CrowdyServerObjectSpec::CreateAsset(*Unbakeable, ContentPath, Error));
	ExpectError(*this, {Error}, TEXT("does not bake"));
	const UPackage* Left = FindPackage(nullptr, *CrowdyServerObjectSpec::MakePackageName(ContentPath, *Unbakeable));
	TestTrue(TEXT("and its package is not left behind"), !Left || !IsValid(Left));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecNewTypePathTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.NewTypePathOutsideGame", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecNewTypePathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Candidate = NewDefinition(TEXT("path_check"));
	Candidate->State = FCrowdyExecTestRecord::StaticStruct();
	for (const TCHAR* Path : {TEXT("/Engine/Transient"), TEXT("/Script/CoreUObject"), TEXT("/Game"), TEXT("Game/ServerObjects")})
	{
		FString Error;
		TestNull(FString::Printf(TEXT("%s makes no asset"), Path), CrowdyServerObjectSpec::CreateAsset(*Candidate, Path, Error));
		ExpectError(*this, {Error}, TEXT("must start with /Game/"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecTakenAssetNameTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.Refuses.AssetNameTakenByAnotherType", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecTakenAssetNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	const FString ContentPath = FString::Printf(TEXT("/Game/CrowdySpecTestScratch/%s"), *FGuid::NewGuid().ToString());
	const FString PackageName = ContentPath / TEXT("CSO_TipJar");
	UPackage* Package = CreatePackage(*PackageName);
	FDefinitionPtr Holder(NewObject<UCrowdyServerObjectDefinition>(Package, TEXT("CSO_TipJar"), RF_Public | RF_Standalone));
	Holder->TypeName = TEXT("tip_jar");
	FDefinitionPtr Candidate = NewDefinition(TEXT("tip__jar"));
	Candidate->State = FCrowdyExecTestRecord::StaticStruct();
	FString Error;
	TestNull(TEXT("a type whose asset name another type's asset has makes no asset"), CrowdyServerObjectSpec::CreateAsset(*Candidate, ContentPath, Error));
	TestEqualSensitive(TEXT("and the error names the holder and the way out"), Error,
		FString::Printf(TEXT("The asset name CSO_TipJar is taken by tip_jar (%s); pass -path= to put tip__jar somewhere else"), *PackageName));
	Holder->ClearFlags(RF_Standalone);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecFailedEditRestoresTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.EditThatFailsPutsTheTypeBack", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecFailedEditRestoresTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	FDefinitionPtr Existing = MakeStructState();
	TArray<FString> Errors;
	if (!TestTrue(TEXT("the existing type bakes"), Existing->Bake(Errors)))
	{
		return false;
	}
	FDefinitionPtr Before = NewDefinition(TEXT(""));
	CrowdyServerObjectSpec::CopyAuthored(*Existing, *Before);
	FDefinitionPtr Unbakeable = NewDefinition(TEXT(""));
	CrowdyServerObjectSpec::CopyAuthored(*Existing, *Unbakeable);
	Unbakeable->Description = TEXT("Changed");
	Unbakeable->WatchedFields = {TEXT("Nope")};
	FString Error;
	TestFalse(TEXT("an edit the bake refuses fails"), CrowdyServerObjectSpec::ApplyEdit(*Unbakeable, *Existing, Error));
	ExpectError(*this, {Error}, TEXT("Watched field 'Nope' is not a field of"));
	TestEqual(TEXT("and leaves every authored setting as it was"), CrowdyServerObjectSpec::FindChangedKeys(*Before, *Existing).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSpecPackageNameTest, "CrowdySDK.CrowdyExecEditor.ServerObjectSpec.PackageNameIsCsoAndPascalCase", CrowdyServerObjectSpecTest::TestFlags)
bool FCrowdyServerObjectSpecPackageNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectSpecTest;
	TestEqual(TEXT("the package of tip_jar"), CrowdyServerObjectSpec::MakePackageName(TEXT("/Game/ServerObjects"), *NewDefinition(TEXT("tip_jar"))), FString(TEXT("/Game/ServerObjects/CSO_TipJar")));
	TestEqual(TEXT("a path with a closing slash"), CrowdyServerObjectSpec::MakePackageName(TEXT("/Game/Types/"), *NewDefinition(TEXT("boss2"))), FString(TEXT("/Game/Types/CSO_Boss2")));
	return true;
}

#endif
