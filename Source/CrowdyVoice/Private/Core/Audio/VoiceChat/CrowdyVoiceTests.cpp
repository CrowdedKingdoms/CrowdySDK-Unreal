#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/Audio/VoiceChat/CrowdyVoiceMic.h"
#include "Core/Audio/VoiceChat/CrowdyVoicePlayback.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceSettings.h"
#include "CrowdyCppVoice.h"

#include <limits>

namespace
{
	constexpr EAutomationTestFlags CrowdyVoiceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FCrowdyVoiceTestDeviceCounts
	{
		int32 Opens = 0;
		int32 Closes = 0;
	};

	/** Opens at 48 kHz without hardware; the test feeds the sink itself. */
	class FCrowdyVoiceTestMicDevice final : public ICrowdyVoiceMicDevice
	{
	public:
		explicit FCrowdyVoiceTestMicDevice(FCrowdyVoiceTestDeviceCounts& InCounts)
			: Counts(InCounts)
		{
		}

		virtual ECrowdyVoiceMicOpen Open(const FCrowdyVoiceCaptureSinkRef& Sink) override
		{
			++Counts.Opens;
			return Sink->Configure(FCrowdyVoiceCaptureSink::OutputSampleRate)
				? ECrowdyVoiceMicOpen::Opened
				: ECrowdyVoiceMicOpen::Failed;
		}

		virtual void Close() override
		{
			++Counts.Closes;
		}

	private:
		FCrowdyVoiceTestDeviceCounts& Counts;
	};

	struct FCrowdyVoiceTestFrame
	{
		TArray<int16> Samples;
		bool bLast = false;
	};

	/** Records what the mic sends, and a one-letter trace: F a frame, L a last frame, S a skipped frame. */
	class FCrowdyVoiceTestFrameSink final : public ICrowdyVoiceFrameSink
	{
	public:
		virtual void SendFrame(const TConstArrayView<int16> Frame, const bool bLast) override
		{
			Frames.Add({ TArray<int16>(Frame.GetData(), Frame.Num()), bLast });
			Trace += bLast ? TEXT("L") : TEXT("F");
		}

		virtual void SkipFrames(const int32 Count) override
		{
			Skipped += Count;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Trace += TEXT("S");
			}
		}

		TArray<FCrowdyVoiceTestFrame> Frames;
		FString Trace;
		int32 Skipped = 0;
	};

	/** The Index-th sample of a run of distinct values loud enough to pass the silence gate. */
	int16 CrowdyVoiceTestRampValue(const int32 Index)
	{
		return static_cast<int16>(Index % 2000 - 1000);
	}

	void CrowdyVoiceTestWriteRamp(const FCrowdyVoiceMic& Mic, const int32 First, const int32 Num)
	{
		TArray<float> Samples;
		Samples.SetNumUninitialized(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Samples[Index] = CrowdyVoiceTestRampValue(First + Index) / 32767.0f;
		}
		Mic.GetSink()->Write(Samples.GetData(), Num, 1, FCrowdyVoiceCaptureSink::OutputSampleRate);
	}

	void CrowdyVoiceTestWriteSilence(const FCrowdyVoiceMic& Mic, const int32 Num)
	{
		TArray<float> Samples;
		Samples.SetNumZeroed(Num);
		Mic.GetSink()->Write(Samples.GetData(), Num, 1, FCrowdyVoiceCaptureSink::OutputSampleRate);
	}

	/** Records what playback asked of the engine, in order. */
	class FCrowdyVoiceTestOutput final : public ICrowdyVoiceSpeakerOutput
	{
	public:
		virtual void OpenSpeaker(const FString& SpeakerKey, const int32 SampleRate) override
		{
			Calls.Add(FString::Printf(TEXT("open %s %d"), *SpeakerKey, SampleRate));
		}

		virtual void PlaySpeaker(const FString& SpeakerKey, const TConstArrayView<int16> Pcm) override
		{
			Calls.Add(FString::Printf(TEXT("play %s %d"), *SpeakerKey, Pcm.Num()));
		}

		virtual void CloseSpeaker(const FString& SpeakerKey) override
		{
			Calls.Add(FString::Printf(TEXT("close %s"), *SpeakerKey));
		}

		virtual void SpeakerTalking(const FString& SpeakerKey, const bool bTalking) override
		{
			Calls.Add(FString::Printf(TEXT("talk %s %d"), *SpeakerKey, bTalking ? 1 : 0));
		}

		TArray<FString> Calls;
	};

	FCrowdyCppVoicePlayout CrowdyVoiceTestPlayout(const TCHAR* SpeakerKey, const bool bSpurtStart, const bool bSpurtEnd,
		const bool bConcealed, const int32 SampleRate = 48000)
	{
		FCrowdyCppVoicePlayout Playout;
		Playout.SpeakerKey = SpeakerKey;
		Playout.SampleRate = SampleRate;
		Playout.Pcm.Init(5, SampleRate / 50);
		Playout.bSpurtStart = bSpurtStart;
		Playout.bSpurtEnd = bSpurtEnd;
		Playout.bConcealed = bConcealed;
		return Playout;
	}

	TArray<uint8> CrowdyVoiceTestPacket(FAutomationTestBase& Test)
	{
		FCrowdyCppVoiceEncoder Encoder;
		TArray<int16> Silence;
		Silence.SetNumZeroed(FCrowdyCppVoiceEncoder::FrameSamples);
		TArray<uint8> Packet;
		Test.TestTrue(TEXT("a frame encodes"), Encoder.IsValid() && Encoder.Encode(Silence, false, Packet));
		return Packet;
	}

	/** Stands in for a map's voice chat when settings are applied to it. */
	struct FCrowdyVoiceTestVoiceChat
	{
		void SetStreamTimeoutThreshold(const float Seconds) { Calls.Add(FString::Printf(TEXT("timeout %.1f"), Seconds)); }
		void SetDistance(const int32 Chunks) { Calls.Add(FString::Printf(TEXT("distance %d"), Chunks)); }
		void SetOwnerEcho(const bool bEnable) { Calls.Add(FString::Printf(TEXT("echo %d"), bEnable ? 1 : 0)); }
		void PlayVoiceChat() { Calls.Add(TEXT("play")); }
		void MuteVoiceChat() { Calls.Add(TEXT("mute")); }
		void StartVoiceChat() { Calls.Add(TEXT("start")); }

		TArray<FString> Calls;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicStartTwiceTest,
	"CrowdySDK.CrowdyVoice.MicStartTwiceIsNoOp", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicStartTwiceTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	Mic.Start(0);
	TestEqual(TEXT("the first start opens the device"), Counts.Opens, 1);
	TestEqual(TEXT("an open device is starting until audio arrives"), Mic.GetState(), ECrowdyVoiceMicState::Starting);

	Mic.Start(10);
	TestEqual(TEXT("a second start while starting opens nothing"), Counts.Opens, 1);
	TestEqual(TEXT("and closes nothing"), Counts.Closes, 0);

	CrowdyVoiceTestWriteRamp(Mic, 0, 100);
	Mic.Pump(20);
	TestEqual(TEXT("the first audio means capturing"), Mic.GetState(), ECrowdyVoiceMicState::Capturing);

	Mic.Start(30);
	TestEqual(TEXT("a start while capturing opens nothing"), Counts.Opens, 1);
	TestEqual(TEXT("and leaves it capturing"), Mic.GetState(), ECrowdyVoiceMicState::Capturing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicStopWhenOffTest,
	"CrowdySDK.CrowdyVoice.MicStopWhenOffIsNoOp", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicStopWhenOffTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	Mic.Stop();

	TestTrue(TEXT("stop while off sends and skips nothing"), Out.Trace.IsEmpty());
	TestEqual(TEXT("stop while off touches no device"), Counts.Closes, 0);
	TestEqual(TEXT("stop while off stays off"), Mic.GetState(), ECrowdyVoiceMicState::Off);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicStopSendsLastTest,
	"CrowdySDK.CrowdyVoice.MicStopSendsLastFrame", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicStopSendsLastTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	Mic.Start(0);
	CrowdyVoiceTestWriteRamp(Mic, 0, 1000);
	Mic.Pump(20);
	if (!TestEqual(TEXT("one whole frame is sent while talking"), Out.Frames.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("a frame sent while talking is not the last"), Out.Frames[0].bLast);

	Mic.Stop();
	if (!TestEqual(TEXT("stop sends what is left as one more frame"), Out.Frames.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the frame stop sends is flagged last"), Out.Frames[1].bLast);
	TestEqual(TEXT("it is a whole frame"), Out.Frames[1].Samples.Num(), FCrowdyVoiceMic::FrameSamples);
	TestEqual(TEXT("it starts with the samples left over"), Out.Frames[1].Samples[0], CrowdyVoiceTestRampValue(960));
	TestEqual(TEXT("and is padded with silence"), Out.Frames[1].Samples[40], static_cast<int16>(0));
	TestEqual(TEXT("stop closes the device"), Counts.Closes, 1);
	TestEqual(TEXT("stop leaves the mic off"), Mic.GetState(), ECrowdyVoiceMicState::Off);

	// A spurt that ended on a whole frame still gets its end, as a frame of silence.
	Out.Frames.Reset();
	Mic.Start(100);
	CrowdyVoiceTestWriteRamp(Mic, 0, FCrowdyVoiceMic::FrameSamples);
	Mic.Pump(120);
	Mic.Stop();
	if (!TestEqual(TEXT("a whole frame and then the end"), Out.Frames.Num(), 2))
	{
		return false;
	}
	TestFalse(TEXT("the captured frame was sent as talking"), Out.Frames[0].bLast);
	TestTrue(TEXT("the end follows as its own frame"), Out.Frames[1].bLast);
	TestEqual(TEXT("which is silence"), Out.Frames[1].Samples[0], static_cast<int16>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicFramingTest,
	"CrowdySDK.CrowdyVoice.MicFramesAreExact", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicFramingTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	Mic.Start(0);
	CrowdyVoiceTestWriteRamp(Mic, 0, 2500);
	Mic.Pump(20);
	TestEqual(TEXT("2500 samples make two whole frames"), Out.Frames.Num(), 2);

	CrowdyVoiceTestWriteRamp(Mic, 2500, 380);
	Mic.Pump(40);
	if (!TestEqual(TEXT("the 580 kept back and 380 more make the third"), Out.Frames.Num(), 3))
	{
		return false;
	}

	for (int32 FrameIndex = 0; FrameIndex < Out.Frames.Num(); ++FrameIndex)
	{
		const TArray<int16>& Samples = Out.Frames[FrameIndex].Samples;
		TestEqual(TEXT("every frame is 960 samples"), Samples.Num(), FCrowdyVoiceMic::FrameSamples);
		TestEqual(TEXT("a frame starts where the last one ended"), Samples[0],
			CrowdyVoiceTestRampValue(FrameIndex * FCrowdyVoiceMic::FrameSamples));
		TestEqual(TEXT("and ends in order"), Samples.Last(),
			CrowdyVoiceTestRampValue((FrameIndex + 1) * FCrowdyVoiceMic::FrameSamples - 1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicSilenceGateTest,
	"CrowdySDK.CrowdyVoice.MicSilenceGate", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicSilenceGateTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	int64 NowMs = 0;
	Mic.Start(NowMs);
	auto PumpOne = [&Mic, &NowMs](const bool bVoiced)
	{
		if (bVoiced)
		{
			CrowdyVoiceTestWriteRamp(Mic, 0, FCrowdyVoiceMic::FrameSamples);
		}
		else
		{
			CrowdyVoiceTestWriteSilence(Mic, FCrowdyVoiceMic::FrameSamples);
		}
		NowMs += 20;
		Mic.Pump(NowMs);
	};

	FString Expected;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		PumpOne(false);
		Expected += TEXT("S");
	}
	for (int32 Index = 0; Index < 5; ++Index)
	{
		PumpOne(true);
		Expected += TEXT("F");
	}
	for (int32 Index = 0; Index < 20; ++Index)
	{
		PumpOne(false);
	}
	Expected += FString::ChrN(FCrowdyVoiceMic::SilenceHangFrames - 1, TEXT('F'));
	Expected += TEXT("L");
	Expected += FString::ChrN(20 - FCrowdyVoiceMic::SilenceHangFrames, TEXT('S'));
	PumpOne(true);
	Expected += TEXT("F");

	// Silence before speech is skipped, a spurt ends 15 silent frames in, and the voice coming back starts another.
	TestTrue(TEXT("the gate sends speech and its tail, and skips the rest"), Out.Trace.Equals(Expected, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceMicBacklogTest,
	"CrowdySDK.CrowdyVoice.MicBacklogIsSkipped", CrowdyVoiceTestFlags)
bool FCrowdyVoiceMicBacklogTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestDeviceCounts Counts;
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceMic Mic(MakeUnique<FCrowdyVoiceTestMicDevice>(Counts), Out);

	constexpr int32 QueuedFrames = 15;
	Mic.Start(0);
	CrowdyVoiceTestWriteRamp(Mic, 0, QueuedFrames * FCrowdyVoiceMic::FrameSamples);
	Mic.Pump(20);

	const int32 Discarded = QueuedFrames - FCrowdyVoiceMic::BacklogKeepFrames;
	TestEqual(TEXT("the oldest frames of a backlog are skipped"), Out.Skipped, Discarded);
	if (!TestEqual(TEXT("and only the newest are sent"), Out.Frames.Num(), FCrowdyVoiceMic::BacklogKeepFrames))
	{
		return false;
	}
	TestEqual(TEXT("the first frame sent is the first one kept"), Out.Frames[0].Samples[0],
		CrowdyVoiceTestRampValue(Discarded * FCrowdyVoiceMic::FrameSamples));
	TestTrue(TEXT("the skip comes before the frames sent"), Out.Trace.StartsWith(TEXT("S"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceQueuePolicyTest,
	"CrowdySDK.CrowdyVoice.PlaybackQueueIsBounded", CrowdyVoiceTestFlags)
bool FCrowdyVoiceQueuePolicyTest::RunTest(const FString& Parameters)
{
	constexpr int32 Rate = 48000;
	constexpr int32 FrameBytes = Rate / 50 * 2;

	TestTrue(TEXT("an empty queue takes a frame"), FCrowdyVoiceQueuePolicy::ShouldQueue(0, FrameBytes, Rate));
	TestTrue(TEXT("a frame that fills it to 120 ms is taken"), FCrowdyVoiceQueuePolicy::ShouldQueue(5 * FrameBytes, FrameBytes, Rate));
	TestFalse(TEXT("a frame past 120 ms is dropped"), FCrowdyVoiceQueuePolicy::ShouldQueue(5 * FrameBytes + 1, FrameBytes, Rate));
	TestFalse(TEXT("one frame is not enough to start"), FCrowdyVoiceQueuePolicy::ShouldStart(FrameBytes, Rate));
	TestTrue(TEXT("two frames start playback"), FCrowdyVoiceQueuePolicy::ShouldStart(2 * FrameBytes, Rate));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceStatsQueueTest,
	"CrowdySDK.CrowdyVoice.StatsCountPlayedAndDropped", CrowdyVoiceTestFlags)
bool FCrowdyVoiceStatsQueueTest::RunTest(const FString& Parameters)
{
	constexpr int32 Rate = 48000;
	constexpr int32 FrameBytes = Rate / 50 * 2;
	FCrowdyVoiceStats Stats;

	TestTrue(TEXT("a frame with room is queued"), FCrowdyVoiceQueuePolicy::Admit(0, FrameBytes, Rate, Stats));
	TestEqual(TEXT("and counted as played"), Stats.FramesPlayed, static_cast<int64>(1));
	TestEqual(TEXT("not as dropped"), Stats.FramesDroppedQueueFull, static_cast<int64>(0));

	TestFalse(TEXT("a frame past the bound is not queued"), FCrowdyVoiceQueuePolicy::Admit(6 * FrameBytes, FrameBytes, Rate, Stats));
	TestEqual(TEXT("and counted as dropped"), Stats.FramesDroppedQueueFull, static_cast<int64>(1));
	TestEqual(TEXT("not as played"), Stats.FramesPlayed, static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceFirstPlayoutTest,
	"CrowdySDK.CrowdyVoice.FirstPlayoutOpensAndPlays", CrowdyVoiceTestFlags)
bool FCrowdyVoiceFirstPlayoutTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);
	Playback.SetPlaying(true);

	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), true, false, false), 0);
	const TArray<FString> FirstFrame = { TEXT("talk A 1"), TEXT("open A 48000"), TEXT("play A 960") };
	TestEqual(TEXT("the first frame opens the stream and that same frame plays"), Output.Calls, FirstFrame);

	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, false), 20);
	TestEqual(TEXT("the next frame plays on the same stream"), Output.Calls.Last(), FString(TEXT("play A 960")));
	TestEqual(TEXT("without opening another"), Output.Calls.Num(), 4);

	Playback.SetPlaying(false);
	TestEqual(TEXT("mute closes the stream"), Output.Calls.Last(), FString(TEXT("close A")));
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, false), 40);
	TestEqual(TEXT("a muted frame is not played"), Output.Calls.Num(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceRateChangeTest,
	"CrowdySDK.CrowdyVoice.RateChangeReopensStream", CrowdyVoiceTestFlags)
bool FCrowdyVoiceRateChangeTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);
	Playback.SetPlaying(true);

	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), true, false, false, 48000), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, false, 16000), 20);

	const TArray<FString> Expected = { TEXT("talk A 1"), TEXT("open A 48000"), TEXT("play A 960"),
		TEXT("close A"), TEXT("open A 16000"), TEXT("play A 320") };
	TestEqual(TEXT("a frame at another rate reopens the stream at that rate"), Output.Calls, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceTalkingTest,
	"CrowdySDK.CrowdyVoice.TalkingFollowsSpurts", CrowdyVoiceTestFlags)
bool FCrowdyVoiceTalkingTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);

	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), true, false, false), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, false), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, true, false), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, true), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), true, false, false), 0);

	const TArray<FString> Expected = { TEXT("talk A 1"), TEXT("talk A 0"), TEXT("talk A 1") };
	TestEqual(TEXT("talking starts and ends with the spurt, and concealment starts nothing"), Output.Calls, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceTalkingHoldTest,
	"CrowdySDK.CrowdyVoice.TalkingEndsWhenQuiet", CrowdyVoiceTestFlags)
bool FCrowdyVoiceTalkingHoldTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);

	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), true, false, false), 0);
	Playback.ApplyPlayout(CrowdyVoiceTestPlayout(TEXT("A"), false, false, true), 100);
	Playback.ExpireSpeakers(150, 2000);
	TestEqual(TEXT("still talking within the hold"), Output.Calls.Num(), 1);

	Playback.ExpireSpeakers(FCrowdyVoicePlayback::TalkingHoldMs + 50, 2000);
	TestEqual(TEXT("a lost spurt end still stops talking once the hold passes"), Output.Calls.Last(), FString(TEXT("talk A 0")));
	TestEqual(TEXT("and the speaker is kept until the timeout"), Playback.NumSpeakers(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceTimeoutTest,
	"CrowdySDK.CrowdyVoice.TimeoutForgetsSpeaker", CrowdyVoiceTestFlags)
bool FCrowdyVoiceTimeoutTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = CrowdyVoiceTestPacket(*this);
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);
	Playback.SetPlaying(true);
	Playback.Push(TEXT("A"), Packet, 0);
	Playback.Update(150, 2000);
	TestEqual(TEXT("the speaker is heard"), Playback.NumSpeakers(), 1);
	TestEqual(TEXT("and buffered by the receiver"), Playback.GetReceiver().NumSpeakers(), 1);
	TestTrue(TEXT("its first frame opened a stream"), Output.Calls.Contains(FString(TEXT("open A 48000"))));
	TestTrue(TEXT("and started talking"), Output.Calls.Contains(FString(TEXT("talk A 1"))));

	Playback.Update(1000, 2000);
	TestEqual(TEXT("a speaker silent for less than the timeout is kept"), Playback.NumSpeakers(), 1);
	TestFalse(TEXT("and its stream stays open"), Output.Calls.Contains(FString(TEXT("close A"))));

	Playback.Update(2500, 2000);
	TestEqual(TEXT("a speaker silent past the timeout is forgotten"), Playback.NumSpeakers(), 0);
	TestEqual(TEXT("the receiver forgets it too"), Playback.GetReceiver().NumSpeakers(), 0);
	TestEqual(TEXT("its stream closes"), Output.Calls.Last(), FString(TEXT("close A")));

	const int32 LastTalk = Output.Calls.FindLastByPredicate([](const FString& Call) { return Call.StartsWith(TEXT("talk A")); });
	TestTrue(TEXT("it ends not talking"), LastTalk != INDEX_NONE && Output.Calls[LastTalk] == TEXT("talk A 0"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceRepeatedPacketTest,
	"CrowdySDK.CrowdyVoice.RepeatedPacketKeepsNoSpeakerAlive", CrowdyVoiceTestFlags)
bool FCrowdyVoiceRepeatedPacketTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = CrowdyVoiceTestPacket(*this);
	FCrowdyVoiceTestOutput Output;
	FCrowdyVoicePlayback Playback(Output);

	Playback.Push(TEXT("A"), Packet, 0);
	Playback.Push(TEXT("A"), Packet, 100);
	Playback.Update(2050, 2000);
	TestEqual(TEXT("a repeated packet does not count as hearing the speaker"), Playback.NumSpeakers(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceSettingsTest,
	"CrowdySDK.CrowdyVoice.SettingsCarryTheMicAcrossMaps", CrowdyVoiceTestFlags)
bool FCrowdyVoiceSettingsTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceSettings Settings;
	TestEqual(TEXT("voice reaches the farthest the server relays by default"), Settings.DistanceChunks, FCrowdyVoiceSettings::MaxDistanceChunks);
	Settings.StreamTimeoutSeconds = 3.0f;
	Settings.DistanceChunks = 1;
	Settings.bOwnerEcho = true;
	Settings.bPlaying = true;
	Settings.bMicWanted = true;

	FCrowdyVoiceTestVoiceChat NextMap;
	Settings.ApplyTo(NextMap);
	const TArray<FString> Wanted = { TEXT("timeout 3.0"), TEXT("distance 1"), TEXT("echo 1"), TEXT("play"), TEXT("start") };
	TestEqual(TEXT("a mic that was on is started on the next map, at the distance set"), NextMap.Calls, Wanted);

	Settings.bMicWanted = false;
	Settings.bPlaying = false;
	FCrowdyVoiceTestVoiceChat AfterStop;
	Settings.ApplyTo(AfterStop);
	const TArray<FString> Stopped = { TEXT("timeout 3.0"), TEXT("distance 1"), TEXT("echo 1"), TEXT("mute") };
	TestEqual(TEXT("a mic that was stopped stays closed"), AfterStop.Calls, Stopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceTimeoutClampTest,
	"CrowdySDK.CrowdyVoice.StreamTimeoutIsClamped", CrowdyVoiceTestFlags)
bool FCrowdyVoiceTimeoutClampTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an in-range timeout is kept"), FCrowdyVoiceSettings::ClampStreamTimeout(2.0f), 2.0f);
	TestEqual(TEXT("too short is raised to the minimum"), FCrowdyVoiceSettings::ClampStreamTimeout(0.1f),
		FCrowdyVoiceSettings::MinStreamTimeoutSeconds);
	TestEqual(TEXT("too long is lowered to the maximum"), FCrowdyVoiceSettings::ClampStreamTimeout(1.0e6f),
		FCrowdyVoiceSettings::MaxStreamTimeoutSeconds);
	TestEqual(TEXT("not a number reads as the minimum"), FCrowdyVoiceSettings::ClampStreamTimeout(std::numeric_limits<float>::quiet_NaN()),
		FCrowdyVoiceSettings::MinStreamTimeoutSeconds);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceDistanceClampTest,
	"CrowdySDK.CrowdyVoice.DistanceIsClamped", CrowdyVoiceTestFlags)
bool FCrowdyVoiceDistanceClampTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an in-range distance is kept"), FCrowdyVoiceSettings::ClampDistance(1), 1);
	TestEqual(TEXT("the sender's own chunk is a distance"), FCrowdyVoiceSettings::ClampDistance(0), 0);
	TestEqual(TEXT("a negative distance is raised to 0"), FCrowdyVoiceSettings::ClampDistance(-3), 0);
	TestEqual(TEXT("past the server's reach is lowered to it"), FCrowdyVoiceSettings::ClampDistance(9), FCrowdyVoiceSettings::MaxDistanceChunks);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceFeedFramesTest,
	"CrowdySDK.CrowdyVoice.FeedCutsFramesAcrossPushes", CrowdyVoiceTestFlags)
bool FCrowdyVoiceFeedFramesTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceFeed Feed(Out);

	TArray<int16> Samples;
	Samples.SetNumUninitialized(FCrowdyVoiceMic::FrameSamples * 2 + 100);
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		Samples[Index] = CrowdyVoiceTestRampValue(Index);
	}

	const TConstArrayView<int16> All(Samples);
	Feed.Push(All.Left(500));
	TestEqual(TEXT("less than a frame sends nothing"), Out.Trace, FString());

	Feed.Push(All.Mid(500, FCrowdyVoiceMic::FrameSamples * 2 - 400));
	TestEqual(TEXT("each completed frame is sent, the remainder kept"), Out.Trace, FString(TEXT("FF")));
	if (TestEqual(TEXT("two frames went out"), Out.Frames.Num(), 2))
	{
		TestEqual(TEXT("the second frame starts where the first ended"), Out.Frames[1].Samples[0],
			CrowdyVoiceTestRampValue(FCrowdyVoiceMic::FrameSamples));
	}
	TestTrue(TEXT("a voiced feed is mid-spurt"), Feed.IsInSpurt());

	Feed.End();
	TestEqual(TEXT("ending sends the remainder, padded, as the last frame"), Out.Trace, FString(TEXT("FFL")));
	if (TestEqual(TEXT("three frames went out"), Out.Frames.Num(), 3))
	{
		TestEqual(TEXT("the remainder's first sample is kept"), Out.Frames[2].Samples[0], CrowdyVoiceTestRampValue(FCrowdyVoiceMic::FrameSamples * 2));
		TestEqual(TEXT("and past it the frame is silence"), Out.Frames[2].Samples[200], static_cast<int16>(0));
	}
	TestFalse(TEXT("the spurt is over"), Feed.IsInSpurt());

	Feed.End();
	TestEqual(TEXT("ending outside a spurt sends nothing"), Out.Trace, FString(TEXT("FFL")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoiceFeedGateTest,
	"CrowdySDK.CrowdyVoice.FeedPassesTheSilenceGate", CrowdyVoiceTestFlags)
bool FCrowdyVoiceFeedGateTest::RunTest(const FString& Parameters)
{
	FCrowdyVoiceTestFrameSink Out;
	FCrowdyVoiceFeed Feed(Out);

	TArray<int16> Silence;
	Silence.SetNumZeroed(FCrowdyVoiceMic::FrameSamples * 2);
	Feed.Push(Silence);
	TestEqual(TEXT("silence is skipped, not sent"), Out.Trace, FString(TEXT("SS")));
	TestFalse(TEXT("and starts no spurt"), Feed.IsInSpurt());

	Feed.End();
	TestEqual(TEXT("ending a silent feed sends nothing"), Out.Trace, FString(TEXT("SS")));
	return true;
}

#endif
