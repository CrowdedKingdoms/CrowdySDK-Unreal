#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Serialization/CrowdyActorId.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

/**
 * One inbound message as a subscriber sees it.
 *
 * Threading, which is the contract every subscriber depends on and the one place it is stated.
 *
 * A handler is always called on the game thread. There is one inbound path: the replication
 * connection is drained once per frame from the game thread's ticker, and every message it yields
 * is decoded and dispatched inside that drain. A handler may therefore touch UObjects, timers and
 * Blueprint delegates directly and needs no marshalling of its own.
 *
 * Subscriptions are expected to be released on the game thread as well, which is what makes a
 * release and a delivery sequential rather than concurrent. Given that, releasing a handle
 * guarantees the handler is not invoked again, including later in the fan-out of the delivery the
 * release was made from, because liveness is checked immediately before every call. What it does
 * not do is unwind a call already on the stack: a handler that releases its own handle still runs
 * to its own return. Nothing is guaranteed for a release issued from another thread; no caller
 * does that, and a subscriber that introduces one is outside this contract.
 *
 * Lifetime of this struct. It lives on the dispatching stack for the duration of the call and is
 * not copyable: keep the individual fields you need, or retain Message, which is a shared
 * reference. Do not try to keep this struct itself past the end of the call.
 *
 * Lifetime of the BYTES, which is a separate question and the one that catches people out. Every
 * octet view here, and every one hanging off Message, points into the receive buffer the transport
 * is still holding, and that buffer is reused as soon as this call returns. Retaining Message keeps
 * its decoded payload, which owns its own storage; it does NOT keep the bytes alive. A handler that
 * hands work to a later frame must therefore capture the DECODED value, never a view and never the
 * message that carries one.
 */
struct FCrowdyDelivery
{
	FCrowdyDelivery(ECrowdyMessageType InOpcode, FCrowdyPayloadKey InPayloadKey, const FInstancedStruct* InPayload,
		TConstArrayView<uint8> InPayloadBytes, const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe>& InMessage)
		: Opcode(InOpcode)
		, PayloadKey(InPayloadKey)
		, Payload(InPayload)
		, PayloadBytes(InPayloadBytes)
		, Message(InMessage)
	{
	}

	FCrowdyDelivery(const FCrowdyDelivery&) = delete;
	FCrowdyDelivery& operator=(const FCrowdyDelivery&) = delete;

	/** The opcode that carried this. Information attached to the payload, not the routing key. */
	ECrowdyMessageType Opcode = ECrowdyMessageType::BAD_MESSAGE;

	/** What this is. Unset for the opcodes that carry no payload type tag. */
	FCrowdyPayloadKey PayloadKey;

	/**
	 * The decoded payload, const. Null when the type number has no locally registered struct
	 * or when the carrier ships raw application bytes rather than a serialized struct.
	 */
	const FInstancedStruct* Payload = nullptr;

	/**
	 * The payload exactly as it arrived. Present even when Payload is null. Borrowed from the receive
	 * buffer, so it is valid only for the duration of the call it is passed to.
	 */
	TConstArrayView<uint8> PayloadBytes;

	/**
	 * The whole message, const. A shared reference, so the message object may be retained past the end of
	 * the call; its decoded payload comes with it, but the octet views on it do not (see above).
	 */
	const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe>& Message;

	const ICrowdyMessage& Get() const { return Message.Get(); }

	/** The header actor id. On a targeted send this is the destination, not the sender. */
	CROWDYNET_API const FCrowdyActorId& HeaderActorId() const;
	CROWDYNET_API FGuid HeaderGuid() const;

	CROWDYNET_API int64 ChunkX() const;
	CROWDYNET_API int64 ChunkY() const;
	CROWDYNET_API int64 ChunkZ() const;
	CROWDYNET_API int64 Timestamp() const;
	CROWDYNET_API uint8 SequenceNumber() const;

	/** The concrete message for an opcode-keyed subscription. The opcode fixes the type. */
	template <typename TMessage>
	const TMessage& GetAs() const { return static_cast<const TMessage&>(Message.Get()); }
};

using FCrowdyDeliveryHandler = TFunction<void(const FCrowdyDelivery&)>;
