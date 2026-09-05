#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Engine/GameInstance.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Network/CrowdyCpp/CrowdyCppInboundFrame.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Network/UDP/CrowdyCppReplicationTestSupport.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Serialization/FCrowdyMessageParser.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"

namespace CrowdyReceiveTestSupport
{
	/**
	 * The engine objects a parser needs before it can be asked to parse anything: it reads the local actor id off
	 * the session for every actor notification and counts client notifications on the UDP subsystem. Both are game
	 * instance subsystems, so they can only be built inside a game instance; a bare one is enough because neither is
	 * initialised and nothing on the parsing path reads it.
	 */
	struct FParserFixture
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyUDPSubsystem> Stats;
		FCrowdyServiceRegistry Registry;
		TUniquePtr<FCrowdyMessageParser> Parser;

		int32 TokenExpiredCount = 0;

		bool Open()
		{
			Instance.Reset(NewObject<UGameInstance>(GetTransientPackage()));
			if (!Instance.IsValid())
			{
				return false;
			}

			Session.Reset(NewObject<UCrowdyGameSession>(Instance.Get()));
			Stats.Reset(NewObject<UCrowdyUDPSubsystem>(Instance.Get()));
			if (!Session.IsValid() || !Stats.IsValid())
			{
				return false;
			}

			// The local actor is the id every frame in these tests is addressed to, which is what a real session looks
			// like: the connection sends under this id and the server echoes those messages back.
			Session->SetUUID(FString(ANSI_TO_TCHAR(CrowdyWireParity::GoldenUuid())));

			Parser = MakeUnique<FCrowdyMessageParser>(&Registry, Stats.Get(), Session.Get(),
				[this] { ++TokenExpiredCount; });
			return Parser.IsValid();
		}

		int64 MessagesReceived() const
		{
			return Stats.IsValid() ? Stats->GetUDPNetworkStats().TotalMessagesReceived : -1;
		}
	};

	/** Renders the fields every message carries, so a comparison names the field that diverged. */
	inline FString DescribeBase(const ICrowdyMessage& Message)
	{
		return FString::Printf(TEXT("type=%u uuid=%s app=%lld chunk=(%lld,%lld,%lld) timestamp=%lld sequence=%u"),
			static_cast<uint8>(Message.GetType()), *Message.UUID.ToString(), Message.AppID,
			Message.ChunkX, Message.ChunkY, Message.ChunkZ, Message.Timestamp, Message.SequenceNumber);
	}

	inline FString DescribeBytes(const TConstArrayView<uint8> Bytes)
	{
		return FString::Printf(TEXT("%d:%s"), Bytes.Num(), *BytesToHex(Bytes.GetData(), Bytes.Num()));
	}

	/** One inbound opcode: the payload a server would put in it, and what a consumer reads back out. */
	struct FInboundCase
	{
		ECrowdyMessageType Type = ECrowdyMessageType::GENERIC_SPATIAL_1;
		const TCHAR* Name = TEXT("");
		TArray<uint8> Payload;
		TFunction<FString(const ICrowdyMessage&)> Describe;
	};

	inline void AppendInt16(TArray<uint8>& Payload, const int16 Value)
	{
		Payload.Append(USerializationFunctionLibrary::SerializeValue(Value));
	}

	inline void AppendUInt16(TArray<uint8>& Payload, const uint16 Value)
	{
		Payload.Append(USerializationFunctionLibrary::SerializeValue(Value));
	}

	inline void AppendInt32(TArray<uint8>& Payload, const int32 Value)
	{
		Payload.Append(USerializationFunctionLibrary::SerializeValue(Value));
	}

	inline void AppendInt64(TArray<uint8>& Payload, const int64 Value)
	{
		Payload.Append(USerializationFunctionLibrary::SerializeValue(Value));
	}

	/** The 32 hex digits an event envelope carries as its target id. */
	inline void AppendTargetId(TArray<uint8>& Payload)
	{
		Payload.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);
	}

	inline TArray<FInboundCase> BuildInboundCases()
	{
		TArray<FInboundCase> Cases;

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION;
			Case.Name = TEXT("actor update");
			AppendInt32(Case.Payload, 8);
			Case.Payload.Append({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Actor = static_cast<const FActorUpdateNotificationMessage&>(Message);
				return FString::Printf(TEXT("%s stateSize=%d state=%s guid=%s"), *DescribeBase(Message),
					Actor.StateSize, *DescribeBytes(Actor.StateView), *Actor.GUID.ToString());
			};
		}

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
			Case.Name = TEXT("game event with an envelope");
			AppendUInt16(Case.Payload, 4242);
			AppendInt32(Case.Payload, 6);
			Case.Payload.Append({0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6});
			Case.Payload.Add(static_cast<uint8>(ECrowdyTarget::AllExceptSender));
			AppendTargetId(Case.Payload);
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Event = static_cast<const FGameEventNotification&>(Message);
				return FString::Printf(TEXT("%s eventType=%u state=%s target=%u targetId=%s"), *DescribeBase(Message),
					Event.EventType, *DescribeBytes(Event.StateView), static_cast<uint8>(Event.Target),
					*Event.TargetID.ToString());
			};
		}

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::SINGLE_ACTOR_MESSAGE;
			Case.Name = TEXT("actor to actor message");
			AppendUInt16(Case.Payload, 77);
			AppendInt32(Case.Payload, 4);
			Case.Payload.Append({0xb1, 0xb2, 0xb3, 0xb4});
			Case.Payload.Add(static_cast<uint8>(ECrowdyTarget::Everyone));
			AppendTargetId(Case.Payload);
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Event = static_cast<const FSingleActorNotification&>(Message);
				return FString::Printf(TEXT("%s eventType=%u state=%s target=%u"), *DescribeBase(Message),
					Event.EventType, *DescribeBytes(Event.StateView), static_cast<uint8>(Event.Target));
			};
		}

		{
			// The one case whose payload length is derived from the end of the frame rather than from a prefix, so
			// it is the case a signature region present on one path and absent on the other would break.
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::SERVER_EVENT_NOTIFICATION;
			Case.Name = TEXT("server event");
			AppendUInt16(Case.Payload, 60000);
			Case.Payload.Append(reinterpret_cast<const uint8*>("container-4711"), 14);
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Event = static_cast<const FServerEventNotification&>(Message);
				return FString::Printf(TEXT("%s eventType=%u state=%s"), *DescribeBase(Message),
					Event.EventType, *DescribeBytes(Event.StateView));
			};
		}

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION;
			Case.Name = TEXT("voxel update");
			AppendInt16(Case.Payload, 3);
			AppendInt16(Case.Payload, -4);
			AppendInt16(Case.Payload, 5);
			AppendInt16(Case.Payload, 9);
			AppendUInt16(Case.Payload, 3);
			Case.Payload.Append({0xc1, 0xc2, 0xc3});
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Voxel = static_cast<const FVoxelUpdateNotificationMessage&>(Message);
				return FString::Printf(TEXT("%s voxel=(%d,%d,%d) voxelType=%d stateSize=%u state=%s hasState=%d"),
					*DescribeBase(Message), Voxel.Vx, Voxel.Vy, Voxel.Vz, Voxel.VoxelType, Voxel.StateSize,
					*DescribeBytes(Voxel.StateBytes), Voxel.bContainsState ? 1 : 0);
			};
		}

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION;
			Case.Name = TEXT("audio notification");
			AppendInt32(Case.Payload, 48000);
			AppendInt32(Case.Payload, 2);
			AppendInt32(Case.Payload, 2);
			AppendInt32(Case.Payload, 3);
			Case.Payload.Append({0xd1, 0xd2, 0xd3});
			AppendInt32(Case.Payload, 2);
			Case.Payload.Append({0xd4, 0xd5});
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Audio = static_cast<const FClientAudioNotification&>(Message);
				FString Frames;
				for (const FCrowdyAudioFrame& Frame : Audio.Frames)
				{
					Frames += FString::Printf(TEXT("[%d:%s]"), Frame.FrameSize, *DescribeBytes(Frame.AudioData));
				}
				return FString::Printf(TEXT("%s rate=%d channels=%d frames=%s"), *DescribeBase(Message),
					Audio.SampleRate, Audio.NumChannels, *Frames);
			};
		}

		{
			// The parser has no case for a text notification, so both paths produce the default message. The assertion
			// is that they agree on that, which is what says the seam introduced no divergence of its own.
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION;
			Case.Name = TEXT("text notification");
			Case.Payload.Append({0xe1, 0xe2, 0xe3, 0xe4});
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				return FString::Printf(TEXT("type=%u name=%s"), static_cast<uint8>(Message.GetType()),
					*Message.GetTypeName().ToString());
			};
		}

		{
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::GENERIC_SPATIAL_1;
			Case.Name = TEXT("ping");
			AppendInt64(Case.Payload, 1699999999999);
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Ping = static_cast<const FPingTestMessage&>(Message);
				// ReceiveTime is stamped from the local clock at parse time, so it is deliberately not compared.
				return FString::Printf(TEXT("%s sendTime=%lld"), *DescribeBase(Message), Ping.SendTime);
			};
		}

		return Cases;
	}

	/** The datagram a server sends for a channel delivery, which carries no signature. */
	inline TArray<uint8> ChannelNotificationDatagram(const int64 ChannelId, const TArray<uint8>& SenderUuid,
		const TArray<uint8>& Payload, const int64 EpochMillis, const uint8 Sequence)
	{
		TArray<uint8> Datagram;
		Datagram.Add(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION));
		Datagram.Append(USerializationFunctionLibrary::SerializeValue(ChannelId));
		Datagram.Append(SenderUuid);
		Datagram.Append(USerializationFunctionLibrary::SerializeValue(static_cast<uint16>(Payload.Num())));
		Datagram.Append(Payload);
		Datagram.Append(USerializationFunctionLibrary::SerializeValue(EpochMillis));
		Datagram.Add(Sequence);
		return Datagram;
	}

	/** Records what it was dispatched, so the test can tell delivery from parsing. Holds its own
	 *  subscription handle instead of implementing a reception-layer interface: releasing the handle
	 *  is what stops delivery. */
	struct FRecordingReceptionLayer
	{
		int32 ReceiveCount = 0;
		FString LastTypeName;
		FCrowdySubscription Subscription;

		FCrowdyDeliveryHandler MakeHandler()
		{
			return [this](const FCrowdyDelivery& Delivery)
			{
				++ReceiveCount;
				LastTypeName = Delivery.Message->GetTypeName().ToString();
			};
		}

		/** Matches how a claimed opcode with no payload type tag used to be routed. */
		void SubscribeToOpcode(FCrowdyServiceRegistry& Registry, const ECrowdyMessageType Type)
		{
			Subscription = Registry.SubscribeToOpcode(Type,
				{ ECrowdySubscriptionRole::Observe, false, TEXT("FRecordingReceptionLayer") }, MakeHandler());
		}

		/** Matches how a claim of ACTOR_UPDATE_NOTIFICATION with no payload names used to be routed:
		 *  every actor-update payload, whatever its type. */
		void SubscribeToAllActorUpdates(FCrowdyServiceRegistry& Registry)
		{
			Subscription = Registry.SubscribeToAllPayloads(ECrowdyPayloadCategory::ActorUpdate,
				{ ECrowdySubscriptionRole::Observe, false, TEXT("FRecordingReceptionLayer") }, MakeHandler());
		}
	};

	/**
	 * Records everything it is dispatched in arrival order across several opcodes at once, which is what
	 * makes the order of a packed datagram's members observable rather than just their number.
	 */
	struct FBundleRecorder
	{
		TArray<FString> Received;
		TArray<FCrowdySubscription> Subscriptions;

		void Watch(FCrowdyServiceRegistry& Registry, const ECrowdyMessageType Type)
		{
			Subscriptions.Add(Registry.SubscribeToOpcode(Type,
				{ ECrowdySubscriptionRole::Observe, false, TEXT("FBundleRecorder") },
				[this](const FCrowdyDelivery& Delivery)
				{
					Received.Add(Delivery.Message->GetTypeName().ToString());
				}));
		}

		/** The two member types these tests pack, plus the placeholder a member that failed to decode becomes. */
		void WatchBundleMembers(FCrowdyServiceRegistry& Registry)
		{
			Subscriptions.Reserve(3);
			Watch(Registry, ECrowdyMessageType::GENERIC_SPATIAL_1);
			Watch(Registry, ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION);
			Watch(Registry, ECrowdyMessageType::BAD_MESSAGE);
		}

		FString Describe() const
		{
			return FString::Join(Received, TEXT(" | "));
		}

		void Release()
		{
			for (FCrowdySubscription& Subscription : Subscriptions)
			{
				Subscription.Release();
			}
			Subscriptions.Reset();
		}
	};

	inline const TCHAR* BundlePingTypeName() { return TEXT("Ping Test Message"); }
	inline const TCHAR* BundleVoxelTypeName() { return TEXT("Voxel Update Notification Message"); }
	inline const TCHAR* BundlePlaceholderTypeName() { return TEXT("Unknown or Default Message"); }

	/** One whole well-formed ping message, opcode byte included, as it sits inside a packed datagram. */
	inline TArray<uint8> BundlePingMember(const uint8 Sequence)
	{
		TArray<uint8> Payload;
		AppendInt64(Payload, 1699999999999);
		return CrowdyReplicationTestSupport::ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3,
			Payload, 8, 1, 1700000000000, Sequence);
	}

	/** The same for a voxel update, so the two members are told apart by type and not only by count. */
	inline TArray<uint8> BundleVoxelMember(const uint8 Sequence)
	{
		TArray<uint8> Payload;
		AppendInt16(Payload, 3);
		AppendInt16(Payload, -4);
		AppendInt16(Payload, 5);
		AppendInt16(Payload, 9);
		AppendUInt16(Payload, 0);
		return CrowdyReplicationTestSupport::ExpectedSpatial(ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, 7, 1, 2, 3,
			Payload, 8, 1, 1700000000000, Sequence);
	}

	/** The opcode a packed datagram leads with, before any member. */
	inline TArray<uint8> OpenBundle()
	{
		TArray<uint8> Datagram;
		Datagram.Add(static_cast<uint8>(ECrowdyMessageType::MESSAGE_BUNDLE));
		return Datagram;
	}

	/** Appends a member behind the two-byte length that introduces it. */
	inline void AppendBundleMember(TArray<uint8>& Datagram, const TArray<uint8>& Member)
	{
		AppendUInt16(Datagram, static_cast<uint16>(Member.Num()));
		Datagram.Append(Member);
	}

	/**
	 * Appends a length that claims more bytes than are actually written behind it, which is what a
	 * datagram cut short in transit looks like to a reader.
	 */
	inline void AppendTruncatedBundleMember(TArray<uint8>& Datagram, const TArray<uint8>& Member,
		const int32 BytesWritten)
	{
		AppendUInt16(Datagram, static_cast<uint16>(Member.Num()));
		Datagram.Append(Member.GetData(), FMath::Min(BytesWritten, Member.Num()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedInboundParityTest,
	"CrowdySDK.Transport.RoutedInboundFrameParsesLikeTheHandRolledFrame",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedInboundParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	// Both paths parse the same event payloads, whose type ids resolve against nothing in a headless run. That is
	// reported as a rate-gated warning by the payload registry's caller rather than as an error, so it fails
	// nothing here and no expectation is declared for it.
	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}
	if (!TestTrue(TEXT("the stand-in server learned where to answer"), Fixture.Prime()))
	{
		Fixture.Shut();
		return false;
	}

	const TArray<FInboundCase> Cases = BuildInboundCases();

	// Decoded AND described inside the handler, because a decoded message's payload octets are borrowed from
	// the connection's receive buffer and are valid only for the duration of the callback. Keeping the message
	// and reading it afterwards is the one use the delivery contract forbids, so what is kept here is the
	// description rather than the message. This is what production does for the same reason, and through the
	// same builder, so the mapping under test here is the one that ships.
	TMap<uint8, FString> Routed;
	{
		FCrowdyCppReplicationHandlers Handlers;
		Handlers.OnSpatial = [&Routed, &Parsing, &Cases](const FCrowdyCppSpatialMessage& Message)
		{
			const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Decoded =
				Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromSpatial(Message));

			const FInboundCase* Case = Cases.FindByPredicate([&Message](const FInboundCase& Candidate)
			{
				return static_cast<uint8>(Candidate.Type) == Message.Opcode;
			});

			Routed.Add(Message.Opcode, Case ? Case->Describe(*Decoded) : TEXT("no case declares this opcode"));
		};
		Fixture.Connection->SetHandlers(MoveTemp(Handlers));
	}

	// Distinct per case, so a field read from the wrong offset shows up as a difference rather than as a match
	// against a shared value.
	int64 Epoch = 1700000000000;
	uint8 Sequence = 11;
	TMap<uint8, TArray<uint8>> Sent;
	for (const FInboundCase& Case : Cases)
	{
		const TArray<uint8> Datagram = ExpectedSpatial(Case.Type, 7, 12, -34, 56, Case.Payload, 8, 1, Epoch, Sequence);
		Sent.Add(static_cast<uint8>(Case.Type), Datagram);
		TestTrue(FString::Printf(TEXT("the %s frame was pushed to the client"), Case.Name),
			Fixture.Server.SendToClient(Datagram));
		++Epoch;
		++Sequence;
	}

	const bool bAllArrived = WaitUntil([&Fixture, &Routed, &Cases]()
	{
		Fixture.Connection->Poll();
		return Routed.Num() >= Cases.Num();
	});

	if (!TestTrue(FString::Printf(TEXT("every pushed frame was delivered and decoded (%d of %d)"),
		Routed.Num(), Cases.Num()), bAllArrived))
	{
		Fixture.Shut();
		return false;
	}

	for (const FInboundCase& Case : Cases)
	{
		const uint8 Opcode = static_cast<uint8>(Case.Type);
		const FString* Routing = Routed.Find(Opcode);
		if (!TestNotNull(*FString::Printf(TEXT("the %s frame was decoded"), Case.Name), Routing))
		{
			continue;
		}

		// Two producers, one decoder: one side read the envelope off the datagram the server actually sent, the
		// other was handed it already decoded. This asks whether a consumer could tell the two apart.
		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> HandRolled =
			Parsing.Parser->ParseMessage(Sent[Opcode]);

		// Deliberately not compared: the replication distance, the decay rate and the signature flag. A delivered
		// message does not report any of the three, so a routed frame leaves all of them unset by design, and the
		// test below pins that rather than this one hiding it.
		TestEqual(FString::Printf(TEXT("a routed %s message is what the hand-rolled path produced"), Case.Name),
			*Routing, Case.Describe(*HandRolled));
	}

	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedInboundUncarriedFieldsTest,
	"CrowdySDK.Transport.RoutedInboundFrameReportsNoSignatureOrFanOut",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedInboundUncarriedFieldsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	// A ping frame, because it is the shortest layout that carries the full spatial header.
	TArray<uint8> Payload;
	AppendInt64(Payload, 1699999999999);
	const TArray<uint8> ServerDatagram = ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3, Payload,
		8, 1, 1700000000000, 9);

	FCrowdyCppSpatialMessage Decoded;
	Decoded.Opcode = static_cast<uint8>(ECrowdyMessageType::GENERIC_SPATIAL_1);
	Decoded.AppId = 7;
	Decoded.ChunkX = 1;
	Decoded.ChunkY = 2;
	Decoded.ChunkZ = 3;
	Decoded.Uuid = GoldenUuidView();
	Decoded.Payload = Payload;
	Decoded.EpochMillis = 1700000000000;
	Decoded.Sequence = 9;

	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> HandRolled = Parsing.Parser->ParseMessage(ServerDatagram);
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Routed =
		Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromSpatial(Decoded));

	// What the hand-rolled path reports, for contrast: these three fields do come off a real frame.
	TestTrue(TEXT("the hand-rolled frame reports a signature"), HandRolled->bContainsAuth);
	TestEqual(TEXT("the hand-rolled frame reports its fan-out distance"),
		static_cast<uint8>(HandRolled->ReplicationDistance), static_cast<uint8>(8));

	// And what the routed one reports instead. The three are not read back off a delivered message, so a routed
	// frame leaves them unset rather than guessing at them.
	TestFalse(TEXT("a routed frame does not claim a signature"), Routed->bContainsAuth);
	TestEqual(TEXT("a routed frame carries no fan-out distance"),
		static_cast<uint8>(Routed->ReplicationDistance),
		static_cast<uint8>(ECrowdyReplicationDistance::None));
	TestEqual(TEXT("a routed frame carries no decay rate"),
		static_cast<uint8>(Routed->DecayRate), static_cast<uint8>(ECrowdyDecayRate::No_Decay));

	// The fields that do survive, on the same message, so this test cannot pass by decoding nothing at all.
	TestEqual(TEXT("the routed frame keeps the server timestamp"), Routed->Timestamp, HandRolled->Timestamp);
	TestEqual(TEXT("the routed frame keeps the sequence number"),
		static_cast<int32>(Routed->SequenceNumber), static_cast<int32>(HandRolled->SequenceNumber));
	TestEqual(TEXT("the routed frame keeps the actor id"), Routed->UUID, HandRolled->UUID);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedInboundChannelTest,
	"CrowdySDK.Transport.RoutedChannelFrameParsesLikeTheHandRolledFrame",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedInboundChannelTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}
	if (!TestTrue(TEXT("the stand-in server learned where to answer"), Fixture.Prime()))
	{
		Fixture.Shut();
		return false;
	}

	// Keyed by channel id rather than by arrival order: two loopback datagrams can be delivered in either order, and
	// an order-indexed comparison would report that as a parity break.
	TMap<int64, TSharedPtr<ICrowdyMessage, ESPMode::ThreadSafe>> Routed;
	{
		FCrowdyCppReplicationHandlers Handlers;
		Handlers.OnChannel = [&Routed, &Parsing](const FCrowdyCppChannelMessage& Message)
		{
			Routed.Add(Message.ChannelId,
				Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromChannel(Message)));
		};
		Fixture.Connection->SetHandlers(MoveTemp(Handlers));
	}

	TArray<uint8> RelayedSender;
	RelayedSender.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);

	// A server-originated notification has no sending client, so those 32 octets are not a 32-character id at all.
	// Dropping such a frame once silently broke every model-driven re-pull hint, so it is covered explicitly.
	TArray<uint8> AbsentSender;
	AbsentSender.SetNumZeroed(32);

	// Held separately so the comparison below can be anchored to what was pushed rather than only to what
	// the other path decoded.
	TMap<int64, TArray<uint8>> ExpectedPayloads;
	ExpectedPayloads.Add(9001, TArray<uint8>({0x01, 0x02, 0x03, 0x04}));
	ExpectedPayloads.Add(9002, TArray<uint8>(reinterpret_cast<const uint8*>("cmc:4711"), 8));

	// The senders those two frames were pushed with, written as literal ids rather than derived from the
	// same octets through the same call the decoder makes, so a change to how the sender region is read
	// cannot move the expectation along with it. A frame the server originated has no sender, and 32 zero
	// octets are an id that was never filled in.
	TMap<int64, FCrowdyActorId> ExpectedSenders;
	ExpectedSenders.Add(9001, CrowdyWireParity::GoldenActorId());
	ExpectedSenders.Add(9002, FCrowdyActorId());

	TMap<int64, TArray<uint8>> Sent;
	Sent.Add(9001, ChannelNotificationDatagram(9001, RelayedSender, {0x01, 0x02, 0x03, 0x04}, 1700000000001, 21));
	Sent.Add(9002, ChannelNotificationDatagram(9002, AbsentSender,
		TArray<uint8>(reinterpret_cast<const uint8*>("cmc:4711"), 8), 1700000000002, 22));

	for (const TPair<int64, TArray<uint8>>& Pair : Sent)
	{
		TestTrue(TEXT("a channel frame was pushed to the client"), Fixture.Server.SendToClient(Pair.Value));
	}

	const bool bAllArrived = WaitUntil([&Fixture, &Routed, &Sent]()
	{
		Fixture.Connection->Poll();
		return Routed.Num() >= Sent.Num();
	});

	if (!TestTrue(FString::Printf(TEXT("every channel frame was delivered and decoded (%d of %d)"),
		Routed.Num(), Sent.Num()), bAllArrived))
	{
		Fixture.Shut();
		return false;
	}

	for (const TPair<int64, TArray<uint8>>& Pair : Sent)
	{
		const TSharedPtr<ICrowdyMessage, ESPMode::ThreadSafe>* Routing = Routed.Find(Pair.Key);
		if (!TestNotNull(*FString::Printf(TEXT("channel %lld was delivered"), Pair.Key), Routing))
		{
			continue;
		}

		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> HandRolled = Parsing.Parser->ParseMessage(Pair.Value);

		const auto& HandRolledChannel = static_cast<const FChannelMessageNotification&>(*HandRolled);
		const auto& RoutedChannel = static_cast<const FChannelMessageNotification&>(**Routing);

		// Anchored to the bytes that were actually pushed, not only to each other. Both sides run the same
		// decoder, so comparing them alone would be satisfied by two identically empty results.
		TestEqual(TEXT("the routed channel id is the one that was sent"), RoutedChannel.ChannelId, Pair.Key);
		TestEqual(TEXT("the routed payload is the payload that was sent"),
			DescribeBytes(RoutedChannel.Payload), DescribeBytes(ExpectedPayloads[Pair.Key]));

		TestEqual(TEXT("the routed channel id matches"), RoutedChannel.ChannelId, HandRolledChannel.ChannelId);
		TestEqual(TEXT("the routed channel payload matches"),
			DescribeBytes(RoutedChannel.Payload), DescribeBytes(HandRolledChannel.Payload));
		TestEqual(TEXT("the routed channel timestamp matches"), RoutedChannel.Timestamp, HandRolledChannel.Timestamp);
		TestEqual(TEXT("the routed channel sequence matches"),
			static_cast<int32>(RoutedChannel.SequenceNumber), static_cast<int32>(HandRolledChannel.SequenceNumber));
		// Anchored like the two above it: one of these frames carries no sender at all, so comparing the
		// two decoders to each other alone would be satisfied by both of them reading nothing.
		TestEqual(TEXT("the routed sender id is the sender that was pushed"), RoutedChannel.UUID,
			ExpectedSenders[Pair.Key]);
		TestEqual(TEXT("the routed sender id matches"), RoutedChannel.UUID, HandRolledChannel.UUID);
	}

	Fixture.Shut();
	return true;
}

/**
 * An actor id is a fixed 32 octets, and every spatial decoder refuses a frame that carries any other
 * number of them rather than reading an id out of a shorter run.
 *
 * The refusal is only reachable from the routed transport. A datagram read off the socket has its id
 * taken from a fixed region of the header, so that path can produce no other length; a routed frame
 * carries the octets the connection handed over, and those arrive as a plain view with no length
 * attached to it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedShortActorIdTest,
	"CrowdySDK.Transport.RoutedSpatialFrameWithAShortActorIdIsRefused",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedShortActorIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	// One octet short, and made of the same characters a real id is made of, so a refusal is caused by the
	// length and not by the octets being unreadable as text.
	TArray<uint8> ShortUuid;
	ShortUuid.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()),
		FCrowdyActorId::NumOctets - 1);

	// Three layouts rather than one, because each decoder carries its own copy of the refusal and one of
	// them could be dropped without the others noticing.
	struct FShortIdCase
	{
		ECrowdyMessageType Type = ECrowdyMessageType::GENERIC_SPATIAL_1;
		const TCHAR* Name = TEXT("");
		TArray<uint8> Payload;
	};

	TArray<FShortIdCase> Cases;
	{
		FShortIdCase& Case = Cases.AddDefaulted_GetRef();
		Case.Type = ECrowdyMessageType::GENERIC_SPATIAL_1;
		Case.Name = TEXT("ping");
		AppendInt64(Case.Payload, 1699999999999);
	}
	{
		FShortIdCase& Case = Cases.AddDefaulted_GetRef();
		Case.Type = ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION;
		Case.Name = TEXT("actor update");
		AppendInt32(Case.Payload, 8);
		Case.Payload.Append({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
	}
	{
		FShortIdCase& Case = Cases.AddDefaulted_GetRef();
		Case.Type = ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION;
		Case.Name = TEXT("voxel update");
		AppendInt16(Case.Payload, 3);
		AppendInt16(Case.Payload, -4);
		AppendInt16(Case.Payload, 5);
		AppendInt16(Case.Payload, 9);
		AppendUInt16(Case.Payload, 3);
		Case.Payload.Append({0xc1, 0xc2, 0xc3});
	}

	for (const FShortIdCase& Case : Cases)
	{
		auto BuildMessage = [&Case](const TArrayView<const uint8> Uuid)
		{
			FCrowdyCppSpatialMessage Message;
			Message.Opcode = static_cast<uint8>(Case.Type);
			Message.AppId = 7;
			Message.ChunkX = 1;
			Message.ChunkY = 2;
			Message.ChunkZ = 3;
			Message.Uuid = Uuid;
			Message.Payload = Case.Payload;
			Message.EpochMillis = 1700000000000;
			Message.Sequence = 9;
			return Message;
		};

		// The same payload behind a full-width id, so the refusal below is caused by the id rather than by
		// a payload no decoder would have accepted anyway.
		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Accepted =
			Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromSpatial(BuildMessage(GoldenUuidView())));
		TestEqual(*FString::Printf(TEXT("a routed %s frame with a full-width actor id decodes"), Case.Name),
			static_cast<int32>(Accepted->GetType()), static_cast<int32>(Case.Type));

		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Refused =
			Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromSpatial(BuildMessage(ShortUuid)));
		TestEqual(*FString::Printf(TEXT("a routed %s frame with a short actor id is refused"), Case.Name),
			static_cast<int32>(Refused->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));
	}

	return true;
}

/**
 * The channel layout is the exception, and deliberately so: a notification the server originated has no
 * sending client, so its sender region holds whatever the server left there. It is taken for what it is
 * worth and the message is delivered either way, since receivers act on the channel and the payload.
 *
 * An id that cannot be read is left unset rather than half-read, which is the same value an absent sender
 * produces. Refusing here instead once dropped every server-driven channel notification silently.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedChannelShortSenderTest,
	"CrowdySDK.Transport.RoutedChannelFrameWithAShortSenderIsDeliveredWithNoSender",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedChannelShortSenderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	TArray<uint8> ShortUuid;
	ShortUuid.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()),
		FCrowdyActorId::NumOctets - 1);

	const TArray<uint8> Payload(reinterpret_cast<const uint8*>("cmc:4711"), 8);

	auto BuildMessage = [&Payload](const TArrayView<const uint8> Sender)
	{
		FCrowdyCppChannelMessage Message;
		Message.ChannelId = 9200;
		Message.SenderUuid = Sender;
		Message.Payload = Payload;
		Message.EpochMillis = 1700000000000;
		Message.Sequence = 12;
		return Message;
	};

	// A full-width sender first, so the assertions below cannot pass because the sender region is ignored
	// outright.
	{
		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Decoded =
			Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromChannel(BuildMessage(GoldenUuidView())));
		TestEqual(TEXT("a channel frame with a full-width sender decodes"),
			static_cast<int32>(Decoded->GetType()),
			static_cast<int32>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION));

		const auto& Channel = static_cast<const FChannelMessageNotification&>(*Decoded);
		TestEqual(TEXT("and reports the sender it was pushed with"), Channel.UUID,
			CrowdyWireParity::GoldenActorId());
	}

	{
		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Decoded =
			Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromChannel(BuildMessage(ShortUuid)));
		TestEqual(TEXT("a channel frame with a short sender is still delivered"),
			static_cast<int32>(Decoded->GetType()),
			static_cast<int32>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION));

		const auto& Channel = static_cast<const FChannelMessageNotification&>(*Decoded);
		TestFalse(TEXT("with no sender rather than a truncated one"), Channel.UUID.IsSet());
		TestEqual(TEXT("and the channel it was sent on"), Channel.ChannelId, static_cast<int64>(9200));
		TestEqual(TEXT("and the payload it carried"), DescribeBytes(Channel.Payload), DescribeBytes(Payload));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedInboundErrorTest,
	"CrowdySDK.Transport.RoutedServerErrorReachesTheExpiredTokenRecovery",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedInboundErrorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	// The expired-token code, which is the one an error frame has to keep reaching: it is what re-mints the app
	// token and re-assigns the session, and it is reported through the parser rather than handled at the seam so
	// that recovery behaves the same whichever transport carried the frame.
	constexpr uint8 TokenExpired = 32;

	// The sequence precedes the code, and getting the two the wrong way round would make code 7 look like an expiry
	// and an expiry look like something to log and forget, so the order is stated here and read by the decoder.
	const uint8 ExpiryBody[] = { 7, TokenExpired };
	const uint8 OtherBody[] = { 8, 15 };

	// An error frame decodes to the default message: what matters is the side effect on the way through, not what
	// comes back, which is why the results are named and then left alone.
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Expiry =
		Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromError(ExpiryBody));
	TestEqual(TEXT("an expired-token error asks for a token refresh"), Parsing.TokenExpiredCount, 1);

	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> OtherError =
		Parsing.Parser->DecodeFrame(CrowdyCppInboundFrame::FromError(OtherBody));
	TestEqual(TEXT("any other error code asks for nothing"), Parsing.TokenExpiredCount, 1);

	// The same two octets arriving as a datagram reach the same recovery, which is the property that lets the
	// expired-token path stay in the parser rather than being handled at each transport's seam.
	const TArray<uint8> Datagram = {
		static_cast<uint8>(ECrowdyMessageType::GENERIC_ERROR_MESSAGE), 7, TokenExpired };
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> FromBytes = Parsing.Parser->ParseMessage(Datagram);
	TestEqual(TEXT("the same error read off a datagram asks for a refresh too"), Parsing.TokenExpiredCount, 2);
	TestEqual(TEXT("and produces the same message"),
		static_cast<int32>(FromBytes->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	TestEqual(TEXT("an error frame does not become a message a reception layer could act on"),
		static_cast<int32>(Expiry->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));
	TestEqual(TEXT("and neither does any other error code"),
		static_cast<int32>(OtherError->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedInboundReachesLayersTest,
	"CrowdySDK.Transport.RoutedInboundReachesTheReceptionLayers",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedInboundReachesLayersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}
	if (!TestTrue(TEXT("the stand-in server learned where to answer"), Fixture.Prime()))
	{
		Fixture.Shut();
		return false;
	}

	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(Parsing.Instance.Get()));
	if (!TestTrue(TEXT("the routing host was created"), Routing.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	FRecordingReceptionLayer Layer;
	Layer.SubscribeToOpcode(Parsing.Registry, ECrowdyMessageType::GENERIC_SPATIAL_1);

	Routing->SetReceiveTarget(Parsing.Parser.Get(), &Parsing.Registry, Parsing.Stats.Get());

	TestFalse(TEXT("nothing is polled before a connection is installed"), Routing->IsPolling());

	// Installing the connection is what installs the delivery handlers on it and starts the drain, so from here a
	// pushed frame should reach the reception layer with nothing else wired up.
	Routing->SetConnection(Fixture.Connection);
	TestTrue(TEXT("installing a connection starts polling it"), Routing->IsPolling());

	// Re-installing the same connection must not dispose it. Disposal is terminal, so getting this wrong leaves a dead
	// connection installed, and everything below would then fail for a reason that looks like a delivery bug.
	Routing->SetConnection(Fixture.Connection);
	TestTrue(TEXT("re-installing the same connection leaves it usable"),
		Fixture.Connection->GetState() != ECrowdyCppConnState::Closed);

	TArray<uint8> PingPayload;
	AppendInt64(PingPayload, 1699999999999);
	const TArray<uint8> PingFrame = ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3, PingPayload,
		8, 1, 1700000000000, 33);

	const int64 ReceivedBefore = Parsing.MessagesReceived();
	TestTrue(TEXT("a ping notification was pushed to the client"), Fixture.Server.SendToClient(PingFrame));

	// Polled directly rather than through the frame ticker, because an automation test body runs to completion inside
	// one frame. That the ticker exists at all is asserted through IsPolling above and below.
	const bool bReached = WaitUntil([&Fixture, &Layer]()
	{
		Fixture.Connection->Poll();
		return Layer.ReceiveCount > 0;
	});

	TestTrue(TEXT("a routed message reaches the reception layers"), bReached);
	TestEqual(TEXT("and arrives as the message type the opcode names"), Layer.LastTypeName,
		FString(TEXT("Ping Test Message")));
	TestTrue(TEXT("and is counted in the received-message statistics"),
		Parsing.MessagesReceived() > ReceivedBefore);

	// The other installed handlers, each of which could be deleted outright without any of the above noticing. A
	// channel delivery is how a model change is announced, and an error frame is what starts token recovery.
	FRecordingReceptionLayer ChannelLayer;
	ChannelLayer.SubscribeToOpcode(Parsing.Registry, ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION);

	// An actor notification carrying the local id used to be a liveness signal as well as a message, because the
	// parser called back into the socket transport's monitor for it. It is now only a message, and the connection
	// reports its own liveness instead.
	FRecordingReceptionLayer ActorLayer;
	ActorLayer.SubscribeToAllActorUpdates(Parsing.Registry);

	TArray<uint8> ActorPayload;
	AppendInt32(ActorPayload, 8);
	ActorPayload.Append({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
	TestTrue(TEXT("an actor notification carrying our own id was pushed"),
		Fixture.Server.SendToClient(ExpectedSpatial(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, 7, 1, 2, 3,
			ActorPayload, 8, 1, 1700000000004, 34)));

	TArray<uint8> ChannelSender;
	ChannelSender.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);
	TestTrue(TEXT("a channel notification was pushed"),
		Fixture.Server.SendToClient(ChannelNotificationDatagram(9100, ChannelSender,
			TArray<uint8>(reinterpret_cast<const uint8*>("cmc:99"), 6), 1700000000005, 35)));

	const bool bBothArrived = WaitUntil([&Fixture, &ActorLayer, &ChannelLayer]()
	{
		Fixture.Connection->Poll();
		return ChannelLayer.ReceiveCount > 0 && ActorLayer.ReceiveCount > 0;
	});

	TestTrue(TEXT("a routed channel message reaches its reception layer"), ChannelLayer.ReceiveCount > 0);
	TestTrue(TEXT("a routed self-addressed actor notification reaches its reception layer"),
		ActorLayer.ReceiveCount > 0);
	TestTrue(TEXT("both handlers were reached"), bBothArrived);

	// Detaching leaves the connection installed and polled, so delivery has to stop without anything crashing. The
	// frame is followed to the connection's own counter first, so this cannot pass because nothing arrived.
	const int64 ReceivedBeforeDetach = Parsing.MessagesReceived();
	const int64 ConnectionSawBeforeDetach = Fixture.Connection->GetStats().MessagesReceived;
	Routing->SetReceiveTarget(nullptr, nullptr, nullptr);
	TestTrue(TEXT("a ping notification was pushed after detaching"), Fixture.Server.SendToClient(PingFrame));

	const bool bArrivedWhileDetached = WaitUntil([&Fixture, ConnectionSawBeforeDetach]()
	{
		Fixture.Connection->Poll();
		return Fixture.Connection->GetStats().MessagesReceived > ConnectionSawBeforeDetach;
	});

	TestTrue(TEXT("the connection still received it"), bArrivedWhileDetached);
	TestEqual(TEXT("but with no parser attached nothing is counted"),
		Parsing.MessagesReceived(), ReceivedBeforeDetach);

	Routing->SetReceiveTarget(Parsing.Parser.Get(), &Parsing.Registry, Parsing.Stats.Get());

	// The error handler, proved by the fact that an error frame reaches the parser at all: it parses to nothing a
	// reception layer can act on, so what is observable is that it was counted like any other delivered message.
	//
	// The code here is deliberately NOT the expiry one. That one makes the connection re-assign its session on its
	// own account, independently of this handler, and a re-assignment provoked at the end of a test is torn down
	// before it can finish and reports a failure. What the expiry code goes on to do is covered on its own, against
	// the parser, where no connection is involved.
	const int64 CountedBeforeError = Parsing.MessagesReceived();
	constexpr uint8 InvalidRequest = 15;
	const TArray<uint8> ErrorFrame =
		{static_cast<uint8>(ECrowdyMessageType::GENERIC_ERROR_MESSAGE), 37, InvalidRequest};
	TestTrue(TEXT("a server error frame was pushed"), Fixture.Server.SendToClient(ErrorFrame));

	const bool bErrorDelivered = WaitUntil([&Fixture, &Parsing, CountedBeforeError]()
	{
		Fixture.Connection->Poll();
		return Parsing.MessagesReceived() > CountedBeforeError;
	});

	TestTrue(TEXT("a routed error frame reaches the parser"), bErrorDelivered);

	// Clearing the connection removes the frame ticker as well as disposing the connection, so this is not just
	// tidiness: a ticker outliving the test would fire against a released subsystem.
	Routing->SetConnection(nullptr);
	TestFalse(TEXT("clearing the connection stops polling"), Routing->IsPolling());

	ActorLayer.Subscription.Release();
	ChannelLayer.Subscription.Release();
	Layer.Subscription.Release();
	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConnectionUpAnnouncesSessionTest,
	"CrowdySDK.Transport.RoutedConnectionUpAnnouncesTheSession",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyConnectionUpAnnouncesSessionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	Parsing.Stats->SetConnectionState(EUDPConnectionState::Connecting);

	// The state is the visible half; the broadcast is the half everything else hangs off, including the reliable-RPC
	// channel joins and the timers a live session needs. Observed through a no-argument UFUNCTION on this same
	// object, because the delegate is a dynamic one and can only bind to a UFUNCTION: the counter is given a value
	// first, so the reset the broadcast triggers is visible.
	Parsing.Stats->IncrementReceivedMessageCount();
	TestTrue(TEXT("the counter the broadcast is observed through was given a value"),
		Parsing.Stats->GetUDPNetworkStats().TotalMessagesReceived > 0);

	Parsing.Stats->OnUDPConnectionSuccessful.AddDynamic(Parsing.Stats.Get(),
		&UCrowdyUDPSubsystem::ResetUDPNetworkStats);

	Parsing.Stats->MarkRoutedConnectionUp();
	TestEqual(TEXT("a connection reporting itself up connects the session"),
		Parsing.Stats->GetConnectionState(), EUDPConnectionState::Connected);
	TestEqual(TEXT("and announces it to everything waiting on the connection"),
		Parsing.Stats->GetUDPNetworkStats().TotalMessagesReceived, static_cast<int64>(0));

	Parsing.Stats->OnUDPConnectionSuccessful.RemoveAll(Parsing.Stats.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRoutedDeliveryHonoursDiscardTest,
	"CrowdySDK.Transport.RoutedDeliveryHonoursTheDiscardSwitch",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyRoutedDeliveryHonoursDiscardTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		Fixture.Shut();
		return false;
	}
	if (!TestTrue(TEXT("the stand-in server learned where to answer"), Fixture.Prime()))
	{
		Fixture.Shut();
		return false;
	}

	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(Parsing.Instance.Get()));
	if (!TestTrue(TEXT("the routing host was created"), Routing.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	Routing->SetReceiveTarget(Parsing.Parser.Get(), &Parsing.Registry, Parsing.Stats.Get());
	Routing->SetConnection(Fixture.Connection);

	FRecordingReceptionLayer Layer;
	Layer.SubscribeToOpcode(Parsing.Registry, ECrowdyMessageType::GENERIC_SPATIAL_1);

	TArray<uint8> PingPayload;
	AppendInt64(PingPayload, 1699999999999);
	const TArray<uint8> PingFrame = ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3, PingPayload,
		8, 1, 1700000000000, 33);

	// Turning message processing off is a development switch on the receive path, so a delivered message has to
	// honour it. The frame is followed to the connection's own counter, so this cannot pass because nothing
	// arrived.
	Parsing.Stats->ToggleUDPMessageProcessing();

	const int64 ConnectionSawBefore = Fixture.Connection->GetStats().MessagesReceived;
	TestTrue(TEXT("a ping notification was pushed while processing was off"), Fixture.Server.SendToClient(PingFrame));

	const bool bArrived = WaitUntil([&Fixture, ConnectionSawBefore]()
	{
		Fixture.Connection->Poll();
		return Fixture.Connection->GetStats().MessagesReceived > ConnectionSawBefore;
	});

	TestTrue(TEXT("the connection received it"), bArrived);
	TestEqual(TEXT("but a discarded message reaches no reception layer"), Layer.ReceiveCount, 0);

	// And is delivered normally again once processing is back on, so the assertion above is about the switch rather
	// than about delivery being broken.
	Parsing.Stats->ToggleUDPMessageProcessing();
	TestTrue(TEXT("a second ping notification was pushed"), Fixture.Server.SendToClient(PingFrame));

	const bool bDelivered = WaitUntil([&Fixture, &Layer]()
	{
		Fixture.Connection->Poll();
		return Layer.ReceiveCount > 0;
	});

	TestTrue(TEXT("a message delivered with processing back on reaches the reception layer"), bDelivered);

	Routing->SetConnection(nullptr);
	Layer.Subscription.Release();
	Fixture.Shut();
	return true;
}

// A server may pack several messages into one datagram. Every one of them has to be delivered, in the
// order it was packed, and the packing itself must not become a message of its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBundleTwoMembersTest,
	"CrowdySDK.Parser.BundleTwoMembersBothDispatched",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyBundleTwoMembersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FBundleRecorder Recorder;
	Recorder.WatchBundleMembers(Parsing.Registry);

	TArray<uint8> Bundle = OpenBundle();
	AppendBundleMember(Bundle, BundlePingMember(41));
	AppendBundleMember(Bundle, BundleVoxelMember(42));

	const int64 CountedBefore = Parsing.MessagesReceived();
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Result = Parsing.Parser->ParseMessage(Bundle);

	TestEqual(TEXT("the packing itself is not a message a subscriber can act on"),
		static_cast<int32>(Result->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	TestEqual(*FString::Printf(TEXT("both members were dispatched (got %s)"), *Recorder.Describe()),
		Recorder.Received.Num(), 2);
	if (Recorder.Received.Num() == 2)
	{
		TestEqual(TEXT("the first member is the one packed first"), Recorder.Received[0],
			FString(BundlePingTypeName()));
		TestEqual(TEXT("the second member is the one packed second"), Recorder.Received[1],
			FString(BundleVoxelTypeName()));
	}

	TestEqual(TEXT("both members were counted as received"),
		Parsing.MessagesReceived() - CountedBefore, static_cast<int64>(2));

	Recorder.Release();
	return true;
}

// A length of zero introduces no message at all. Reading one anyway would deliver a placeholder that
// nothing subscribed to and count it as traffic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBundleZeroLengthMemberTest,
	"CrowdySDK.Parser.BundleZeroLengthMemberIsSkipped",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyBundleZeroLengthMemberTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FBundleRecorder Recorder;
	Recorder.WatchBundleMembers(Parsing.Registry);

	TArray<uint8> Bundle = OpenBundle();
	AppendBundleMember(Bundle, BundlePingMember(43));
	AppendUInt16(Bundle, 0);
	AppendBundleMember(Bundle, BundleVoxelMember(44));

	const int64 CountedBefore = Parsing.MessagesReceived();
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Result = Parsing.Parser->ParseMessage(Bundle);

	TestEqual(TEXT("the packing itself is not a message a subscriber can act on"),
		static_cast<int32>(Result->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	// The placeholder type is watched too, so an entry read out of the empty run would show up here as a
	// third delivery rather than passing unnoticed.
	TestEqual(*FString::Printf(TEXT("only the two real members were dispatched (got %s)"), *Recorder.Describe()),
		Recorder.Received.Num(), 2);
	if (Recorder.Received.Num() == 2)
	{
		TestEqual(TEXT("the member before the empty run"), Recorder.Received[0], FString(BundlePingTypeName()));
		TestEqual(TEXT("the member after the empty run"), Recorder.Received[1], FString(BundleVoxelTypeName()));
	}

	TestEqual(TEXT("only the two real members were counted as received"),
		Parsing.MessagesReceived() - CountedBefore, static_cast<int64>(2));

	Recorder.Release();
	return true;
}

// A datagram cut short costs the tail of itself, not all of it: everything whole in front of the
// truncation is still delivered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBundleTruncatedTest,
	"CrowdySDK.Parser.BundleTruncatedDeliversThePrefix",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyBundleTruncatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FBundleRecorder Recorder;
	Recorder.WatchBundleMembers(Parsing.Registry);

	TArray<uint8> Bundle = OpenBundle();
	AppendBundleMember(Bundle, BundlePingMember(45));
	AppendTruncatedBundleMember(Bundle, BundleVoxelMember(46), 10);

	const int64 CountedBefore = Parsing.MessagesReceived();
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Result = Parsing.Parser->ParseMessage(Bundle);

	TestEqual(TEXT("the packing itself is not a message a subscriber can act on"),
		static_cast<int32>(Result->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	TestEqual(*FString::Printf(TEXT("the whole member ahead of the truncation was still dispatched (got %s)"),
		*Recorder.Describe()), Recorder.Received.Num(), 1);
	if (Recorder.Received.Num() == 1)
	{
		TestEqual(TEXT("and it is the member that arrived whole"), Recorder.Received[0],
			FString(BundlePingTypeName()));
	}

	TestEqual(TEXT("only the whole member was counted as received"),
		Parsing.MessagesReceived() - CountedBefore, static_cast<int64>(1));

	Recorder.Release();
	return true;
}

// A packed datagram may itself contain one. Its members have to be unpacked and delivered rather than
// handed on whole to a decoder that has no layout for them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBundleNestedTest,
	"CrowdySDK.Parser.BundleNestedBundleIsUnpacked",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyBundleNestedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FBundleRecorder Recorder;
	Recorder.WatchBundleMembers(Parsing.Registry);

	TArray<uint8> Inner = OpenBundle();
	AppendBundleMember(Inner, BundlePingMember(47));
	AppendBundleMember(Inner, BundleVoxelMember(48));

	TArray<uint8> Outer = OpenBundle();
	AppendBundleMember(Outer, Inner);

	const int64 CountedBefore = Parsing.MessagesReceived();
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Result = Parsing.Parser->ParseMessage(Outer);

	TestEqual(TEXT("the outer packing is not a message a subscriber can act on"),
		static_cast<int32>(Result->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	// The inner members first, then the placeholder standing in for the inner packing, which is delivered
	// exactly as a packed datagram standing on its own would be.
	TestEqual(*FString::Printf(TEXT("the inner members and the inner packing's placeholder (got %s)"),
		*Recorder.Describe()), Recorder.Received.Num(), 3);
	if (Recorder.Received.Num() == 3)
	{
		TestEqual(TEXT("the first inner member"), Recorder.Received[0], FString(BundlePingTypeName()));
		TestEqual(TEXT("the second inner member"), Recorder.Received[1], FString(BundleVoxelTypeName()));
		TestEqual(TEXT("the inner packing's placeholder"), Recorder.Received[2],
			FString(BundlePlaceholderTypeName()));
	}

	TestEqual(TEXT("everything delivered was counted as received"),
		Parsing.MessagesReceived() - CountedBefore, static_cast<int64>(3));

	Recorder.Release();
	return true;
}

// A stray byte behind the last member is too short to introduce another one, so it describes nothing and
// must not become a message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBundleTrailingByteTest,
	"CrowdySDK.Parser.BundleTrailingSingleByteIsIgnored",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyBundleTrailingByteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	FBundleRecorder Recorder;
	Recorder.WatchBundleMembers(Parsing.Registry);

	TArray<uint8> Bundle = OpenBundle();
	AppendBundleMember(Bundle, BundlePingMember(49));
	Bundle.Add(static_cast<uint8>(0x07));

	const int64 CountedBefore = Parsing.MessagesReceived();
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Result = Parsing.Parser->ParseMessage(Bundle);

	TestEqual(TEXT("the packing itself is not a message a subscriber can act on"),
		static_cast<int32>(Result->GetType()), static_cast<int32>(ECrowdyMessageType::BAD_MESSAGE));

	TestEqual(*FString::Printf(TEXT("only the whole member was dispatched (got %s)"), *Recorder.Describe()),
		Recorder.Received.Num(), 1);
	if (Recorder.Received.Num() == 1)
	{
		TestEqual(TEXT("and it is the member that was packed"), Recorder.Received[0],
			FString(BundlePingTypeName()));
	}

	TestEqual(TEXT("only the whole member was counted as received"),
		Parsing.MessagesReceived() - CountedBefore, static_cast<int64>(1));

	Recorder.Release();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
