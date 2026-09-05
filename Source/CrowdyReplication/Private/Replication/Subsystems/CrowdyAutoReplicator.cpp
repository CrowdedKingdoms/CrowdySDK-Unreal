// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "CrowdyReplicationLog.h"

#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Replication/Interfaces/CrowdyReplicationSource.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Replication/State/CrowdyBitwiseCompare.h"
#include "Utils/UCrowdyClassRegistry.h"

namespace CrowdyAutoReplication
{
	ESendDecision DecideSend(const FSendInputs& Inputs)
	{
		// An entry that has never sent has nothing to have changed from, so its first interval is always a full
		// send. Without this it would compare against a default-constructed state and could decide it is unchanged.
		if (!Inputs.bSendOnlyOnChange || !Inputs.bHasSentBefore)
		{
			return ESendDecision::FullUpdate;
		}

		if (Inputs.bStateOrChunkChanged || Inputs.bKeyframeDue)
		{
			return ESendDecision::FullUpdate;
		}

		// Only reached while the actor is unchanged, which is the only time a heartbeat says anything a full send
		// would not have said better.
		return Inputs.bHeartbeatDue ? ESendDecision::Heartbeat : ESendDecision::None;
	}

	bool HasStateChanged(const FInstancedStruct& LastSent, const FInstancedStruct& Live, const int32 ComparableBytes)
	{
		// The engine's own compare, which additionally forces a change while its struct is being reinstanced.
		if (ComparableBytes <= 0 || LastSent.GetScriptStruct() != Live.GetScriptStruct())
		{
			return LastSent != Live;
		}

		const uint8* const LastMemory = LastSent.GetMemory();
		const uint8* const LiveMemory = Live.GetMemory();
		if (!LastMemory || !LiveMemory)
		{
			return LastMemory != LiveMemory;
		}

		return FMemory::Memcmp(LastMemory, LiveMemory, ComparableBytes) != 0;
	}

	int32 ResolveCompareBytes(const UScriptStruct* PreviousStruct, const UScriptStruct* SentStruct,
		const int32 CurrentBytes)
	{
		// A type answers this the same way every time and an entry's type does not move, so an entry that
		// sends every tick must not walk its type's properties every tick to be told the same thing.
		if (PreviousStruct == SentStruct)
		{
			return CurrentBytes;
		}

		return CrowdyBitwiseCompare::ComparableBytes(SentStruct);
	}
}

void UCrowdyAutoReplicator::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();

	if (!IsValid(World))
		return;

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
		return;

	Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	check(Bridge);


	if (!IsValid(Bridge))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy SDK][AutoReplicator]: Invalid Bridge subsystem."));
		return;
	}

	constexpr int32 ExpectedActor = 2048;
	Data.Components.Reserve(ExpectedActor);
	Data.Sources.Reserve(ExpectedActor);
	Data.Positions.Reserve(ExpectedActor);
	Data.Chunks.Reserve(ExpectedActor);
	Data.UUIDs.Reserve(ExpectedActor);
	Data.LastSentStates.Reserve(ExpectedActor);
	Data.LastSentCompareBytes.Reserve(ExpectedActor);
	Data.LastSentChunks.Reserve(ExpectedActor);
	Data.NextKeyframeTimes.Reserve(ExpectedActor);
	Data.NextHeartbeatTimes.Reserve(ExpectedActor);
	Data.bHasSent.Reserve(ExpectedActor);

	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	bIsTicking = Profile && Profile->bUseAutoReplicator;

	if (bIsTicking)
	{
		ReplicationInterval = 1.0f / FMath::Max(1, Profile->ReplicationIntervalHz);
		bSendOnlyOnChange = Profile->bSendActorStateOnlyOnChange;
		KeyframeIntervalSeconds = FMath::Max(0.0f, Profile->ActorKeyframeIntervalSeconds);
		HeartbeatIntervalSeconds = FMath::Max(0.0f, Profile->ActorHeartbeatIntervalSeconds);
	}
}

double UCrowdyAutoReplicator::NowSeconds() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

void UCrowdyAutoReplicator::ScheduleFirstSends(const int32 Index)
{
	const double Now = NowSeconds();

	// A deterministic offset from the entry's own id rather than a random one, so two clients replaying the same
	// session stagger the same way and a repro stays a repro.
	const uint32 Spread = GetTypeHash(Data.UUIDs[Index]);
	const double KeyframePhase = KeyframeIntervalSeconds > 0.0f
		? (Spread % 1024) / 1024.0 * KeyframeIntervalSeconds : 0.0;
	const double HeartbeatPhase = HeartbeatIntervalSeconds > 0.0f
		? ((Spread >> 10) % 1024) / 1024.0 * HeartbeatIntervalSeconds : 0.0;

	Data.NextKeyframeTimes[Index] = Now + KeyframePhase;
	Data.NextHeartbeatTimes[Index] = Now + HeartbeatPhase;
}

void UCrowdyAutoReplicator::Deinitialize()
{
	Super::Deinitialize();
}

bool UCrowdyAutoReplicator::ShouldCreateSubsystem(UObject* Outer) const
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

TStatId UCrowdyAutoReplicator::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(AutoReplicator, STATGROUP_Tickables);
}

void UCrowdyAutoReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	
	if (!bIsTicking)
		return;
	
	ReplicationAccumulator += DeltaTime;
	
	if (ReplicationAccumulator >= ReplicationInterval)
	{
		ReplicationAccumulator -= ReplicationInterval;
		ReplicationLoop();
	}
}

void UCrowdyAutoReplicator::RegisterReplicationComponent(UActorComponent* Component)
{
	if (!IsValid(Component)) return;

	const ICrowdyReplicationSource* Source = Cast<ICrowdyReplicationSource>(Component);
	if (!Source)
	{
		UE_LOG(LogCrowdyReplication, Error,
			TEXT("[Crowdy SDK][AutoReplicator]: '%s' does not implement ICrowdyReplicationSource."),
			*GetNameSafe(Component));
		return;
	}

	if (Data.Components.Contains(Component)) return;

	// Write UUID/State by value: no dangling pointer risk
	Data.Components.Add(Component);
	Data.Sources.Add(Source);
	Data.Positions.Add(FVector3f::ZeroVector);
	Data.Chunks.Add(FInt64Vector::ZeroValue);
	Data.UUIDs.Add(Source->GetReplicationUUID());   // copy

	// Resolved here, once, rather than in the replication loop: the lookup builds the class path into a
	// string and hashes it, which the loop must not pay per entity per tick.
	const AActor* RegisteringActor = Source->GetReplicatedActor();
	const FCrowdyClassID ClassID = RegisteringActor
		? UCrowdyClassRegistry::Get()->GetID(RegisteringActor->GetClass()) : CROWDY_INVALID_CLASS_ID;

	// An update carries no class path to fall back on, so an entry that cannot name its class would send
	// frames a receiver has to resolve by guessing from the wire struct, where two classes sharing one
	// struct collapse into whichever registered last. Said once here rather than per dropped update.
	UE_CLOG(ClassID == CROWDY_INVALID_CLASS_ID, LogCrowdyReplication, Error,
		TEXT("[Crowdy SDK][AutoReplicator]: '%s' has no entity class id, so its updates cannot name their class and will not be sent. A class registration refused over an id clash is the usual cause."),
		*GetNameSafe(RegisteringActor));

	Data.ClassIDs.Add(ClassID);
	Data.LastSentStates.AddDefaulted();
	Data.LastSentCompareBytes.Add(0);
	Data.LastSentChunks.Add(FInt64Vector::ZeroValue);
	Data.NextKeyframeTimes.Add(0.0);
	Data.NextHeartbeatTimes.Add(0.0);
	Data.bHasSent.Add(false);
	Data.Count++;

	ScheduleFirstSends(Data.Count - 1);
}

void UCrowdyAutoReplicator::UnregisterReplicationComponent(UActorComponent* Component)
{
	if (!IsValid(Component)) return;

	// Swap-remove to avoid O(n) shifts
	for (int32 i = 0; i < Data.Count; i++)
	{
		if (Data.Components[i] != Component) continue;

		Data.Components.RemoveAtSwap(i);
		Data.Sources.RemoveAtSwap(i);
		Data.Positions.RemoveAtSwap(i);
		Data.Chunks.RemoveAtSwap(i);
		Data.UUIDs.RemoveAtSwap(i);
		Data.ClassIDs.RemoveAtSwap(i);
		Data.LastSentStates.RemoveAtSwap(i);
		Data.LastSentCompareBytes.RemoveAtSwap(i);
		Data.LastSentChunks.RemoveAtSwap(i);
		Data.NextKeyframeTimes.RemoveAtSwap(i);
		Data.NextHeartbeatTimes.RemoveAtSwap(i);
		Data.bHasSent.RemoveAtSwap(i);
		Data.Count--;
		break;
	}
}

void UCrowdyAutoReplicator::ReplicationLoop()
{
	const int32 Count = Data.Count;
	if (Count == 0) return;

	if (!Bridge || !Bridge->DispatchActorUpdateFn)
		return;

	const double Now = NowSeconds();

	for (int32 i = 0; i < Count; i++)
	{
		if (!Data.Components[i].IsValid())
			continue;

		const ICrowdyReplicationSource* Comp = Data.Sources[i];

		const AActor* Actor = Comp->GetReplicatedActor();
		if (!IsValid(Actor))
			continue;

		const FVector3f NewPos = FVector3f(Actor->GetActorLocation());
		Data.Positions[i]    = NewPos;
		UHelperFunctions::GetChunkCoordinateAtLocation(this,FVector(NewPos), Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z);

		const FInstancedStruct& State = Comp->GetReplicatedState();

		CrowdyAutoReplication::FSendInputs Inputs;
		Inputs.bSendOnlyOnChange = bSendOnlyOnChange;
		Inputs.bHasSentBefore = Data.bHasSent[i];
		// The chunk is compared as well as the state, because it rides the header rather than the payload, so an
		// actor can cross a boundary without its state changing.
		Inputs.bStateOrChunkChanged = Data.LastSentChunks[i] != Data.Chunks[i]
			|| CrowdyAutoReplication::HasStateChanged(Data.LastSentStates[i], State,
				Data.LastSentCompareBytes[i]);
		Inputs.bKeyframeDue = KeyframeIntervalSeconds > 0.0f && Now >= Data.NextKeyframeTimes[i];
		Inputs.bHeartbeatDue = HeartbeatIntervalSeconds > 0.0f && Now >= Data.NextHeartbeatTimes[i];

		const CrowdyAutoReplication::ESendDecision Decision = CrowdyAutoReplication::DecideSend(Inputs);

		if (Decision == CrowdyAutoReplication::ESendDecision::FullUpdate)
		{
			Bridge->DispatchActorUpdateFn(Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z,
				ECrowdyDecayRate::No_Decay,
				ECrowdyReplicationDistance::Eight_Chunks,
				Data.UUIDs[i],
				State,
				Data.ClassIDs[i],
				false);

			// A send that fails to serialize is not worth re-sending identically next interval, and the keyframe
			// below is what repairs anything lost, so an entry records what it offered rather than what left.
			const UScriptStruct* const PreviousStruct = Data.LastSentStates[i].GetScriptStruct();
			Data.LastSentStates[i] = State;
			Data.LastSentCompareBytes[i] = CrowdyAutoReplication::ResolveCompareBytes(PreviousStruct,
				State.GetScriptStruct(), Data.LastSentCompareBytes[i]);
			Data.LastSentChunks[i] = Data.Chunks[i];
			Data.bHasSent[i] = true;

			// A full send makes both clocks moot: it is itself a keyframe, and it is proof of presence.
			if (KeyframeIntervalSeconds > 0.0f)
				Data.NextKeyframeTimes[i] = Now + KeyframeIntervalSeconds;
			if (HeartbeatIntervalSeconds > 0.0f)
				Data.NextHeartbeatTimes[i] = Now + HeartbeatIntervalSeconds;

			continue;
		}

		// Unchanged and no keyframe due: say the actor is still here for the cost of a header.
		if (Decision == CrowdyAutoReplication::ESendDecision::Heartbeat && Bridge->DispatchActorHeartbeatFn)
		{
			Bridge->DispatchActorHeartbeatFn(Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z, Data.UUIDs[i]);
			Data.NextHeartbeatTimes[i] = Now + HeartbeatIntervalSeconds;
		}
	}

}
