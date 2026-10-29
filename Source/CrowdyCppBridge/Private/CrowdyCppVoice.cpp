#include "CrowdyCppVoice.h"

#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/result.hpp"
#include "crowdy/media/opus.hpp"
#include "crowdy/media/voice_frames.hpp"
#if __has_include(<opus.h>)
#include <opus.h>
#else
#include <opus/opus.h>
#endif
THIRD_PARTY_INCLUDES_END

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	constexpr std::uint8_t CrowdyCppVoiceOpusCodec = static_cast<std::uint8_t>(crowdy::media::VoiceCodec::Opus);
	constexpr std::uint8_t CrowdyCppVoiceMulawCodec = static_cast<std::uint8_t>(crowdy::media::VoiceCodec::Mulaw);
	constexpr std::uint8_t CrowdyCppVoiceRawCodec = static_cast<std::uint8_t>(crowdy::media::VoiceCodec::Raw);
	constexpr int32 CrowdyCppVoiceMinRawRate = 8000;
	constexpr int32 CrowdyCppVoiceMaxRawRate = 48000;

	static_assert(FCrowdyCppVoiceEncoder::MaxPacketBytes
			== static_cast<int32>(crowdy::media::kVoiceHeaderBytes + crowdy::media::kMaxVoiceFrameBytes),
		"A voice packet no longer holds the octets this facade publishes.");
	static_assert(FCrowdyCppVoiceEncoder::SampleRate
			== static_cast<int32>(crowdy::media::voiceClockRate(CrowdyCppVoiceOpusCodec)),
		"Opus voice no longer runs at the sample rate this facade publishes.");
	static_assert(FCrowdyCppVoiceEncoder::FrameSamples
			== static_cast<int32>(crowdy::media::voiceSamplesPerFrame(CrowdyCppVoiceOpusCodec,
				static_cast<std::uint8_t>(FCrowdyCppVoiceEncoder::FrameMs))),
		"An Opus voice frame no longer holds the samples this facade publishes.");
	static_assert(FCrowdyCppVoiceReceiver::DefaultTargetDelayMs == crowdy::media::kVoiceTargetDelayMs,
		"Voice no longer plays out after the delay this facade publishes.");

	static_assert(static_cast<uint8>(FCrowdyCppVoiceReceiver::EPushResult::Buffered)
		== static_cast<uint8>(crowdy::media::VoicePushResult::Buffered), "Push results no longer line up.");
	static_assert(static_cast<uint8>(FCrowdyCppVoiceReceiver::EPushResult::Late)
		== static_cast<uint8>(crowdy::media::VoicePushResult::Late), "Push results no longer line up.");
	static_assert(static_cast<uint8>(FCrowdyCppVoiceReceiver::EPushResult::Duplicate)
		== static_cast<uint8>(crowdy::media::VoicePushResult::Duplicate), "Push results no longer line up.");
	static_assert(static_cast<uint8>(FCrowdyCppVoiceReceiver::EPushResult::Malformed)
		== static_cast<uint8>(crowdy::media::VoicePushResult::Malformed), "Push results no longer line up.");

	struct FCrowdyCppVoiceSpeaker
	{
		FString Key;
		std::optional<crowdy::media::OpusVoiceDecoder> Decoder;

		/** The rate of the last raw frame decoded, so a lost one is replaced by silence of the same length; 0 until then. */
		int32 RawSampleRate = 0;
	};

	std::span<const std::uint8_t> CrowdyCppVoiceBytesFrom(const TArrayView<const uint8> Data)
	{
		return std::span<const std::uint8_t>(Data.GetData(), static_cast<std::size_t>(Data.Num()));
	}

	void CrowdyCppVoiceKeyFrom(const FString& Key, std::string& Out)
	{
		const auto Utf8 = StringCast<UTF8CHAR>(*Key, Key.Len());
		Out.assign(reinterpret_cast<const char*>(Utf8.Get()), static_cast<std::size_t>(Utf8.Length()));
	}

	void CrowdyCppVoiceCopyPcm(const std::vector<std::int16_t>& Pcm, TArray<int16>& Out)
	{
		Out.SetNumUninitialized(static_cast<int32>(Pcm.size()));
		if (!Pcm.empty())
		{
			FMemory::Memcpy(Out.GetData(), Pcm.data(), Pcm.size() * sizeof(std::int16_t));
		}
	}

	bool CrowdyCppVoiceIsOpusFrameMs(const std::uint8_t FrameMs)
	{
		return FrameMs == 10 || FrameMs == 20 || FrameMs == 40 || FrameMs == 60;
	}

	// A frame holding more audio than its slot would make every listener decode faster than realtime.
	bool CrowdyCppVoiceOpusFitsSlot(const std::vector<std::uint8_t>& Frame, const std::uint8_t FrameMs)
	{
		if (Frame.empty())
		{
			return false;
		}

		const int Samples = opus_packet_get_nb_samples(Frame.data(), static_cast<opus_int32>(Frame.size()),
			FCrowdyCppVoiceEncoder::SampleRate);
		return Samples > 0 && Samples <= FCrowdyCppVoiceEncoder::SampleRate / 1000 * FrameMs;
	}

	/** G.711 mu-law expansion to 16-bit linear PCM. */
	int16 CrowdyCppVoiceMulawToLinear(const uint8 Encoded)
	{
		const int32 Inverted = static_cast<uint8>(~Encoded);
		const int32 Magnitude = ((((Inverted & 0x0F) << 3) + 0x84) << ((Inverted & 0x70) >> 4)) - 0x84;
		return static_cast<int16>((Inverted & 0x80) != 0 ? -Magnitude : Magnitude);
	}

	void CrowdyCppVoiceExpandMulaw(const std::vector<std::uint8_t>& Frame, TArray<int16>& Out)
	{
		Out.SetNumUninitialized(static_cast<int32>(Frame.size()));
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			Out[Index] = CrowdyCppVoiceMulawToLinear(Frame[Index]);
		}
	}

	void CrowdyCppVoiceReadRaw(const std::vector<std::uint8_t>& Frame, TArray<int16>& Out)
	{
		Out.SetNumUninitialized(static_cast<int32>(Frame.size() / 2));
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			Out[Index] = static_cast<int16>(Frame[2 * Index] | (Frame[2 * Index + 1] << 8));
		}
	}
}

struct FCrowdyCppVoiceEncoder::FImpl
{
	std::optional<crowdy::media::OpusVoiceEncoder> Encoder;
	crowdy::media::VoicePacketizer Packetizer{
		crowdy::media::VoicePacketizerOptions{CrowdyCppVoiceOpusCodec, static_cast<std::uint8_t>(FrameMs)}};
};

FCrowdyCppVoiceEncoder::FCrowdyCppVoiceEncoder(const int32 BitrateBps, const int32 PacketLossPercent)
	: Impl(MakeUnique<FImpl>())
{
	crowdy::media::OpusVoiceEncoderOptions Options;
	Options.frameMs = static_cast<std::uint8_t>(FrameMs);
	Options.bitrate = BitrateBps;
	Options.packetLossPercent = PacketLossPercent;

	crowdy::Result<crowdy::media::OpusVoiceEncoder> Created = crowdy::media::OpusVoiceEncoder::create(Options);
	if (!Created.ok())
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Voice: the Opus encoder refused %d bps at %d%% loss: %hs."),
			BitrateBps, PacketLossPercent, crowdy::errcName(Created.error()));
		return;
	}

	Impl->Encoder.emplace(std::move(Created.value()));
}

FCrowdyCppVoiceEncoder::~FCrowdyCppVoiceEncoder() = default;

bool FCrowdyCppVoiceEncoder::IsValid() const
{
	return Impl->Encoder.has_value();
}

bool FCrowdyCppVoiceEncoder::Encode(const TArrayView<const int16> Pcm, const bool bLast, TArray<uint8>& OutPacket)
{
	if (!Impl->Encoder.has_value() || Pcm.Num() != FrameSamples)
	{
		return false;
	}

	crowdy::Result<std::vector<std::uint8_t>> Frame =
		Impl->Encoder->encode(std::span<const std::int16_t>(Pcm.GetData(), static_cast<std::size_t>(Pcm.Num())));
	if (!Frame.ok())
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Voice: Opus could not encode a frame: %hs."), crowdy::errcName(Frame.error()));
		return false;
	}

	const std::vector<std::uint8_t> Packet = Impl->Packetizer.packetize(Frame.value(), bLast);
	if (Packet.empty())
	{
		return false;
	}

	OutPacket.SetNumUninitialized(static_cast<int32>(Packet.size()));
	FMemory::Memcpy(OutPacket.GetData(), Packet.data(), Packet.size());
	return true;
}

void FCrowdyCppVoiceEncoder::Skip(const int32 Frames)
{
	Impl->Packetizer.skip(static_cast<std::uint32_t>(FMath::Max(Frames, 0)));
}

struct FCrowdyCppVoiceReceiver::FImpl
{
	explicit FImpl(const int64 TargetDelayMs)
		: Buffer(crowdy::media::VoiceJitterBufferOptions{TargetDelayMs})
	{
	}

	bool DecodeOpus(FCrowdyCppVoiceSpeaker& Speaker, const crowdy::media::VoicePlayout& Slot, TArray<int16>& OutPcm);
	bool DecodeRaw(FCrowdyCppVoiceSpeaker& Speaker, const crowdy::media::VoicePlayout& Slot, TArray<int16>& OutPcm,
		int32& OutSampleRate);
	bool Decode(FCrowdyCppVoiceSpeaker& Speaker, const crowdy::media::VoicePlayout& Slot, TArray<int16>& OutPcm,
		int32& OutSampleRate);

	crowdy::media::VoiceJitterBuffer Buffer;
	std::unordered_map<std::string, FCrowdyCppVoiceSpeaker> Speakers;

	// Reused for every key conversion so a push does not allocate one.
	std::string KeyScratch;

	int64 Malformed = 0;
	int64 Concealed = 0;
};

bool FCrowdyCppVoiceReceiver::FImpl::DecodeOpus(FCrowdyCppVoiceSpeaker& Speaker,
	const crowdy::media::VoicePlayout& Slot, TArray<int16>& OutPcm)
{
	if (!CrowdyCppVoiceIsOpusFrameMs(Slot.frameMs))
	{
		Malformed += Slot.gap ? 0 : 1;
		return false;
	}

	if (!Slot.gap && !CrowdyCppVoiceOpusFitsSlot(Slot.frame, Slot.frameMs))
	{
		++Malformed;
		return false;
	}

	if (!Speaker.Decoder.has_value())
	{
		crowdy::Result<crowdy::media::OpusVoiceDecoder> Created = crowdy::media::OpusVoiceDecoder::create();
		if (!Created.ok())
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Voice: could not create an Opus decoder: %hs."),
				crowdy::errcName(Created.error()));
			return false;
		}
		Speaker.Decoder.emplace(std::move(Created.value()));
	}

	const crowdy::Result<std::vector<std::int16_t>> Pcm = Slot.gap
		? Speaker.Decoder->conceal(Slot.frameMs)
		: Speaker.Decoder->decode(Slot.frame);
	if (!Pcm.ok())
	{
		Malformed += Slot.gap ? 0 : 1;
		return false;
	}

	CrowdyCppVoiceCopyPcm(Pcm.value(), OutPcm);
	return true;
}

bool FCrowdyCppVoiceReceiver::FImpl::DecodeRaw(FCrowdyCppVoiceSpeaker& Speaker,
	const crowdy::media::VoicePlayout& Slot, TArray<int16>& OutPcm, int32& OutSampleRate)
{
	if (Slot.gap && Speaker.RawSampleRate == 0)
	{
		return false;
	}

	if (Slot.gap)
	{
		OutSampleRate = Speaker.RawSampleRate;
		OutPcm.SetNumZeroed(static_cast<int32>(static_cast<int64>(Speaker.RawSampleRate) * Slot.frameMs / 1000));
		return true;
	}

	// Raw frames carry no rate of their own, so it is the one their length and duration imply.
	const int32 SampleRate = static_cast<int32>(Slot.frame.size() / 2) * 1000 / Slot.frameMs;
	if (Slot.frame.size() % 2 != 0 || SampleRate < CrowdyCppVoiceMinRawRate || SampleRate > CrowdyCppVoiceMaxRawRate)
	{
		++Malformed;
		return false;
	}

	CrowdyCppVoiceReadRaw(Slot.frame, OutPcm);
	OutSampleRate = SampleRate;
	Speaker.RawSampleRate = SampleRate;
	return true;
}

bool FCrowdyCppVoiceReceiver::FImpl::Decode(FCrowdyCppVoiceSpeaker& Speaker, const crowdy::media::VoicePlayout& Slot,
	TArray<int16>& OutPcm, int32& OutSampleRate)
{
	if (Slot.codec == CrowdyCppVoiceOpusCodec)
	{
		OutSampleRate = FCrowdyCppVoiceEncoder::SampleRate;
		return DecodeOpus(Speaker, Slot, OutPcm);
	}

	if (Slot.codec == CrowdyCppVoiceRawCodec)
	{
		return DecodeRaw(Speaker, Slot, OutPcm, OutSampleRate);
	}

	if (Slot.codec != CrowdyCppVoiceMulawCodec)
	{
		Malformed += Slot.gap ? 0 : 1;
		return false;
	}

	if (!Slot.gap && Slot.frame.empty())
	{
		++Malformed;
		return false;
	}

	OutSampleRate = static_cast<int32>(crowdy::media::voiceClockRate(CrowdyCppVoiceMulawCodec));
	if (Slot.gap)
	{
		OutPcm.SetNumZeroed(static_cast<int32>(crowdy::media::voiceSamplesPerFrame(CrowdyCppVoiceMulawCodec, Slot.frameMs)));
		return true;
	}

	CrowdyCppVoiceExpandMulaw(Slot.frame, OutPcm);
	return true;
}

FCrowdyCppVoiceReceiver::FCrowdyCppVoiceReceiver(const int64 TargetDelayMs)
	: Impl(MakeUnique<FImpl>(TargetDelayMs))
{
}

FCrowdyCppVoiceReceiver::~FCrowdyCppVoiceReceiver() = default;

FCrowdyCppVoiceReceiver::EPushResult FCrowdyCppVoiceReceiver::Push(const FString& SpeakerKey,
	const TArrayView<const uint8> Packet, const int64 NowMs)
{
	CrowdyCppVoiceKeyFrom(SpeakerKey, Impl->KeyScratch);

	const bool bKnown = Impl->Speakers.contains(Impl->KeyScratch);
	if (!bKnown && Impl->Speakers.size() >= static_cast<std::size_t>(MaxSpeakers))
	{
		++Impl->Malformed;
		return EPushResult::Malformed;
	}

	const crowdy::media::VoicePushResult Result =
		Impl->Buffer.push(Impl->KeyScratch, CrowdyCppVoiceBytesFrom(Packet), NowMs);
	if (!bKnown && Result == crowdy::media::VoicePushResult::Buffered)
	{
		Impl->Speakers.try_emplace(Impl->KeyScratch).first->second.Key = SpeakerKey;
	}
	return static_cast<EPushResult>(Result);
}

void FCrowdyCppVoiceReceiver::Poll(const int64 NowMs, TArray<FCrowdyCppVoicePlayout>& OutFrames)
{
	const std::vector<crowdy::media::VoicePlayout> Slots = Impl->Buffer.poll(NowMs);
	for (const crowdy::media::VoicePlayout& Slot : Slots)
	{
		const auto Found = Impl->Speakers.find(Slot.key);
		if (Found == Impl->Speakers.end())
		{
			continue;
		}

		FCrowdyCppVoicePlayout& Out = OutFrames.AddDefaulted_GetRef();
		Out.SpeakerKey = Found->second.Key;
		Out.bSpurtStart = (Slot.flags & crowdy::media::VoiceFlag::kSpurtStart) != 0;
		Out.bSpurtEnd = (Slot.flags & crowdy::media::VoiceFlag::kSpurtEnd) != 0;

		// An undecodable frame still plays as an empty one, so its spurt flags reach the caller.
		if (!Impl->Decode(Found->second, Slot, Out.Pcm, Out.SampleRate))
		{
			Out.Pcm.Reset();
			Out.SampleRate = 0;
			continue;
		}

		Out.bConcealed = Slot.gap;
		Impl->Concealed += Slot.gap ? 1 : 0;
	}
}

void FCrowdyCppVoiceReceiver::Forget(const FString& SpeakerKey)
{
	CrowdyCppVoiceKeyFrom(SpeakerKey, Impl->KeyScratch);
	Impl->Buffer.forget(Impl->KeyScratch);
	Impl->Speakers.erase(Impl->KeyScratch);
}

int32 FCrowdyCppVoiceReceiver::NumSpeakers() const
{
	return static_cast<int32>(Impl->Speakers.size());
}

int64 FCrowdyCppVoiceReceiver::GetLatePackets() const
{
	return static_cast<int64>(Impl->Buffer.late);
}

int64 FCrowdyCppVoiceReceiver::GetDuplicatePackets() const
{
	return static_cast<int64>(Impl->Buffer.duplicates);
}

int64 FCrowdyCppVoiceReceiver::GetMalformedPackets() const
{
	return static_cast<int64>(Impl->Buffer.malformed) + Impl->Malformed;
}

int64 FCrowdyCppVoiceReceiver::GetConcealedFrames() const
{
	return Impl->Concealed;
}
