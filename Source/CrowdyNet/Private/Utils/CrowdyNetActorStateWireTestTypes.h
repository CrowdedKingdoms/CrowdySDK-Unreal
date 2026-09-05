#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "CrowdyNetActorStateWireTestTypes.generated.h"

/**
 * Writes the actor-state framing prefix by hand: [uint16 sentinel][uint8 format version][uint16 type
 * tag][uint32 class id].
 *
 * Spelled out here rather than obtained by calling SerializeActorState, because these tests exist to
 * catch a bug shared between the writer and the reader, which a round trip through the production
 * writer is structurally unable to see. It lives in a header because two test files in this module
 * need it, and the same helper in two anonymous namespaces of one module redefines itself
 * nondeterministically under adaptive unity.
 */
inline void AppendActorStateFraming(TArray<uint8>& Out, const FCrowdyTypeID TypeID, const FCrowdyClassID ClassID)
{
	auto AppendRaw = [&Out](const void* Bytes, const int32 Size)
	{
		const int32 Base = Out.Num();
		Out.SetNumUninitialized(Base + Size);
		FMemory::Memcpy(Out.GetData() + Base, Bytes, Size);
	};

	const FCrowdyTypeID Sentinel = CROWDY_ACTOR_STATE_SENTINEL;
	const uint8 FormatVersion = CROWDY_ACTOR_STATE_FORMAT_VERSION;

	AppendRaw(&Sentinel, sizeof(Sentinel));
	AppendRaw(&FormatVersion, sizeof(FormatVersion));
	AppendRaw(&TypeID, sizeof(TypeID));
	AppendRaw(&ClassID, sizeof(ClassID));
}

// Any non-zero value serves: these tests pin framing and body decoding, not which class sent a frame.
// Zero is the one id the framing refuses outright.
inline constexpr FCrowdyClassID CrowdyActorStateWireTestClassID = 0xC1A551D0u;

/**
 * A minimal two-field actor-update payload, used only to exercise
 * USerializationFunctionLibrary::DeserializeActorState against hand-built bytes without depending on
 * any of the project's real payload structs (which may grow fields over time and would silently
 * change what byte offsets these tests are pinned to).
 */
USTRUCT()
struct FCrowdyNetActorStateWireTestPayload
{
	GENERATED_BODY()

	UPROPERTY()
	int32 A = 0;

	// Deliberately not zero. A short payload has to leave this field at the value the struct constructs
	// it with, and a default of zero would be indistinguishable from the memory simply being zeroed, so
	// the assertion that the default survived would prove nothing.
	UPROPERTY()
	int32 B = 9091;
};
