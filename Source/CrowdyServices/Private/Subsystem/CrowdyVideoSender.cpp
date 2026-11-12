#include "Subsystem/CrowdyVideoSender.h"

#include "CrowdyCppReplication.h"
#include "CrowdyCppVideo.h"
#include "Engine/GameInstance.h"
#include "Subsystem/CrowdyGameSession.h"

const int32 UCrowdyVideoSender::MaxFrameBytes =
	FCrowdyCppVideoAssembler::MaxFragments * FCrowdyCppVideoAssembler::MaxFragmentBodyBytes;

void UCrowdyVideoSender::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (UCrowdyCppReplicationSubsystem* Replication = Collection.InitializeDependency<UCrowdyCppReplicationSubsystem>())
	{
		Replication->OnSendRefused.AddUniqueDynamic(this, &UCrowdyVideoSender::HandleSendRefused);
	}
}

void UCrowdyVideoSender::Deinitialize()
{
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UCrowdyCppReplicationSubsystem* Replication = GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>())
		{
			Replication->OnSendRefused.RemoveAll(this);
		}
	}

	Super::Deinitialize();
}

FCrowdyVideoFrameSend UCrowdyVideoSender::SendVideoFrame(const TArray<uint8>& Frame, const ECrowdyVideoCodec Codec, const int32 Distance)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	const UCrowdyGameSession* GameSession = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	const UCrowdyCppReplicationSubsystem* Replication = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;

	const TSharedPtr<FCrowdyCppReplication> Connection = Replication ? Replication->GetConnection() : nullptr;
	const bool bConnected = Connection.IsValid() && Replication->IsRouting();
	const FCrowdyActorId SenderId = GameSession ? FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID()) : FCrowdyActorId();

	FInt64Vector ChunkValue;
	const bool bHasChunk = GameSession && GameSession->TryGetLocalPlayerChunk(World, ChunkValue);

	return SendFrame(Frame, Codec, Distance, bConnected, SenderId, bHasChunk ? &ChunkValue : nullptr,
		[&](const int32 FrameId, const FInt64Vector& Chunk, const uint8 CodecByte, const uint8 DistanceChunks, int32& OutFragments, FString& OutError)
		{
			return Connection->SendVideoFrame(Chunk.X, Chunk.Y, Chunk.Z, SenderId.AsOctets(), Frame, FrameId, CodecByte,
				DistanceChunks, static_cast<uint8>(ECrowdyDecayRate::No_Decay), OutFragments, OutError);
		});
}

FCrowdyVideoFrameSend UCrowdyVideoSender::SendFrame(const TArray<uint8>& Frame, const ECrowdyVideoCodec Codec, const int32 Distance,
	const bool bConnected, const FCrowdyActorId& SenderId, const FInt64Vector* Chunk, const FTransmit Transmit)
{
	FCrowdyVideoFrameSend Send;
	auto Refuse = [&Send](const ECrowdyVideoSendResult Result)
	{
		Send.Result = Result;
		Send.Message = DescribeVideoSendResult(Result);
		return Send;
	};

	if (Codec != ECrowdyVideoCodec::Jpeg && Codec != ECrowdyVideoCodec::WebP)
	{
		return Refuse(ECrowdyVideoSendResult::CodecNotSent);
	}
	if (Frame.IsEmpty())
	{
		return Refuse(ECrowdyVideoSendResult::Empty);
	}
	if (Frame.Num() > MaxFrameBytes)
	{
		return Refuse(ECrowdyVideoSendResult::TooLarge);
	}
	if (!bConnected)
	{
		return Refuse(ECrowdyVideoSendResult::NotConnected);
	}
	if (!SenderId.IsSet() || !Chunk)
	{
		return Refuse(ECrowdyVideoSendResult::NoLocalPlayer);
	}

	// Taken before the send and kept when it fails, so a frame that may have reached the wire never shares its id.
	// A relaunched sender seeds from the clock so a receiver still holding its old frame id does not drop the new frames as stale.
	uint16* NextId = NextFrameIds.Find(SenderId);
	if (!NextId)
	{
		NextId = &NextFrameIds.Add(SenderId, FrameIdSeedAt(FDateTime::UtcNow()));
	}
	const int32 FrameId = (*NextId)++;

	int32 Fragments = 0;
	FString Error;
	if (!Transmit(FrameId, *Chunk, static_cast<uint8>(Codec), static_cast<uint8>(FMath::Clamp(Distance, 0, 8)), Fragments, Error))
	{
		// The bridge's other refusals (queue room, nothing to send) mean the queue could not take the frame; the rest are pre-checked here.
		const bool bLinkDown = Error.Contains(TEXT("closed")) || Error.Contains(TEXT("not open"));
		Refuse(bLinkDown ? ECrowdyVideoSendResult::NotConnected : ECrowdyVideoSendResult::QueueFull);
		Send.Message = Error;
		return Send;
	}

	Send.Result = ECrowdyVideoSendResult::Sent;
	Send.FrameId = FrameId;
	Send.Fragments = Fragments;
	return Send;
}

uint16 UCrowdyVideoSender::FrameIdSeedAt(const FDateTime& Now)
{
	return static_cast<uint16>((Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond()) / 100);
}

FString UCrowdyVideoSender::DescribeVideoSendResult(const ECrowdyVideoSendResult Result)
{
	switch (Result)
	{
	case ECrowdyVideoSendResult::Sent:
		return TEXT("The frame left this client. There is no delivery receipt; a refusal arrives later on On Video Send Refused.");
	case ECrowdyVideoSendResult::NotConnected:
		return TEXT("The realtime connection is not up.");
	case ECrowdyVideoSendResult::NoLocalPlayer:
		return TEXT("The player has no actor id yet, or no local pawn to place the frame at.");
	case ECrowdyVideoSendResult::Empty:
		return TEXT("There is no frame to send.");
	case ECrowdyVideoSendResult::TooLarge:
		return FString::Printf(TEXT("A frame can be at most %d bytes."), MaxFrameBytes);
	case ECrowdyVideoSendResult::QueueFull:
		return TEXT("The outbound queue had no room for the frame, or the connection refused it.");
	case ECrowdyVideoSendResult::CodecNotSent:
		return TEXT("Only JPEG and WebP frames can be sent.");
	}
	return FString();
}

bool UCrowdyVideoSender::IsVideoRefusal(const FCrowdySendRefusal& Refusal)
{
	return !Refusal.bChannelMessage && Refusal.MessageType == static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_PACKET);
}

void UCrowdyVideoSender::HandleSendRefused(const FCrowdySendRefusal& Refusal)
{
	if (!IsVideoRefusal(Refusal))
	{
		return;
	}

#if WITH_DEV_AUTOMATION_TESTS
	++VideoRefusalsBroadcast;
#endif
	OnVideoSendRefused.Broadcast(Refusal);
}
