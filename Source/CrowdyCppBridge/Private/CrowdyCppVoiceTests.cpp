#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppVoice.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/media/opus.hpp"
#include "crowdy/media/voice_frames.hpp"
THIRD_PARTY_INCLUDES_END

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace
{
	constexpr EAutomationTestFlags CrowdyCppVoiceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	constexpr int32 CrowdyCppVoiceTestRawSamples = 160;

	// One frame of a continuous 440 Hz tone, FrameIndex frames into it.
	TArray<int16> CrowdyCppVoiceTestSine(const int32 FrameIndex)
	{
		TArray<int16> Pcm;
		Pcm.SetNumUninitialized(FCrowdyCppVoiceEncoder::FrameSamples);
		for (int32 Index = 0; Index < Pcm.Num(); ++Index)
		{
			const double Time = static_cast<double>(FrameIndex * FCrowdyCppVoiceEncoder::FrameSamples + Index)
				/ FCrowdyCppVoiceEncoder::SampleRate;
			Pcm[Index] = static_cast<int16>(8000.0 * std::sin(2.0 * UE_DOUBLE_PI * 440.0 * Time));
		}
		return Pcm;
	}

	double CrowdyCppVoiceTestRms(const TArray<int16>& Pcm)
	{
		double Sum = 0.0;
		for (const int16 Sample : Pcm)
		{
			Sum += static_cast<double>(Sample) * Sample;
		}
		return Pcm.IsEmpty() ? 0.0 : std::sqrt(Sum / Pcm.Num());
	}

	TArray<uint8> CrowdyCppVoiceTestEncode(FCrowdyCppVoiceEncoder& Encoder, const int32 FrameIndex, const bool bLast)
	{
		TArray<uint8> Packet;
		Encoder.Encode(CrowdyCppVoiceTestSine(FrameIndex), bLast, Packet);
		return Packet;
	}

	TArray<uint8> CrowdyCppVoiceTestPacketize(crowdy::media::VoicePacketizer& Packetizer,
		const std::vector<std::uint8_t>& Frame, const bool bLast)
	{
		const std::vector<std::uint8_t> Packet = Packetizer.packetize(Frame, bLast);
		return TArray<uint8>(Packet.data(), static_cast<int32>(Packet.size()));
	}

	// A raw 16-bit little-endian frame of Samples samples, every one Value.
	TArray<uint8> CrowdyCppVoiceTestRaw(crowdy::media::VoicePacketizer& Packetizer, const int16 Value, const bool bLast,
		const int32 Samples = CrowdyCppVoiceTestRawSamples)
	{
		std::vector<std::uint8_t> Frame;
		for (int32 Index = 0; Index < Samples; ++Index)
		{
			Frame.push_back(static_cast<std::uint8_t>(Value & 0xFF));
			Frame.push_back(static_cast<std::uint8_t>((Value >> 8) & 0xFF));
		}
		return CrowdyCppVoiceTestPacketize(Packetizer, Frame, bLast);
	}

	crowdy::media::VoicePacketizer CrowdyCppVoiceTestPacketizer(const crowdy::media::VoiceCodec Codec,
		const std::uint8_t FrameMs = 20)
	{
		return crowdy::media::VoicePacketizer(
			crowdy::media::VoicePacketizerOptions{static_cast<std::uint8_t>(Codec), FrameMs});
	}

	// One packet carrying Frame as Opus in a slot of FrameMs, the last of its spurt.
	TArray<uint8> CrowdyCppVoiceTestOpusPacket(const std::vector<std::uint8_t>& Frame, const std::uint8_t FrameMs = 20)
	{
		crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Opus, FrameMs);
		return CrowdyCppVoiceTestPacketize(Packetizer, Frame, true);
	}

	const FCrowdyCppVoicePlayout* CrowdyCppVoiceTestFind(const TArray<FCrowdyCppVoicePlayout>& Frames, const TCHAR* Speaker)
	{
		return Frames.FindByPredicate([Speaker](const FCrowdyCppVoicePlayout& Frame) { return Frame.SpeakerKey == Speaker; });
	}

	// The codec frame of a packet, without its voice header.
	std::vector<std::uint8_t> CrowdyCppVoiceTestFrameOf(const TArray<uint8>& Packet)
	{
		return std::vector<std::uint8_t>(Packet.GetData() + crowdy::media::kVoiceHeaderBytes, Packet.GetData() + Packet.Num());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceRoundTripTest,
	"CrowdySDK.CrowdyCppBridge.Voice.RoundTrip", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceRoundTripTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	if (!TestTrue(TEXT("the encoder accepts its default settings"), Encoder.IsValid()))
	{
		return false;
	}

	FCrowdyCppVoiceReceiver Receiver;
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		const TArray<uint8> Packet = CrowdyCppVoiceTestEncode(Encoder, Frame, Frame == 2);
		TestTrue(TEXT("a packet carries a header and a frame"), Packet.Num() > 10);
		TestTrue(TEXT("a packet fits MaxPacketBytes"), Packet.Num() <= FCrowdyCppVoiceEncoder::MaxPacketBytes);
		TestEqual(TEXT("the packet names Opus"), static_cast<int32>(Packet.IsEmpty() ? 0 : Packet[1]), 1);
		TestTrue(TEXT("the packet is buffered"),
			Receiver.Push(TEXT("Speaker"), Packet, Frame) == FCrowdyCppVoiceReceiver::EPushResult::Buffered);
	}

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(150, Frames);
	if (!TestEqual(TEXT("every frame plays"), Frames.Num(), 3))
	{
		return false;
	}

	for (const FCrowdyCppVoicePlayout& Frame : Frames)
	{
		TestEqual(TEXT("a frame is 20 ms of 48 kHz"), Frame.Pcm.Num(), 960);
		TestEqual(TEXT("its rate is 48 kHz"), Frame.SampleRate, 48000);
		TestEqual(TEXT("its speaker"), Frame.SpeakerKey, FString(TEXT("Speaker")));
		TestFalse(TEXT("it is not concealment"), Frame.bConcealed);
	}
	TestTrue(TEXT("the tone comes back with energy"), CrowdyCppVoiceTestRms(Frames[2].Pcm) > 1000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceWrongLengthRefusedTest,
	"CrowdySDK.CrowdyCppBridge.Voice.WrongLengthRefused", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceWrongLengthRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	TArray<int16> Short = CrowdyCppVoiceTestSine(0);
	Short.Pop();
	TArray<int16> Long = CrowdyCppVoiceTestSine(0);
	Long.Add(0);

	TArray<uint8> Packet = {0xAB};
	TestFalse(TEXT("one sample short is refused"), Encoder.Encode(Short, false, Packet));
	TestFalse(TEXT("one sample long is refused"), Encoder.Encode(Long, false, Packet));
	TestFalse(TEXT("no samples is refused"), Encoder.Encode(TArrayView<const int16>(), false, Packet));
	TestTrue(TEXT("a refusal writes nothing"), Packet.Num() == 1 && Packet[0] == 0xAB);

	TArray<uint8> Accepted;
	TestTrue(TEXT("the right length still encodes"), Encoder.Encode(CrowdyCppVoiceTestSine(0), false, Accepted));
	TestEqual(TEXT("the refusals numbered nothing, so this is seq 0"),
		static_cast<int32>(Accepted.Num() > 3 ? Accepted[2] | (Accepted[3] << 8) : -1), 0);

	AddExpectedError(TEXT("the Opus encoder refused"), EAutomationExpectedErrorFlags::Contains, 1);
	FCrowdyCppVoiceEncoder Refused(1000);
	TestFalse(TEXT("a bitrate Opus refuses leaves the encoder invalid"), Refused.IsValid());
	TestFalse(TEXT("an invalid encoder encodes nothing"), Refused.Encode(CrowdyCppVoiceTestSine(0), false, Packet));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceOutOfOrderTest,
	"CrowdySDK.CrowdyCppBridge.Voice.OutOfOrder", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceOutOfOrderTest::RunTest(const FString& Parameters)
{
	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw);
	TArray<TArray<uint8>> Packets;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Packets.Add(CrowdyCppVoiceTestRaw(Packetizer, static_cast<int16>(100 * (Index + 1)), Index == 3));
	}

	FCrowdyCppVoiceReceiver Receiver;
	const int32 ArrivalOrder[] = {0, 3, 1, 2};
	for (int32 Arrival = 0; Arrival < 4; ++Arrival)
	{
		Receiver.Push(TEXT("Speaker"), Packets[ArrivalOrder[Arrival]], Arrival);
	}

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(150, Frames);
	if (!TestEqual(TEXT("every frame plays"), Frames.Num(), 4))
	{
		return false;
	}

	for (int32 Index = 0; Index < 4; ++Index)
	{
		TestEqual(TEXT("frames play in sequence order"), static_cast<int32>(Frames[Index].Pcm[0]), 100 * (Index + 1));
		TestEqual(TEXT("a raw frame keeps its sample count"), Frames[Index].Pcm.Num(), CrowdyCppVoiceTestRawSamples);
		TestEqual(TEXT("a raw frame's rate is the one its length implies"), Frames[Index].SampleRate, 8000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceMissingPacketConcealedTest,
	"CrowdySDK.CrowdyCppBridge.Voice.MissingPacketConcealed", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceMissingPacketConcealedTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	const TArray<uint8> First = CrowdyCppVoiceTestEncode(Encoder, 0, false);
	CrowdyCppVoiceTestEncode(Encoder, 1, false);
	const TArray<uint8> Third = CrowdyCppVoiceTestEncode(Encoder, 2, true);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Speaker"), First, 0);
	Receiver.Push(TEXT("Speaker"), Third, 1);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(150, Frames);
	if (!TestEqual(TEXT("two frames and one gap play"), Frames.Num(), 3))
	{
		return false;
	}

	TestFalse(TEXT("the first frame is real"), Frames[0].bConcealed);
	TestTrue(TEXT("the missing frame is concealed"), Frames[1].bConcealed);
	TestEqual(TEXT("concealment fills a whole frame"), Frames[1].Pcm.Num(), 960);
	TestEqual(TEXT("concealment plays at 48 kHz"), Frames[1].SampleRate, 48000);
	TestFalse(TEXT("the last frame is real"), Frames[2].bConcealed);
	TestEqual(TEXT("one frame was concealed"), Receiver.GetConcealedFrames(), int64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceMulawTest,
	"CrowdySDK.CrowdyCppBridge.Voice.Mulaw", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceMulawTest::RunTest(const FString& Parameters)
{
	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Mulaw);
	std::vector<std::uint8_t> Frame(160, 0xFF);
	Frame[1] = 0x80;
	Frame[2] = 0x00;
	const TArray<uint8> First = CrowdyCppVoiceTestPacketize(Packetizer, Frame, false);
	CrowdyCppVoiceTestPacketize(Packetizer, Frame, false);
	const TArray<uint8> Third = CrowdyCppVoiceTestPacketize(Packetizer, Frame, true);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Speaker"), First, 0);
	Receiver.Push(TEXT("Speaker"), Third, 1);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(150, Frames);
	if (!TestEqual(TEXT("two frames and one gap play"), Frames.Num(), 3))
	{
		return false;
	}

	TestEqual(TEXT("mu-law plays at 8 kHz"), Frames[0].SampleRate, 8000);
	TestEqual(TEXT("one sample per byte"), Frames[0].Pcm.Num(), 160);
	TestEqual(TEXT("0xFF expands to zero"), static_cast<int32>(Frames[0].Pcm[0]), 0);
	TestEqual(TEXT("0x80 expands to the positive peak"), static_cast<int32>(Frames[0].Pcm[1]), 32124);
	TestEqual(TEXT("0x00 expands to the negative peak"), static_cast<int32>(Frames[0].Pcm[2]), -32124);

	TestTrue(TEXT("the missing frame is concealed"), Frames[1].bConcealed);
	TestEqual(TEXT("the gap is one frame long"), Frames[1].Pcm.Num(), 160);
	TestEqual(TEXT("the gap is silence"), CrowdyCppVoiceTestRms(Frames[1].Pcm), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceRefusalsCountedTest,
	"CrowdySDK.CrowdyCppBridge.Voice.RefusalsCounted", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceRefusalsCountedTest::RunTest(const FString& Parameters)
{
	using EPushResult = FCrowdyCppVoiceReceiver::EPushResult;

	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw);
	const TArray<uint8> First = CrowdyCppVoiceTestRaw(Packetizer, 1, false);

	FCrowdyCppVoiceReceiver Receiver;
	TestTrue(TEXT("the first packet is buffered"), Receiver.Push(TEXT("Speaker"), First, 0) == EPushResult::Buffered);
	TestTrue(TEXT("the same packet again is a duplicate"),
		Receiver.Push(TEXT("Speaker"), First, 1) == EPushResult::Duplicate);
	TestEqual(TEXT("the duplicate is counted"), Receiver.GetDuplicatePackets(), int64(1));

	const TArray<uint8> Short = {1, 2, 3};
	TArray<uint8> OtherVersion = First;
	OtherVersion[0] = 2;
	TestTrue(TEXT("a packet shorter than a header is malformed"),
		Receiver.Push(TEXT("Speaker"), Short, 2) == EPushResult::Malformed);
	TestTrue(TEXT("another header version is malformed"),
		Receiver.Push(TEXT("Speaker"), OtherVersion, 3) == EPushResult::Malformed);
	TestEqual(TEXT("both are counted"), Receiver.GetMalformedPackets(), int64(2));

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(60, Frames);
	TestEqual(TEXT("the first frame plays"), Frames.Num(), 1);
	TestTrue(TEXT("a packet whose slot has played is late"),
		Receiver.Push(TEXT("Speaker"), First, 61) == EPushResult::Late);
	TestEqual(TEXT("the late packet is counted"), Receiver.GetLatePackets(), int64(1));

	crowdy::media::VoicePacketizer Unknown(crowdy::media::VoicePacketizerOptions{9, 20});
	crowdy::media::VoicePacketizer Odd = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw);
	FCrowdyCppVoiceReceiver Undecodable;
	Undecodable.Push(TEXT("Unknown"), CrowdyCppVoiceTestPacketize(Unknown, {1, 2, 3, 4}, true), 0);
	Undecodable.Push(TEXT("Odd"), CrowdyCppVoiceTestPacketize(Odd, {1, 2, 3}, true), 0);

	TArray<FCrowdyCppVoicePlayout> Empty;
	Undecodable.Poll(100, Empty);
	if (!TestEqual(TEXT("frames that cannot be decoded still play"), Empty.Num(), 2))
	{
		return false;
	}
	for (const FCrowdyCppVoicePlayout& Frame : Empty)
	{
		TestTrue(TEXT("an undecodable frame plays empty"), Frame.Pcm.IsEmpty() && Frame.SampleRate == 0);
		TestTrue(TEXT("and still ends its spurt"), Frame.bSpurtEnd && !Frame.bConcealed);
	}
	TestEqual(TEXT("an unknown codec and an odd raw length are counted malformed"),
		Undecodable.GetMalformedPackets(), int64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceSpurtFlagsTest,
	"CrowdySDK.CrowdyCppBridge.Voice.SpurtFlags", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceSpurtFlagsTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	const TArray<uint8> Opening = CrowdyCppVoiceTestEncode(Encoder, 0, false);
	const TArray<uint8> Closing = CrowdyCppVoiceTestEncode(Encoder, 1, true);
	Encoder.Skip(5);
	const TArray<uint8> Reopening = CrowdyCppVoiceTestEncode(Encoder, 2, false);
	if (!TestTrue(TEXT("every frame encodes"), Opening.Num() > 10 && Closing.Num() > 10 && Reopening.Num() > 10))
	{
		return false;
	}

	TestEqual(TEXT("the first packet starts a spurt"), static_cast<int32>(Opening[9]), 1);
	TestEqual(TEXT("bLast ends the spurt"), static_cast<int32>(Closing[9]), 2);
	TestEqual(TEXT("the packet after silence starts a spurt"), static_cast<int32>(Reopening[9]), 1);
	const int64 Timestamp = Reopening[4] | (Reopening[5] << 8) | (Reopening[6] << 16) | (static_cast<int64>(Reopening[7]) << 24);
	TestEqual(TEXT("Skip moved the clock past the unsent frames"), Timestamp, int64(7 * 960));
	TestEqual(TEXT("Skip did not number a packet"), static_cast<int32>(Reopening[2] | (Reopening[3] << 8)), 2);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Speaker"), Opening, 0);
	Receiver.Push(TEXT("Speaker"), Closing, 1);
	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(100, Frames);
	if (!TestEqual(TEXT("the spurt plays"), Frames.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the first frame starts the spurt"), Frames[0].bSpurtStart && !Frames[0].bSpurtEnd);
	TestTrue(TEXT("the last frame ends it"), Frames[1].bSpurtEnd && !Frames[1].bSpurtStart);

	Receiver.Push(TEXT("Speaker"), Reopening, 120);
	TArray<FCrowdyCppVoicePlayout> Next;
	Receiver.Poll(190, Next);
	if (!TestEqual(TEXT("the next spurt plays"), Next.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("it starts a spurt"), Next[0].bSpurtStart);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceForgetTest,
	"CrowdySDK.CrowdyCppBridge.Voice.Forget", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceForgetTest::RunTest(const FString& Parameters)
{
	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw);
	const TArray<uint8> Packet = CrowdyCppVoiceTestRaw(Packetizer, 1, true);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Leaving"), Packet, 0);
	Receiver.Push(TEXT("Staying"), Packet, 0);
	TestEqual(TEXT("two speakers"), Receiver.NumSpeakers(), 2);

	Receiver.Forget(TEXT("Leaving"));
	TestEqual(TEXT("one speaker is left"), Receiver.NumSpeakers(), 1);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(100, Frames);
	if (!TestEqual(TEXT("only the remaining speaker plays"), Frames.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("it is the one that stayed"), Frames[0].SpeakerKey, FString(TEXT("Staying")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceMaxSpeakersTest,
	"CrowdySDK.CrowdyCppBridge.Voice.MaxSpeakers", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceMaxSpeakersTest::RunTest(const FString& Parameters)
{
	using EPushResult = FCrowdyCppVoiceReceiver::EPushResult;

	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw);
	const TArray<uint8> First = CrowdyCppVoiceTestRaw(Packetizer, 1, false);
	const TArray<uint8> Second = CrowdyCppVoiceTestRaw(Packetizer, 2, false);

	FCrowdyCppVoiceReceiver Receiver;
	int32 Buffered = 0;
	for (int32 Index = 0; Index < FCrowdyCppVoiceReceiver::MaxSpeakers; ++Index)
	{
		Buffered += Receiver.Push(FString::Printf(TEXT("Speaker%d"), Index), First, 0) == EPushResult::Buffered ? 1 : 0;
	}
	TestEqual(TEXT("every speaker up to the limit is buffered"), Buffered, FCrowdyCppVoiceReceiver::MaxSpeakers);

	TestTrue(TEXT("a speaker beyond the limit is refused"),
		Receiver.Push(TEXT("Extra"), First, 1) == EPushResult::Malformed);
	TestEqual(TEXT("the refusal is counted malformed"), Receiver.GetMalformedPackets(), int64(1));
	TestEqual(TEXT("the refused speaker is not kept"), Receiver.NumSpeakers(), FCrowdyCppVoiceReceiver::MaxSpeakers);
	TestTrue(TEXT("a known speaker is still heard"),
		Receiver.Push(TEXT("Speaker0"), Second, 1) == EPushResult::Buffered);

	Receiver.Forget(TEXT("Speaker1"));
	TestTrue(TEXT("forgetting one frees a place"), Receiver.Push(TEXT("Extra"), First, 2) == EPushResult::Buffered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceOpusLongerThanSlotRefusedTest,
	"CrowdySDK.CrowdyCppBridge.Voice.OpusLongerThanSlotRefused", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceOpusLongerThanSlotRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Honest"), CrowdyCppVoiceTestEncode(Encoder, 0, true), 0);
	// A 60 ms SILK frame with no payload, which decodes as 60 ms of concealment.
	Receiver.Push(TEXT("Long"), CrowdyCppVoiceTestOpusPacket({0x18}), 0);
	// Six empty 10 ms frames in one code-3 packet.
	Receiver.Push(TEXT("Padded"), CrowdyCppVoiceTestOpusPacket({0x03, 0x06}), 0);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(100, Frames);
	const FCrowdyCppVoicePlayout* Honest = CrowdyCppVoiceTestFind(Frames, TEXT("Honest"));
	const FCrowdyCppVoicePlayout* Long = CrowdyCppVoiceTestFind(Frames, TEXT("Long"));
	const FCrowdyCppVoicePlayout* Padded = CrowdyCppVoiceTestFind(Frames, TEXT("Padded"));
	if (!TestTrue(TEXT("every speaker plays a slot"), Honest && Long && Padded))
	{
		return false;
	}

	TestEqual(TEXT("a frame that fits its slot decodes"), Honest->Pcm.Num(), 960);
	TestTrue(TEXT("60 ms in a 20 ms slot is not decoded"), Long->Pcm.IsEmpty() && Long->SampleRate == 0);
	TestTrue(TEXT("60 ms of sub-frames in a 20 ms slot is not decoded"), Padded->Pcm.IsEmpty() && Padded->SampleRate == 0);
	TestTrue(TEXT("a refused frame still ends its spurt"), Long->bSpurtEnd && Padded->bSpurtEnd);
	TestEqual(TEXT("both refusals are counted malformed"), Receiver.GetMalformedPackets(), int64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceOpusFrameMsRefusedTest,
	"CrowdySDK.CrowdyCppBridge.Voice.OpusFrameMsRefused", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceOpusFrameMsRefusedTest::RunTest(const FString& Parameters)
{
	crowdy::Result<crowdy::media::OpusVoiceEncoder> Created =
		crowdy::media::OpusVoiceEncoder::create(crowdy::media::OpusVoiceEncoderOptions{10, 24000, 0});
	if (!TestTrue(TEXT("a 10 ms encoder is created"), Created.ok()))
	{
		return false;
	}

	const TArray<int16> Sine = CrowdyCppVoiceTestSine(0);
	const crowdy::Result<std::vector<std::uint8_t>> Frame =
		Created.value().encode(std::span<const std::int16_t>(Sine.GetData(), 480));
	if (!TestTrue(TEXT("a 10 ms frame encodes"), Frame.ok()))
	{
		return false;
	}

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Standard"), CrowdyCppVoiceTestOpusPacket(Frame.value(), 10), 0);
	Receiver.Push(TEXT("Nonstandard"), CrowdyCppVoiceTestOpusPacket(Frame.value(), 15), 0);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(100, Frames);
	const FCrowdyCppVoicePlayout* Standard = CrowdyCppVoiceTestFind(Frames, TEXT("Standard"));
	const FCrowdyCppVoicePlayout* Nonstandard = CrowdyCppVoiceTestFind(Frames, TEXT("Nonstandard"));
	if (!TestTrue(TEXT("both speakers play a slot"), Standard && Nonstandard))
	{
		return false;
	}

	TestEqual(TEXT("a 10 ms slot decodes"), Standard->Pcm.Num(), 480);
	TestTrue(TEXT("a 15 ms Opus slot is not decoded"), Nonstandard->Pcm.IsEmpty() && Nonstandard->SampleRate == 0);
	TestEqual(TEXT("the refusal is counted malformed"), Receiver.GetMalformedPackets(), int64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceRawRateRangeTest,
	"CrowdySDK.CrowdyCppBridge.Voice.RawRateRange", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceRawRateRangeTest::RunTest(const FString& Parameters)
{
	crowdy::media::VoicePacketizer Slow = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw, 20);
	crowdy::media::VoicePacketizer Low = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw, 20);
	crowdy::media::VoicePacketizer High = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw, 1);
	crowdy::media::VoicePacketizer Fast = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Raw, 1);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Slow"), CrowdyCppVoiceTestRaw(Slow, 1, true, 100), 0);
	Receiver.Push(TEXT("Low"), CrowdyCppVoiceTestRaw(Low, 1, true, 160), 0);
	Receiver.Push(TEXT("High"), CrowdyCppVoiceTestRaw(High, 1, true, 48), 0);
	Receiver.Push(TEXT("Fast"), CrowdyCppVoiceTestRaw(Fast, 1, true, 100), 0);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(100, Frames);
	const FCrowdyCppVoicePlayout* SlowFrame = CrowdyCppVoiceTestFind(Frames, TEXT("Slow"));
	const FCrowdyCppVoicePlayout* LowFrame = CrowdyCppVoiceTestFind(Frames, TEXT("Low"));
	const FCrowdyCppVoicePlayout* HighFrame = CrowdyCppVoiceTestFind(Frames, TEXT("High"));
	const FCrowdyCppVoicePlayout* FastFrame = CrowdyCppVoiceTestFind(Frames, TEXT("Fast"));
	if (!TestTrue(TEXT("every speaker plays a slot"), SlowFrame && LowFrame && HighFrame && FastFrame))
	{
		return false;
	}

	TestTrue(TEXT("5 kHz is below the range"), SlowFrame->Pcm.IsEmpty() && SlowFrame->SampleRate == 0);
	TestEqual(TEXT("8 kHz is the bottom of the range"), LowFrame->SampleRate, 8000);
	TestEqual(TEXT("48 kHz is the top of the range"), HighFrame->SampleRate, 48000);
	TestTrue(TEXT("100 kHz is above the range"), FastFrame->Pcm.IsEmpty() && FastFrame->SampleRate == 0);
	TestEqual(TEXT("both refusals are counted malformed"), Receiver.GetMalformedPackets(), int64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVoiceCorruptFrameKeepsSpurtFlagsTest,
	"CrowdySDK.CrowdyCppBridge.Voice.CorruptFrameKeepsSpurtFlags", CrowdyCppVoiceTestFlags)
bool FCrowdyCppVoiceCorruptFrameKeepsSpurtFlagsTest::RunTest(const FString& Parameters)
{
	FCrowdyCppVoiceEncoder Encoder;
	const TArray<uint8> Encoded = CrowdyCppVoiceTestEncode(Encoder, 0, false);
	if (!TestTrue(TEXT("a frame encodes"), Encoded.Num() > 10))
	{
		return false;
	}

	crowdy::media::VoicePacketizer Packetizer = CrowdyCppVoiceTestPacketizer(crowdy::media::VoiceCodec::Opus);
	const TArray<uint8> Opening = CrowdyCppVoiceTestPacketize(Packetizer, CrowdyCppVoiceTestFrameOf(Encoded), false);
	// Two 10 ms frames, fitting the slot, whose first length runs past the packet, so libopus refuses it.
	const TArray<uint8> Closing = CrowdyCppVoiceTestPacketize(Packetizer, {0x03, 0x82, 200, 0x00}, true);

	FCrowdyCppVoiceReceiver Receiver;
	Receiver.Push(TEXT("Speaker"), Opening, 0);
	Receiver.Push(TEXT("Speaker"), Closing, 1);

	TArray<FCrowdyCppVoicePlayout> Frames;
	Receiver.Poll(150, Frames);
	if (!TestEqual(TEXT("both slots play"), Frames.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("the opening frame decodes and starts the spurt"), Frames[0].Pcm.Num() == 960 && Frames[0].bSpurtStart);
	TestTrue(TEXT("the corrupt frame plays empty"), Frames[1].Pcm.IsEmpty() && Frames[1].SampleRate == 0);
	TestTrue(TEXT("the corrupt frame still ends the spurt"), Frames[1].bSpurtEnd);
	TestFalse(TEXT("the corrupt frame is not concealment"), Frames[1].bConcealed);
	TestEqual(TEXT("it is counted malformed"), Receiver.GetMalformedPackets(), int64(1));
	TestEqual(TEXT("nothing was concealed"), Receiver.GetConcealedFrames(), int64(0));
	return true;
}

#endif
