#if WITH_DEV_AUTOMATION_TESTS

#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Messages/Actor/FActorLeftNotification.h"
#include "Messages/Communication/FClientVideoNotification.h"
#include "Misc/AutomationTest.h"
#include "Replication/Subsystems/CrowdyActorDepartureTestSupport.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyVideoFrameReceiver.h"
#include "Serialization/CrowdyFrame.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"

// A sender that leaves mid-frame leaves a partial frame that nothing will ever complete: the newer-frame
// rule needs a newer frame, and the timeout sweep only runs while fragments are still arriving. So a
// departure is the trigger, and this is what says the receiver's departure path actually reaches the
// assembler rather than merely existing.
namespace
{
	constexpr EAutomationTestFlags CrowdyVideoReceiverTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** The six-octet header this SDK writes, then a body, stated here independently of the decoder. */
	TArray<uint8> CrowdyVideoReceiverTestFragment(const uint8 FrameId, const uint8 FragmentIndex,
		const uint8 FragmentCount)
	{
		TArray<uint8> Fragment = { 1, 0, 0, FrameId, FragmentIndex, FragmentCount };
		Fragment.Append({ 0x10, 0x11, 0x12, 0x13 });
		return Fragment;
	}

	TUniquePtr<FCrowdyActorTestDeliveryScope> CrowdyVideoReceiverTestDelivery(
		const FCrowdyActorId& Sender, const uint8 FrameId, const uint8 FragmentIndex, const uint8 FragmentCount)
	{
		TArray<uint8> Bytes = CrowdyVideoReceiverTestFragment(FrameId, FragmentIndex, FragmentCount);

		FCrowdyFrame Frame;
		Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION);
		Frame.Body = Bytes;
		Frame.Envelope.ChunkX = 11;
		Frame.Envelope.ChunkY = -22;
		Frame.Envelope.ChunkZ = 33;
		Frame.Envelope.Uuid = TConstArrayView<uint8>(Sender.Octets, FCrowdyActorId::NumOctets);
		Frame.Envelope.Timestamp = 1700000000123;
		Frame.Envelope.Sequence = 42;
		Frame.bHasEnvelope = true;

		TSharedRef<FClientVideoNotification, ESPMode::ThreadSafe> Message =
			MakeShared<FClientVideoNotification, ESPMode::ThreadSafe>();
		if (!Message->DecodePayload(Frame))
		{
			return nullptr;
		}

		TUniquePtr<FCrowdyActorTestDeliveryScope> Scope =
			MakeUnique<FCrowdyActorTestDeliveryScope>(Message, MoveTemp(Bytes));

		// Rebound onto the storage the scope owns, since the array the decode read from has just moved.
		Message->FragmentView = Scope->Bytes;
		Message->BodyView = TConstArrayView<uint8>(Scope->Bytes).RightChop(CrowdyVideoFragment::HeaderBytes);
		return Scope;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReceiverForgetsDepartedSenderTest,
	"CrowdySDK.Replication.VideoReceiverForgetsADepartedSender", CrowdyVideoReceiverTestFlags)

bool FCrowdyVideoReceiverForgetsDepartedSenderTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Receiver(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	if (!TestTrue(TEXT("a receiver was created"), Receiver.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Leaver = CrowdyActorTestSender('a');
	const FCrowdyActorId Stayer = CrowdyActorTestSender('b');
	const FGuid LeaverGuid = USerializationFunctionLibrary::ToGuid(Leaver);
	const FGuid StayerGuid = USerializationFunctionLibrary::ToGuid(Stayer);

	if (!TestTrue(TEXT("the two senders derive different keys"), LeaverGuid != StayerGuid))
	{
		return false;
	}

	FCrowdyCppAssembledVideoFrame Frame;

	// One fragment of two from each, so both are left holding a frame nothing will complete.
	TestFalse(TEXT("the departing sender's first fragment completes nothing"),
		Receiver->IngestFragment(Leaver, LeaverGuid, CrowdyVideoReceiverTestFragment(1, 0, 2), 1000, Frame));
	TestFalse(TEXT("nor does the other sender's"),
		Receiver->IngestFragment(Stayer, StayerGuid, CrowdyVideoReceiverTestFragment(1, 0, 2), 1000, Frame));
	TestEqual(TEXT("both senders are holding a frame"), Receiver->GetPendingSenderCount(), 2);

	const int64 AbandonedBefore = Receiver->GetAbandonedFrameCount();

	// Through the function the tracker's departure delegate is bound to, and with the report that delegate
	// carries, so what is exercised is the wiring and not just the helper underneath it.
	FCrowdyActorLeft Departure;
	Departure.UUID = LeaverGuid;
	Departure.Reason = ECrowdyActorLeftReason::Stale;
	Receiver->HandleActorLeft(Departure, 1);

	TestEqual(TEXT("the departed sender's partial frame is dropped"), Receiver->GetPendingSenderCount(), 1);
	TestEqual(TEXT("and counted as abandoned"), Receiver->GetAbandonedFrameCount(), AbandonedBefore + 1);

	// The control: a departure must drop one sender's frame and not every sender's. Without this a forget
	// that cleared the whole assembler would satisfy the count above.
	TestTrue(TEXT("the sender that stayed still completes its frame"),
		Receiver->IngestFragment(Stayer, StayerGuid, CrowdyVideoReceiverTestFragment(1, 1, 2), 1010, Frame));
	TestEqual(TEXT("out of both its fragments"), Frame.Bytes.Num(), 8);
	TestEqual(TEXT("and nothing is left half assembled"), Receiver->GetPendingSenderCount(), 0);

	// And a departure naming a sender the receiver never saw must do nothing at all.
	const int64 AbandonedAfter = Receiver->GetAbandonedFrameCount();
	Receiver->ForgetSender(FGuid::NewGuid());
	TestEqual(TEXT("forgetting a sender never seen abandons nothing"), Receiver->GetAbandonedFrameCount(),
		AbandonedAfter);

	return true;
}

// Reassembly costs a copy of every fragment, so the receiver does none of it while nothing would receive
// a frame. That guard is what keeps an app with no video surface paying nothing, and it is also the way
// this whole path could read as tested while being permanently switched off, so it is stated both ways.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReceiverAnnouncesAssembledFramesTest,
	"CrowdySDK.Replication.VideoReceiverAnnouncesAnAssembledFrame", CrowdyVideoReceiverTestFlags)

bool FCrowdyVideoReceiverAnnouncesAssembledFramesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Receiver(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	if (!TestTrue(TEXT("a receiver was created"), Receiver.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Sender = CrowdyActorTestSender('c');

	const auto Deliver = [&Receiver, &Sender](const uint8 FrameId, const uint8 Index, const uint8 Count)
	{
		const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope =
			CrowdyVideoReceiverTestDelivery(Sender, FrameId, Index, Count);
		if (!Scope.IsValid())
		{
			return false;
		}
		Receiver->HandleVideoDelivery(*Scope->Delivery);
		return true;
	};

	// Nothing bound yet: the fragments are dropped where they arrive, which is the state a shipped app
	// with no video surface is in.
	if (!TestTrue(TEXT("two fragments were built and delivered"), Deliver(7, 0, 2) && Deliver(7, 1, 2)))
	{
		return false;
	}
	TestEqual(TEXT("with nothing listening, no fragment is even held"), Receiver->GetPendingSenderCount(), 0);

	int32 Announced = 0;
	FCrowdyVideoFrame Seen;
	Receiver->OnVideoFrameReady.AddLambda([&Announced, &Seen](const FCrowdyVideoFrame& Frame)
	{
		++Announced;
		Seen = Frame;
	});

	Deliver(9, 0, 2);
	TestEqual(TEXT("the first of two fragments announces nothing"), Announced, 0);
	TestEqual(TEXT("but is held now that something is listening"), Receiver->GetPendingSenderCount(), 1);

	Deliver(9, 1, 2);

	if (!TestEqual(TEXT("the second announces the completed frame once"), Announced, 1))
	{
		return false;
	}

	TestEqual(TEXT("the frame is both bodies"), Seen.Bytes.Num(), 8);
	TestEqual(TEXT("under the frame id the header carried"), Seen.FrameId, 9);
	TestEqual(TEXT("and the codec the header carried"), static_cast<int32>(Seen.Codec),
		static_cast<int32>(ECrowdyVideoCodec::Jpeg));
	TestTrue(TEXT("and the sender the envelope carried"),
		Seen.SenderUUID == USerializationFunctionLibrary::ToGuid(Sender));
	TestEqual(TEXT("and the chunk the envelope carried"), Seen.ChunkX, static_cast<int64>(11));
	TestEqual(TEXT("and the server's timestamp"), Seen.ServerTimestamp, static_cast<int64>(1700000000123));
	TestEqual(TEXT("and nothing is left half assembled"), Receiver->GetPendingSenderCount(), 0);

	return true;
}

// An actor id is 32 opaque octets and hexadecimal only by convention, while the key derived from it reads
// them as hexadecimal and folds every component holding anything else onto one value. So two senders can
// share a key, and a departure names only the key. The assembler is keyed by the octets, which is what the
// two must not be confused over: forgetting whichever sender was seen most recently would destroy a frame
// that was still arriving and leave the departed sender's counter behind for the life of the world.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReceiverForgetsEverySenderAKeyNamesTest,
	"CrowdySDK.Replication.VideoReceiverForgetsEverySenderAKeyNames", CrowdyVideoReceiverTestFlags)

bool FCrowdyVideoReceiverForgetsEverySenderAKeyNamesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Receiver(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	if (!TestTrue(TEXT("a receiver was created"), Receiver.IsValid()))
	{
		return false;
	}

	// Neither octet is a hexadecimal digit, so every eight-octet component of both derives the same value.
	const FCrowdyActorId First = CrowdyActorTestSender('z');
	const FCrowdyActorId Second = CrowdyActorTestSender('y');
	const FCrowdyActorId Unrelated = CrowdyActorTestSender('a');

	const FGuid SharedGuid = USerializationFunctionLibrary::ToGuid(First);
	if (!TestTrue(TEXT("the two senders are different ids"), First != Second)
		|| !TestTrue(TEXT("that derive one key between them"),
			SharedGuid == USerializationFunctionLibrary::ToGuid(Second)))
	{
		return false;
	}

	const FGuid UnrelatedGuid = USerializationFunctionLibrary::ToGuid(Unrelated);
	if (!TestTrue(TEXT("and a third sender whose key is its own"), UnrelatedGuid != SharedGuid))
	{
		return false;
	}

	FCrowdyCppAssembledVideoFrame Frame;

	// One fragment of two from each, so all three are holding a frame nothing will complete on its own.
	TestFalse(TEXT("the first colliding sender's fragment completes nothing"),
		Receiver->IngestFragment(First, SharedGuid, CrowdyVideoReceiverTestFragment(5, 0, 2), 1000, Frame));
	TestFalse(TEXT("nor the second's"),
		Receiver->IngestFragment(Second, SharedGuid, CrowdyVideoReceiverTestFragment(5, 0, 2), 1000, Frame));
	TestFalse(TEXT("nor the unrelated sender's"),
		Receiver->IngestFragment(Unrelated, UnrelatedGuid, CrowdyVideoReceiverTestFragment(5, 0, 2), 1000, Frame));
	TestEqual(TEXT("all three are holding a frame"), Receiver->GetPendingSenderCount(), 3);

	const int64 AbandonedBefore = Receiver->GetAbandonedFrameCount();

	FCrowdyActorLeft Departure;
	Departure.UUID = SharedGuid;
	Departure.Reason = ECrowdyActorLeftReason::Stale;
	Receiver->HandleActorLeft(Departure, 1);

	// Keeping only the sender seen most recently would leave the other one here, still holding a frame that
	// nothing will ever complete and a counter nothing will ever clear.
	TestEqual(TEXT("both senders the key names are forgotten"), Receiver->GetPendingSenderCount(), 1);
	TestEqual(TEXT("and both their frames are counted as abandoned"), Receiver->GetAbandonedFrameCount(),
		AbandonedBefore + 2);

	// The control: a departure must forget the senders its key names and no others.
	TestTrue(TEXT("the unrelated sender still completes its frame"),
		Receiver->IngestFragment(Unrelated, UnrelatedGuid, CrowdyVideoReceiverTestFragment(5, 1, 2), 1010, Frame));
	TestEqual(TEXT("out of both its fragments"), Frame.Bytes.Num(), 8);

	// The counter, which is the half a pending-frame count cannot see. A sender that rejoins restarts at
	// frame zero, and a counter left behind at five makes every fragment of it a straggler.
	TestTrue(TEXT("a forgotten sender that rejoins at frame zero is heard again"),
		Receiver->IngestFragment(First, SharedGuid, CrowdyVideoReceiverTestFragment(0, 0, 1), 1020, Frame));
	TestEqual(TEXT("carrying its one fragment's body"), Frame.Bytes.Num(), 4);

	TestTrue(TEXT("and so is the other sender the same key named"),
		Receiver->IngestFragment(Second, SharedGuid, CrowdyVideoReceiverTestFragment(0, 0, 1), 1030, Frame));

	return true;
}

// The tracker suppresses its departure report for an actor it was not holding, so that one departure is
// never reported twice. A video sender the tracker never held is exactly that case: its updates may never
// have decoded, or it may have arrived past the tracked ceiling. Its assembler state has to be released
// anyway, and the announced departure is the only signal that reaches it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReceiverForgetsAnUntrackedSenderTest,
	"CrowdySDK.Replication.VideoReceiverForgetsASenderTheTrackerNeverHeld", CrowdyVideoReceiverTestFlags)

bool FCrowdyVideoReceiverForgetsAnUntrackedSenderTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(
		NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Announced(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Reported(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	if (!TestTrue(TEXT("a tracker and two receivers were created"),
		Tracker.IsValid() && Announced.IsValid() && Reported.IsValid()))
	{
		return false;
	}

	// Bound the way Initialize binds them, one to each departure delegate, so which of the two fires is
	// what the two receivers below disagree about.
	Tracker->OnRemoteEntityLeftAnnounced.AddDynamic(Announced.Get(), &UCrowdyVideoFrameReceiver::HandleActorLeft);
	Tracker->OnRemoteEntityLeft.AddDynamic(Reported.Get(), &UCrowdyVideoFrameReceiver::HandleActorLeft);

	const FCrowdyActorId Sender = CrowdyActorTestSender('d');
	const FGuid SenderGuid = USerializationFunctionLibrary::ToGuid(Sender);

	FCrowdyCppAssembledVideoFrame Frame;
	Announced->IngestFragment(Sender, SenderGuid, CrowdyVideoReceiverTestFragment(1, 0, 2), 1000, Frame);
	Reported->IngestFragment(Sender, SenderGuid, CrowdyVideoReceiverTestFragment(1, 0, 2), 1000, Frame);
	if (!TestEqual(TEXT("both receivers are holding the sender's frame"),
		Announced->GetPendingSenderCount() + Reported->GetPendingSenderCount(), 2))
	{
		return false;
	}

	// This tracker has never seen an update, so it is holding nothing and the actor is untracked.
	const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope = CrowdyActorTestDeparture(Sender);
	if (!TestTrue(TEXT("a departure delivery was built"), Scope.IsValid()))
	{
		return false;
	}

	Tracker->HandleActorLeftDelivery(*Scope->Delivery);

	TestEqual(TEXT("the announced departure releases the sender's frame"),
		Announced->GetPendingSenderCount(), 0);

	// The other half, and the reason the two delegates are not one: the report stays suppressed for an
	// actor this client was not holding, so a departure is still never reported twice.
	TestEqual(TEXT("while the reported departure stays suppressed"), Reported->GetPendingSenderCount(), 1);

	return true;
}

// The server fans a video packet out over the chunk it was sent to, and the sender is in that chunk, so a
// client sending video is delivered its own frames. Reassembling them costs what a real sender's costs and
// produces a picture the caller already had.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoReceiverDiscardsItsOwnFramesTest,
	"CrowdySDK.Replication.VideoReceiverDiscardsItsOwnFrames", CrowdyVideoReceiverTestFlags)

bool FCrowdyVideoReceiverDiscardsItsOwnFramesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyVideoFrameReceiver> Receiver(
		NewObject<UCrowdyVideoFrameReceiver>(GetTransientPackage()));
	if (!TestTrue(TEXT("a receiver was created"), Receiver.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Local = CrowdyActorTestSender('e');
	const FCrowdyActorId Remote = CrowdyActorTestSender('f');

	int32 Announced = 0;
	Receiver->OnVideoFrameReady.AddLambda([&Announced](const FCrowdyVideoFrame&) { ++Announced; });

	const auto Deliver = [&Receiver](const FCrowdyActorId& Sender, const uint8 FrameId, const uint8 Index,
		const uint8 Count)
	{
		const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope =
			CrowdyVideoReceiverTestDelivery(Sender, FrameId, Index, Count);
		if (!Scope.IsValid())
		{
			return false;
		}
		Receiver->HandleVideoDelivery(*Scope->Delivery);
		return true;
	};

	Receiver->HandleOwnerUUIDUpdated(Local.ToString());

	if (!TestTrue(TEXT("both fragments of the client's own frame were delivered"),
		Deliver(Local, 3, 0, 2) && Deliver(Local, 3, 1, 2)))
	{
		return false;
	}

	TestEqual(TEXT("a client's own frame is not announced"), Announced, 0);
	TestEqual(TEXT("and not even held"), Receiver->GetPendingSenderCount(), 0);

	// The control: the filter has to name one sender rather than close the path. Without it a receiver that
	// dropped everything would satisfy both lines above.
	if (!TestTrue(TEXT("another sender's fragments were delivered"),
		Deliver(Remote, 3, 0, 2) && Deliver(Remote, 3, 1, 2)))
	{
		return false;
	}

	TestEqual(TEXT("another sender's frame is announced"), Announced, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
