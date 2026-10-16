#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "BlueprintActionDatabase.h"
#include "CrowdyExecNodesSpawn.h"
#include "CrowdyExecNodesTestTypes.h"
#include "CrowdyK2Node_CallServerFunction.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace CrowdyServerObjectNodesLoadTests
{
	bool Save(UObject* Asset)
	{
		UPackage* Package = Asset->GetPackage();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *File, Args);
	}

	UCrowdyServerObjectDefinition* MakeAsset(const FString& Root, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(Root / Name));
		return NewObject<UCrowdyServerObjectDefinition>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	}

	UCrowdyServerObjectDefinition* MakeListAsset(const FString& Root)
	{
		UCrowdyServerObjectDefinition* Definition = MakeAsset(Root, TEXT("DA_LoadList"));
		Definition->TypeName = TEXT("load_list");
		Definition->StateForm = ECrowdyServerValuesForm::List;
		Definition->StateList.AddProperty(TEXT("Total"), EPropertyBagPropertyType::Int32);
		Definition->WatchedFields.Add(TEXT("Total"));
		FCrowdyServerFunction& Tip = Definition->Functions.AddDefaulted_GetRef();
		Tip.Name = TEXT("Tip");
		Tip.ParamsForm = ECrowdyServerValuesForm::List;
		Tip.ParamsList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		Tip.ReplyForm = ECrowdyServerValuesForm::List;
		Tip.ReplyList.AddProperty(TEXT("Total"), EPropertyBagPropertyType::Int32);
		FCrowdyServerMessage& Tipped = Definition->Messages.AddDefaulted_GetRef();
		Tipped.Name = TEXT("Tipped");
		Tipped.PayloadForm = ECrowdyServerValuesForm::List;
		Tipped.PayloadList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		return Definition;
	}

	UCrowdyServerObjectDefinition* MakeStructAsset(const FString& Root)
	{
		UCrowdyServerObjectDefinition* Definition = MakeAsset(Root, TEXT("DA_LoadStruct"));
		Definition->TypeName = TEXT("load_struct");
		Definition->State = FCrowdyExecNodesTestEveryType::StaticStruct();
		Definition->WatchedFields = {TEXT("MaybeString"), TEXT("Hit")};
		FCrowdyServerFunction& Echo = Definition->Functions.AddDefaulted_GetRef();
		Echo.Name = TEXT("Echo");
		Echo.Params = FCrowdyExecNodesTestEveryType::StaticStruct();
		Echo.Reply = FCrowdyExecNodesTestEveryType::StaticStruct();
		FCrowdyServerMessage& Struck = Definition->Messages.AddDefaulted_GetRef();
		Struck.Name = TEXT("Struck");
		Struck.Payload = FCrowdyExecNodesTestHit::StaticStruct();
		return Definition;
	}

	// Every kind of node on Definition, with each exec chained from Then so none of them warns.
	UEdGraphNode* SpawnEveryNode(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Variable, FName Function, FName Message, UEdGraphNode* Previous, int32 Y)
	{
		UEdGraphNode* Changed = CrowdyExecNodesSpawn::SpawnServerVariableChanged(Graph, Definition, Variable, FVector2D(300, Y));
		UEdGraphNode* Received = CrowdyExecNodesSpawn::SpawnServerMessageReceived(Graph, Definition, Message, FVector2D(450, Y));
		UEdGraphNode* Call = CrowdyExecNodesSpawn::SpawnServerFunctionCall(Graph, Definition, Function, FVector2D(600, Y));
		CrowdyExecNodesSpawn::SpawnServerVariableGetter(Graph, Definition, Variable, FVector2D(300, Y + 200));
		CrowdyExecNodesSpawn::SpawnServerState(Graph, Definition, FVector2D(600, Y + 200));
		if (!Changed || !Received || !Call)
		{
			return nullptr;
		}
		Previous->FindPinChecked(UEdGraphSchema_K2::PN_Then, EGPD_Output)->MakeLinkTo(Changed->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		Changed->FindPinChecked(UEdGraphSchema_K2::PN_Then, EGPD_Output)->MakeLinkTo(Received->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		Received->FindPinChecked(UEdGraphSchema_K2::PN_Then, EGPD_Output)->MakeLinkTo(Call->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		return Call;
	}

	// Each pin's name, direction, full type, defaults and rename id, which a rebuild must reproduce exactly.
	FString DescribePins(const UEdGraphNode* Node)
	{
		TArray<FString> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			const FEdGraphPinType& Type = Pin->PinType;
			Pins.Add(FString::Printf(TEXT("%s %d %s/%s/%s/%d %s|%s|%s %s"), *Pin->PinName.ToString(), int32(Pin->Direction), *Type.PinCategory.ToString(), *Type.PinSubCategory.ToString(),
				*GetPathNameSafe(Type.PinSubCategoryObject.Get()), int32(Type.ContainerType), *Pin->DefaultValue, *Pin->AutogeneratedDefaultValue, *GetPathNameSafe(Pin->DefaultObject),
				*Pin->PersistentGuid.ToString()));
		}
		return FString::Join(Pins, TEXT("; "));
	}

	TArray<UEdGraphNode*> AllNodes(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<UEdGraphNode*> Nodes;
		for (const UEdGraph* Graph : Graphs)
		{
			Nodes.Append(Graph->Nodes);
		}
		return Nodes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesSavedBlueprintTest, "CrowdySDK.Editor.ServerObjectNodes.SavedBlueprintOpensUpToDate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCrowdyServerObjectNodesSavedBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesLoadTests;
	UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the editor runs"), Editors))
	{
		return false;
	}
	const FString Root = FString::Printf(TEXT("/Temp/CrowdyExecNodesLoadTest_%s"), *FGuid::NewGuid().ToString());
	UCrowdyServerObjectDefinition* List = MakeListAsset(Root);
	UCrowdyServerObjectDefinition* Struct = MakeStructAsset(Root);
	ON_SCOPE_EXIT
	{
		FBlueprintActionDatabase::Get().ClearAssetActions(List);
		FBlueprintActionDatabase::Get().ClearAssetActions(Struct);
		FText Error;
		UPackageTools::UnloadPackages({List->GetPackage(), Struct->GetPackage()}, Error, true);
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	};

	UPackage* Package = CreatePackage(*(Root / TEXT("BP_LoadTest")));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_LoadTest"), BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	int32 PositionY = 0;
	UK2Node_Event* BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
	BeginPlay = BeginPlay ? BeginPlay : FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, TEXT("ReceiveBeginPlay"), AActor::StaticClass(), PositionY);
	UEdGraphNode* ListCall = SpawnEveryNode(Graph, List, TEXT("Total"), TEXT("Tip"), TEXT("Tipped"), BeginPlay, 0);
	UEdGraphNode* StructCall = ListCall ? SpawnEveryNode(Graph, Struct, TEXT("Hit"), TEXT("Echo"), TEXT("Struck"), ListCall, 600) : nullptr;
	if (!TestNotNull(TEXT("every node is placed"), StructCall))
	{
		return false;
	}
	ListCall->FindPinChecked(CrowdyServerCallPins::Timeout, EGPD_Input)->DefaultValue = TEXT("2.5");
	FCompilerResultsLog Results;
	Results.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	TestEqual(TEXT("the Blueprint compiles clean before it is saved"), int32(Blueprint->Status), int32(BS_UpToDate));
	if (!TestTrue(TEXT("the definitions and the Blueprint save"), Save(List) && Save(Struct) && Save(Blueprint)))
	{
		return false;
	}
	const FString BlueprintPath = Blueprint->GetPathName();

	FText UnloadError;
	TestTrue(TEXT("the Blueprint unloads"), UPackageTools::UnloadPackages({Package}, UnloadError, true));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNull(TEXT("nothing of the Blueprint is left in memory"), FindObject<UBlueprint>(nullptr, *BlueprintPath));

	UBlueprint* Loaded = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!TestNotNull(TEXT("the Blueprint loads back from disk"), Loaded))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Editors->CloseAllEditorsForAsset(Loaded);
		UPackageTools::UnloadPackages({Loaded->GetPackage()}, UnloadError, true);
	};
	TestEqual(TEXT("its compile on load leaves it up to date"), int32(Loaded->Status), int32(BS_UpToDate));
	for (const UEdGraphNode* Node : AllNodes(Loaded))
	{
		TestFalse(FString::Printf(TEXT("%s loads with no compiler message"), *Node->GetName()), Node->bHasCompilerMessage);
		TestTrue(FString::Printf(TEXT("%s keeps its undo flag, which the editor otherwise adds on open and marks the Blueprint changed"), *Node->GetName()),
			Node->HasAnyFlags(RF_Transactional));
	}

	Editors->OpenEditorForAsset(Loaded);
	TestEqual(TEXT("opening it in the Blueprint editor leaves it up to date"), int32(Loaded->Status), int32(BS_UpToDate));
	TestFalse(TEXT("and leaves its package unchanged"), Loaded->GetPackage()->IsDirty());

	int32 TypedNodes = 0;
	for (UEdGraphNode* Node : AllNodes(Loaded))
	{
		if (!Node->IsA<UCrowdyK2Node_ServerObjectBase>())
		{
			continue;
		}
		++TypedNodes;
		const FString Before = DescribePins(Node);
		Node->ReconstructNode();
		TestEqual(FString::Printf(TEXT("%s rebuilds the pins it loaded with"), *Node->GetName()), DescribePins(Node), Before);
	}
	TestEqual(TEXT("all ten typed nodes load"), TypedNodes, 10);

	int32 TipCalls = 0;
	for (const UEdGraphNode* Node : AllNodes(Loaded))
	{
		const UCrowdyK2Node_CallServerFunction* Call = Cast<UCrowdyK2Node_CallServerFunction>(Node);
		if (!Call || Call->Member != TEXT("Tip"))
		{
			continue;
		}
		++TipCalls;
		TestEqual(TEXT("a Timeout set before the save loads back"), Call->FindPinChecked(CrowdyServerCallPins::Timeout, EGPD_Input)->DefaultValue, FString(TEXT("2.5")));
	}
	TestEqual(TEXT("the Tip call loads"), TipCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesDefinitionLoadsLaterTest, "CrowdySDK.Editor.ServerObjectNodes.BlueprintLoadedBeforeItsDefinitionKeepsItsPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCrowdyServerObjectNodesDefinitionLoadsLaterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesLoadTests;
	const FString Root = FString::Printf(TEXT("/Temp/CrowdyExecNodesLoadTest_%s"), *FGuid::NewGuid().ToString());
	UCrowdyServerObjectDefinition* List = MakeListAsset(Root);
	UPackage* Package = CreatePackage(*(Root / TEXT("BP_LoadLater")));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_LoadLater"), BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	const FString BlueprintPath = Blueprint->GetPathName();
	const FString DefinitionPath = List->GetPathName();
	// By path, since the objects are unloaded and loaded again on the way: whatever is in memory at the end goes.
	ON_SCOPE_EXIT
	{
		TArray<UPackage*> Packages;
		if (UCrowdyServerObjectDefinition* Definition = FindObject<UCrowdyServerObjectDefinition>(nullptr, *DefinitionPath))
		{
			FBlueprintActionDatabase::Get().ClearAssetActions(Definition);
			Packages.Add(Definition->GetPackage());
		}
		if (UBlueprint* Left = FindObject<UBlueprint>(nullptr, *BlueprintPath))
		{
			Packages.Add(Left->GetPackage());
		}
		FText Error;
		UPackageTools::UnloadPackages(Packages, Error, true);
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	};
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	int32 PositionY = 0;
	UK2Node_Event* BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
	BeginPlay = BeginPlay ? BeginPlay : FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, TEXT("ReceiveBeginPlay"), AActor::StaticClass(), PositionY);
	if (!TestNotNull(TEXT("every node is placed"), SpawnEveryNode(Graph, List, TEXT("Total"), TEXT("Tip"), TEXT("Tipped"), BeginPlay, 0)))
	{
		return false;
	}
	FCompilerResultsLog Results;
	Results.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	TMap<FString, FString> PinsBefore;
	for (const UEdGraphNode* Node : AllNodes(Blueprint))
	{
		if (Node->IsA<UCrowdyK2Node_ServerObjectBase>())
		{
			PinsBefore.Add(Node->GetName(), DescribePins(Node));
		}
	}
	if (!TestTrue(TEXT("the definition and the Blueprint save"), Save(List) && Save(Blueprint)))
	{
		return false;
	}

	// Unloading the definition too means loading the Blueprint is what brings it back, so its nodes meet it mid-load.
	FBlueprintActionDatabase::Get().ClearAssetActions(List);
	FText UnloadError;
	TestTrue(TEXT("the Blueprint and the definition unload"), UPackageTools::UnloadPackages({Package, List->GetPackage()}, UnloadError, true));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNull(TEXT("the definition is no longer in memory"), FindObject<UCrowdyServerObjectDefinition>(nullptr, *DefinitionPath));

	UBlueprint* Loaded = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!TestNotNull(TEXT("the Blueprint loads back from disk"), Loaded))
	{
		return false;
	}
	TestNotNull(TEXT("loading the Blueprint loaded its definition"), FindObject<UCrowdyServerObjectDefinition>(nullptr, *DefinitionPath));
	TestEqual(TEXT("its compile on load leaves it up to date"), int32(Loaded->Status), int32(BS_UpToDate));
	int32 Compared = 0;
	for (const UEdGraphNode* Node : AllNodes(Loaded))
	{
		const FString* Before = PinsBefore.Find(Node->GetName());
		if (!Before || !Node->IsA<UCrowdyK2Node_ServerObjectBase>())
		{
			continue;
		}
		++Compared;
		TestEqual(FString::Printf(TEXT("%s loads with the pins it was saved with"), *Node->GetName()), DescribePins(Node), *Before);
	}
	TestEqual(TEXT("all five typed nodes load"), Compared, 5);
	return true;
}

#endif
