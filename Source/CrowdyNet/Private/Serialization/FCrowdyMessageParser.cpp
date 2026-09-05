#include "Serialization/FCrowdyMessageParser.h"
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/FDefaultMessage.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Messages/GameObjects/FGameObjectActivationNotification.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Serialization/CrowdyFrameSplit.h"
#include "Subsystem/CrowdyGameSession.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/wire/codec.hpp"
THIRD_PARTY_INCLUDES_END

namespace
{
	int32 GCrowdyParserPoolActorUpdates = 1;
	FAutoConsoleVariableRef CVarCrowdyParserPoolActorUpdates(
		TEXT("crowdy.net.receive.poolactorupdates"),
		GCrowdyParserPoolActorUpdates,
		TEXT("Reuse actor update message objects instead of allocating one per received message (default 1). ")
		TEXT("Turn it off to measure what the pool is worth on the same build."),
		ECVF_Default);

	crowdy::Bytes CrowdyParserSpanFrom(const TConstArrayView<uint8> Data)
	{
		return Data.IsEmpty()
			? crowdy::Bytes()
			: crowdy::Bytes(Data.GetData(), static_cast<std::size_t>(Data.Num()));
	}

	TConstArrayView<uint8> CrowdyParserViewFrom(const crowdy::Bytes Span)
	{
		return Span.empty()
			? TConstArrayView<uint8>()
			: TConstArrayView<uint8>(Span.data(), static_cast<int32>(Span.size()));
	}

	// Two descriptions of one opcode decide whether a datagram is unpacked: the one below, and the one
	// the shared walk uses to recognise the same thing. If they ever disagreed, the walk would hand back
	// the whole datagram as a single message, which unpacks to the same datagram again and never ends.
	static_assert(static_cast<uint8>(ECrowdyMessageType::MESSAGE_BUNDLE)
			== static_cast<uint8>(crowdy::wire::MessageType::MessageBundle),
		"A packed datagram is no longer the same opcode on both sides of the walk.");
}

FCrowdyMessageParser::FCrowdyMessageParser(FCrowdyServiceRegistry* InServiceRegistry,
                                           UCrowdyUDPSubsystem* InUDPSubsystem, UCrowdyGameSession* InGameSession,
                                           TFunction<void()> InTokenExpiredCallback)
{
	ServiceRegistry = InServiceRegistry;
	UDPSubsystem = InUDPSubsystem;
	GameSession = InGameSession;
	TokenExpiredCallback = MoveTemp(InTokenExpiredCallback);
}

TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> FCrowdyMessageParser::ParseMessage(
	const TConstArrayView<uint8> Datagram)
{
	FCrowdyFrame Frame;
	if (!FCrowdyFrame::FromMessageBytes(Datagram, Frame))
	{
		return MakeShared<FDefaultMessage>();
	}

	if (static_cast<ECrowdyMessageType>(Frame.Opcode) == ECrowdyMessageType::MESSAGE_BUNDLE)
	{
		return ParseBundle(Datagram);
	}

	if (!CrowdyFrameSplit::ReadEnvelopeForOpcode(Frame))
	{
		// A message dropped here reaches nothing at all, which looks exactly like a message that never
		// arrived, so it is worth saying. crowdy.net.trace 1 dumps the bytes.
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[Parser] An opcode %u datagram of %d bytes is too short for the envelope that opcode carries."),
			Frame.Opcode, Datagram.Num());
		return MakeShared<FDefaultMessage>();
	}

	return DecodeFrame(Frame);
}

TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> FCrowdyMessageParser::ParseBundle(
	const TConstArrayView<uint8> Datagram, const bool bNested)
{
	TArray<TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>> MessagesInBundle;

	const crowdy::Status Walk = crowdy::wire::forEachMessage(CrowdyParserSpanFrom(Datagram),
		[this, &MessagesInBundle](const crowdy::Bytes Member)
		{
			// An entry of no length describes no message. Reading one would deliver an
			// unknown-opcode placeholder that nothing subscribed to.
			FCrowdyFrame MemberFrame;
			if (!FCrowdyFrame::FromMessageBytes(CrowdyParserViewFrom(Member), MemberFrame, true))
			{
				return;
			}

			// A bundle may itself contain one. Its members are delivered from the inner walk, and the
			// placeholder it returns is delivered alongside them, which is what a bundle standing on
			// its own also produces.
			if (static_cast<ECrowdyMessageType>(MemberFrame.Opcode) == ECrowdyMessageType::MESSAGE_BUNDLE)
			{
				MessagesInBundle.Add(ParseBundle(CrowdyParserViewFrom(Member), true));
				return;
			}

			// A member that cannot carry the envelope its opcode requires is delivered as the
			// unknown-opcode placeholder, which is what this walk already does with a member no
			// decoder accepts.
			MessagesInBundle.Add(CrowdyFrameSplit::ReadEnvelopeForOpcode(MemberFrame)
				? DecodeFrame(MemberFrame)
				: MakeShared<FDefaultMessage>());
		});

	// Reported once for the datagram rather than once per level, since a sender controls both the
	// nesting and the truncation and could otherwise turn one datagram into a flood of log lines.
	if (!Walk.ok() && !bNested)
	{
		// The walk stops at the first entry it cannot read whole, whether that is a declared length
		// running past the end or a remainder too short to hold one. Everything whole in front of it is
		// still delivered, so ending early costs the tail of the datagram rather than all of it.
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[Parser] A %d byte packed datagram ended early; delivering the %d whole messages ahead of the break."),
			Datagram.Num(), MessagesInBundle.Num());
	}

	// Every member is decoded before any at this level is delivered, so a subscriber cannot tear down
	// what is running this walk while the datagram is still being read. A member that is itself a packed
	// datagram is the exception: its own members are delivered from the inner walk, which runs while this
	// one is still iterating.
	for (const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message : MessagesInBundle)
	{
		ServiceRegistry->DispatchMessage(Message);
		UDPSubsystem->IncrementReceivedMessageCount();
	}

	return MakeShared<FDefaultMessage>(); // bundle itself doesn't count as a message
}

TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> FCrowdyMessageParser::DecodeFrame(const FCrowdyFrame& Frame)
{
	// Half of the per-message delivery cost splits here, between building the message and routing it.
	// This scope also covers the message OBJECT itself: every branch below allocates one through
	// MakeShared, so an atomically refcounted heap allocation per received message is inside this number
	// rather than beside it.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DecodeFrame);

	switch (static_cast<ECrowdyMessageType>(Frame.Opcode))
	{
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
		{
			// Pooled rather than allocated, and the pool only ever hands back a message nothing else still
			// holds, so a subscriber that retained the last one keeps reading the message it retained.
			TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
				GCrowdyParserPoolActorUpdates != 0
					? ActorUpdatePool.Acquire()
					: MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();

			Message->SetExpectedStateSize(ExpectedActorStateSize);
			
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			
			return Message;
		}
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
		{
			TSharedRef<FGameEventNotification> Message = MakeShared<FGameEventNotification>();
			if (!Message->DecodePayload(Frame))
			{
				UDPSubsystem->IncrementTotalClientNotifiesReceived();
				return MakeShared<FDefaultMessage>();
			}
			UDPSubsystem->IncrementTotalClientNotifiesReceived();
			return Message;
		}
	case ECrowdyMessageType::SERVER_EVENT_NOTIFICATION:
		{
			// Server-originated spatial event (opcode 139). Same wire layout as a client event; the Game
			// Model subsystem subscribes to this event type and filters it by EventType. Used by Game Model
			// model-driven notifications (kind: spatial, emitAs: server_event).
			TSharedRef<FServerEventNotification> Message = MakeShared<FServerEventNotification>();
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION:
		{
			TSharedRef<FVoxelUpdateNotificationMessage> Message = MakeShared<FVoxelUpdateNotificationMessage>();
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION:
		{
			TSharedRef<FClientAudioNotification> Message = MakeShared<FClientAudioNotification>();
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::GENERIC_ERROR_MESSAGE:
		{
			if (Frame.Body.Num() >= 2)
			{
				HandleGenericErrorMessage(Frame.Body[1], Frame.Body[0]);
			}
			return MakeShared<FDefaultMessage>();
		}
	case ECrowdyMessageType::GENERIC_SPATIAL_1:
		{
			TSharedRef<FPingTestMessage> Message = MakeShared<FPingTestMessage>();
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::SINGLE_ACTOR_MESSAGE:
		{
			TSharedRef<FSingleActorNotification> Message = MakeShared<FSingleActorNotification>();
			if (!Message->DecodePayload(Frame))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION:
		{
			TSharedRef<FChannelMessageNotification> Message = MakeShared<FChannelMessageNotification>();
			if (!Message->DecodePayload(Frame))
			{
				// A dropped channel frame is otherwise invisible; surface it (crowdy.net.trace 1 dumps the bytes).
				UE_LOG(LogCrowdyNet, Warning,
					TEXT("[Parser] CHANNEL(18) frame dropped: the decode rejected a malformed frame (%d bytes)."),
					Frame.Body.Num());
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}

	default:
		return MakeShared<FDefaultMessage>();
	}
	
}

void FCrowdyMessageParser::SetExpectedActorStateSize(const int32 NewSize)
{
	ExpectedActorStateSize = NewSize;
}

void FCrowdyMessageParser::HandleGenericErrorMessage(const uint8 ErrorType, const uint8 SequenceNumber) const
{
	// Server-side gameplay error code (not the legacy ECrowdyErrorCode set): the app
	// token lapsed mid-session and Buddy dropped the session. The owner re-mints and
	// re-assigns. See cks-docs replication-api error codes / native-clients.
	constexpr uint8 TokenExpiredCode = 32;
	if (ErrorType == TokenExpiredCode)
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("UDP TOKEN_EXPIRED (32) seq=%d - requesting app-token refresh."), SequenceNumber);
		if (TokenExpiredCallback)
		{
			TokenExpiredCallback();
		}
		return;
	}

	// Every other code is reported by the transport, which is the only thing that can say which send the sequence
	// belongs to. Naming it a second time here would be a duplicate line carrying strictly less information.
}
