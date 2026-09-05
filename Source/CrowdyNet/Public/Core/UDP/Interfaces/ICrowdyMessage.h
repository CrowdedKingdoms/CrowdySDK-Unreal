#pragma once
#include "CrowdyNetLog.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Serialization/CrowdyActorId.h"
#include "Serialization/CrowdyFrame.h"
#include "StructUtils/InstancedStruct.h"
#include "Utils/SerializationFunctionLibrary.h"

constexpr  int32 MetadataSize =  sizeof(int64)    // MapID
		+ sizeof(int64)  // ChunkX
		+ sizeof(int64)  // ChunkY
		+ sizeof(int64)  // ChunkZ
		+ sizeof(uint8)  // ReplicationDistance
		+ sizeof(uint8)  // DecayRate
		+ sizeof(uint8)  // bContainsAuth
		+ 32;            // actor id

constexpr int32 TailSize = sizeof(int64) + sizeof(uint8);

/**
 * How much room to leave for a body whose encoded size cannot be known before it is written, which is the
 * case whenever a message serializes its payload straight into the frame.
 *
 * Purely an allocation hint, sized to one datagram so a frame that fits in one never has to grow. It is
 * NOT the bound: the real ceiling is enforced in the transport against the assembled frame, and a body
 * past this simply grows the array the way every body did before.
 */
constexpr int32 CrowdyFrameReserveHint = 1232;

/**
 * Where each field of the spatial header sits, counted from the opcode that leads it.
 *
 * Stated as offsets rather than as a sequence of appends so that the order of the writes cannot change
 * what the header means, and so that each position can be checked against the shared protocol table.
 */
namespace CrowdySpatialHeader
{
	constexpr int32 OpcodeOffset = 0;
	constexpr int32 AppIdOffset = OpcodeOffset + sizeof(uint8);
	constexpr int32 ChunkXOffset = AppIdOffset + sizeof(int64);
	constexpr int32 ChunkYOffset = ChunkXOffset + sizeof(int64);
	constexpr int32 ChunkZOffset = ChunkYOffset + sizeof(int64);
	constexpr int32 DistanceOffset = ChunkZOffset + sizeof(int64);
	constexpr int32 DecayOffset = DistanceOffset + sizeof(uint8);
	constexpr int32 ContainsAuthOffset = DecayOffset + sizeof(uint8);
	constexpr int32 ActorIdOffset = ContainsAuthOffset + sizeof(uint8);
	constexpr int32 Size = ActorIdOffset + FCrowdyActorId::NumOctets;

	static_assert(Size == MetadataSize + 1,
		"The spatial header is no longer the size the metadata block adds up to.");
}

/**
 * Interface representing a generic message in the Crowdy system.
 */
class CROWDYNET_API ICrowdyMessage
{
public:
	
	FCrowdyActorId UUID;

	int64 AppID = 0;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;
	int64 Timestamp = 0;
	
	ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
	ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay;
	uint8 SequenceNumber = 0;
	bool bContainsAuth = true;
	
public:
	
	virtual ~ICrowdyMessage() = default;
	
	virtual ECrowdyMessageType GetType() const = 0;
	
	virtual FName GetTypeName() const = 0;
	
	virtual TArray<uint8> Serialize() const = 0;
	
	/**
	 * Reads this message out of the frame it arrived in. Returns false when the bytes do not form a
	 * message of this type, in which case the message is discarded.
	 *
	 * A decoder may keep a view into the frame's bytes instead of copying them out, and the notification
	 * types do, so the frame's octets must stay valid until this message has finished being delivered.
	 * Anything that has to outlive the DELIVERY, rather than merely this call, must be copied in here.
	 */
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) = 0;

	/**
	 * What this message carries, independent of the opcode that carried it. A message that
	 * returns a set key is routed by that key; everything else is routed by its opcode.
	 */
	virtual FCrowdyPayloadKey GetPayloadKey() const { return {}; }

	/**
	 * The decoded payload, or null when the carrier ships raw application bytes or when no
	 * struct is registered locally for the type number that arrived.
	 */
	virtual const FInstancedStruct* GetPayload() const { return nullptr; }

	/**
	 * The payload exactly as it arrived. Present even when GetPayload() is null. Borrowed from the frame
	 * the message decoded from, so it is valid only for the duration of the call it is handed to.
	 */
	virtual TConstArrayView<uint8> GetPayloadBytes() const { return {}; }

protected:

	/**
	 * Copies the frame's envelope onto this message.
	 *
	 * The actor id is deliberately not copied. What those 32 octets hold depends on the layout: most
	 * carry text, one carries two binary GUIDs, and a server-originated channel notification carries
	 * nothing meaningful at all, so a message reads them the way its own layout describes.
	 */
	FORCEINLINE void ApplyEnvelope(const FCrowdyFrame& Frame)
	{
		AppID = Frame.Envelope.AppId;
		ChunkX = Frame.Envelope.ChunkX;
		ChunkY = Frame.Envelope.ChunkY;
		ChunkZ = Frame.Envelope.ChunkZ;
		ReplicationDistance = static_cast<ECrowdyReplicationDistance>(Frame.Envelope.Distance);
		DecayRate = static_cast<ECrowdyDecayRate>(Frame.Envelope.Decay);
		bContainsAuth = Frame.Envelope.bContainsAuth;
		Timestamp = Frame.Envelope.Timestamp;
		SequenceNumber = Frame.Envelope.Sequence;
	}

	/**
	 * Copies the frame's actor id onto this message. Returns false when the frame carries no envelope and
	 * so has no actor id to copy, which is a frame this message's layout cannot describe.
	 *
	 * The octets are carried across verbatim. Most layouts put 32 characters of text there, but that is a
	 * convention rather than a guarantee, so nothing is decoded here and nothing is rejected for holding
	 * something other than text.
	 */
	FORCEINLINE [[nodiscard]] bool ApplyEnvelopeActorId(const FCrowdyFrame& Frame)
	{
		return FCrowdyActorId::TryFromOctets(Frame.Envelope.Uuid, UUID);
	}

	/**
	 * The header every spatial message leads with, written at fixed offsets into a buffer sized from the
	 * layout. Subclasses append their own body onto what this returns.
	 *
	 * The actor id occupies exactly the octets the layout reserves for it whatever it holds, so the header
	 * is always the same length and every field behind the id stays where a reader expects to find it.
	 */
	FORCEINLINE TArray<uint8> SerializeMetadata(const int32 BodyBytes = 0) const
	{
		using namespace CrowdySpatialHeader;

		TArray<uint8> Data;

		// Sized for the whole datagram before a single octet is written, so appending the body cannot
		// reallocate and re-copy the header behind it. A message with no body passes nothing and gets
		// exactly the header, as before.
		Data.Reserve(Size + BodyBytes);
		Data.SetNumUninitialized(Size);
		uint8* const Header = Data.GetData();

		Header[OpcodeOffset] = static_cast<uint8>(GetType());
		USerializationFunctionLibrary::WriteValue(Header + AppIdOffset, AppID);
		USerializationFunctionLibrary::WriteValue(Header + ChunkXOffset, ChunkX);
		USerializationFunctionLibrary::WriteValue(Header + ChunkYOffset, ChunkY);
		USerializationFunctionLibrary::WriteValue(Header + ChunkZOffset, ChunkZ);
		Header[DistanceOffset] = static_cast<uint8>(ReplicationDistance);
		Header[DecayOffset] = static_cast<uint8>(DecayRate);
		Header[ContainsAuthOffset] = bContainsAuth ? static_cast<uint8>(1) : static_cast<uint8>(0);
		UUID.WriteTo(Header + ActorIdOffset);

		return Data;
	}

};
