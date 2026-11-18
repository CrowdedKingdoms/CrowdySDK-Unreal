#include "Subsystem/CrowdyVideoSender.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags VideoSenderTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A game instance subsystem can only be created inside a game instance.
	struct FVideoSenderFixture
	{
		TStrongObjectPtr<UGameInstance> Instance{NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient)};
		TStrongObjectPtr<UCrowdyVideoSender> Sender{NewObject<UCrowdyVideoSender>(Instance.Get())};

		UCrowdyVideoSender* operator->() const { return Sender.Get(); }
	};

	TArray<uint8> MakeVideoFrame(const int32 Bytes)
	{
		TArray<uint8> Frame;
		Frame.Init(0x5A, Bytes);
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderRefusalsTest,
	"CrowdySDK.Services.Video.RefusesBeforeSending", VideoSenderTestFlags)
bool FCrowdyVideoSenderRefusalsTest::RunTest(const FString& Parameters)
{
	using EResult = ECrowdyVideoSendResult;
	const FVideoSenderFixture Sender;
	const FCrowdyActorId Id = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FInt64Vector Here(1, 2, 3);
	int32 Transmitted = 0;
	auto Accept = [&Transmitted](int32, const FInt64Vector&, uint8, uint8, int32& OutFragments, FString&)
	{
		++Transmitted;
		OutFragments = 1;
		return true;
	};
	auto Send = [&](const TArray<uint8>& Frame, const ECrowdyVideoCodec Codec = ECrowdyVideoCodec::Jpeg, const bool bConnected = true,
		const FCrowdyActorId& SenderId = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4)), const FInt64Vector* Chunk = nullptr)
	{
		return Sender->SendFrame(Frame, Codec, 1, bConnected, SenderId, Chunk ? Chunk : &Here, Accept).Result;
	};

	const TArray<uint8> Small = MakeVideoFrame(10);
	TestEqual(TEXT("a small JPEG is sent"), Send(Small), EResult::Sent);
	TestEqual(TEXT("a WebP is sent"), Send(Small, ECrowdyVideoCodec::WebP), EResult::Sent);
	TestEqual(TEXT("an unknown codec is not sent"), Send(Small, ECrowdyVideoCodec::Unknown), EResult::CodecNotSent);
	TestEqual(TEXT("the codec is judged before the frame is"), Send(TArray<uint8>(), ECrowdyVideoCodec::Unknown, false), EResult::CodecNotSent);
	TestEqual(TEXT("an empty frame is refused"), Send(TArray<uint8>()), EResult::Empty);

	const int32 Max = UCrowdyVideoSender::MaxFrameBytes;
	TestEqual(TEXT("exactly the limit is sent"), Send(MakeVideoFrame(Max)), EResult::Sent);
	TestEqual(TEXT("one byte over is too large"), Send(MakeVideoFrame(Max + 1)), EResult::TooLarge);
	TestEqual(TEXT("the size is judged before the connection"), Send(MakeVideoFrame(Max + 1), ECrowdyVideoCodec::Jpeg, false), EResult::TooLarge);

	TestEqual(TEXT("no connection is not connected"), Send(Small, ECrowdyVideoCodec::Jpeg, false), EResult::NotConnected);
	TestEqual(TEXT("no sender id has no local player"), Send(Small, ECrowdyVideoCodec::Jpeg, true, FCrowdyActorId()), EResult::NoLocalPlayer);
	TestEqual(TEXT("no chunk has no local player"),
		Sender->SendFrame(Small, ECrowdyVideoCodec::Jpeg, 1, true, Id, nullptr, Accept).Result, EResult::NoLocalPlayer);

	TestEqual(TEXT("only the three accepted frames reached the transmit"), Transmitted, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderFrameIdsTest,
	"CrowdySDK.Services.Video.FrameIdsCountPerSender", VideoSenderTestFlags)
bool FCrowdyVideoSenderFrameIdsTest::RunTest(const FString& Parameters)
{
	const FVideoSenderFixture Sender;
	const FCrowdyActorId A = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FCrowdyActorId B = FCrowdyActorId::FromGuid(FGuid(5, 6, 7, 8));
	const FInt64Vector Here(0, 0, 0);
	const TArray<uint8> Frame = MakeVideoFrame(10);
	int32 Fragments = 4;
	auto Accept = [&Fragments](int32, const FInt64Vector&, uint8, uint8, int32& OutFragments, FString&)
	{
		OutFragments = Fragments;
		return true;
	};
	auto Next = [&](const FCrowdyActorId& Id)
	{
		return Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Accept);
	};

	Sender->SeedFrameIdForTest(A, 0);
	Sender->SeedFrameIdForTest(B, 0);
	TestEqual(TEXT("a sender's first frame is its seed"), Next(A).FrameId, 0);
	TestEqual(TEXT("then 1"), Next(A).FrameId, 1);
	TestEqual(TEXT("then 2"), Next(A).FrameId, 2);

	const FCrowdyVideoFrameSend Other = Next(B);
	TestEqual(TEXT("a second sender counts from its own seed"), Other.FrameId, 0);
	TestEqual(TEXT("the first sender carries on"), Next(A).FrameId, 3);

	TestEqual(TEXT("the result says Sent"), Other.Result, ECrowdyVideoSendResult::Sent);
	TestEqual(TEXT("with the fragment count the bridge reported"), Other.Fragments, 4);
	TestTrue(TEXT("and no message"), Other.Message.IsEmpty());

	Sender->SendFrame(TArray<uint8>(), ECrowdyVideoCodec::Jpeg, 1, true, A, &Here, Accept);
	Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, false, A, &Here, Accept);
	TestEqual(TEXT("a refusal before the send spends no id"), Next(A).FrameId, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderWrapTest,
	"CrowdySDK.Services.Video.FrameIdWrapsAfter65535", VideoSenderTestFlags)
bool FCrowdyVideoSenderWrapTest::RunTest(const FString& Parameters)
{
	const FVideoSenderFixture Sender;
	const FCrowdyActorId Id = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FInt64Vector Here(0, 0, 0);
	const TArray<uint8> Frame = MakeVideoFrame(1);
	auto Accept = [](int32, const FInt64Vector&, uint8, uint8, int32& OutFragments, FString&)
	{
		OutFragments = 1;
		return true;
	};

	Sender->SeedFrameIdForTest(Id, 65535);
	TestEqual(TEXT("a sender seeded at 65535 sends 65535"), Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Accept).FrameId, 65535);
	TestEqual(TEXT("the next wraps to 0"), Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Accept).FrameId, 0);
	TestEqual(TEXT("and counts on from there"), Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Accept).FrameId, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderSeedTest,
	"CrowdySDK.Services.Video.FirstFrameIdComesFromTheClock", VideoSenderTestFlags)
bool FCrowdyVideoSenderSeedTest::RunTest(const FString& Parameters)
{
	const FDateTime Base(2030, 1, 1, 0, 0, 0, 0);
	const int32 Step = static_cast<int32>(UCrowdyVideoSender::FrameIdSeedAt(Base + FTimespan::FromSeconds(1)))
		- static_cast<int32>(UCrowdyVideoSender::FrameIdSeedAt(Base));
	TestEqual(TEXT("seeds a second apart differ by 10, one step per 100 ms"), (Step + 65536) % 65536, 10);
	TestEqual(TEXT("100 ms is one step"),
		(UCrowdyVideoSender::FrameIdSeedAt(Base + FTimespan::FromMilliseconds(100)) - UCrowdyVideoSender::FrameIdSeedAt(Base) + 65536) % 65536, 1);

	const FVideoSenderFixture Sender;
	const FCrowdyActorId Id = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FInt64Vector Here(0, 0, 0);
	const TArray<uint8> Frame = MakeVideoFrame(1);
	auto Accept = [](int32, const FInt64Vector&, uint8, uint8, int32& OutFragments, FString&)
	{
		OutFragments = 1;
		return true;
	};

	const int32 Before = UCrowdyVideoSender::FrameIdSeedAt(FDateTime::UtcNow());
	const int32 First = Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Accept).FrameId;
	const int32 After = UCrowdyVideoSender::FrameIdSeedAt(FDateTime::UtcNow());
	TestTrue(TEXT("an unseeded sender's first frame carries the clock's seed"), First == Before || First == After);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderFailedSendTest,
	"CrowdySDK.Services.Video.FailedSendSpendsItsId", VideoSenderTestFlags)
bool FCrowdyVideoSenderFailedSendTest::RunTest(const FString& Parameters)
{
	const FVideoSenderFixture Sender;
	const FCrowdyActorId Id = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FInt64Vector Here(0, 0, 0);
	const TArray<uint8> Frame = MakeVideoFrame(10);
	TArray<int32> SeenIds;
	FString Failure;
	Sender->SeedFrameIdForTest(Id, 0);
	auto Transmit = [&](const int32 FrameId, const FInt64Vector&, uint8, uint8, int32& OutFragments, FString& OutError)
	{
		SeenIds.Add(FrameId);
		OutError = Failure;
		OutFragments = 1;
		return Failure.IsEmpty();
	};
	auto Send = [&]()
	{
		return Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Transmit);
	};

	Failure = TEXT("the outbound queue is full");
	const FCrowdyVideoFrameSend Full = Send();
	TestEqual(TEXT("a refused queue is Queue Full"), Full.Result, ECrowdyVideoSendResult::QueueFull);
	TestEqual(TEXT("carrying the bridge's own text"), Full.Message, Failure);
	TestEqual(TEXT("and no fragments"), Full.Fragments, 0);

	Failure = TEXT("the connection is closed");
	const FCrowdyVideoFrameSend Closed = Send();
	TestEqual(TEXT("a closed connection is Not Connected"), Closed.Result, ECrowdyVideoSendResult::NotConnected);
	TestEqual(TEXT("carrying the bridge's own text"), Closed.Message, Failure);

	Failure = TEXT("the connection is not open");
	const FCrowdyVideoFrameSend Down = Send();
	TestEqual(TEXT("a link that is down but not closed is Not Connected"), Down.Result, ECrowdyVideoSendResult::NotConnected);

	Failure = TEXT("the outbound queue has room for 3 more sends, not the 5 this one needs");
	TestEqual(TEXT("a queue without room for every fragment is Queue Full"), Send().Result, ECrowdyVideoSendResult::QueueFull);

	Failure.Reset();
	const FCrowdyVideoFrameSend Sent = Send();
	TestEqual(TEXT("the next send is accepted"), Sent.Result, ECrowdyVideoSendResult::Sent);
	TestEqual(TEXT("under an id the four failures did not reuse"), Sent.FrameId, 4);
	TestEqual(TEXT("every send reached the transmit"), SeenIds.Num(), 5);
	TestTrue(TEXT("each under its own id, 0 to 4"), SeenIds.Num() == 5 && SeenIds[0] == 0 && SeenIds[1] == 1 && SeenIds[2] == 2 && SeenIds[3] == 3 && SeenIds[4] == 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderDistanceTest,
	"CrowdySDK.Services.Video.DistanceIsClampedAndFrameIsPlaced", VideoSenderTestFlags)
bool FCrowdyVideoSenderDistanceTest::RunTest(const FString& Parameters)
{
	const FVideoSenderFixture Sender;
	const FCrowdyActorId Id = FCrowdyActorId::FromGuid(FGuid(1, 2, 3, 4));
	const FInt64Vector Here(7, -8, 9);
	const TArray<uint8> Frame = MakeVideoFrame(10);
	int32 SeenDistance = -1;
	int32 SeenCodec = -1;
	FInt64Vector SeenChunk;
	auto Transmit = [&](int32, const FInt64Vector& Chunk, const uint8 CodecByte, const uint8 Distance, int32& OutFragments, FString&)
	{
		SeenDistance = Distance;
		SeenCodec = CodecByte;
		SeenChunk = Chunk;
		OutFragments = 1;
		return true;
	};
	auto DistanceSent = [&](const int32 Requested)
	{
		Sender->SendFrame(Frame, ECrowdyVideoCodec::WebP, Requested, true, Id, &Here, Transmit);
		return SeenDistance;
	};

	TestEqual(TEXT("a negative distance is 0"), DistanceSent(-5), 0);
	TestEqual(TEXT("0 stays 0"), DistanceSent(0), 0);
	TestEqual(TEXT("4 stays 4"), DistanceSent(4), 4);
	TestEqual(TEXT("8 stays 8"), DistanceSent(8), 8);
	TestEqual(TEXT("9 is 8"), DistanceSent(9), 8);
	TestEqual(TEXT("a huge distance is 8"), DistanceSent(1000000), 8);
	TestEqual(TEXT("WebP travels as codec byte 1"), SeenCodec, 1);
	TestTrue(TEXT("from the chunk the pawn stands in"), SeenChunk == Here);

	Sender->SendFrame(Frame, ECrowdyVideoCodec::Jpeg, 1, true, Id, &Here, Transmit);
	TestEqual(TEXT("JPEG travels as codec byte 0"), SeenCodec, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVideoSenderRefusalEventTest,
	"CrowdySDK.Services.Video.OnlyVideoRefusalsAreReported", VideoSenderTestFlags)
bool FCrowdyVideoSenderRefusalEventTest::RunTest(const FString& Parameters)
{
	const FVideoSenderFixture Sender;
	FCrowdySendRefusal Refusal;
	Refusal.Code = 7;

	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_PACKET);
	TestTrue(TEXT("a refused video frame is video"), UCrowdyVideoSender::IsVideoRefusal(Refusal));
	Sender->HandleSendRefused(Refusal);
	TestEqual(TEXT("and is reported"), Sender->VideoRefusalsBroadcast, 1);

	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::SINGLE_ACTOR_MESSAGE);
	Sender->HandleSendRefused(Refusal);
	TestEqual(TEXT("a message to one actor is not"), Sender->VideoRefusalsBroadcast, 1);

	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::CLIENT_TEXT_PACKET);
	Sender->HandleSendRefused(Refusal);
	TestEqual(TEXT("nor is proximity text"), Sender->VideoRefusalsBroadcast, 1);

	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::CLIENT_VIDEO_PACKET);
	Refusal.bChannelMessage = true;
	Refusal.ChannelId = 9;
	Sender->HandleSendRefused(Refusal);
	TestEqual(TEXT("nor is a channel message"), Sender->VideoRefusalsBroadcast, 1);
	return true;
}

#endif
