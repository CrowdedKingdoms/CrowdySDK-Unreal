// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModel.h"

#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyJsonValueSupport.h"
#include "Replication/GameModel/CrowdyNumericSupport.h"
#include "Serialization/CrowdyJsonSafety.h" // bound a collection item's state JSON before parsing it
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Resolves WorldContext's Game Model subsystem. Null when WorldContext has no world or the subsystem is
	// absent (not a play world), mirroring UCrowdyModel's ResolveModel.
	UCrowdyGameModelSubsystem* ResolveModelSubsystem(const UObject* WorldContext)
	{
		const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
		return World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	}

	// Reads one field's JSON value from a collection item's whole state object ({"quantity":5,"item_id":"sword"}).
	// Null when the JSON is empty, too deeply nested (a forged/hand-authored blob would overflow the stack on the
	// DOM's recursive teardown), unparseable, or the key is absent.
	TSharedPtr<FJsonValue> ReadItemStateField(const FString& StateJson, const FName Key)
	{
		if (StateJson.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(StateJson))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StateJson);
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object->TryGetField(Key.ToString());
	}
}

UCrowdyGameModelSubsystem* UCrowdyGameModel::GetGameModelSubsystem(const UObject* WorldContext)
{
	return ResolveModelSubsystem(WorldContext);
}

int64 UCrowdyGameModel::GetLocalUserId(const UObject* WorldContext)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	return Model ? Model->GetLocalUserId() : 0;
}

bool UCrowdyGameModel::IsMyTurn(const UObject* WorldContext, const FCrowdyGameModelSession& Session)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	return Model && Model->IsLocalUsersTurn(Session);
}

int32 UCrowdyGameModel::GetContainerInt(const UObject* WorldContext, const FString& ContainerId, FName Key, int32 Default)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	FString Json;
	if (Model && Model->TryGetContainerValueJson(ContainerId, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			return ClampDoubleToInt32(Value->AsNumber());
		}
	}
	return Default;
}

float UCrowdyGameModel::GetContainerFloat(const UObject* WorldContext, const FString& ContainerId, FName Key, float Default)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	FString Json;
	if (Model && Model->TryGetContainerValueJson(ContainerId, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			return static_cast<float>(Value->AsNumber());
		}
	}
	return Default;
}

bool UCrowdyGameModel::GetContainerBool(const UObject* WorldContext, const FString& ContainerId, FName Key, bool bDefault)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	FString Json;
	if (Model && Model->TryGetContainerValueJson(ContainerId, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Boolean)
		{
			return Value->AsBool();
		}
	}
	return bDefault;
}

FString UCrowdyGameModel::GetContainerString(const UObject* WorldContext, const FString& ContainerId, FName Key, const FString& Default)
{
	const UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext);
	FString Json;
	if (Model && Model->TryGetContainerValueJson(ContainerId, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			return Value->AsString();
		}
	}
	return Default;
}

int32 UCrowdyGameModel::GetItemInt(const FString& ItemStateJson, FName Key, int32 Default)
{
	const TSharedPtr<FJsonValue> Value = ReadItemStateField(ItemStateJson, Key);
	return (Value.IsValid() && Value->Type == EJson::Number) ? ClampDoubleToInt32(Value->AsNumber()) : Default;
}

float UCrowdyGameModel::GetItemFloat(const FString& ItemStateJson, FName Key, float Default)
{
	const TSharedPtr<FJsonValue> Value = ReadItemStateField(ItemStateJson, Key);
	return (Value.IsValid() && Value->Type == EJson::Number) ? static_cast<float>(Value->AsNumber()) : Default;
}

bool UCrowdyGameModel::GetItemBool(const FString& ItemStateJson, FName Key, bool bDefault)
{
	const TSharedPtr<FJsonValue> Value = ReadItemStateField(ItemStateJson, Key);
	return (Value.IsValid() && Value->Type == EJson::Boolean) ? Value->AsBool() : bDefault;
}

FString UCrowdyGameModel::GetItemString(const FString& ItemStateJson, FName Key, const FString& Default)
{
	const TSharedPtr<FJsonValue> Value = ReadItemStateField(ItemStateJson, Key);
	return (Value.IsValid() && Value->Type == EJson::String) ? Value->AsString() : Default;
}

int32 UCrowdyGameModel::GetItemFieldInt(const FCrowdyCollectionItem& Item, FName Key, int32 Default)
{
	return GetItemInt(Item.StateJson, Key, Default);
}

float UCrowdyGameModel::GetItemFieldFloat(const FCrowdyCollectionItem& Item, FName Key, float Default)
{
	return GetItemFloat(Item.StateJson, Key, Default);
}

bool UCrowdyGameModel::GetItemFieldBool(const FCrowdyCollectionItem& Item, FName Key, bool bDefault)
{
	return GetItemBool(Item.StateJson, Key, bDefault);
}

FString UCrowdyGameModel::GetItemFieldString(const FCrowdyCollectionItem& Item, FName Key, const FString& Default)
{
	return GetItemString(Item.StateJson, Key, Default);
}

void UCrowdyGameModel::WatchDataContainer(const UObject* WorldContext, const FString& ContainerId)
{
	if (UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext))
	{
		Model->WatchDataContainer(ContainerId);
	}
}

void UCrowdyGameModel::UnwatchDataContainer(const UObject* WorldContext, const FString& ContainerId)
{
	if (UCrowdyGameModelSubsystem* Model = ResolveModelSubsystem(WorldContext))
	{
		Model->UnwatchDataContainer(ContainerId);
	}
}
