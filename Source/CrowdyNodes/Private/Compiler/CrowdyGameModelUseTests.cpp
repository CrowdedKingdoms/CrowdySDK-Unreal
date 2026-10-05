#include "CrowdyBlueprintCompilerExtension.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyGameModelUseTests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	constexpr const TCHAR* WarningPrefix = TEXT("[CrowdySDK] This Blueprint uses Game Models");

	// Looked up by path so the deprecated C++ types are never named here.
	UClass* FindClassByPath(const TCHAR* Path)
	{
		return FindObject<UClass>(nullptr, Path);
	}

	UBlueprint* MakeActorBlueprint(const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(
			*FString::Printf(TEXT("/Temp/CrowdyNodesTests/%s_%s"), Name, *FGuid::NewGuid().ToString()));
		return FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	template <typename NodeType>
	NodeType* AddNode(UEdGraph* Graph)
	{
		NodeType* Node = NewObject<NodeType>(Graph);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		return Node;
	}

	UK2Node_CallFunction* AddCall(UEdGraph* Graph, UClass* Class, const FName Function)
	{
		UK2Node_CallFunction* Node = AddNode<UK2Node_CallFunction>(Graph);
		Node->FunctionReference.SetExternalMember(Function, Class);
		return Node;
	}

	FString NodeUse(const UEdGraphNode* Node, const UEdGraph* Graph)
	{
		return FString::Printf(TEXT("%s (%s)"),
			*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Graph->GetName());
	}

	FEdGraphPinType PinTypeOf(const FName Category, UObject* SubCategoryObject = nullptr)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = Category;
		PinType.PinSubCategoryObject = SubCategoryObject;
		return PinType;
	}

	// Every message of one compile that is the Game Model warning, whatever severity it was raised at.
	TArray<TSharedRef<FTokenizedMessage>> CompileForGameModelWarnings(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		TArray<TSharedRef<FTokenizedMessage>> Found;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->ToText().ToString().Contains(WarningPrefix))
			{
				Found.Add(Message);
			}
		}
		return Found;
	}
}

// The classifier covers the libraries, the subsystem, the async actions (Kit included) and the effect and manifest
// asset types, and nothing else in the same module (the binding key interface stays), in Server Compute, or in the engine.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelClassifierTest,
	"CrowdySDK.Compiler.GameModelClassifierMatchesOnlyGameModelTypes", CrowdyGameModelUseTests::Flags)
bool FCrowdyGameModelClassifierTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelUseTests;

	const TCHAR* GameModelPaths[] = {
		TEXT("/Script/CrowdyReplication.CrowdyGameModelSubsystem"),
		TEXT("/Script/CrowdyReplication.CrowdyModel"),
		TEXT("/Script/CrowdyReplication.CrowdyEffects"),
		TEXT("/Script/CrowdyReplication.CrowdyEffect"),
		TEXT("/Script/CrowdyReplication.CrowdyInvokeModelFunctionAction"),
		TEXT("/Script/CrowdyReplication.CrowdySpawnCombatantAction"),
		TEXT("/Script/CrowdyReplication.CrowdyContainerManifest"),
	};

	const TCHAR* OtherPaths[] = {
		TEXT("/Script/CrowdyReplication.CrowdyEntitySubsystem"),
		TEXT("/Script/CrowdyReplication.CrowdyBindingKeyProvider"),
		TEXT("/Script/CrowdyExec.CrowdyServerObjectLibrary"),
		TEXT("/Script/CrowdyExec.CrowdyServerCallAction"),
	};

	for (const TCHAR* Path : GameModelPaths)
	{
		const UClass* Class = FindClassByPath(Path);
		if (!TestNotNull(FString::Printf(TEXT("%s resolves"), Path), Class)) continue;

		TestTrue(FString::Printf(TEXT("%s is a Game Model class"), Path),
			UCrowdyBlueprintCompilerExtension::IsGameModelClass(Class));
	}

	for (const TCHAR* Path : OtherPaths)
	{
		const UClass* Class = FindClassByPath(Path);
		if (!TestNotNull(FString::Printf(TEXT("%s resolves"), Path), Class)) continue;

		TestFalse(FString::Printf(TEXT("%s is not a Game Model class"), Path),
			UCrowdyBlueprintCompilerExtension::IsGameModelClass(Class));
	}

	TestFalse(TEXT("an engine class is not a Game Model class"),
		UCrowdyBlueprintCompilerExtension::IsGameModelClass(AActor::StaticClass()));
	TestFalse(TEXT("no class is not a Game Model class"),
		UCrowdyBlueprintCompilerExtension::IsGameModelClass(nullptr));

	// A class outside the Game Model's module whose source path happens to match is still not one. The metadata
	// is process-wide, so it is put back exactly.
	UClass* Probe = UCrowdyBlueprintCompilerExtension::StaticClass();
	static const FName ModuleRelativePathKey(TEXT("ModuleRelativePath"));
	const FString* SavedPath = Probe->FindMetaData(ModuleRelativePathKey);
	const FString RestorePath = SavedPath ? *SavedPath : FString();
	const bool bHadPath = SavedPath != nullptr;
	ON_SCOPE_EXIT
	{
		if (bHadPath)
		{
			Probe->SetMetaData(ModuleRelativePathKey, *RestorePath);
		}
		else
		{
			Probe->RemoveMetaData(ModuleRelativePathKey);
		}
	};

	Probe->SetMetaData(ModuleRelativePathKey, TEXT("Public/Replication/GameModel/CrowdyNodesProbe.h"));
	TestFalse(TEXT("a matching source path in another module is not a Game Model class"),
		UCrowdyBlueprintCompilerExtension::IsGameModelClass(Probe));

	return true;
}

// Every kind of use is found and named once: an async action, a library call (three of them, folded into a
// count), a bind to a subsystem event, an effect literal on a pin, a pin typed as an effect, a variable typed as
// one, a Server Owned variable and the class's own container tag. A plain call and a plain variable are not uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelUsesAreCollectedTest,
	"CrowdySDK.Compiler.GameModelUsesAreCollected", CrowdyGameModelUseTests::Flags)
bool FCrowdyGameModelUsesAreCollectedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelUseTests;

	UClass* ModelLibrary = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyModel"));
	UClass* Subsystem = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyGameModelSubsystem"));
	UClass* InvokeAction = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyInvokeModelFunctionAction"));
	UClass* EffectClass = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyEffect"));
	if (!TestNotNull(TEXT("UCrowdyModel resolves"), ModelLibrary)
		|| !TestNotNull(TEXT("UCrowdyGameModelSubsystem resolves"), Subsystem)
		|| !TestNotNull(TEXT("UCrowdyInvokeModelFunctionAction resolves"), InvokeAction)
		|| !TestNotNull(TEXT("UCrowdyEffect resolves"), EffectClass))
	{
		return false;
	}

	const UFunction* Factory = InvokeAction->FindFunctionByName(TEXT("CallModelFunction"));
	FMulticastDelegateProperty* SessionChanged =
		FindFProperty<FMulticastDelegateProperty>(Subsystem, TEXT("OnSessionChanged"));
	if (!TestNotNull(TEXT("the Call Model Function factory resolves"), Factory)
		|| !TestNotNull(TEXT("the On Game Session Changed event resolves"), SessionChanged))
	{
		return false;
	}

	UBlueprint* Blueprint = MakeActorBlueprint(TEXT("BP_GameModelUses"));
	UEdGraph* Graph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("the Blueprint's event graph"), Graph))
	{
		return false;
	}

	TestEqual(TEXT("a fresh actor Blueprint uses nothing"),
		UCrowdyBlueprintCompilerExtension::CollectGameModelUses(*Blueprint, Blueprint->GeneratedClass).Num(), 0);

	// Variables first: adding one recompiles the skeleton, which the hand-built nodes below are not ready for.
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("EffectVar"), PinTypeOf(UEdGraphSchema_K2::PC_Object, EffectClass));
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Health"), PinTypeOf(UEdGraphSchema_K2::PC_Int));
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Score"), PinTypeOf(UEdGraphSchema_K2::PC_Int));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(
		Blueprint, TEXT("Health"), nullptr, FName(CrowdyGameModelMetaKeys::Model), FString());

	// The hand-built nodes below cannot compile, so they leave with the test instead of lingering in a loaded asset.
	const int32 FirstHandBuiltNode = Graph->Nodes.Num();
	ON_SCOPE_EXIT
	{
		for (int32 Index = Graph->Nodes.Num() - 1; Index >= FirstHandBuiltNode; --Index)
		{
			Graph->RemoveNode(Graph->Nodes[Index]);
		}
	};

	UK2Node_AsyncAction* AsyncNode = AddNode<UK2Node_AsyncAction>(Graph);
	AsyncNode->InitializeProxyFromFunction(Factory);

	UK2Node_CallFunction* GetIntNode = AddCall(Graph, ModelLibrary, TEXT("GetInt"));
	AddCall(Graph, ModelLibrary, TEXT("GetInt"));
	AddCall(Graph, ModelLibrary, TEXT("GetInt"));
	if (!TestNotNull(TEXT("UCrowdyModel::GetInt resolves through the call node"), GetIntNode->GetTargetFunction()))
	{
		return false;
	}

	UK2Node_AddDelegate* BindNode = AddNode<UK2Node_AddDelegate>(Graph);
	BindNode->SetFromProperty(SessionChanged, false, Subsystem);
	// The node learns its event is deprecated when it resolves the event, as building its pins does.
	TestNotNull(TEXT("the bind node resolves its event"), BindNode->GetProperty());
	TestTrue(TEXT("the bind node carries the event's deprecation"), BindNode->HasDeprecatedReference());

	const TStrongObjectPtr<UObject> Effect(NewObject<UObject>(GetTransientPackage(), EffectClass));
	UK2Node_CallFunction* LiteralNode = AddCall(
		Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	UEdGraphPin* LiteralPin = LiteralNode->CreatePin(
		EGPD_Input, UEdGraphSchema_K2::PC_Object, UObject::StaticClass(), TEXT("Effect"));
	LiteralPin->DefaultObject = Effect.Get();

	UK2Node_Knot* TypedNode = AddNode<UK2Node_Knot>(Graph);
	TypedNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, EffectClass, TEXT("InputPin"));

	AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));

	UClass* GeneratedClass = Blueprint->GeneratedClass;
	GeneratedClass->SetMetaData(CrowdyGameModelMetaKeys::Container, TEXT("CrowdyNodesProbeType"));
	ON_SCOPE_EXIT
	{
		GeneratedClass->RemoveMetaData(CrowdyGameModelMetaKeys::Container);
	};

	const TArray<FString> Uses = UCrowdyBlueprintCompilerExtension::CollectGameModelUses(*Blueprint, GeneratedClass);
	AddInfo(FString::Join(Uses, TEXT(" | ")));

	TestTrue(TEXT("the class's own container tag is named"),
		Uses.Contains(TEXT("Game Model container tag CrowdyNodesProbeType")));
	TestTrue(TEXT("a Server Owned variable is named"), Uses.Contains(TEXT("variable Health (CrowdyModel)")));
	TestTrue(TEXT("a variable typed as an effect is named"), Uses.Contains(TEXT("variable EffectVar (CrowdyEffect)")));
	TestTrue(TEXT("an async action is named"), Uses.Contains(NodeUse(AsyncNode, Graph)));
	TestTrue(TEXT("three calls to one library function are named once, with a count"),
		Uses.Contains(FString::Printf(TEXT("%s x3 (%s)"),
			*GetIntNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Graph->GetName())));
	TestTrue(TEXT("a bind to a subsystem event is named"), Uses.Contains(NodeUse(BindNode, Graph)));
	TestTrue(TEXT("an effect literal on an ordinary call is named, and the plain call beside it is not"),
		Uses.Contains(NodeUse(LiteralNode, Graph)));
	TestTrue(TEXT("a pin typed as an effect is named"), Uses.Contains(NodeUse(TypedNode, Graph)));
	TestEqual(TEXT("nothing else is named"), Uses.Num(), 8);

	return true;
}

// The list is capped so a Blueprint full of uses still yields a readable warning, and no uses yields none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelWarningCapsItsListTest,
	"CrowdySDK.Compiler.GameModelWarningCapsItsList", CrowdyGameModelUseTests::Flags)
bool FCrowdyGameModelWarningCapsItsListTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelUseTests;

	TestTrue(TEXT("no uses, no warning"),
		UCrowdyBlueprintCompilerExtension::DescribeGameModelUses(TArray<FString>()).IsEmpty());

	TArray<FString> Uses;
	for (int32 Index = 0; Index < 14; ++Index)
	{
		Uses.Add(FString::Printf(TEXT("Use%02d"), Index));
	}

	const FString Warning = UCrowdyBlueprintCompilerExtension::DescribeGameModelUses(Uses);
	TestTrue(TEXT("the warning carries the Game Model prefix"), Warning.StartsWith(WarningPrefix));
	TestTrue(TEXT("it points to Server Compute"), Warning.Contains(TEXT("Server Compute")));
	TestTrue(TEXT("the twelfth use is listed"), Warning.Contains(TEXT("Use11")));
	TestFalse(TEXT("the thirteenth is not"), Warning.Contains(TEXT("Use12")));
	TestTrue(TEXT("the rest are counted"), Warning.Contains(TEXT("and 2 more")));

	Uses.SetNum(12);
	TestFalse(TEXT("a list within the cap is not summarised"),
		UCrowdyBlueprintCompilerExtension::DescribeGameModelUses(Uses).Contains(TEXT("more")));

	return true;
}

// A real compile raises exactly one warning for a Blueprint with Game Model uses, naming them, and none for a
// Blueprint without any. The engine's own deprecation warning on the call node is a separate message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelUseWarnsOncePerBlueprintTest,
	"CrowdySDK.Compiler.GameModelUseWarnsOncePerBlueprint", CrowdyGameModelUseTests::Flags)
bool FCrowdyGameModelUseWarnsOncePerBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelUseTests;

	UClass* ModelLibrary = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyModel"));
	if (!TestNotNull(TEXT("UCrowdyModel resolves"), ModelLibrary))
	{
		return false;
	}

	UBlueprint* Blueprint = MakeActorBlueprint(TEXT("BP_GameModelWarning"));
	UEdGraph* Graph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("the Blueprint's event graph"), Graph))
	{
		return false;
	}

	// Abstract, so the separate check that a Server Owned variable sits on a container stays out of the result.
	Blueprint->bGenerateAbstractClass = true;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Health"), PinTypeOf(UEdGraphSchema_K2::PC_Int));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(
		Blueprint, TEXT("Health"), nullptr, FName(CrowdyGameModelMetaKeys::Model), FString());

	UK2Node_CallFunction* GetIntNode = AddCall(Graph, ModelLibrary, TEXT("GetInt"));
	GetIntNode->AllocateDefaultPins();

	const TArray<TSharedRef<FTokenizedMessage>> Warnings = CompileForGameModelWarnings(Blueprint);
	if (!TestEqual(TEXT("one Game Model warning for the whole Blueprint"), Warnings.Num(), 1))
	{
		return false;
	}

	const FString Text = Warnings[0]->ToText().ToString();
	AddInfo(Text);
	TestEqual(TEXT("it is a warning, not an error"),
		static_cast<int32>(Warnings[0]->GetSeverity()), static_cast<int32>(EMessageSeverity::Warning));
	TestTrue(TEXT("it names the call"), Text.Contains(NodeUse(GetIntNode, Graph)));
	TestTrue(TEXT("it names the Server Owned variable"), Text.Contains(TEXT("variable Health (CrowdyModel)")));

	UBlueprint* Clean = MakeActorBlueprint(TEXT("BP_NoGameModel"));
	UEdGraph* CleanGraph = Clean ? FBlueprintEditorUtils::FindEventGraph(Clean) : nullptr;
	if (!TestNotNull(TEXT("the clean Blueprint's event graph"), CleanGraph))
	{
		return false;
	}

	AddCall(CleanGraph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString))
		->AllocateDefaultPins();
	FBlueprintEditorUtils::AddMemberVariable(Clean, TEXT("Score"), PinTypeOf(UEdGraphSchema_K2::PC_Int));

	TestEqual(TEXT("a Blueprint with no Game Model use gets no such warning"),
		CompileForGameModelWarnings(Clean).Num(), 0);

	return true;
}

// The library functions carry the engine's own deprecation marker, so the call node itself warns with the move.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCallNodeIsDeprecatedTest,
	"CrowdySDK.Compiler.GameModelCallNodeIsDeprecated", CrowdyGameModelUseTests::Flags)
bool FCrowdyGameModelCallNodeIsDeprecatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelUseTests;

	UClass* ModelLibrary = FindClassByPath(TEXT("/Script/CrowdyReplication.CrowdyModel"));
	UBlueprint* Blueprint = MakeActorBlueprint(TEXT("BP_GameModelCallNode"));
	UEdGraph* Graph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("UCrowdyModel resolves"), ModelLibrary) || !TestNotNull(TEXT("the event graph"), Graph))
	{
		return false;
	}

	// The compiler prunes an unconnected node before validating it, so the node's own answer is what is checked.
	UK2Node_CallFunction* CallNode = AddCall(Graph, ModelLibrary, TEXT("GetInt"));
	CallNode->AllocateDefaultPins();

	TestTrue(TEXT("the call node reports a deprecated reference"), CallNode->HasDeprecatedReference());
	const FEdGraphNodeDeprecationResponse Response =
		CallNode->GetDeprecationResponse(EEdGraphNodeDeprecationType::NodeHasDeprecatedReference);
	TestEqual(TEXT("as a warning"), static_cast<int32>(Response.MessageType),
		static_cast<int32>(EEdGraphNodeDeprecationMessageType::Warning));
	TestTrue(TEXT("with the move to Server Compute"),
		Response.MessageText.ToString().Contains(TEXT("use a Server Object (Server Compute) instead")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
