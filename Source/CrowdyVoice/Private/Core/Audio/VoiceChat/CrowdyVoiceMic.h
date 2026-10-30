#pragma once

#include "CoreMinimal.h"
#include "Containers/CircularQueue.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceMicState.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>
#include <samplerate.h>

/**
 * Turns microphone callbacks into 48 kHz mono 16-bit samples on a single-producer single-consumer queue.
 *
 * Write is the producer and runs on the audio thread: it never locks or allocates. Everything else is the
 * consumer side and runs on the game thread, Configure only before the device that writes here opens. One
 * sink serves one opened device, so a device still closing can never write into the next one's sink.
 */
class FCrowdyVoiceCaptureSink
{
public:
	static constexpr int32 OutputSampleRate = 48000;

	/** Device frames processed per pass, which bounds the scratch buffers Configure allocates. */
	static constexpr int32 MaxBlockFrames = 2048;

	FCrowdyVoiceCaptureSink();
	~FCrowdyVoiceCaptureSink();

	/** Prepares for a device delivering InSampleRate; false for a rate the resampler cannot take. */
	bool Configure(int32 InSampleRate);

	/** Interleaved float samples; a block at a rate other than the configured one is dropped. */
	void Write(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 InSampleRate);

	bool Dequeue(int16& OutSample) { return Queue.Dequeue(OutSample); }
	bool IsEmpty() const { return Queue.IsEmpty(); }
	int32 Num() const { return static_cast<int32>(Queue.Count()); }
	void DiscardOldest(int32 NumSamples);

	/** True once the device has called Write at all, whatever it delivered. */
	bool HasDelivered() const { return bDelivered.load(std::memory_order_acquire); }

	/** Samples dropped because the queue was full since the last call. */
	int32 TakeDroppedSamples() { return DroppedSamples.exchange(0, std::memory_order_relaxed); }

private:
	FCrowdyVoiceCaptureSink(const FCrowdyVoiceCaptureSink&) = delete;
	FCrowdyVoiceCaptureSink& operator=(const FCrowdyVoiceCaptureSink&) = delete;

	void PushResampled(int32 NumMonoSamples);
	void PushPcm(const float* Samples, int32 NumSamples);

	TCircularQueue<int16> Queue;
	SRC_STATE* Resampler = nullptr;
	int32 SampleRate = 0;
	TArray<float> MonoScratch;
	TArray<float> ResampledScratch;
	std::atomic<int32> DroppedSamples{0};
	std::atomic<bool> bDelivered{false};
};

using FCrowdyVoiceCaptureSinkRef = TSharedRef<FCrowdyVoiceCaptureSink, ESPMode::ThreadSafe>;

/** How opening the microphone went. */
enum class ECrowdyVoiceMicOpen : uint8
{
	Opened,
	NoDevice,
	Unsupported,
	Failed
};

/** The platform microphone as the mic state machine sees it, so the machine runs without audio hardware. */
class ICrowdyVoiceMicDevice
{
public:
	virtual ~ICrowdyVoiceMicDevice() = default;

	/** Opens the default microphone into Sink, kept alive for as long as the device may write to it. */
	virtual ECrowdyVoiceMicOpen Open(const FCrowdyVoiceCaptureSinkRef& Sink) = 0;

	/** Stops the device without blocking; it may finish in the background. Safe when nothing is open. */
	virtual void Close() = 0;
};

/** The engine's default capture device. */
TUniquePtr<ICrowdyVoiceMicDevice> MakeCrowdyVoiceEngineMicDevice();

/** Waits up to TimeoutSeconds for engine capture devices still closing in the background; answers how many remain. */
int32 WaitForCrowdyVoiceMicCloses(double TimeoutSeconds);

/** Where captured frames go: the voice subsystem encodes and sends them. */
class ICrowdyVoiceFrameSink
{
public:
	virtual ~ICrowdyVoiceFrameSink() = default;

	/** One frame of FrameSamples samples; bLast ends the talk spurt. */
	virtual void SendFrame(TConstArrayView<int16> Frame, bool bLast) = 0;

	/** Frames that pass unsent; the next frame sent starts a new talk spurt. */
	virtual void SkipFrames(int32 Count) = 0;
};

/**
 * The silence gate over whole 20 ms frames. Silence is not sent: a talk spurt ends with a frame flagged last once the
 * voice has stayed below the gate for SilenceHangFrames, and silent frames after it are skipped until the voice returns.
 */
class FCrowdyVoiceSpurtGate
{
public:
	/** Frames below the gate before a spurt ends: 300 ms. */
	static constexpr int32 SilenceHangFrames = 15;

	/** The gate as an RMS sample value: about -50 dBFS. */
	static constexpr int32 SilenceRms = 104;

	/** Sends or skips one whole frame. */
	void Process(TConstArrayView<int16> Frame, ICrowdyVoiceFrameSink& Out);

	/** Ends an open spurt with Frame, silencing its samples from Fill on; outside a spurt it only resets. */
	void End(TArrayView<int16> Frame, int32 Fill, ICrowdyVoiceFrameSink& Out);

	void Reset();
	bool IsInSpurt() const { return bInSpurt; }

private:
	int32 SilentFrames = 0;
	bool bInSpurt = false;
};

/** A voice source that is not the microphone: 48 kHz mono samples pushed from game code, cut and gated as the microphone's are. Game thread only. */
class FCrowdyVoiceFeed
{
public:
	explicit FCrowdyVoiceFeed(ICrowdyVoiceFrameSink& InOut);

	/** Sends every whole frame Samples completes; a remainder waits for the next push. */
	void Push(TConstArrayView<int16> Samples);

	/** Ends the talk spurt and drops a partial frame. */
	void End();

	bool IsInSpurt() const { return Gate.IsInSpurt(); }

private:
	ICrowdyVoiceFrameSink& Out;
	TArray<int16> Frame;
	int32 FrameFill = 0;
	FCrowdyVoiceSpurtGate Gate;
};

/** The microphone state machine and the cut into 20 ms frames, sent through the silence gate. Game thread only. */
class FCrowdyVoiceMic
{
public:
	static constexpr int32 FrameSamples = 960;

	/** How long an open microphone may deliver nothing before it is reported as an error. */
	static constexpr int64 StartingTimeoutMs = 3000;

	/** How long a capturing microphone may go quiet at the device before it is reopened. */
	static constexpr int64 StallTimeoutMs = 1000;

	static constexpr int64 NoDeviceRetryMs = 2000;

	static constexpr int32 SilenceHangFrames = FCrowdyVoiceSpurtGate::SilenceHangFrames;
	static constexpr int32 SilenceRms = FCrowdyVoiceSpurtGate::SilenceRms;

	/** Queued frames beyond this after a hitch are discarded down to BacklogKeepFrames. */
	static constexpr int32 MaxBacklogFrames = 10;
	static constexpr int32 BacklogKeepFrames = 2;

	FCrowdyVoiceMic(TUniquePtr<ICrowdyVoiceMicDevice> InDevice, ICrowdyVoiceFrameSink& InOut);
	~FCrowdyVoiceMic();

	ECrowdyVoiceMicState GetState() const { return State; }

	/** The sink of the open device; null while closed. */
	FCrowdyVoiceCaptureSink* GetSink() const { return Sink.Get(); }

	/** Opens the microphone; does nothing while it is already Starting or Capturing. */
	void Start(int64 NowMs);

	/** Closes the microphone and ends the talk spurt; does nothing while Off. */
	void Stop();

	/** Reopens the microphone after a device change; does nothing while Off. */
	void Restart(int64 NowMs);

	/** Tries again to open a microphone that was not found, at most every NoDeviceRetryMs. */
	void RetryIfNoDevice(int64 NowMs);

	/** Sends every whole frame captured so far through the silence gate. */
	void Pump(int64 NowMs);

	/** Closes the microphone and reports Reason as an error. */
	void Fail(const TCHAR* Reason);

private:
	FCrowdyVoiceMic(const FCrowdyVoiceMic&) = delete;
	FCrowdyVoiceMic& operator=(const FCrowdyVoiceMic&) = delete;

	void Open(int64 NowMs);
	void CloseAndEndSpurt();
	void TrimBacklog();
	bool PullFrame();

	TSharedPtr<FCrowdyVoiceCaptureSink, ESPMode::ThreadSafe> Sink;
	TUniquePtr<ICrowdyVoiceMicDevice> Device;
	ICrowdyVoiceFrameSink& Out;
	TArray<int16> Frame;
	int64 OpenedAtMs = 0;
	int64 LastAudioMs = 0;
	int32 FrameFill = 0;
	FCrowdyVoiceSpurtGate Gate;
	ECrowdyVoiceMicState State = ECrowdyVoiceMicState::Off;
};
