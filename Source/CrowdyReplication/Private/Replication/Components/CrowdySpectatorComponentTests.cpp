#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Replication/Components/CrowdySpectatorComponent.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"

namespace
{
	constexpr EAutomationTestFlags SpectatorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString SpectatorTestUuid = TEXT("0123456789ABCDEF0123456789ABCDEF");

	struct FSpectatorTestSink
	{
		TArray<CrowdySpectator::FBeat> Beats;
		FString Uuid = SpectatorTestUuid;
		bool bAccepts = true;

		void Bind(CrowdySpectator::FCore& Core)
		{
			Core.ResolveUuid = [this]() { return Uuid; };
			Core.Send = [this](const CrowdySpectator::FBeat& Beat)
			{
				if (!bAccepts)
				{
					return false;
				}
				Beats.Add(Beat);
				return true;
			};
		}
	};

	class FSpectatorTestNetWarnings : public FOutputDevice
	{
	public:
		FSpectatorTestNetWarnings() { GLog->AddOutputDevice(this); }
		virtual ~FSpectatorTestNetWarnings() override { GLog->RemoveOutputDevice(this); }

		virtual void Serialize(const TCHAR*, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity <= ELogVerbosity::Warning && Category == LogCrowdyNet.GetCategoryName())
			{
				++Count;
			}
		}

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		int32 Count = 0;
	};

	struct FSpectatorTestFrame
	{
		FTCHARToUTF8 UuidText;
		TArray<uint8> Body;
		FCrowdyFrame Frame;

		explicit FSpectatorTestFrame(const FString& Uuid, const TArray<uint8>& InBody)
			: UuidText(*Uuid)
			, Body(InBody)
			, Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), TConstArrayView<uint8>(Body))
		{
			Frame.Envelope.Uuid = TConstArrayView<uint8>(reinterpret_cast<const uint8*>(UuidText.Get()), UuidText.Length());
			Frame.Envelope.ChunkX = 3;
			Frame.Envelope.ChunkY = -4;
			Frame.Envelope.ChunkZ = 5;
			Frame.bHasEnvelope = true;
		}
	};

	TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> SpectatorTestMessage(const FGuid& Key, const bool bViewer)
	{
		TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
			MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->GUID = Key;
		Message->bViewerBeat = bViewer;
		return Message;
	}
}

// A request below 0.25 s is raised to it, and the property starts at the platform's 2 s.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorIntervalFloorTest,
	"CrowdySDK.Replication.Spectator.IntervalFloor", SpectatorTestFlags)
bool FCrowdySpectatorIntervalFloorTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a tenth of a second is raised to the floor"), CrowdySpectator::ClampInterval(0.1f), 0.25f);
	TestEqual(TEXT("zero is raised to the floor"), CrowdySpectator::ClampInterval(0.0f), 0.25f);
	TestEqual(TEXT("a negative value is raised to the floor"), CrowdySpectator::ClampInterval(-3.0f), 0.25f);
	TestEqual(TEXT("the floor itself stands"), CrowdySpectator::ClampInterval(0.25f), 0.25f);
	TestEqual(TEXT("two seconds stands"), CrowdySpectator::ClampInterval(2.0f), 2.0f);

	const TStrongObjectPtr<UCrowdySpectatorComponent> Component(NewObject<UCrowdySpectatorComponent>(GetTransientPackage()));
	TestEqual(TEXT("the default interval is two seconds"), Component->Interval, 2.0f);
	TestEqual(TEXT("the default distance is eight chunks"), Component->Distance, 8);
	TestTrue(TEXT("it starts by itself"), Component->bAutoStart);
	return true;
}

// The distance is 0 to 8 both ways, and the beat carries the clamped value whatever the property holds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorDistanceClampTest,
	"CrowdySDK.Replication.Spectator.DistanceClamp", SpectatorTestFlags)
bool FCrowdySpectatorDistanceClampTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("below zero is zero"), CrowdySpectator::ClampDistance(-3), 0);
	TestEqual(TEXT("zero stands"), CrowdySpectator::ClampDistance(0), 0);
	TestEqual(TEXT("eight stands"), CrowdySpectator::ClampDistance(8), 8);
	TestEqual(TEXT("nine is eight"), CrowdySpectator::ClampDistance(9), 8);
	TestEqual(TEXT("a large value is eight"), CrowdySpectator::ClampDistance(1000), 8);

	FSpectatorTestSink Sink;
	CrowdySpectator::FCore Core;
	Sink.Bind(Core);
	Core.Distance = 99;
	Core.Start(FInt64Vector(0, 0, 0));
	if (!TestEqual(TEXT("one beat went out"), Sink.Beats.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("it carries the clamped distance"), Sink.Beats[0].Distance, 8);
	return true;
}

// Start beats at once, with the chunk it was given and the id and distance of the viewer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorBeatsAtOnceOnStartTest,
	"CrowdySDK.Replication.Spectator.BeatsAtOnceOnStart", SpectatorTestFlags)
bool FCrowdySpectatorBeatsAtOnceOnStartTest::RunTest(const FString& Parameters)
{
	FSpectatorTestSink Sink;
	CrowdySpectator::FCore Core;
	Sink.Bind(Core);
	Core.Distance = 3;

	TestFalse(TEXT("it is not running before Start"), Core.IsRunning());
	Core.Start(FInt64Vector(7, -2, 1));

	TestTrue(TEXT("it is running after Start"), Core.IsRunning());
	if (!TestEqual(TEXT("Start beat once, at once"), Sink.Beats.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("at the chunk it was started in"), Sink.Beats[0].Chunk == FInt64Vector(7, -2, 1));
	TestEqual(TEXT("under the viewer's id"), Sink.Beats[0].Uuid, SpectatorTestUuid);
	TestEqual(TEXT("at the distance asked"), Sink.Beats[0].Distance, 3);
	TestFalse(TEXT("and owes nothing"), Core.IsOwingBeat());
	return true;
}

// A move into another chunk beats at once from the new chunk; staying in a chunk does not, and the periodic beat
// keeps the chunk it last saw.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorBeatsOnChunkChangeOnlyTest,
	"CrowdySDK.Replication.Spectator.BeatsOnChunkChangeOnly", SpectatorTestFlags)
bool FCrowdySpectatorBeatsOnChunkChangeOnlyTest::RunTest(const FString& Parameters)
{
	FSpectatorTestSink Sink;
	CrowdySpectator::FCore Core;
	Sink.Bind(Core);
	Core.Start(FInt64Vector(1, 1, 0));

	TestFalse(TEXT("staying in the chunk asks for no beat"), Core.Watch(FInt64Vector(1, 1, 0)));
	TestEqual(TEXT("and sends none"), Sink.Beats.Num(), 1);

	TestTrue(TEXT("moving into another chunk beats"), Core.Watch(FInt64Vector(2, 1, 0)));
	if (!TestEqual(TEXT("and sends at once"), Sink.Beats.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("from the new chunk"), Sink.Beats[1].Chunk == FInt64Vector(2, 1, 0));

	TestFalse(TEXT("the new chunk is the chunk now"), Core.Watch(FInt64Vector(2, 1, 0)));
	TestEqual(TEXT("so staying there sends none"), Sink.Beats.Num(), 2);

	TestTrue(TEXT("a move along the other axis counts too"), Core.Watch(FInt64Vector(2, 1, 1)));
	TestEqual(TEXT("and sends"), Sink.Beats.Num(), 3);

	Core.Pulse();
	if (!TestEqual(TEXT("the periodic beat sends"), Sink.Beats.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("from the last chunk seen"), Sink.Beats[3].Chunk == FInt64Vector(2, 1, 1));
	return true;
}

// After Stop nothing goes out, whichever way a beat is asked for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorSilentAfterStopTest,
	"CrowdySDK.Replication.Spectator.SilentAfterStop", SpectatorTestFlags)
bool FCrowdySpectatorSilentAfterStopTest::RunTest(const FString& Parameters)
{
	FSpectatorTestSink Sink;
	CrowdySpectator::FCore Core;
	Sink.Bind(Core);
	Core.Start(FInt64Vector(0, 0, 0));
	Core.Stop();

	Core.Pulse();
	TestFalse(TEXT("a move after Stop asks for nothing"), Core.Watch(FInt64Vector(9, 9, 9)));
	TestFalse(TEXT("it is not running"), Core.IsRunning());
	TestEqual(TEXT("only the beat from Start went out"), Sink.Beats.Num(), 1);

	Core.Start(FInt64Vector(0, 0, 0));
	TestEqual(TEXT("it can be started again"), Sink.Beats.Num(), 2);
	return true;
}

// A beat that cannot go out (no identity yet, or no connection) is owed, and goes at the next Watch without waiting
// a whole interval or for the viewer to move.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorOwedBeatGoesWhenPossibleTest,
	"CrowdySDK.Replication.Spectator.OwedBeatGoesWhenPossible", SpectatorTestFlags)
bool FCrowdySpectatorOwedBeatGoesWhenPossibleTest::RunTest(const FString& Parameters)
{
	FSpectatorTestSink Sink;
	CrowdySpectator::FCore Core;
	Sink.Bind(Core);

	Sink.Uuid.Reset();
	Core.Start(FInt64Vector(4, 4, 4));
	TestTrue(TEXT("no identity yet owes the beat"), Core.IsOwingBeat());
	TestEqual(TEXT("and sends nothing"), Sink.Beats.Num(), 0);

	TestTrue(TEXT("the next watch tries again"), Core.Watch(FInt64Vector(4, 4, 4)));
	TestTrue(TEXT("still owing while there is no identity"), Core.IsOwingBeat());

	Sink.Uuid = SpectatorTestUuid;
	Sink.bAccepts = false;
	TestTrue(TEXT("with an identity but no connection"), Core.Watch(FInt64Vector(4, 4, 4)));
	TestTrue(TEXT("it is still owed"), Core.IsOwingBeat());

	Sink.bAccepts = true;
	TestTrue(TEXT("once it can go, it goes"), Core.Watch(FInt64Vector(4, 4, 4)));
	if (!TestEqual(TEXT("exactly once"), Sink.Beats.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("from the chunk it was started in"), Sink.Beats[0].Chunk == FInt64Vector(4, 4, 4));
	TestFalse(TEXT("and owes nothing"), Core.IsOwingBeat());
	TestFalse(TEXT("so the next watch in the same chunk is quiet"), Core.Watch(FInt64Vector(4, 4, 4)));
	return true;
}

// A beat the connection did not accept is not a beat sent: it stays owed, so realtime events do not stop for a whole interval.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorDroppedSendStaysOwedTest,
	"CrowdySDK.Replication.Spectator.DroppedSendStaysOwed", SpectatorTestFlags)
bool FCrowdySpectatorDroppedSendStaysOwedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("accepted counts as sent"), CrowdySpectator::CountsAsSent(ECrowdyCppSendOutcome::Sent));
	TestFalse(TEXT("no connection does not"), CrowdySpectator::CountsAsSent(ECrowdyCppSendOutcome::NotRouted));
	TestFalse(TEXT("nor does a refusal"), CrowdySpectator::CountsAsSent(ECrowdyCppSendOutcome::Refused));

	ECrowdyCppSendOutcome Outcome = ECrowdyCppSendOutcome::NotRouted;
	int32 Offered = 0;
	CrowdySpectator::FCore Core;
	Core.ResolveUuid = []() { return SpectatorTestUuid; };
	Core.Send = [&Outcome, &Offered](const CrowdySpectator::FBeat&)
	{
		++Offered;
		return CrowdySpectator::CountsAsSent(Outcome);
	};

	Core.Start(FInt64Vector(0, 0, 0));
	TestTrue(TEXT("a beat the connection dropped is owed"), Core.IsOwingBeat());

	Outcome = ECrowdyCppSendOutcome::Sent;
	TestTrue(TEXT("the next watch offers it again"), Core.Watch(FInt64Vector(0, 0, 0)));
	TestEqual(TEXT("having offered it twice"), Offered, 2);
	TestFalse(TEXT("and once accepted it is no longer owed"), Core.IsOwingBeat());
	return true;
}

// What reaches the wire: the header for the beat's chunk, distance and id, and no state, not even a length.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorBeatIsAnEmptyActorUpdateTest,
	"CrowdySDK.Replication.Spectator.BeatIsAnEmptyActorUpdate", SpectatorTestFlags)
bool FCrowdySpectatorBeatIsAnEmptyActorUpdateTest::RunTest(const FString& Parameters)
{
	CrowdySpectator::FBeat Beat;
	Beat.Chunk = FInt64Vector(11, -12, 13);
	Beat.Uuid = SpectatorTestUuid;
	Beat.Distance = 5;

	FActorUpdateRequestMessage Message;
	CrowdySpectator::FillBeatMessage(Beat, 77, Message);

	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;
	if (!TestTrue(TEXT("the beat splits into a frame"), CrowdyCppSend::SplitMessage(Message, Storage, Frame, Error)))
	{
		return false;
	}

	TestEqual(TEXT("it is an actor update"), static_cast<int32>(Frame.Opcode), static_cast<int32>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST));
	TestEqual(TEXT("chunk x"), Frame.ChunkX, static_cast<int64>(11));
	TestEqual(TEXT("chunk y"), Frame.ChunkY, static_cast<int64>(-12));
	TestEqual(TEXT("chunk z"), Frame.ChunkZ, static_cast<int64>(13));
	TestEqual(TEXT("distance"), static_cast<int32>(Frame.Distance), 5);
	TestEqual(TEXT("app"), Frame.AppId, static_cast<int64>(77));
	TestEqual(TEXT("no state and no length"), Frame.Payload.Num(), 0);

	const FTCHARToUTF8 Expected(*SpectatorTestUuid);
	TestTrue(TEXT("under the viewer's id"), Frame.Uuid.Num() == Expected.Length()
		&& FMemory::Memcmp(Frame.Uuid.GetData(), Expected.Get(), Expected.Length()) == 0);

	FActorUpdateRequestMessage Ordinary;
	Ordinary.UUID = FCrowdyActorId::FromStringOrUnset(SpectatorTestUuid);
	TArray<uint8> OrdinaryStorage;
	FCrowdyCppOutboundFrame OrdinaryFrame;
	if (TestTrue(TEXT("an ordinary update splits too"), CrowdyCppSend::SplitMessage(Ordinary, OrdinaryStorage, OrdinaryFrame, Error)))
	{
		TestEqual(TEXT("and does carry its length, so the empty beat is not an artefact of the split"),
			OrdinaryFrame.Payload.Num(), static_cast<int32>(sizeof(int32)));
	}
	return true;
}

// The id: the session's when it is a whole id, else the account's, else none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorViewerUuidTest,
	"CrowdySDK.Replication.Spectator.ViewerUuid", SpectatorTestFlags)
bool FCrowdySpectatorViewerUuidTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the session's id wins"), CrowdySpectator::ViewerUuid(SpectatorTestUuid, 42), SpectatorTestUuid);

	const FString FromAccount = UHelperFunctions::GetDeterministicID(42).ToString(EGuidFormats::Digits);
	TestEqual(TEXT("with no session id the account's is the pawn's"), CrowdySpectator::ViewerUuid(FString(), 42), FromAccount);
	TestEqual(TEXT("a session id of another length is no id"), CrowdySpectator::ViewerUuid(TEXT("short"), 42), FromAccount);
	TestEqual(TEXT("it is 32 characters"), FromAccount.Len(), 32);
	TestTrue(TEXT("with neither there is none"), CrowdySpectator::ViewerUuid(FString(), 0).IsEmpty());
	return true;
}

// A beat arriving at an Unreal client decodes as a viewer's, with no state and no warning, and a frame that is
// merely malformed is still refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorEmptyUpdateDecodesAsViewerTest,
	"CrowdySDK.Replication.Spectator.EmptyUpdateDecodesAsViewer", SpectatorTestFlags)
bool FCrowdySpectatorEmptyUpdateDecodesAsViewerTest::RunTest(const FString& Parameters)
{
	FSpectatorTestNetWarnings Warnings;

	const FSpectatorTestFrame Beat(SpectatorTestUuid, TArray<uint8>());
	FActorUpdateNotificationMessage Viewer;
	if (!TestTrue(TEXT("an update with no body is accepted"), Viewer.DecodePayload(Beat.Frame)))
	{
		return false;
	}
	TestTrue(TEXT("as a viewer's"), Viewer.bViewerBeat);
	TestFalse(TEXT("with no state"), Viewer.State.IsValid());
	TestEqual(TEXT("for the actor that sent it"), Viewer.GUID, USerializationFunctionLibrary::ToGuid(SpectatorTestUuid));
	TestEqual(TEXT("from the chunk it named"), Viewer.ChunkY, static_cast<int64>(-4));
	TestEqual(TEXT("without a warning"), Warnings.Count, 0);

	const FSpectatorTestFrame Zeroes(SpectatorTestUuid, TArray<uint8>({0, 0, 0, 0}));
	FActorUpdateNotificationMessage Malformed;
	TestFalse(TEXT("a body that declares no state is still refused"), Malformed.DecodePayload(Zeroes.Frame));
	TestFalse(TEXT("and is no viewer"), Malformed.bViewerBeat);
	TestTrue(TEXT("and says so, so the count above can see a warning"), Warnings.Count > 0);
	return true;
}

// The tracker spawns nothing for a viewer and queues nothing; an avatar it already holds under that id times out as usual.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorTrackerSpawnsNothingTest,
	"CrowdySDK.Replication.Spectator.TrackerSpawnsNothing", SpectatorTestFlags)
bool FCrowdySpectatorTrackerSpawnsNothingTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("worker pool is not accepting work"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->SetupQueuesForTest();

	const auto Deliver = [&Tracker](const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>& Message)
	{
		const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> Shared = Message;
		const FCrowdyDelivery Delivery(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, Message->GetPayloadKey(),
			Message->GetPayload(), Message->GetPayloadBytes(), Shared);
		Tracker->HandleActorUpdateDeliveryForTest(Delivery);
	};

	const FGuid Stranger = FGuid::NewGuid();
	Deliver(SpectatorTestMessage(Stranger, true));
	TestEqual(TEXT("a viewer's beat queues nothing to spawn"), Tracker->NumQueuedUpdatesForTest(), 0);
	TestFalse(TEXT("is not tracked"), Tracker->IsTrackedForTest(Stranger));
	TestFalse(TEXT("and is not remembered as heard, which would later report it timed out"), Tracker->IsHeardForTest(Stranger));

	const FGuid Other = FGuid::NewGuid();
	Deliver(SpectatorTestMessage(Other, false));
	TestEqual(TEXT("an update of the same kind that is no beat is queued, so the count above can see one"),
		Tracker->NumQueuedUpdatesForTest(), 1);

	const FGuid Held = FGuid::NewGuid();
	Tracker->MarkActorTrackedForTest(Held);
	Tracker->BackdateHeardForTest(Held, 10.0);
	TestTrue(TEXT("the held actor was last heard long ago"), Tracker->HeardAgeForTest(Held) > 9.0);
	Deliver(SpectatorTestMessage(Held, true));
	TestTrue(TEXT("a beat under its id does not keep its avatar alive"), Tracker->HeardAgeForTest(Held) > 9.0);
	TestEqual(TEXT("and is gathered as no update for it"), Tracker->GetGatheredForTest().Num(), 0);

	Deliver(SpectatorTestMessage(Held, false));
	TestEqual(TEXT("where an update that is no beat is, so the count above can see one"), Tracker->GetGatheredForTest().Num(), 1);

	Tracker->ReleaseQueuesForTest();
	return true;
}

// Without a world nothing starts and nothing throws.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpectatorNoWorldDoesNothingTest,
	"CrowdySDK.Replication.Spectator.NoWorldDoesNothing", SpectatorTestFlags)
bool FCrowdySpectatorNoWorldDoesNothingTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdySpectatorComponent> Component(NewObject<UCrowdySpectatorComponent>(GetTransientPackage()));
	Component->StartSpectating();
	TestFalse(TEXT("it does not start"), Component->IsSpectating());
	Component->StopSpectating();
	TestFalse(TEXT("stopping what never started is harmless"), Component->IsSpectating());
	return true;
}

#endif
