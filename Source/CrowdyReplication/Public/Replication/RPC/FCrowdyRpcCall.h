#pragma once

#include "CoreMinimal.h"
#include "FCrowdyRpcCall.generated.h"

/**
 * Wire payload for an RPC-style CrowdyEvent. An RPC is just a normal Crowdy event
 * whose payload is this struct, so it rides the existing FInstancedStruct transport
 * unchanged; the event router recognizes it and dispatches the call to the target's
 * UFunction instead of to a struct handler.
 */
USTRUCT()
struct CROWDYREPLICATION_API FCrowdyRpcCall
{
	GENERATED_BODY()

	// Identifies the target's class (UCrowdyClassRegistry id of the calling object's
	// class). Stored as int64 so the struct stays Blueprint/reflection friendly.
	UPROPERTY()
	int64 ClassID = 0;

	// Which entity instance the call targets. The receiver resolves the actor from this against
	// its own local registry, independent of how the message was routed. Invalid for a call from
	// an untracked object.
	UPROPERTY()
	FGuid EntityID;

	// The client that originated the call (its player GUID). Carried in the payload because the
	// single-actor transport has no wire sender; the broadcast receive path drops the sender's
	// own echo by comparing this, and an owner can see who requested a change.
	UPROPERTY()
	FGuid SenderID;

	// Stable hash of the receiver's full signature. Resolves the UFunction and lets a
	// drifted build reject the call cleanly instead of misparsing the parameter bytes.
	UPROPERTY()
	int64 FunctionID = 0;

	// Reflected parameter bytes: a one-byte format version followed by each input
	// parameter serialized in declaration order.
	UPROPERTY()
	TArray<uint8> ParamBlob;
};
