#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "CrowdyCppVideo.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystems/WorldSubsystem.h"

#include "CrowdyVideoFrameReceiver.generated.h"

struct FCrowdyDelivery;

/**
 * One complete webcam frame from one sender, put back together out of the fragments it crossed as.
 *
 * Bytes is the encoded image and nothing more: this SDK does not decode it, does not know its dimensions,
 * and has not checked that it is a valid image. A consumer turns it into a texture itself.
 */
USTRUCT(BlueprintType)
struct FCrowdyVideoFrame
{
	GENERATED_BODY()

	/**
	 * The key derived from the sender's wire id, which is what the rest of the SDK addresses an actor by.
	 *
	 * The derivation reads the 32 octets as hexadecimal, so two senders whose ids differ only outside that
	 * alphabet derive the same key. Use SenderId to tell two senders apart for certain.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	FGuid SenderUUID;

	/** The sender's 32 wire octets as text. Exact, unlike SenderUUID, and empty for an unset id. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	FString SenderId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	ECrowdyVideoCodec Codec = ECrowdyVideoCodec::Jpeg;

	/** The sender's own counter. It wraps, so it orders that sender's frames and means nothing across two. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	int32 FrameId = 0;

	/** The encoded image, JPEG or WebP according to Codec. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	TArray<uint8> Bytes;

	/** The chunk the last fragment was broadcast over. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	int64 ChunkX = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	int64 ChunkY = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	int64 ChunkZ = 0;

	/** When the server stamped that last fragment, in milliseconds since the epoch. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Video")
	int64 ServerTimestamp = 0;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCrowdyVideoFrameReady, const FCrowdyVideoFrame&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyVideoFrameAssembled, const FCrowdyVideoFrame&, Frame);

/**
 * Turns the video fragments the server fans out into whole frames.
 *
 * A single notification carries at most 1117 octets of encoded image, so a consumer subscribing to the
 * opcode itself would see slices and never a picture. This holds the reassembler that puts them back
 * together and announces each frame once, which is the level a consumer wants.
 *
 * With no delegate bound a fragment is dropped as soon as it reaches this class, so an app that never shows
 * video pays for none of the reassembly: no copy of a fragment, no partial frame held, no announcement. It
 * still pays what delivering any message costs, because the drop happens after the frame has been decoded
 * and routed here. Binding is what turns reassembly on, and a frame already in flight when a listener binds
 * is the one frame that may be missed.
 *
 * A client's own fragments are discarded rather than reassembled. The server fans a video packet out over
 * the chunk it was sent to, which includes the sender, and a frame of your own camera is one you already
 * have.
 *
 * There is no capture or display here, and no texture: Bytes is handed over encoded.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Video Frame Receiver"))
class CROWDYREPLICATION_API UCrowdyVideoFrameReceiver : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	UCrowdyVideoFrameReceiver();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** Fires on the game thread once per completed frame. C++ only. */
	FOnCrowdyVideoFrameReady OnVideoFrameReady;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Video|Events")
	FOnCrowdyVideoFrameAssembled OnVideoFrameAssembled;

	/** Senders with a frame half arrived. Zero while nothing is listening, since nothing is ingested. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Video")
	int32 GetPendingSenderCount() const;

	/** Fragments dropped for a malformed header or for a frame their sender has already moved past. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Video")
	int64 GetDroppedFragmentCount() const;

	/** Frames abandoned incomplete, whether by a newer frame, by the timeout, or by a sender leaving. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Video")
	int64 GetAbandonedFrameCount() const;

	/**
	 * Feed one fragment as though it had arrived, and answer whether it completed a frame.
	 *
	 * The delivery path calls this; it is public so the reassembly rules can be exercised without a socket.
	 */
	bool IngestFragment(const FCrowdyActorId& SenderId, const FGuid& SenderGuid,
		TConstArrayView<uint8> Fragment, int64 NowMs, FCrowdyCppAssembledVideoFrame& OutFrame);

	/**
	 * Drop whatever is half assembled for one sender, and forget its frame counter.
	 *
	 * The departure paths call this. It is exposed because a sender the server never announced as gone is
	 * never released otherwise, and a sender still holding a counter that rejoins and restarts at frame zero
	 * has every fragment dropped as a straggler until its counter climbs back past the old value.
	 *
	 * One key can name more than one sender, since the derivation is lossy, and every sender it names is
	 * forgotten.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Video")
	void ForgetSender(const FGuid& SenderGuid);

	/**
	 * Take one delivered fragment, and announce the frame if it completed one.
	 *
	 * The subscription calls this; it is public so the announce step can be driven without a socket, which
	 * is the only way to tell a receiver that assembles frames from one that assembles them and tells
	 * nobody.
	 */
	void HandleVideoDelivery(const FCrowdyDelivery& Delivery);

	/** What the tracker's departure delegate calls. Public for the same reason as the delivery handler. */
	UFUNCTION()
	void HandleActorLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount);

	// A departure the server never announced, or whose datagram was lost, is only ever seen by the staleness
	// check, so this releases a sender's partial frames on that path too.
	UFUNCTION()
	void HandleActorTimedOut(FGuid UUID, int32 ActorCount);

	/** Learns which id is this client's own, so its own fragments can be discarded. Public for testing. */
	UFUNCTION()
	void HandleOwnerUUIDUpdated(FString NewUUID);

private:

	// Released on Deinitialize, which is what stops delivery.
	FCrowdySubscription VideoSubscription;

	/** True when something would receive a frame, and so when a fragment is worth reassembling. */
	bool HasListeners() const;

	TUniquePtr<FCrowdyCppVideoAssembler> Assembler;

	// The assembler is keyed by the 32 octets on the wire; a departure names only the derived guid, and that
	// derivation reads the octets as hexadecimal, so ids differing only outside that alphabet derive one key.
	// Every id a key names is therefore kept, and a departure forgets all of them: keeping only the newest
	// would forget the wrong sender's frame and strand the departed one's counter for the life of the world.
	// One entry is the case that is not a collision, so the inline element carries it without allocating.
	TMap<FGuid, TArray<FCrowdyActorId, TInlineAllocator<1>>> SenderIdsByGuid;

	// This client's own wire id, compared octet for octet rather than by the derived key so a collision
	// cannot silence another sender. Unset until the session reports one, which is when nothing is being
	// sent either.
	FCrowdyActorId LocalSenderId;

	// When the timeout sweep last ran. Frames only expire while fragments are arriving, which is when a
	// stalled one is worth reclaiming; with no traffic at all the most that lingers is one partial frame
	// per sender until that sender leaves.
	int64 LastPruneMs = 0;
};
