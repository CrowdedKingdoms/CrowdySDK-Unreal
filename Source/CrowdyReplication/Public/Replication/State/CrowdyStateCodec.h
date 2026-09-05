#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "UObject/WeakObjectPtr.h"

class FProperty;
class UClass;
struct FCrowdyRepLayout;
struct FCrowdyStateDelta;

// Format version prefixed to every FCrowdyStateDelta::Blob. Bump whenever the on-wire body encoding
// (selector modes, value framing, quantization framing) changes so a stale peer drops instead of
// misparsing. Separate from CrowdyRpcParamBlobVersion: the two blobs share the persistent-archive value
// encoding but not the selector/keyframe framing, so they version independently.
//
// Version 2 carries an enum leaf as its underlying integer instead of its entry name (see
// EnumValueProperty in the .cpp). That is a value-framing change and nothing else, so it moves this
// version and NOT the layout hash: the hash is computed from a type's canonical name, which is unchanged,
// and moving it would drag every RPC function id with it for a difference the RPC plane does not have.
// A version-1 peer and a version-2 peer therefore drop each other's deltas here rather than reading a
// length-prefixed string as an integer.
inline constexpr uint8 CrowdyStateBlobVersion = 2;

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
 * Reusable decode scratch for one rep layout, so decoding a delta costs no allocation per present field.
 *
 * It holds one value slot for every property the layout names, packed in layout order at each value's own
 * alignment and constructed once when the block is built. A decode reads a value into its slot, compares it
 * against the live container and copies only on a change, exactly as it does without one; what disappears is
 * the allocation and the free a value used to cost. Every slot is destroyed once, when the block is.
 *
 * The block is sized by the LAYOUT and never by a class, so it is not shaped like any container a decode can
 * be handed and is always a distinct allocation from one. Supplying it is optional: FCrowdyStateCodec::Decode
 * allocates and destroys a value per field when it is given none, which is what every caller without a hot
 * path does.
 *
 * One block must not be decoded into by two decodes at once, and it holds no lock: the CrowdyState receive
 * path is game thread only.
 */
struct CROWDYREPLICATION_API FCrowdyStateDecodeScratch
{
	FCrowdyStateDecodeScratch() = default;

	// Out of line: destroying the slots needs the full FProperty definition.
	~FCrowdyStateDecodeScratch();

	// Non-copyable and non-movable: it owns one allocation plus constructed values (a string's heap, for
	// example) that must be destroyed exactly once.
	FCrowdyStateDecodeScratch(const FCrowdyStateDecodeScratch&) = delete;
	FCrowdyStateDecodeScratch& operator=(const FCrowdyStateDecodeScratch&) = delete;
	FCrowdyStateDecodeScratch(FCrowdyStateDecodeScratch&&) = delete;
	FCrowdyStateDecodeScratch& operator=(FCrowdyStateDecodeScratch&&) = delete;

	// Builds the block for Layout, or rebuilds it when the layout it was built against has moved under it.
	// Cheap to call per delta: a block that already describes Layout is kept as it is. Owner is the class the
	// layout's properties belong to, passed in rather than re-derived so a caller that has already resolved it
	// does not resolve the same handle twice; a null Owner leaves the block unbuilt.
	void EnsureForLayout(const FCrowdyRepLayout& Layout, const UClass* Owner);

	// True when the block describes a layout and can be decoded into.
	bool IsReady() const { return Block != nullptr; }

	// True when the block was already built for this layout's slots. Answered from the block's own record, so
	// it costs no handle resolve; it says the block fits Layout, not that its class is still alive, which is
	// what EnsureForLayout establishes.
	bool DescribesLayout(const FCrowdyRepLayout& Layout) const;

	// The slot for one layout index, or null when that index has none (an unresolved property).
	void* SlotFor(int32 LayoutIndex) const;

	// True when Address lies inside the block. Decode refuses a scratch that is the very container it would
	// compare against, because every value would then be compared with itself and nothing would ever be
	// reported as changed.
	bool Contains(const void* Address) const;

private:

	void Release();

	// The class the layout's properties belong to; weak so a block never keeps a class alive, and so a class
	// that went away is detectable before its properties are dereferenced.
	TWeakObjectPtr<const UClass> OwnerClass;

	// Hash of the layout the block was built against. A layout whose hash has moved describes a different set
	// of slots, so the block is rebuilt rather than reused under it.
	int64 LayoutHash = 0;

	uint8* Block = nullptr;
	int32 BlockSize = 0;

	// One entry per layout index. The properties are snapshotted at build time so teardown never has to
	// consult a layout that may since have been freed and rebuilt.
	TArray<const FProperty*> SlotProperties;
	TArray<int32> SlotOffsets;
};

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
 * This is the codec layer only: no networking, dispatch, dirty-tracking, or shadow diffing.
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
	// OutPresentIndices, when supplied, receives every slot the delta CARRIED, whether or not the value moved.
	// The two sets differ, and which one a caller wants depends on what its container is. A caller decoding
	// into the target's own storage wants OutChangedIndices, so a keyframe re-sending unchanged values stays
	// idempotent and refires no notify. A caller decoding into a buffer SHARED between targets cannot use it:
	// "differs from what is already there" then means "differs from the previous target's value", so a target
	// legitimately sent the same value its predecessor had would be reported as having been sent nothing at
	// all. Such a caller wants the present set, which is a fact about the delta alone.
	// Scratch, when supplied, is the caller's reusable per-layout value block (see FCrowdyStateDecodeScratch):
	// it is built for Layout here unless it already describes it, and it removes the allocation each present
	// field would otherwise cost. It changes no outcome, so a caller off the hot path passes nothing.
	static bool Decode(const FCrowdyRepLayout& Layout, int64 IncomingLayoutHash, const TArray<uint8>& Blob,
		void* Container, TArray<int32>& OutChangedIndices, TArray<int32>* OutPresentIndices = nullptr,
		FCrowdyStateDecodeScratch* Scratch = nullptr);

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
