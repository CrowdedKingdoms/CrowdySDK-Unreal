#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"

struct FCrowdyRepLayout;
struct FCrowdyStateDelta;

// Format version prefixed to every FCrowdyStateDelta::Blob. Bump whenever the on-wire body encoding
// (selector modes, value framing, quantization framing) changes so a stale peer drops instead of
// misparsing. Separate from CrowdyRpcParamBlobVersion: the two blobs share the persistent-archive value
// encoding but not the selector/keyframe framing, so they version independently.
inline constexpr uint8 CrowdyStateBlobVersion = 1;

// Payload-kind tag prefixed to a CrowdyState channel payload (EncodeChannelStateDelta) so
// UCrowdyChannels::ForwardChannelRpc can discriminate it from an RPC channel payload by peeking the first
// byte. An RPC channel payload leads with CrowdyChannelRpcVersion (a small int; 1 today), never 0xC5, so
// the two channel wire formats can never collide. INVARIANT: no CrowdyChannelRpcVersion value may ever
// equal this tag.
inline constexpr uint8 CrowdyChannelStateDeltaTag = 0xC5;

// Format version for the CrowdyState channel-payload framing (tag + version + fields). Independent of
// CrowdyStateBlobVersion (the delta Blob's own body version) and CrowdyChannelRpcVersion; bump if the
// channel framing changes.
inline constexpr uint8 CrowdyChannelStateDeltaVersion = 1;

/**
 * Stateless codec for the CrowdyState delta body. Encodes the changed (or, for a keyframe, all) values
 * of a class's CrowdyState properties into a positional blob and decodes one back onto a live container,
 * guarding untrusted network bytes at every read.
 *
 * Blob layout: [uint8 Version][uint8 SelectorMode][selector...][positional value bytes...].
 *   - SelectorMode 0 (bitmask): ceil(N/8) bytes, LSB-first, bit i set == layout index i present.
 *   - SelectorMode 1 (index-list): a varint count, then that many STRICTLY-ASCENDING varint layout
 *     indices. The encoder picks whichever selector is smaller for the given dirty set.
 * Present values follow in ascending layout-index order, one per present index, byte-identical to the
 * RPC/event value encoding (FProperty::SerializeItem on a persistent archive)  except an
 * FStructProperty whose struct has a native net serializer, which is carried as a length-prefixed
 * NetSerializeItem sub-blob (quantization). Encode and decode branch on the same net-serialized test so
 * the framing always matches.
 *
 * This is the codec layer only: no networking, dispatch, dirty-tracking, or shadow diffing (Phases 3-5).
 */
class CROWDYREPLICATION_API FCrowdyStateCodec
{
public:
	// Reads the set-bit values out of Container (addressed by Layout's positional slots) into OutBlob.
	// Dirty is indexed by layout position; a keyframe treats every layout bit as set regardless of Dirty.
	// A hot delta with no bits set still produces a valid (empty) blob. Container is only read.
	static void Encode(const FCrowdyRepLayout& Layout, const void* Container, const TBitArray<>& Dirty,
		bool bKeyframe, TArray<uint8>& OutBlob);

	// Writes each present value from Blob into Container, but only where it differs from the value already
	// there, and appends the ACTUALLY-CHANGED layout indices (ascending) to OutChangedIndices  so a caller
	// firing OnRep off this set gets RepNotify-on-change semantics, and a keyframe re-sending unchanged values
	// is an idempotent no-op (no OnRep re-fires). An unchanged present slot is decoded and compared but leaves
	// its live value untouched and is omitted from OutChangedIndices. Returns false and leaves OutChangedIndices
	// in whatever partial state it reached on any guard failure a layout-hash mismatch, a bad
	// version/selector, a forged or non-ascending index, a truncated value, or trailing bytes. A hash mismatch
	// or a pre-read failure leaves Container fully untouched; a mid-value truncation may have written earlier
	// changed values, and the caller re-pulls.
	static bool Decode(const FCrowdyRepLayout& Layout, int64 IncomingLayoutHash, const TArray<uint8>& Blob,
		void* Container, TArray<int32>& OutChangedIndices);

	// Frames one FCrowdyStateDelta as a reliable-channel payload, a byte-for-byte mirror of
	// FCrowdyRPC::EncodeChannelRpc: [u8 tag=CrowdyChannelStateDeltaTag][u8 version][ClassID][EntityID]
	// [SenderID][LayoutHash][u8 Flags][Blob]. The leading tag is the discriminator ForwardChannelRpc peeks
	// (see CrowdyChannelStateDeltaTag). Used for non-spatial (subsystem) participants, which ride the
	// reliable channel rather than the spatial transport.
	static void EncodeChannelStateDelta(const FCrowdyStateDelta& Delta, TArray<uint8>& OutPayload);

	// Reverses EncodeChannelStateDelta. Returns false (Warning-logged, never Error) and leaves Out partial
	// on: too-short, a wrong kind tag, a wrong version, a forged/oversized Blob length, or trailing bytes.
	// The Blob length prefix is untrusted, so it is bounds-checked against the payload before the byte read
	// (the bulk TArray<uint8> load is NOT gated by ArMaxSerializeSize for a non-net archive), so a forged
	// length can never drive a giant allocation.
	static bool DecodeChannelStateDelta(const TArray<uint8>& Payload, FCrowdyStateDelta& Out);
};
