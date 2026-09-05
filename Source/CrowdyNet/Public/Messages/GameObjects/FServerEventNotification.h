// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Messages/GameObjects/FGameEventNotification.h"
#include "Serialization/CrowdyFrame.h"

/**
 * A server-ORIGINATED spatial event (UDP opcode 139, SERVER_EVENT_NOTIFICATION).
 *
 * It derives from FGameEventNotification to share that type's fields, NOT its decode. The two body
 * layouts differ, so this type overrides DecodePayload and the override is load-bearing: the base
 * expects a length-prefixed state and would read that length out of the container id, losing the
 * payload while reporting success. The differences are set out above the override. Do not fold this
 * decode back into the base's.
 *
 * Game Model model-driven notifications (a function's notifications entry with kind: spatial,
 * emitAs: server_event) arrive as this. It is routed by its EventType rather than by opcode, so a
 * subscriber names the event type it wants instead of claiming the whole opcode.
 */
struct FServerEventNotification : FGameEventNotification
{
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::SERVER_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Server Event Notification";
	}

	// The server's model-driven SERVER_EVENT (139) shares the spatial header with a relayed client event
	// but not the body layout, so the base's decode cannot be reused. Two differences matter:
	//   - the header's 32-octet actor id region holds a source GUID (16 BINARY octets) followed by a
	//     target GUID (16), not a 32-character ASCII id, so it is never read as text here;
	//   - the state carries NO length prefix. It runs to the end of the message, and the Game Model
	//     model-changed carrier writes the container id (ASCII) into it.
	// Body layout, little-endian, after the header the frame has already taken off:
	//   [0..1]   eventType (uint16)
	//   [2..]    state, raw app bytes
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		constexpr int32 StateOffset = sizeof(uint16);

		if (Data.Num() < StateOffset)
		{
			return false;
		}

		// Only the fields this layout actually carries. The actor id is left alone for the reason above,
		// and the trailer's timestamp and sequence are not read here, matching what this type has always
		// reported for them.
		AppID = Frame.Envelope.AppId;
		ChunkX = Frame.Envelope.ChunkX;
		ChunkY = Frame.Envelope.ChunkY;
		ChunkZ = Frame.Envelope.ChunkZ;
		bContainsAuth = Frame.Envelope.bContainsAuth;

		int32 Offset = 0;
		USerializationFunctionLibrary::DeserializeValue(Data, EventType, Offset);

		const int32 StateLen = Data.Num() - StateOffset;
		StateView = StateLen > 0
			? TConstArrayView<uint8>(Data.GetData() + StateOffset, StateLen)
			: TConstArrayView<uint8>();

		// Derived from the frame's length rather than read off the wire, but still assigned: every other
		// carrier of this body guarantees StateSize == StateView.Num(), and a reader that trusts the
		// declared length would otherwise see every 139 as an empty event.
		StateSize = StateLen > 0 ? StateLen : 0;

		State.Reset(); // raw app payload (the container id), not a serialized CrowdyEvent FInstancedStruct
		return true;
	}
};
