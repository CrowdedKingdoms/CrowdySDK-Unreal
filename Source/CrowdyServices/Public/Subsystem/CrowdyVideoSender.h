#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyVideoSender.generated.h"

/** Why Send Video Frame did or did not send. */
UENUM(BlueprintType)
enum class ECrowdyVideoSendResult : uint8
{
	/** The frame left this client. There is no delivery receipt; a refusal arrives later on On Video Send Refused. */
	Sent UMETA(DisplayName = "Sent"),
	/** No realtime connection yet, or it has closed. */
	NotConnected UMETA(DisplayName = "Not Connected"),
	/** The player has no actor id yet, or no local pawn to place the frame at. */
	NoLocalPlayer UMETA(DisplayName = "No Local Player"),
	Empty UMETA(DisplayName = "Empty"),
	/** The frame needs more fragments than one frame may cross as. */
	TooLarge UMETA(DisplayName = "Too Large"),
	/** The outbound queue had no room for every fragment, or the connection refused the frame. */
	QueueFull UMETA(DisplayName = "Queue Full"),
	/** Only JPEG and WebP frames can be sent. */
	CodecNotSent UMETA(DisplayName = "Codec Not Sent")
};

/** What Send Video Frame did with one frame. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyVideoFrameSend
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Video")
	ECrowdyVideoSendResult Result = ECrowdyVideoSendResult::NotConnected;

	/** The id the frame travelled under, which wraps at 65535. Meaningful only when Result is Sent. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Video")
	int32 FrameId = 0;

	/** How many datagrams the frame was split into. Zero unless Result is Sent. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Video")
	int32 Fragments = 0;

	/** Why the frame was not sent, in words; empty when it was. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Video")
	FString Message;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyVideoSendRefused, const FCrowdySendRefusal&, Refusal);

/**
 * Sends encoded video frames to the players near you. A frame is one whole JPEG or WebP image, split into datagrams
 * for you and reassembled by every receiver. It reaches every player whose chunk is within the distance you give of
 * the chunk your pawn stands in, CrowdyJS and CrowdyCPP players included, and your own client does not get it back.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyVideoSender : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The largest frame in bytes: the most fragments a frame may cross as, each carrying its full slice. */
	static const int32 MaxFrameBytes;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Sends Frame, one whole encoded image, to every player within Distance chunks (0 to 8) of the chunk your pawn stands
	 * in. Nothing is sent unless the result is Sent, and Sent means only that the frame was queued on this client. The
	 * frame id counts up per player and wraps, and is spent even when the send fails, so no id is used twice.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Video")
	FCrowdyVideoFrameSend SendVideoFrame(const TArray<uint8>& Frame, ECrowdyVideoCodec Codec = ECrowdyVideoCodec::Jpeg, int32 Distance = 1);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Video")
	static FString DescribeVideoSendResult(ECrowdyVideoSendResult Result);

	/** Whether Refusal is the server refusing a video frame. */
	static bool IsVideoRefusal(const FCrowdySendRefusal& Refusal);

	/** Queues one frame: fills in the frame id, chunk and codec byte, and returns whether it was accepted with how many fragments or why not. */
	using FTransmit = TFunctionRef<bool(int32 FrameId, const FInt64Vector& Chunk, uint8 CodecByte, uint8 DistanceChunks, int32& OutFragments, FString& OutError)>;

	/** The send with its context already resolved, so every refusal can be checked without a connection. Chunk is null when there is no local pawn. */
	FCrowdyVideoFrameSend SendFrame(const TArray<uint8>& Frame, ECrowdyVideoCodec Codec, int32 Distance, bool bConnected,
		const FCrowdyActorId& SenderId, const FInt64Vector* Chunk, FTransmit Transmit);

	/** Fires when the server refuses one of your frames, for example because the player's access tier lacks use_video_chat. Late, at most once a second, and on a busy connection it may name the wrong send: show it, do not act on it. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Video|Events")
	FOnCrowdyVideoSendRefused OnVideoSendRefused;

	UFUNCTION()
	void HandleSendRefused(const FCrowdySendRefusal& Refusal);

	/** The first frame id of a sender at Now: one step per 100 ms, wrapping at 65536. */
	static uint16 FrameIdSeedAt(const FDateTime& Now);

#if WITH_DEV_AUTOMATION_TESTS
	int32 VideoRefusalsBroadcast = 0;

	void SeedFrameIdForTest(const FCrowdyActorId& SenderId, const uint16 Seed) { NextFrameIds.Add(SenderId, Seed); }
#endif

private:
	TMap<FCrowdyActorId, uint16> NextFrameIds;
};
