// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyActorTracker.h"

#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"

void UCrowdyActorTracker::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();
	
	if (!IsValid(World))
		return;
	
	
	WorkerThreadsSubsystem = World->GetGameInstance()->GetSubsystem<UCrowdyWorkerThreadsSubsystem>();
	UCrowdyGameSession* GameSession = World->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();
	const UCrowdySDKSubsystem* CrowdySDK = World->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
	
	check(IsValid(WorkerThreadsSubsystem.Get()));
	check(IsValid(GameSession));
	check(IsValid(CrowdySDK));
	
	if (!IsValid(WorkerThreadsSubsystem.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Tracker]: Invalid Worker Threads subsystem."));
		return;
	}
	
	GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyActorTracker::SetLocalUUID);
	
	if (!LoadDeveloperSettings())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Crowdy Actor Tracker]: Disabled or error in config of Developer Settings."));
		return;
	}
		
	SetupQueues();
	
	CrowdySDK->RegisterReceptionLayer(this);
	
	World->GetTimerManager().SetTimer(TimeoutTimerHandle, 
		this, 
		&UCrowdyActorTracker::CheckTimeouts, 
		ActorTimeoutThreshold, 
		true);
	
	UE_LOG(LogTemp, Log, TEXT("[Crowdy Actor Tracker]: Initialized."));
}

void UCrowdyActorTracker::Deinitialize()
{
	if (const UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(TimeoutTimerHandle);

	if (!UpdateQueues.IsEmpty())
	{
		// Drain all queues before shutdown so worker lambdas don't fire after this
		for (int32 i = 0; i < NumOfWorkerThread; i++)
		{
			FCrowdyActorUpdate Discarded;
			while (UpdateQueues[i]->Dequeue(Discarded)) {}
		}
	}
	
	Super::Deinitialize();
}

bool UCrowdyActorTracker::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	// PIE or Game only
	if (World->WorldType != EWorldType::PIE &&
		World->WorldType != EWorldType::Game)
	{
		return false;
	}
	
	return true;
}

void UCrowdyActorTracker::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	switch (Message->GetType())
	{
		case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
		{
			const auto& AUN = static_cast<const FActorUpdateNotificationMessage&>(*Message);
				
				FCrowdyActorUpdate Update;
				Update.UUID = AUN.GUID;
				Update.State = AUN.State;
				Update.ServerTimestamp = AUN.Timestamp;
				EnqueueUpdate(MoveTemp(Update));
		}
		break;
		
		default:
		break;
	}
}

TArray<ECrowdyMessageType> UCrowdyActorTracker::GetSupportedResponseTypes() const
{
	return 
	{
		ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, 
		ECrowdyMessageType::ACTOR_UPDATE_RESPONSE
	};
}

TArray<FName> UCrowdyActorTracker::GetSupportedActorUpdateTypes() const
{
	return SupportedActorUpdateTypes;
}

void UCrowdyActorTracker::Configure(const int32 InMaxTrackedActors, const int32 InMaxUpdatesPerBatch, const float InMaxBatchWaitTime,
                                    const float InActorTimeoutThreshold)
{
	MaxTrackedActors      = InMaxTrackedActors;
	MaxUpdatesPerBatch    = InMaxUpdatesPerBatch;
	MaxBatchWaitTime      = InMaxBatchWaitTime;
	ActorTimeoutThreshold = InActorTimeoutThreshold;

	// Rebuild sets with new capacity
	TrackedUUIDs  = MakeUnique<FLockFreeGuidSet>(MaxTrackedActors);
	PendingSpawns = MakeUnique<FLockFreeGuidSet>(MaxTrackedActors);
}

void UCrowdyActorTracker::ToggleOwnerTracking(const bool bEnable)
{
	bOwnerTrackingEnabled = bEnable;
}

void UCrowdyActorTracker::ToggleBroadcastUpdatesToGameThread(const bool bEnable)
{
	bBroadcastUpdatesToGameThread = bEnable;
}

void UCrowdyActorTracker::SetupQueues()
{
	NumOfWorkerThread = FMath::Max(NumOfWorkerThread, FPlatformMisc::NumberOfCores() - 2);
	
	UpdateQueues.SetNum(NumOfWorkerThread);
	WorkerScheduledFlags.SetNum(NumOfWorkerThread);
	
	for (int32 i = 0; i < NumOfWorkerThread; i++)
	{
		UpdateQueues[i] = MakeUnique<TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc>>();
		WorkerScheduledFlags[i].store(false, std::memory_order_relaxed);
	}
	
	TrackedUUIDs = MakeUnique<FLockFreeGuidSet>(MaxTrackedActors);
	PendingSpawns = MakeUnique<FLockFreeGuidSet>(MaxTrackedActors);
	
	LastUpdateTimes.Reserve(MaxTrackedActors);
}

void UCrowdyActorTracker::EnqueueUpdate(const FCrowdyActorUpdate& Update)
{
	FCrowdyActorUpdate Copy = Update;
	EnqueueUpdate(MoveTemp(Copy));
}

void UCrowdyActorTracker::EnqueueUpdate(FCrowdyActorUpdate&& Update)
{
	if (Update.UUID == LocalUUID && !bOwnerTrackingEnabled)
		return;

	const int32 WorkerIndex = GetTypeHash(Update.UUID) % NumOfWorkerThread;
	UpdateQueues[WorkerIndex]->Enqueue(MoveTemp(Update));

	bool bExpected = false;
	if (WorkerScheduledFlags[WorkerIndex].compare_exchange_strong(
			bExpected, true,
			std::memory_order_acq_rel,
			std::memory_order_relaxed))
	{
		TArray<TFunction<void()>> Tasks;
		Tasks.Add([this, WorkerIndex]()
		{
			ProcessQueue(WorkerIndex);
			WorkerScheduledFlags[WorkerIndex].store(false, std::memory_order_release);
		});
		WorkerThreadsSubsystem->EnqueueTasks(Tasks);
	}
}

bool UCrowdyActorTracker::LoadDeveloperSettings()
{
	
	const UCrowdySDKDeveloperSettings* DeveloperSettings = GetDefault<UCrowdySDKDeveloperSettings>();
    if (!DeveloperSettings)
        return false;

    const UWorld* World = GetWorld();
    if (!IsValid(World))
        return false;

	UActorUpdatePayloadRegistry::Get()->GetAllRegisteredNames(SupportedActorUpdateTypes);
	
	for (const auto& SupportedActorUpdateType : SupportedActorUpdateTypes)
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdyActorTracker]: Supported actor update type: %s"), *SupportedActorUpdateType.ToString());
	}

	FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());

#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
		CurrentMap = World->RemovePIEPrefix(CurrentMap);
#endif

	for (const auto& [WorldPtr, ConfigAssetPtr] : DeveloperSettings->ActorManagementConfigs)
	{
		if (WorldPtr.IsNull()) continue;

		const FString AllowedMap = FPackageName::GetShortName(WorldPtr.GetAssetName());
		if (AllowedMap != CurrentMap) continue;

		const UCrowdyActorManagementConfig* ConfigAsset = ConfigAssetPtr.LoadSynchronous();
		if (!IsValid(ConfigAsset))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[CrowdyActorTracker]: Config asset for map '%s' is invalid."), *CurrentMap);
			return false;
		}

		const FCrowdyActorManagementConfigStruct& Config = ConfigAsset->Config;
		if (!Config.bUseCrowdyActorTracker) return false;

		Configure(
			Config.MaxTrackedActors,
			Config.MaxUpdatesPerBatch,
			Config.MaxBatchWaitTime,
			Config.ActorTimeoutThreshold
		);

		ToggleOwnerTracking(Config.bEnableOwnerTracking);
		ToggleBroadcastUpdatesToGameThread(Config.bDispatchUpdatesOnGameThread);
		return true;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[CrowdyActorTracker]: Map '%s' is not in the allowed levels list."), *CurrentMap);
	return false;
}

void UCrowdyActorTracker::ProcessQueue(int32 WorkerIndex)
{
	TArray<FCrowdyActorUpdate> Batch;
    Batch.Reserve(MaxUpdatesPerBatch);
    
    const double StartTime = FPlatformTime::Seconds();
    
    while (true)
    {
        FCrowdyActorUpdate Update;
        
        while (Batch.Num() < MaxUpdatesPerBatch && UpdateQueues[WorkerIndex]->Dequeue(Update))
            Batch.Add(MoveTemp(Update));
        
        const double Elapsed = FPlatformTime::Seconds() - StartTime;
        
        if (Batch.Num() >= MaxUpdatesPerBatch)               break;
        if (!Batch.IsEmpty() && Elapsed >= MaxBatchWaitTime) break;
        if (Batch.IsEmpty()  && Elapsed >= MaxBatchWaitTime) return;
        
        FPlatformProcess::Sleep(0.0005f);
    }
    
    if (Batch.IsEmpty())
        return;

	
    TArray<const FCrowdyActorUpdate*, TInlineAllocator<64>> ExistingUpdates;
    TArray<const FCrowdyActorUpdate*, TInlineAllocator<16>> NewUpdates;
    
    for (const FCrowdyActorUpdate& Update : Batch)
    {
        if (TrackedUUIDs->Contains(Update.UUID))
            ExistingUpdates.Add(&Update);
        else if (!PendingSpawns->Contains(Update.UUID))
            NewUpdates.Add(&Update);
    }
	
    if (!ExistingUpdates.IsEmpty())
    {
        // Single allocation, built from pointers — Batch is still alive here
        TArray<FCrowdyActorUpdate> ExistingBatch;
        ExistingBatch.Reserve(ExistingUpdates.Num());
        for (const FCrowdyActorUpdate* U : ExistingUpdates)
            ExistingBatch.Add(*U);

        // C++ fast path — direct ref, no extra copy
        OnUpdatesWorkerThread.Broadcast(ExistingBatch);

        if (bBroadcastUpdatesToGameThread)
        {
            // Move the array we just built into the capture — no extra copy
            AsyncTask(ENamedThreads::GameThread, [this, GTBatch = MoveTemp(ExistingBatch)]()
            {
                OnUpdatesGameThread.Broadcast(GTBatch);
            });
        }
    }
	
    if (!NewUpdates.IsEmpty())
    {
        TArray<FCrowdyActorUpdate> SpawnBatch;
        SpawnBatch.Reserve(NewUpdates.Num());
        
        for (const FCrowdyActorUpdate* Update : NewUpdates)
        {
            if (PendingSpawns->Add(Update->UUID))
                SpawnBatch.Add(*Update);
        }
        
        if (!SpawnBatch.IsEmpty())
        {
        	// Capture batch by value — safe to read from GT
        	AsyncTask(ENamedThreads::GameThread,
				[this, SpawnBatch = MoveTemp(SpawnBatch)]()
				{
					// Now truly on the game thread — GC locked, UObjects safe
					for (const FCrowdyActorUpdate& Update : SpawnBatch)
					{
						const int32 Count = ++NumOfTrackedActors;
						OnNewPlayerJoined.Broadcast(Update.UUID, Update.State, Count + 1);
					}

					for (const FCrowdyActorUpdate& Update : SpawnBatch)
					{
						PendingSpawns->Remove(Update.UUID);
						TrackedUUIDs->Add(Update.UUID);
					}
				}
			);
        }
    }
	
    {
        const double Now = FPlatformTime::Seconds();
        FWriteScopeLock W(LastUpdateLock);
        for (const FCrowdyActorUpdate& Update : Batch)
            LastUpdateTimes.Add(Update.UUID, Now);
    }
	
}

void UCrowdyActorTracker::CheckTimeouts()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FGuid> TimedOut;

	{
		FReadScopeLock R(LastUpdateLock);
		for (const auto& Pair : LastUpdateTimes)
			if (Now - Pair.Value > ActorTimeoutThreshold)
				TimedOut.Add(Pair.Key);
	}

	if (TimedOut.IsEmpty())
		return;

	{
		FWriteScopeLock W(LastUpdateLock);
		for (const FGuid& UUID : TimedOut)
			LastUpdateTimes.Remove(UUID);
	}

	ProcessTimedOutActors(MoveTemp(TimedOut));
}

void UCrowdyActorTracker::ProcessTimedOutActors(TArray<FGuid> TimedOut)
{
	for (const FGuid& UUID : TimedOut)
	{
		TrackedUUIDs->Remove(UUID);
		PendingSpawns->Remove(UUID);
		--NumOfTrackedActors;
	}

	const int32 Count = NumOfTrackedActors.load();

	FFunctionGraphTask::CreateAndDispatchWhenReady(
		[this, TimedOut = MoveTemp(TimedOut), Count]()
		{
			check(IsInGameThread());
			for (const FGuid& UUID : TimedOut)
				OnPlayerLeft.Broadcast(UUID, Count + 1);
		},
		TStatId{},
		nullptr,
		ENamedThreads::GameThread
	);
}

void UCrowdyActorTracker::SetLocalUUID(FString NewUUID)
{
	LocalUUID = USerializationFunctionLibrary::ToGuid(NewUUID);
	
	if (!LocalUUID.IsValid())
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Tracker]: Invalid local UUID: %s"), *NewUUID);
	
}

