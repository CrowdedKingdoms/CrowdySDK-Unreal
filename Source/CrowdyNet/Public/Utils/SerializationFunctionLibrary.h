#pragma once
#include "CrowdyNetLog.h"
#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include <type_traits>
#include <cstring>
#include "Dom/JsonObject.h"
#include "Serialization/CrowdyActorId.h"
#include "StructUtils/InstancedStruct.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include "openssl/hmac.h"
THIRD_PARTY_INCLUDES_END
#undef UI

#include "SerializationFunctionLibrary.generated.h"

/**
 * @class USerializationFunctionLibrary
 * @brief A utility class providing static methods for data serialization and deserialization.
 *
 * The USerializationFunctionLibrary class offers a set of functions for
 * converting data between in-memory structures and a serialized format,
 * and vice versa. It is designed to facilitate the process of saving and
 * loading data in a structured or compact form.
 *
 * This class is commonly used for tasks such as saving game states,
 * transferring data over networks, or storing configuration files. It
 * ensures that data can be efficiently serialized and reliably
 * deserialized without data loss or corruption.
 */
UCLASS()
class CROWDYNET_API USerializationFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	template<typename T>
	/**
	 * Serializes the given value into a format suitable for storage or transmission.
	 *
	 * @param Value The value to be serialized. The type of the value must be compatible
	 *              with the serialization logic implemented in this method.
	 * @return A serialized representation of the input value as a string. The output
	 *         format will depend on the serialization implementation.
	 */
	static TArray<uint8> SerializeValue(T Value);

	/** Writes Value's octets to a buffer the caller has already sized. */
	template <typename T>
	static void WriteValue(uint8* Dest, T Value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");
		FMemory::Memcpy(Dest, &Value, sizeof(T));
	}

	/** Appends Value's octets. Unlike SerializeValue it builds no intermediate array. */
	template <typename T>
	static void AppendValue(TArray<uint8>& OutBytes, T Value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");
		const int32 At = OutBytes.AddUninitialized(static_cast<int32>(sizeof(T)));
		WriteValue(OutBytes.GetData() + At, Value);
	}

	template<typename T>
	/**
	 * Deserializes a given string into its corresponding object representation.
	 *
	 * @param Data The serialized string representation of the object.
	 * @param OutValue A reference to the object that will hold the deserialized value.
	 * @param Offset The starting index in the byte array from which to begin deserialization.
	 * @return True if deserialization is successful, false otherwise.
	 */
	static bool DeserializeValue(TConstArrayView<uint8> Data, T& OutValue, int Offset = 0);


	/**
	 * Deserializes a serialized string representation into its original form or object.
	 *
	 * @param Payload The serialized string that needs to be deserialized.
	 * @param Offset The starting index in the byte array from which to begin deserialization.
	 * @param Length The number of bytes to read for the string deserialization.
	 * @return The original form or object represented by the serialized string.
	 */
	static FString DeserializeString(TConstArrayView<uint8> Payload, int32 Offset, int32 Length);
	/**
	 * Deserializes a 32-bit integer from a binary data buffer.
	 *
	 * This method reads a 4-byte sequence from the provided binary data buffer
	 * starting at the given offset and converts it into a 32-bit integer. The
	 * buffer is expected to use a little-endian encoding for the integer value.
	 *
	 * @param buffer A constant reference to the vector of bytes representing the binary data.
	 * @param offset The zero-based position within the buffer where the 4-byte integer starts.
	 * @return The deserialized 32-bit integer.
	 * @throws std::out_of_range If the offset plus 4 exceeds the buffer size.
	 */
	static int32 DeserializeInt32(const TArray<uint8>& Payload, int32 Offset);
	/**
	 * Deserializes a 64-bit integer (int64) from the provided byte array starting at the specified offset.
	 *
	 * @param Payload The array of bytes containing the serialized data.
	 * @param Offset The starting index in the array from which to begin deserialization.
	 * @return The deserialized 64-bit integer value. Returns 0 if the Offset is invalid or insufficient data remains in the array.
	 */
	static int64 DeserializeInt64(const TArray<uint8>& Payload, int32 Offset);
	/**
	 * Deserializes a string representation of a floating-point number into its float equivalent.
	 *
	 * @param Payload The string containing the floating-point number to deserialize.
	 * @param Offset The starting index in the byte array from which to begin deserialization.
	 * @return The deserialized float value if the input is valid; otherwise, returns 0.0 or handles errors as appropriate.
	 */
	static float DeserializeFloat(const TArray<uint8>& Payload, int32 Offset);

	/**
	 * Calculates the HMAC (Hash-based Message Authentication Code) for the given data using the specified key and algorithm.
	 *
	 * @param Payload The input data for which the HMAC will be calculated. It must be a non-null byte array.
	 * @param GameToken The secret key used for the HMAC calculation. It must be a non-empty byte array.
	 *
	 * @return The computed HMAC as a byte array.
	 */
	static TArray<uint8> CalculateHMAC(const TArray<uint8>& Payload, const FString& GameToken);
	/**
	 * Performs HMAC (Hash-based Message Authentication Code) authentication
	 * using the specified key and message.
	 *
	 * @param GameToken The secret key used for generating the HMAC.
	 *            This should be a non-empty string.
	 * @param ReceivedMessage The input message that needs to be authenticated.
	 *                This should be a non-empty string.
	 * @return The computed HMAC as a string. The return value will be
	 *         in hexadecimal format representing the hash.
	 */
	static bool AuthenticateHMAC(const TArray<uint8>& ReceivedMessage, const FString& GameToken);


	/**
	 * Extracts chunk coordinates (X, Y, Z) from a JSON object.
	 * The JSON object is expected to have a "coordinates" field that contains the subfields "x", "y", and "z",
	 * which represent the chunk coordinates as strings. These string values are converted to int64 values.
	 *
	 * @param JsonObj The JSON object containing the "coordinates" field with "x", "y", and "z" subfields.
	 * @param X Reference to an int64 variable where the X coordinate will be stored.
	 * @param Y Reference to an int64 variable where the Y coordinate will be stored.
	 * @param Z Reference to an int64 variable where the Z coordinate will be stored.
	 * @return True if the coordinates were successfully extracted and converted; otherwise, false.
	 */
	static bool ExtractChunkCoordinates(const TSharedPtr<FJsonObject>& JsonObj, int64& X, int64& Y, int64& Z);

	/**
	 * The local comparison key for an actor id that is held as text rather than as octets.
	 *
	 * @param String The id as text. It addresses an actor only when it is exactly the 32 octets an id
	 *               occupies in UTF-8; anything else yields an invalid guid rather than a partial read.
	 * @return The key, or an invalid guid when the text is not an actor id.
	 */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Serialization Function Library")
	static FGuid ToGuid(const FString& String);

	/**
	 * The 128 bits the 32 octets encode when they are hexadecimal, which is the convention they follow.
	 *
	 * The result is a comparison key, not the identity: a component that is not hexadecimal reads as the
	 * largest value, so two ids that differ only outside the hexadecimal alphabet derive the same key.
	 * The 32 octets are what addresses an actor; this is only a cheap way to look one up locally.
	 *
	 * An id that is not set derives no key and returns an invalid guid, so it never collapses onto the
	 * same key a non-hexadecimal id derives.
	 */
	static FGuid ToGuid(const FCrowdyActorId& ActorId);

	/**
	 * Generates a unique voxel ID based on the provided coordinates and layer information.
	 *
	 * @param ChunkX The x-coordinate of the chunk.
	 * @param ChunkY The y-coordinate of the chunk.
	 * @param ChunkZ The z-coordinate of the chunk.
	 * @param VoxelX The x-coordinate of the voxel within the chunk.
	 * @param VoxelY The y-coordinate of the voxel within the chunk.
	 * @param VoxelZ The z-coordinate of the voxel within the chunk.
	 * @return A unique integer identifier representing the voxel.
	 */
	UFUNCTION(BlueprintCallable, Category = "Serialization Function Library")
	static FString GenerateVoxelID(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 VoxelX, int32 VoxelY, int32 VoxelZ);
	
	/**
	 * Frames a state blob as [sentinel][format version][type tag][class id][body].
	 *
	 * Shape and identity are stated separately and on every update, because the transport is lossy,
	 * relevance gated and continuously refreshed: a receiver can begin listening at any instant, so
	 * anything announced once is lost to whoever was not there for it.
	 *
	 * ClassID must name the entity's class. It is refused when invalid rather than sent, because an
	 * update carries no class path to fall back on and a receiver that cannot read the class would have
	 * to derive it from the struct, where two classes sharing one struct collapse into one.
	 */
	static bool SerializeActorState(const FInstancedStruct& Payload, FCrowdyClassID ClassID, TArray<uint8>& OutBytes);

	/**
	 * Decodes a state blob that arrived over the network, so every byte of it is treated as forged.
	 *
	 * Payload is read during this call only, so it may be a view straight into the receive buffer; the
	 * decoded struct that comes back owns everything it holds and outlives the bytes.
	 *
	 * A state struct only ever grows by appending fields, and the type tag hashes the struct's path
	 * name alone, so the tag does not change when it grows. Both directions of that version skew are
	 * therefore ordinary rather than hostile, and neither rejects the payload:
	 *
	 * A payload SHORTER than the struct this build knows leaves the fields past its end at the values
	 * the struct constructs them with. Those defaults are chosen so that a missing field reads as the
	 * safe answer, which is what lets a peer one version behind keep being understood instead of
	 * having every one of its updates dropped. A payload LONGER than the struct this build knows keeps
	 * its complete leading fields and drops the tail, which is a peer one version ahead. Both are
	 * reported, rate limited, because a skew nobody can see is the failure this framing cannot detect
	 * for itself.
	 *
	 * What IS rejected is a blob that names no readable type at all. On any rejection OutPayload is
	 * left empty, so a caller that does not read the return value finds nothing rather than a stale
	 * payload from a previous call.
	 *
	 * Contrast DeserializeEventState, which rejects a short payload: an event's fields are call
	 * arguments, where an argument nobody sent is not the same as one that equals its default.
	 */
	static bool DeserializeActorState(TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload);

	/**
	 * The same decode, additionally reporting the payload type tag that leads the state blob.
	 * OutTypeID is set whenever the tag could be read, including when no struct is registered for
	 * it, so a caller can still say what arrived after the decode fails.
	 */
	static bool DeserializeActorState(TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload, FCrowdyTypeID& OutTypeID);

	/**
	 * The same decode, additionally reporting the entity class the sender named.
	 *
	 * This is the overload the receive path wants: the class comes off the message rather than being
	 * derived from the wire struct, so two different classes may share one state struct and still
	 * resolve separately on an observer.
	 *
	 * Three framing faults are rejected here and nowhere else downstream, because everything past this
	 * point tolerates a peer a version apart and so cannot tell a reshaped frame from an appended one:
	 * a frame that leads with a type tag instead of the reserved sentinel (a sender predating this
	 * framing), a format version this build does not decode, and a frame naming no class at all.
	 */
	static bool DeserializeActorState(TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload,
		FCrowdyTypeID& OutTypeID, FCrowdyClassID& OutClassID);

	static bool SerializeEventState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes);

	/**
	 * The same encode for a caller that has already resolved the payload's type id, so it is not resolved
	 * twice per message. TypeID MUST be the id Payload's own struct is registered under: the tag written
	 * here comes from it while the body framing is still chosen from the struct, so an id naming a
	 * different type puts a frame on the wire that a receiver reads as that other type. Resolve it from
	 * Payload.GetScriptStruct() and pass it straight through, or call the overload above.
	 */
	static bool SerializeEventState(const FInstancedStruct& Payload, FCrowdyTypeID TypeID, TArray<uint8>& OutBytes);

	/**
	 * The same encode, APPENDED to whatever OutBytes already holds and taken from a borrowed view of the
	 * payload rather than an FInstancedStruct. This is what lets a message serialize its payload straight
	 * into the frame it is building, so the bytes are never written to a second buffer and copied in.
	 *
	 * The view is read during the call only. TypeID carries the same obligation as the overload above: it
	 * must be the id StructType is registered under, because it becomes the tag while the framing is still
	 * chosen from StructType.
	 */
	static bool AppendEventState(const UScriptStruct* StructType, const void* StructMemory,
		FCrowdyTypeID TypeID, TArray<uint8>& OutBytes);

	/**
	 * Decodes an event blob from the network, treating every byte as forged, and bounded the same way
	 * DeserializeActorState is. Payload carries the same read-during-this-call-only contract.
	 *
	 * It differs on one deliberate point: a payload that ends before the struct it names does is
	 * REJECTED here. An event's fields are the arguments of a call, and an argument the sender never
	 * wrote is not the same thing as an argument that happens to equal its default, so running a
	 * handler on invented arguments is worse than not running it at all.
	 */
	static bool DeserializeEventState(TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload);
	

	static void LogStructContent(const FInstancedStruct& Payload);

};

template <typename T>
/**
 * Serializes the given value into a string representation.
 *
 * @param value The value to serialize. This could be of any type that supports serialization.
 * @return A string representation of the serialized value.
 */
TArray<uint8> USerializationFunctionLibrary::SerializeValue(T Value)
{
	TArray<uint8> SerializedArray;
	SerializedArray.SetNumUninitialized(sizeof(T));

	// Direct memory copy for little-endian to little-endian
	FMemory::Memcpy(SerializedArray.GetData(), &Value, sizeof(T));

	return SerializedArray;
}

template <typename T>
/**
 * Reads one integral value out of a byte range, little-endian, at the given offset.
 *
 * @param Data The bytes to read from.
 * @param OutValue Receives the value, and is left untouched when the read is refused.
 * @param Offset The index of the value's first octet.
 * @return False when fewer octets remain at Offset than the value occupies.
 */
bool USerializationFunctionLibrary::DeserializeValue(const TConstArrayView<uint8> Data, T& OutValue, const int Offset)
{
	static_assert(std::is_integral_v<T>, "T must be an integral type.");

	// Warning rather than Error: this is the first bound a truncated or forged frame meets, and a short
	// frame is ordinary input on an open network rather than a fault in this build.
	if (Data.Num() < sizeof(T) + Offset)
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Not enough data to deserialize %s."), *FString(__FUNCTION__));
		return false;  // Indicate failure to deserialize
	}

	// Directly copy the bytes for little endian
	std::memcpy(&OutValue, Data.GetData() + Offset, sizeof(T));

	return true;  // Indicate success
}