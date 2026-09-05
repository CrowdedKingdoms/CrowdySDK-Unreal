// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModel.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyJsonValueSupport.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Resolves Entity's Game Model subsystem + entity NetID in one step, so every accessor shares the same
	// null-safe path. Entity is any registered participant (an actor or a subsystem); resolution routes through
	// the subsystem's single ResolveTargetNetID seam. Returns null when Entity has no world, is not a registered
	// entity, or the subsystem is absent (not a play world).
	UCrowdyGameModelSubsystem* ResolveModel(const UObject* Entity, FGuid& OutNetID)
	{
		OutNetID.Invalidate();
		const UWorld* World = Entity ? Entity->GetWorld() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		UCrowdyGameModelSubsystem* Model = World->GetSubsystem<UCrowdyGameModelSubsystem>();
		if (!Model)
		{
			return nullptr;
		}
		return Model->ResolveTargetNetID(Entity, OutNetID) ? Model : nullptr;
	}
}

void UCrowdyModel::PullNow(UObject* Entity)
{
	FGuid NetID;
	if (UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID))
	{
		Model->HandleModelChanged(NetID);
	}
}

int32 UCrowdyModel::GetInt(const UObject* Entity, FName Key, int32 Default)
{
	FGuid NetID;
	UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID);
	FString Json;
	if (Model && Model->TryGetCachedValueJson(NetID, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			return static_cast<int32>(Value->AsNumber());
		}
	}
	return Default;
}

float UCrowdyModel::GetFloat(const UObject* Entity, FName Key, float Default)
{
	FGuid NetID;
	UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID);
	FString Json;
	if (Model && Model->TryGetCachedValueJson(NetID, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			return static_cast<float>(Value->AsNumber());
		}
	}
	return Default;
}

bool UCrowdyModel::GetBool(const UObject* Entity, FName Key, bool bDefault)
{
	FGuid NetID;
	UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID);
	FString Json;
	if (Model && Model->TryGetCachedValueJson(NetID, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::Boolean)
		{
			return Value->AsBool();
		}
	}
	return bDefault;
}

FString UCrowdyModel::GetString(const UObject* Entity, FName Key, const FString& Default)
{
	FGuid NetID;
	UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID);
	FString Json;
	if (Model && Model->TryGetCachedValueJson(NetID, Key, Json))
	{
		const TSharedPtr<FJsonValue> Value = ParseCachedValue(Json);
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			return Value->AsString();
		}
	}
	return Default;
}

bool UCrowdyModel::IsContainerBound(const UObject* Entity)
{
	FGuid NetID;
	UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID);
	FString ContainerId;
	return Model && Model->TryGetContainerId(NetID, ContainerId);
}

UActorComponent* UCrowdyModel::GetModelComponent(AActor* Actor, TSubclassOf<UActorComponent> ContainerClass)
{
	if (!Actor || !*ContainerClass)
	{
		return nullptr;
	}
	return Actor->FindComponentByClass(ContainerClass);
}

void UCrowdyModel::EnrollModelComponent(UActorComponent* Component)
{
	const UWorld* World = Component ? Component->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	if (UCrowdyGameModelSubsystem* Model = World->GetSubsystem<UCrowdyGameModelSubsystem>())
	{
		Model->EnrollModelComponent(Component);
	}
}

void UCrowdyModel::UnenrollModelComponent(UActorComponent* Component)
{
	const UWorld* World = Component ? Component->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	if (UCrowdyGameModelSubsystem* Model = World->GetSubsystem<UCrowdyGameModelSubsystem>())
	{
		Model->UnenrollModelComponent(Component);
	}
}

void UCrowdyModel::Invoke(UObject* Entity, FName FunctionName)
{
	FGuid NetID;
	if (UCrowdyGameModelSubsystem* Model = ResolveModel(Entity, NetID))
	{
		Model->InvokeAndApply(NetID, FunctionName.ToString(), nullptr, FString(), nullptr);
	}
}
