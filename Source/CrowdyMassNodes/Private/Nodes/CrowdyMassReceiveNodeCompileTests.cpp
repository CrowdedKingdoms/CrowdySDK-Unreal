#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Nodes/CrowdyK2Node_ReceiveCrowdyEventOnEntity.h"
#include "Nodes/CrowdyK2Node_ReceiveServerValuesChangedOnEntity.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyMassReceiveNodeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UBlueprint* MakeActorBlueprint(const TCHAR* Prefix)
	{
		const FName Name = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(Prefix));
		return FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(), Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	template <typename T>
	T* PlaceNode(UEdGraph* Graph, TFunctionRef<void(T&)> Setup)
	{
		T* Node = NewObject<T>(Graph);
		Setup(*Node);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	// An actor Blueprint declaring Ping, a custom event with Crowdy Replicates ticked: an event a crowd row can receive.
	UClass* MakeEntityClassWithPing()
	{
		UBlueprint* Entity = MakeActorBlueprint(TEXT("CrowdyMassReceiveNodeEntity"));
		PlaceNode<UK2Node_CustomEvent>(FBlueprintEditorUtils::FindEventGraph(Entity), [](UK2Node_CustomEvent& Event)
		{
			Event.CustomFunctionName = TEXT("Ping");
			Event.GetUserDefinedMetaData().SetMetaData(FName(CrowdyRpcMetaKeys::Replicates), FString());
		});
		FKismetEditorUtilities::CompileBlueprint(Entity);
		return Entity->GeneratedClass;
	}

	// Compiles a listener with Bind (the pin PlaceListener returns) wired from Begin Play or left open; counts messages holding Text.
	int32 CountCompileMessages(bool bWireBind, const TCHAR* Text, TFunctionRef<UEdGraphPin*(UEdGraph*)> PlaceListener)
	{
		UBlueprint* Listener = MakeActorBlueprint(TEXT("CrowdyMassReceiveNodeListener"));
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Listener);
		UK2Node_Event* BeginPlay = PlaceNode<UK2Node_Event>(Graph, [](UK2Node_Event& Event)
		{
			Event.EventReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(AActor, ReceiveBeginPlay), AActor::StaticClass());
			Event.bOverrideFunction = true;
		});
		UEdGraphPin* Bind = PlaceListener(Graph);
		if (bWireBind)
		{
			GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(BeginPlay->FindPinChecked(UEdGraphSchema_K2::PN_Then), Bind);
		}

		FCompilerResultsLog Results;
		Results.SetSilentMode(true);
		FKismetEditorUtilities::CompileBlueprint(Listener, EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave, &Results);
		return Results.Messages.FilterByPredicate([Text](const TSharedRef<FTokenizedMessage>& Message)
		{
			return Message->ToText().ToString().Contains(Text);
		}).Num();
	}

	constexpr const TCHAR* NeverReached = TEXT("is never reached");
}

// Expansion moves Bind's links before the merged graph is validated. Checking there again turns the bound half red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassReceiveNodeWarnsOnlyWhenUnboundTest,
	"CrowdySDK.MassNodes.ReceiveNodeWarnsOnlyWhenUnbound", CrowdyMassReceiveNodeTestFlags)
bool FCrowdyMassReceiveNodeWarnsOnlyWhenUnboundTest::RunTest(const FString& Parameters)
{
	UClass* EntityClass = MakeEntityClassWithPing();
	if (!TestNotNull(TEXT("the entity Blueprint compiled into a class with Ping"), EntityClass ? EntityClass->FindFunctionByName(TEXT("Ping")) : nullptr))
	{
		return false;
	}
	// The event is set before the pins exist, as the docs exporter does: the node's own setter rebuilds on a later editor tick.
	auto ReceivePing = [EntityClass](UEdGraph* Graph)
	{
		UCrowdyK2Node_ReceiveCrowdyEventOnEntity* Receive = PlaceNode<UCrowdyK2Node_ReceiveCrowdyEventOnEntity>(Graph, [EntityClass](UCrowdyK2Node_ReceiveCrowdyEventOnEntity& Node)
		{
			const FStructProperty* Reference = FindFProperty<FStructProperty>(UCrowdyK2Node_ReceiveCrowdyEventOnEntity::StaticClass(), TEXT("EventReference"));
			Reference->ContainerPtrToValuePtr<FMemberReference>(&Node)->SetExternalMember(TEXT("Ping"), EntityClass);
		});
		return Receive->GetBindPin();
	};
	TestEqual(TEXT("a node with Bind wired compiles without the never-reached warning"), CountCompileMessages(true, NeverReached, ReceivePing), 0);
	TestEqual(TEXT("a node with nothing wired into Bind still gets it"), CountCompileMessages(false, NeverReached, ReceivePing), 1);
	return true;
}

// The same order of validation and expansion on the server values node, whose Entity Class pin is also moved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerValuesNodeWarnsOnlyWhenUnboundTest,
	"CrowdySDK.MassNodes.ServerValuesNodeWarnsOnlyWhenUnbound", CrowdyMassReceiveNodeTestFlags)
bool FCrowdyMassServerValuesNodeWarnsOnlyWhenUnboundTest::RunTest(const FString& Parameters)
{
	auto ReceiveValues = [](UEdGraph* Graph)
	{
		UCrowdyK2Node_ReceiveServerValuesChangedOnEntity* Receive = PlaceNode<UCrowdyK2Node_ReceiveServerValuesChangedOnEntity>(Graph, [](UCrowdyK2Node_ReceiveServerValuesChangedOnEntity&) {});
		Receive->GetClassPin()->DefaultObject = AActor::StaticClass();
		return Receive->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	};
	TestEqual(TEXT("a node with Bind wired compiles without the never-reached warning"), CountCompileMessages(true, NeverReached, ReceiveValues), 0);
	TestEqual(TEXT("a node with nothing wired into Bind still gets it"), CountCompileMessages(false, NeverReached, ReceiveValues), 1);

	// Entity Class wired from a class variable, with no class picked on the pin.
	auto ReceiveWiredClass = [](UEdGraph* Graph)
	{
		UBlueprint* Listener = FBlueprintEditorUtils::FindBlueprintForGraphChecked(Graph);
		FEdGraphPinType ClassType;
		ClassType.PinCategory = UEdGraphSchema_K2::PC_Class;
		ClassType.PinSubCategoryObject = AActor::StaticClass();
		FBlueprintEditorUtils::AddMemberVariable(Listener, TEXT("WatchedClass"), ClassType);
		UK2Node_VariableGet* Get = PlaceNode<UK2Node_VariableGet>(Graph, [](UK2Node_VariableGet& Node) { Node.VariableReference.SetSelfMember(TEXT("WatchedClass")); });
		UCrowdyK2Node_ReceiveServerValuesChangedOnEntity* Receive = PlaceNode<UCrowdyK2Node_ReceiveServerValuesChangedOnEntity>(Graph, [](UCrowdyK2Node_ReceiveServerValuesChangedOnEntity&) {});
		GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Get->FindPinChecked(TEXT("WatchedClass")), Receive->GetClassPin());
		return Receive->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	};
	TestEqual(TEXT("an Entity Class wired from a variable is not reported missing"), CountCompileMessages(true, TEXT("has no Entity Class"), ReceiveWiredClass), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
