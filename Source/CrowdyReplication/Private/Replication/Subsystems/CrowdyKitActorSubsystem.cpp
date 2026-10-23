#include "Replication/Subsystems/CrowdyKitActorSubsystem.h"

#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyKitActorComponent.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

void UCrowdyKitActorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UCrowdyActorTracker* Found = Collection.InitializeDependency<UCrowdyActorTracker>();
	UCrowdyEntitySubsystem* FoundEntities = Collection.InitializeDependency<UCrowdyEntitySubsystem>();
	if (!Found || !FoundEntities)
	{
		return;
	}
	Tracker = Found;
	Entities = FoundEntities;
	Found->OnRemoteEntityAppeared.AddDynamic(this, &UCrowdyKitActorSubsystem::HandleAppeared);
	Found->OnRemoteEntityTimedOut.AddDynamic(this, &UCrowdyKitActorSubsystem::HandleTimedOut);
	Found->OnRemoteEntityLeft.AddDynamic(this, &UCrowdyKitActorSubsystem::HandleLeft);
	UpdatesHandle = Found->OnTrackedActorUpdates.AddUObject(this, &UCrowdyKitActorSubsystem::TakeUpdates);
	FoundEntities->OnEntityRegistered.AddDynamic(this, &UCrowdyKitActorSubsystem::HandleEntityRegistered);
}

void UCrowdyKitActorSubsystem::Deinitialize()
{
	if (UCrowdyActorTracker* Live = Tracker.Get())
	{
		Live->OnRemoteEntityAppeared.RemoveAll(this);
		Live->OnRemoteEntityTimedOut.RemoveAll(this);
		Live->OnRemoteEntityLeft.RemoveAll(this);
		Live->OnTrackedActorUpdates.Remove(UpdatesHandle);
	}
	if (UCrowdyEntitySubsystem* Live = Entities.Get())
	{
		Live->OnEntityRegistered.RemoveAll(this);
	}
	Seen.Reset();
	Super::Deinitialize();
}

TArray<FCrowdyKitActorState> UCrowdyKitActorSubsystem::GetKitActors(const FName Kind) const
{
	TArray<FCrowdyKitActorState> Out;
	for (const TPair<FGuid, FCrowdyKitActorState>& Pair : Seen)
	{
		if (Kind.IsNone() || Pair.Value.Kind == Kind)
		{
			Out.Add(Pair.Value);
		}
	}
	return Out;
}

void UCrowdyKitActorSubsystem::HandleAppeared(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount)
{
	const FCrowdyKitActorState* State = InitialState.GetPtr<FCrowdyKitActorState>();
	if (!State)
	{
		return;
	}
	Seen.Add(UUID, *State);
	// Drawn already when the actor manager heard this first; otherwise its registration delivers it.
	Deliver(UUID, *State, true);
}

void UCrowdyKitActorSubsystem::HandleEntityRegistered(const FGuid& NetID)
{
	if (const FCrowdyKitActorState* State = Seen.Find(NetID))
	{
		Deliver(NetID, *State, true);
	}
}

void UCrowdyKitActorSubsystem::TakeUpdates(const TArray<FCrowdyActorUpdate>& Updates)
{
	for (const FCrowdyActorUpdate& Update : Updates)
	{
		if (!UCrowdyActorTracker::IsKitActorUpdate(Update))
		{
			continue;
		}
		if (const FCrowdyKitActorState* State = Update.ResolveState().GetPtr<FCrowdyKitActorState>())
		{
			Take(Update.UUID, *State);
		}
	}
}

void UCrowdyKitActorSubsystem::Take(const FGuid& Key, const FCrowdyKitActorState& State)
{
	FCrowdyKitActorState* Known = Seen.Find(Key);
	const bool bFactsChanged = !Known || !State.HasSameFacts(*Known);
	if (Known)
	{
		*Known = State;
	}
	else
	{
		Seen.Add(Key, State);
	}
	// Only a change in what it is or holds reaches the actor: its movement is the rendering backend's to draw.
	if (bFactsChanged)
	{
		Deliver(Key, State, false);
	}
}

void UCrowdyKitActorSubsystem::Deliver(const FGuid& Key, const FCrowdyKitActorState& State, const bool bStartsDrawing) const
{
	const UCrowdyEntitySubsystem* Live = Entities.Get();
	AActor* Actor = Live ? Live->FindEntity(Key) : nullptr;
	if (!IsValid(Actor))
	{
		return;
	}
	UCrowdyKitActorComponent* Component = Actor->FindComponentByClass<UCrowdyKitActorComponent>();
	if (!Component)
	{
		Component = NewObject<UCrowdyKitActorComponent>(Actor, NAME_None, RF_Transient);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	Component->ApplyKitActor(Key, State, bStartsDrawing);
}

void UCrowdyKitActorSubsystem::Forget(const FGuid& Key)
{
	Seen.Remove(Key);
}

void UCrowdyKitActorSubsystem::HandleTimedOut(FGuid UUID, int32 ActorCount)
{
	Forget(UUID);
}

void UCrowdyKitActorSubsystem::HandleLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount)
{
	Forget(ActorLeft.UUID);
}
