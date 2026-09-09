#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Engine/GameInstance.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Actor/FActorLeftNotification.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "CrowdyCppVideo.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/Communication/FClientVideoNotification.h"
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

	/**
	 * The six octets in front of every video fragment's slice.
	 *
	 * Written here rather than taken from the decoder, so the test states the layout independently of the
	 * code that reads it. The frame id is big endian, unlike every other integer on this wire.
	 */
	inline void AppendVideoFragmentHeader(TArray<uint8>& Payload, const uint8 Codec, const uint16 FrameId,
		const uint8 FragmentIndex, const uint8 FragmentCount, const uint8 Version = CrowdyVideoFragment::Version)
	{
		Payload.Add(Version);
		Payload.Add(Codec);
		Payload.Add(static_cast<uint8>(FrameId >> 8));
		Payload.Add(static_cast<uint8>(FrameId & 0xff));
		Payload.Add(FragmentIndex);
		Payload.Add(FragmentCount);
	}

	/** One whole fragment: the header, then a body of Size octets whose first is Fill. */
	inline TArray<uint8> VideoFragment(const uint8 Codec, const uint16 FrameId, const uint8 FragmentIndex,
		const uint8 FragmentCount, const int32 Size, const uint8 Fill)
	{
		TArray<uint8> Fragment;
		AppendVideoFragmentHeader(Fragment, Codec, FrameId, FragmentIndex, FragmentCount);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Fragment.Add(static_cast<uint8>(Fill + Index));
		}
		return Fragment;
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
			// Every header field is given a value the struct does not default to, so a decoder that read
			// nothing would differ in all five rather than agree by coincidence. The codec is WebP because
			// the field defaults to Unknown, and the frame id is wide enough to need both its octets.
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION;
			Case.Name = TEXT("video fragment");
			AppendVideoFragmentHeader(Case.Payload, static_cast<uint8>(ECrowdyVideoCodec::WebP), 0x1234, 2, 5);
			Case.Payload.Append({0xf1, 0xf2, 0xf3});
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Video = static_cast<const FClientVideoNotification&>(Message);
				return FString::Printf(TEXT("%s codec=%u raw=%u frameId=%d fragment=%d/%d body=%s guid=%s"),
					*DescribeBase(Message), static_cast<uint8>(Video.Codec), Video.RawCodec, Video.FrameId,
					Video.FragmentIndex, Video.FragmentCount, *DescribeBytes(Video.BodyView),
					*Video.GUID.ToString());
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

		{
			// Carries the session-released reason rather than the stale one, so the reason actually read off the
			// frame differs from the value the message would hold if nothing decoded it.
			FInboundCase& Case = Cases.AddDefaulted_GetRef();
			Case.Type = ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION;
			Case.Name = TEXT("actor left");
			Case.Payload.Add(static_cast<uint8>(ECrowdyActorLeftReason::SessionReleased));
			Case.Describe = [](const ICrowdyMessage& Message)
			{
				const auto& Left = static_cast<const FActorLeftNotification&>(Message);
				return FString::Printf(TEXT("%s reason=%u raw=%u guid=%s"), *DescribeBase(Message),
					static_cast<uint8>(Left.Reason), Left.RawReason, *Left.GUID.ToString());
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

// The reason is one byte of a payload the server may omit entirely, and the protocol says every value but the
// reserved one means a plain stale drop. So the table below is the whole of what this SDK may claim to know,
// and reading it off a real datagram is what says the byte is taken from the payload rather than from a
// default that happens to agree with it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorLeftReasonNormalizationTest,
	"CrowdySDK.Transport.ActorLeftReasonNormalizesUnknownToStale",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyActorLeftReasonNormalizationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	struct FReasonCase
	{
		const TCHAR* Name;
		TArray<uint8> Payload;
		ECrowdyActorLeftReason Expected;
		uint8 ExpectedRaw;
	};

	const TArray<FReasonCase> Cases = {
		{ TEXT("a stale drop"), { 0 }, ECrowdyActorLeftReason::Stale, 0 },
		{ TEXT("a session the server ended"), { 1 }, ECrowdyActorLeftReason::SessionReleased, 1 },
		{ TEXT("a reason no version defines"), { 2 }, ECrowdyActorLeftReason::Stale, 2 },
		{ TEXT("the highest reason byte"), { 255 }, ECrowdyActorLeftReason::Stale, 255 },
		{ TEXT("no payload at all"), {}, ECrowdyActorLeftReason::Stale, 0 },
	};

	for (const FReasonCase& Case : Cases)
	{
		const TArray<uint8> Datagram = ExpectedSpatial(ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION,
			7, 11, -22, 33, Case.Payload, 8, 1, 1700000000123, 42);

		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Decoded = Parsing.Parser->ParseMessage(Datagram);

		if (!TestEqual(*FString::Printf(TEXT("%s decodes as an actor left notification"), Case.Name),
			static_cast<int32>(Decoded->GetType()),
			static_cast<int32>(ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION)))
		{
			continue;
		}

		const FActorLeftNotification& Left = static_cast<const FActorLeftNotification&>(Decoded.Get());

		TestEqual(*FString::Printf(TEXT("%s reads as the reason the protocol assigns it"), Case.Name),
			static_cast<int32>(Left.Reason), static_cast<int32>(Case.Expected));

		// The raw byte is what says an unrecognised reason was seen rather than replaced: two cases above
		// normalize to Stale from different bytes, and only this tells them apart.
		TestEqual(*FString::Printf(TEXT("%s keeps the byte exactly as it arrived"), Case.Name),
			static_cast<int32>(Left.RawReason), static_cast<int32>(Case.ExpectedRaw));

		// The envelope, from the same frame: a decoder that read the reason out of the wrong place would
		// have taken these from the wrong place too.
		TestEqual(*FString::Printf(TEXT("%s carries the chunk the actor was last seen in"), Case.Name),
			Left.ChunkX, static_cast<int64>(11));
		TestEqual(TEXT("and the rest of that chunk"), Left.ChunkY, static_cast<int64>(-22));
		TestEqual(TEXT("and its last axis"), Left.ChunkZ, static_cast<int64>(33));
		TestEqual(TEXT("and the server's timestamp"), Left.Timestamp, static_cast<int64>(1700000000123));
		TestEqual(TEXT("and the sequence number"), static_cast<int32>(Left.SequenceNumber), 42);
		TestEqual(TEXT("and the departing actor's id"), Left.UUID, FCrowdyActorId::FromOctets(GoldenUuidView()));
		TestTrue(TEXT("which reads back as a usable key"), Left.GUID.IsValid());
	}

	// The control: the same normalization asked of the shared reader directly, so a decoder that hard-coded
	// its answers instead of calling it would leave this pair disagreeing with the frames above.
	TestEqual(TEXT("an empty payload reads as stale"),
		static_cast<int32>(CrowdyActorLeftReasonFromPayload(TConstArrayView<uint8>())),
		static_cast<int32>(ECrowdyActorLeftReason::Stale));

	const uint8 SessionReleased[] = { 1 };
	TestEqual(TEXT("byte one is the session the server ended, not a stale drop"),
		static_cast<int32>(CrowdyActorLeftReasonFromPayload(SessionReleased)),
		static_cast<int32>(ECrowdyActorLeftReason::SessionReleased));

	// The control that keeps the line above meaning something: the wire format reserves 2 to 255 and requires
	// an unrecognised byte to read as a plain stale drop, so a decoder that simply passed the byte through
	// would answer with a reason this build cannot name.
	const uint8 Reserved[] = { 2 };
	TestEqual(TEXT("a reserved byte still reads as stale"),
		static_cast<int32>(CrowdyActorLeftReasonFromPayload(Reserved)),
		static_cast<int32>(ECrowdyActorLeftReason::Stale));

	const uint8 FarReserved[] = { 200 };
	TestEqual(TEXT("and so does one far up the reserved range"),
		static_cast<int32>(CrowdyActorLeftReasonFromPayload(FarReserved)),
		static_cast<int32>(ECrowdyActorLeftReason::Stale));

	return true;
}

// Opcode 145 has to be admitted by three separate gates before a consumer can subscribe to it, and each of
// them defaults to refusing an opcode it does not name. A miss in any one is silent: the frame simply never
// arrives, which looks exactly like a server that never sent it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorLeftIsRoutableTest,
	"CrowdySDK.Transport.ActorLeftIsDeliverableAndRoutable",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyActorLeftIsRoutableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	constexpr uint8 ActorLeftOpcode = static_cast<uint8>(ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION);
	TestEqual(TEXT("the actor left opcode is the one the protocol assigns"),
		static_cast<int32>(ActorLeftOpcode), 145);

	TestTrue(TEXT("an actor left notification is deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(ActorLeftOpcode));

	// The control on that gate: it is server-only, so admitting it inbound must not have admitted it outbound.
	TestFalse(TEXT("but it is not something a client may send"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(ActorLeftOpcode));

	// And the control on the gate itself, since a gate that answered true for everything would pass the line
	// above without admitting anything in particular.
	TestFalse(TEXT("a request opcode is still refused inbound"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(
			static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST)));

	// The routing gate. A subscription to an opcode the parser never produces is reported as dead and fires
	// nothing, so this is what makes the tracker's subscription real.
	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	int32 Received = 0;
	ECrowdyActorLeftReason SeenReason = ECrowdyActorLeftReason::Stale;
	FCrowdySubscription Subscription = Parsing.Registry.SubscribeToOpcode(
		ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("ActorLeftRoutingTest") },
		[&Received, &SeenReason](const FCrowdyDelivery& Delivery)
		{
			++Received;
			SeenReason = Delivery.GetAs<FActorLeftNotification>().Reason;
		});

	TArray<uint8> Payload;
	Payload.Add(static_cast<uint8>(ECrowdyActorLeftReason::SessionReleased));
	const TArray<uint8> Datagram = ExpectedSpatial(ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION,
		7, 1, 2, 3, Payload, 8, 1, 1700000000456, 5);

	Parsing.Registry.DispatchMessage(Parsing.Parser->ParseMessage(Datagram));

	TestEqual(TEXT("a subscriber on the actor left opcode is reached"), Received, 1);
	TestEqual(TEXT("and is handed the reason off the frame rather than the default"),
		static_cast<int32>(SeenReason), static_cast<int32>(ECrowdyActorLeftReason::SessionReleased));

	// The control on the routing: a frame this subscription does not name must not reach it, so the count
	// above is a match rather than a subscriber that receives everything.
	TArray<uint8> PingPayload;
	AppendInt64(PingPayload, 1699999999999);
	Parsing.Registry.DispatchMessage(Parsing.Parser->ParseMessage(
		ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3, PingPayload, 8, 1, 1700000000457, 6)));

	TestEqual(TEXT("and nothing else reaches it"), Received, 1);

	Subscription.Release();
	return true;
}

// Opcode 144 passes the same three gates opcode 145 does, and 143 is its outbound half. A miss in any one
// of them is silent: the fragment simply never arrives, which looks exactly like a camera nobody turned on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoIsRoutableTest,
	"CrowdySDK.Transport.VideoIsDeliverableAndRoutable",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoIsRoutableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	constexpr uint8 VideoNotification = static_cast<uint8>(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION);
	constexpr uint8 VideoPacket = static_cast<uint8>(ECrowdyMessageType::CLIENT_VIDEO_PACKET);

	TestEqual(TEXT("the video notification opcode is the one the protocol assigns"),
		static_cast<int32>(VideoNotification), 144);
	TestEqual(TEXT("and the video packet opcode is its outbound half"), static_cast<int32>(VideoPacket), 143);

	TestTrue(TEXT("a video notification is deliverable"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(VideoNotification));
	TestFalse(TEXT("but a notification is not something a client may send"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(VideoNotification));

	TestTrue(TEXT("a video packet is sendable"), FCrowdyCppReplication::IsSendableSpatialOpcode(VideoPacket));
	TestFalse(TEXT("but a packet is not something the server hands back"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(VideoPacket));

	// The controls on both gates. Without these a gate that answered true for everything would satisfy the
	// two lines above without admitting these opcodes in particular.
	TestFalse(TEXT("a request opcode is still refused inbound"),
		FCrowdyCppReplication::IsDeliverableSpatialOpcode(
			static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST)));
	TestFalse(TEXT("a server event is still refused outbound"),
		FCrowdyCppReplication::IsSendableSpatialOpcode(
			static_cast<uint8>(ECrowdyMessageType::SERVER_EVENT_NOTIFICATION)));

	// The routing gate. A subscription to an opcode the parser never produces is reported as dead and fires
	// nothing, so this is what makes a consumer's subscription real.
	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	int32 Received = 0;
	int32 SeenFrameId = -1;
	ECrowdyVideoCodec SeenCodec = ECrowdyVideoCodec::Jpeg;
	FCrowdySubscription Subscription = Parsing.Registry.SubscribeToOpcode(
		ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("VideoRoutingTest") },
		[&Received, &SeenFrameId, &SeenCodec](const FCrowdyDelivery& Delivery)
		{
			++Received;
			SeenFrameId = Delivery.GetAs<FClientVideoNotification>().FrameId;
			SeenCodec = Delivery.GetAs<FClientVideoNotification>().Codec;
		});

	TArray<uint8> Payload = VideoFragment(static_cast<uint8>(ECrowdyVideoCodec::WebP), 0x0501, 0, 1, 4, 0x70);
	Parsing.Registry.DispatchMessage(Parsing.Parser->ParseMessage(
		ExpectedSpatial(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION, 7, 1, 2, 3, Payload, 8, 1,
			1700000000456, 5)));

	TestEqual(TEXT("a subscriber on the video opcode is reached"), Received, 1);
	TestEqual(TEXT("and is handed the frame id off the header rather than the default"), SeenFrameId, 0x0501);
	TestEqual(TEXT("and the codec off the header rather than the default"),
		static_cast<int32>(SeenCodec), static_cast<int32>(ECrowdyVideoCodec::WebP));

	// The control on the routing: a frame this subscription does not name must not reach it.
	TArray<uint8> PingPayload;
	AppendInt64(PingPayload, 1699999999999);
	Parsing.Registry.DispatchMessage(Parsing.Parser->ParseMessage(
		ExpectedSpatial(ECrowdyMessageType::GENERIC_SPATIAL_1, 7, 1, 2, 3, PingPayload, 8, 1, 1700000000457, 6)));

	TestEqual(TEXT("and nothing else reaches it"), Received, 1);

	Subscription.Release();
	return true;
}

// The fragment header is the one part of a video frame this SDK reads, and it is described twice: once by
// the vendored contract and once by the message decoder, which cannot include a crowdy:: header. So the
// table below states what each field means at each offset, independently of both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoFragmentHeaderTest,
	"CrowdySDK.Transport.VideoFragmentHeaderDecodesAndRefusesWhatItMust",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoFragmentHeaderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	struct FHeaderCase
	{
		const TCHAR* Name;
		TArray<uint8> Payload;
		bool bDecodes = false;
		ECrowdyVideoCodec Codec = ECrowdyVideoCodec::Jpeg;
		int32 FrameId = 0;
		int32 Index = 0;
		int32 Count = 0;
		int32 BodySize = 0;
	};

	TArray<FHeaderCase> Cases;

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a jpeg fragment");
		Case.Payload = VideoFragment(0, 1, 0, 1, 5, 0x10);
		Case.bDecodes = true;
		Case.Codec = ECrowdyVideoCodec::Jpeg;
		Case.FrameId = 1;
		Case.Count = 1;
		Case.BodySize = 5;
	}

	{
		// Both octets of the frame id are significant and they are big endian, so a reader that swapped
		// them would report 0x3412 here rather than 0x1234.
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a webp fragment late in a frame");
		Case.Payload = VideoFragment(1, 0x1234, 15, 16, 7, 0x20);
		Case.bDecodes = true;
		Case.Codec = ECrowdyVideoCodec::WebP;
		Case.FrameId = 0x1234;
		Case.Index = 15;
		Case.Count = 16;
		Case.BodySize = 7;
	}

	{
		// The forward-compatibility case: a codec byte no version assigns. It is refused rather than
		// carried, because nothing downstream could decode the bytes behind it.
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a codec no version assigns");
		Case.Payload = VideoFragment(2, 1, 0, 1, 5, 0x30);
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("the highest codec byte");
		Case.Payload = VideoFragment(255, 1, 0, 1, 5, 0x30);
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a header version this build does not write");
		Case.Payload.Reset();
		AppendVideoFragmentHeader(Case.Payload, 0, 1, 0, 1, /*Version*/ 2);
		Case.Payload.Append({0x40, 0x41});
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a frame claiming more fragments than one may cross as");
		Case.Payload = VideoFragment(0, 1, 0, CrowdyVideoFragment::MaxFragments + 1, 5, 0x50);
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a frame of no fragments at all");
		Case.Payload = VideoFragment(0, 1, 0, 0, 5, 0x50);
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("an index at or past the count");
		Case.Payload = VideoFragment(0, 1, 3, 3, 5, 0x60);
	}

	{
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a payload too short to hold a header");
		Case.Payload = { CrowdyVideoFragment::Version, 0, 0, 1, 0 };
	}

	{
		// A header and nothing behind it. The split never writes one, since a frame that divides exactly
		// ends on a full fragment rather than an extra empty one, so this is a broken or hostile peer. It
		// is refused because with a fragment count of one it would otherwise complete a zero octet frame
		// and hand it to whatever decodes images.
		FHeaderCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("a header with no body behind it");
		Case.Payload = VideoFragment(0, 9, 0, 1, 0, 0);
	}


	for (const FHeaderCase& Case : Cases)
	{
		const TArray<uint8> Datagram = ExpectedSpatial(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION,
			7, 11, -22, 33, Case.Payload, 8, 1, 1700000000123, 42);

		const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Decoded = Parsing.Parser->ParseMessage(Datagram);
		const bool bDecoded = Decoded->GetType() == ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION;

		if (!TestEqual(*FString::Printf(TEXT("%s is %s"), Case.Name,
			Case.bDecodes ? TEXT("decoded") : TEXT("refused")), bDecoded, Case.bDecodes))
		{
			continue;
		}

		if (!Case.bDecodes)
		{
			continue;
		}

		const FClientVideoNotification& Video = static_cast<const FClientVideoNotification&>(Decoded.Get());

		TestEqual(*FString::Printf(TEXT("%s names its codec"), Case.Name),
			static_cast<int32>(Video.Codec), static_cast<int32>(Case.Codec));
		TestEqual(*FString::Printf(TEXT("%s keeps the codec byte as it arrived"), Case.Name),
			static_cast<int32>(Video.RawCodec), static_cast<int32>(Case.Codec));
		TestEqual(*FString::Printf(TEXT("%s reads its frame id from both octets"), Case.Name),
			Video.FrameId, Case.FrameId);
		TestEqual(*FString::Printf(TEXT("%s reads its fragment index"), Case.Name),
			Video.FragmentIndex, Case.Index);
		TestEqual(*FString::Printf(TEXT("%s reads its fragment count"), Case.Name),
			Video.FragmentCount, Case.Count);
		TestEqual(*FString::Printf(TEXT("%s carries the body behind the header"), Case.Name),
			Video.BodyView.Num(), Case.BodySize);
		TestEqual(*FString::Printf(TEXT("%s carries the whole fragment for the assembler"), Case.Name),
			Video.FragmentView.Num(), Case.BodySize + CrowdyVideoFragment::HeaderBytes);

		// The envelope, from the same frame: a decoder that read the header from the wrong place would
		// have taken these from the wrong place too.
		TestEqual(TEXT("and the chunk the fragment was broadcast over"), Video.ChunkX, static_cast<int64>(11));
		TestEqual(TEXT("and the rest of that chunk"), Video.ChunkY, static_cast<int64>(-22));
		TestEqual(TEXT("and its last axis"), Video.ChunkZ, static_cast<int64>(33));
		TestEqual(TEXT("and the server's timestamp"), Video.Timestamp, static_cast<int64>(1700000000123));
		TestEqual(TEXT("and the sequence number"), static_cast<int32>(Video.SequenceNumber), 42);
		TestEqual(TEXT("and the sending actor's id"), Video.UUID, FCrowdyActorId::FromOctets(GoldenUuidView()));
		TestTrue(TEXT("which reads back as a usable key"), Video.GUID.IsValid());
	}

	// The body's upper bound, asked of the decoder directly rather than through a datagram: a payload over
	// this size does not fit one, so routing it through the encoder would say only that the encoder refused
	// it. What is being pinned is that the decoder measures the body at all.
	{
		const auto DecodeBody = [](const int32 BodySize) -> bool
		{
			TArray<uint8> Payload = VideoFragment(0, 9, 0, 1, BodySize, 0x90);

			FCrowdyFrame Frame;
			Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION);
			Frame.Body = Payload;
			Frame.Envelope.Uuid = GoldenUuidView();
			Frame.bHasEnvelope = true;

			FClientVideoNotification Video;
			return Video.DecodePayload(Frame);
		};

		TestFalse(TEXT("a body larger than one datagram carries is refused"),
			DecodeBody(CrowdyVideoFragment::MaxBodyBytes + 1));

		// The control: one octet less is the largest a fragment may carry, and it is accepted, so a decoder
		// that refused every body would not read as measuring one.
		TestTrue(TEXT("while exactly the largest one carries is accepted"),
			DecodeBody(CrowdyVideoFragment::MaxBodyBytes));
	}

	// The control on the codec mapping, asked of the shared reader directly, so a decoder that hard-coded
	// its answers would leave this disagreeing with the frames above.
	TestEqual(TEXT("codec byte 0 is jpeg"), static_cast<int32>(CrowdyVideoCodecFromByte(0)),
		static_cast<int32>(ECrowdyVideoCodec::Jpeg));
	TestEqual(TEXT("codec byte 1 is webp"), static_cast<int32>(CrowdyVideoCodecFromByte(1)),
		static_cast<int32>(ECrowdyVideoCodec::WebP));
	TestEqual(TEXT("codec byte 2 is one no version assigns"), static_cast<int32>(CrowdyVideoCodecFromByte(2)),
		static_cast<int32>(ECrowdyVideoCodec::Unknown));
	TestEqual(TEXT("and so is the highest codec byte"), static_cast<int32>(CrowdyVideoCodecFromByte(255)),
		static_cast<int32>(ECrowdyVideoCodec::Unknown));

	return true;
}

// The header offsets are the one part of the contract nothing can assert at compile time: the vendored
// reader names them as literal indices, so there is no constant for the message decoder's mirror to be
// checked against. The test above states the layout independently, but both it and the decoder are written
// here, so a change made consistently to the pair would drift from the other SDK unnoticed. This compares
// the mirror against the vendored writer instead, which is a second source rather than a second opinion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoFragmentOffsetsTest,
	"CrowdySDK.Transport.VideoFragmentOffsetsMatchTheVendoredSplit",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoFragmentOffsetsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	FParserFixture Parsing;
	if (!TestTrue(TEXT("a parser and the session it reads were created"), Parsing.Open()))
	{
		return false;
	}

	// Both octets of the frame id are significant and unequal, so a mirror that swapped them reads 0x0201.
	constexpr int32 FrameId = 0x0102;
	constexpr int32 TailBodySize = 5;
	constexpr uint8 Codec = static_cast<uint8>(ECrowdyVideoCodec::WebP);

	// Long enough to need a second fragment, so the index and the count are different numbers and a mirror
	// that read one at the other's offset would be visible.
	TArray<uint8> Image;
	Image.SetNumZeroed(FCrowdyCppVideoAssembler::MaxFragmentBodyBytes + TailBodySize);

	TArray<TArray<uint8>> Fragments;
	if (!TestTrue(TEXT("the vendored split produced fragments"),
		FCrowdyCppVideoAssembler::FragmentFrame(Image, FrameId, Codec, Fragments))
		|| !TestEqual(TEXT("two of them"), Fragments.Num(), 2))
	{
		return false;
	}

	const TArray<uint8>& Tail = Fragments[1];
	if (!TestEqual(TEXT("the second is the header and what the first did not carry"), Tail.Num(),
		CrowdyVideoFragment::HeaderBytes + TailBodySize))
	{
		return false;
	}

	// Field by field, at the offset the message decoder reads each from.
	TestEqual(TEXT("the version sits where the decoder reads it"),
		static_cast<int32>(Tail[CrowdyVideoFragment::VersionOffset]),
		static_cast<int32>(CrowdyVideoFragment::Version));
	TestEqual(TEXT("and the codec where the decoder reads it"),
		static_cast<int32>(Tail[CrowdyVideoFragment::CodecOffset]), static_cast<int32>(Codec));
	TestEqual(TEXT("and the frame id's high octet first"),
		static_cast<int32>(Tail[CrowdyVideoFragment::FrameIdOffset]), 0x01);
	TestEqual(TEXT("and its low octet second"),
		static_cast<int32>(Tail[CrowdyVideoFragment::FrameIdOffset + 1]), 0x02);
	TestEqual(TEXT("and the fragment index where the decoder reads it"),
		static_cast<int32>(Tail[CrowdyVideoFragment::IndexOffset]), 1);
	TestEqual(TEXT("and the fragment count where the decoder reads it"),
		static_cast<int32>(Tail[CrowdyVideoFragment::CountOffset]), 2);

	// And through the decoder itself, so the offsets above are what it actually uses rather than what it is
	// documented to use.
	const auto Decode = [&Parsing](const TArray<uint8>& Fragment) -> TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>
	{
		return Parsing.Parser->ParseMessage(ExpectedSpatial(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION,
			7, 11, -22, 33, Fragment, 8, 1, 1700000000123, 42));
	};

	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> DecodedTail = Decode(Tail);
	if (!TestEqual(TEXT("the split's own fragment decodes"),
		static_cast<int32>(DecodedTail->GetType()),
		static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION)))
	{
		return false;
	}

	const FClientVideoNotification& Tailed = static_cast<const FClientVideoNotification&>(DecodedTail.Get());
	TestEqual(TEXT("under the frame id it was split with, both octets"), Tailed.FrameId, FrameId);
	TestEqual(TEXT("as the fragment it is"), Tailed.FragmentIndex, 1);
	TestEqual(TEXT("of the count it was split into"), Tailed.FragmentCount, 2);
	TestEqual(TEXT("under the codec it was split with"), static_cast<int32>(Tailed.Codec),
		static_cast<int32>(ECrowdyVideoCodec::WebP));
	TestEqual(TEXT("carrying what the first fragment could not"), Tailed.BodyView.Num(), TailBodySize);

	// The control: the other fragment of the same frame reads as the other fragment, so an index and a count
	// that were both hard-coded would not satisfy the pair.
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> DecodedHead = Decode(Fragments[0]);
	if (TestEqual(TEXT("the first fragment decodes too"), static_cast<int32>(DecodedHead->GetType()),
		static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION)))
	{
		const FClientVideoNotification& Headed =
			static_cast<const FClientVideoNotification&>(DecodedHead.Get());
		TestEqual(TEXT("under the same frame id"), Headed.FrameId, FrameId);
		TestEqual(TEXT("as the fragment before it"), Headed.FragmentIndex, 0);
		TestEqual(TEXT("filled to what one fragment carries"), Headed.BodyView.Num(),
			FCrowdyCppVideoAssembler::MaxFragmentBodyBytes);
	}

	return true;
}

// A single notification is one slice of an image, so everything a consumer wants happens in the assembler.
// Its rules are shared with the browser SDK byte for byte, and each of them is a way for a frame to never
// arrive while the wire looks healthy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReassemblyTest,
	"CrowdySDK.Transport.VideoFramesReassembleAcrossFragments",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoReassemblyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;
	using namespace CrowdyReceiveTestSupport;

	const TArrayView<const uint8> Sender = GoldenUuidView();

	FCrowdyCppVideoAssembler Assembler;
	FCrowdyCppAssembledVideoFrame Frame;

	// Delivered out of order, because the transport gives no ordering guarantee and a reassembler that
	// only ever concatenated in arrival order would pass an in-order test and produce a scrambled image.
	const TArray<uint8> Second = VideoFragment(1, 77, 1, 3, 4, 0xb0);
	const TArray<uint8> Third = VideoFragment(1, 77, 2, 3, 2, 0xc0);
	const TArray<uint8> First = VideoFragment(1, 77, 0, 3, 3, 0xa0);

	TestFalse(TEXT("one fragment of three completes nothing"), Assembler.Ingest(Sender, Second, 1000, Frame));
	TestFalse(TEXT("nor do two"), Assembler.Ingest(Sender, Third, 1010, Frame));
	TestEqual(TEXT("and the sender is holding a frame in the meantime"), Assembler.PendingSenders(), 1);

	if (!TestTrue(TEXT("the last fragment completes the frame"), Assembler.Ingest(Sender, First, 1020, Frame)))
	{
		return false;
	}

	TestEqual(TEXT("the frame is the three bodies in index order"), Frame.Bytes.Num(), 3 + 4 + 2);
	const TArray<uint8> Expected = { 0xa0, 0xa1, 0xa2, 0xb0, 0xb1, 0xb2, 0xb3, 0xc0, 0xc1 };
	TestEqual(TEXT("and byte for byte it is what the sender split"), DescribeBytes(Frame.Bytes),
		DescribeBytes(Expected));
	TestEqual(TEXT("under the sender's own frame id"), Frame.FrameId, 77);
	TestEqual(TEXT("and the codec every fragment named"), static_cast<int32>(Frame.Codec), 1);
	TestEqual(TEXT("stamped with the clock the last fragment arrived on"), Frame.CompletedAtMs,
		static_cast<int64>(1020));
	TestEqual(TEXT("and addressed to the sender it came from"), Frame.SenderUuid.Num(), 32);
	TestEqual(TEXT("nothing is left half assembled"), Assembler.PendingSenders(), 0);

	// A straggler from a frame already delivered is dropped rather than starting that frame again, which
	// is what stops a lost-and-late fragment resurrecting an image the consumer has already shown.
	const int64 DroppedBefore = Assembler.GetDroppedFragments();
	TestFalse(TEXT("a straggler from the completed frame completes nothing"),
		Assembler.Ingest(Sender, First, 1030, Frame));
	TestEqual(TEXT("and is counted as dropped"), Assembler.GetDroppedFragments(), DroppedBefore + 1);
	TestEqual(TEXT("and starts nothing"), Assembler.PendingSenders(), 0);

	// The timeout. A sender that stops halfway leaves a frame that will never complete, and only the sweep
	// reclaims it: nothing else is going to arrive to trigger the newer-frame rule.
	TestFalse(TEXT("a new frame starts"), Assembler.Ingest(Sender,
		VideoFragment(0, 78, 0, 2, 3, 0xd0), 2000, Frame));
	TestEqual(TEXT("and is held"), Assembler.PendingSenders(), 1);
	TestEqual(TEXT("a sweep inside the timeout abandons nothing"),
		Assembler.Prune(2000 + FCrowdyCppVideoAssembler::DefaultFrameTimeoutMs), 0);
	TestEqual(TEXT("and the frame is still held"), Assembler.PendingSenders(), 1);
	TestEqual(TEXT("a sweep past the timeout abandons it"),
		Assembler.Prune(2001 + FCrowdyCppVideoAssembler::DefaultFrameTimeoutMs), 1);
	TestEqual(TEXT("and nothing is held afterwards"), Assembler.PendingSenders(), 0);

	// Forget, which is the departure path. It has to drop a partial frame that no timeout has reached yet.
	TestFalse(TEXT("another frame starts"), Assembler.Ingest(Sender,
		VideoFragment(0, 90, 0, 2, 3, 0xe0), 3000, Frame));
	TestEqual(TEXT("and is held"), Assembler.PendingSenders(), 1);

	const int64 AbandonedBefore = Assembler.GetAbandonedFrames();
	Assembler.Forget(Sender);
	TestEqual(TEXT("forgetting the sender drops it"), Assembler.PendingSenders(), 0);
	TestEqual(TEXT("and counts it abandoned"), Assembler.GetAbandonedFrames(), AbandonedBefore + 1);

	// The control on Forget: a sender id it was never given must not disturb the one it was. Without this
	// a Forget that cleared everything would satisfy the line above.
	TestFalse(TEXT("a third frame starts"), Assembler.Ingest(Sender,
		VideoFragment(0, 91, 0, 2, 3, 0xf0), 4000, Frame));
	// Asserted before the forget below, so a failure there names which of the two happened: the frame never
	// being held, or the forget reaching a sender it was not given.
	TestEqual(TEXT("and is held"), Assembler.PendingSenders(), 1);

	// A genuinely different id. The golden uuid these tests send from is "0123456789abcdef" twice over, so an
	// id spelled that way is the same sender and the forget below would legitimately clear it.
	const uint8 OtherSenderOctets[32] = {
		'f', 'e', 'd', 'c', 'b', 'a', '9', '8', '7', '6', '5', '4', '3', '2', '1', '0',
		'f', 'e', 'd', 'c', 'b', 'a', '9', '8', '7', '6', '5', '4', '3', '2', '1', '0' };
	const TArrayView<const uint8> OtherSender(OtherSenderOctets, 32);
	if (!TestNotEqual(TEXT("the other sender really is a different id"),
		DescribeBytes(TArray<uint8>(OtherSender)), DescribeBytes(TArray<uint8>(Sender))))
	{
		return false;
	}

	Assembler.Forget(OtherSender);
	TestEqual(TEXT("forgetting a different sender leaves it alone"), Assembler.PendingSenders(), 1);

	return true;
}

// The refusal that matters most on the send side, because it is the one whose failure mode is invisible:
// an over-large frame that went out in part would arrive as an image that never completes, and would be
// read months later as packet loss.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoFragmentationLimitTest,
	"CrowdySDK.Transport.VideoFrameRefusesToSplitPastTheFragmentLimit",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyVideoFragmentationLimitTest::RunTest(const FString& Parameters)
{
	constexpr int32 MaxBody = FCrowdyCppVideoAssembler::MaxFragmentBodyBytes;
	constexpr int32 MaxFragments = FCrowdyCppVideoAssembler::MaxFragments;

	TArray<TArray<uint8>> Fragments;

	TArray<uint8> Empty;
	TestFalse(TEXT("an empty frame splits into nothing"),
		FCrowdyCppVideoAssembler::FragmentFrame(Empty, 1, 0, Fragments));
	TestEqual(TEXT("and writes no fragments"), Fragments.Num(), 0);

	TArray<uint8> Largest;
	Largest.SetNumZeroed(MaxBody * MaxFragments);
	if (!TestTrue(TEXT("the largest frame that fits splits"),
		FCrowdyCppVideoAssembler::FragmentFrame(Largest, 1, 0, Fragments)))
	{
		return false;
	}
	TestEqual(TEXT("into exactly the fragment limit"), Fragments.Num(), MaxFragments);
	TestEqual(TEXT("each carrying a header and a full body"), Fragments[0].Num(),
		MaxBody + FCrowdyCppVideoAssembler::FragmentHeaderBytes);

	// One octet past it, which is the boundary the refusal is stated at.
	TArray<uint8> TooLarge;
	TooLarge.SetNumZeroed(MaxBody * MaxFragments + 1);
	TestFalse(TEXT("one octet more is refused"),
		FCrowdyCppVideoAssembler::FragmentFrame(TooLarge, 1, 0, Fragments));
	TestEqual(TEXT("and nothing at all is written for it"), Fragments.Num(), 0);

	// Round trip: what the split writes is what the assembler reads, which is the only thing that says the
	// two halves of this SDK agree about the header they share.
	TArray<uint8> Frame;
	for (int32 Index = 0; Index < MaxBody + 40; ++Index)
	{
		Frame.Add(static_cast<uint8>(Index % 251));
	}

	if (!TestTrue(TEXT("a frame over one datagram splits"),
		FCrowdyCppVideoAssembler::FragmentFrame(Frame, 0x2222, 1, Fragments)))
	{
		return false;
	}
	TestEqual(TEXT("into two fragments"), Fragments.Num(), 2);

	FCrowdyCppVideoAssembler Assembler;
	FCrowdyCppAssembledVideoFrame Assembled;
	bool bCompleted = false;
	for (const TArray<uint8>& Fragment : Fragments)
	{
		bCompleted = Assembler.Ingest(CrowdyReplicationTestSupport::GoldenUuidView(), Fragment, 500, Assembled);
	}

	if (!TestTrue(TEXT("and the fragments reassemble into a frame"), bCompleted))
	{
		return false;
	}
	TestEqual(TEXT("of the size that went in"), Assembled.Bytes.Num(), Frame.Num());
	TestEqual(TEXT("byte for byte"), CrowdyReceiveTestSupport::DescribeBytes(Assembled.Bytes),
		CrowdyReceiveTestSupport::DescribeBytes(Frame));
	TestEqual(TEXT("under the frame id it was split with"), Assembled.FrameId, 0x2222);
	TestEqual(TEXT("and the codec it was split with"), static_cast<int32>(Assembled.Codec), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
