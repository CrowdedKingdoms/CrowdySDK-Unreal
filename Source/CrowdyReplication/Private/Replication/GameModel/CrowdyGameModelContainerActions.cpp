// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelContainerActions.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelActionSupport.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyModelErrorText.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Parses a Blueprint-supplied JSON object literal into invoke params. Empty Json yields a null OutParams
	// (the façade sends "{}"); a non-empty, malformed Json fails, since a bad literal is always a caller bug
	// InvokeOnContainer should not silently swallow as "no params".
	bool TryParseParamsJson(const FString& Json, TSharedPtr<FJsonObject>& OutParams)
	{
		if (Json.IsEmpty())
		{
			OutParams = nullptr;
			return true;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, OutParams) && OutParams.IsValid();
	}
}

UCrowdyCreateDataContainerAction* UCrowdyCreateDataContainerAction::CreateDataContainer(UObject* WorldContext,
	const FString& TypeName, const FString& DisplayName, const FString& SessionId, const FString& MetadataJson)
{
	UCrowdyCreateDataContainerAction* Action = NewObject<UCrowdyCreateDataContainerAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypeName = TypeName;
	Action->DisplayName = DisplayName;
	Action->SessionId = SessionId;
	Action->MetadataJson = MetadataJson;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyCreateDataContainerAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(FString());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyCreateDataContainerAction> WeakThis(this);
	Model->CreateDataContainer(TypeName, DisplayName, SessionId, MetadataJson,
		[WeakThis](bool bOk, const FString& ContainerId)
		{
			UCrowdyCreateDataContainerAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(ContainerId);
			}
			else
			{
				Action->Failed.Broadcast(ContainerId);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyPullDataContainerAction* UCrowdyPullDataContainerAction::PullDataContainer(UObject* WorldContext, const FString& ContainerId)
{
	UCrowdyPullDataContainerAction* Action = NewObject<UCrowdyPullDataContainerAction>();
	Action->WorldContextObject = WorldContext;
	Action->ContainerId = ContainerId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyPullDataContainerAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyPullDataContainerAction> WeakThis(this);
	Model->PullDataContainer(ContainerId,
		[WeakThis](bool bOk)
		{
			UCrowdyPullDataContainerAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast();
			}
			else
			{
				Action->Failed.Broadcast();
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyInvokeOnContainerAction* UCrowdyInvokeOnContainerAction::InvokeOnContainer(UObject* WorldContext,
	const FString& ContainerId, const FString& FunctionName, const FString& ParamsJson, const FString& SessionId)
{
	UCrowdyInvokeOnContainerAction* Action = NewObject<UCrowdyInvokeOnContainerAction>();
	Action->WorldContextObject = WorldContext;
	Action->ContainerId = ContainerId;
	Action->FunctionName = FunctionName;
	Action->ParamsJson = ParamsJson;
	Action->SessionId = SessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyInvokeOnContainerAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(false, FString(), CrowdyModelErrorText::SubsystemUnavailable());
		SetReadyToDestroy();
		return;
	}

	TSharedPtr<FJsonObject> Params;
	if (!TryParseParamsJson(ParamsJson, Params))
	{
		Failed.Broadcast(false, FString(), TEXT("ParamsJson is not valid JSON"));
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyInvokeOnContainerAction> WeakThis(this);
	Model->InvokeOnContainer(ContainerId, FunctionName, Params, SessionId,
		[WeakThis](FCrowdyInvokeResult Result)
		{
			UCrowdyInvokeOnContainerAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				Action->Succeeded.Broadcast(Result.bSuccess, Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				// Guarantee a non-empty reason on the Failed pin: use the server/transport error verbatim when the
				// result carries one, else a phrasing that still tells a rolled-back call from a transport failure.
				Action->Failed.Broadcast(Result.bSuccess, Result.ReturnValueJson,
					CrowdyModelErrorText::FromInvokeResult(Result));
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyAddEdgeAction* UCrowdyAddEdgeAction::AddEdge(UObject* WorldContext, const FString& FromContainerId,
	const FString& ToContainerId, const FString& RelationshipType, float Weight, const FString& MetadataJson)
{
	UCrowdyAddEdgeAction* Action = NewObject<UCrowdyAddEdgeAction>();
	Action->WorldContextObject = WorldContext;
	Action->FromContainerId = FromContainerId;
	Action->ToContainerId = ToContainerId;
	Action->RelationshipType = RelationshipType;
	Action->Weight = Weight;
	// Derive "has weight" from a non-zero value: a designer who sets Weight gets a weighted edge with no separate
	// toggle, and leaving Weight at 0 sends an unweighted edge (the common containment-edge case).
	Action->bHasWeight = !FMath::IsNearlyZero(Weight);
	Action->MetadataJson = MetadataJson;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyAddEdgeAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(FCrowdyContainerEdge());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyAddEdgeAction> WeakThis(this);
	Model->AddEdge(FromContainerId, ToContainerId, RelationshipType, Weight, bHasWeight, MetadataJson,
		[WeakThis](bool bOk, const FCrowdyContainerEdge& Edge)
		{
			UCrowdyAddEdgeAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Edge);
			}
			else
			{
				Action->Failed.Broadcast(Edge);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyTraverseAction* UCrowdyTraverseAction::Traverse(UObject* WorldContext, const FString& RootId,
	const FString& RelationshipType, int32 Depth)
{
	UCrowdyTraverseAction* Action = NewObject<UCrowdyTraverseAction>();
	Action->WorldContextObject = WorldContext;
	Action->RootId = RootId;
	Action->RelationshipType = RelationshipType;
	Action->Depth = Depth;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyTraverseAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyContainerRef>(), TArray<FCrowdyContainerEdge>());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyTraverseAction> WeakThis(this);
	Model->Traverse(RootId, RelationshipType, Depth,
		[WeakThis](bool bOk, const TArray<FCrowdyContainerRef>& Nodes, const TArray<FCrowdyContainerEdge>& Edges)
		{
			UCrowdyTraverseAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Nodes, Edges);
			}
			else
			{
				Action->Failed.Broadcast(Nodes, Edges);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyListChildrenAction* UCrowdyListChildrenAction::ListChildren(UObject* WorldContext, const FString& RootId, const FString& RelationshipType)
{
	UCrowdyListChildrenAction* Action = NewObject<UCrowdyListChildrenAction>();
	Action->WorldContextObject = WorldContext;
	Action->RootId = RootId;
	Action->RelationshipType = RelationshipType;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListChildrenAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyContainerRef>());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyListChildrenAction> WeakThis(this);
	Model->ListChildren(RootId, RelationshipType,
		[WeakThis](bool bOk, const TArray<FCrowdyContainerRef>& Children)
		{
			UCrowdyListChildrenAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Children);
			}
			else
			{
				Action->Failed.Broadcast(Children);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdySetDataPropertyAction* UCrowdySetDataPropertyAction::SetDataProperty(UObject* WorldContext,
	const FString& ContainerId, const FString& Key, const FString& ValueType, const FString& ValueJson)
{
	UCrowdySetDataPropertyAction* Action = NewObject<UCrowdySetDataPropertyAction>();
	Action->WorldContextObject = WorldContext;
	Action->ContainerId = ContainerId;
	Action->Key = Key;
	Action->ValueType = ValueType;
	Action->ValueJson = ValueJson;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdySetDataPropertyAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	// ValueJson must be a JSON value literal ("\"Aria\"", "42", "true", "[1,2]"): catch a bare word or an empty
	// string locally instead of paying a server round-trip to be rejected (mirrors InvokeOnContainer's check).
	{
		const FString Probe = FString::Printf(TEXT("{\"v\":%s}"), *ValueJson);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Probe);
		TSharedPtr<FJsonObject> Parsed;
		if (ValueJson.IsEmpty() || !FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			// This node's Failed pin carries no message, so record why on the subsystem where "Get Last Crowdy
			// Model Error" can read it back. The value never reached the server, so nothing else will set it.
			Model->SetLastModelError(TEXT("Set Model Attribute: the value must be a JSON literal, e.g. 42, true, or \"text\"."));
			Failed.Broadcast();
			SetReadyToDestroy();
			return;
		}
	}

	TWeakObjectPtr<UCrowdySetDataPropertyAction> WeakThis(this);
	Model->SetDataProperty(ContainerId, Key, ValueType, ValueJson,
		[WeakThis](bool bOk)
		{
			UCrowdySetDataPropertyAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast();
			}
			else
			{
				Action->Failed.Broadcast();
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyDeleteContainerAction* UCrowdyDeleteContainerAction::DeleteContainer(UObject* WorldContext, const FString& ContainerId)
{
	UCrowdyDeleteContainerAction* Action = NewObject<UCrowdyDeleteContainerAction>();
	Action->WorldContextObject = WorldContext;
	Action->ContainerId = ContainerId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyDeleteContainerAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyDeleteContainerAction> WeakThis(this);
	Model->DeleteContainer(ContainerId, [WeakThis](bool bOk)
	{
		UCrowdyDeleteContainerAction* Action = WeakThis.Get();
		if (!Action)
		{
			return;
		}
		if (bOk)
		{
			Action->Succeeded.Broadcast();
		}
		else
		{
			Action->Failed.Broadcast();
		}
		Action->SetReadyToDestroy();
	});
}

UCrowdyDeleteEdgeAction* UCrowdyDeleteEdgeAction::DeleteEdge(UObject* WorldContext, const FString& EdgeId)
{
	UCrowdyDeleteEdgeAction* Action = NewObject<UCrowdyDeleteEdgeAction>();
	Action->WorldContextObject = WorldContext;
	Action->EdgeId = EdgeId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyDeleteEdgeAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyDeleteEdgeAction> WeakThis(this);
	Model->DeleteEdge(EdgeId, [WeakThis](bool bOk)
	{
		UCrowdyDeleteEdgeAction* Action = WeakThis.Get();
		if (!Action)
		{
			return;
		}
		if (bOk)
		{
			Action->Succeeded.Broadcast();
		}
		else
		{
			Action->Failed.Broadcast();
		}
		Action->SetReadyToDestroy();
	});
}
