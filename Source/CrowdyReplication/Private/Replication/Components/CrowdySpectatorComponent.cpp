#include "Replication/Components/CrowdySpectatorComponent.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "TimerManager.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"

float CrowdySpectator::ClampInterval(const float Seconds)
{
	return FMath::Max(Seconds, MinIntervalSeconds);
}

int32 CrowdySpectator::ClampDistance(const int32 Distance)
{
	return FMath::Clamp(Distance, 0, MaxDistance);
}

bool CrowdySpectator::CountsAsSent(const ECrowdyCppSendOutcome Outcome)
{
	return Outcome == ECrowdyCppSendOutcome::Sent;
}

FString CrowdySpectator::ViewerUuid(const FString& SessionUuid, const int64 UserId)
{
	FCrowdyActorId Id;
	if (FCrowdyActorId::TryFromString(SessionUuid, Id))
	{
		return SessionUuid;
	}

	if (UserId <= 0)
	{
		return FString();
	}

	return UHelperFunctions::GetDeterministicID(UserId).ToString(EGuidFormats::Digits);
}

void CrowdySpectator::FillBeatMessage(const FBeat& Beat, const int64 AppId, FActorUpdateRequestMessage& Message)
{
	Message.AppID = AppId;
	Message.ChunkX = Beat.Chunk.X;
	Message.ChunkY = Beat.Chunk.Y;
	Message.ChunkZ = Beat.Chunk.Z;
	Message.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(ClampDistance(Beat.Distance));
	Message.UUID = FCrowdyActorId::FromStringOrUnset(Beat.Uuid);
	Message.bViewerBeat = true;
}

void CrowdySpectator::FCore::Start(const FInt64Vector& Chunk)
{
	bRunning = true;
	LastChunk = Chunk;
	Emit();
}

void CrowdySpectator::FCore::Stop()
{
	bRunning = false;
	bOwesBeat = false;
}

void CrowdySpectator::FCore::Pulse()
{
	if (!bRunning)
	{
		return;
	}

	Emit();
}

bool CrowdySpectator::FCore::Watch(const FInt64Vector& Chunk)
{
	if (!bRunning)
	{
		return false;
	}

	if (Chunk == LastChunk && !bOwesBeat)
	{
		return false;
	}

	LastChunk = Chunk;
	Emit();
	return true;
}

void CrowdySpectator::FCore::Emit()
{
	FBeat Beat;
	Beat.Chunk = LastChunk;
	Beat.Distance = ClampDistance(Distance);
	Beat.Uuid = ResolveUuid ? ResolveUuid() : FString();
	bOwesBeat = Beat.Uuid.IsEmpty() || !Send || !Send(Beat);
}

UCrowdySpectatorComponent::UCrowdySpectatorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UCrowdySpectatorComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoStart)
	{
		StartSpectating();
	}
}

void UCrowdySpectatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSpectating();
	Super::EndPlay(EndPlayReason);
}

void UCrowdySpectatorComponent::StartSpectating()
{
	if (Core.IsRunning())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !GetOwner())
	{
		return;
	}

	FInt64Vector Chunk;
	if (!ChunkOfOwner(Chunk))
	{
		return;
	}

	Core.Distance = Distance;
	Core.ResolveUuid = [this]() { return ResolveBeatUuid(); };
	Core.Send = [this](const CrowdySpectator::FBeat& Beat) { return SendBeat(Beat); };
	Core.Start(Chunk);

	World->GetTimerManager().SetTimer(WatchTimer, this, &UCrowdySpectatorComponent::OnWatchTimer,
		CrowdySpectator::WatchSeconds, true);
	RestartBeatTimer();
}

void UCrowdySpectatorComponent::StopSpectating()
{
	Core.Stop();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FTimerManager& Timers = World->GetTimerManager();
	Timers.ClearTimer(BeatTimer);
	Timers.ClearTimer(WatchTimer);
}

void UCrowdySpectatorComponent::OnBeatTimer()
{
	Core.Pulse();
}

void UCrowdySpectatorComponent::OnWatchTimer()
{
	FInt64Vector Chunk;
	if (!ChunkOfOwner(Chunk))
	{
		return;
	}

	if (Core.Watch(Chunk) && !Core.IsOwingBeat())
	{
		RestartBeatTimer();
	}
}

void UCrowdySpectatorComponent::RestartBeatTimer()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	World->GetTimerManager().SetTimer(BeatTimer, this, &UCrowdySpectatorComponent::OnBeatTimer,
		CrowdySpectator::ClampInterval(Interval), true);
}

bool UCrowdySpectatorComponent::ChunkOfOwner(FInt64Vector& OutChunk)
{
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	const UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
	if (!IsValid(Owner) || !Instance || !Instance->GetSubsystem<UCrowdyGameSession>())
	{
		return false;
	}

	UHelperFunctions::GetChunkCoordinateAtLocation(this, Owner->GetActorLocation(), OutChunk.X, OutChunk.Y, OutChunk.Z);
	return true;
}

FString UCrowdySpectatorComponent::ResolveBeatUuid() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
	const UCrowdyGameSession* Session = Instance ? Instance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	if (!Session)
	{
		return FString();
	}

	FString Uuid = CrowdySpectator::ViewerUuid(Session->GetUUID(), Session->GetUserID());
	if (Uuid.IsEmpty())
	{
		return Uuid;
	}

	// The local player's own replicated actor beats under this id with its real state, and an empty beat must never
	// stand in for it.
	const UCrowdyEntitySubsystem* Entities = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (Entities && IsValid(Entities->FindParticipant(USerializationFunctionLibrary::ToGuid(Uuid))))
	{
		return FString();
	}

	return Uuid;
}

bool UCrowdySpectatorComponent::SendBeat(const CrowdySpectator::FBeat& Beat) const
{
	const UWorld* World = GetWorld();
	const UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
	if (!Instance)
	{
		return false;
	}

	const UCrowdyGameSession* Session = Instance->GetSubsystem<UCrowdyGameSession>();
	UCrowdyCppReplicationSubsystem* Replication = Instance->GetSubsystem<UCrowdyCppReplicationSubsystem>();
	if (!Session || !Replication)
	{
		return false;
	}

	if (!Replication->IsRouting() && !Replication->IsLocalDeliveryArmed())
	{
		return false;
	}

	FActorUpdateRequestMessage Message;
	CrowdySpectator::FillBeatMessage(Beat, Session->GetAppID(), Message);
	return CrowdySpectator::CountsAsSent(Replication->TrySendMessage(Message));
}
