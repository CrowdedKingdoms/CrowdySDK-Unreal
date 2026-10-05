#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
namespace crowdy::session {

struct VoxelState {
  std::int16_t voxelType = 0;
  std::vector<std::uint8_t> state;
};

struct ChunkData {
  ChunkCoord coord{};
  /// Dense 16^3 voxel-type grid (index x + y*16 + z*256).
  std::array<std::uint8_t, kChunkVolume> voxels{};
  /// Sparse per-voxel metadata blobs, keyed by voxel index.
  std::unordered_map<int, VoxelState> voxelStates;
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
  /// Within-chunk voxel coordinates (0-15); an entry outside them is ignored.
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
  /// Applied over `voxels` on load: each entry's type at its voxel, and its state.
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
  };

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
  /// the cached state there). Every voxel edit recorded for a chunk arrives only
  /// that way. Coordinates already cached are refreshed. Does nothing without a
  /// durable store; the source's errors propagate.
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

  /// The cached voxel type at a local coordinate (0 when the chunk is not
  /// cached or the coordinate is out of range).
  std::uint8_t voxelTypeAt(const ChunkCoord& coord, int x, int y, int z) const {
    if (x < 0 || x >= kChunkSize || y < 0 || y >= kChunkSize || z < 0 || z >= kChunkSize)
      return 0;
    const ChunkData* c = find(coord);
    return c ? c->voxels[static_cast<std::size_t>(voxelIndex(x, y, z))] : 0;
  }

  /// The cached per-voxel metadata blob at a local coordinate (nullptr when
  /// none).
  const VoxelState* voxelStateAt(const ChunkCoord& coord, int x, int y, int z) const {
    const ChunkData* c = find(coord);
    if (!c) return nullptr;
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
  /// write-back. Returns the send's sequence number.
  Result<std::uint8_t> setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                std::int16_t voxelType, Bytes voxelState,
                                const core::ActorUuid& uuid) {
    if (x < 0 || x >= kChunkSize || y < 0 || y >= kChunkSize || z < 0 || z >= kChunkSize)
      return Errc::InvalidArgument;
    applyLocal(coord, x, y, z, voxelType, voxelState);
    auto seq = conn_.sendVoxelUpdate(coord, uuid, static_cast<std::int16_t>(x),
                                     static_cast<std::int16_t>(y), static_cast<std::int16_t>(z),
                                     voxelType, voxelState, options_.voxelSendDistance);
    return seq;
  }

  /// Merge an inbound VOXEL_UPDATE_NOTIFICATION.
  void ingest(const replication::SpatialNotification& n, const wire::VoxelPayloadView& voxel) {
    applyLocal(n.chunk, voxel.x, voxel.y, voxel.z, voxel.voxelType, voxel.state,
               /*markDirty=*/false);
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
  void applyLocal(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
                  Bytes state, bool shouldMarkDirty = true) {
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    const int index = voxelIndex(x, y, z);
    c.voxels[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(voxelType);
    if (state.empty()) {
      c.voxelStates.erase(index);
    } else {
      VoxelState& vs = c.voxelStates[index];
      vs.voxelType = voxelType;
      vs.state.assign(state.begin(), state.end());
    }
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
  std::function<void(const ChunkData&)> onChunkChanged_;
  std::function<void(const ChunkWriteBackFailure&)> onWriteBackFailed_;
  std::atomic<std::uint64_t> revision_{0};
  std::atomic<std::size_t> pendingWriteBacks_{0};
};

}  // namespace crowdy::session
