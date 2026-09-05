#include "Replication/Wire/CrowdyOutboundGoldenTestTypes.h"
#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Serialization/CrowdyActorId.h"
#include "StructUtils/InstancedStruct.h"
#include "Utils/CrowdyPodCopyPlan.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

// To regenerate a vector after an INTENTIONAL wire-format change: empty the stored array, run the test,
// and read the hex the failure message prints back as the new literal. Check each field of it against the
// layout the message documents before storing it; a vector taken on trust pins whatever the change did.
namespace
{
	constexpr EAutomationTestFlags CrowdyOutboundGoldenTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A local copy of the parity harness's reporter: that one lives in CrowdyNet's private tree, and
	// exporting a test helper across a module boundary to save nine lines is not worth the public surface.
	FString DescribeDifference(const TArray<uint8>& Actual, const TArray<uint8>& Expected)
	{
		if (Actual.Num() != Expected.Num())
		{
			return FString::Printf(TEXT("length %d, expected %d"), Actual.Num(), Expected.Num());
		}

		for (int32 Index = 0; Index < Actual.Num(); ++Index)
		{
			if (Actual[Index] != Expected[Index])
			{
				return FString::Printf(TEXT("first difference at offset %d: 0x%02x, expected 0x%02x"),
					Index, Actual[Index], Expected[Index]);
			}
		}

		return TEXT("identical");
	}

	FString HexOf(const TConstArrayView<uint8> Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num());
	}

	// The spatial header every vector below leads with. Fixed values, and a negative chunk coordinate so a
	// header written unsigned could not pass.
	constexpr int64 GoldenAppID = 7;
	constexpr int64 GoldenChunkX = 1;
	constexpr int64 GoldenChunkY = -2;
	constexpr int64 GoldenChunkZ = 3;

	FCrowdyActorId GoldenInstigator()
	{
		return FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
	}

	// Written into the routing block as 32 uppercase hexadecimal digits by FGameEventBody::AppendBody.
	FGuid GoldenTargetID()
	{
		return FGuid(0x99999999u, 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu);
	}

	FGuid GoldenEntityID()
	{
		return FGuid(0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
	}

	FGuid GoldenSenderID()
	{
		return FGuid(0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u);
	}

	// The class and layout identities the payloads carry. Literals, never a value recomputed from the
	// layout under test: a golden that derives its own key would still pass after the key moved.
	constexpr int64 GoldenPayloadClassID = 0x0A0B0C0D;
	constexpr int64 GoldenLayoutHash = 0x0102030405060708;
	constexpr int64 GoldenFunctionID = 0x1122334455667788;

	// An id no other fixture in this module registers, so the actor-state vector's type tag is fixed here
	// rather than inherited from whatever the path hash happens to produce.
	constexpr FCrowdyTypeID GoldenActorStateTypeID = 61201;
	constexpr FCrowdyClassID GoldenActorStateClassID = 0x0A0B0C0D;

	void FillGoldenHeader(ICrowdyMessage& Message)
	{
		Message.AppID = GoldenAppID;
		Message.ChunkX = GoldenChunkX;
		Message.ChunkY = GoldenChunkY;
		Message.ChunkZ = GoldenChunkZ;
		Message.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Exponential_Decay;
		Message.UUID = GoldenInstigator();
	}

	// The exact values every CrowdyState vector encodes. Each leaf kind the fixture carries gets a value
	// that is not its default, so a slot dropped from the blob shortens it rather than encoding as zero.
	void FillGoldenStateTarget(UCrowdyStateTestTarget& Target)
	{
		Target.RepInt = 42;
		Target.RepBigInt = static_cast<int64>(9000000001);
		Target.RepFloat = 1.25f;
		Target.RepDouble = -3.5;
		Target.bRepFlag = true;
		Target.RepByte = 200;
		Target.RepEnum = ECrowdyStateTestEnum::Gamma;
		Target.RepName = FName(TEXT("MyTag"));
		Target.RepString = TEXT("hello");
		Target.RepVector = FVector(1.0, -2.5, 3.25);
		Target.RepRotator = FRotator(10.0, 20.0, 30.0);
		Target.RepOwnerOnly = 7;
		Target.RepManualDirty = 9;
		Target.RepHealth = 0.5f;
	}

	// 68 header octets, then event type 54889, a 159-octet payload, the target mode and 32 target octets.
	// The payload is the delta's type tag, its class and identity fields, its layout hash and flags, and a
	// 104-octet blob: version 2, selector mode 0, the two mask octets 0xff 0x3f for all fourteen slots, and
	// the fourteen values in layout order.
	TArray<uint8> GoldenStateDeltaDatagram()
	{
		static const uint8 Bytes[] = {
			0x8a, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
			0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
			0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x69, 0xd6, 0x9f, 0x00, 0x00, 0x00, 0x69,
			0xd6, 0x0d, 0x0c, 0x0b, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22,
			0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x66,
			0x66, 0x66, 0x66, 0x77, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88, 0x88, 0x08, 0x07, 0x06, 0x05,
			0x04, 0x03, 0x02, 0x01, 0x01, 0x68, 0x00, 0x00, 0x00, 0x02, 0x00, 0xff, 0x3f, 0x2a, 0x00,
			0x00, 0x00, 0x01, 0x1a, 0x71, 0x18, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x3f, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xc0, 0x01, 0xc8, 0x02, 0x06, 0x00, 0x00, 0x00, 0x4d,
			0x79, 0x54, 0x61, 0x67, 0x00, 0x06, 0x00, 0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00,
			0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x04, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x40, 0x09, 0x00,
			0x00, 0x00, 0x01, 0x1c, 0x07, 0x01, 0x39, 0x0e, 0x01, 0x55, 0x15, 0x07, 0x00, 0x00, 0x00,
			0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x01, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39,
			0x39, 0x39, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x42, 0x42, 0x42,
			0x42, 0x42, 0x42, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43 };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	// The same header and the same routing block, with event type 57330 and a 59-octet payload: the call's
	// type tag, its class and identity fields, its function id, and the five parameter octets.
	TArray<uint8> GoldenRpcEventDatagram()
	{
		static const uint8 Bytes[] = {
			0x8a, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
			0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
			0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xf2, 0xdf, 0x3b, 0x00, 0x00, 0x00, 0xf2,
			0xdf, 0x0d, 0x0c, 0x0b, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22,
			0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x66,
			0x66, 0x66, 0x66, 0x77, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88, 0x88, 0x88, 0x77, 0x66, 0x55,
			0x44, 0x33, 0x22, 0x11, 0x05, 0x00, 0x00, 0x00, 0x01, 0xde, 0xad, 0xbe, 0xef, 0x01, 0x39,
			0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
			0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43,
			0x43 };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	// 68 header octets, a declared 27-octet state length, then the actor-state framing (a zero sentinel,
	// format version 1, type tag 61201, class id 0x0a0b0c0d) and the struct's own eighteen octets.
	TArray<uint8> GoldenActorUpdateDatagram()
	{
		static const uint8 Bytes[] = {
			0x80, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
			0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
			0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x11, 0xef, 0x0d, 0x0c, 0x0b, 0x0a, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x3f, 0x00,
			0x00, 0x20, 0xc0, 0x00, 0x00, 0x70, 0x40, 0xc8, 0x01 };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	// The body half of the same actor-update datagram, as the send adapter hands it to the bridge. Pinned
	// separately so the boundary between the header the adapter reads and the payload it forwards is
	// itself a stored fact, not merely a consequence of the whole datagram matching.
	TArray<uint8> GoldenActorUpdateSplitPayload()
	{
		static const uint8 Bytes[] = {
			0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0xef, 0x0d, 0x0c, 0x0b, 0x0a, 0x2a, 0x00,
			0x00, 0x00, 0x00, 0x00, 0xa0, 0x3f, 0x00, 0x00, 0x20, 0xc0, 0x00, 0x00, 0x70, 0x40, 0xc8,
			0x01 };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}
}

// The CrowdyState delta, assembled the way the replicator assembles one: a layout over the live fixture,
// a keyframe blob from the real codec, that blob inside an FCrowdyStateDelta, the delta through the real
// event serializer, and the whole thing framed as the request the transport is handed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOutboundGoldenStateDeltaTest,
	"CrowdySDK.Wire.GoldenOutboundStateDelta", CrowdyOutboundGoldenTestFlags)
bool FCrowdyOutboundGoldenStateDeltaTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyStateDelta::StaticStruct());

	// The tag that leads the serialized payload. Asserted against a literal here so a change to the id
	// generator, or a registry override, names itself instead of arriving as an unexplained byte diff.
	FCrowdyTypeID DeltaTypeID = CROWDY_INVALID_TYPE_ID;
	if (!TestTrue(TEXT("the delta payload is registered"),
		UEventPayloadRegistry::Get()->GetID(FCrowdyStateDelta::StaticStruct(), DeltaTypeID)))
	{
		return false;
	}
	TestEqual(TEXT("the delta payload is pinned to this exact path"),
		FCrowdyStateDelta::StaticStruct()->GetPathName(), FString(TEXT("/Script/CrowdyReplication.CrowdyStateDelta")));
	TestEqual(TEXT("the delta payload's derived wire type id"),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyStateDelta::StaticStruct())), 54889);
	TestEqual(TEXT("the delta payload's registered wire type id"), static_cast<int32>(DeltaTypeID), 54889);

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("the fixture's layout builds"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateTestTarget::StaticClass(), Layout)))
	{
		return false;
	}

	// The vector is a keyframe over the whole fixture, so a property lost from the layout changes the
	// selector as well as the body and cannot go unnoticed.
	TestEqual(TEXT("the fixture's layout is the property set this vector was taken over"),
		Layout.Properties.Num(), 14);

	UCrowdyStateTestTarget* Source = NewObject<UCrowdyStateTestTarget>();
	FillGoldenStateTarget(*Source);

	FCrowdyStateDelta Delta;
	Delta.ClassID = GoldenPayloadClassID;
	Delta.EntityID = GoldenEntityID();
	Delta.SenderID = GoldenSenderID();
	Delta.LayoutHash = GoldenLayoutHash;
	Delta.Flags = CrowdyStateDeltaFlags::Keyframe;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, Layout.Properties.Num()),
		/*bKeyframe=*/true, Delta.Blob);

	FGameEventRequest Request;
	FillGoldenHeader(Request);
	Request.EventType = static_cast<uint16>(DeltaTypeID);
	Request.Target = ECrowdyTarget::Entity;
	Request.TargetID = GoldenTargetID();

	if (!TestTrue(TEXT("the delta serializes as an event payload"),
		USerializationFunctionLibrary::SerializeEventState(FInstancedStruct::Make(Delta), Request.StateBytes)))
	{
		return false;
	}
	Request.StateSize = Request.StateBytes.Num();

	const TArray<uint8> Datagram = Request.Serialize();
	const TArray<uint8> Golden = GoldenStateDeltaDatagram();

	// Length first and on its own: a truncation and a corruption are different failures, and the offset
	// report below is only meaningful once the two buffers are the same size.
	TestEqual(TEXT("state delta datagram length"), Datagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("state delta datagram matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(Datagram, Golden), *HexOf(Datagram)), Datagram == Golden);

	// The same frame built the way the send actually builds it: the payload serialized straight into the
	// frame from a view, with the declared length patched in afterwards. Asserted against the SAME stored
	// bytes rather than against the buffer above, so this cannot pass by both routes drifting together.
	FGameEventRequest ViewRequest;
	FillGoldenHeader(ViewRequest);
	ViewRequest.EventType = static_cast<uint16>(DeltaTypeID);
	ViewRequest.Target = ECrowdyTarget::Entity;
	ViewRequest.TargetID = GoldenTargetID();
	ViewRequest.PayloadStruct = FCrowdyStateDelta::StaticStruct();
	ViewRequest.PayloadMemory = &Delta;
	ViewRequest.PayloadTypeID = DeltaTypeID;

	const TArray<uint8> ViewDatagram = ViewRequest.Serialize();
	TestEqual(TEXT("view-built state delta datagram length"), ViewDatagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("view-built state delta datagram matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(ViewDatagram, Golden), *HexOf(ViewDatagram)), ViewDatagram == Golden);

	return true;
}

// The RPC event: the same tail as the state delta, carrying the call struct FCrowdyRPC::RouteOverWire
// puts on the wire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOutboundGoldenRpcEventTest,
	"CrowdySDK.Wire.GoldenOutboundRpcEvent", CrowdyOutboundGoldenTestFlags)
bool FCrowdyOutboundGoldenRpcEventTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyRpcCall::StaticStruct());

	FCrowdyTypeID CallTypeID = CROWDY_INVALID_TYPE_ID;
	if (!TestTrue(TEXT("the RPC call payload is registered"),
		UEventPayloadRegistry::Get()->GetID(FCrowdyRpcCall::StaticStruct(), CallTypeID)))
	{
		return false;
	}
	// The id is derived from the path, so the path is asserted beside it: moving or renaming the struct
	// would change the vector for a reason that has nothing to do with the framing.
	TestEqual(TEXT("the RPC call payload is pinned to this exact path"),
		FCrowdyRpcCall::StaticStruct()->GetPathName(), FString(TEXT("/Script/CrowdyReplication.CrowdyRpcCall")));
	TestEqual(TEXT("the RPC call payload's derived wire type id"),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyRpcCall::StaticStruct())), 57330);
	TestEqual(TEXT("the RPC call payload's registered wire type id"), static_cast<int32>(CallTypeID), 57330);

	FCrowdyRpcCall Call;
	Call.ClassID = GoldenPayloadClassID;
	Call.EntityID = GoldenEntityID();
	Call.SenderID = GoldenSenderID();
	Call.FunctionID = GoldenFunctionID;
	Call.ParamBlob = { 0x01, 0xde, 0xad, 0xbe, 0xef };

	FGameEventRequest Request;
	FillGoldenHeader(Request);
	Request.EventType = static_cast<uint16>(CallTypeID);
	Request.Target = ECrowdyTarget::Entity;
	Request.TargetID = GoldenTargetID();

	if (!TestTrue(TEXT("the call serializes as an event payload"),
		USerializationFunctionLibrary::SerializeEventState(FInstancedStruct::Make(Call), Request.StateBytes)))
	{
		return false;
	}
	Request.StateSize = Request.StateBytes.Num();

	const TArray<uint8> Datagram = Request.Serialize();
	const TArray<uint8> Golden = GoldenRpcEventDatagram();

	TestEqual(TEXT("rpc event datagram length"), Datagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("rpc event datagram matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(Datagram, Golden), *HexOf(Datagram)), Datagram == Golden);

	// The same frame built the way the send actually builds it: the call serialized straight into the
	// frame from a view. Asserted against the SAME stored bytes, not against the buffer above.
	FGameEventRequest ViewRequest;
	FillGoldenHeader(ViewRequest);
	ViewRequest.EventType = static_cast<uint16>(CallTypeID);
	ViewRequest.Target = ECrowdyTarget::Entity;
	ViewRequest.TargetID = GoldenTargetID();
	ViewRequest.PayloadStruct = FCrowdyRpcCall::StaticStruct();
	ViewRequest.PayloadMemory = &Call;
	ViewRequest.PayloadTypeID = CallTypeID;

	const TArray<uint8> ViewDatagram = ViewRequest.Serialize();
	TestEqual(TEXT("view-built rpc event datagram length"), ViewDatagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("view-built rpc event datagram matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(ViewDatagram, Golden), *HexOf(ViewDatagram)), ViewDatagram == Golden);

	return true;
}

// The actor update: a plain-old-data state struct through the real actor-state serializer and its own
// request type, plus the payload the send adapter splits back out of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOutboundGoldenActorUpdateTest,
	"CrowdySDK.Wire.GoldenOutboundActorUpdate", CrowdyOutboundGoldenTestFlags)
bool FCrowdyOutboundGoldenActorUpdateTest::RunTest(const FString& Parameters)
{
	// Registering under an explicit id rather than the path hash: the vector's type tag is then a value
	// this file chose, and moving or renaming the fixture struct cannot silently move it.
	UActorUpdatePayloadRegistry::Get()->RegisterStruct(
		FCrowdyOutboundGoldenActorState::StaticStruct(), GoldenActorStateTypeID);

	FCrowdyTypeID StateTypeID = CROWDY_INVALID_TYPE_ID;
	if (!TestTrue(TEXT("the actor state payload is registered"),
		UActorUpdatePayloadRegistry::Get()->GetID(FCrowdyOutboundGoldenActorState::StaticStruct(), StateTypeID)))
	{
		return false;
	}
	TestEqual(TEXT("the actor state payload carries the id this vector was taken under"),
		static_cast<int32>(StateTypeID), static_cast<int32>(GoldenActorStateTypeID));

	FCrowdyOutboundGoldenActorState State;
	State.Sequence = 42;
	State.PositionX = 1.25f;
	State.PositionY = -2.5f;
	State.PositionZ = 3.75f;
	State.Stance = 200;
	State.bAirborne = true;

	FActorUpdateRequestMessage Request;
	FillGoldenHeader(Request);

	if (!TestTrue(TEXT("the actor state serializes"),
		USerializationFunctionLibrary::SerializeActorState(
			FInstancedStruct::Make(State), GoldenActorStateClassID, Request.StateBytes)))
	{
		return false;
	}
	Request.StateSize = Request.StateBytes.Num();

	const TArray<uint8> Datagram = Request.Serialize();
	const TArray<uint8> Golden = GoldenActorUpdateDatagram();

	TestEqual(TEXT("actor update datagram length"), Datagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("actor update datagram matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(Datagram, Golden), *HexOf(Datagram)), Datagram == Golden);

	// The storage outlives the frame: the frame's payload is a view into it, not a copy.
	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;

	// Split first, then assert. Reading Error inside an argument to the call that fills it leaves the two
	// unsequenced, so the reason could come out empty in exactly the run that needs it.
	const bool bSplit = CrowdyCppSend::SplitMessage(Request, Storage, Frame, Error);
	if (!TestTrue(FString::Printf(TEXT("the actor update splits (%s)"), *Error), bSplit))
	{
		return false;
	}

	const TArray<uint8> SplitPayload(Frame.Payload.GetData(), Frame.Payload.Num());
	const TArray<uint8> GoldenSplit = GoldenActorUpdateSplitPayload();

	TestEqual(TEXT("split payload length"), SplitPayload.Num(), GoldenSplit.Num());
	TestTrue(FString::Printf(TEXT("split payload matches the stored bytes (%s) actual=%s"),
		*DescribeDifference(SplitPayload, GoldenSplit), *HexOf(SplitPayload)), SplitPayload == GoldenSplit);

	return true;
}

// Serializing the payload straight into the frame moved WHERE an unencodable payload is caught. It used to
// be caught by the caller, which encoded into its own buffer and abandoned the send when that failed; the
// body writer has no way to abandon anything, so the frame has to come out empty instead. That matters
// because the send path's only refusal for this is "the message serialized to nothing": a body that wrote a
// partial payload under a length describing it would be sent, and read as a different value on arrival.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOutboundUnencodablePayloadTest,
	"CrowdySDK.Wire.OutboundUnencodablePayloadSerializesToNothing", CrowdyOutboundGoldenTestFlags)
bool FCrowdyOutboundUnencodablePayloadTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("[AppendEventState]"), EAutomationExpectedErrorFlags::Contains, 0);

	FGameEventRequest Request;
	FillGoldenHeader(Request);
	Request.Target = ECrowdyTarget::Everyone;

	// A view naming a struct with no memory behind it: the shape the encoder refuses, reached through the
	// same field the send path fills in.
	Request.PayloadStruct = FCrowdyRpcCall::StaticStruct();
	Request.PayloadMemory = nullptr;
	Request.PayloadTypeID = 1;

	// Not merely "shorter than a good frame": the send path refuses on emptiness alone, so anything at all
	// here is a frame that goes out.
	TestEqual(TEXT("a payload that cannot be encoded produces no frame at all"),
		Request.Serialize().Num(), 0);

	// The header alone would be a well-formed-looking frame, so prove the refusal is not simply the body
	// being skipped: the same request with a real payload still serializes.
	FCrowdyRpcCall Call;
	Call.ParamBlob = { 0x01 };
	Request.PayloadMemory = &Call;
	TestTrue(TEXT("and the same request with an encodable payload still produces one"),
		Request.Serialize().Num() > 0);

	return true;
}

// Which of the three vectors above is a live gate on the baked copy plan, and which two are gates on it
// being refused. The two event envelopes each end in a byte array, whose width is its length prefix
// rather than its storage, so they keep the reflective walk; the actor state is packed leaves and takes
// the plan, which makes GoldenOutboundActorUpdate the byte-for-byte proof that the plan's write side
// puts the same octets on the wire as the walk it replaced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGoldenVectorPlanCoverageTest,
	"CrowdySDK.Wire.GoldenVectorsSayWhichTakeTheBakedPlan", CrowdyOutboundGoldenTestFlags)
bool FCrowdyGoldenVectorPlanCoverageTest::RunTest(const FString& Parameters)
{
	const auto Refused = [this](const TCHAR* What, const UScriptStruct* Struct, const TCHAR* NamedMember)
	{
		FString Reason;
		const bool bValid = CrowdyPodCopyPlan::Build(Struct, Reason).IsValid();
		TestFalse(FString::Printf(TEXT("%s keeps the reflective walk (%s)"), What, *Reason), bValid);
		TestTrue(FString::Printf(TEXT("and the refusal of %s names '%s'"), What, NamedMember),
			Reason.Contains(NamedMember));
	};

	Refused(TEXT("the CrowdyState delta envelope"), FCrowdyStateDelta::StaticStruct(), TEXT("Blob"));
	Refused(TEXT("the RPC call envelope"), FCrowdyRpcCall::StaticStruct(), TEXT("ParamBlob"));

	FString ActorStateReason;
	const FCrowdyCopyPlan ActorStatePlan =
		CrowdyPodCopyPlan::Build(FCrowdyOutboundGoldenActorState::StaticStruct(), ActorStateReason);

	TestTrue(FString::Printf(TEXT("the actor-state vector's struct does take a plan (%s)"), *ActorStateReason),
		ActorStatePlan.IsValid());

	// The vector pins eighteen body octets, and the struct occupies twenty. A plan that included the two
	// trailing pad bytes would move a width the walk never wrote, so this is the width claim itself.
	TestEqual(TEXT("and the plan's width is the vector's body, not the struct's size"),
		ActorStatePlan.TotalBytes, 18);

	// The SDK's own default actor state, which is what an entity replicates with no executor of its own.
	// A transform is two structs that serialize themselves, so this is also the assertion that the probe
	// lets FVector and FRotator through rather than the plan quietly falling back for every real update.
	FString DefaultStateReason;
	const FCrowdyCopyPlan DefaultStatePlan =
		CrowdyPodCopyPlan::Build(FCrowdyActorState::StaticStruct(), DefaultStateReason);

	TestTrue(FString::Printf(TEXT("the SDK's default actor state takes a plan (%s)"), *DefaultStateReason),
		DefaultStatePlan.IsValid());
	TestEqual(TEXT("of one span, because a transform is packed"), DefaultStatePlan.Runs.Num(), 1);
	TestEqual(TEXT("covering both structs and nothing else"), DefaultStatePlan.TotalBytes, 48);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
