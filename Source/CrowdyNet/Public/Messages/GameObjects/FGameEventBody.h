#pragma once

#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyActorId.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * The payload a game event carries, shared by the outbound request and the inbound notification so
 * that one description of the layout serves both directions.
 *
 * The layout is a two-octet event type, a declared state length, that many state octets, and then a
 * routing block of one target octet plus 32 octets of target id. The routing block is appended after
 * the payload so that a reader which predates it simply runs out of bytes and keeps its defaults.
 */
struct FGameEventBody : ICrowdyMessage
{
	uint16 EventType = 0;

	int32 StateSize = 0;

protected:

	/**
	 * The payload octets a REQUEST owns and sends. Re-exposed by FGameEventRequest and hidden from the
	 * inbound notification, which does not own its bytes: it points at the receive buffer through its own
	 * StateView instead, and an empty array here would otherwise read as an event that carried nothing.
	 */
	TArray<uint8> StateBytes;

public:

	/**
	 * A borrowed view of the payload to serialize straight into the frame, used by the outbound request
	 * instead of encoding into StateBytes and copying that in. Set all three or none: with them set the
	 * body writes the payload in place and derives the declared length from what it actually wrote, so
	 * the two cannot disagree. StateBytes stays for the inbound notification, which owns its bytes.
	 *
	 * The view is read during Serialize() only, so it has to outlive that call and nothing else.
	 */
	const UScriptStruct* PayloadStruct = nullptr;
	const void* PayloadMemory = nullptr;
	FCrowdyTypeID PayloadTypeID = CROWDY_INVALID_TYPE_ID;

	/** Who the relay should deliver this to. Defaults to everyone, which is what an absent block means. */
	ECrowdyTarget Target = ECrowdyTarget::Everyone;

	/** The recipient when the target names one. Written as 32 hexadecimal digits. */
	FGuid TargetID;

protected:

	/**
	 * What AppendBody will write, so the frame can be sized once before anything is appended.
	 *
	 * Exact on the StateBytes path. On the payload-view path the encoded size is not knowable without
	 * encoding, so this leaves a datagram's worth of room instead: sizing it from StateBytes there would
	 * reserve for a buffer that is deliberately empty, and the frame would then regrow twice per send,
	 * the first time on the fixed fields alone before a single payload octet.
	 */
	int32 BodySize() const
	{
		const int32 AroundThePayload = sizeof(EventType)
			+ sizeof(StateSize)
			+ sizeof(uint8)
			+ FCrowdyActorId::NumOctets;

		return AroundThePayload + (IsSendingPayloadView() ? CrowdyFrameReserveHint : StateBytes.Num());
	}

	/** True when AppendBody will serialize a payload view into the frame rather than copy StateBytes in. */
	bool IsSendingPayloadView() const
	{
		return PayloadStruct != nullptr || PayloadMemory != nullptr;
	}

	/**
	 * Returns false when the payload could not be encoded, having left Data as it found it. The caller
	 * turns that into an empty frame, which the send path already refuses: encoding straight into the
	 * frame means this is the only place the failure the separate-buffer encode used to report is seen.
	 */
	[[nodiscard]] bool AppendBody(TArray<uint8>& Data) const
	{
		const int32 BodyStart = Data.Num();

		USerializationFunctionLibrary::AppendValue(Data, EventType);

		// Either field set means the caller meant to send a view, so a half-set one goes to the encoder and
		// is refused there. Requiring both to be set would read a half-set view as "no view given" and send
		// an empty payload under a zero length, which is a frame nobody asked for rather than a refusal.
		// BodySize reads the same question through IsSendingPayloadView, so the sizing and the branch
		// cannot disagree about which path this is.
		if (IsSendingPayloadView())
		{
			// The declared length is written as a placeholder and patched once the payload is in, because
			// writing it straight into the frame is the whole point and its length is only known after.
			// Derived from what was actually written, so unlike the StateBytes path below the length
			// cannot disagree with the octets that follow it.
			const int32 LengthOffset = Data.Num();
			USerializationFunctionLibrary::AppendValue(Data, static_cast<int32>(0));
			const int32 PayloadStart = Data.Num();

			if (!USerializationFunctionLibrary::AppendEventState(PayloadStruct, PayloadMemory, PayloadTypeID, Data))
			{
				// A spawn event whose nested state names no registered type fails partway through, so the
				// partial payload is rolled back rather than shipped under a length that describes it.
				Data.SetNum(BodyStart, EAllowShrinking::No);
				return false;
			}

			const int32 Written = Data.Num() - PayloadStart;
			USerializationFunctionLibrary::WriteValue(Data.GetData() + LengthOffset, Written);
		}
		else
		{
			USerializationFunctionLibrary::AppendValue(Data, StateSize);
			Data.Append(StateBytes);
		}

		Data.Add(static_cast<uint8>(Target));

		// The target id occupies a fixed 32 octets, the same width and the same hexadecimal convention as
		// the actor id in the header, so nothing behind it can shift.
		FCrowdyActorId::FromGuid(TargetID).AppendTo(Data);
		return true;
	}
};
