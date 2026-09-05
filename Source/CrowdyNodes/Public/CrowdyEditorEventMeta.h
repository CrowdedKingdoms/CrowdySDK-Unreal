#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
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

// The recipient a Crowdy replicated event routes to, from the raw metadata string an author's choice
// is stored as. An unset or unrecognised value is SpatialMulticast, which is what the runtime defaults
// to (FCrowdyFnInfo::Recipient), so a node that was never given a recipient reads the same here as it
// behaves at runtime. "OwningPlayer" is the older spelling of OwningClient and still appears on assets
// authored before the rename.
inline ECrowdyEventRecipient ResolveCrowdyRecipient(const FString& RecipientMetaValue)
{
	if (RecipientMetaValue.Equals(TEXT("OwningPlayer"), ESearchCase::IgnoreCase)
		|| RecipientMetaValue.Equals(TEXT("Owning Player"), ESearchCase::IgnoreCase))
	{
		return ECrowdyEventRecipient::OwningClient;
	}

	const UEnum* EnumType = StaticEnum<ECrowdyEventRecipient>();
	const int64 Value = EnumType ? EnumType->GetValueByNameString(RecipientMetaValue) : INDEX_NONE;
	return Value == INDEX_NONE
		? ECrowdyEventRecipient::SpatialMulticast
		: static_cast<ECrowdyEventRecipient>(Value);
}

// The recipient a Crowdy replicated custom event node carries, read off the node's own user-declared
// metadata rather than off the generated function: a node in the Blueprint being compiled has not been
// stamped yet at ProcessBlueprintCompiled time, so the function's metadata is not readable there.
inline ECrowdyEventRecipient ResolveCrowdyRecipient(const FKismetUserDeclaredFunctionMetadata& Meta)
{
	const FName Key(CrowdyRpcMetaKeys::Recipient);
	return ResolveCrowdyRecipient(Meta.HasMetaData(Key) ? Meta.GetMetaData(Key) : FString());
}
