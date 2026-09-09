// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Messages/Actor/FActorLeftNotification.h"
#include "Serialization/CrowdyActorId.h"
#include "Serialization/CrowdyFrame.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

// Building a delivery is the only way to drive a departure without a socket, and more than one case needs one.
// It lives in a header rather than in each file's anonymous namespace because this module compiles without unity
// batching turned off everywhere, and two identical helpers in two translation units redefine each other.

/**
 * One delivery as the router would build it, with everything it borrows kept alive alongside it.
 *
 * A delivery holds a reference to the octets and to the message, and both die with the call in production.
 * Here they have to outlive the construction, so all three travel together.
 */
struct FCrowdyActorTestDeliveryScope
{
	TArray<uint8> Bytes;
	TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> Message;
	TUniquePtr<FCrowdyDelivery> Delivery;

	explicit FCrowdyActorTestDeliveryScope(
		const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe>& InMessage, TArray<uint8>&& InBytes,
		const ECrowdyMessageType InType = ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION)
		: Bytes(MoveTemp(InBytes))
		, Message(InMessage)
	{
		// Message rather than InMessage: the delivery keeps a reference to the shared pointer it is handed, so it
		// has to be the one this scope owns and not the argument that is about to go away.
		Delivery = MakeUnique<FCrowdyDelivery>(InType, FCrowdyPayloadKey(), nullptr, Bytes, Message);
	}
};

/** An actor id whose 32 octets are all one character, so two senders are told apart by what they are filled with. */
inline FCrowdyActorId CrowdyActorTestSender(const ANSICHAR Fill)
{
	FCrowdyActorId Id;
	for (int32 Index = 0; Index < FCrowdyActorId::NumOctets; ++Index)
	{
		Id.Octets[Index] = static_cast<uint8>(Fill);
	}
	return Id;
}

/** One departure delivery, built the way the router builds one, for an actor named by its wire octets. */
inline TUniquePtr<FCrowdyActorTestDeliveryScope> CrowdyActorTestDeparture(const FCrowdyActorId& Sender,
	const ECrowdyActorLeftReason Reason = ECrowdyActorLeftReason::Stale)
{
	TArray<uint8> Bytes = { static_cast<uint8>(Reason) };

	FCrowdyFrame Frame;
	Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION);
	Frame.Body = Bytes;
	Frame.Envelope.ChunkX = 11;
	Frame.Envelope.ChunkY = -22;
	Frame.Envelope.ChunkZ = 33;
	Frame.Envelope.Uuid = TConstArrayView<uint8>(Sender.Octets, FCrowdyActorId::NumOctets);
	Frame.Envelope.Timestamp = 1700000000123;
	Frame.Envelope.Sequence = 43;
	Frame.bHasEnvelope = true;

	TSharedRef<FActorLeftNotification, ESPMode::ThreadSafe> Message =
		MakeShared<FActorLeftNotification, ESPMode::ThreadSafe>();
	if (!Message->DecodePayload(Frame))
	{
		return nullptr;
	}

	return MakeUnique<FCrowdyActorTestDeliveryScope>(Message, MoveTemp(Bytes),
		ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION);
}

#endif // WITH_DEV_AUTOMATION_TESTS
