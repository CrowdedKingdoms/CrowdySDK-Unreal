// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelCollectionActions.h"

#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

namespace
{
	// Resolves Ctx's Game Model subsystem. Null when Ctx has been garbage-collected, has no world, or the subsystem
	// is absent (not a play world). Deliberately named distinctly from the container-actions helper so the two do
	// not collide in a unity build (both are file-local).
	UCrowdyGameModelSubsystem* GetCollectionModelSubsystem(const TWeakObjectPtr<UObject>& Ctx)
	{
		if (!Ctx.IsValid())
		{
			return nullptr;
		}
		const UWorld* World = Ctx->GetWorld();
		return World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	}
}

UCrowdyAddToCollectionAction* UCrowdyAddToCollectionAction::AddToCollection(UObject* WorldContext,
	const FString& ParentContainerId, const FString& ParentContainerType, const FString& ItemContainerId,
	const FString& CollectionName)
{
	UCrowdyAddToCollectionAction* Action = NewObject<UCrowdyAddToCollectionAction>();
	Action->WorldContextObject = WorldContext;
	Action->ParentContainerId = ParentContainerId;
	Action->ParentContainerType = ParentContainerType;
	Action->ItemContainerId = ItemContainerId;
	Action->CollectionName = CollectionName;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyAddToCollectionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetCollectionModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyAddToCollectionAction> WeakThis(this);
	Model->AddToCollection(ParentContainerId, ParentContainerType, ItemContainerId, CollectionName,
		[WeakThis](bool bOk)
		{
			UCrowdyAddToCollectionAction* Action = WeakThis.Get();
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

UCrowdyRemoveFromCollectionAction* UCrowdyRemoveFromCollectionAction::RemoveFromCollection(UObject* WorldContext,
	const FString& ParentContainerId, const FString& ParentContainerType, const FString& ItemContainerId,
	const FString& CollectionName)
{
	UCrowdyRemoveFromCollectionAction* Action = NewObject<UCrowdyRemoveFromCollectionAction>();
	Action->WorldContextObject = WorldContext;
	Action->ParentContainerId = ParentContainerId;
	Action->ParentContainerType = ParentContainerType;
	Action->ItemContainerId = ItemContainerId;
	Action->CollectionName = CollectionName;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyRemoveFromCollectionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetCollectionModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyRemoveFromCollectionAction> WeakThis(this);
	Model->RemoveFromCollection(ParentContainerId, ParentContainerType, ItemContainerId, CollectionName,
		[WeakThis](bool bOk)
		{
			UCrowdyRemoveFromCollectionAction* Action = WeakThis.Get();
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

UCrowdyGetCollectionAction* UCrowdyGetCollectionAction::GetCollection(UObject* WorldContext,
	const FString& ParentContainerId, const FString& CollectionName)
{
	UCrowdyGetCollectionAction* Action = NewObject<UCrowdyGetCollectionAction>();
	Action->WorldContextObject = WorldContext;
	Action->ParentContainerId = ParentContainerId;
	Action->CollectionName = CollectionName;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetCollectionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetCollectionModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyContainerRef>());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetCollectionAction> WeakThis(this);
	// "Get Collection" is the depth-1 item list of the parent; ListChildren already drops the root.
	Model->ListChildren(ParentContainerId, CollectionName,
		[WeakThis](bool bOk, const TArray<FCrowdyContainerRef>& Items)
		{
			UCrowdyGetCollectionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Items);
			}
			else
			{
				Action->Failed.Broadcast(Items);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetCollectionWithStateAction* UCrowdyGetCollectionWithStateAction::GetCollectionWithState(UObject* WorldContext,
	const FString& ParentContainerId, const FString& CollectionName, int32 MaxItems)
{
	UCrowdyGetCollectionWithStateAction* Action = NewObject<UCrowdyGetCollectionWithStateAction>();
	Action->WorldContextObject = WorldContext;
	Action->ParentContainerId = ParentContainerId;
	Action->CollectionName = CollectionName;
	Action->MaxItems = MaxItems;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetCollectionWithStateAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetCollectionModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyCollectionItem>());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetCollectionWithStateAction> WeakThis(this);
	Model->GetCollectionWithState(ParentContainerId, CollectionName, MaxItems,
		[WeakThis](bool bOk, const TArray<FCrowdyCollectionItem>& Items)
		{
			UCrowdyGetCollectionWithStateAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Items);
			}
			else
			{
				Action->Failed.Broadcast(Items);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyCreateModelItemAction* UCrowdyCreateModelItemAction::CreateModelItem(UObject* WorldContext,
	const FString& TypeName, const FString& DisplayName, const FString& SessionId)
{
	UCrowdyCreateModelItemAction* Action = NewObject<UCrowdyCreateModelItemAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypeName = TypeName;
	Action->DisplayName = DisplayName;
	Action->SessionId = SessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyCreateModelItemAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetCollectionModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(FString());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyCreateModelItemAction> WeakThis(this);
	// A model item is a free/data container of an item type; the collection-workflow name for Create Data Container.
	Model->CreateDataContainer(TypeName, DisplayName, SessionId, FString(),
		[WeakThis](bool bOk, const FString& ContainerId)
		{
			UCrowdyCreateModelItemAction* Action = WeakThis.Get();
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
