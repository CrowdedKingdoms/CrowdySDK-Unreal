#include "Core/Audio/VoiceChat/CrowdyVoiceMic.h"

#include "AudioCaptureDeviceInterface.h"
#include "CrowdyVoiceLog.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Tasks/Task.h"

namespace
{
	// Rounded up to a power of two by the queue: about 340 ms of 48 kHz audio.
	constexpr uint32 CrowdyVoiceMicQueueSamples = 16384;
	constexpr uint32 CrowdyVoiceMicDeviceFramesDesired = 1024;
	constexpr int32 CrowdyVoiceMicMinSampleRate = 8000;
	constexpr int32 CrowdyVoiceMicMaxSampleRate = 384000;

	// Capture streams still closing in the background; the module waits for them before it unloads.
	std::atomic<int32> CrowdyVoiceMicClosesInFlight{0};

	int16 CrowdyVoiceMicToPcm16(const float Sample)
	{
		return static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Sample, -1.0f, 1.0f) * 32767.0f));
	}

	void CrowdyVoiceMicDownmix(const float* Interleaved, const int32 NumFrames, const int32 NumChannels, float* OutMono)
	{
		const float Scale = 1.0f / static_cast<float>(NumChannels);
		for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			const float* Samples = Interleaved + FrameIndex * NumChannels;
			float Sum = 0.0f;
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				Sum += Samples[Channel];
			}
			OutMono[FrameIndex] = Sum * Scale;
		}
	}

	bool CrowdyVoiceMicIsSilent(const TArray<int16>& Samples)
	{
		int64 SumOfSquares = 0;
		for (const int16 Sample : Samples)
		{
			SumOfSquares += static_cast<int64>(Sample) * Sample;
		}
		const int64 Threshold = static_cast<int64>(FCrowdyVoiceMic::SilenceRms) * FCrowdyVoiceMic::SilenceRms;
		return SumOfSquares < Threshold * Samples.Num();
	}

	Audio::IAudioCaptureFactory* CrowdyVoiceMicFindFactory()
	{
		if (!FApp::CanEverRenderAudio())
		{
			return nullptr;
		}

		IModularFeatures& Features = IModularFeatures::Get();
		Features.LockModularFeatureList();
		const TArray<Audio::IAudioCaptureFactory*> Factories =
			Features.GetModularFeatureImplementations<Audio::IAudioCaptureFactory>(
				Audio::IAudioCaptureFactory::GetModularFeatureName());
		Features.UnlockModularFeatureList();
		return Factories.IsEmpty() ? nullptr : Factories[0];
	}

	// Stopping a stream whose capture thread has not delivered yet can hang the engine's own shutdown of that
	// thread, so the first delivery is awaited, bounded by the starting timeout.
	void CrowdyVoiceMicRetireStream(Audio::IAudioCaptureStream& Stream, const FCrowdyVoiceCaptureSink* Sink)
	{
		const double Deadline = FPlatformTime::Seconds() + FCrowdyVoiceMic::StartingTimeoutMs / 1000.0;
		while (Sink && !Sink->HasDelivered() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		Stream.StopStream();
		Stream.CloseStream();
	}

	class FCrowdyVoiceEngineMicDevice final : public ICrowdyVoiceMicDevice
	{
	public:
		virtual ~FCrowdyVoiceEngineMicDevice() override
		{
			Close();
		}

		virtual ECrowdyVoiceMicOpen Open(const FCrowdyVoiceCaptureSinkRef& Sink) override
		{
			Close();
			Audio::IAudioCaptureFactory* Factory = CrowdyVoiceMicFindFactory();
			if (!Factory)
			{
				return ECrowdyVoiceMicOpen::Unsupported;
			}

			Stream = Factory->CreateNewAudioCaptureStream();
			Audio::FCaptureDeviceInfo Info;
			if (!Stream || !Stream->GetCaptureDeviceInfo(Info, Audio::DefaultDeviceIndex))
			{
				Stream.Reset();
				return ECrowdyVoiceMicOpen::NoDevice;
			}

			// Configured before the stream opens, so no callback can race it.
			if (!Sink->Configure(Info.PreferredSampleRate))
			{
				Stream.Reset();
				return ECrowdyVoiceMicOpen::Failed;
			}

			// The callback holds its own reference: a stream still closing in the background writes only here.
			Audio::FOnAudioCaptureFunction OnCapture =
				[Sink](const void* Buffer, const int32 NumFrames, const int32 NumChannels, const int32 SampleRate,
					double, bool)
				{
					Sink->Write(static_cast<const float*>(Buffer), NumFrames, NumChannels, SampleRate);
				};

			StreamSink = Sink;
			Audio::FAudioCaptureDeviceParams Params;
			if (!Stream->OpenAudioCaptureStream(Params, MoveTemp(OnCapture), CrowdyVoiceMicDeviceFramesDesired)
				|| !Stream->StartStream())
			{
				Close();
				return ECrowdyVoiceMicOpen::Failed;
			}
			return ECrowdyVoiceMicOpen::Opened;
		}

		virtual void Close() override
		{
			if (!Stream)
			{
				StreamSink.Reset();
				return;
			}

			// Stopping a capture thread can take a while, so it happens off the game thread.
			TSharedPtr<Audio::IAudioCaptureStream, ESPMode::ThreadSafe> Retiring(Stream.Release());
			TSharedPtr<FCrowdyVoiceCaptureSink, ESPMode::ThreadSafe> RetiringSink = MoveTemp(StreamSink);
			CrowdyVoiceMicClosesInFlight.fetch_add(1);
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [Retiring, RetiringSink]() mutable
				{
					CrowdyVoiceMicRetireStream(*Retiring, RetiringSink.Get());

					// Destroyed here rather than with the task, so the count drops only once nothing is left.
					Retiring.Reset();
					RetiringSink.Reset();
					CrowdyVoiceMicClosesInFlight.fetch_sub(1);
				}, LowLevelTasks::ETaskPriority::BackgroundLow);
		}

	private:
		TUniquePtr<Audio::IAudioCaptureStream> Stream;
		TSharedPtr<FCrowdyVoiceCaptureSink, ESPMode::ThreadSafe> StreamSink;
	};
}

TUniquePtr<ICrowdyVoiceMicDevice> MakeCrowdyVoiceEngineMicDevice()
{
	return MakeUnique<FCrowdyVoiceEngineMicDevice>();
}

int32 WaitForCrowdyVoiceMicCloses(const double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (CrowdyVoiceMicClosesInFlight.load() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.01f);
	}
	return CrowdyVoiceMicClosesInFlight.load();
}

FCrowdyVoiceCaptureSink::FCrowdyVoiceCaptureSink()
	: Queue(CrowdyVoiceMicQueueSamples)
{
}

FCrowdyVoiceCaptureSink::~FCrowdyVoiceCaptureSink()
{
	if (Resampler)
	{
		src_delete(Resampler);
	}
}

bool FCrowdyVoiceCaptureSink::Configure(const int32 InSampleRate)
{
	Queue.Empty();
	if (Resampler)
	{
		src_delete(Resampler);
		Resampler = nullptr;
	}
	SampleRate = 0;

	if (InSampleRate < CrowdyVoiceMicMinSampleRate || InSampleRate > CrowdyVoiceMicMaxSampleRate)
	{
		return false;
	}

	MonoScratch.SetNumZeroed(MaxBlockFrames);
	ResampledScratch.Reset();
	if (InSampleRate != OutputSampleRate)
	{
		int Error = 0;
		Resampler = src_new(SRC_SINC_FASTEST, 1, &Error);
		if (!Resampler)
		{
			return false;
		}
		const double Ratio = static_cast<double>(OutputSampleRate) / InSampleRate;
		ResampledScratch.SetNumZeroed(FMath::CeilToInt32(MaxBlockFrames * Ratio) + 64);
	}

	SampleRate = InSampleRate;
	return true;
}

void FCrowdyVoiceCaptureSink::Write(const float* Interleaved, const int32 NumFrames, const int32 NumChannels,
	const int32 InSampleRate)
{
	bDelivered.store(true, std::memory_order_release);
	if (!Interleaved || SampleRate == 0 || InSampleRate != SampleRate || NumChannels <= 0)
	{
		return;
	}

	for (int32 Done = 0; Done < NumFrames; Done += MaxBlockFrames)
	{
		const int32 Block = FMath::Min(NumFrames - Done, MaxBlockFrames);
		CrowdyVoiceMicDownmix(Interleaved + Done * NumChannels, Block, NumChannels, MonoScratch.GetData());
		PushResampled(Block);
	}
}

void FCrowdyVoiceCaptureSink::DiscardOldest(const int32 NumSamples)
{
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		if (!Queue.Dequeue())
		{
			return;
		}
	}
}

void FCrowdyVoiceCaptureSink::PushResampled(const int32 NumMonoSamples)
{
	if (!Resampler)
	{
		PushPcm(MonoScratch.GetData(), NumMonoSamples);
		return;
	}

	SRC_DATA Data = {};
	Data.data_in = MonoScratch.GetData();
	Data.input_frames = NumMonoSamples;
	Data.data_out = ResampledScratch.GetData();
	Data.output_frames = ResampledScratch.Num();
	Data.src_ratio = static_cast<double>(OutputSampleRate) / SampleRate;
	Data.end_of_input = 0;
	if (src_process(Resampler, &Data) != 0)
	{
		return;
	}
	PushPcm(ResampledScratch.GetData(), static_cast<int32>(Data.output_frames_gen));
}

void FCrowdyVoiceCaptureSink::PushPcm(const float* Samples, const int32 NumSamples)
{
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		if (!Queue.Enqueue(CrowdyVoiceMicToPcm16(Samples[Index])))
		{
			DroppedSamples.fetch_add(NumSamples - Index, std::memory_order_relaxed);
			return;
		}
	}
}

FCrowdyVoiceMic::FCrowdyVoiceMic(TUniquePtr<ICrowdyVoiceMicDevice> InDevice, ICrowdyVoiceFrameSink& InOut)
	: Device(MoveTemp(InDevice))
	, Out(InOut)
{
	check(Device.IsValid());
	Frame.SetNumZeroed(FrameSamples);
}

FCrowdyVoiceMic::~FCrowdyVoiceMic()
{
	Device->Close();
}

void FCrowdyVoiceMic::Start(const int64 NowMs)
{
	if (State == ECrowdyVoiceMicState::Starting || State == ECrowdyVoiceMicState::Capturing)
	{
		return;
	}
	Open(NowMs);
}

void FCrowdyVoiceMic::Stop()
{
	if (State == ECrowdyVoiceMicState::Off)
	{
		return;
	}
	CloseAndEndSpurt();
	State = ECrowdyVoiceMicState::Off;
	UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat closed the microphone."));
}

void FCrowdyVoiceMic::Restart(const int64 NowMs)
{
	if (State == ECrowdyVoiceMicState::Off)
	{
		return;
	}
	UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat is reopening the microphone."));
	CloseAndEndSpurt();
	Open(NowMs);
}

void FCrowdyVoiceMic::RetryIfNoDevice(const int64 NowMs)
{
	if (State == ECrowdyVoiceMicState::NoDevice && NowMs - OpenedAtMs >= NoDeviceRetryMs)
	{
		Open(NowMs);
	}
}

void FCrowdyVoiceMic::Pump(const int64 NowMs)
{
	if (State != ECrowdyVoiceMicState::Starting && State != ECrowdyVoiceMicState::Capturing)
	{
		return;
	}

	const int32 Dropped = Sink->TakeDroppedSamples();
	UE_CLOG(Dropped > 0 && CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log,
		TEXT("Voice chat dropped %d microphone samples that were not collected in time."), Dropped);

	if (!Sink->IsEmpty())
	{
		LastAudioMs = NowMs;
	}

	if (State == ECrowdyVoiceMicState::Starting && Sink->IsEmpty())
	{
		if (NowMs - OpenedAtMs >= StartingTimeoutMs)
		{
			Fail(TEXT("Voice chat opened the microphone but no audio arrived. Check the microphone and that the OS microphone permission allows this app, then start voice chat again."));
		}
		return;
	}

	// A reopen that brings no audio ends in the starting timeout above, which bounds these restarts.
	if (Sink->IsEmpty() && NowMs - LastAudioMs >= StallTimeoutMs)
	{
		Restart(NowMs);
		return;
	}

	State = ECrowdyVoiceMicState::Capturing;
	TrimBacklog();
	while (PullFrame())
	{
		ProcessFrame();
	}
}

void FCrowdyVoiceMic::Fail(const TCHAR* Reason)
{
	Device->Close();
	Sink.Reset();
	FrameFill = 0;
	SilentFrames = 0;
	bInSpurt = false;
	State = ECrowdyVoiceMicState::Error;
	UE_LOG(LogCrowdyVoice, Warning, TEXT("%s"), Reason);
}

void FCrowdyVoiceMic::Open(const int64 NowMs)
{
	const ECrowdyVoiceMicState Previous = State;
	FrameFill = 0;
	SilentFrames = 0;
	bInSpurt = false;
	OpenedAtMs = NowMs;
	LastAudioMs = NowMs;

	Sink = MakeShared<FCrowdyVoiceCaptureSink, ESPMode::ThreadSafe>();
	const ECrowdyVoiceMicOpen Result = Device->Open(Sink.ToSharedRef());
	if (Result == ECrowdyVoiceMicOpen::Opened)
	{
		State = ECrowdyVoiceMicState::Starting;
		UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat opened the microphone."));
		return;
	}

	Device->Close();
	Sink.Reset();
	if (Result == ECrowdyVoiceMicOpen::NoDevice)
	{
		State = ECrowdyVoiceMicState::NoDevice;
		UE_CLOG(Previous != ECrowdyVoiceMicState::NoDevice, LogCrowdyVoice, Warning,
			TEXT("Voice chat found no microphone. Connect one or enable it in the OS sound settings; voice chat opens it as soon as it is available."));
		return;
	}

	State = ECrowdyVoiceMicState::Error;
	UE_LOG(LogCrowdyVoice, Warning, TEXT("%s"), Result == ECrowdyVoiceMicOpen::Unsupported
		? TEXT("Voice capture is not supported on this platform, or audio is disabled, so the microphone stays closed.")
		: TEXT("Voice chat could not open the microphone. Check that it is connected and not held by another app, and that the OS microphone permission allows this app, then start voice chat again."));
}

void FCrowdyVoiceMic::CloseAndEndSpurt()
{
	Device->Close();
	const bool bWasDelivering = State == ECrowdyVoiceMicState::Starting || State == ECrowdyVoiceMicState::Capturing;
	if (bWasDelivering && Sink)
	{
		// Whatever was captured goes out, then the spurt ends on a padded or silent frame.
		while (PullFrame())
		{
			ProcessFrame();
		}
		if (bInSpurt)
		{
			FMemory::Memzero(Frame.GetData() + FrameFill, (FrameSamples - FrameFill) * sizeof(int16));
			Out.SendFrame(Frame, true);
		}
	}
	Sink.Reset();
	FrameFill = 0;
	SilentFrames = 0;
	bInSpurt = false;
}

void FCrowdyVoiceMic::TrimBacklog()
{
	const int32 QueuedFrames = (FrameFill + Sink->Num()) / FrameSamples;
	if (QueuedFrames <= MaxBacklogFrames)
	{
		return;
	}

	const int32 Discarded = QueuedFrames - BacklogKeepFrames;
	Sink->DiscardOldest(Discarded * FrameSamples - FrameFill);
	FrameFill = 0;
	Out.SkipFrames(Discarded);
	UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat skipped %d late microphone frames."), Discarded);
}

void FCrowdyVoiceMic::ProcessFrame()
{
	const bool bSilent = CrowdyVoiceMicIsSilent(Frame);
	SilentFrames = bSilent ? FMath::Min(SilentFrames + 1, SilenceHangFrames) : 0;
	if (!bInSpurt && bSilent)
	{
		Out.SkipFrames(1);
		return;
	}

	const bool bEndsSpurt = SilentFrames >= SilenceHangFrames;
	Out.SendFrame(Frame, bEndsSpurt);
	bInSpurt = !bEndsSpurt;
}

bool FCrowdyVoiceMic::PullFrame()
{
	int16* Samples = Frame.GetData();
	while (FrameFill < FrameSamples && Sink->Dequeue(Samples[FrameFill]))
	{
		++FrameFill;
	}

	if (FrameFill < FrameSamples)
	{
		return false;
	}
	FrameFill = 0;
	return true;
}
