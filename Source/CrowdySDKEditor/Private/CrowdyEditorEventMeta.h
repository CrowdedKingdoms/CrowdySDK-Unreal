#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "Replication/RPC/CrowdyRPC.h"

// Shared editor helpers for Crowdy replicated-event metadata on Blueprint nodes.
// Defined inline in one place (rather than copied into each .cpp) so they collapse
// to a single definition inside Unreal's unity/jumbo translation units instead of
// colliding as duplicate anonymous-namespace symbols.

// True when a Blueprint custom-event / function entry carries the Crowdy "replicates" marker.
inline bool HasCrowdyReplicatesMeta(const FKismetUserDeclaredFunctionMetadata& Meta)
{
	return Meta.HasMetaData(FName(CrowdyRpcMetaKeys::Replicates))
		|| Meta.HasMetaData(FName(CrowdyRpcMetaKeys::LegacyReplicate));
}

// The function name a UK2Node_FunctionEntry generates: its custom-generated name, else the
// referenced member name, else the owning graph's name.
inline FName GetFunctionEntryName(const UK2Node_FunctionEntry* Entry)
{
	if (!Entry)
	{
		return NAME_None;
	}

	if (Entry->CustomGeneratedFunctionName != NAME_None)
	{
		return Entry->CustomGeneratedFunctionName;
	}

	const FName ReferencedName = Entry->FunctionReference.GetMemberName();
	if (ReferencedName != NAME_None)
	{
		return ReferencedName;
	}

	const UEdGraph* Graph = Entry->GetGraph();
	return Graph ? Graph->GetFName() : NAME_None;
}
