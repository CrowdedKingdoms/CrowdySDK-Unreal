#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/keys.hpp"

namespace crowdy::domains {
class ChunksAPI;
}

/// ChunkStore — a chunk/voxel cache: bulk hydrate over GraphQL, realtime
/// merge from voxel notifications, optimistic local edits with UDP sends, and
/// optional durable write-back for locally generated chunks (the shared-
/// worldgen pattern).
///
/// It is a helper for 16x16x16 chunks with one byte per voxel. Voxel positions
/// and types are the app's signed 16-bit values, which the platform does not
/// check: an edit the dense grid cannot hold (a type outside 0-255, a position
/// outside 0-15) goes to the chunk's `overlay` instead, and reads of that voxel
/// return it. An app with other addressing reads the raw voxel events
/// (Handlers::voxelUpdate, WorldSessionConfig::onVoxel, StoredChunk::voxelStates).
namespace crowdy::session {

struct VoxelState {
  std::int16_t voxelType = 0;
  std::vector<std::uint8_t> state;
};

/// A voxel the dense grid cannot hold, kept in ChunkData::overlay: a type outside
/// 0-255, or a position outside 0-15 (the app's signed 16-bit values, as the edit
/// carried them).
struct OverlayVoxel {
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::int16_t z = 0;
  /// Its type and state (`state` empty when it has none).
  VoxelState voxel;
};

/// The key ChunkData::overlay holds the voxel at (x, y, z) under: the three
/// signed 16-bit coordinates packed into 48 bits.
inline constexpr std::uint64_t voxelKey(std::int16_t x, std::int16_t y, std::int16_t z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 32) |
         (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16) |
         static_cast<std::uint64_t>(static_cast<std::uint16_t>(z));
}

struct ChunkData {
  ChunkCoord coord{};
  /// Dense 16^3 voxel-type grid (index x + y*16 + z*256). Holds 0 where an
  /// in-grid voxel's type is in `overlay`.
  std::array<std::uint8_t, kChunkVolume> voxels{};
  /// Sparse per-voxel metadata blobs, keyed by voxel index (voxels in the grid).
  std::unordered_map<int, VoxelState> voxelStates;
  /// The voxels the dense grid cannot hold (a type outside 0-255, a position
  /// outside 0-15), each with its type and state, keyed by voxelKey(x, y, z).
  /// voxelTypeAt() and voxelStateAt() read them first.
  std::unordered_map<std::uint64_t, OverlayVoxel> overlay;
  bool storedOnServer = false;  ///< false = locally generated, pending write-back
  bool dirty = false;           ///< local edits not yet persisted
  std::int64_t hydratedAtMs = 0;
};

/// Why the store stopped trying a chunk's write-back.
enum class ChunkWriteBackDrop {
  /// The server will refuse it again unchanged: no permission on the chunk (someone
  /// else's claimed plot, a safe zone), a closed wilderness, an invalid request.
  Refused,
  /// Every attempt failed with an error that could have cleared (busy, network, a
  /// timeout, a server error).
  Exhausted,
};

/// A chunk write-back the store stopped trying. The chunk keeps its local voxels
/// and is no longer dirty, so it can be pruned and loaded again from the server's
/// copy; the store does not undo the edit.
struct ChunkWriteBackFailure {
  ChunkCoord coord{};
  ChunkWriteBackDrop reason = ChunkWriteBackDrop::Refused;
  /// Attempts made, the last one included.
  int attempts = 0;
  /// The last attempt's outcome: `kind`, `httpStatus`, and the GraphQL errors
  /// with their extensions (`code`, `retryable`, `httpStatus`).
  graphql::GraphQLOutcome error;
};

/// One `voxelStates` entry of a stored chunk: a voxel's type and its state. Since ck-api
/// v2.33.0 the entries also carry every voxel edit recorded for the chunk (a hub's or mod's
/// `world.set_voxels`, `updateVoxel`, a realtime voxel update), none of which is in its dense
/// `voxels`.
struct StoredVoxelState {
  /// Within-chunk voxel coordinates: 0-15 lands in the dense grid, any other signed
  /// 16-bit value in the chunk's overlay (an entry outside 16 bits is ignored).
  int x = 0;
  int y = 0;
  int z = 0;
  std::int16_t voxelType = 0;
  /// Empty when the voxel has no state.
  std::vector<std::uint8_t> state;
};

/// One stored chunk, as a chunk source reports it.
struct StoredChunk {
  ChunkCoord coord{};
  /// Dense 16^3 voxel types (kChunkVolume bytes); any other size is ignored.
  std::vector<std::uint8_t> voxels;
  /// Applied over `voxels` on load: each entry's type at its voxel, and its state
  /// (in the overlay for an entry the grid cannot hold).
  std::vector<StoredVoxelState> voxelStates;
};

/// Where a ChunkStore hydrates from and writes back to. The Game API's chunks
/// surface is the default (the ChunksAPI constructor); an engine or a language
/// binding supplies its own transport. Called on the thread that calls
/// ensureAround(), tick(), pruneBeyond() and flush().
class IChunkSource {
 public:
  virtual ~IChunkSource() = default;
  /// Every stored chunk within `distance` (Chebyshev, 1-8) of `center`, with its
  /// `voxelStates`: a source that leaves them out loses every edit a hub or mod
  /// made there. May throw; the exception surfaces from ensureAround().
  virtual std::vector<StoredChunk> chunksAround(const std::string& appId,
                                                const ChunkCoord& center, int distance) = 0;
  /// Persist one chunk's voxels. The outcome classifies a failure exactly as a
  /// ChunksAPI write does: refused (dropped after one attempt) or able to clear
  /// (retried with backoff).
  virtual graphql::GraphQLOutcome writeChunk(const std::string& appId, const ChunkCoord& coord,
                                             Bytes voxels) = 0;
};

/// What ChunkStore::flush() did.
struct ChunkFlushResult {
  /// Chunks persisted.
  std::size_t persisted = 0;
  /// Write-backs dropped along the way (also reported through onWriteBackFailed).
  std::vector<ChunkWriteBackFailure> dropped;
};

class ChunkStore {
 public:
  struct Options {
    /// Persist locally generated / edited chunks back through chunks.update,
    /// at most one chunk per writeBackIntervalMs (0 disables write-back).
    ///
    /// A write the server refuses (FORBIDDEN, SCOPE_MISSING, a validation error,
    /// NOT_FOUND, `extensions.retryable: false`, HTTP 400/403/404/413/422) is dropped
    /// after that one attempt. One that fails for a reason that can clear
    /// (PLATFORM_BUSY, UNAUTHENTICATED, network, a timeout, a server error) is tried
    /// again after 0.7 s, 1.4 s, 2.8 s and 5.6 s, then dropped. Both are reported
    /// through onWriteBackFailed, and neither holds up any other chunk.
    std::int64_t writeBackIntervalMs = 700;
    std::uint8_t voxelSendDistance = 8;
    /// Attempts for a write-back whose failures can clear.
    int writeBackAttempts = 5;
    /// Wait before the second attempt; doubles for each attempt after it.
    std::int64_t writeBackBackoffMs = 700;
    /// How flush() waits out a backoff (defaults to sleeping this thread).
    std::function<void(std::int64_t ms)> sleep;
    /// Monotonic milliseconds, for how long a local edit waits for its echo
    /// (defaults to core::systemClock()).
    std::function<std::int64_t()> now;
  };

  /// How long a local edit waits for the server's echo before it is forgotten (a lost or
  /// refused send); after that a matching echo is merged like any other edit.
  static constexpr std::int64_t kPendingEditTtlMs = 10000;

  /// Hydrate and write back through the Game API. `chunksApi` may be null
  /// (offline / tests): then there is no durable store.
  ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi, std::string appId,
             Options options);
  /// Hydrate and write back through `source` (null: no durable store). The
  /// source must outlive the store.
  ChunkStore(replication::Connection& conn, IChunkSource* source, std::string appId,
             Options options)
      : conn_(conn), source_(source), appId_(std::move(appId)), options_(std::move(options)) {}
  /// No durable store; keeps a literal `nullptr` unambiguous between the two above.
  ChunkStore(replication::Connection& conn, std::nullptr_t, std::string appId, Options options)
      : ChunkStore(conn, static_cast<IChunkSource*>(nullptr), std::move(appId),
                   std::move(options)) {}

  /// Load every stored chunk within `distance` of `center` from the durable
  /// store (one round trip). Each chunk's `voxelStates` go over its dense grid:
  /// an entry's type at its voxel, and its state (an entry without one clears
  /// the cached state there); an entry the grid cannot hold goes to the overlay.
  /// Every voxel edit recorded for a chunk arrives only that way. Coordinates
  /// already cached are refreshed. Does nothing without a durable store; the
  /// source's errors propagate.
  std::size_t ensureAround(const ChunkCoord& center, int distance);

  /// Look up a cached chunk (nullptr when absent).
  const ChunkData* find(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    return it == chunks_.end() ? nullptr : &it->second;
  }

  /// Alias of ensureAround for one coordinate (loads/refreshes from the
  /// durable store).
  std::size_t hydrate(const ChunkCoord& coord) { return ensureAround(coord, 0); }

  /// Snapshot of every cached chunk.
  std::vector<const ChunkData*> list() const {
    std::vector<const ChunkData*> out;
    out.reserve(chunks_.size());
    for (const auto& [coord, chunk] : chunks_) out.push_back(&chunk);
    return out;
  }

  /// The cached voxel type at a local coordinate: the overlay's when it holds that
  /// voxel, else the dense grid's (0 when the chunk is not cached, or for a
  /// position outside the grid with no overlay entry). Signed 16-bit since 0.60.0.
  std::int16_t voxelTypeAt(const ChunkCoord& coord, int x, int y, int z) const {
    const ChunkData* c = find(coord);
    if (!c) return 0;
    if (const OverlayVoxel* wide = overlayAt(*c, x, y, z)) return wide->voxel.voxelType;
    if (!inGrid(x, y, z)) return 0;
    return c->voxels[static_cast<std::size_t>(voxelIndex(x, y, z))];
  }

  /// The cached per-voxel metadata blob at a local coordinate, the overlay's first
  /// (nullptr when none).
  const VoxelState* voxelStateAt(const ChunkCoord& coord, int x, int y, int z) const {
    const ChunkData* c = find(coord);
    if (!c) return nullptr;
    if (const OverlayVoxel* wide = overlayAt(*c, x, y, z)) {
      return wide->voxel.state.empty() ? nullptr : &wide->voxel;
    }
    if (!inGrid(x, y, z)) return nullptr;
    auto it = c->voxelStates.find(voxelIndex(x, y, z));
    return it == c->voxelStates.end() ? nullptr : &it->second;
  }

  /// Mark a chunk dirty so the write-back loop persists it.
  void markDirty(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it != chunks_.end() && options_.writeBackIntervalMs > 0 &&
        !it->second.dirty) {
      setDirty(it->second, true);
      touch(it->second);
    }
  }

  /// Alias of insertGenerated (worldgen naming parity).
  ChunkData& seed(const ChunkCoord& coord,
                  const std::array<std::uint8_t, kChunkVolume>& voxels) {
    return insertGenerated(coord, voxels);
  }

  /// Evict cached chunks farther than `distance` (Chebyshev) from `center`.
  /// A dirty chunk gets one write-back attempt first when a durable store is
  /// attached: persisted or dropped (refused, or out of attempts), it is evicted;
  /// one whose failure can still clear stays, dirty, for the next tick.
  std::size_t pruneBeyond(const ChunkCoord& center, int distance) {
    std::vector<ChunkCoord> distant;
    for (const auto& [coord, chunk] : chunks_) {
      if (chebyshev(coord, center) > distance) distant.push_back(coord);
    }
    std::size_t pruned = 0;
    for (const ChunkCoord& coord : distant) {
      auto it = chunks_.find(coord);
      if (it == chunks_.end()) continue;
      if (it->second.dirty && source_) {
        if (attemptWriteBack(it->second, lastTickMs_, nullptr) == WriteBack::Retry) continue;
        // The callbacks it fired may have changed the store.
        it = chunks_.find(coord);
        if (it == chunks_.end()) continue;
      }
      if (it->second.dirty) setDirty(it->second, false);
      forgetWriteBack(coord);
      pending_.erase(coord);
      chunks_.erase(it);
      revision_.fetch_add(1, std::memory_order_relaxed);
      ++pruned;
    }
    return pruned;
  }

  /// Persist every dirty chunk now (ignoring the write-back throttle), waiting
  /// out the backoff of one whose attempt fails for a reason that can clear.
  /// Returns what was persisted and the write-backs dropped along the way; a
  /// dropped chunk is no longer dirty.
  ChunkFlushResult flush();

  /// Observe realtime/local chunk changes (fired on ingest and setVoxel).
  void onChunkChanged(std::function<void(const ChunkData&)> cb) {
    onChunkChanged_ = std::move(cb);
  }

  /// Observe write-backs the store gave up on (refused, or out of attempts).
  /// Undo or flag the local edit here; the store does not revert it.
  void onWriteBackFailed(std::function<void(const ChunkWriteBackFailure&)> cb) {
    onWriteBackFailed_ = std::move(cb);
  }

  /// Insert a locally generated chunk (worldgen write-back pattern: chunks
  /// the server has never stored are generated client-side and persisted so
  /// the world stays identical for everyone).
  ChunkData& insertGenerated(const ChunkCoord& coord,
                             const std::array<std::uint8_t, kChunkVolume>& voxels) {
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    c.voxels = voxels;
    c.storedOnServer = false;
    setDirty(c, options_.writeBackIntervalMs > 0);
    touch(c);
    return c;
  }

  /// Optimistic edit: apply locally, replicate over UDP, mark for durable
  /// write-back. Returns the send's sequence number. A position outside 0-15 or a
  /// type outside 0-255 is kept in the chunk's overlay and sent as it is (0.60.0;
  /// before, a position outside 0-15 was refused); a position that does not fit
  /// in 16 bits, or a state over wire::voxel::kMaxStateSize (1,024 bytes), is
  /// InvalidArgument and changes nothing.
  ///
  /// The server delivers every accepted edit back to its sender (Buddy v0.37.0);
  /// ingest() matches that echo to this edit (sender uuid + sequence + voxel) and
  /// does not apply it again, so a local edit fires onChunkChanged once.
  Result<std::uint8_t> setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                std::int16_t voxelType, Bytes voxelState,
                                const core::ActorUuid& uuid) {
    if (!fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return Errc::InvalidArgument;
    if (voxelState.size() > wire::voxel::kMaxStateSize) return Errc::InvalidArgument;
    applyLocal(coord, x, y, z, voxelType, voxelState);
    auto seq = conn_.sendVoxelUpdate(coord, uuid, static_cast<std::int16_t>(x),
                                     static_cast<std::int16_t>(y), static_cast<std::int16_t>(z),
                                     voxelType, voxelState, options_.voxelSendDistance);
    if (seq.ok()) {
      rememberEdit(coord, voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                   static_cast<std::int16_t>(z)),
                   PendingEdit{uuid, seq.value(), nowMs(), false});
    }
    return seq;
  }

  /// Merge an inbound VOXEL_UPDATE_NOTIFICATION (into the overlay when the grid
  /// cannot hold it). Returns whether it changed the cache.
  ///
  /// The echo of this client's own setVoxel is not applied again, and never over a
  /// newer local edit of the same voxel. Another client's edit is applied and
  /// marks the pending local edits of that voxel stale: their echoes then restore
  /// them, because the server ordered them last.
  bool ingest(const replication::SpatialNotification& n, const wire::VoxelPayloadView& voxel) {
    if (!takeEcho(n, voxel)) return false;
    applyLocal(n.chunk, voxel.x, voxel.y, voxel.z, voxel.voxelType, voxel.state,
               /*markDirty=*/false);
    return true;
  }

  /// Drive throttled durable write-back. Call from the session tick.
  void tick(std::int64_t nowMs);

  std::size_t size() const { return chunks_.size(); }
  std::uint64_t revision() const {
    return revision_.load(std::memory_order_relaxed);
  }
  std::size_t pendingWriteBacks() const {
    return pendingWriteBacks_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr bool fitsInt16(int v) { return v >= -32768 && v <= 32767; }
  static constexpr bool inGrid(int x, int y, int z) {
    return x >= 0 && x < kChunkSize && y >= 0 && y < kChunkSize && z >= 0 && z < kChunkSize;
  }
  /// Whether the dense grid can hold an edit: a position inside it and a type 0-255.
  static constexpr bool fitsDenseGrid(int x, int y, int z, std::int16_t voxelType) {
    return inGrid(x, y, z) && voxelType >= 0 && voxelType <= 255;
  }

  /// A setVoxel the server has not echoed yet. `stale`: another client's edit of
  /// the voxel arrived since, so the echo must be applied.
  struct PendingEdit {
    core::ActorUuid uuid{};
    std::uint8_t sequence = 0;
    std::int64_t sentAtMs = 0;
    bool stale = false;
  };
  using PendingByVoxel = std::unordered_map<std::uint64_t, std::vector<PendingEdit>>;

  std::int64_t nowMs() const {
    return options_.now ? options_.now() : core::systemClock().monotonicMillis();
  }

  /// The unexpired pending edits of one voxel (expired ones dropped); null when none.
  std::vector<PendingEdit>* livePending(const ChunkCoord& coord, std::uint64_t key) {
    auto chunk = pending_.find(coord);
    if (chunk == pending_.end()) return nullptr;
    auto voxel = chunk->second.find(key);
    if (voxel == chunk->second.end()) return nullptr;
    const std::int64_t cutoff = nowMs() - kPendingEditTtlMs;
    auto& edits = voxel->second;
    edits.erase(std::remove_if(edits.begin(), edits.end(),
                               [cutoff](const PendingEdit& e) { return e.sentAtMs < cutoff; }),
                edits.end());
    if (edits.empty()) {
      chunk->second.erase(voxel);
      if (chunk->second.empty()) pending_.erase(chunk);
      return nullptr;
    }
    return &edits;
  }

  void rememberEdit(const ChunkCoord& coord, std::uint64_t key, PendingEdit edit) {
    livePending(coord, key);
    pending_[coord][key].push_back(edit);
  }

  /// Whether an inbound voxel edit should be applied (see ingest()).
  bool takeEcho(const replication::SpatialNotification& n, const wire::VoxelPayloadView& voxel) {
    const std::uint64_t key = voxelKey(voxel.x, voxel.y, voxel.z);
    std::vector<PendingEdit>* edits = livePending(n.chunk, key);
    if (!edits) return true;
    const core::ActorUuid sender = n.uuidArray();
    auto echoed = std::find_if(edits->begin(), edits->end(), [&](const PendingEdit& e) {
      return e.sequence == n.sequence && e.uuid == sender;
    });
    if (echoed == edits->end()) {
      for (PendingEdit& e : *edits) e.stale = true;
      return true;
    }
    const bool stale = echoed->stale;
    const bool newerLocalEdit = std::next(echoed) != edits->end();
    edits->erase(echoed);
    livePending(n.chunk, key);
    return stale && !newerLocalEdit;
  }

  static const OverlayVoxel* overlayAt(const ChunkData& c, int x, int y, int z) {
    if (c.overlay.empty() || !fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return nullptr;
    auto it = c.overlay.find(voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                      static_cast<std::int16_t>(z)));
    return it == c.overlay.end() ? nullptr : &it->second;
  }

  /// Write one voxel: into the dense grid when it fits there, else into the
  /// overlay (an in-grid position then holds 0 and no dense state). Never writes
  /// the grid at a position outside it. Positions must fit in 16 bits.
  static void putVoxel(ChunkData& c, int x, int y, int z, std::int16_t voxelType, Bytes state) {
    const std::uint64_t key =
        voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), static_cast<std::int16_t>(z));
    if (fitsDenseGrid(x, y, z, voxelType)) {
      c.overlay.erase(key);
      const int index = voxelIndex(x, y, z);
      c.voxels[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(voxelType);
      if (state.empty()) {
        c.voxelStates.erase(index);
      } else {
        VoxelState& vs = c.voxelStates[index];
        vs.voxelType = voxelType;
        vs.state.assign(state.begin(), state.end());
      }
      return;
    }
    if (inGrid(x, y, z)) {
      const int index = voxelIndex(x, y, z);
      c.voxels[static_cast<std::size_t>(index)] = 0;
      c.voxelStates.erase(index);
    }
    OverlayVoxel& wide = c.overlay[key];
    wide.x = static_cast<std::int16_t>(x);
    wide.y = static_cast<std::int16_t>(y);
    wide.z = static_cast<std::int16_t>(z);
    wide.voxel.voxelType = voxelType;
    wide.voxel.state.assign(state.begin(), state.end());
  }

  void applyLocal(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
                  Bytes state, bool shouldMarkDirty = true) {
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    putVoxel(c, x, y, z, voxelType, state);
    if (shouldMarkDirty && options_.writeBackIntervalMs > 0) {
      setDirty(c, true);
    }
    touch(c);
  }

  void setDirty(ChunkData& chunk, bool dirty) {
    if (chunk.dirty == dirty) return;
    chunk.dirty = dirty;
    if (dirty) {
      pendingWriteBacks_.fetch_add(1, std::memory_order_relaxed);
    } else {
      pendingWriteBacks_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void touch(ChunkData& chunk) {
    revision_.fetch_add(1, std::memory_order_relaxed);
    if (onChunkChanged_) onChunkChanged_(chunk);
  }

  enum class WriteBack { Persisted, Retry, Dropped };

  /// One write-back attempt through the durable store; on Dropped the chunk is no
  /// longer dirty, the failure is reported, and copied to `dropped` when given.
  WriteBack attemptWriteBack(ChunkData& chunk, std::int64_t nowMs,
                             std::vector<ChunkWriteBackFailure>* dropped);

  void forgetWriteBack(const ChunkCoord& coord) {
    writeBackAttempts_.erase(coord);
    writeBackDueAt_.erase(coord);
  }

  replication::Connection& conn_;
  /// The adapter the ChunksAPI constructor builds; empty when a source was injected.
  std::unique_ptr<IChunkSource> ownedSource_;
  IChunkSource* source_ = nullptr;  // null (offline / tests): no hydrate/write-back
  std::string appId_;
  Options options_;
  std::unordered_map<ChunkCoord, ChunkData, ChunkCoordHash> chunks_;
  /// Failed attempts so far, per chunk whose write-back is being retried.
  std::unordered_map<ChunkCoord, int, ChunkCoordHash> writeBackAttempts_;
  /// When such a chunk may be tried again (tick clock).
  std::unordered_map<ChunkCoord, std::int64_t, ChunkCoordHash> writeBackDueAt_;
  std::int64_t lastWriteBackMs_ = 0;
  std::int64_t lastTickMs_ = 0;
  /// This client's edits not yet echoed back, oldest first, per chunk and voxelKey.
  std::unordered_map<ChunkCoord, PendingByVoxel, ChunkCoordHash> pending_;
  std::function<void(const ChunkData&)> onChunkChanged_;
  std::function<void(const ChunkWriteBackFailure&)> onWriteBackFailed_;
  std::atomic<std::uint64_t> revision_{0};
  std::atomic<std::size_t> pendingWriteBacks_{0};
};

}  // namespace crowdy::session
