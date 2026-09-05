#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"

/**
 * Which id space a payload type number belongs to. A game event and an actor update
 * may carry the same number and mean different things.
 */
enum class ECrowdyPayloadCategory : uint8
{
	None = 0,
	/** The uint16 event type carried in the frame by opcodes 138, 139 and 142. */
	Event,
	/** The uint16 tag at the head of the state blob carried by opcode 130. */
	ActorUpdate,
};

/**
 * What a message is, independent of the opcode that carried it. Zero is a legal type
 * number (a legacy event enum uses it), so a key is set or unset by its category alone,
 * never by its number being zero.
 */
struct CROWDYNET_API FCrowdyPayloadKey
{
	ECrowdyPayloadCategory Category = ECrowdyPayloadCategory::None;
	FCrowdyTypeID TypeID = 0;

	bool IsSet() const { return Category != ECrowdyPayloadCategory::None; }

	bool operator==(const FCrowdyPayloadKey& Other) const;
	bool operator!=(const FCrowdyPayloadKey& Other) const { return !(*this == Other); }

	static FCrowdyPayloadKey Event(FCrowdyTypeID InTypeID);
	static FCrowdyPayloadKey ActorUpdate(FCrowdyTypeID InTypeID);

	/** "Event/61234 (FCrowdyRpcCall)" or "Event/0 (unregistered)", for diagnostics only. */
	FString Describe() const;

	/** The category on its own, for reporting a wildcard subscription. */
	static const TCHAR* CategoryName(ECrowdyPayloadCategory InCategory);
};

CROWDYNET_API uint32 GetTypeHash(const FCrowdyPayloadKey& Key);
