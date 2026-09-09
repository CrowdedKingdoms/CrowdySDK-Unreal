#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/UniquePtr.h"

/**
 * One video frame put back together out of the fragments it crossed as.
 *
 * Everything here is owned, unlike a delivered message, so a completed frame may be kept and decoded on
 * another thread. Bytes is the encoded image exactly as the sender produced it: nothing in this SDK
 * decodes it, and nothing has checked that it is a valid image.
 */
struct FCrowdyCppAssembledVideoFrame
{
	/** 32 ASCII octets addressing the sender, not null-terminated. */
	TArray<uint8> SenderUuid;

	/** The sender's own frame counter. It wraps, so it orders frames from one sender and nothing else. */
	int32 FrameId = 0;

	/** The wire codec byte: 0 for JPEG, 1 for WebP. Only those two ever reach here. */
	uint8 Codec = 0;

	/** The whole encoded image. */
	TArray<uint8> Bytes;

	/** The caller's clock when the last fragment arrived, as passed to Ingest. */
	int64 CompletedAtMs = 0;
};

/**
 * Puts video fragments back into frames, per sender.
 *
 * A datagram carries at most 1117 octets of encoded image, so every frame arrives as several fragments and
 * a consumer that reads one notification has an unusable slice. Feed every fragment here and take the frame
 * when one comes back.
 *
 * This wraps the vendored assembler rather than reimplementing it, because the rules it applies are shared
 * with the browser SDK byte for byte: an older frame id is a straggler and is dropped, a newer one abandons
 * whatever was still in progress, and the counter wraps, so "older" is a comparison and not a subtraction.
 * A second implementation of those rules would diverge from the other SDK the first time one of them was
 * corrected.
 *
 * There is no recovery for a lost fragment: the protocol has no retransmit, and the next frame is the
 * recovery. Nothing here is thread safe, and one instance holds one sender's partial frame at a time.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppVideoAssembler
{
public:
	/** The header in front of every fragment's slice. Asserted against the vendored contract. */
	static constexpr int32 FragmentHeaderBytes = 6;

	/** The most encoded image one fragment carries, with the signature tail present. */
	static constexpr int32 MaxFragmentBodyBytes = 1117;

	/** A frame is refused above this many fragments, which is about 17.8 KB of image. */
	static constexpr int32 MaxFragments = 16;

	/** How long an incomplete frame survives without a new fragment before Prune abandons it. */
	static constexpr int64 DefaultFrameTimeoutMs = 500;

	explicit FCrowdyCppVideoAssembler(int64 TimeoutMs = DefaultFrameTimeoutMs);
	~FCrowdyCppVideoAssembler();

	/**
	 * Take one fragment, header included, and answer whether it completed a frame.
	 *
	 * NowMs is the caller's own monotonic clock in milliseconds, and is what the timeout is measured
	 * against; a caller that passes an inconsistent one gets frames abandoned early or never. A fragment
	 * with a malformed header, or one belonging to a frame this sender has already moved past, is counted
	 * and dropped rather than refused, so false means "not yet" and not "bad input".
	 */
	bool Ingest(TArrayView<const uint8> SenderUuid, TArrayView<const uint8> Fragment, int64 NowMs,
		FCrowdyCppAssembledVideoFrame& OutFrame);

	/** Abandon frames that have not progressed within the timeout, and answer how many. */
	int32 Prune(int64 NowMs);

	/** Drop a sender's partial frame and forget its counter. Call it when the sender leaves. */
	void Forget(TArrayView<const uint8> SenderUuid);

	/** How many senders have a frame in progress. */
	int32 PendingSenders() const;

	/** Fragments dropped for a malformed header or for a frame the sender has moved past. */
	int64 GetDroppedFragments() const;

	/** Frames abandoned incomplete, whether by a newer frame, by the timeout, or by Forget. */
	int64 GetAbandonedFrames() const;

	/**
	 * Split one encoded frame into the fragments it is sent as, each header plus slice.
	 *
	 * Answers false and writes nothing when the frame is empty or would need more than MaxFragments, which
	 * is the send path's own refusal: a frame is never sent in part.
	 *
	 * A codec the protocol does not assign, and a frame id outside the range the two header octets carry,
	 * are refused here as well. Both would otherwise produce fragments that look well formed and that every
	 * receiver is required to drop, so the caller would see a successful split and the far end nothing.
	 */
	static bool FragmentFrame(TArrayView<const uint8> Frame, int32 FrameId, uint8 Codec,
		TArray<TArray<uint8>>& OutFragments);

private:
	FCrowdyCppVideoAssembler(const FCrowdyCppVideoAssembler&) = delete;
	FCrowdyCppVideoAssembler& operator=(const FCrowdyCppVideoAssembler&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
