#pragma once

#include "CoreMinimal.h"
#include "FCrowdyStateDelta.generated.h"

// Flag bits carried in FCrowdyStateDelta::Flags. Defined here (the payload's home) so the encode/send
// path (Phase 3) and the receive/apply path (Phase 5) agree on the bit layout; only bit0/bit1 are used
// today, the rest are reserved and must stay zero on the wire.
namespace CrowdyStateDeltaFlags
{
	// The blob is a full keyframe (every layout property present), not a hot delta. Set by the encoder
	// when bKeyframe is true; a receiver that has no prior state for the entity requires one of these.
	inline constexpr uint8 Keyframe = 1u << 0;

	// The originator was acting as the elected host, so a host-precedence receiver may prefer this over
	// a same-tick non-host delta. Advisory only at the codec layer; the codec never sets or reads it.
	inline constexpr uint8 HostSourced = 1u << 1;
}

/**
 * Wire payload for one CrowdyState delta: a positional snapshot of the changed (or, for a keyframe, all)
 * CrowdyState properties of one entity. Like FCrowdyRpcCall it rides the existing FInstancedStruct
 * transport unchanged, so the ClassID / EntityID / SenderID triple is laid out identically for uniform
 * receive-side resolution; the codec (FCrowdyStateCodec) owns the Blob's internal format.
 *
 * ClassID and LayoutHash occupy DIFFERENT id spaces and must not be conflated: ClassID is a 32-bit
 * FCrowdyClassID (UCrowdyClassRegistry id of the owning class) widened to int64 for Blueprint/reflection
 * friendliness; LayoutHash is a 64-bit FCrowdyTypeIDGenerator hash of the class's ordered rep layout,
 * used purely as the positional guard. A matching ClassID does not imply a matching LayoutHash (two
 * builds of the same class can drift), which is exactly why both travel.
 */
USTRUCT()
struct CROWDYREPLICATION_API FCrowdyStateDelta
{
	GENERATED_BODY()

	// Owning class (UCrowdyClassRegistry id widened). Phase 3 fills it from the sending entity's class;
	// the codec itself does not read it.
	UPROPERTY()
	int64 ClassID = 0;

	// Target entity's network NetID. Phase 4 resolves the local actor from this; invalid until then.
	UPROPERTY()
	FGuid EntityID;

	// Originating player's GUID, for echo-drop and host precedence on receipt. Carried in the payload
	// because the single-actor transport has no wire sender.
	UPROPERTY()
	FGuid SenderID;

	// Positional guard: the sender's FCrowdyRepLayout::LayoutHash. A receiver whose layout hashes
	// differently drops the delta cleanly rather than misparsing the positional body.
	UPROPERTY()
	int64 LayoutHash = 0;

	// CrowdyStateDeltaFlags bits: bit0 Keyframe, bit1 HostSourced; the rest are reserved and zero.
	UPROPERTY()
	uint8 Flags = 0;

	// Encoded body: [version][selector-mode][selector][positional values]. Opaque to everything but
	// FCrowdyStateCodec, which reads it against the receiver's own layout.
	UPROPERTY()
	TArray<uint8> Blob;
};
