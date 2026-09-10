// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/Subsystems/CrowdyActorManager.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UCrowdyClassRegistry.h"

namespace
{
	// CrowdyEventRouter retries an RPC or state delta that races its target entity's spawn
	// event for this many ticks before giving up on that single message. A pending activation
	// here is the same race (a position update arriving before the entity's class is known),
	// so a stranded entry is reused as the warning bound: past this many ticks nothing later
	// changed a message-by-message retry into a resolution, so it is worth telling a developer
	// while still leaving the entry in place for a genuinely late spawn event to complete.
	constexpr int32 CrowdyPendingActivationWarnTicks = 600;

	// Past this many ticks a parked entry gives its slot back instead of holding it for the life of the
	// world. Set against the two clocks that already bound a legitimate wait: the tracker
	// drops an entity that goes quiet for ActorTimeoutThreshold (12 seconds by default, enforced by a sweep
	// on a shorter period so the wait stays near it rather than up to twice it), which takes the
	// pending entry with it, so an entry still parked here is being fed by a sender that has not stopped;
	// and the retry window for this same spawn-event race is the 600 ticks above. This is a tick count while
	// the staleness bound is a wall clock, so the frame rate decides whether the premise above still holds:
	// eviction has to stay comfortably beyond the staleness bound, or an entry can be evicted for a sender
	// the tracker has not reaped yet and the log line says something untrue. At 240 frames per second this
	// is 30 seconds against a bound of about 14, and at 60 it is two minutes.
	constexpr int32 CrowdyPendingActivationEvictTicks = 7200;

	static_assert(CrowdyPendingActivationEvictTicks > CrowdyPendingActivationWarnTicks,
		"An entry has to be reported as stranded before it can be evicted, or the eviction is the first and only thing ever said about it.");

	// Both park messages end with this, so an entry that cannot even name its own payload says why
	// instead of printing an empty name and sending the reader after a struct that does not exist.
	FString CrowdyDescribeParkedActivationPayload(const FInstancedStruct& InitialState)
	{
		const UScriptStruct* PayloadStruct = InitialState.GetScriptStruct();
		if (!PayloadStruct)
		{
			return TEXT("its updates carried a state blob that decoded into no struct registered on this client, so the payload cannot be named here either. ")
				TEXT("Register the sender's state struct in this build, or check that both sides agree on which struct the payload type id names");
		}

		return FString::Printf(
			TEXT("its update payload struct is '%s'. Register a class for that struct with RegisterStateClass, since a remote entity built from position updates alone never sends a spawn event"),
			*PayloadStruct->GetName());
	}
}

void UCrowdyActorManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	check(IsValid(World))

	if (!IsValid(World))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid world"));
		return;
	}

	ActorTracker = World->GetSubsystem<UCrowdyActorTracker>();
	check(IsValid(ActorTracker))

	if (!IsValid(ActorTracker))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid actor tracker"));
		return;
	}

	EntitySubsystem = Collection.InitializeDependency<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Failed to get CrowdyEntitySubsystem"));
		return;
	}

	if (!LoadConfig())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Failed to load config due to incorrect params or disabled by choice."));
		return;
	}

	BindToTracker(ActorTracker);

	// Deferred activations fire when EntitySubsystem finishes processing a spawn event.
	EntitySubsystem->OnEntityRegistered.AddDynamic(this, &UCrowdyActorManager::OnEntityRegistered);
}

void UCrowdyActorManager::Deinitialize()
{
	if (IsValid(EntitySubsystem))
		EntitySubsystem->OnEntityRegistered.RemoveDynamic(this, &UCrowdyActorManager::OnEntityRegistered);

	UCrowdyActorTracker* CrowdyActorTracker = GetWorld()->GetSubsystem<UCrowdyActorTracker>();
	if (IsValid(CrowdyActorTracker))
	{
		CrowdyActorTracker->OnRemoteEntityAppeared.RemoveAll(this);
		CrowdyActorTracker->OnRemoteEntityTimedOut.RemoveAll(this);
		CrowdyActorTracker->OnRemoteEntityLeft.RemoveAll(this);
		CrowdyActorTracker->OnTrackedActorUpdates.RemoveAll(this);
	}

	FCrowdyActorUpdate Discarded;
	while (UpdateQueue.Dequeue(Discarded)) {}
	PendingUpdates.Empty();

	Slots.Empty();
	UUIDToSlot.Empty();
	PendingActivations.Empty();
	EntitySubsystem = nullptr;

	if (IsValid(ActiveBackend))
	{
		ActiveBackend->DeinitializeBackend();
		ActiveBackend = nullptr;
	}

	Super::Deinitialize();
}

bool UCrowdyActorManager::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
		return false;

	if (World->WorldType != EWorldType::PIE &&
		World->WorldType != EWorldType::Game)
		return false;

	return true;
}

void UCrowdyActorManager::Tick(float DeltaTime)
{
	ApplyPendingUpdates();
	TickInterpolation();
	TickPendingActivations();
}

void UCrowdyActorManager::RegisterStateClass(UScriptStruct* Struct, TSubclassOf<AActor> ActorClass)
{
	if (Struct && ActorClass)
		StateClassMap.Add(Struct, ActorClass);
}

void UCrowdyActorManager::SetBackend(UCrowdyRenderingBackend* NewBackend)
{
	if (!IsValid(NewBackend))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid backend provided"));
		return;
	}

	if (NewBackend == ActiveBackend)
		return;

	if (IsValid(ActiveBackend))
		ActiveBackend->DeinitializeBackend();

	ActiveBackend = NewBackend;
}

void UCrowdyActorManager::UpdateServerTimeOffset(const int64 ServerTimestampMs, const int64 ClientNowMs)
{
	const int64 EstimatedOffset = ServerTimestampMs - ClientNowMs;

	if (!bHasInitialOffset)
	{
		ServerTimeOffsetMs = EstimatedOffset;
		BestOffsetMs = EstimatedOffset;
		bHasInitialOffset = true;
		return;
	}

	if (EstimatedOffset > BestOffsetMs)
		BestOffsetMs = EstimatedOffset;

	if (FMath::Abs(EstimatedOffset - ServerTimeOffsetMs) > 200)
		return;

	ServerTimeOffsetMs = static_cast<int64>(FMath::Lerp(
		static_cast<double>(ServerTimeOffsetMs),
		static_cast<double>(BestOffsetMs),
		0.05
	));
}

int64 UCrowdyActorManager::GetEstimatedServerTimeMs() const
{
	const int64 ClientNowMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();
	return ClientNowMs + ServerTimeOffsetMs;
}

void UCrowdyActorManager::ApplyPendingUpdates()
{
	if (!IsValid(ActiveBackend))
	{
		// Still cleared, or a backend arriving later would be handed a frame of stale positions at once.
		PendingUpdates.Reset();
		return;
	}

	if (UpdateQueue.IsEmpty() && PendingUpdates.IsEmpty()) return;

	// Sampled once for the drain rather than once per update. A drain carries a couple of thousand of them
	// and the wall clock cannot meaningfully move across one, so the per-update read bought nothing.
	const int64 ClientNowMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();

	auto Apply = [this, ClientNowMs](const FCrowdyActorUpdate& Update)
	{
		UpdateServerTimeOffset(Update.ServerTimestamp, ClientNowMs);

		const int32* SlotPtr = UUIDToSlot.Find(Update.UUID);
		if (!SlotPtr) return;

		ActiveBackend->ExtractUpdate(Update.ResolveState(), Update.ServerTimestamp, *SlotPtr);
	};

	// The queue first: anything in it was handed over from another thread and so was gathered before
	// whatever the game thread appended after it.
	FCrowdyActorUpdate Queued;
	while (UpdateQueue.Dequeue(Queued))
		Apply(Queued);

	for (const FCrowdyActorUpdate& Update : PendingUpdates)
		Apply(Update);

	PendingUpdates.Reset();
}

void UCrowdyActorManager::TickInterpolation()
{
	if (!IsValid(ActiveBackend)) return;

	const int64 RenderTime = GetEstimatedServerTimeMs() - InterpolationDelayMs;
	const int32 Count = Slots.Num();

	for (int32 i = 0; i < Count; i++)
	{
		if (!Slots[i].bActive) continue;
		ActiveBackend->ApplyInterpolation(i, RenderTime);
	}
}

void UCrowdyActorManager::BindToTracker(UCrowdyActorTracker* Tracker)
{
	if (!IsValid(Tracker))
	{
		return;
	}

	// Both departures are bound, and that is the whole point of them being listed together: the server announcing
	// an actor gone removes it from the map the timeout check reads, so an actor reported by one of these is never
	// reported by the other and binding only one leaves those actors holding their slots forever.
	Tracker->OnRemoteEntityAppeared.AddDynamic(this, &UCrowdyActorManager::HandleActorSpawned);
	Tracker->OnRemoteEntityTimedOut.AddDynamic(this, &UCrowdyActorManager::HandleActorDestroyed);
	Tracker->OnRemoteEntityLeft.AddDynamic(this, &UCrowdyActorManager::HandleActorLeft);
	Tracker->OnTrackedActorUpdates.AddUObject(this, &UCrowdyActorManager::HandleUpdateBatch);
}

int32 UCrowdyActorManager::AllocateSlot(const FGuid& UUID)
{
	for (int32 i = 0; i < Slots.Num(); i++)
	{
		if (!Slots[i].bActive)
		{
			Slots[i].bActive = true;
			Slots[i].UUID    = UUID;
			UUIDToSlot.Add(UUID, i);
			return i;
		}
	}

	FSlotEntry NewSlot;
	NewSlot.bActive = true;
	NewSlot.UUID    = UUID;

	const int32 SlotId = Slots.Add(NewSlot);
	UUIDToSlot.Add(UUID, SlotId);
	return SlotId;
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyActorManager::ParkActivationForTest(const FGuid& UUID)
{
	FPendingActivation& Pending = PendingActivations.Add(UUID);
	Pending.SlotId = AllocateSlot(UUID);
}

void UCrowdyActorManager::ExtractUpdateForTest(const FGuid& UUID, const FInstancedStruct& State)
{
	const int32* SlotPtr = UUIDToSlot.Find(UUID);
	if (!SlotPtr || !IsValid(ActiveBackend)) return;

	ActiveBackend->ExtractUpdate(State, GetEstimatedServerTimeMs(), *SlotPtr);
}

int32 UCrowdyActorManager::GetPendingActivationEvictTicksForTest()
{
	return CrowdyPendingActivationEvictTicks;
}
#endif

void UCrowdyActorManager::ReleaseSlot(const FGuid& UUID)
{
	const int32* SlotPtr = UUIDToSlot.Find(UUID);
	if (!SlotPtr) return;

	// The backend is told to clean the slot even when the instance was never activated. A slot allocated for
	// an entity still waiting on its spawn event is already reachable from UUIDToSlot, so ApplyPendingUpdates
	// has been feeding this slot's interpolation buffers through ExtractUpdate for as long as it waited.
	// Freeing it without that cleanup hands those samples to whichever entity is given the slot next, which
	// then draws at, and streaks in from, this entity's last known position.
	if (IsValid(ActiveBackend))
		ActiveBackend->DeactivateInstance(*SlotPtr, UUID);

	FreeSlot(UUID);
}

void UCrowdyActorManager::FreeSlot(const FGuid& UUID)
{
	const int32* SlotPtr = UUIDToSlot.Find(UUID);
	if (!SlotPtr) return;

	const int32 SlotId = *SlotPtr;

	if (Slots.IsValidIndex(SlotId))
	{
		Slots[SlotId].bActive = false;
		Slots[SlotId].UUID    = FGuid{};
	}

	UUIDToSlot.Remove(UUID);
}

bool UCrowdyActorManager::LoadConfig()
{
	// ResolveProfileForWorld reports a map that resolves nothing, once for the whole world. Repeating it
	// here would say the same thing again for every subsystem that asks.
	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
	if (!Profile)
		return false;

	const FCrowdyActorManagementConfigStruct& Config = Profile->ActorManagement;

	if (!Config.bUseCrowdyActorTracker)
		return false;

	if (!IsValid(Config.BackendClass.Get()))
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorManager]: BackendClass is not set in the map profile. Assign a UCrowdyRenderingBackend subclass."));
		return false;
	}

	UCrowdyRenderingBackend* Backend = NewObject<UCrowdyRenderingBackend>(this, Config.BackendClass.Get());

	// A backend that cannot draw is refused rather than installed. Installing one anyway leaves every remote
	// entity tracked, slotted and updated with nothing on screen, which is the same outcome as having no
	// backend at all but without the map ever saying so.
	if (!Backend->InitializeBackend(GetWorld(), Config.BackendConfig))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyActorManager]: Backend '%s' could not initialize, so no remote entity will be drawn on this map. ")
			TEXT("The backend logged what it needs just above this line."),
			*GetNameSafe(Config.BackendClass.Get()));
		return false;
	}

	SetBackend(Backend);

	return true;
}

UClass* UCrowdyActorManager::ResolveEntityClass(const FGuid& UUID, const FInstancedStruct& State) const
{
	// Primary path: the class the sender named on its own update. Shape and identity are separate facts
	// on the wire, so this asks the message what the entity IS rather than inferring it from the shape
	// of its bytes. That inference is what made two classes sharing one state struct resolve as
	// whichever of them registered last, and it could never have been fixed by a better guess.
	if (IsValid(ActorTracker))
	{
		const FCrowdyClassID ClassID = ActorTracker->GetClassIDForUUID(UUID);
		if (ClassID != CROWDY_INVALID_CLASS_ID)
		{
			const FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(ClassID);

			if (UClass* Resolved = ClassPath.IsValid() ? ClassPath.ResolveClass() : nullptr)
			{
				return Resolved;
			}

			// Said out loud rather than quietly falling through to the struct map. The registry is built
			// from loaded classes, and ResolveClass does not load one, so an entity class this observer
			// has never loaded lands here and would otherwise sit invisible in PendingActivations with
			// nothing naming the cause. Preloading the entity class set is what closes this.
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[Crowdy SDK][ActorManager]: entity %s named class id %u, which this client cannot resolve to a loaded class. It will fall back to the state struct, which cannot tell two classes sharing one struct apart. Preload the entity class or give it a ClassIDOverride."),
				*UUID.ToString(), ClassID);
		}
	}

	// Compatibility fallback: derive the class from the state struct type. Correct only while a struct
	// names exactly one class, which is the limitation the class id above removes.
	if (const UScriptStruct* StateStruct = State.GetScriptStruct())
	{
		if (const TSubclassOf<AActor>* Found = StateClassMap.Find(StateStruct))
			return Found->Get();
	}

	// Fallback: entity was registered via a spawn event (static entities, or dynamic
	// entities where the spawn event arrived before position updates).
	if (IsValid(EntitySubsystem))
	{
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(UUID);
		if (!Record) return nullptr;

		const FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(Record->ClassID);
		if (!ClassPath.IsValid()) return nullptr;

		return ClassPath.ResolveClass();
	}

	return nullptr;
}

void UCrowdyActorManager::HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount)
{
	if (!IsValid(ActiveBackend)) return;

	// Slot already exists: duplicate tracker broadcast or cascade from a pool actor's own
	// auto-replication. Ignore it: the entity is already active.
	if (UUIDToSlot.Contains(UUID)) return;

	UClass* EntityClass = ResolveEntityClass(UUID, InitialState);

	if (!EntityClass)
	{
		// Class not resolvable yet: defer until a spawn event registers the entity.
		FPendingActivation& Pending = PendingActivations.Add(UUID);
		Pending.SlotId       = AllocateSlot(UUID);
		Pending.InitialState = MoveTemp(InitialState);
		return;
	}

	const int32 SlotId = AllocateSlot(UUID);
	ActiveBackend->ActivateInstance(SlotId, UUID, EntityClass, InitialState);
	ActiveBackend->ExtractUpdate(InitialState, GetEstimatedServerTimeMs(), SlotId);

}

void UCrowdyActorManager::HandleActorDestroyed(FGuid UUID, int32 ActorCount)
{
	// Drop any pending activation that never fired.
	PendingActivations.Remove(UUID);

	ReleaseSlot(UUID);
}

void UCrowdyActorManager::HandleActorLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount)
{
	// A departure the server announced and one this client guessed at retire the actor the same way. The
	// tracker reports whichever arrives first and suppresses the other, so this cannot double release.
	HandleActorDestroyed(ActorLeft.UUID, ActorCount);
}

void UCrowdyActorManager::HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates)
{
	// The tracker hands tracked updates over on the game thread, which is the thread that drains them, so
	// they are appended straight to the pending array and cost no queue node. The queue stays for anything
	// broadcasting from elsewhere, which is what the delegate still allows.
	if (IsInGameThread())
	{
		PendingUpdates.Append(Updates);
		return;
	}

	for (const FCrowdyActorUpdate& Update : Updates)
		UpdateQueue.Enqueue(Update);
}

void UCrowdyActorManager::AgePendingActivations(TMap<FGuid, FPendingActivation>& Park, const int32 WarnTicks, const int32 EvictTicks, TArray<FGuid>& OutEvicted)
{
	if (Park.IsEmpty()) return;

	// OutEvicted is appended to rather than cleared, so a caller can reuse one array across ticks.
	// Only what this call added is removed below: a UUID left over from an earlier call may have been
	// parked again since, and removing it a second time would throw away a live entry.
	const int32 FirstEvictedThisTick = OutEvicted.Num();

	for (TPair<FGuid, FPendingActivation>& Pair : Park)
	{
		FPendingActivation& Pending = Pair.Value;

		// Counted for every parked entry, including one already reported as stranded, because the
		// eviction bound is measured from when the entry was parked and not from the report.
		Pending.TicksWaiting++;

		if (Pending.TicksWaiting >= EvictTicks)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[Crowdy Actor Manager]: Entity %s waited %d ticks for a spawn event that never resolved its class, ")
				TEXT("so the render slot it was holding has been released and its updates are ignored from here on; %s. ")
				TEXT("It is picked up again only if it stops sending for long enough to time out and reappear."),
				*Pair.Key.ToString(),
				Pending.TicksWaiting,
				*CrowdyDescribeParkedActivationPayload(Pending.InitialState));

			OutEvicted.Add(Pair.Key);
			continue;
		}

		if (Pending.bWarnedStranded) continue;
		if (Pending.TicksWaiting < WarnTicks) continue;

		Pending.bWarnedStranded = true;

		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[Crowdy Actor Manager]: Entity %s has been waiting for a spawn event for %d ticks, still has no class ")
			TEXT("resolved, and is holding a render slot while it waits; %s."),
			*Pair.Key.ToString(),
			Pending.TicksWaiting,
			*CrowdyDescribeParkedActivationPayload(Pending.InitialState));
	}

	// Removed after the walk rather than during it, since erasing from the map being iterated is what
	// the eviction is for and not something to risk doing mid-walk.
	for (int32 Index = FirstEvictedThisTick; Index < OutEvicted.Num(); Index++)
		Park.Remove(OutEvicted[Index]);
}

void UCrowdyActorManager::TickPendingActivations()
{
	if (PendingActivations.IsEmpty()) return;

	TArray<FGuid> Evicted;
	AgePendingActivations(PendingActivations, CrowdyPendingActivationWarnTicks, CrowdyPendingActivationEvictTicks, Evicted);

	for (const FGuid& EntityID : Evicted)
		ReleaseSlot(EntityID);
}

void UCrowdyActorManager::OnEntityRegistered(const FGuid& EntityID)
{
	FPendingActivation Pending;
	if (!PendingActivations.RemoveAndCopyValue(EntityID, Pending))
		return;

	UClass* EntityClass = ResolveEntityClass(EntityID, Pending.InitialState);
	if (!EntityClass)
	{
		// Class still not resolvable after the spawn event, which happens when the class path the
		// spawn event carried was invalid. Give the slot back rather than parking the entry again.
		ReleaseSlot(EntityID);
		return;
	}

	if (!IsValid(ActiveBackend))
	{
		ReleaseSlot(EntityID);
		return;
	}

	ActiveBackend->ActivateInstance(Pending.SlotId, EntityID, EntityClass, Pending.InitialState);
	ActiveBackend->ExtractUpdate(Pending.InitialState, GetEstimatedServerTimeMs(), Pending.SlotId);
}
