#pragma once

#include "CoreMinimal.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceStats.h"
#include "CrowdyCppVoice.h"

/** What playback needs from the engine; the voice subsystem answers it with audio components. */
class ICrowdyVoiceSpeakerOutput
{
public:
	virtual ~ICrowdyVoiceSpeakerOutput() = default;

	virtual void OpenSpeaker(const FString& SpeakerKey, int32 SampleRate) = 0;
	virtual void PlaySpeaker(const FString& SpeakerKey, TConstArrayView<int16> Pcm) = 0;
	virtual void CloseSpeaker(const FString& SpeakerKey) = 0;
	virtual void SpeakerTalking(const FString& SpeakerKey, bool bTalking) = 0;
};

/** How much decoded voice a speaker's playback queue holds: kept short, and started once it holds a little. */
struct FCrowdyVoiceQueuePolicy
{
	static constexpr int64 MaxQueuedMs = 120;
	static constexpr int64 PrebufferMs = 40;

	/** False when queuing FrameBytes more 16-bit mono would hold more than MaxQueuedMs. */
	static bool ShouldQueue(int32 QueuedBytes, int32 FrameBytes, int32 SampleRate);

	/** ShouldQueue, counting the frame in Stats as played or as dropped. */
	static bool Admit(int32 QueuedBytes, int32 FrameBytes, int32 SampleRate, FCrowdyVoiceStats& Stats);

	/** True when a stream that is not playing yet holds enough to start. */
	static bool ShouldStart(int32 QueuedBytes, int32 SampleRate);
};

/**
 * Received voice, game thread only.
 *
 * Orders and decodes packets per speaker, opens a speaker's stream on its first decoded frame and plays
 * that same frame, reports talking from the talk-spurt flags, and forgets a speaker that left or has been
 * silent for longer than the timeout. Output must not call back into this object.
 */
class FCrowdyVoicePlayback
{
public:
	/** Playout delay unless configured: absorbs the game thread delivering packets in bursts at 20 to 30 frames a second. */
	static constexpr int64 ReceiveDelayMs = 120;

	/** A talking speaker with no real frame for this long is reported as stopped, even without a spurt end. */
	static constexpr int64 TalkingHoldMs = 200;

	static constexpr int32 DefaultMaxSpeakers = 16;

	/** The most Max Speakers may be: one under the receiver's ceiling, so a newcomer's packet is buffered before the farthest gives way. */
	static constexpr int32 MaxSpeakersCeiling = FCrowdyCppVoiceReceiver::MaxSpeakers - 1;

	/** With every place taken, a new speaker displaces the farthest only from at most this fraction of its distance, so near-equals do not trade places. */
	static constexpr float DisplaceRatio = 0.9f;

	explicit FCrowdyVoicePlayback(ICrowdyVoiceSpeakerOutput& InOutput, int64 PlayoutDelayMs = ReceiveDelayMs, int32 InMaxSpeakers = DefaultMaxSpeakers);
	~FCrowdyVoicePlayback();

	/** DistanceSquared is the speaker's from the listener, which decides who is heard once MaxSpeakers are. */
	void Push(const FString& SpeakerKey, TConstArrayView<uint8> Packet, int64 NowMs, float DistanceSquared = 0.0f);

	/** Plays every frame now due, then ends quiet talkers and forgets speakers silent past TimeoutMs. */
	void Update(int64 NowMs, int64 TimeoutMs);

	void ApplyPlayout(const FCrowdyCppVoicePlayout& Playout, int64 NowMs);
	void ExpireSpeakers(int64 NowMs, int64 TimeoutMs);
	void Forget(const FString& SpeakerKey);
	void ForgetAll();

	/** While not playing, frames still drive talking but are not queued, and open streams are closed. */
	void SetPlaying(bool bInPlaying);
	bool IsPlaying() const { return bPlaying; }

	int32 NumSpeakers() const { return Speakers.Num(); }
	bool IsHeard(const FString& SpeakerKey) const { return Speakers.Contains(SpeakerKey); }
	int32 GetMaxSpeakers() const { return MaxSpeakers; }
	const FCrowdyCppVoiceReceiver& GetReceiver() const { return Receiver; }

	/** Packets from a speaker left unheard because every place was taken by someone nearer. */
	int64 GetPacketsOverCap() const { return PacketsOverCap; }

	/** Heard speakers that gave their place to a nearer one. */
	int64 GetSpeakersDisplaced() const { return SpeakersDisplaced; }

private:
	FCrowdyVoicePlayback(const FCrowdyVoicePlayback&) = delete;
	FCrowdyVoicePlayback& operator=(const FCrowdyVoicePlayback&) = delete;

	struct FSpeaker
	{
		int64 LastHeardMs = 0;
		int64 LastVoicedMs = 0;

		float DistanceSquared = 0.0f;

		/** The rate the stream was opened at; zero while closed. */
		int32 OpenRate = 0;

		bool bTalking = false;
	};

	void PlayFrame(const FCrowdyCppVoicePlayout& Playout, FSpeaker& Speaker);
	void Release(const FString& SpeakerKey, const FSpeaker& Speaker);
	bool FindFarthestToDisplace(float DistanceSquared, FString& OutFarthest) const;

	ICrowdyVoiceSpeakerOutput& Output;
	FCrowdyCppVoiceReceiver Receiver;
	TMap<FString, FSpeaker> Speakers;
	TArray<FCrowdyCppVoicePlayout> Playouts;
	TArray<FString> Expired;
	int64 PacketsOverCap = 0;
	int64 SpeakersDisplaced = 0;
	int32 MaxSpeakers = DefaultMaxSpeakers;
	bool bPlaying = false;
	bool bWarnedRefused = false;
};
