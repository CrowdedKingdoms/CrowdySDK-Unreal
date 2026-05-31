#include "Utils/SerializationFunctionLibrary.h"
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/Archive.h"
#include "Utils/UEventPayloadRegistry.h"

FString USerializationFunctionLibrary::DeserializeString(const TArray<uint8>& Payload, int32 Offset, int32 Length)
{
	if (Offset + Length > Payload.Num())
	{
		UE_LOG(LogTemp, Error, TEXT("Offset + Length exceeds Payload size"));
		return FString();
	}

	// Convert directly from the UTF-8 byte range with explicit length
	const char* Utf8Ptr = reinterpret_cast<const char*>(Payload.GetData() + Offset);
	const FUTF8ToTCHAR Converter(Utf8Ptr, Length);

	// Build an FString from the converted buffer
	return FString(Converter.Length(), Converter.Get());
}

int32 USerializationFunctionLibrary::DeserializeInt32(const TArray<uint8>& Payload, int32 Offset)
{
	int32 Value = 0;
	if (Offset + 4 <= Payload.Num())
	{
		FMemory::Memcpy(&Value, Payload.GetData() + Offset, 4);
	}
	return Value;
}

int64 USerializationFunctionLibrary::DeserializeInt64(const TArray<uint8>& Payload, int32 Offset)
{
	int64 Value = 0;
	if (Offset + sizeof(int64) <= Payload.Num())
	{
		FMemory::Memcpy(&Value, Payload.GetData() + Offset, sizeof(int64));
	}
	return Value;
}

float USerializationFunctionLibrary::DeserializeFloat(const TArray<uint8>& Payload, int32 Offset)
{
	if (Payload.Num() < Offset + sizeof(float))
	{
		UE_LOG(LogTemp, Error, TEXT("Buffer too small to deserialize float."));
		return 0.0f;
	}

	// Directly copy the bytes from the payload to the float
	float Value;
	FMemory::Memcpy(&Value, &Payload[Offset], sizeof(float));
	return Value;
}

TArray<uint8> USerializationFunctionLibrary::CalculateHMAC(const TArray<uint8>& Payload, const FString& GameToken)
{
	TArray<uint8> HMACResult;

	const FTCHARToUTF8 Converter(*GameToken);
	const uint8* TokenBytes = reinterpret_cast<const uint8*>(Converter.Get());
	const int32 TokenLen = Converter.Length();

	// Ensure key is exactly 64 bytes
	check(TokenLen == 64);

	TArray<uint8> Key;
	Key.Append(TokenBytes, TokenLen);

	TArray<uint8> Message;
	Message.Reserve(Payload.Num() + Key.Num());
	Message.Append(Payload);
	Message.Append(Key);

	uint8 HMACBuffer[32] = {0};
	unsigned int OutLen = 0;

	HMAC(
		EVP_sha256(),
		Key.GetData(),
		Key.Num(),
		Message.GetData(),
		Message.Num(),
		HMACBuffer,
		&OutLen
	);

	HMACResult.Append(HMACBuffer, OutLen);

	return HMACResult;
}

bool USerializationFunctionLibrary::AuthenticateHMAC(const TArray<uint8>& ReceivedMessage, const FString& GameToken)
{
	constexpr int32 HmacSize = 32;
	constexpr int32 TailSizeWithAuth = 41;

	if (ReceivedMessage.Num() < TailSizeWithAuth)
	{
		return false;
	}

	const bool bContainsAuth = ReceivedMessage[35] == 1;

	if (!bContainsAuth)
	{
		return true;
	}

	const int32 PrefixLen = ReceivedMessage.Num() - TailSizeWithAuth;

	if (PrefixLen <= 0)
	{
		return false;
	}

	const uint8* MessageData = ReceivedMessage.GetData();
	const uint8* ReceivedHmac = MessageData + PrefixLen;

	const TArray<uint8> Prefix(MessageData, PrefixLen);
	TArray<uint8> ComputedHmac = CalculateHMAC(Prefix, GameToken);

	if (ComputedHmac.Num() != HmacSize)
	{
		return false;
	}

	return CRYPTO_memcmp(ComputedHmac.GetData(), ReceivedHmac, HmacSize) == 0;
}

bool USerializationFunctionLibrary::ExtractChunkCoordinates(const TSharedPtr<FJsonObject>& JsonObj, int64& X, int64& Y,
                                                            int64& Z)
{
	const TSharedPtr<FJsonObject>* Coordinates;

	if (!JsonObj->TryGetObjectField(TEXT("coordinates"), Coordinates))
	{
		UE_LOG(LogTemp, Warning, TEXT("Coordinates not found in updateChunk object"));
		return false;
	}

	FString sX, sY, sZ;
	if (!(*Coordinates)->TryGetStringField(TEXT("x"), sX))
	{
		UE_LOG(LogTemp, Warning, TEXT("X coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	if (!(*Coordinates)->TryGetStringField(TEXT("y"), sY))
	{
		UE_LOG(LogTemp, Warning, TEXT("Y coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	if (!(*Coordinates)->TryGetStringField(TEXT("z"), sZ))
	{
		UE_LOG(LogTemp, Warning, TEXT("Z coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	X = FCString::Atoi64(*sX);
	Y = FCString::Atoi64(*sY);
	Z = FCString::Atoi64(*sZ);

	return true;
}

FGuid USerializationFunctionLibrary::ToGuid(const FString& String)
{
	// Direct character pointer access for speed
	const TCHAR* Data = *String;

	// Fast hex character to value conversion
	auto GetHexValue = [](const TCHAR c) -> uint8
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		return 255; // Invalid
	};

	// Inline hex parsing function
	auto ParseHex = [&](const TCHAR* Ptr, const int32 Len) -> uint32
	{
		uint32 Result = 0;
		for (int32 i = 0; i < Len; ++i)
		{
			const uint8 Value = GetHexValue(Ptr[i]);
			if (Value == 255) return UINT32_MAX; // Invalid character
			Result = (Result << 4) | Value;
		}
		return Result;
	};

	// Parse components directly without string operations
	const uint32 A = ParseHex(Data, 8);
	const uint32 B = ParseHex(Data + 8, 8);
	const uint32 C = ParseHex(Data + 16, 8);
	const uint32 D = ParseHex(Data + 24, 8);

	return FGuid(A, B, C, D);
}

FString USerializationFunctionLibrary::GenerateVoxelID(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 VoxelX,
                                                       int32 VoxelY, int32 VoxelZ)
{
	const FString Input = FString::Printf(
		TEXT("%lld,%lld,%lld,%d,%d,%d"),
		ChunkX, ChunkY, ChunkZ,
		VoxelX, VoxelY, VoxelZ);

	// Convert to UTF-8 for hashing
	const FTCHARToUTF8 UTF8String(*Input);

	// Calculate SHA256 hash using OpenSSL
	unsigned char HashBytes[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(UTF8String.Get()), UTF8String.Length(), HashBytes);

	// Convert hash to hex string
	FString Result;
	for (int32 i = 0; i < SHA256_DIGEST_LENGTH; ++i)
	{
		Result += FString::Printf(TEXT("%02x"), HashBytes[i]);
	}

	return Result;
}

bool USerializationFunctionLibrary::SerializeActorState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();
	const void* StructMemory = Payload.GetMemory();

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogTemp, Error, TEXT("[SerializeActorState]: Invalid script struct or memory pointer"));
		return false;
	}

	FCrowdyTypeID TypeID;

	if (!UActorUpdatePayloadRegistry::Get()->GetID(StructType, TypeID))
	{
		UE_LOG(LogTemp, Error, TEXT("[SerializeActorState]: Failed to get ID for script struct"));
		return false;
	}

	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, true);

	Writer << TypeID;

	StructType->SerializeBin(Writer, const_cast<void*>(StructMemory));

	//UE_LOG(LogTemp, Log, TEXT("[SerializeActorState] '%s' -> TypeID=%d, Size=%d bytes"),
	//	*StructType->GetName(), TypeID, OutBytes.Num());

	return true;
}

bool USerializationFunctionLibrary::DeserializeActorState(const TArray<uint8>& Payload, FInstancedStruct& OutPayload)
{
	if (Payload.Num() < sizeof(uint8))
	{
		UE_LOG(LogTemp, Error, TEXT("[DeserializeActorState]: Payload too small"));
		return false;
	}

	FMemoryReader Reader(Payload, true);

	FCrowdyTypeID TypeID;
	Reader << TypeID;

	const UScriptStruct* StructType = UActorUpdatePayloadRegistry::Get()->Resolve(TypeID);

	if (!StructType)
	{
		//UE_LOG(LogTemp, Error, TEXT("[DeserializeActorState]: Failed to resolve script struct for TypeID=%d"), TypeID);
		return false;
	}

	OutPayload.InitializeAs(StructType);
	StructType->SerializeBin(Reader, OutPayload.GetMutableMemory());

	//UE_LOG(LogTemp, Log, TEXT("[DeserializeActorState] TypeID=%d -> '%s'"), TypeID, *StructType->GetName());

	return true;
}

bool USerializationFunctionLibrary::SerializeEventState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();
	const void* StructMemory = Payload.GetMemory();

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogTemp, Error, TEXT("[SerializeEventState]: Invalid script struct or memory pointer"));
		return false;
	}

	FCrowdyTypeID TypeID;
	if (!UEventPayloadRegistry::Get()->GetID(StructType, TypeID))
	{
		UE_LOG(LogTemp, Error, TEXT("[SerializeEventState]: Failed to get ID for script struct"));
		return false;
	}

	static const FCrowdyTypeID SpawnEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntitySpawnEvent::StaticStruct());
	static const FCrowdyTypeID StateEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntityStateEvent::StaticStruct());
	static const FCrowdyTypeID DestroyEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntityDestroyEvent::StaticStruct());

	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, true);
	Writer << TypeID;

	if (TypeID == SpawnEventID)
	{
		const FCrowdyEntitySpawnEvent* Event =
			static_cast<const FCrowdyEntitySpawnEvent*>(StructMemory);

		FGuid ObjectID = Event->EntityID;
		FGuid OwnerID = Event->OwnerID;
		int32 ObjectTypeID = Event->TypeID;
		FTransform SpawnTransform = Event->SpawnTransform;

		Writer << ObjectID;
		Writer << OwnerID;
		Writer << ObjectTypeID;
		Writer << SpawnTransform;

		const bool bHasInitialState = Event->InitialState.IsValid();
		Writer << const_cast<bool&>(bHasInitialState);

		if (!bHasInitialState)
		{
			return true;
		}

		const UScriptStruct* InnerStructType =
			Event->InitialState.GetScriptStruct();

		const void* InnerStructMemory =
			Event->InitialState.GetMemory();

		FCrowdyTypeID InnerTypeID;
		if (!UEventPayloadRegistry::Get()->GetID(
			InnerStructType,
			InnerTypeID))
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("[SerializeEventState]: Failed to get ID for InitialState struct"));
			return false;
		}

		Writer << InnerTypeID;
		InnerStructType->SerializeBin(
			Writer,
			const_cast<void*>(InnerStructMemory));

		return true;
	}

	if (TypeID == StateEventID)
	{
		const FCrowdyEntityStateEvent* Event = static_cast<const FCrowdyEntityStateEvent*>(StructMemory);
		FGuid ObjectID = Event->EntityID;

		const UScriptStruct* InnerStructType = Event->NewState.GetScriptStruct();
		const void* InnerStructMemory = Event->NewState.GetMemory();

		FCrowdyTypeID InnerTypeID;
		if (!UEventPayloadRegistry::Get()->GetID(InnerStructType, InnerTypeID))
		{
			UE_LOG(LogTemp, Error, TEXT("[SerializeEventState]: Failed to get ID for NewState struct"));
			return false;
		}

		Writer << ObjectID;
		Writer << InnerTypeID;
		InnerStructType->SerializeBin(Writer, const_cast<void*>(InnerStructMemory));
		return true;
	}

	if (TypeID == DestroyEventID)
	{
		const FCrowdyEntityDestroyEvent* Event = static_cast<const FCrowdyEntityDestroyEvent*>(StructMemory);
		FGuid ObjectID = Event->EntityID;
		Writer << ObjectID;
		return true;
	}

	// Default — flat struct, straight SerializeBin
	StructType->SerializeBin(Writer, const_cast<void*>(StructMemory));
	return true;
}

bool USerializationFunctionLibrary::DeserializeEventState(const TArray<uint8>& Payload, FInstancedStruct& OutPayload)
{
	if (Payload.Num() < sizeof(FCrowdyTypeID))
	{
		UE_LOG(LogTemp, Error, TEXT("[DeserializeEventState]: Payload too small"));
		return false;
	}

	FMemoryReader Reader(Payload, true);

	FCrowdyTypeID TypeID;
	Reader << TypeID;

	const UScriptStruct* StructType = UEventPayloadRegistry::Get()->Resolve(TypeID);
	if (!StructType)
	{
		UE_LOG(LogTemp, Error, TEXT("[DeserializeEventState]: Failed to resolve TypeID=%d"), TypeID);
		return false;
	}

	// Resolve internal event IDs once from the registry.
	// These replace the hardcoded 47/48/49 — the values are now
	// stable hash-derived IDs but we never need to know what they are.
	static const FCrowdyTypeID SpawnEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntitySpawnEvent::StaticStruct());
	static const FCrowdyTypeID StateEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntityStateEvent::StaticStruct());
	static const FCrowdyTypeID DestroyEventID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntityDestroyEvent::StaticStruct());

	OutPayload.InitializeAs(StructType);
	void* StructMemory = OutPayload.GetMutableMemory();

	if (TypeID == SpawnEventID)
	{
		auto* Event =
			static_cast<FCrowdyEntitySpawnEvent*>(StructMemory);

		Reader << Event->EntityID;
		Reader << Event->OwnerID;
		Reader << Event->TypeID;
		Reader << Event->SpawnTransform;

		bool bHasInitialState = false;
		Reader << bHasInitialState;

		if (!bHasInitialState)
		{
			Event->InitialState.Reset();
			return true;
		}

		FCrowdyTypeID InnerTypeID;
		Reader << InnerTypeID;

		const UScriptStruct* InnerType =
			UEventPayloadRegistry::Get()->Resolve(InnerTypeID);

		if (!InnerType)
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("[DeserializeEventState]: Failed to resolve inner TypeID=%d"),
				InnerTypeID);
			return false;
		}

		Event->InitialState.InitializeAs(InnerType);

		InnerType->SerializeBin(
			Reader,
			Event->InitialState.GetMutableMemory());

		return true;
	}

	if (TypeID == StateEventID)
	{
		auto* Event = static_cast<FCrowdyEntityStateEvent*>(StructMemory);
		Reader << Event->EntityID;

		FCrowdyTypeID InnerTypeID;
		Reader << InnerTypeID;

		const UScriptStruct* InnerType = UEventPayloadRegistry::Get()->Resolve(InnerTypeID);
		if (!InnerType)
		{
			UE_LOG(LogTemp, Error, TEXT("[DeserializeEventState]: Failed to resolve inner TypeID=%d"), InnerTypeID);
			return false;
		}

		Event->NewState.InitializeAs(InnerType);
		InnerType->SerializeBin(Reader, Event->NewState.GetMutableMemory());
		return true;
	}

	if (TypeID == DestroyEventID)
	{
		auto* Event = static_cast<FCrowdyEntityDestroyEvent*>(StructMemory);
		Reader << Event->EntityID;
		return true;
	}

	// Default — flat struct, straight SerializeBin
	StructType->SerializeBin(Reader, StructMemory);
	return true;
}


void USerializationFunctionLibrary::LogStructContent(const FInstancedStruct& Payload)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();
	const void* StructMemory = Payload.GetMemory();

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LogStructContents] Empty or invalid FInstancedStruct."));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[LogStructContents] Struct: %s"), *StructType->GetName());

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		FProperty* Prop = *It;
		const void* PropMemory = Prop->ContainerPtrToValuePtr<void>(StructMemory);

		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, PropMemory, nullptr, nullptr, PPF_None);

		UE_LOG(LogTemp, Log, TEXT("  %s = %s"), *Prop->GetName(), *ValueStr);
	}
}
