#include "Replication/Subsystems/CrowdyKitActorSubsystem.h"

#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyKitActorComponent.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"

void UCrowdyKitActorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const UWorld* World = GetWorld();
	if (!World || !World->GetGameInstance())
	{
		return;
	}
	UCrowdyActorTracker* Found = Collection.InitializeDependency<UCrowdyActorTracker>();
	UCrowdyEntitySubsystem* FoundEntities = Collection.InitializeDependency<UCrowdyEntitySubsystem>();
	if (!Found || !FoundEntities)
	{
		return;
	}
	Tracker = Found;
	Entities = FoundEntities;
	ResolveSession();
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
	Slots.Reset();
	ByKind.Reset();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyKitActorSubsystem::UseSessionForTest(const UCrowdyGameSession* InSession)
{
	Session = InSession;
}
#endif

void UCrowdyKitActorSubsystem::ResolveSession()
{
	if (Session.IsValid())
	{
		return;
	}
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
}

double UCrowdyKitActorSubsystem::GetChunkSize() const
{
	const UCrowdyGameSession* Live = Session.Get();
	return Live ? Live->GetChunkSize() : FCrowdyKitActorState::ChunkWorldSize;
}

const TArray<FCrowdyKitActorState>& UCrowdyKitActorSubsystem::ViewKitActors(const FName Kind) const
{
	static const TArray<FCrowdyKitActorState> None;
	const FKindActors* Group = ByKind.Find(Kind);
	return Group ? Group->States : None;
}

int32 UCrowdyKitActorSubsystem::GetKitActorCount(const FName Kind) const
{
	return Kind.IsNone() ? Slots.Num() : ViewKitActors(Kind).Num();
}

bool UCrowdyKitActorSubsystem::GetKitActor(const FName Kind, int32 Index, FCrowdyKitActorState& KitActor) const
{
	KitActor = FCrowdyKitActorState();
	if (Index < 0)
	{
		return false;
	}
	if (!Kind.IsNone())
	{
		const TArray<FCrowdyKitActorState>& States = ViewKitActors(Kind);
		if (!States.IsValidIndex(Index))
		{
			return false;
		}
		KitActor = States[Index];
		return true;
	}
	for (const TPair<FName, FKindActors>& Pair : ByKind)
	{
		const TArray<FCrowdyKitActorState>& States = Pair.Value.States;
		if (Index < States.Num())
		{
			KitActor = States[Index];
			return true;
		}
		Index -= States.Num();
	}
	return false;
}

TArray<FCrowdyKitActorState> UCrowdyKitActorSubsystem::GetKitActors(const FName Kind) const
{
	if (!Kind.IsNone())
	{
		return ViewKitActors(Kind);
	}
	TArray<FCrowdyKitActorState> Out;
	Out.Reserve(Slots.Num());
	for (const TPair<FName, FKindActors>& Pair : ByKind)
	{
		Out.Append(Pair.Value.States);
	}
	return Out;
}

const FCrowdyKitActorState* UCrowdyKitActorSubsystem::FindKitActor(const FGuid& Key) const
{
	const FKitSlot* Slot = Slots.Find(Key);
	return Slot ? &ByKind.FindChecked(Slot->Kind).States[Slot->Index] : nullptr;
}

void UCrowdyKitActorSubsystem::HandleAppeared(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount)
{
	const FCrowdyKitActorState* State = InitialState.GetPtr<FCrowdyKitActorState>();
	if (!State)
	{
		return;
	}
	ResolveSession();
	// Drawn already when the actor manager heard this first; otherwise its registration delivers it.
	Deliver(UUID, Store(UUID, *State, GetChunkSize()), true);
}

void UCrowdyKitActorSubsystem::HandleEntityRegistered(const FGuid& NetID)
{
	if (const FCrowdyKitActorState* State = FindKitActor(NetID))
	{
		Deliver(NetID, *State, true);
	}
}

void UCrowdyKitActorSubsystem::TakeUpdates(const TArray<FCrowdyActorUpdate>& Updates)
{
	ResolveSession();
	const double ChunkSize = GetChunkSize();
	for (const FCrowdyActorUpdate& Update : Updates)
	{
		if (!UCrowdyActorTracker::IsKitActorUpdate(Update))
		{
			continue;
		}
		if (const FCrowdyKitActorState* State = Update.ResolveState().GetPtr<FCrowdyKitActorState>())
		{
			Take(Update.UUID, *State, ChunkSize);
		}
	}
}

const FCrowdyKitActorState& UCrowdyKitActorSubsystem::Store(const FGuid& Key, const FCrowdyKitActorState& State, const double ChunkSize)
{
	const FKitSlot* Known = Slots.Find(Key);
	if (Known && Known->Kind != State.Kind)
	{
		Forget(Key);
		Known = nullptr;
	}
	if (Known)
	{
		FCrowdyKitActorState& Held = ByKind.FindChecked(Known->Kind).States[Known->Index];
		Held = State;
		Held.Location = Held.ChunkPosition * ChunkSize;
		return Held;
	}
	FKindActors& Group = ByKind.FindOrAdd(State.Kind);
	Slots.Add(Key, FKitSlot{ State.Kind, Group.States.Num() });
	Group.Keys.Add(Key);
	FCrowdyKitActorState& Held = Group.States.Add_GetRef(State);
	Held.Location = Held.ChunkPosition * ChunkSize;
	return Held;
}

void UCrowdyKitActorSubsystem::Take(const FGuid& Key, const FCrowdyKitActorState& State, const double ChunkSize)
{
	const FCrowdyKitActorState* Known = FindKitActor(Key);
	const bool bFactsChanged = !Known || !State.HasSameFacts(*Known);
	const FCrowdyKitActorState& Held = Store(Key, State, ChunkSize);
	// Only a change in what it is or holds reaches the actor: its movement is the rendering backend's to draw.
	if (bFactsChanged)
	{
		Deliver(Key, Held, false);
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
	FKitSlot Slot;
	if (!Slots.RemoveAndCopyValue(Key, Slot))
	{
		return;
	}
	FKindActors& Group = ByKind.FindChecked(Slot.Kind);
	Group.States.RemoveAtSwap(Slot.Index);
	Group.Keys.RemoveAtSwap(Slot.Index);
	if (Group.Keys.IsValidIndex(Slot.Index))
	{
		Slots.FindChecked(Group.Keys[Slot.Index]).Index = Slot.Index;
		return;
	}
	// Kind arrives from any peer, so an emptied group goes rather than piling up one per name ever seen.
	if (Group.Keys.IsEmpty())
	{
		ByKind.Remove(Slot.Kind);
	}
}

void UCrowdyKitActorSubsystem::HandleTimedOut(FGuid UUID, int32 ActorCount)
{
	Forget(UUID);
}

void UCrowdyKitActorSubsystem::HandleLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount)
{
	Forget(ActorLeft.UUID);
}
