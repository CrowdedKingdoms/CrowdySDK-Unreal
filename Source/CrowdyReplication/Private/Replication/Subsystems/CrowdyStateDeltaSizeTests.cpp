#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyEntityTypes.h"
#include "Messages/Actor/CrowdyUpdateExtras.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateDeltaSizeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString Text(const int32 Length)
	{
		return FString::ChrN(Length, TEXT('x'));
	}

	struct FSizeRig
	{
		UCrowdyAutoRegistry* Registry = MakeStateRegistry();
		UCrowdyStateReplicator* Rep = MakeReplicator(Registry, FGuid::NewGuid());
		TArray<FCrowdyStateDelta> Broadcast;
		TArray<FCrowdyStateDelta> Targeted;
		TArray<FCrowdyStateDelta> Channel;

		FSizeRig()
		{
			Rep->SetTimeForTest(0.0);
			Rep->SetKeyframeIntervalForTest(0.f);
			Rep->DispatchHookForTests = [this](const FCrowdyStateDelta& Delta, const bool bTargeted)
			{
				(bTargeted ? Targeted : Broadcast).Add(Delta);
			};
			Rep->ChannelDispatchHookForTests = [this](const FCrowdyStateDelta& Delta) { Channel.Add(Delta); };
		}

		// Registers a participant and drains the first tick, which sends nothing while it holds its defaults.
		bool Track(UObject* Participant, const FGuid& Id)
		{
			const bool bTracked = Rep->RegisterOwnedEntityForTest(Id, Participant);
			Rep->RunReplicationLoopForTest();
			return bTracked;
		}

		template <typename FContainer>
		void Apply(const TArray<FCrowdyStateDelta>& Deltas, FContainer* Destination) const
		{
			const FCrowdyRepLayout* Layout = Registry->FindRepLayout(FContainer::StaticClass());
			TArray<int32> Changed;
			for (const FCrowdyStateDelta& Delta : Deltas)
			{
				FCrowdyStateCodec::Decode(*Layout, Delta.LayoutHash, Delta.Blob, Destination, Changed);
			}
		}
	};

	int32 WireOctetsOf(const FGameEventRequest& Request, FAutomationTestBase& Test)
	{
		TArray<uint8> Storage;
		FCrowdyCppOutboundFrame Frame;
		FString Error;
		const bool bSplit = CrowdyCppSend::SplitMessage(Request, Storage, Frame, Error);
		Test.TestTrue(FString::Printf(TEXT("the frame splits (%s)"), *Error), bSplit);
		return Frame.Payload.Num();
	}

	void FillRequest(FGameEventRequest& Request, const FCrowdyStateDelta& Delta, const FCrowdyTypeID TypeID)
	{
		Request.AppID = 1;
		Request.UUID = FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
		Request.EventType = static_cast<uint16>(TypeID);
		Request.Target = ECrowdyTarget::Everyone;
		Request.PayloadStruct = FCrowdyStateDelta::StaticStruct();
		Request.PayloadMemory = &Delta;
		Request.PayloadTypeID = TypeID;
	}
}

// Every route is measured through the real encoders, so a new field around the blob fails here rather than on the wire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateBlobLimitsMatchTheEncodersTest,
	"CrowdySDK.State.BlobLimitsMatchTheEncoders", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateBlobLimitsMatchTheEncodersTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyStateDelta::StaticStruct());

	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	if (!TestTrue(TEXT("the delta has a wire type"),
		UEventPayloadRegistry::Get()->GetID(FCrowdyStateDelta::StaticStruct(), TypeID)))
	{
		return false;
	}

	FCrowdyStateDelta Delta;
	Delta.ClassID = 1;
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = 2;
	Delta.Blob.SetNumZeroed(CrowdyStateBlobLimit::Event);

	FGameEventRequest Spatial;
	FillRequest(Spatial, Delta, TypeID);
	TestEqual(TEXT("a broadcast at the limit fills the payload exactly"),
		WireOctetsOf(Spatial, *this), CrowdyUpdateExtras::MaxPayloadOctets);

	FSingleActorRequest Single;
	FillRequest(Single, Delta, TypeID);
	TestEqual(TEXT("a targeted send at the limit fills the payload exactly"),
		WireOctetsOf(Single, *this), CrowdyUpdateExtras::MaxPayloadOctets);

	Delta.Blob.SetNumZeroed(CrowdyStateBlobLimit::Channel);
	TArray<uint8> ChannelPayload;
	FCrowdyStateCodec::EncodeChannelStateDelta(Delta, ChannelPayload);
	TestEqual(TEXT("a channel delta at the limit fills the payload exactly"),
		ChannelPayload.Num(), CrowdyChannelPayloadMaxBytes);

	return true;
}

// The largest value that fits goes, and one byte more does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateBlobAtTheLimitGoesTest,
	"CrowdySDK.State.BlobAtTheLimitGoes", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateBlobAtTheLimitGoesTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, FGuid::NewGuid()));

	const FCrowdyRepLayout* Layout = Rig.Registry->FindRepLayout(ACrowdyStateSizeTestActor::StaticClass());
	const int32 FirstIndex = Layout->Properties.IndexOfByPredicate([](const FCrowdyRepProperty& Row)
	{
		return Row.Property && Row.Property->GetFName() == GET_MEMBER_NAME_CHECKED(ACrowdyStateSizeTestActor, RepFirst);
	});
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[FirstIndex] = true;

	int32 Length = 900;
	TArray<uint8> Blob;
	do
	{
		Actor->RepFirst = Text(++Length);
		FCrowdyStateCodec::Encode(*Layout, Actor, Dirty, /*bKeyframe=*/false, Blob);
	}
	while (Blob.Num() < CrowdyStateBlobLimit::Event);

	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("the value at the limit goes"), Rig.Broadcast.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("it fills the limit"), Rig.Broadcast[0].Blob.Num(), CrowdyStateBlobLimit::Event);

	AddExpectedMessagePlain(TEXT("RepFirst on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Actor->RepFirst = Text(Length + 1);
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("one byte more does not go"), Rig.Broadcast.Num(), 1);

	// The same value inside a split: it fills a part alone and its neighbour takes the next.
	Actor->RepFirst = FString::ChrN(Length, TEXT('y'));
	Actor->RepSmall = 5;
	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("the split sends both"), Rig.Broadcast.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the first part fills the limit"), Rig.Broadcast[1].Blob.Num(), CrowdyStateBlobLimit::Event);

	return true;
}

// A value too big for any send stays unsent, warns once, leaves its neighbours moving, and goes when it fits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOversizedPropertyStaysDirtyTest,
	"CrowdySDK.State.OversizedPropertyStaysDirty", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateOversizedPropertyStaysDirtyTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	ACrowdyStateSizeTestActor* Observer = NewObject<ACrowdyStateSizeTestActor>();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, FGuid::NewGuid()));

	AddExpectedMessagePlain(TEXT("RepFirst on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	Actor->RepSmall = 5;
	Actor->RepFirst = Text(2000);
	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("the small property goes on its own"), Rig.Broadcast.Num(), 1))
	{
		return false;
	}
	Rig.Apply(Rig.Broadcast, Observer);
	TestEqual(TEXT("the observer has the small value"), Observer->RepSmall, 5);
	TestTrue(TEXT("the observer has no oversized value"), Observer->RepFirst.IsEmpty());

	const int32 SentBytes = Rig.Rep->GetLastSentStateBytes(Actor);
	TestTrue(TEXT("the small send is counted"), SentBytes > 0);

	Rig.Rep->RunReplicationLoopForTest();
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("nothing goes again while it stays too big"), Rig.Broadcast.Num(), 1);
	TestEqual(TEXT("a tick that sent nothing keeps the last real count"), Rig.Rep->GetLastSentStateBytes(Actor), SentBytes);

	Actor->RepFirst = TEXT("fits");
	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("it goes once it fits"), Rig.Broadcast.Num(), 2))
	{
		return false;
	}
	Rig.Apply(Rig.Broadcast, Observer);
	TestEqual(TEXT("the observer has the value that fit"), Observer->RepFirst, FString(TEXT("fits")));

	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("sent means no longer dirty"), Rig.Broadcast.Num(), 2);

	// Growing past the limit a second time warns a second time: the first warning ended when the value went.
	Actor->RepFirst = Text(2000);
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("too big again goes nowhere"), Rig.Broadcast.Num(), 2);

	return true;
}

// The property after an oversized one starts a clean part: the one that was refused is not carried along.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOversizedPropertyLeavesThePartTest,
	"CrowdySDK.State.OversizedPropertyLeavesThePart", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateOversizedPropertyLeavesThePartTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	ACrowdyStateSizeTestActor* Observer = NewObject<ACrowdyStateSizeTestActor>();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, FGuid::NewGuid()));

	AddExpectedMessagePlain(TEXT("RepSecond on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	Actor->RepFirst = TEXT("before");
	Actor->RepSecond = Text(2000);
	Actor->RepSmall = 5;
	Rig.Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("the properties either side go in two parts"), Rig.Broadcast.Num(), 2);
	Rig.Apply(Rig.Broadcast, Observer);
	TestEqual(TEXT("the property before it arrived"), Observer->RepFirst, FString(TEXT("before")));
	TestTrue(TEXT("the oversized property did not"), Observer->RepSecond.IsEmpty());
	TestEqual(TEXT("the property after it arrived"), Observer->RepSmall, 5);

	return true;
}

// A one-shot host push has nothing to retry with, so it is refused out loud and runs no notify.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOversizedHostPushIsRefusedTest,
	"CrowdySDK.State.OversizedHostPushIsRefused", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateOversizedHostPushIsRefusedTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(LocalPlayer);
	Rig.Rep->SetLocalPlayerIDForTest(LocalPlayer);
	Rig.Rep->SetEntitySubsystemForTest(ES);
	Rig.Rep->SetHostIDForTest(LocalPlayer);

	ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Target->Entity->HostOverride = ECrowdyHostOverride::Allow;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.OwnerID = FGuid::NewGuid();
	Record.Participant = Target;
	ES->RegisterEntity(Record);

	Target->RepText = TEXT("fits");
	Rig.Rep->MarkStateDirty(Record.NetID, TEXT("RepText"));
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("a push that fits goes"), Rig.Broadcast.Num(), 1);

	AddExpectedMessagePlain(TEXT("host push of RepText"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Target->RepText = Text(2000);
	Rig.Rep->MarkStateDirty(Record.NetID, TEXT("RepText"));
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("a push too big to go does not"), Rig.Broadcast.Num(), 1);

	return true;
}

// A marked property keeps its mark until it really goes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOversizedMarkSurvivesTest,
	"CrowdySDK.State.OversizedMarkSurvives", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateOversizedMarkSurvivesTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	const FGuid Id = FGuid::NewGuid();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, Id));

	AddExpectedMessagePlain(TEXT("RepMarked on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	Actor->RepMarked = Text(2000);
	Rig.Rep->MarkStateDirty(Id, GET_MEMBER_NAME_CHECKED(ACrowdyStateSizeTestActor, RepMarked));
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the marked value is too big to go"), Rig.Broadcast.Num(), 0);

	Actor->RepMarked = TEXT("fits");
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the mark held, so it goes with no new mark"), Rig.Broadcast.Num(), 1);

	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("sending it cleared the mark"), Rig.Broadcast.Num(), 1);

	return true;
}

// Two values that fit apart go in two deltas that an observer puts back together.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLargeUpdateSplitsTest,
	"CrowdySDK.State.LargeUpdateSplits", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateLargeUpdateSplitsTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	ACrowdyStateSizeTestActor* Observer = NewObject<ACrowdyStateSizeTestActor>();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, FGuid::NewGuid()));

	Actor->RepFirst = Text(600);
	Actor->RepSecond = Text(601);
	Actor->RepSmall = 7;
	Rig.Rep->RunReplicationLoopForTest();

	if (!TestEqual(TEXT("the update takes two deltas"), Rig.Broadcast.Num(), 2))
	{
		return false;
	}
	for (const FCrowdyStateDelta& Delta : Rig.Broadcast)
	{
		TestTrue(TEXT("each part fits"), Delta.Blob.Num() <= CrowdyStateBlobLimit::Event);
		TestEqual(TEXT("each part names its entity"), Delta.EntityID, Rig.Broadcast[0].EntityID);
	}

	Rig.Apply(Rig.Broadcast, Observer);
	TestEqual(TEXT("the first value arrived"), Observer->RepFirst, Actor->RepFirst);
	TestEqual(TEXT("the second value arrived"), Observer->RepSecond, Actor->RepSecond);
	TestEqual(TEXT("the small value arrived"), Observer->RepSmall, 7);
	TestEqual(TEXT("both parts count as sent"), Rig.Rep->GetLastSentStateBytes(Actor),
		Rig.Broadcast[0].Blob.Num() + Rig.Broadcast[1].Blob.Num());

	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("nothing is sent twice"), Rig.Broadcast.Num(), 2);

	return true;
}

// Owner-only values split and refuse the same way, and stay on the targeted route.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnerOnlyHonoursTheLimitTest,
	"CrowdySDK.State.OwnerOnlyHonoursTheLimit", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateOwnerOnlyHonoursTheLimitTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	ACrowdyStateSizeTestActor* Owner = NewObject<ACrowdyStateSizeTestActor>();
	TestTrue(TEXT("tracked"), Rig.Track(Actor, FGuid::NewGuid()));

	Actor->RepOwnerFirst = Text(600);
	Actor->RepOwnerSecond = Text(601);
	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("the owner-only update takes two targeted deltas"), Rig.Targeted.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("nothing owner-only is broadcast"), Rig.Broadcast.Num(), 0);
	Rig.Apply(Rig.Targeted, Owner);
	TestEqual(TEXT("the first owner-only value arrived"), Owner->RepOwnerFirst, Actor->RepOwnerFirst);
	TestEqual(TEXT("the second owner-only value arrived"), Owner->RepOwnerSecond, Actor->RepOwnerSecond);

	AddExpectedMessagePlain(TEXT("RepOwnerFirst on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Actor->RepOwnerFirst = Text(2000);
	Actor->RepSmall = 3;
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("the oversized owner-only value does not go"), Rig.Targeted.Num(), 2);
	TestEqual(TEXT("it does not stop the broadcast"), Rig.Broadcast.Num(), 1);

	Actor->RepOwnerFirst = TEXT("fits");
	Rig.Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("it goes once it fits"), Rig.Targeted.Num(), 3);

	return true;
}

// A value that fits a broadcast can be too big for the channel, so each route is held to its own limit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateChannelHasItsOwnLimitTest,
	"CrowdySDK.State.ChannelHasItsOwnLimit", CrowdyStateDeltaSizeTestFlags)
bool FCrowdyStateChannelHasItsOwnLimitTest::RunTest(const FString& Parameters)
{
	FSizeRig Rig;
	ACrowdyStateSizeTestActor* Actor = NewObject<ACrowdyStateSizeTestActor>();
	UCrowdyStateSizeTestSubsystem* Subsystem = NewObject<UCrowdyStateSizeTestSubsystem>();
	UCrowdyStateSizeTestSubsystem* Observer = NewObject<UCrowdyStateSizeTestSubsystem>();
	TestTrue(TEXT("actor tracked"), Rig.Track(Actor, FGuid::NewGuid()));
	TestTrue(TEXT("subsystem tracked"), Rig.Track(Subsystem, FGuid::NewGuid()));

	AddExpectedMessagePlain(TEXT("RepText on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const int32 BetweenTheLimits = 1000;
	Actor->RepFirst = Text(BetweenTheLimits);
	Subsystem->RepText = Text(BetweenTheLimits);
	Subsystem->RepSmall = 9;
	Rig.Rep->RunReplicationLoopForTest();

	TestEqual(TEXT("the value goes as a broadcast"), Rig.Broadcast.Num(), 1);
	if (!TestEqual(TEXT("over the channel only the small property goes"), Rig.Channel.Num(), 1))
	{
		return false;
	}
	Rig.Apply(Rig.Channel, Observer);
	TestEqual(TEXT("the small value arrived"), Observer->RepSmall, 9);
	TestTrue(TEXT("the value too big for the channel did not"), Observer->RepText.IsEmpty());

	Subsystem->RepText = Text(900);
	Rig.Rep->RunReplicationLoopForTest();
	if (!TestEqual(TEXT("it goes once it fits the channel"), Rig.Channel.Num(), 2))
	{
		return false;
	}

	TArray<uint8> Payload;
	FCrowdyStateCodec::EncodeChannelStateDelta(Rig.Channel[1], Payload);
	TestTrue(TEXT("the encoded delta fits the channel"), Payload.Num() <= CrowdyChannelPayloadMaxBytes);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
