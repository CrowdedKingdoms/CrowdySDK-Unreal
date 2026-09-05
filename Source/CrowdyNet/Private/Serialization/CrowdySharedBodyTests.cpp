#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/Communication/FClientAudioPacketMessageRequest.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/Voxel/FVoxelStateUpdateRequest.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"

#include "CrowdyWireParitySupport.h"

// The outbound and inbound halves of a message pair now share one description of their payload, so what
// needs pinning is that the shared description still writes exactly what each half used to write, and
// that a frame one half produces is a frame the other half reads.
namespace
{
	constexpr EAutomationTestFlags CrowdySharedBodyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** The body a spatial message serialized: everything after the fixed header. */
	TArray<uint8> SharedBodyOf(const ICrowdyMessage& Message)
	{
		const TArray<uint8> Serialized = Message.Serialize();
		if (Serialized.Num() <= CrowdySpatialHeader::Size)
		{
			return TArray<uint8>();
		}

		return TArray<uint8>(Serialized.GetData() + CrowdySpatialHeader::Size,
			Serialized.Num() - CrowdySpatialHeader::Size);
	}

	/** Same length, same octets. A borrowed view and an owned array have no operator== between them. */
	bool SharedBodyOctetsEqual(const TConstArrayView<uint8> A, const TConstArrayView<uint8> B)
	{
		return A.Num() == B.Num() && (A.IsEmpty() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0);
	}

	/** The whole datagram minus its leading opcode, which is the form a decoder is handed. */
	TArray<uint8> SharedBodyStrippedDatagram(const ICrowdyMessage& Message, const uint8 Sequence)
	{
		const TArray<uint8> Datagram = CrowdyWireParity::AppendSignedTail(
			Message.Serialize(), CrowdyWireParity::GoldenToken(), 123456789, Sequence, true);

		return TArray<uint8>(Datagram.GetData() + 1, Datagram.Num() - 1);
	}
}

// Every one of these layouts writes a length the sender set rather than one derived from the bytes
// behind it. That is deliberate: the sender is the only party that knows what it meant to send, and a
// mismatch is a frame worth surfacing rather than one worth silently correcting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySharedBodyDeclaredLengthTest,
	"CrowdySDK.Wire.SharedBodyDeclaresTheLengthTheSenderSet", CrowdySharedBodyTestFlags)
bool FCrowdySharedBodyDeclaredLengthTest::RunTest(const FString& Parameters)
{
	{
		// An actor update declaring a shorter length than it carries. The declared length is what goes on
		// the wire; the state octets all follow it.
		FActorUpdateRequestMessage Message;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.StateBytes = { 0x01, 0x02, 0x03, 0x04, 0x05 };
		Message.StateSize = 3;

		const TArray<uint8> Body = SharedBodyOf(Message);

		TestEqual(TEXT("an actor body is the declared length followed by the state"), Body.Num(),
			static_cast<int32>(sizeof(int32)) + Message.StateBytes.Num());
		if (Body.Num() >= static_cast<int32>(sizeof(int32)))
		{
			int32 Declared = 0;
			FMemory::Memcpy(&Declared, Body.GetData(), sizeof(int32));
			TestEqual(TEXT("and the length written is the one the sender set"), Declared, 3);
		}
	}

	{
		// A voxel update that advertises a state length while suppressing the state itself. Nothing the
		// shared encoder can produce looks like this, and it must keep being producible here.
		FVoxelStateUpdateRequest Message;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.Vx = -17;
		Message.Vy = 300;
		Message.Vz = 31;
		Message.VoxelType = -2;
		Message.StateBytes = { 0x10, 0x20, 0x30 };
		Message.StateSize = 3;
		Message.bContainsState = false;

		const TArray<uint8> Body = SharedBodyOf(Message);

		constexpr int32 VoxelFixedSize = 5 * static_cast<int32>(sizeof(int16));
		TestEqual(TEXT("a suppressed state leaves only the fixed voxel fields"), Body.Num(), VoxelFixedSize);

		if (Body.Num() == VoxelFixedSize)
		{
			int16 Vx = 0;
			int16 Vy = 0;
			int16 Vz = 0;
			int16 VoxelType = 0;
			uint16 Declared = 0;
			FMemory::Memcpy(&Vx, Body.GetData(), sizeof(int16));
			FMemory::Memcpy(&Vy, Body.GetData() + 2, sizeof(int16));
			FMemory::Memcpy(&Vz, Body.GetData() + 4, sizeof(int16));
			FMemory::Memcpy(&VoxelType, Body.GetData() + 6, sizeof(int16));
			FMemory::Memcpy(&Declared, Body.GetData() + 8, sizeof(uint16));

			// Read back one at a time rather than as a block, so a transposition names the field it moved.
			TestEqual(TEXT("vx is first"), static_cast<int32>(Vx), -17);
			TestEqual(TEXT("vy is second"), static_cast<int32>(Vy), 300);
			TestEqual(TEXT("vz is third"), static_cast<int32>(Vz), 31);
			TestEqual(TEXT("the voxel type is fourth"), static_cast<int32>(VoxelType), -2);
			TestEqual(TEXT("and the declared state length is still written with no state behind it"),
				static_cast<int32>(Declared), 3);
		}
	}

	return true;
}

// The audio layout is the one exception, and the reason is that its frame type now carries a length
// field that only the receive path fills in. The encoder has to write the octets it is actually
// appending, or a frame whose audio was replaced would advertise the length of the audio it replaced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySharedBodyAudioLengthTest,
	"CrowdySDK.Wire.SharedBodyAudioDeclaresTheOctetsItSends", CrowdySharedBodyTestFlags)
bool FCrowdySharedBodyAudioLengthTest::RunTest(const FString& Parameters)
{
	FClientAudioPacketMessageRequest Message;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.SampleRate = 48000;
	Message.NumChannels = 2;

	FCrowdyAudioFrame Frame;
	Frame.AudioData = { 0xd1, 0xd2, 0xd3 };

	// A length left over from somewhere else. The encoder must ignore it.
	Frame.FrameSize = 99;
	Message.Frames.Add(Frame);

	const TArray<uint8> Body = SharedBodyOf(Message);

	// Sample rate, channel count, frame count, then this frame's own length and its octets.
	const int32 ExpectedLength = 4 * static_cast<int32>(sizeof(int32)) + Frame.AudioData.Num();
	TestEqual(TEXT("an audio body is the header fields, the frame length and the audio"), Body.Num(),
		ExpectedLength);

	if (Body.Num() == ExpectedLength)
	{
		int32 SampleRate = 0;
		int32 NumChannels = 0;
		int32 FrameCount = 0;
		int32 DeclaredFrameLength = 0;
		FMemory::Memcpy(&SampleRate, Body.GetData(), sizeof(int32));
		FMemory::Memcpy(&NumChannels, Body.GetData() + 4, sizeof(int32));
		FMemory::Memcpy(&FrameCount, Body.GetData() + 8, sizeof(int32));
		FMemory::Memcpy(&DeclaredFrameLength, Body.GetData() + 12, sizeof(int32));

		TestEqual(TEXT("the sample rate is first"), SampleRate, 48000);
		TestEqual(TEXT("the channel count is second"), NumChannels, 2);
		TestEqual(TEXT("the frame count is third"), FrameCount, 1);
		TestEqual(TEXT("and a frame declares the octets that follow it, not the length it arrived with"),
			DeclaredFrameLength, 3);
	}

	return true;
}

// The target id sits at the very end of an event body with nothing behind it, but it is still written at
// a fixed width, because the block in front of it is optional and a reader decides whether it is present
// by counting the bytes that are left.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySharedBodyTargetIdWidthTest,
	"CrowdySDK.Wire.SharedBodyTargetIdIsAlwaysThirtyTwoOctets", CrowdySharedBodyTestFlags)
bool FCrowdySharedBodyTargetIdWidthTest::RunTest(const FString& Parameters)
{
	auto BodyForTarget = [](const FGuid& TargetID)
	{
		FGameEventRequest Message;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.EventType = 0xBEEF;
		Message.StateBytes = { 0xca, 0xfe };
		Message.StateSize = Message.StateBytes.Num();
		Message.Target = ECrowdyTarget::Everyone;
		Message.TargetID = TargetID;
		return SharedBodyOf(Message);
	};

	// Event type, declared length, the state, one target octet, then the id.
	const int32 ExpectedLength = static_cast<int32>(sizeof(uint16)) + static_cast<int32>(sizeof(int32))
		+ 2 + 1 + FCrowdyActorId::NumOctets;

	const TArray<uint8> Populated = BodyForTarget(FGuid(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00));
	const TArray<uint8> Unset = BodyForTarget(FGuid());

	TestEqual(TEXT("an event body with a target id is the length the layout describes"), Populated.Num(),
		ExpectedLength);
	TestEqual(TEXT("and an event body with no target id is exactly as long"), Unset.Num(), ExpectedLength);

	// Everything ahead of the id has to be untouched by what the id holds, which is what keeps the
	// optional block findable by counting backwards from the end.
	const int32 IdOffset = ExpectedLength - FCrowdyActorId::NumOctets;
	TestTrue(TEXT("nothing ahead of the target id depends on the target id"),
		Populated.Num() == Unset.Num()
			&& FMemory::Memcmp(Populated.GetData(), Unset.GetData(), IdOffset) == 0);

	// And the id really is the hexadecimal form of the value, since that is what the reader parses back.
	const FCrowdyActorId Expected = FCrowdyActorId::FromStringOrUnset(
		FGuid(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00).ToString(EGuidFormats::Digits));
	TestTrue(TEXT("the target id is written as its 32 hexadecimal digits"),
		Expected.IsSet()
			&& FMemory::Memcmp(Populated.GetData() + IdOffset, Expected.Octets, FCrowdyActorId::NumOctets) == 0);

	return true;
}

// The claim a shared layout type makes is that the two halves describe the same bytes. Nothing asserted
// that before, because each half was written out separately and only ever tested on its own side.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySharedBodyRoundTripTest,
	"CrowdySDK.Wire.SharedBodyRequestAndNotificationAgree", CrowdySharedBodyTestFlags)
bool FCrowdySharedBodyRoundTripTest::RunTest(const FString& Parameters)
{
	// The event state below carries a type id nothing registers. That is reported as a rate-gated
	// warning, so it fails nothing here and cannot be expected a fixed number of times either. The
	// decode still succeeds and every assertion here is about the fields around it.
	{
		FGameEventRequest Request;
		Request.UUID = CrowdyWireParity::GoldenActorId();
		Request.AppID = 13;
		Request.ChunkX = -8;
		Request.ChunkY = 9;
		Request.ChunkZ = 10;
		Request.EventType = 0x1234;
		Request.StateBytes = { 0xFF, 0xFF, 0xaa, 0xbb, 0xcc };
		Request.StateSize = Request.StateBytes.Num();
		Request.Target = ECrowdyTarget::AllExceptSender;
		Request.TargetID = FGuid(1, 2, 3, 4);

		// Named rather than passed inline: a decoded notification points at these octets rather than owning
		// them, so the datagram has to outlive every assertion made about what it carried.
		const TArray<uint8> Datagram = SharedBodyStrippedDatagram(Request, 7);

		FGameEventNotification Notification;
		if (TestTrue(TEXT("an event a request wrote decodes as a notification"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, Datagram)))
		{
			TestEqual(TEXT("event type survives"), static_cast<int32>(Notification.EventType), 0x1234);
			TestEqual(TEXT("declared state length survives"), Notification.StateSize, Request.StateSize);
			TestTrue(TEXT("state octets survive"),
				SharedBodyOctetsEqual(Notification.StateView, Request.StateBytes));
			TestEqual(TEXT("the routing target survives"), static_cast<int32>(Notification.Target),
				static_cast<int32>(ECrowdyTarget::AllExceptSender));
			TestEqual(TEXT("the target id survives"), Notification.TargetID, Request.TargetID);
			TestEqual(TEXT("and so does the actor id in the header"), Notification.UUID, Request.UUID);
		}
	}

	{
		FClientAudioPacketMessageRequest Request;
		Request.UUID = CrowdyWireParity::GoldenActorId();
		Request.AppID = 21;
		Request.SampleRate = 24000;
		Request.NumChannels = 1;

		FCrowdyAudioFrame First;
		First.AudioData = { 0x01, 0x02, 0x03 };
		FCrowdyAudioFrame Second;
		Second.AudioData = { 0x04, 0x05 };
		Request.Frames.Add(First);
		Request.Frames.Add(Second);

		const TArray<uint8> Datagram = SharedBodyStrippedDatagram(Request, 3);

		FClientAudioNotification Notification;
		if (TestTrue(TEXT("a voice packet a request wrote decodes as a notification"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, Datagram)))
		{
			TestEqual(TEXT("sample rate survives"), Notification.SampleRate, 24000);
			TestEqual(TEXT("channel count survives"), Notification.NumChannels, 1);
			if (TestEqual(TEXT("both frames survive"), Notification.Frames.Num(), 2))
			{
				TestEqual(TEXT("the first frame reports the length it was sent with"),
					Notification.Frames[0].FrameSize, First.AudioData.Num());
				TestTrue(TEXT("and its audio"), Notification.Frames[0].AudioData == First.AudioData);
				TestEqual(TEXT("the second frame reports the length it was sent with"),
					Notification.Frames[1].FrameSize, Second.AudioData.Num());
				TestTrue(TEXT("and its audio"), Notification.Frames[1].AudioData == Second.AudioData);
			}
		}
	}

	{
		FActorUpdateRequestMessage Request;
		Request.UUID = CrowdyWireParity::GoldenActorId();
		Request.AppID = 31;
		Request.ChunkX = 4;
		Request.ChunkY = -5;
		Request.ChunkZ = 6;
		Request.StateBytes = { 0xFF, 0xFF, 0x0a, 0x0b, 0x0c };
		Request.StateSize = Request.StateBytes.Num();

		// Named rather than passed inline: a decoded notification points at these octets rather than owning
		// them, so the datagram has to outlive every assertion made about what it carried.
		const TArray<uint8> Datagram = SharedBodyStrippedDatagram(Request, 5);

		FActorUpdateNotificationMessage Notification;
		if (TestTrue(TEXT("an actor update a request wrote decodes as a notification"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, Datagram)))
		{
			TestEqual(TEXT("the declared state length survives"), Notification.StateSize, Request.StateSize);
			TestTrue(TEXT("state octets survive"),
				SharedBodyOctetsEqual(Notification.StateView, Request.StateBytes));
			TestEqual(TEXT("the chunk the update was sent from survives"), Notification.ChunkY, Request.ChunkY);
			TestEqual(TEXT("and so does the actor id in the header"), Notification.UUID, Request.UUID);

			// The key the receiving side indexes an actor by is derived from the id that arrived rather
			// than carried on the wire, so it is only right if the id is.
			TestEqual(TEXT("and the key derived from that id"), Notification.GUID,
				USerializationFunctionLibrary::ToGuid(Request.UUID));
		}
	}

	{
		FVoxelStateUpdateRequest Request;
		Request.UUID = CrowdyWireParity::GoldenActorId();
		Request.AppID = 44;
		Request.Vx = -17;
		Request.Vy = 300;
		Request.Vz = 31;
		Request.VoxelType = -2;
		Request.StateBytes = { 0x10, 0x20, 0x30 };
		Request.StateSize = 3;
		Request.bContainsState = true;

		const TArray<uint8> Datagram = SharedBodyStrippedDatagram(Request, 8);

		FVoxelUpdateNotificationMessage Notification;
		if (TestTrue(TEXT("a voxel update a request wrote decodes as a notification"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, Datagram)))
		{
			// Four different values read back one at a time, so a pair of fields that swapped places names
			// itself instead of passing because both happened to hold the same number.
			TestEqual(TEXT("vx survives"), static_cast<int32>(Notification.Vx), -17);
			TestEqual(TEXT("vy survives"), static_cast<int32>(Notification.Vy), 300);
			TestEqual(TEXT("vz survives"), static_cast<int32>(Notification.Vz), 31);
			TestEqual(TEXT("the voxel type survives"), static_cast<int32>(Notification.VoxelType), -2);

			TestEqual(TEXT("the declared state length survives"),
				static_cast<int32>(Notification.StateSize), 3);
			TestTrue(TEXT("state octets survive"), Notification.StateBytes == Request.StateBytes);
			TestTrue(TEXT("and the update reports that it carried state"), Notification.bContainsState);
			TestEqual(TEXT("and so does the actor id in the header"), Notification.UUID, Request.UUID);
		}
	}

	return true;
}

// A voxel update writes the length its sender declared whether or not it sends any state behind it, so
// this is the one shared layout whose two halves can describe a frame they do not agree on. What the
// reader does with that frame is the rest of the story the declared length starts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySharedBodyVoxelSuppressedStateTest,
	"CrowdySDK.Wire.SharedBodyVoxelSuppressedStateStillDeclaresItsLength", CrowdySharedBodyTestFlags)
bool FCrowdySharedBodyVoxelSuppressedStateTest::RunTest(const FString& Parameters)
{
	auto BuildRequest = [](const uint16 DeclaredLength)
	{
		FVoxelStateUpdateRequest Request;
		Request.UUID = CrowdyWireParity::GoldenActorId();
		Request.Vx = -17;
		Request.Vy = 300;
		Request.Vz = 31;
		Request.VoxelType = -2;
		Request.StateBytes = { 0x10, 0x20, 0x30 };
		Request.StateSize = DeclaredLength;
		Request.bContainsState = false;
		return Request;
	};

	constexpr int32 VoxelFixedSize = 5 * static_cast<int32>(sizeof(int16));

	{
		// A length with nothing behind it. The body is the fixed fields alone, so the length the reader
		// takes off it runs past the end of the frame, and the update is refused rather than read short.
		const FVoxelStateUpdateRequest Request = BuildRequest(static_cast<uint16>(3));

		TestEqual(TEXT("a suppressed state still writes the length its sender declared"),
			SharedBodyOf(Request).Num(), VoxelFixedSize);

		FVoxelUpdateNotificationMessage Notification;
		TestFalse(TEXT("and the reader refuses a length that runs past the end of the frame"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, SharedBodyStrippedDatagram(Request, 9)));
	}

	{
		// Suppressing the state and declaring none is the well-formed way to send an update that carries
		// no state, and it reads back as exactly that rather than as a truncated frame.
		const FVoxelStateUpdateRequest Request = BuildRequest(static_cast<uint16>(0));

		TestEqual(TEXT("declaring no state writes the same fixed fields"),
			SharedBodyOf(Request).Num(), VoxelFixedSize);

		const TArray<uint8> Datagram = SharedBodyStrippedDatagram(Request, 10);

		FVoxelUpdateNotificationMessage Notification;
		if (TestTrue(TEXT("an update declaring no state decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Notification, Datagram)))
		{
			TestEqual(TEXT("the voxel it names survives"), static_cast<int32>(Notification.Vx), -17);
			TestEqual(TEXT("the voxel type survives"), static_cast<int32>(Notification.VoxelType), -2);
			TestEqual(TEXT("nothing is declared"), static_cast<int32>(Notification.StateSize), 0);
			TestFalse(TEXT("the update reports carrying no state"), Notification.bContainsState);
			TestEqual(TEXT("and no state octets came with it"), Notification.StateBytes.Num(), 0);
		}
	}

	return true;
}

#endif
