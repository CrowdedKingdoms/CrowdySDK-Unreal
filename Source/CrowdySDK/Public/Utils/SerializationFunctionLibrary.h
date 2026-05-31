#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include <type_traits>
#include <cstring>
#include "Dom/JsonObject.h"
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
class CROWDYSDK_API USerializationFunctionLibrary : public UBlueprintFunctionLibrary
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

	template<typename T>
	/**
	 * Deserializes a given string into its corresponding object representation.
	 *
	 * @param Data The serialized string representation of the object.
	 * @param OutValue A reference to the object that will hold the deserialized value.
	 * @param Offset The starting index in the byte array from which to begin deserialization.
	 * @return True if deserialization is successful, false otherwise.
	 */
	static bool DeserializeValue(const TArray<uint8>& Data, T& OutValue, int Offset = 0);


	/**
	 * Deserializes a serialized string representation into its original form or object.
	 *
	 * @param Payload The serialized string that needs to be deserialized.
	 * @param Offset The starting index in the byte array from which to begin deserialization.
	 * @param Length The number of bytes to read for the string deserialization.
	 * @return The original form or object represented by the serialized string.
	 */
	static FString DeserializeString(const TArray<uint8>& Payload, int32 Offset, int32 Length);
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
	 * Converts the given input into a GUID (Globally Unique Identifier) string.
	 *
	 * @param String The value to be converted into a GUID. This can be a string,
	 *              byte array, or other compatible type that supports GUID conversion.
	 * @return A FGuid representation of the String converted from the input.
	 *         If the conversion fails, an appropriate exception or error may be thrown
	 *         depending on the implementation.
	 */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Serialization Function Library")
	static FGuid ToGuid(const FString& String);

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
	
	static bool SerializeActorState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes);
	static bool DeserializeActorState(const TArray<uint8>& Payload, FInstancedStruct& OutPayload);
	
	static bool SerializeEventState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes);
	static bool DeserializeEventState(const TArray<uint8>& Payload, FInstancedStruct& OutPayload);
	

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
 * Deserializes a string representation of a value into its corresponding object type.
 *
 * @param serializedValue The string representation of the serialized value.
 * @param targetType The class type to which the value should be deserialized.
 * @param <T> The type of the resulting object after deserialization.
 * @return The deserialized object of the specified type.
 * @throws IllegalArgumentException If the serialized value cannot be converted to the target type.
 * @throws NullPointerException If the serialized value or target type is null.
 */
bool USerializationFunctionLibrary::DeserializeValue(const TArray<uint8>& Data, T& OutValue, const int Offset)
{
	static_assert(std::is_integral_v<T>, "T must be an integral type.");

	// Ensure there are enough bytes in the array
	if (Data.Num() < sizeof(T) + Offset)
	{
		UE_LOG(LogTemp, Error, TEXT("Not enough data to deserialize %s."), *FString(__FUNCTION__));
		return false;  // Indicate failure to deserialize
	}

	// Directly copy the bytes for little endian
	std::memcpy(&OutValue, Data.GetData() + Offset, sizeof(T));

	return true;  // Indicate success
}