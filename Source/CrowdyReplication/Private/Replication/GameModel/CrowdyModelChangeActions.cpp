// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelChangeActions.h"

#include "CrowdyGameModelLog.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

bool CrowdyModelListen::PassesFilter(bool bHasTargetFilter, const UObject* TargetFilter,
	const FString& ModelIdFilter, const UObject* InTarget, const FString& InModelId)
{
	if (bHasTargetFilter)
	{
		// A Target was requested: match only that exact object. A now-null TargetFilter (the watched object was
		// destroyed) matches nothing, so the listener never silently widens to every change.
		return TargetFilter != nullptr && TargetFilter == InTarget;
	}
	if (!ModelIdFilter.IsEmpty())
	{
		return InModelId == ModelIdFilter;
	}
	return true;
}

UCrowdyListenForModelChangesAction* UCrowdyListenForModelChangesAction::ListenForModelChanges(
	UObject* WorldContext, UObject* Target, const FString& ModelId)
{
	UCrowdyListenForModelChangesAction* Action = NewObject<UCrowdyListenForModelChangesAction>();
	Action->WorldContextObject = WorldContext;
	Action->TargetFilter = Target;
	Action->bHasTargetFilter = (Target != nullptr);
	Action->ModelIdFilter = ModelId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListenForModelChangesAction::Activate()
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject.Get(),
		EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	UCrowdyGameModelSubsystem* Model = World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	if (!Model)
	{
		// No subsystem means no change source: the node could never fire, so reap it instead of leaving it rooted
		// with a live-forever registration.
		UE_LOG(LogCrowdyGameModel, Warning,
			TEXT("[GameModel] Listen for Model Changes: no Game Model subsystem for this world context - the node will not fire."));
		SetReadyToDestroy();
		return;
	}
	BoundModel = Model;
	BoundWorld = World;
	Model->OnModelAttributeChanged.AddDynamic(this, &UCrowdyListenForModelChangesAction::HandleModelAttributeChanged);
	// RegisterWithGameInstance roots this node to the GameInstance, which outlives the world; reap on the bound
	// world's teardown so a listener does not linger inert (and rooted) across level travel.
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(
		this, &UCrowdyListenForModelChangesAction::HandleWorldBeginTearDown);
}

void UCrowdyListenForModelChangesAction::HandleModelAttributeChanged(UObject* InTarget, const FString& InModelId,
	FName Attribute, const FString& OldValueJson, const FString& NewValueJson)
{
	// A Target-filtered listener whose Target was destroyed can never match again: stop listening and reap.
	if (bHasTargetFilter && !TargetFilter.IsValid())
	{
		Shutdown();
		return;
	}
	if (CrowdyModelListen::PassesFilter(bHasTargetFilter, TargetFilter.Get(), ModelIdFilter, InTarget, InModelId))
	{
		OnChanged.Broadcast(InTarget, InModelId, Attribute, OldValueJson, NewValueJson);
	}
}

void UCrowdyListenForModelChangesAction::HandleWorldBeginTearDown(UWorld* World)
{
	if (World == BoundWorld.Get())
	{
		Shutdown();
	}
}

void UCrowdyListenForModelChangesAction::Shutdown()
{
	if (bShutDown)
	{
		return;
	}
	bShutDown = true;
	if (UCrowdyGameModelSubsystem* Model = BoundModel.Get())
	{
		Model->OnModelAttributeChanged.RemoveDynamic(this, &UCrowdyListenForModelChangesAction::HandleModelAttributeChanged);
	}
	if (WorldTearDownHandle.IsValid())
	{
		FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
		WorldTearDownHandle.Reset();
	}
	SetReadyToDestroy();
}

void UCrowdyListenForModelChangesAction::BeginDestroy()
{
	// Defensive unbind if we are GC'd without a Shutdown (e.g. the GameInstance teardown clears the async list).
	if (UCrowdyGameModelSubsystem* Model = BoundModel.Get())
	{
		Model->OnModelAttributeChanged.RemoveDynamic(this, &UCrowdyListenForModelChangesAction::HandleModelAttributeChanged);
	}
	if (WorldTearDownHandle.IsValid())
	{
		FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
		WorldTearDownHandle.Reset();
	}
	Super::BeginDestroy();
}
