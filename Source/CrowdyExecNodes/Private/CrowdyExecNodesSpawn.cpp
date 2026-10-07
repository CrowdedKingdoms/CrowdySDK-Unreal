#include "CrowdyExecNodesSpawn.h"

#include "CrowdyExecLog.h"
#include "CrowdyK2Node_CallServerFunction.h"
#include "CrowdyK2Node_GetServerState.h"
#include "CrowdyK2Node_GetServerVariable.h"
#include "CrowdyK2Node_ServerVariableChanged.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraph/EdGraph.h"

namespace CrowdyExecNodesSpawnPrivate
{
	bool HasVariable(const UCrowdyServerObjectDefinition* Definition, FName Variable)
	{
		TArray<FCrowdyServerValuePin> Variables;
		CrowdyServerObjectPins::GatherVariables(Definition, Variables);
		return CrowdyServerObjectPins::FindValue(Variables, FGuid(), Variable) != nullptr;
	}

	// A node for a member the definition lacks would have no pin for it, so none is placed.
	template <typename NodeType>
	UEdGraphNode* Spawn(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Member, FVector2D Position, bool bExists)
	{
		if (!Graph)
		{
			return nullptr;
		}
		if (!Definition || !bExists)
		{
			UE_LOG(LogCrowdyExec, Warning, TEXT("Server Object definition %s has no member %s, so no %s node is placed"), *GetNameSafe(Definition), *Member.ToString(),
				*NodeType::StaticClass()->GetName());
			return nullptr;
		}
		NodeType* Node = NewObject<NodeType>(Graph);
		Node->SetFlags(RF_Transactional);
		Node->CreateNewGuid();
		Node->Definition = Definition;
		Node->Member = Member;
		Node->NodePosX = FMath::RoundToInt(Position.X);
		Node->NodePosY = FMath::RoundToInt(Position.Y);
		Graph->AddNode(Node, false, false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}
}

UEdGraphNode* CrowdyExecNodesSpawn::SpawnServerVariableGetter(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Variable, FVector2D Position)
{
	const bool bExists = CrowdyExecNodesSpawnPrivate::HasVariable(Definition, Variable);
	return CrowdyExecNodesSpawnPrivate::Spawn<UCrowdyK2Node_GetServerVariable>(Graph, Definition, Variable, Position, bExists);
}

UEdGraphNode* CrowdyExecNodesSpawn::SpawnServerVariableChanged(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Variable, FVector2D Position)
{
	const bool bExists = CrowdyExecNodesSpawnPrivate::HasVariable(Definition, Variable);
	return CrowdyExecNodesSpawnPrivate::Spawn<UCrowdyK2Node_ServerVariableChanged>(Graph, Definition, Variable, Position, bExists);
}

UEdGraphNode* CrowdyExecNodesSpawn::SpawnServerFunctionCall(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Function, FVector2D Position)
{
	const bool bExists = Definition && Definition->FindFunction(Function);
	return CrowdyExecNodesSpawnPrivate::Spawn<UCrowdyK2Node_CallServerFunction>(Graph, Definition, Function, Position, bExists);
}

UEdGraphNode* CrowdyExecNodesSpawn::SpawnServerState(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FVector2D Position)
{
	return CrowdyExecNodesSpawnPrivate::Spawn<UCrowdyK2Node_GetServerState>(Graph, Definition, NAME_None, Position, true);
}

void CrowdyExecNodesSpawn::SetFindByAsset(UEdGraphNode* Node, ECrowdyServerObjectFind Instance)
{
	UCrowdyK2Node_ServerObjectBase* ServerNode = Cast<UCrowdyK2Node_ServerObjectBase>(Node);
	if (!ServerNode)
	{
		return;
	}
	ServerNode->bFind = true;
	ServerNode->Instance = Instance;
	ServerNode->ReconstructNode();
}

FName CrowdyExecNodesSpawn::HasValuePinName(FName Value)
{
	return CrowdyServerObjectPins::HasValuePinName(Value);
}
