#pragma once

#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * One fragment of one webcam video frame from one sender.
 *
 * A datagram holds at most 1117 octets of encoded image, and even a small JPEG is several kilobytes, so a
 * frame crosses as up to sixteen of these and a single notification is never an image. Reassembly is per
 * sender and per frame id; FCrowdyCppVideoAssembler is the implementation of it, and a consumer that only
 * wants pictures should use that rather than reading these fields.
 *
 * FragmentView is the whole payload, header included, and is what the assembler ingests. BodyView is the
 * slice behind the header. Both borrow the frame's octets, so neither outlives the delivery.
 *
 * The sender, the chunk it was in, the server's timestamp and the sequence number are the spatial envelope
 * the base class already holds.
 */
struct FClientVideoNotification : ICrowdyMessage
{
	/** The sending actor, as the 32 octets read as a key. */
	FGuid GUID;

	/** Counts up per sender and wraps, so it orders frames from one sender and means nothing across two. */
	int32 FrameId = 0;

	ECrowdyVideoCodec Codec = ECrowdyVideoCodec::Unknown;

	/** The codec byte exactly as it arrived, so a value this build has no entry for is still readable. */
	uint8 RawCodec = 0;

	int32 FragmentIndex = 0;
	int32 FragmentCount = 0;

	/** The fragment as it arrived, header included. Feed this to the assembler. */
	TConstArrayView<uint8> FragmentView;

	/** This fragment's slice of the encoded frame, without the header. */
	TConstArrayView<uint8> BodyView;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Client Video Notification";
	}

	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	virtual TConstArrayView<uint8> GetPayloadBytes() const override
	{
		return FragmentView;
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FClientVideoNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		const TConstArrayView<uint8> Data = Frame.Body;

		// A fragment carrying no body at all is refused, not merely empty. The split never produces one: the
		// last fragment of a frame that divided exactly is a full one, not an extra empty one. So a header
		// with nothing behind it comes from a broken or hostile peer, and accepting it with a fragment count
		// of one would complete a zero octet frame straight into whatever decodes images.
		const int32 BodySize = Data.Num() - CrowdyVideoFragment::HeaderBytes;
		if (BodySize < 1 || BodySize > CrowdyVideoFragment::MaxBodyBytes)
		{
			return false;
		}

		// Refused rather than carried, because a fragment this SDK cannot place in a frame is one the
		// assembler would drop anyway, and delivering it would ask every consumer to repeat these checks.
		if (Data[CrowdyVideoFragment::VersionOffset] != CrowdyVideoFragment::Version)
		{
			return false;
		}

		RawCodec = Data[CrowdyVideoFragment::CodecOffset];
		Codec = CrowdyVideoCodecFromByte(RawCodec);
		if (Codec == ECrowdyVideoCodec::Unknown)
		{
			return false;
		}

		// Big endian, unlike every other field this SDK reads off the wire, because the fragment header is
		// a media contract shared with the browser SDK rather than part of the spatial frame.
		FrameId = (static_cast<int32>(Data[CrowdyVideoFragment::FrameIdOffset]) << 8)
			| static_cast<int32>(Data[CrowdyVideoFragment::FrameIdOffset + 1]);

		FragmentIndex = Data[CrowdyVideoFragment::IndexOffset];
		FragmentCount = Data[CrowdyVideoFragment::CountOffset];
		if (FragmentCount < 1 || FragmentCount > CrowdyVideoFragment::MaxFragments)
		{
			return false;
		}

		if (FragmentIndex >= FragmentCount)
		{
			return false;
		}

		GUID = USerializationFunctionLibrary::ToGuid(UUID);

		FragmentView = Data;
		BodyView = Data.RightChop(CrowdyVideoFragment::HeaderBytes);
		return true;
	}
};
