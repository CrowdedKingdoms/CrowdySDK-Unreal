#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectTypes.h"

class UCrowdyServerObjectDefinition;
class UEdGraph;
class UEdGraphNode;

/** The pin names Server Object nodes reserve; value pins are named after their variable, input or output. */
namespace CrowdyServerObjectNodePins
{
	CROWDYEXECNODES_API extern const FName InstanceId;
	CROWDYEXECNODES_API extern const FName TeamId;
	CROWDYEXECNODES_API extern const FName Changed;

	/** Get Server State's advanced pin, false until the server has sent the object's values. */
	CROWDYEXECNODES_API extern const FName HasValues;
}

namespace CrowdyServerCallPins
{
	CROWDYEXECNODES_API extern const FName FunctionPicker;
	CROWDYEXECNODES_API extern const FName OnSuccess;
	CROWDYEXECNODES_API extern const FName OnFailed;
	CROWDYEXECNODES_API extern const FName Outcome;
	CROWDYEXECNODES_API extern const FName Reason;
	CROWDYEXECNODES_API extern const FName Retryable;
}

/** Places Server Object nodes in a graph from C++, as the action menu does; null, with a warning, when the definition lacks the variable or function. */
namespace CrowdyExecNodesSpawn
{
	CROWDYEXECNODES_API UEdGraphNode* SpawnServerVariableGetter(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Variable, FVector2D Position);
	CROWDYEXECNODES_API UEdGraphNode* SpawnServerVariableChanged(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Variable, FVector2D Position);
	CROWDYEXECNODES_API UEdGraphNode* SpawnServerFunctionCall(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FName Function, FVector2D Position);
	CROWDYEXECNODES_API UEdGraphNode* SpawnServerState(UEdGraph* Graph, UCrowdyServerObjectDefinition* Definition, FVector2D Position);

	/** Makes Node find its object by its definition and Instance instead of a Target, and rebuilds its pins. */
	CROWDYEXECNODES_API void SetFindByAsset(UEdGraphNode* Node, ECrowdyServerObjectFind Instance);

	/** The Has Value pin beside an optional value's pin, and Get's advanced one beside any other value. */
	CROWDYEXECNODES_API FName HasValuePinName(FName Value);
}
