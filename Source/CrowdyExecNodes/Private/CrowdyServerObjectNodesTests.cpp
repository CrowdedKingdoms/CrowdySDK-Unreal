#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "CrowdyExecNodesSpawn.h"
#include "CrowdyExecNodesTestTypes.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyK2Node_CallServerFunction.h"
#include "CrowdyK2Node_GetServerState.h"
#include "CrowdyK2Node_GetServerVariable.h"
#include "CrowdyK2Node_ServerMessageReceived.h"
#include "CrowdyK2Node_ServerVariableChanged.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectNodesTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	// Marks the test enum a Blueprint type for each test's run only.
	struct FTestEnumIsBlueprintTypeWhileTesting
	{
		FTestEnumIsBlueprintTypeWhileTesting()
		{
			StartHandle = FAutomationTestFramework::Get().OnTestStartEvent.AddLambda([](FAutomationTestBase*) { StaticEnum<ECrowdyExecNodesTestPhase>()->SetMetaData(TEXT("BlueprintType"), TEXT("true")); });
			EndHandle = FAutomationTestFramework::Get().OnTestEndEvent.AddLambda([](FAutomationTestBase*) { StaticEnum<ECrowdyExecNodesTestPhase>()->RemoveMetaData(TEXT("BlueprintType")); });
		}

		~FTestEnumIsBlueprintTypeWhileTesting()
		{
			FAutomationTestFramework::Get().OnTestStartEvent.Remove(StartHandle);
			FAutomationTestFramework::Get().OnTestEndEvent.Remove(EndHandle);
		}

		FDelegateHandle StartHandle;
		FDelegateHandle EndHandle;
	};
	const FTestEnumIsBlueprintTypeWhileTesting TestEnumIsBlueprintTypeWhileTesting;

	struct FExpectedPin
	{
		const TCHAR* Field = nullptr;
		FName Category;
		const UObject* Object = nullptr;
		FName SubCategory;
		EPinContainerType Container = EPinContainerType::None;
		bool bOptional = false;
		FName ValueCategory;
	};

	// Every kind a value can be, and the pin each must get; the widened integers are the rows whose C++ type differs.
	TArray<FExpectedPin> EveryTypePins()
	{
		using K2 = UEdGraphSchema_K2;
		const UScriptStruct* Hit = FCrowdyExecNodesTestHit::StaticStruct();
		return {
			{TEXT("bFlag"), K2::PC_Boolean},
			{TEXT("Byte"), K2::PC_Byte},
			{TEXT("Tiny"), K2::PC_Int},
			{TEXT("Short"), K2::PC_Int},
			{TEXT("UShort"), K2::PC_Int},
			{TEXT("Int"), K2::PC_Int},
			{TEXT("UInt"), K2::PC_Int64},
			{TEXT("Big"), K2::PC_Int64},
			{TEXT("UBig"), K2::PC_Int64},
			{TEXT("Float"), K2::PC_Real, nullptr, K2::PC_Float},
			{TEXT("Double"), K2::PC_Real, nullptr, K2::PC_Double},
			{TEXT("String"), K2::PC_String},
			{TEXT("Name"), K2::PC_Name},
			{TEXT("Phase"), K2::PC_Byte, StaticEnum<ECrowdyExecNodesTestPhase>()},
			{TEXT("Hit"), K2::PC_Struct, Hit},
			{TEXT("Vector"), K2::PC_Struct, TBaseStructure<FVector>::Get()},
			{TEXT("Vector2D"), K2::PC_Struct, TBaseStructure<FVector2D>::Get()},
			{TEXT("Rotator"), K2::PC_Struct, TBaseStructure<FRotator>::Get()},
			{TEXT("Quat"), K2::PC_Struct, TBaseStructure<FQuat>::Get()},
			{TEXT("IntPoint"), K2::PC_Struct, TBaseStructure<FIntPoint>::Get()},
			{TEXT("IntVector"), K2::PC_Struct, TBaseStructure<FIntVector>::Get()},
			{TEXT("LinearColor"), K2::PC_Struct, TBaseStructure<FLinearColor>::Get()},
			{TEXT("Color"), K2::PC_Struct, TBaseStructure<FColor>::Get()},
			{TEXT("DateTime"), K2::PC_Struct, TBaseStructure<FDateTime>::Get()},
			{TEXT("Timespan"), K2::PC_Struct, FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.Timespan"))},
			{TEXT("Guid"), K2::PC_Struct, TBaseStructure<FGuid>::Get()},
			{TEXT("Tag"), K2::PC_Struct, FGameplayTag::StaticStruct()},
			{TEXT("SoftObject"), K2::PC_SoftObject, UObject::StaticClass()},
			{TEXT("SoftClass"), K2::PC_SoftClass, UObject::StaticClass()},
			{TEXT("Path"), K2::PC_Struct, TBaseStructure<FSoftObjectPath>::Get()},
			{TEXT("MaybeInt"), K2::PC_Int, nullptr, NAME_None, EPinContainerType::None, true},
			{TEXT("MaybeShort"), K2::PC_Int, nullptr, NAME_None, EPinContainerType::None, true},
			{TEXT("MaybeString"), K2::PC_String, nullptr, NAME_None, EPinContainerType::None, true},
			{TEXT("Ints"), K2::PC_Int, nullptr, NAME_None, EPinContainerType::Array},
			{TEXT("Shorts"), K2::PC_Int, nullptr, NAME_None, EPinContainerType::Array},
			{TEXT("Names"), K2::PC_Name, nullptr, NAME_None, EPinContainerType::Set},
			{TEXT("UShorts"), K2::PC_Int, nullptr, NAME_None, EPinContainerType::Set},
			{TEXT("Scores"), K2::PC_String, nullptr, NAME_None, EPinContainerType::Map, false, K2::PC_Int},
			{TEXT("TinyByUInt"), K2::PC_Int64, nullptr, NAME_None, EPinContainerType::Map, false, K2::PC_Int},
			{TEXT("Hits"), K2::PC_Struct, Hit, NAME_None, EPinContainerType::Array},
		};
	}

	// bReadyPin: the node is a Get, which has a Has Value pin for every variable, advanced unless the variable is optional.
	void CheckPin(FAutomationTestBase& Test, const FString& Where, const UEdGraphNode* Node, const FExpectedPin& Expected, EEdGraphPinDirection Direction, bool bReadyPin = false)
	{
		const FString What = FString::Printf(TEXT("%s %s"), *Where, Expected.Field);
		const UEdGraphPin* Pin = Node->FindPin(Expected.Field, Direction);
		if (!Test.TestNotNull(What + TEXT(" has a pin"), Pin))
		{
			return;
		}
		Test.TestEqual(What + TEXT(" category"), Pin->PinType.PinCategory, Expected.Category);
		Test.TestEqual(What + TEXT(" subcategory"), Pin->PinType.PinSubCategory, Expected.SubCategory);
		Test.TestTrue(What + TEXT(" type object"), Pin->PinType.PinSubCategoryObject.Get() == Expected.Object);
		Test.TestTrue(What + TEXT(" container"), Pin->PinType.ContainerType == Expected.Container);
		Test.TestEqual(What + TEXT(" map value"), Pin->PinType.PinValueType.TerminalCategory, Expected.ValueCategory);
		Test.TestFalse(What + TEXT(" is not by reference"), Pin->PinType.bIsReference);
		const UEdGraphPin* HasValue = Node->FindPin(CrowdyServerObjectPins::HasValuePinName(Expected.Field), Direction);
		Test.TestEqual(What + TEXT(" has a Has Value pin exactly when optional, or on a Get"), HasValue != nullptr, Expected.bOptional || bReadyPin);
		if (HasValue)
		{
			Test.TestEqual(What + TEXT(" Has Value is a Boolean"), HasValue->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
			Test.TestEqual(What + TEXT(" Has Value is advanced exactly when the value is not optional"), HasValue->bAdvancedView, !Expected.bOptional);
		}
	}

	UCrowdyServerObjectDefinition* MakeDefinition(const TCHAR* Name, UObject* Outer = GetTransientPackage())
	{
		return NewObject<UCrowdyServerObjectDefinition>(Outer, MakeUniqueObjectName(Outer, UCrowdyServerObjectDefinition::StaticClass(), Name));
	}

	UCrowdyServerObjectDefinition* MakeEveryTypeDefinition(UObject* Outer = GetTransientPackage())
	{
		UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_EveryType"), Outer);
		Definition->TypeName = TEXT("every_type");
		Definition->State = FCrowdyExecNodesTestEveryType::StaticStruct();
		FCrowdyServerFunction& Echo = Definition->Functions.AddDefaulted_GetRef();
		Echo.Name = TEXT("Echo");
		Echo.Params = FCrowdyExecNodesTestEveryType::StaticStruct();
		Echo.Reply = FCrowdyExecNodesTestEveryType::StaticStruct();
		for (const FExpectedPin& Row : EveryTypePins())
		{
			Definition->WatchedFields.Add(Row.Field);
		}
		return Definition;
	}

	UCrowdyServerObjectDefinition* MakeListDefinition(FName Variable, EPropertyBagPropertyType Type, UObject* Outer = GetTransientPackage())
	{
		UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_List"), Outer);
		Definition->TypeName = TEXT("list_type");
		Definition->StateForm = ECrowdyServerValuesForm::List;
		Definition->StateList.AddProperty(Variable, Type);
		Definition->WatchedFields.Add(Variable);
		return Definition;
	}

	// A struct payload, a List payload with an Array, and a message with no payload.
	UCrowdyServerObjectDefinition* MakeMessageDefinition(UObject* Outer = GetTransientPackage())
	{
		UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_Messages"), Outer);
		Definition->TypeName = TEXT("messages_type");
		FCrowdyServerMessage& Hit = Definition->Messages.AddDefaulted_GetRef();
		Hit.Name = TEXT("HitTaken");
		Hit.Payload = FCrowdyExecNodesTestHit::StaticStruct();
		FCrowdyServerMessage& Loot = Definition->Messages.AddDefaulted_GetRef();
		Loot.Name = TEXT("LootDropped");
		Loot.PayloadForm = ECrowdyServerValuesForm::List;
		Loot.PayloadList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
		Loot.PayloadList.AddContainerProperty(TEXT("Items"), FPropertyBagContainerTypes{EPropertyBagContainerType::Array}, EPropertyBagPropertyType::String, nullptr);
		Definition->Messages.AddDefaulted_GetRef().Name = TEXT("Won");
		return Definition;
	}

	UCrowdyServerObjectDefinition* MakeRefusedDefinition()
	{
		UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_Refused"));
		Definition->State = FCrowdyExecNodesTestRefused::StaticStruct();
		Definition->WatchedFields = {TEXT("Hidden"), TEXT("HiddenEnum"), TEXT("Fine")};
		return Definition;
	}

	UEdGraph* MakeGraph()
	{
		UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
		UEdGraph* Graph = NewObject<UEdGraph>(Blueprint);
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		return Graph;
	}

	template <typename NodeType>
	NodeType* AddNode(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Member)
	{
		NodeType* Node = NewObject<NodeType>(Graph);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->Definition = Definition;
		Node->Member = Member;
		Node->AllocateDefaultPins();
		return Node;
	}

	UK2Node_CallFunction* AddCall(UEdGraph* Graph, UClass* Class, FName Function)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->FunctionReference.SetExternalMember(Function, Class);
		Node->AllocateDefaultPins();
		return Node;
	}

	UK2Node_CallFunction* AddPrint(UEdGraph* Graph)
	{
		return AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	}

	struct FCompileRig
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_Event* BeginPlay = nullptr;
	};

	FCompileRig MakeActorBlueprint()
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/BP_%s"), *FGuid::NewGuid().ToString()));
		FCompileRig Rig;
		Rig.Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_ServerObjectNodes"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		Rig.Graph = FBlueprintEditorUtils::FindEventGraph(Rig.Blueprint);
		const FName BeginPlay(TEXT("ReceiveBeginPlay"));
		Rig.BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Rig.Blueprint, AActor::StaticClass(), BeginPlay);
		if (!Rig.BeginPlay)
		{
			int32 PositionY = 0;
			Rig.BeginPlay = FKismetEditorUtilities::AddDefaultEventNode(Rig.Blueprint, Rig.Graph, BeginPlay, AActor::StaticClass(), PositionY);
		}
		Rig.BeginPlay->SetEnabledState(ENodeEnabledState::Enabled, false);
		return Rig;
	}

	TArray<FString> Compile(UBlueprint* Blueprint, int32& OutErrors, EMessageSeverity::Type Severity = EMessageSeverity::Error)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		OutErrors = Results.NumErrors;
		TArray<FString> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == Severity)
			{
				Messages.Add(Message->ToText().ToString());
			}
		}
		return Messages;
	}

	UCrowdyK2Node_CallServerFunction* AddPicker(UEdGraph* Graph)
	{
		UCrowdyK2Node_CallServerFunction* Call = NewObject<UCrowdyK2Node_CallServerFunction>(Graph, NAME_None, RF_Transactional);
		Graph->AddNode(Call, false, false);
		Call->CreateNewGuid();
		Call->bPickFunction = true;
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	// A definition edit as the Details panel reports it.
	void BroadcastEdit(UCrowdyServerObjectDefinition* Definition)
	{
		FPropertyChangedEvent Edit(nullptr, EPropertyChangeType::ValueSet);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Definition, Edit);
	}

	void AddTipsVariable(UCrowdyServerObjectDefinition* Definition)
	{
		Definition->StateList.AddProperty(TEXT("Tips"), EPropertyBagPropertyType::String);
		Definition->WatchedFields.Add(TEXT("Tips"));
	}

	bool AnyContains(const TArray<FString>& Messages, const TCHAR* Text)
	{
		return Messages.ContainsByPredicate([Text](const FString& Message) { return Message.Contains(Text); });
	}

	// Compiled bytecode stores each called function's address, so finding the address finds the call.
	bool CallsFunction(const UFunction* Code, const UFunction* Callee)
	{
		if (!Code || !Callee)
		{
			return false;
		}
		const ScriptPointerType Address = reinterpret_cast<ScriptPointerType>(Callee);
		for (int32 Index = 0; Index + int32(sizeof(Address)) <= Code->Script.Num(); ++Index)
		{
			if (FMemory::Memcmp(&Code->Script[Index], &Address, sizeof(Address)) == 0)
			{
				return true;
			}
		}
		return false;
	}

	// Where Code first calls Callee, or INDEX_NONE; a call that runs earlier sits at a lower offset.
	int32 CallOffset(const UFunction* Code, const UFunction* Callee)
	{
		if (!Code || !Callee)
		{
			return INDEX_NONE;
		}
		const ScriptPointerType Address = reinterpret_cast<ScriptPointerType>(Callee);
		for (int32 Index = 0; Index + int32(sizeof(Address)) <= Code->Script.Num(); ++Index)
		{
			if (FMemory::Memcmp(&Code->Script[Index], &Address, sizeof(Address)) == 0)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	// A float literal is compiled into its function's bytecode as its four bytes.
	bool ContainsFloat(const UFunction* Code, float Value)
	{
		if (!Code)
		{
			return false;
		}
		for (int32 Index = 0; Index + int32(sizeof(Value)) <= Code->Script.Num(); ++Index)
		{
			if (FMemory::Memcmp(&Code->Script[Index], &Value, sizeof(Value)) == 0)
			{
				return true;
			}
		}
		return false;
	}

	UFunction* UbergraphOf(UBlueprint* Blueprint)
	{
		const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		return Generated ? Generated->UberGraphFunction : nullptr;
	}

	void Link(UEdGraphNode* From, FName FromPin, UEdGraphNode* To, FName ToPin)
	{
		From->FindPinChecked(FromPin, EGPD_Output)->MakeLinkTo(To->FindPinChecked(ToPin, EGPD_Input));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesPinTypesTest, "CrowdySDK.Editor.ServerObjectNodes.PinTypesEveryKind", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesPinTypesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();
	const TArray<FExpectedPin> Expected = EveryTypePins();

	UCrowdyK2Node_GetServerState* State = AddNode<UCrowdyK2Node_GetServerState>(Graph, Definition, NAME_None);
	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	for (const FExpectedPin& Row : Expected)
	{
		CheckPin(*this, TEXT("Get"), AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, Row.Field), Row, EGPD_Output, true);
		CheckPin(*this, TEXT("On Changed"), AddNode<UCrowdyK2Node_ServerVariableChanged>(Graph, Definition, Row.Field), Row, EGPD_Output);
		CheckPin(*this, TEXT("Get Server State"), State, Row, EGPD_Output);
		CheckPin(*this, TEXT("Call input"), Call, Row, EGPD_Input);
		CheckPin(*this, TEXT("Call output"), Call, Row, EGPD_Output);
	}

	TestNull(TEXT("a transient field travels nowhere, so it has no pin"), State->FindPin(TEXT("Cache")));
	const int32 ValuePins = State->Pins.FilterByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Output; }).Num();
	const int32 Optionals = Expected.FilterByPredicate([](const FExpectedPin& Row) { return Row.bOptional; }).Num();
	TestEqual(TEXT("Get Server State has one pin per variable, one per optional's Has Value, and Has Values"), ValuePins, Expected.Num() + Optionals + 1);
	const UEdGraphPin* HasValues = State->FindPin(CrowdyServerObjectNodePins::HasValues, EGPD_Output);
	if (TestNotNull(TEXT("Get Server State has a Has Values pin"), HasValues))
	{
		TestTrue(TEXT("which is advanced, so hidden until expanded"), HasValues->bAdvancedView && State->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesRefusalsTest, "CrowdySDK.Editor.ServerObjectNodes.Refusals", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeRefusedDefinition();
	TArray<FCrowdyServerValuePin> Variables;
	CrowdyServerObjectPins::GatherVariables(Definition, Variables);

	const FCrowdyServerValuePin* Hidden = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("Hidden"));
	const FCrowdyServerValuePin* HiddenEnum = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("HiddenEnum"));
	const FCrowdyServerValuePin* Fine = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("Fine"));
	if (!TestNotNull(TEXT("Hidden is a variable"), Hidden) || !TestNotNull(TEXT("HiddenEnum is a variable"), HiddenEnum) || !TestNotNull(TEXT("Fine is a variable"), Fine))
	{
		return false;
	}
	TestTrue(TEXT("a struct that is not BlueprintType is refused, saying what to do"), Hidden->Refusal.ToString().Contains(TEXT("mark it BlueprintType")));
	TestTrue(TEXT("an enum that is not BlueprintType is refused, saying what to do"), HiddenEnum->Refusal.ToString().Contains(TEXT("mark it BlueprintType")));
	TestTrue(TEXT("a Blueprint type beside them is not"), Fine->HasPin());

	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_GetServerVariable* Getter = AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, TEXT("Hidden"));
	TestNull(TEXT("a refused variable's getter has no value pin"), Getter->FindPin(TEXT("Hidden")));
	FCrowdyServerFunction& Refusing = Definition->Functions.AddDefaulted_GetRef();
	Refusing.Name = TEXT("Refusing");
	Refusing.Params = FCrowdyExecNodesTestRefused::StaticStruct();
	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestFalse(TEXT("the menu offers no getter for a refused variable"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Hidden"); }));
	TestTrue(TEXT("the menu still offers the others"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Fine"); }));
	TestFalse(TEXT("the menu offers no Get Server State while a variable has no pin"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry)
	{
		return Entry.NodeClass == UCrowdyK2Node_GetServerState::StaticClass();
	}));
	TestFalse(TEXT("the menu offers no Call while an input has no pin"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Refusing"); }));

	UCrowdyServerObjectDefinition* Nested = MakeDefinition(TEXT("DA_Nested"));
	Nested->StateForm = ECrowdyServerValuesForm::List;
	Nested->StateList.AddContainerProperty(TEXT("Grid"), FPropertyBagContainerTypes{EPropertyBagContainerType::Array, EPropertyBagContainerType::Array}, EPropertyBagPropertyType::Int32, nullptr);
	CrowdyServerObjectPins::GatherVariables(Nested, Variables);
	const FCrowdyServerValuePin* Grid = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("Grid"));
	if (TestNotNull(TEXT("an Array of Arrays is a variable"), Grid))
	{
		TestTrue(TEXT("an Array inside an Array is refused"), Grid->Refusal.ToString().Contains(TEXT("inside another")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesClashTest, "CrowdySDK.Editor.ServerObjectNodes.NameClashRefused", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesClashTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Then"), EPropertyBagPropertyType::Int32);
	Definition->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	Definition->WatchedFields.Add(TEXT("Gold"));
	TArray<FCrowdyServerValuePin> Variables;
	CrowdyServerObjectPins::GatherVariables(Definition, Variables);
	const FCrowdyServerValuePin* Then = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("Then"));
	const FCrowdyServerValuePin* Gold = CrowdyServerObjectPins::FindValue(Variables, FGuid(), TEXT("Gold"));
	if (!TestNotNull(TEXT("Then is a variable"), Then) || !TestNotNull(TEXT("Gold is a variable"), Gold))
	{
		return false;
	}
	TestTrue(TEXT("a variable named like a node pin, in any case, is refused saying what to do"), Then->Refusal.ToString().Contains(TEXT("clashes with the node's then pin; rename it")));
	TestTrue(TEXT("a variable beside it is not"), Gold->HasPin());

	TArray<FCrowdyServerValuePin> Values;
	FCrowdyServerValuePin& Optional = Values.AddDefaulted_GetRef();
	Optional.Name = TEXT("Coins");
	Optional.bOptional = true;
	Values.AddDefaulted_GetRef().Name = TEXT("Coins Has Value");
	CrowdyServerObjectPins::RefuseClashes(Values);
	TestTrue(TEXT("the optional keeps its pin"), Values[0].HasPin());
	TestTrue(TEXT("a value named like an optional's Has Value pin is refused"), Values[1].Refusal.ToString().Contains(TEXT("clashes with the node's Coins Has Value pin")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesRenameTest, "CrowdySDK.Editor.ServerObjectNodes.RenameKeepsLinks", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesRenameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_GetServerVariable* Getter = AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, TEXT("Amount"));
	UK2Node_CallFunction* Sink = AddCall(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt));
	UEdGraphPin* Amount = Getter->FindPin(TEXT("Amount"), EGPD_Output);
	if (!TestNotNull(TEXT("the getter has an Amount pin"), Amount))
	{
		return false;
	}
	Amount->MakeLinkTo(Sink->FindPinChecked(TEXT("A")));

	TestEqual(TEXT("the List entry is renamed"), Definition->StateList.RenameProperty(TEXT("Amount"), TEXT("Coins")), EPropertyBagAlterationResult::Success);
	Definition->WatchedFields = {TEXT("Coins")};
	Getter->ReconstructNode();

	TestEqual(TEXT("the node follows the variable to its new name"), Getter->Member, FName(TEXT("Coins")));
	TestNull(TEXT("the old pin is gone"), Getter->FindPin(TEXT("Amount"), EGPD_Output));
	const UEdGraphPin* Coins = Getter->FindPin(TEXT("Coins"), EGPD_Output);
	if (TestNotNull(TEXT("the pin has the new name"), Coins))
	{
		TestTrue(TEXT("the renamed pin keeps its wire"), Coins->LinkedTo.Contains(Sink->FindPinChecked(TEXT("A"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesOnlyOneInstanceTest, "CrowdySDK.Editor.ServerObjectNodes.OnlyOneInstancePinless", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesOnlyOneInstanceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();

	UCrowdyK2Node_GetServerVariable* Targeted = AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, TEXT("Int"));
	TestNotNull(TEXT("by default the node takes a Target"), Targeted->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));

	UCrowdyK2Node_GetServerVariable* Team = NewObject<UCrowdyK2Node_GetServerVariable>(Graph);
	Graph->AddNode(Team, false, false);
	Team->Definition = Definition;
	Team->Member = TEXT("Int");
	Team->bFind = true;
	Team->Instance = ECrowdyServerObjectFind::PlayersTeam;
	Team->AllocateDefaultPins();
	TestNull(TEXT("Find By Asset has no Target"), Team->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
	TestNotNull(TEXT("Player's Team takes a Team Id"), Team->FindPin(CrowdyServerObjectNodePins::TeamId, EGPD_Input));

	Definition->bOnlyOneInstance = true;
	TArray<UK2Node*> Nodes = {
		AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, TEXT("Int")),
		AddNode<UCrowdyK2Node_ServerVariableChanged>(Graph, Definition, TEXT("Int")),
		AddNode<UCrowdyK2Node_GetServerState>(Graph, Definition, NAME_None),
		AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"))};
	for (const UK2Node* Node : Nodes)
	{
		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		TestTrue(Title + TEXT(" finds its object by the asset"), CastChecked<UCrowdyK2Node_ServerObjectBase>(Node)->UsesFind());
		TestNull(Title + TEXT(" has no Target"), Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
		TestNull(Title + TEXT(" has no Instance Id"), Node->FindPin(CrowdyServerObjectNodePins::InstanceId, EGPD_Input));
		TestNull(Title + TEXT(" has no Team Id"), Node->FindPin(CrowdyServerObjectNodePins::TeamId, EGPD_Input));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesLibraryPinlessTest, "CrowdySDK.Editor.ServerObjectNodes.LibraryCallPinless", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesLibraryPinlessTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
	UEdGraph* Graph = MakeGraph();

	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	TestTrue(TEXT("a library's Call node finds its object by the asset"), Call->UsesFind());
	TestNull(TEXT("it has no Target"), Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
	TestNull(TEXT("it has no Instance Id"), Call->FindPin(CrowdyServerObjectNodePins::InstanceId, EGPD_Input));
	TestNull(TEXT("it has no Team Id"), Call->FindPin(CrowdyServerObjectNodePins::TeamId, EGPD_Input));
	TestNotNull(TEXT("it keeps the function's input pins"), Call->FindPin(TEXT("Int"), EGPD_Input));
	TestEqual(TEXT("its menu category names the library"), Call->GetMenuCategory().ToString(), FString::Printf(TEXT("Server Function Libraries|%s"), *Definition->GetName()));

	Call->bFind = false;
	Definition->Kind = ECrowdyServerObjectKind::ServerObject;
	Call->ReconstructNode();
	TestFalse(TEXT("as a Server Object it takes a Target again"), Call->UsesFind());
	TestNotNull(TEXT("and the Target pin returns"), Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesLibraryMenuTest, "CrowdySDK.Editor.ServerObjectNodes.LibraryMenuOffersOnlyCalls", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesLibraryMenuTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/DA_LibraryMenu_%s"), *FGuid::NewGuid().ToString()));
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition(Package);
	Definition->SetFlags(RF_Public | RF_Standalone);
	Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Definition->Messages.AddDefaulted_GetRef().Name = TEXT("Won");
	const FString Asset = Definition->GetName();

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestEqual(TEXT("one Call node per function, and no variable, state, message or member entries"), Entries.Num(), 1);
	const bool bOnlyCalls = !Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.NodeClass != UCrowdyK2Node_CallServerFunction::StaticClass(); });
	TestTrue(TEXT("every entry is a Call node"), bOnlyCalls);

	FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
	Database.RefreshAssetActions(Definition);
	const FBlueprintActionDatabase::FActionList* Actions = Database.GetAllActions().Find(Definition);
	const bool bCategorized = Actions && !Actions->IsEmpty() && !Actions->ContainsByPredicate([&Asset](const UBlueprintNodeSpawner* Spawner)
	{
		return Spawner->DefaultMenuSignature.Category.ToString() != FString::Printf(TEXT("Server Function Libraries|%s"), *Asset);
	});
	TestTrue(TEXT("the entries sit under Server Function Libraries and the asset's name"), bCategorized);

	Database.ClearAssetActions(Definition);
	Definition->ClearFlags(RF_Public | RF_Standalone);
	Definition->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesLibraryCompileTest, "CrowdySDK.Editor.ServerObjectNodes.LibraryCallCompiles", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesLibraryCompileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
	FCompileRig Rig = MakeActorBlueprint();

	TestNull(TEXT("no variable getter is placed for a library"), CrowdyExecNodesSpawn::SpawnServerVariableGetter(Rig.Graph, Definition, TEXT("Int"), FVector2D::ZeroVector));
	TestNull(TEXT("no state node is placed for a library"), CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector));
	UEdGraphNode* Call = CrowdyExecNodesSpawn::SpawnServerFunctionCall(Rig.Graph, Definition, TEXT("Echo"), FVector2D(300, 0));
	if (!TestNotNull(TEXT("the Call node is placed"), Call))
	{
		return false;
	}
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	for (const FString& Error : Errors)
	{
		AddInfo(FString::Printf(TEXT("compile error: %s"), *Error));
	}
	TestEqual(TEXT("a Blueprint calling a library function compiles without errors"), ErrorCount, 0);
	const UFunction* Find = UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, FindServerObjectLink));
	TestTrue(TEXT("the call reaches the library through Find Server Object Link, with no Target"), CallsFunction(UbergraphOf(Rig.Blueprint), Find));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesLibraryStateTest, "CrowdySDK.Editor.ServerObjectNodes.GetStateOfALibraryIsACompileError", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesLibraryStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute);
	Link(State, CrowdyServerObjectNodePins::HasValues, Print, TEXT("bPrintToScreen"));

	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("as a Server Object the node compiles"), ErrorCount, 0);

	Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	const FString Expected = FString::Printf(TEXT("%s is a Server Function Library and has no values to get."), *Definition->GetName());
	TestTrue(TEXT("once its definition is a library, the node in the graph is a compile error that says why"), AnyContains(Errors, *Expected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesEmptyStateTest, "CrowdySDK.Editor.ServerObjectNodes.GetStateOfAnEmptyStateIsNotOfferedAndFailsCompile", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesEmptyStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_EmptyState"), GetTransientPackage());
	Definition->TypeName = TEXT("empty_state");
	FCrowdyServerFunction& Echo = Definition->Functions.AddDefaulted_GetRef();
	Echo.Name = TEXT("Echo");

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	const bool bOffersState = Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.NodeClass == UCrowdyK2Node_GetServerState::StaticClass(); });
	TestFalse(TEXT("Get Server State is not offered for a State with no variables"), bOffersState);

	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);
	if (!TestNotNull(TEXT("a node placed anyway is still created"), State))
	{
		return false;
	}
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute);
	Link(State, CrowdyServerObjectNodePins::HasValues, Print, TEXT("bPrintToScreen"));

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	const FString Expected = FString::Printf(TEXT("%s has no variables visible to players"), *Definition->GetName());
	TestTrue(TEXT("the placed node is a compile error that says why"), AnyContains(Errors, *Expected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMenuTest,"CrowdySDK.Editor.ServerObjectNodes.MenuPerAsset", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMenuTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/DA_Menu_%s"), *FGuid::NewGuid().ToString()));
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition(Package);
	Definition->SetFlags(RF_Public | RF_Standalone);
	const FString Asset = Definition->GetName();

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	const int32 Variables = EveryTypePins().Num();
	TestEqual(TEXT("Get and On Changed per variable, Call per function, and Get Server State"), Entries.Num(), 2 * Variables + 2);
	auto HasTitle = [&Entries](const FString& Title) { return Entries.ContainsByPredicate([&Title](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Title.ToString() == Title; }); };
	TestTrue(TEXT("Get Int"), HasTitle(FString::Printf(TEXT("Get Int (%s)"), *Asset)));
	TestTrue(TEXT("On Int Changed"), HasTitle(FString::Printf(TEXT("On Int Changed (%s)"), *Asset)));
	TestTrue(TEXT("Call Echo"), HasTitle(FString::Printf(TEXT("Call Echo (%s)"), *Asset)));
	TestTrue(TEXT("Get Server State"), HasTitle(FString::Printf(TEXT("Get Server State (%s)"), *Asset)));

	FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
	Database.RefreshAssetActions(Definition);
	const FBlueprintActionDatabase::FActionList* Actions = Database.GetAllActions().Find(Definition);
	const int32 Registered = Actions ? Actions->Num() : 0;
	TestEqual(TEXT("every entry is registered under the definition asset"), Registered, Entries.Num());
	const bool bCategorized = Actions && Actions->ContainsByPredicate([&Asset](const UBlueprintNodeSpawner* Spawner)
	{
		return Spawner->DefaultMenuSignature.Category.ToString() == FString::Printf(TEXT("Server Objects|%s"), *Asset);
	});
	TestTrue(TEXT("the entries sit under Server Objects and the asset's name"), bCategorized);

	Database.ClearAssetActions(Definition);
	Definition->ClearFlags(RF_Public | RF_Standalone);
	Definition->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesDefinitionEditTest, "CrowdySDK.Editor.ServerObjectNodes.DefinitionEditReshapes", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesDefinitionEditTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);
	TestNull(TEXT("a variable not yet added has no pin"), State->FindPin(TEXT("Tips"), EGPD_Output));
	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("the compiled Blueprint is not dirty"), Rig.Blueprint->Status != BS_Dirty);

	AddTipsVariable(Definition);
	BroadcastEdit(Definition);
	const UEdGraphPin* Tips = State->FindPin(TEXT("Tips"), EGPD_Output);
	if (TestNotNull(TEXT("an edit to the definition reshapes the node"), Tips))
	{
		TestEqual(TEXT("the new pin has the new variable's type"), Tips->PinType.PinCategory, UEdGraphSchema_K2::PC_String);
	}
	TestTrue(TEXT("the reshape marks the Blueprint for a compile, so play never runs the old shape"), Rig.Blueprint->Status == BS_Dirty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesPastedNodeTest, "CrowdySDK.Editor.ServerObjectNodes.PastedNodeReshapes", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesPastedNodeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	// Saved as an asset, as a pasted node finds its definition by path.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/DA_Paste_%s"), *FGuid::NewGuid().ToString()));
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Amount"), EPropertyBagPropertyType::Int32, Package);
	Definition->SetFlags(RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		FBlueprintActionDatabase::Get().ClearAssetActions(Definition);
		Definition->ClearFlags(RF_Public | RF_Standalone);
	};
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);

	FString Text;
	FEdGraphUtilities::ExportNodesToText(TSet<UObject*>{State}, Text);
	TSet<UEdGraphNode*> Pasted;
	FEdGraphUtilities::ImportNodesFromText(Rig.Graph, Text, Pasted);
	if (!TestEqual(TEXT("one node is pasted"), Pasted.Num(), 1))
	{
		return false;
	}
	UEdGraphNode* Copy = *Pasted.CreateConstIterator();
	TestTrue(TEXT("the paste is a node of its own"), Copy != State);
	TestTrue(TEXT("the paste keeps its definition"), CastChecked<UCrowdyK2Node_ServerObjectBase>(Copy)->Definition == Definition);

	AddTipsVariable(Definition);
	BroadcastEdit(Definition);
	TestNotNull(TEXT("a pasted node reshapes after an edit to its definition"), Copy->FindPin(TEXT("Tips"), EGPD_Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesCompileCloneTest, "CrowdySDK.Editor.ServerObjectNodes.CompileCloneStaysDeaf", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesCompileCloneTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);

	// A compile clones the graph as this does; the clone is loaded like a saved node.
	UEdGraph* Clone = FEdGraphUtilities::CloneGraph(Rig.Graph, nullptr, nullptr, true);
	TArray<UCrowdyK2Node_GetServerState*> Cloned;
	Clone->GetNodesOfClass(Cloned);
	if (!TestEqual(TEXT("the clone holds a copy of the node"), Cloned.Num(), 1))
	{
		return false;
	}

	AddTipsVariable(Definition);
	BroadcastEdit(Definition);
	TestNotNull(TEXT("the node in the Blueprint reshapes"), State->FindPin(TEXT("Tips"), EGPD_Output));
	TestNull(TEXT("the compile's clone does not listen for edits"), Cloned[0]->FindPin(TEXT("Tips"), EGPD_Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesPickFunctionTest, "CrowdySDK.Editor.ServerObjectNodes.PickFunctionReshapes", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesPickFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_CallServerFunction* Call = AddPicker(Rig.Graph);
	const UEdGraphPin* Picker = Call->FindPin(CrowdyServerCallPins::FunctionPicker, EGPD_Input);
	if (!TestNotNull(TEXT("Call Server Function has a Function dropdown"), Picker))
	{
		return false;
	}
	TestTrue(TEXT("the dropdown takes no wire"), Picker->bNotConnectable);
	TestNull(TEXT("before a pick there are no value pins"), Call->FindPin(TEXT("Int"), EGPD_Input));
	TestEqual(TEXT("before a pick it is the generic node"), Call->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), FString(TEXT("Call Server Function")));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute);
	int32 ErrorCount = 0;
	TestTrue(TEXT("compiling before a pick says to pick a function"), AnyContains(Compile(Rig.Blueprint, ErrorCount), TEXT("Pick a Server Function")));

	Call->PickFunction(Definition, TEXT("Echo"));
	TestNotNull(TEXT("a pick grows the function's inputs"), Call->FindPin(TEXT("Int"), EGPD_Input));
	TestNotNull(TEXT("and its outputs"), Call->FindPin(TEXT("Int"), EGPD_Output));
	TestNotNull(TEXT("and keeps the dropdown"), Call->FindPin(CrowdyServerCallPins::FunctionPicker, EGPD_Input));
	TestEqual(TEXT("the input starts at the function's default"), Call->FindPin(TEXT("Int"), EGPD_Input)->DefaultValue, FString(TEXT("7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesPickUndoTest, "CrowdySDK.Editor.ServerObjectNodes.PickFunctionIsOneUndoStep", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesPickUndoTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	if (!TestNotNull(TEXT("the editor runs"), GEditor))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(MakeEveryTypeDefinition());
	FCompileRig Rig = MakeActorBlueprint();
	const TStrongObjectPtr<UBlueprint> Blueprint(Rig.Blueprint);
	const TStrongObjectPtr<UCrowdyK2Node_CallServerFunction> Call(AddPicker(Rig.Graph));

	Call->SetPickedFunction(Definition.Get(), TEXT("Echo"));
	TestTrue(TEXT("the dropdown's pick waits for the next tick, since its pin is rebuilt"), Call->Member.IsNone());
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Definition, Blueprint, Call, Deadline]()
	{
		if (Call->Member.IsNone() && FPlatformTime::Seconds() < Deadline)
		{
			return false;
		}
		TestEqual(TEXT("the pick lands on the next tick"), Call->Member, FName(TEXT("Echo")));
		TestNotNull(TEXT("and grows the function's pins"), Call->FindPin(TEXT("Int"), EGPD_Input));
		TestTrue(TEXT("and marks the Blueprint for a compile"), Blueprint->Status == BS_Dirty);
		GEditor->UndoTransaction();
		TestTrue(TEXT("one undo takes the pick back"), Call->Member.IsNone());
		TestNull(TEXT("with the pins it grew"), Call->FindPin(TEXT("Int"), EGPD_Input));
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesCompileTest, "CrowdySDK.Editor.ServerObjectNodes.HeadlessCompile", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesCompileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraph* Graph = Rig.Graph;

	// Placed through the public spawn helpers, as generated Blueprints place them.
	UEdGraphNode* Changed = CrowdyExecNodesSpawn::SpawnServerVariableChanged(Graph, Definition, TEXT("MaybeString"), FVector2D(300, 0));
	UEdGraphNode* ShortChanged = CrowdyExecNodesSpawn::SpawnServerVariableChanged(Graph, Definition, TEXT("Short"), FVector2D(500, 0));
	UEdGraphNode* Call = CrowdyExecNodesSpawn::SpawnServerFunctionCall(Graph, Definition, TEXT("Echo"), FVector2D(700, 0));
	UEdGraphNode* Short = CrowdyExecNodesSpawn::SpawnServerVariableGetter(Graph, Definition, TEXT("Short"), FVector2D(400, 300));
	UEdGraphNode* MaybeInt = CrowdyExecNodesSpawn::SpawnServerVariableGetter(Graph, Definition, TEXT("MaybeInt"), FVector2D(400, 400));
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Graph, Definition, FVector2D(400, 500));
	CrowdyExecNodesSpawn::SetFindByAsset(State, ECrowdyServerObjectFind::PlayersTeam);
	if (!TestNotNull(TEXT("the spawn helpers place every node"), Changed) || !ShortChanged || !Call || !Short || !MaybeInt || !State)
	{
		return false;
	}
	TestNotNull(TEXT("Find By Asset with Player's Team has a Team Id pin"), State->FindPin(CrowdyServerObjectNodePins::TeamId, EGPD_Input));
	UK2Node_CallFunction* PrintChanged = AddPrint(Graph);
	UK2Node_CallFunction* PrintShort = AddPrint(Graph);
	UK2Node_CallFunction* PrintReply = AddPrint(Graph);

	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Changed, UEdGraphSchema_K2::PN_Execute);
	Link(Changed, UEdGraphSchema_K2::PN_Then, ShortChanged, UEdGraphSchema_K2::PN_Execute);
	Link(ShortChanged, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute);
	Link(ShortChanged, CrowdyServerObjectNodePins::Changed, PrintShort, UEdGraphSchema_K2::PN_Execute);
	Link(Changed, CrowdyServerObjectNodePins::Changed, PrintChanged, UEdGraphSchema_K2::PN_Execute);
	Link(Changed, TEXT("MaybeString"), PrintChanged, TEXT("InString"));
	Link(Changed, CrowdyServerObjectPins::HasValuePinName(TEXT("MaybeString")), PrintChanged, TEXT("bPrintToScreen"));
	Link(Short, TEXT("Short"), Call, TEXT("Short"));
	Link(MaybeInt, TEXT("MaybeInt"), Call, TEXT("MaybeInt"));
	Link(MaybeInt, CrowdyServerObjectPins::HasValuePinName(TEXT("MaybeInt")), Call, CrowdyServerObjectPins::HasValuePinName(TEXT("MaybeInt")));
	Link(State, TEXT("Name"), Call, TEXT("Name"));
	Call->FindPinChecked(TEXT("Int"), EGPD_Input)->DefaultValue = TEXT("5");
	Call->FindPinChecked(TEXT("MaybeString"), EGPD_Input)->DefaultValue = TEXT("hello");
	Call->FindPinChecked(CrowdyServerObjectPins::HasValuePinName(TEXT("MaybeString")), EGPD_Input)->DefaultValue = TEXT("true");
	Link(Short, CrowdyServerObjectPins::HasValuePinName(TEXT("Short")), PrintShort, TEXT("bPrintToScreen"));
	Link(State, CrowdyServerObjectNodePins::HasValues, PrintReply, TEXT("bPrintToScreen"));
	Link(Call, CrowdyServerCallPins::OnSuccess, PrintReply, UEdGraphSchema_K2::PN_Execute);
	Link(Call, TEXT("String"), PrintReply, TEXT("InString"));
	Link(Call, CrowdyServerObjectPins::HasValuePinName(TEXT("MaybeShort")), PrintReply, TEXT("bPrintToLog"));

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	for (const FString& Error : Errors)
	{
		AddInfo(FString::Printf(TEXT("compile error: %s"), *Error));
	}
	TestEqual(TEXT("the Blueprint with all four nodes compiles without errors"), ErrorCount, 0);

	TArray<FString> Functions;
	for (TFieldIterator<UFunction> It(Rig.Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		Functions.Add(It->GetName());
	}
	auto HasFunction = [&Functions](const TCHAR* Prefix) { return Functions.ContainsByPredicate([Prefix](const FString& Name) { return Name.StartsWith(Prefix); }); };
	TestTrue(TEXT("the change event became a function of the generated class"), HasFunction(TEXT("OnServerVariableChanged_")));
	TestTrue(TEXT("the call's On Success became a function of the generated class"), HasFunction(TEXT("OnSuccess_")));
	TestTrue(TEXT("the call's On Failed became a function of the generated class"), HasFunction(TEXT("OnFailed_")));

	const UFunction* Ubergraph = CastChecked<UBlueprintGeneratedClass>(Rig.Blueprint->GeneratedClass)->UberGraphFunction;
	const UFunction* Typed = UCrowdyServerTypedCallAction::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerTypedCallAction, CallServerFunctionWithInputs));
	const UFunction* Note = UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, NoteInputError));
	TestTrue(TEXT("the call goes through the typed call, which refuses an input that did not fit"), CallsFunction(Ubergraph, Typed));
	TestTrue(TEXT("each set input's result is noted for that refusal"), CallsFunction(Ubergraph, Note));

	// Bind fills a handler's first parameter with the value and the next, when it is a bool, with Has Value.
	TArray<FString> Handlers;
	for (TFieldIterator<UFunction> It(Rig.Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->GetName().StartsWith(TEXT("OnServerVariableChanged_")))
		{
			continue;
		}
		TArray<FString> Params;
		for (TFieldIterator<FProperty> Param(*It); Param && Param->HasAnyPropertyFlags(CPF_Parm); ++Param)
		{
			Params.Add(Param->GetCPPType());
		}
		Handlers.Add(FString::Join(Params, TEXT(",")));
	}
	Handlers.Sort();
	TestEqual(TEXT("each change handler takes the widened value, then Has Value for an optional"), FString::Join(Handlers, TEXT(" | ")), FString(TEXT("FString,bool | int32")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStatusPinsTest, "CrowdySDK.Editor.ServerObjectNodes.CallStatusPinsComeLastAndAdvanced", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesStatusPinsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	TArray<const UEdGraphPin*> Outputs;
	for (const UEdGraphPin* Pin : Call->Pins)
	{
		if (Pin->Direction == EGPD_Output)
		{
			Outputs.Add(Pin);
		}
	}
	if (!TestTrue(TEXT("the call has its exec, value and status outputs"), Outputs.Num() > 6))
	{
		return false;
	}
	TestEqual(TEXT("Then comes first"), Outputs[0]->PinName, UEdGraphSchema_K2::PN_Then);
	TestEqual(TEXT("then On Success"), Outputs[1]->PinName, CrowdyServerCallPins::OnSuccess);
	TestEqual(TEXT("then On Failed"), Outputs[2]->PinName, CrowdyServerCallPins::OnFailed);
	const int32 IntIndex = Outputs.IndexOfByPredicate([](const UEdGraphPin* Pin) { return Pin->PinName == FName(TEXT("Int")); });
	const TArray<FName> Status = {CrowdyServerCallPins::Outcome, CrowdyServerCallPins::Reason, CrowdyServerCallPins::Retryable, CrowdyServerCallPins::Code};
	const int32 First = Outputs.Num() - Status.Num();
	TestTrue(TEXT("the function's outputs follow On Failed and come before the status pins"), IntIndex >= 3 && IntIndex < First);

	for (int32 Index = 0; Index < Outputs.Num(); ++Index)
	{
		const bool bStatus = Index >= First;
		if (bStatus)
		{
			TestEqual(TEXT("the last four outputs are Outcome, Reason, Retryable, Code in order"), Outputs[Index]->PinName, Status[Index - First]);
			TestFalse(Outputs[Index]->PinName.ToString() + TEXT(" has a tooltip"), Outputs[Index]->PinToolTip.IsEmpty());
		}
		TestEqual(Outputs[Index]->PinName.ToString() + TEXT(" is advanced exactly when it is a status pin"), Outputs[Index]->bAdvancedView, bStatus);
	}
	TestTrue(TEXT("the node hides its advanced pins until expanded"), Call->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStatusPinsSavedTest, "CrowdySDK.Editor.ServerObjectNodes.CallSavedBeforeAdvancedKeepsStatusLinks", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesStatusPinsSavedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	Call->AdvancedPinDisplay = ENodeAdvancedPins::NoPins;
	for (const FName Name : {CrowdyServerCallPins::Outcome, CrowdyServerCallPins::Reason, CrowdyServerCallPins::Retryable})
	{
		Call->FindPinChecked(Name, EGPD_Output)->bAdvancedView = false;
	}
	UK2Node_CallFunction* Compare = AddCall(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ByteByte));
	UK2Node_CallFunction* Print = AddPrint(Graph);
	Link(Call, CrowdyServerCallPins::Outcome, Compare, TEXT("A"));
	Link(Call, CrowdyServerCallPins::Reason, Print, TEXT("InString"));
	Link(Call, CrowdyServerCallPins::Retryable, Print, TEXT("bPrintToScreen"));

	Call->ReconstructNode();

	TestTrue(TEXT("a node saved with no advanced pins becomes Hidden"), Call->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	auto CheckStatusPin = [this, Call](FName Name, const UEdGraphNode* Sink)
	{
		const UEdGraphPin* Pin = Call->FindPin(Name, EGPD_Output);
		if (!TestNotNull(Name.ToString() + TEXT(" is still a pin"), Pin))
		{
			return;
		}
		TestTrue(Name.ToString() + TEXT(" is now advanced"), Pin->bAdvancedView);
		TestTrue(Name.ToString() + TEXT(" keeps its wire"), Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0]->GetOwningNode() == Sink);
	};
	CheckStatusPin(CrowdyServerCallPins::Outcome, Compare);
	CheckStatusPin(CrowdyServerCallPins::Reason, Print);
	CheckStatusPin(CrowdyServerCallPins::Retryable, Print);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesTimeoutPinTest, "CrowdySDK.Editor.ServerObjectNodes.CallTimeoutPinIsAdvancedAndDefaultsToZero", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesTimeoutPinTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	const UEdGraphPin* Timeout = Call->FindPin(CrowdyServerCallPins::Timeout, EGPD_Input);
	if (!TestNotNull(TEXT("the call has a Timeout input"), Timeout))
	{
		return false;
	}
	TestEqual(TEXT("Timeout is a real"), Timeout->PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
	TestEqual(TEXT("and a float, as the proxy's parameter is"), Timeout->PinType.PinSubCategory, UEdGraphSchema_K2::PC_Float);
	TestTrue(TEXT("Timeout is advanced"), Timeout->bAdvancedView);
	TestEqual(TEXT("and starts at 0, which uses the project's call timeout"), FCString::Atof(*Timeout->DefaultValue), 0.f);
	TestFalse(TEXT("and has a tooltip"), Timeout->PinToolTip.IsEmpty());
	TestEqualSensitive(TEXT("and is labelled Timeout when no input clashes"), Timeout->GetDisplayName().ToString(), FString(TEXT("Timeout")));
	TestTrue(TEXT("the node hides its advanced pins until expanded"), Call->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	for (const UEdGraphPin* Pin : Call->Pins)
	{
		if (Pin->Direction == EGPD_Input && Pin != Timeout)
		{
			TestFalse(Pin->PinName.ToString() + TEXT(" is not advanced, since Timeout is the only advanced input"), Pin->bAdvancedView);
		}
	}
	const int32 IntIndex = Call->Pins.IndexOfByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Input && Pin->PinName == FName(TEXT("Int")); });
	TestTrue(TEXT("Timeout comes after the function's inputs"), IntIndex >= 0 && Call->Pins.IndexOfByKey(Timeout) > IntIndex);

	UCrowdyK2Node_CallServerFunction* Wired = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	UK2Node_CallFunction* Literal = AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralDouble));
	Link(Literal, UEdGraphSchema_K2::PN_ReturnValue, Wired, CrowdyServerCallPins::Timeout);
	UCrowdyK2Node_CallServerFunction* Typed = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	Typed->FindPinChecked(CrowdyServerCallPins::Timeout, EGPD_Input)->DefaultValue = TEXT("4.5");
	UCrowdyK2Node_CallServerFunction* Saved = AddNode<UCrowdyK2Node_CallServerFunction>(Graph, Definition, TEXT("Echo"));
	Saved->RemovePin(Saved->FindPinChecked(CrowdyServerCallPins::Timeout, EGPD_Input));
	Wired->ReconstructNode();
	Typed->ReconstructNode();
	Saved->ReconstructNode();

	const UEdGraphPin* WiredTimeout = Wired->FindPin(CrowdyServerCallPins::Timeout, EGPD_Input);
	if (TestNotNull(TEXT("a rebuilt node keeps Timeout"), WiredTimeout))
	{
		TestTrue(TEXT("with its wire"), WiredTimeout->LinkedTo.Num() == 1 && WiredTimeout->LinkedTo[0]->GetOwningNode() == Literal);
		TestTrue(TEXT("and still advanced"), WiredTimeout->bAdvancedView);
	}
	const UEdGraphPin* TypedTimeout = Typed->FindPin(CrowdyServerCallPins::Timeout, EGPD_Input);
	if (TestNotNull(TEXT("a rebuilt node keeps a typed Timeout"), TypedTimeout))
	{
		TestEqual(TEXT("and its value"), FCString::Atof(*TypedTimeout->DefaultValue), 4.5f);
	}
	const UEdGraphPin* SavedTimeout = Saved->FindPin(CrowdyServerCallPins::Timeout, EGPD_Input);
	if (TestNotNull(TEXT("a node saved before Timeout existed grows it on rebuild"), SavedTimeout))
	{
		TestEqual(TEXT("at 0"), FCString::Atof(*SavedTimeout->DefaultValue), 0.f);
		TestTrue(TEXT("advanced"), SavedTimeout->bAdvancedView);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesTimeoutExpandsTest, "CrowdySDK.Editor.ServerObjectNodes.CallTimeoutReachesTheProxy", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesTimeoutExpandsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Call = CrowdyExecNodesSpawn::SpawnServerFunctionCall(Rig.Graph, Definition, TEXT("Echo"), FVector2D(300, 0));
	if (!TestNotNull(TEXT("the spawn helper places the call"), Call))
	{
		return false;
	}
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute);
	auto CompileClean = [this, &Rig]()
	{
		int32 ErrorCount = 0;
		for (const FString& Error : Compile(Rig.Blueprint, ErrorCount))
		{
			AddInfo(FString::Printf(TEXT("compile error: %s"), *Error));
		}
		return ErrorCount == 0;
	};

	constexpr float Seconds = 7.25f;
	TestTrue(TEXT("the call compiles with Timeout untouched"), CompileClean());
	TestFalse(TEXT("an untouched Timeout sends no seconds of its own"), ContainsFloat(UbergraphOf(Rig.Blueprint), Seconds));

	Call->FindPinChecked(CrowdyServerCallPins::Timeout, EGPD_Input)->DefaultValue = TEXT("7.25");
	TestTrue(TEXT("the call compiles with a typed Timeout"), CompileClean());
	TestTrue(TEXT("a typed Timeout is the seconds the typed call is given"), ContainsFloat(UbergraphOf(Rig.Blueprint), Seconds));

	UK2Node_CallFunction* Literal = AddCall(Rig.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralDouble));
	Link(Literal, UEdGraphSchema_K2::PN_ReturnValue, Call, CrowdyServerCallPins::Timeout);
	TestTrue(TEXT("the call compiles with a wired Timeout"), CompileClean());
	const UFunction* Ubergraph = UbergraphOf(Rig.Blueprint);
	TestTrue(TEXT("a wired Timeout moves onto the typed call, which evaluates it"), CallsFunction(Ubergraph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralDouble))));
	TestFalse(TEXT("and the wire wins over the typed value"), ContainsFloat(Ubergraph, Seconds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesTimeoutClashTest, "CrowdySDK.Editor.ServerObjectNodes.InputNamedTimeoutKeepsItsPin", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesTimeoutClashTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_TimeoutInput"));
	Definition->TypeName = TEXT("timeout_input");
	FCrowdyServerFunction& Wait = Definition->Functions.AddDefaulted_GetRef();
	Wait.Name = TEXT("Wait");
	Wait.ParamsForm = ECrowdyServerValuesForm::List;
	Wait.ParamsList.AddProperty(TEXT("Timeout"), EPropertyBagPropertyType::Int32);

	TArray<FCrowdyServerValuePin> Inputs;
	CrowdyServerObjectPins::GatherValues(Wait.GetParamsStruct(), Inputs);
	const FCrowdyServerValuePin* Timeout = CrowdyServerObjectPins::FindValue(Inputs, FGuid(), TEXT("Timeout"));
	if (!TestNotNull(TEXT("Timeout is an input"), Timeout))
	{
		return false;
	}
	TestTrue(TEXT("an input called Timeout keeps its pin beside the node's own Timeout"), Timeout->HasPin());

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestTrue(TEXT("and the menu offers its Call"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Wait"); }));

	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Call = CrowdyExecNodesSpawn::SpawnServerFunctionCall(Rig.Graph, Definition, TEXT("Wait"), FVector2D(0, 0));
	if (!TestNotNull(TEXT("the Call node spawns"), Call))
	{
		return false;
	}
	const UEdGraphPin* Input = Call->FindPin(TEXT("Timeout"), EGPD_Input);
	const UEdGraphPin* Own = Call->FindPin(CrowdyServerCallPins::Timeout, EGPD_Input);
	TestTrue(TEXT("the input and the node's own Timeout are two pins"), Input && Own && Input != Own && !Input->bAdvancedView && Own->bAdvancedView);
	if (Input && Own)
	{
		TestEqualSensitive(TEXT("the node's own pin is labelled Call Timeout beside the input"), Own->GetDisplayName().ToString(), FString(TEXT("Call Timeout")));
		TestNotEqual(TEXT("so the two pins show different labels"), Own->GetDisplayName().ToString(), Input->GetDisplayName().ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesChangedTwiceTest,"CrowdySDK.Editor.ServerObjectNodes.ChangedTwiceForOneVariableCompiles", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesChangedTwiceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* First = CrowdyExecNodesSpawn::SpawnServerVariableChanged(Rig.Graph, Definition, TEXT("Int"), FVector2D(300, 0));
	UEdGraphNode* Second = CrowdyExecNodesSpawn::SpawnServerVariableChanged(Rig.Graph, Definition, TEXT("Int"), FVector2D(500, 0));
	if (!TestNotNull(TEXT("the spawn helper places the first node"), First) || !TestNotNull(TEXT("and the second"), Second))
	{
		return false;
	}
	UK2Node_CallFunction* PrintFirst = AddPrint(Rig.Graph);
	UK2Node_CallFunction* PrintSecond = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, First, UEdGraphSchema_K2::PN_Execute);
	Link(First, UEdGraphSchema_K2::PN_Then, Second, UEdGraphSchema_K2::PN_Execute);
	Link(First, CrowdyServerObjectNodePins::Changed, PrintFirst, UEdGraphSchema_K2::PN_Execute);
	Link(Second, CrowdyServerObjectNodePins::Changed, PrintSecond, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	for (const FString& Error : Compile(Rig.Blueprint, ErrorCount))
	{
		AddInfo(FString::Printf(TEXT("compile error: %s"), *Error));
	}
	TestEqual(TEXT("two Changed nodes on one variable compile without errors"), ErrorCount, 0);

	TSet<FString> Handlers;
	for (TFieldIterator<UFunction> It(Rig.Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->GetName().StartsWith(TEXT("OnServerVariableChanged_")))
		{
			Handlers.Add(It->GetName());
		}
	}
	TestEqual(TEXT("each node expands to its own event function"), Handlers.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesHasValuesTest, "CrowdySDK.Editor.ServerObjectNodes.StateHasValuesReads", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesHasValuesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* State = CrowdyExecNodesSpawn::SpawnServerState(Rig.Graph, Definition, FVector2D::ZeroVector);
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute);
	Link(State, CrowdyServerObjectNodePins::HasValues, Print, TEXT("bPrintToScreen"));

	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("Get Server State with only Has Values wired compiles"), ErrorCount, 0);
	const UFunction* Ubergraph = CastChecked<UBlueprintGeneratedClass>(Rig.Blueprint->GeneratedClass)->UberGraphFunction;
	const UFunction* Read = UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, ReadServerVariable));
	TestTrue(TEXT("Has Values reads the object even with no variable wired"), CallsFunction(Ubergraph, Read));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesSpawnMissingTest, "CrowdySDK.Editor.ServerObjectNodes.SpawnRefusesMissingMember", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesSpawnMissingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	UEdGraph* Graph = MakeGraph();
	AddExpectedErrorPlain(TEXT("so no"), EAutomationExpectedErrorFlags::Contains, 4);
	TestNull(TEXT("no getter for a variable the definition lacks"), CrowdyExecNodesSpawn::SpawnServerVariableGetter(Graph, Definition, TEXT("Nope"), FVector2D::ZeroVector));
	TestNull(TEXT("no On Changed for a variable the definition lacks"), CrowdyExecNodesSpawn::SpawnServerVariableChanged(Graph, Definition, TEXT("Nope"), FVector2D::ZeroVector));
	TestNull(TEXT("no Call for a function the definition lacks"), CrowdyExecNodesSpawn::SpawnServerFunctionCall(Graph, Definition, TEXT("Nope"), FVector2D::ZeroVector));
	TestNull(TEXT("no Get Server State without a definition"), CrowdyExecNodesSpawn::SpawnServerState(Graph, nullptr, FVector2D::ZeroVector));
	TestEqual(TEXT("nothing is added to the graph"), Graph->Nodes.Num(), 0);
	TestNotNull(TEXT("a variable it has still spawns"), CrowdyExecNodesSpawn::SpawnServerVariableGetter(Graph, Definition, TEXT("Int"), FVector2D::ZeroVector));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesUnboundTest, "CrowdySDK.Editor.ServerObjectNodes.UnboundChangeWarns", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesUnboundTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_ServerVariableChanged* Changed = AddNode<UCrowdyK2Node_ServerVariableChanged>(Rig.Graph, Definition, TEXT("Int"));
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Changed, CrowdyServerObjectNodePins::Changed, Print, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Warnings = Compile(Rig.Blueprint, ErrorCount, EMessageSeverity::Warning);
	TestTrue(TEXT("an On Changed with nothing on Bind warns that it never runs"), AnyContains(Warnings, TEXT("never runs: connect Bind")));
	TestEqual(TEXT("and is no error"), ErrorCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesWrongTargetTest, "CrowdySDK.Editor.ServerObjectNodes.CompileRefusesWrongTarget", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesWrongTargetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_GetServerVariable* Getter = AddNode<UCrowdyK2Node_GetServerVariable>(Rig.Graph, Definition, TEXT("bFlag"));
	UK2Node_CallFunction* GameInstance = AddCall(Rig.Graph, UGameplayStatics::StaticClass(), GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetGameInstance));
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute);
	Link(Getter, TEXT("bFlag"), Print, TEXT("bPrintToScreen"));

	FString Reason;
	UEdGraphPin* Target = Getter->FindPinChecked(UEdGraphSchema_K2::PN_Self, EGPD_Input);
	UEdGraphPin* Instance = GameInstance->GetReturnValuePin();
	TestTrue(TEXT("the editor refuses to wire a Game Instance into Target"), Getter->IsConnectionDisallowed(Target, Instance, Reason));
	Instance->MakeLinkTo(Target);

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("a Target of another class is a compile error"), AnyContains(Errors, TEXT("Target must be a Server Object")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStaleWireTest, "CrowdySDK.Editor.ServerObjectNodes.RetypeReportsStaleWire", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesStaleWireTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Lit"), EPropertyBagPropertyType::Bool);
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_GetServerVariable* Getter = AddNode<UCrowdyK2Node_GetServerVariable>(Rig.Graph, Definition, TEXT("Lit"));
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute);
	Link(Getter, TEXT("Lit"), Print, TEXT("bPrintToScreen"));

	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("a Boolean variable wired to a Boolean input compiles"), ErrorCount, 0);

	TArray<FPropertyBagPropertyDesc> Descs(Definition->StateList.GetPropertyBagStruct()->GetPropertyDescs());
	Descs[0].ValueType = EPropertyBagPropertyType::Struct;
	Descs[0].ValueTypeObject = TBaseStructure<FVector>::Get();
	Definition->StateList.MigrateToNewBagStruct(UPropertyBag::GetOrCreateFromDescs(Descs));
	Getter->ReconstructNode();

	const UEdGraphPin* Lit = Getter->FindPin(TEXT("Lit"), EGPD_Output);
	if (!TestNotNull(TEXT("the retyped variable keeps its pin"), Lit))
	{
		return false;
	}
	TestTrue(TEXT("the pin takes the new type"), Lit->PinType.PinSubCategoryObject.Get() == TBaseStructure<FVector>::Get());
	TestEqual(TEXT("the wire is kept for the compile to judge"), Lit->LinkedTo.Num(), 1);

	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("a wire the new type no longer fits is a compile error, not dropped"), AnyContains(Errors, TEXT("no longer fits")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesRefusalCompileTest, "CrowdySDK.Editor.ServerObjectNodes.CompileReportsRefusal", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesRefusalCompileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeRefusedDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_ServerVariableChanged* Changed = AddNode<UCrowdyK2Node_ServerVariableChanged>(Rig.Graph, Definition, TEXT("Hidden"));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Changed, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("the compile names the variable and says what to do"), Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("Hidden has no pin")) && Error.Contains(TEXT("mark it BlueprintType"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesHiddenVariableTest, "CrowdySDK.Editor.ServerObjectNodes.HiddenVariableStaysOffPlayers", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesHiddenVariableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	Definition->WatchedFields.Remove(TEXT("Big"));

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestFalse(TEXT("the menu offers nothing for a variable players cannot see"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Big"); }));
	TestTrue(TEXT("the menu still offers a visible one"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Int"); }));

	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_GetServerState* State = AddNode<UCrowdyK2Node_GetServerState>(Rig.Graph, Definition, NAME_None);
	TestNull(TEXT("Get Server State has no pin for it"), State->FindPin(TEXT("Big"), EGPD_Output));
	TestNotNull(TEXT("but keeps the visible ones"), State->FindPin(TEXT("Int"), EGPD_Output));

	UCrowdyK2Node_GetServerVariable* Getter = AddNode<UCrowdyK2Node_GetServerVariable>(Rig.Graph, Definition, TEXT("Big"));
	UCrowdyK2Node_CallServerFunction* Call = AddNode<UCrowdyK2Node_CallServerFunction>(Rig.Graph, Definition, TEXT("Echo"));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Call, UEdGraphSchema_K2::PN_Execute);
	Link(Getter, TEXT("Big"), Call, TEXT("Big"));

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("a node on a hidden variable is a compile error naming it"), AnyContains(Errors, TEXT("Big is not Visible to Players")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStarterVariablesTest, "CrowdySDK.Editor.ServerObjectNodes.StarterVariablesReachPlayers", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesStarterVariablesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	Definition->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Definition->WatchedFields.Reset();

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestTrue(TEXT("a starter's snapshot carries its whole State, so the menu offers Get for each variable"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry)
	{
		return Entry.Member == TEXT("Int") && Entry.NodeClass == UCrowdyK2Node_GetServerVariable::StaticClass();
	}));

	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_ServerVariableChanged* Changed = AddNode<UCrowdyK2Node_ServerVariableChanged>(Rig.Graph, Definition, TEXT("Int"));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Changed, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestFalse(TEXT("On Changed on a starter's variable is no Visible to Players error"), AnyContains(Errors, TEXT("is not Visible to Players")));

	UCrowdyServerObjectDefinition* World = NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage());
	World->State = FCrowdyKitWorldConditions::StaticStruct();
	World->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	World->KitStateTopic = TEXT("world");
	World->bOnlyOneInstance = true;
	World->TypeName = TEXT("world");
	TArray<FString> BakeErrors;
	TestTrue(TEXT("the world bakes"), World->Bake(BakeErrors));
	Entries.Reset();
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(World, Entries);
	auto Offers = [&Entries](const TCHAR* Member, UClass* NodeClass)
	{
		return Entries.ContainsByPredicate([Member, NodeClass](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == Member && Entry.NodeClass == NodeClass; });
	};
	TestTrue(TEXT("a presence flag can be read"), Offers(TEXT("bHasOnline"), UCrowdyK2Node_GetServerVariable::StaticClass()));
	TestFalse(TEXT("but has no On Changed of its own, since its changes are reported as its value's"), Offers(TEXT("bHasOnline"), UCrowdyK2Node_ServerVariableChanged::StaticClass()));
	TestTrue(TEXT("its value has one"), Offers(TEXT("Online"), UCrowdyK2Node_ServerVariableChanged::StaticClass()));

	FCompileRig FlagRig = MakeActorBlueprint();
	UCrowdyK2Node_ServerVariableChanged* FlagChanged = AddNode<UCrowdyK2Node_ServerVariableChanged>(FlagRig.Graph, World, TEXT("bHasOnline"));
	Link(FlagRig.BeginPlay, UEdGraphSchema_K2::PN_Then, FlagChanged, UEdGraphSchema_K2::PN_Execute);
	const TArray<FString> FlagErrors = Compile(FlagRig.Blueprint, ErrorCount);
	TestTrue(TEXT("a pasted On Changed on the flag says which node to use"), AnyContains(FlagErrors, TEXT("bHasOnline changes together with Online")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageMenuTest, "CrowdySDK.Editor.ServerObjectNodes.MessageMenuEntryPerMessage", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageMenuTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	FCrowdyServerMessage& Refusing = Definition->Messages.AddDefaulted_GetRef();
	Refusing.Name = TEXT("Refusing");
	Refusing.Payload = FCrowdyExecNodesTestRefused::StaticStruct();
	FCrowdyServerMessage& Quiet = Definition->Messages.AddDefaulted_GetRef();
	Quiet.Name = TEXT("Quiet");
	Quiet.bPlayersCanListen = false;

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TArray<FCrowdyServerObjectMenuEntry> Received = Entries.FilterByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.NodeClass == UCrowdyK2Node_ServerMessageReceived::StaticClass(); });
	TestEqual(TEXT("the menu offers one On Received per message whose payload has every pin"), Received.Num(), 3);
	const FString Asset = Definition->GetName();
	for (const TCHAR* Message : {TEXT("HitTaken"), TEXT("LootDropped"), TEXT("Won")})
	{
		const bool bOffered = Received.ContainsByPredicate([Message, &Asset](const FCrowdyServerObjectMenuEntry& Entry)
		{
			return Entry.Member == FName(Message) && Entry.Title.ToString() == FString::Printf(TEXT("On %s Received (%s)"), Message, *Asset);
		});
		TestTrue(FString::Printf(TEXT("%s is offered, titled for the message"), Message), bOffered);
	}
	TestFalse(TEXT("a message whose payload has a value without a pin is not offered"), Received.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Refusing"); }));
	TestFalse(TEXT("a message players cannot listen to is not offered"), Received.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Quiet"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessagePlayersTest, "CrowdySDK.Editor.ServerObjectNodes.MessageForServerObjectsOnlyIsACompileError", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessagePlayersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	Definition->Messages.FindByPredicate([](const FCrowdyServerMessage& Message) { return Message.Name == TEXT("Won"); })->bPlayersCanListen = false;
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Won = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("Won"), FVector2D(300, 0));
	if (!TestNotNull(TEXT("the spawn helper still places the node"), Won))
	{
		return false;
	}
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Won, UEdGraphSchema_K2::PN_Execute);
	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("a placed node fails to compile, saying what to turn on"), AnyContains(Errors, TEXT("Won is heard only by other Server Object types; turn on Players Can Listen")));

	Definition->Messages.FindByPredicate([](const FCrowdyServerMessage& Message) { return Message.Name == TEXT("Won"); })->bPlayersCanListen = true;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("and compiles once players can listen"), ErrorCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessagePinsTest, "CrowdySDK.Editor.ServerObjectNodes.MessageReceivedPins", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessagePinsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_ServerMessageReceived* Hit = AddNode<UCrowdyK2Node_ServerMessageReceived>(Graph, Definition, TEXT("HitTaken"));
	UCrowdyK2Node_ServerMessageReceived* Loot = AddNode<UCrowdyK2Node_ServerMessageReceived>(Graph, Definition, TEXT("LootDropped"));
	UCrowdyK2Node_ServerMessageReceived* Won = AddNode<UCrowdyK2Node_ServerMessageReceived>(Graph, Definition, TEXT("Won"));

	for (const UCrowdyK2Node_ServerMessageReceived* Node : {Hit, Loot, Won})
	{
		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		const UEdGraphPin* Bind = Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		TestTrue(Title + TEXT(" has an exec Bind"), Bind && Bind->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
		TestTrue(Title + TEXT(" has Then and Received execs"), Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) && Node->FindPin(CrowdyServerObjectNodePins::Received, EGPD_Output));
		TestNotNull(Title + TEXT(" takes a Target"), Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
		const UEdGraphPin* ReceivedPin = Node->FindPin(CrowdyServerObjectNodePins::Received, EGPD_Output);
		TestTrue(Title + TEXT(" shows its internal Message Received pin as Received"), ReceivedPin && ReceivedPin->PinFriendlyName.ToString() == TEXT("Received"));
	}
	TestEqual(TEXT("the title names the message and the asset"), Hit->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), FString::Printf(TEXT("On HitTaken Received (%s)"), *Definition->GetName()));

	CheckPin(*this, TEXT("struct payload"), Hit, {TEXT("Damage"), UEdGraphSchema_K2::PC_Int}, EGPD_Output);
	CheckPin(*this, TEXT("struct payload"), Hit, {TEXT("Source"), UEdGraphSchema_K2::PC_Name}, EGPD_Output);
	CheckPin(*this, TEXT("List payload"), Loot, {TEXT("Gold"), UEdGraphSchema_K2::PC_Int}, EGPD_Output);
	CheckPin(*this, TEXT("List payload"), Loot, {TEXT("Items"), UEdGraphSchema_K2::PC_String, nullptr, NAME_None, EPinContainerType::Array}, EGPD_Output);
	for (const TCHAR* Field : {TEXT("Gold"), TEXT("Items")})
	{
		const UEdGraphPin* Pin = Loot->FindPin(Field, EGPD_Output);
		TestTrue(FString::Printf(TEXT("the List entry %s keeps its id on its pin, so a rename keeps the wire"), Field), Pin && Pin->PersistentGuid.IsValid());
	}
	TestNull(TEXT("a payload value is not on another message"), Hit->FindPin(TEXT("Gold"), EGPD_Output));
	const int32 WonOutputs = Won->Pins.FilterByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Output; }).Num();
	TestEqual(TEXT("a message with no payload has only Then and Received"), WonOutputs, 2);

	TArray<UStruct*> Dependencies;
	TestTrue(TEXT("the node depends on the definition"), Hit->HasExternalDependencies(&Dependencies));
	TestTrue(TEXT("and on the payload struct, so its Blueprint recompiles when the struct changes"), Dependencies.Contains(FCrowdyExecNodesTestHit::StaticStruct()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessagePinTypesTest, "CrowdySDK.Editor.ServerObjectNodes.MessageReceivedPinsEveryKind", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessagePinTypesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeEveryTypeDefinition();
	FCrowdyServerMessage& Everything = Definition->Messages.AddDefaulted_GetRef();
	Everything.Name = TEXT("Everything");
	Everything.Payload = FCrowdyExecNodesTestEveryType::StaticStruct();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_ServerMessageReceived* Node = AddNode<UCrowdyK2Node_ServerMessageReceived>(Graph, Definition, TEXT("Everything"));
	for (const FExpectedPin& Row : EveryTypePins())
	{
		CheckPin(*this, TEXT("On Received"), Node, Row, EGPD_Output);
	}
	TestNull(TEXT("a transient field travels nowhere, so it has no pin"), Node->FindPin(TEXT("Cache")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesReceivedVariableTest, "CrowdySDK.Editor.ServerObjectNodes.VariableNamedReceivedKeepsItsPin", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesReceivedVariableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeListDefinition(TEXT("Received"), EPropertyBagPropertyType::Int32);
	UEdGraph* Graph = MakeGraph();
	TestNotNull(TEXT("Get keeps the pin of a variable called Received"), AddNode<UCrowdyK2Node_GetServerVariable>(Graph, Definition, TEXT("Received"))->FindPin(TEXT("Received"), EGPD_Output));
	TestNotNull(TEXT("and so does On Changed"), AddNode<UCrowdyK2Node_ServerVariableChanged>(Graph, Definition, TEXT("Received"))->FindPin(TEXT("Received"), EGPD_Output));
	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestTrue(TEXT("and the menu still offers it"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Received"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageClashTest, "CrowdySDK.Editor.ServerObjectNodes.MessagePayloadNamedReceivedKeepsItsPin", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageClashTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_ReceivedClash"));
	Definition->TypeName = TEXT("received_clash");
	FCrowdyServerMessage& Message = Definition->Messages.AddDefaulted_GetRef();
	Message.Name = TEXT("Ping");
	Message.PayloadForm = ECrowdyServerValuesForm::List;
	Message.PayloadList.AddProperty(TEXT("Received"), EPropertyBagPropertyType::Int32);

	TArray<FCrowdyServerValuePin> Values;
	CrowdyServerObjectPins::GatherValues(Message.GetPayloadStruct(), Values);
	const FCrowdyServerValuePin* Plain = CrowdyServerObjectPins::FindValue(Values, FGuid(), TEXT("Received"));
	if (!TestNotNull(TEXT("Received is a payload value"), Plain))
	{
		return false;
	}
	TestTrue(TEXT("a payload value called Received keeps its pin beside the node's own Received"), Plain->HasPin());

	TArray<FCrowdyServerObjectMenuEntry> Entries;
	UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(Definition, Entries);
	TestTrue(TEXT("and the menu offers its On Received"), Entries.ContainsByPredicate([](const FCrowdyServerObjectMenuEntry& Entry) { return Entry.Member == TEXT("Ping"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageCompileTest, "CrowdySDK.Editor.ServerObjectNodes.MessageReceivedCompiles", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageCompileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Hit = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("HitTaken"), FVector2D(300, 0));
	UEdGraphNode* Loot = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("LootDropped"), FVector2D(500, 0));
	UEdGraphNode* Won = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("Won"), FVector2D(700, 0));
	if (!TestNotNull(TEXT("the spawn helper places a struct payload node"), Hit) || !TestNotNull(TEXT("a List payload node"), Loot) || !TestNotNull(TEXT("and a no payload node"), Won))
	{
		return false;
	}
	UK2Node_CallFunction* PrintHit = AddPrint(Rig.Graph);
	UK2Node_CallFunction* PrintLoot = AddPrint(Rig.Graph);
	UK2Node_CallFunction* PrintWon = AddPrint(Rig.Graph);
	UK2Node_CallFunction* Damage = AddCall(Rig.Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
	UK2Node_CallFunction* Gold = AddCall(Rig.Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Hit, UEdGraphSchema_K2::PN_Execute);
	Link(Hit, UEdGraphSchema_K2::PN_Then, Loot, UEdGraphSchema_K2::PN_Execute);
	Link(Loot, UEdGraphSchema_K2::PN_Then, Won, UEdGraphSchema_K2::PN_Execute);
	Link(Hit, CrowdyServerObjectNodePins::Received, PrintHit, UEdGraphSchema_K2::PN_Execute);
	Link(Hit, TEXT("Damage"), Damage, TEXT("InInt"));
	Link(Damage, UEdGraphSchema_K2::PN_ReturnValue, PrintHit, TEXT("InString"));
	Link(Loot, CrowdyServerObjectNodePins::Received, PrintLoot, UEdGraphSchema_K2::PN_Execute);
	Link(Loot, TEXT("Gold"), Gold, TEXT("InInt"));
	Link(Gold, UEdGraphSchema_K2::PN_ReturnValue, PrintLoot, TEXT("InString"));
	Link(Won, CrowdyServerObjectNodePins::Received, PrintWon, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	for (const FString& Error : Compile(Rig.Blueprint, ErrorCount))
	{
		AddInfo(FString::Printf(TEXT("compile error: %s"), *Error));
	}
	TestEqual(TEXT("a Blueprint with a struct, a List and a no payload message node compiles without errors"), ErrorCount, 0);

	// Bind fills the handler's first parameter with the payload, so each handler must take one Instanced Struct.
	TArray<FString> Handlers;
	for (TFieldIterator<UFunction> It(Rig.Blueprint->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->GetName().StartsWith(TEXT("OnServerMessage_")))
		{
			continue;
		}
		TArray<FString> Params;
		for (TFieldIterator<FProperty> Param(*It); Param && Param->HasAnyPropertyFlags(CPF_Parm); ++Param)
		{
			Params.Add(Param->GetCPPType());
		}
		Handlers.Add(FString::Join(Params, TEXT(",")));
	}
	TestEqual(TEXT("each message node became an event function"), Handlers.Num(), 3);
	TestEqual(TEXT("each handler takes the payload as one Instanced Struct"), FString::Join(Handlers, TEXT(" | ")), FString(TEXT("FInstancedStruct | FInstancedStruct | FInstancedStruct")));

	const UFunction* Ubergraph = UbergraphOf(Rig.Blueprint);
	TestTrue(TEXT("the node binds through Bind Server Message"), CallsFunction(Ubergraph, UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, BindServerMessage))));
	TestTrue(TEXT("and reads a wired payload value with Get Server Value"), CallsFunction(Ubergraph, UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, GetServerValue))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageOrderTest, "CrowdySDK.Editor.ServerObjectNodes.MessageReceivedRunsAfterTheReads", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageOrderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Hit = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("HitTaken"), FVector2D(300, 0));
	if (!TestNotNull(TEXT("the spawn helper places the node"), Hit))
	{
		return false;
	}
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	UK2Node_CallFunction* Damage = AddCall(Rig.Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Hit, UEdGraphSchema_K2::PN_Execute);
	Link(Hit, CrowdyServerObjectNodePins::Received, Print, UEdGraphSchema_K2::PN_Execute);
	Link(Hit, TEXT("Damage"), Damage, TEXT("InInt"));
	Link(Damage, UEdGraphSchema_K2::PN_ReturnValue, Print, TEXT("InString"));

	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("the node compiles without errors"), ErrorCount, 0);

	const UFunction* Ubergraph = UbergraphOf(Rig.Blueprint);
	const int32 Read = CallOffset(Ubergraph, UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, GetServerValue)));
	const int32 Ran = CallOffset(Ubergraph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
	if (TestTrue(TEXT("the payload is read and Received runs"), Read != INDEX_NONE && Ran != INDEX_NONE))
	{
		TestTrue(TEXT("Received runs after the payload's values are read"), Read < Ran);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageRemovedTest, "CrowdySDK.Editor.ServerObjectNodes.RemovedMessageIsACompileError", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageRemovedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UEdGraphNode* Won = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Rig.Graph, Definition, TEXT("Won"), FVector2D(300, 0));
	if (!TestNotNull(TEXT("the spawn helper places the node"), Won))
	{
		return false;
	}
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Won, UEdGraphSchema_K2::PN_Execute);
	int32 ErrorCount = 0;
	Compile(Rig.Blueprint, ErrorCount);
	TestEqual(TEXT("the node compiles while its message exists"), ErrorCount, 0);

	Definition->Messages.RemoveAll([](const FCrowdyServerMessage& Message) { return Message.Name == TEXT("Won"); });
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("a message removed from the definition is a compile error naming it"), AnyContains(Errors, TEXT("has no message Won any more. Place it again from the menu, or give the message its old name back.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageRefusalTest, "CrowdySDK.Editor.ServerObjectNodes.MessagePayloadRefusalIsACompileError", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageRefusalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeDefinition(TEXT("DA_RefusedMessage"));
	Definition->TypeName = TEXT("refused_message");
	FCrowdyServerMessage& Refusing = Definition->Messages.AddDefaulted_GetRef();
	Refusing.Name = TEXT("Refusing");
	Refusing.Payload = FCrowdyExecNodesTestRefused::StaticStruct();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_ServerMessageReceived* Node = AddNode<UCrowdyK2Node_ServerMessageReceived>(Rig.Graph, Definition, TEXT("Refusing"));
	Link(Rig.BeginPlay, UEdGraphSchema_K2::PN_Then, Node, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Errors = Compile(Rig.Blueprint, ErrorCount);
	TestTrue(TEXT("the compile names the payload value and says what to do"), Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("Hidden has no pin")) && Error.Contains(TEXT("mark it BlueprintType"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageUnboundTest, "CrowdySDK.Editor.ServerObjectNodes.UnboundMessageWarns", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageUnboundTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	FCompileRig Rig = MakeActorBlueprint();
	UCrowdyK2Node_ServerMessageReceived* Node = AddNode<UCrowdyK2Node_ServerMessageReceived>(Rig.Graph, Definition, TEXT("Won"));
	UK2Node_CallFunction* Print = AddPrint(Rig.Graph);
	Link(Node, CrowdyServerObjectNodePins::Received, Print, UEdGraphSchema_K2::PN_Execute);

	int32 ErrorCount = 0;
	const TArray<FString> Warnings = Compile(Rig.Blueprint, ErrorCount, EMessageSeverity::Warning);
	TestTrue(TEXT("an On Received with nothing on Bind warns that it never runs"), AnyContains(Warnings, TEXT("never runs: connect Bind")));
	TestEqual(TEXT("and is no error"), ErrorCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageRenameTest, "CrowdySDK.Editor.ServerObjectNodes.MessagePayloadRenameKeepsLinks", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageRenameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	UEdGraph* Graph = MakeGraph();
	UCrowdyK2Node_ServerMessageReceived* Node = AddNode<UCrowdyK2Node_ServerMessageReceived>(Graph, Definition, TEXT("LootDropped"));
	UK2Node_CallFunction* Sink = AddCall(Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt));
	if (!TestNotNull(TEXT("the node has a Gold pin"), Node->FindPin(TEXT("Gold"), EGPD_Output)))
	{
		return false;
	}
	Link(Node, TEXT("Gold"), Sink, TEXT("A"));

	FCrowdyServerMessage* Loot = Definition->Messages.FindByPredicate([](const FCrowdyServerMessage& Message) { return Message.Name == TEXT("LootDropped"); });
	TestEqual(TEXT("the List entry is renamed"), Loot->PayloadList.RenameProperty(TEXT("Gold"), TEXT("Coins")), EPropertyBagAlterationResult::Success);
	Node->ReconstructNode();

	TestNull(TEXT("the old pin is gone"), Node->FindPin(TEXT("Gold"), EGPD_Output));
	const UEdGraphPin* Coins = Node->FindPin(TEXT("Coins"), EGPD_Output);
	if (TestNotNull(TEXT("the pin has the new name"), Coins))
	{
		TestTrue(TEXT("the renamed pin keeps its wire"), Coins->LinkedTo.Contains(Sink->FindPinChecked(TEXT("A"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesMessageSpawnMissingTest, "CrowdySDK.Editor.ServerObjectNodes.SpawnRefusesMissingMessage", CrowdyServerObjectNodesTests::Flags)
bool FCrowdyServerObjectNodesMessageSpawnMissingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesTests;
	UCrowdyServerObjectDefinition* Definition = MakeMessageDefinition();
	UEdGraph* Graph = MakeGraph();
	AddExpectedErrorPlain(TEXT("so no"), EAutomationExpectedErrorFlags::Contains, 1);
	TestNull(TEXT("no On Received for a message the definition lacks"), CrowdyExecNodesSpawn::SpawnServerMessageReceived(Graph, Definition, TEXT("Nope"), FVector2D::ZeroVector));
	TestEqual(TEXT("nothing is added to the graph"), Graph->Nodes.Num(), 0);
	TestNotNull(TEXT("a message it has still spawns"), CrowdyExecNodesSpawn::SpawnServerMessageReceived(Graph, Definition, TEXT("Won"), FVector2D::ZeroVector));
	return true;
}

#endif
