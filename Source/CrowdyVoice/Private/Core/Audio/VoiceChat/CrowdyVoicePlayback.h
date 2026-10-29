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
	/** Playout delay: absorbs the game thread delivering packets in bursts at 20 to 30 frames a second. */
	static constexpr int64 ReceiveDelayMs = 120;

	/** A talking speaker with no real frame for this long is reported as stopped, even without a spurt end. */
	static constexpr int64 TalkingHoldMs = 200;

	explicit FCrowdyVoicePlayback(ICrowdyVoiceSpeakerOutput& InOutput);
	~FCrowdyVoicePlayback();

	void Push(const FString& SpeakerKey, TConstArrayView<uint8> Packet, int64 NowMs);

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
	const FCrowdyCppVoiceReceiver& GetReceiver() const { return Receiver; }

private:
	FCrowdyVoicePlayback(const FCrowdyVoicePlayback&) = delete;
	FCrowdyVoicePlayback& operator=(const FCrowdyVoicePlayback&) = delete;

	struct FSpeaker
	{
		int64 LastHeardMs = 0;
		int64 LastVoicedMs = 0;

		/** The rate the stream was opened at; zero while closed. */
		int32 OpenRate = 0;

		bool bTalking = false;
	};

	void PlayFrame(const FCrowdyCppVoicePlayout& Playout, FSpeaker& Speaker);
	void Release(const FString& SpeakerKey, const FSpeaker& Speaker);

	ICrowdyVoiceSpeakerOutput& Output;
	FCrowdyCppVoiceReceiver Receiver;
	TMap<FString, FSpeaker> Speakers;
	TArray<FCrowdyCppVoicePlayout> Playouts;
	TArray<FString> Expired;
	bool bPlaying = false;
	bool bWarnedRefused = false;
};
