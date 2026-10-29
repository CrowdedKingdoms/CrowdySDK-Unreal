#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/UniquePtr.h"

/** One frame of one speaker's voice, decoded to mono 16-bit PCM and due to play now. */
struct FCrowdyCppVoicePlayout
{
	FString SpeakerKey;

	/** Samples a second in Pcm: 48000 for Opus, 8000 for mu-law, the rate a raw frame's length implies; 0 when Pcm is empty. */
	int32 SampleRate = 0;

	/** Empty when the frame could not be decoded; the spurt flags below still apply, so a speaker still stops talking. */
	TArray<int16> Pcm;

	/** The first frame after the speaker was silent. */
	bool bSpurtStart = false;

	/** The last frame before the speaker went silent. */
	bool bSpurtEnd = false;

	/** The frame was lost and Pcm is concealment (or silence where the codec cannot conceal). */
	bool bConcealed = false;
};

/**
 * Encodes microphone audio into voice packets in the format the CrowdyJS and CrowdyCPP SDKs share.
 *
 * Not thread safe; one per sending stream.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppVoiceEncoder
{
public:
	static constexpr int32 SampleRate = 48000;
	static constexpr int32 FrameMs = 20;
	static constexpr int32 FrameSamples = SampleRate / 1000 * FrameMs;

	/** The largest packet Encode writes: the voice header plus the largest Opus frame. */
	static constexpr int32 MaxPacketBytes = 1123;

	explicit FCrowdyCppVoiceEncoder(int32 BitrateBps = 24000, int32 PacketLossPercent = 10);
	~FCrowdyCppVoiceEncoder();

	/** False when the codec refused the settings; Encode then always fails. */
	bool IsValid() const;

	/**
	 * Encode exactly FrameSamples samples of 48 kHz mono PCM into one packet.
	 * bLast marks the end of a talk spurt. Answers false and writes nothing on failure.
	 */
	bool Encode(TArrayView<const int16> Pcm, bool bLast, TArray<uint8>& OutPacket);

	/** Advance the stream past Frames frames of silence that are not sent. */
	void Skip(int32 Frames = 1);

private:
	FCrowdyCppVoiceEncoder(const FCrowdyCppVoiceEncoder&) = delete;
	FCrowdyCppVoiceEncoder& operator=(const FCrowdyCppVoiceEncoder&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

/**
 * Puts received voice packets in order per speaker, decodes them and conceals lost frames.
 *
 * Speakers are keyed by any stable string the caller chooses. Not thread safe.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppVoiceReceiver
{
public:
	enum class EPushResult : uint8
	{
		Buffered,
		Late,
		Duplicate,
		Malformed
	};

	static constexpr int64 DefaultTargetDelayMs = 60;

	/** A new speaker beyond this many is refused as Malformed until one is forgotten. */
	static constexpr int32 MaxSpeakers = 16;

	explicit FCrowdyCppVoiceReceiver(int64 TargetDelayMs = DefaultTargetDelayMs);
	~FCrowdyCppVoiceReceiver();

	/** NowMs is the caller's monotonic clock in milliseconds; Poll must be given the same clock. */
	EPushResult Push(const FString& SpeakerKey, TArrayView<const uint8> Packet, int64 NowMs);

	/** Append every frame now due to OutFrames, in play order per speaker. */
	void Poll(int64 NowMs, TArray<FCrowdyCppVoicePlayout>& OutFrames);

	/** Drop a speaker's buffered frames and decoder. */
	void Forget(const FString& SpeakerKey);

	int32 NumSpeakers() const;

	int64 GetLatePackets() const;
	int64 GetDuplicatePackets() const;
	int64 GetMalformedPackets() const;
	int64 GetConcealedFrames() const;

private:
	FCrowdyCppVoiceReceiver(const FCrowdyCppVoiceReceiver&) = delete;
	FCrowdyCppVoiceReceiver& operator=(const FCrowdyCppVoiceReceiver&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
