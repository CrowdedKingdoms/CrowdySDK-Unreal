#include "CrowdyCppVideo.h"

#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/bytes.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/media/video_frames.hpp"
THIRD_PARTY_INCLUDES_END

#include <array>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace
{
	// The limits are published on the Unreal class so a caller need not include a crowdy:: header to read
	// one. A re-vendor that moved either would leave the two descriptions disagreeing about where a
	// fragment's body starts, so it breaks the build here instead.
	static_assert(FCrowdyCppVideoAssembler::FragmentHeaderBytes
			== static_cast<int32>(crowdy::media::kVideoFragmentHeaderBytes),
		"A video fragment header is no longer the size this facade publishes.");
	static_assert(FCrowdyCppVideoAssembler::MaxFragmentBodyBytes
			== static_cast<int32>(crowdy::media::kMaxVideoFragmentBodyBytes),
		"A video fragment body no longer holds the octets this facade publishes.");
	static_assert(FCrowdyCppVideoAssembler::MaxFragments
			== static_cast<int32>(crowdy::media::kMaxVideoFragments),
		"A video frame is no longer refused above the fragment count this facade publishes.");
	static_assert(FCrowdyCppVideoAssembler::DefaultFrameTimeoutMs == crowdy::media::kVideoFrameTimeoutMs,
		"An incomplete video frame is no longer abandoned after the interval this facade publishes.");

	/** Answers false for a run of any other length, which addresses no sender and must not be keyed on. */
	bool CrowdyCppVideoToActorUuid(const TArrayView<const uint8> Octets, crowdy::core::ActorUuid& Out)
	{
		if (Octets.Num() != static_cast<int32>(std::tuple_size<crowdy::core::ActorUuid>::value))
		{
			return false;
		}

		FMemory::Memcpy(Out.data(), Octets.GetData(), Out.size());
		return true;
	}

	crowdy::Bytes CrowdyCppVideoSpanFrom(const TArrayView<const uint8> Data)
	{
		return Data.IsEmpty()
			? crowdy::Bytes()
			: crowdy::Bytes(Data.GetData(), static_cast<std::size_t>(Data.Num()));
	}
}

struct FCrowdyCppVideoAssembler::FImpl
{
	explicit FImpl(const int64 TimeoutMs) : Assembler(TimeoutMs) {}

	crowdy::media::VideoFrameAssembler Assembler;
};

FCrowdyCppVideoAssembler::FCrowdyCppVideoAssembler(const int64 TimeoutMs)
	: Impl(MakeUnique<FImpl>(TimeoutMs))
{
}

FCrowdyCppVideoAssembler::~FCrowdyCppVideoAssembler() = default;

bool FCrowdyCppVideoAssembler::Ingest(const TArrayView<const uint8> SenderUuid,
	const TArrayView<const uint8> Fragment, const int64 NowMs, FCrowdyCppAssembledVideoFrame& OutFrame)
{
	crowdy::core::ActorUuid Uuid{};
	if (!CrowdyCppVideoToActorUuid(SenderUuid, Uuid))
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Video: a fragment arrived under a sender id of %d octets, not %d."),
			SenderUuid.Num(), static_cast<int32>(Uuid.size()));
		return false;
	}

	std::optional<crowdy::media::AssembledVideoFrame> Completed =
		Impl->Assembler.ingest(Uuid, CrowdyCppVideoSpanFrom(Fragment), NowMs);
	if (!Completed.has_value())
	{
		return false;
	}

	OutFrame.SenderUuid.SetNumUninitialized(static_cast<int32>(Uuid.size()));
	FMemory::Memcpy(OutFrame.SenderUuid.GetData(), Uuid.data(), Uuid.size());
	OutFrame.FrameId = static_cast<int32>(Completed->frameId);
	OutFrame.Codec = static_cast<uint8>(Completed->codec);
	OutFrame.CompletedAtMs = Completed->completedAtMs;

	OutFrame.Bytes.SetNumUninitialized(static_cast<int32>(Completed->bytes.size()));
	if (!Completed->bytes.empty())
	{
		FMemory::Memcpy(OutFrame.Bytes.GetData(), Completed->bytes.data(), Completed->bytes.size());
	}
	return true;
}

int32 FCrowdyCppVideoAssembler::Prune(const int64 NowMs)
{
	return static_cast<int32>(Impl->Assembler.prune(NowMs));
}

void FCrowdyCppVideoAssembler::Forget(const TArrayView<const uint8> SenderUuid)
{
	crowdy::core::ActorUuid Uuid{};
	if (!CrowdyCppVideoToActorUuid(SenderUuid, Uuid))
	{
		return;
	}

	Impl->Assembler.forget(Uuid);
}

int32 FCrowdyCppVideoAssembler::PendingSenders() const
{
	return static_cast<int32>(Impl->Assembler.pendingCount());
}

int64 FCrowdyCppVideoAssembler::GetDroppedFragments() const
{
	return static_cast<int64>(Impl->Assembler.dropped);
}

int64 FCrowdyCppVideoAssembler::GetAbandonedFrames() const
{
	return static_cast<int64>(Impl->Assembler.abandoned);
}

bool FCrowdyCppVideoAssembler::FragmentFrame(const TArrayView<const uint8> Frame, const int32 FrameId,
	const uint8 Codec, TArray<TArray<uint8>>& OutFragments)
{
	OutFragments.Reset();

	if (Codec != static_cast<uint8>(crowdy::media::VideoCodec::Jpeg)
		&& Codec != static_cast<uint8>(crowdy::media::VideoCodec::WebP))
	{
		return false;
	}

	// Refused rather than wrapped: the counter is the receiver's only way to tell one frame from the next,
	// and a silently truncated id would collide with a frame the far end is still assembling.
	if (FrameId < 0 || FrameId > static_cast<int32>(MAX_uint16))
	{
		return false;
	}

	const std::vector<std::vector<std::uint8_t>> Fragments = crowdy::media::fragmentFrame(
		CrowdyCppVideoSpanFrom(Frame), static_cast<std::uint16_t>(FrameId),
		static_cast<crowdy::media::VideoCodec>(Codec));
	if (Fragments.empty())
	{
		return false;
	}

	OutFragments.Reserve(static_cast<int32>(Fragments.size()));
	for (const std::vector<std::uint8_t>& Fragment : Fragments)
	{
		TArray<uint8>& Out = OutFragments.AddDefaulted_GetRef();
		Out.SetNumUninitialized(static_cast<int32>(Fragment.size()));
		FMemory::Memcpy(Out.GetData(), Fragment.data(), Fragment.size());
	}
	return true;
}
