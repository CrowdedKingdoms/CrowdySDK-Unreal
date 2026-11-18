#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/keys.hpp"

namespace crowdy::domains {
class ChunksAPI;
class VoxelsAPI;
}  // namespace crowdy::domains

/// ChunkStore — a chunk/voxel cache: region loads over GraphQL, realtime merge
/// from voxel notifications, paced optimistic local edits with UDP sends, and
/// durable write-back for locally generated chunks (the shared-worldgen
/// pattern).
///
/// It is a helper for 16x16x16 chunks with one byte per voxel. Voxel positions
/// and types are the app's signed 16-bit values, which the platform does not
/// check: an edit the dense grid cannot hold (a type outside 0-255, a position
/// outside 0-15) goes to the chunk's overlay instead, and reads of that voxel
/// return it. An app with other addressing reads the raw voxel events
/// (Handlers::voxelUpdate, WorldSessionConfig::onVoxel, StoredChunk::voxelStates).
///
/// Threads (0.82.0). Everything but the durable store's calls runs on the thread
/// that calls tick(), the game thread: reads, setVoxel(), and every callback
/// (onChanges, onChunkChanged, onEditRefused, an edit's own callback,
/// onWriteBackFailed, onEditSent, a hydration's callback). The chunk source is called
/// on the store's I/O thread (Options::ioThread), or on the thread that calls runIo()
/// when that is off, never on the game thread: loads, hydrations, catch-ups and
/// write-backs are queued there and their results adopted in tick(), within
/// Options::adoptBudgetUs. An IAsyncChunkSource's answers are parsed there too.
/// ensureAround(), hydrate() and flushAndWait() wait for that work and so block.
///
/// Merging (0.84.0). Each voxel holds the last value the server confirmed and the
/// server's time for it: a stored copy's write (or its entry's `updatedAt`), a
/// notification's time, a recorded edit's time. A change older than the value held is
/// dropped, whatever order the changes arrive in, and a stored copy that arrives after
/// a newer realtime change keeps that change. This client's unanswered edits show over
/// the confirmed value; when one is refused or times out, the voxel shows the next
/// unanswered edit, or the confirmed value.
namespace crowdy::session {

/// A voxel's type and state as the store holds them. `state` points into the
/// chunk's state buffer: valid until that chunk next changes, so copy what you keep.
struct VoxelStateView {
  std::int16_t voxelType = 0;
  Bytes state;
};

/// A voxel the dense grid cannot hold (a type outside 0-255, a position outside
/// 0-15), as ChunkData::overlayAt() and forEachOverlay() report it. `state` as in
/// VoxelStateView.
struct OverlayVoxel {
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::int16_t z = 0;
  std::int16_t voxelType = 0;
  Bytes state;
};

/// The key the overlay orders the voxel at (x, y, z) by: the three signed 16-bit
/// coordinates packed into 48 bits.
inline constexpr std::uint64_t voxelKey(std::int16_t x, std::int16_t y, std::int16_t z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 32) |
         (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16) |
         static_cast<std::uint64_t>(static_cast<std::uint16_t>(z));
}

/// One cached chunk. A chunk whose voxels all have one type keeps that type alone,
/// not a 4 KiB grid; every voxel state and overlay entry lives in one buffer per
/// chunk, indexed by sorted arrays.
class ChunkData {
 public:
  ChunkCoord coord{};
  bool storedOnServer = false;  ///< false = locally generated, pending write-back
  bool dirty = false;           ///< a seeded or generated grid not yet persisted
  /// When the stored copy was last applied (0: never loaded from the durable store).
  std::int64_t hydratedAtMs = 0;
  /// The server cut this chunk's recorded edits short (65,536 for one chunk, 262,144
  /// across a page): the rest are not in the cache. Read them with voxels().list().
  bool voxelStatesTruncated = false;
  /// When the server last wrote the chunk's voxels whole (epoch ms; StoredChunk's
  /// voxelsUpdatedAtMs, or its updatedAtMs from a source that does not say), from the
  /// last read: the time of every voxel without one of its own. 0 when unknown or not
  /// written whole since ck-api v2.70.0; then merges are ordered by arrival.
  std::int64_t storedAtMs = 0;
  /// When the server last wrote the chunk's row (epoch ms; any write, a chunk-state or
  /// LOD write included), from the last read; 0 when unknown. Orders copies (0.85.0).
  std::int64_t rowAtMs = 0;

  /// Whether every voxel of the grid has one type (uniformType()).
  bool uniform() const { return grid_.empty(); }
  std::uint8_t uniformType() const { return uniform_; }
  /// The dense voxel-type grid (index x + y*16 + z*256), read-only and without a
  /// copy: the chunk's own bytes, or a shared grid of its uniform type. Never null;
  /// valid until the chunk next changes. Holds 0 where an in-grid voxel's type is in
  /// the overlay.
  const std::uint8_t* grid() const;
  /// The dense grid's type at `index` (voxelIndex(x, y, z)).
  std::uint8_t typeAt(int index) const {
    return grid_.empty() ? uniform_ : grid_[static_cast<std::size_t>(index)];
  }
  /// The state of the grid voxel at `index`, if it has one.
  std::optional<VoxelStateView> stateAt(int index) const;
  std::size_t stateCount() const { return states_.size(); }
  /// Every grid voxel with a state, by ascending index: f(int index, VoxelStateView).
  template <class F>
  void forEachState(F&& f) const {
    for (const StateRef& s : states_) {
      f(static_cast<int>(s.index), VoxelStateView{s.voxelType, bytesOf(s.offset, s.length)});
    }
  }
  std::size_t overlayCount() const { return overlay_.size(); }
  /// The overlay's voxel at (x, y, z), if it holds one.
  std::optional<OverlayVoxel> overlayAt(int x, int y, int z) const;
  /// Every overlay voxel, in voxelKey order: f(const OverlayVoxel&).
  template <class F>
  void forEachOverlay(F&& f) const {
    for (const WideRef& w : overlay_) {
      f(OverlayVoxel{w.x, w.y, w.z, w.voxelType, bytesOf(w.offset, w.length)});
    }
  }
  /// The bytes this chunk's grid, states, overlay and voxel times take.
  std::size_t memoryBytes() const {
    return grid_.capacity() + bytes_.capacity() + states_.capacity() * sizeof(StateRef) +
           overlay_.capacity() * sizeof(WideRef) + times_.capacity() * sizeof(CellTime) +
           arrivals_.capacity() * sizeof(CellArrival);
  }

 private:
  friend class ChunkStore;
  struct StateRef {
    std::uint16_t index = 0;
    std::int16_t voxelType = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
  };
  struct WideRef {
    std::uint64_t key = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t z = 0;
    std::int16_t voxelType = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
  };
  /// A voxel whose value is newer than the stored row: the server's time for it.
  struct CellTime {
    std::uint64_t key = 0;
    std::int64_t timeMs = 0;
  };
  /// A voxel merged since the stored copy, and the merge it arrived in.
  struct CellArrival {
    std::uint64_t key = 0;
    std::uint64_t seq = 0;
  };

  Bytes bytesOf(std::uint32_t offset, std::uint32_t length) const {
    return length == 0 ? Bytes() : Bytes(bytes_.data() + offset, length);
  }
  void reset();
  void setType(int index, std::uint8_t type);
  /// A whole grid (kChunkVolume bytes); a uniform one keeps its type alone.
  void setGrid(const std::uint8_t* grid);
  void setUniform(std::uint8_t type);
  /// An empty state erases the voxel's.
  void putState(int index, std::int16_t voxelType, Bytes state);
  void putWide(std::int16_t x, std::int16_t y, std::int16_t z, std::int16_t voxelType, Bytes state);
  bool eraseWide(std::uint64_t key);
  std::uint32_t append(Bytes state);
  void dropBytes(std::uint32_t length);
  /// The server's time for the voxel's value: its own, or the stored row's.
  std::int64_t timeOf(std::uint64_t key) const;
  /// The merge the voxel's value arrived in (0: the stored copy).
  std::uint64_t arrivalOf(std::uint64_t key) const;
  /// `arrival` 0: the value is the stored copy's.
  void setTime(std::uint64_t key, std::int64_t timeMs, std::uint64_t arrival);

  std::vector<std::uint8_t> grid_;   ///< empty: every voxel has uniform_
  std::uint8_t uniform_ = 0;
  std::vector<StateRef> states_;     ///< ascending index
  std::vector<WideRef> overlay_;     ///< ascending key
  std::vector<std::uint8_t> bytes_;  ///< every state's bytes, back to back
  std::vector<CellTime> times_;      ///< ascending key
  std::vector<CellArrival> arrivals_;  ///< ascending key
  std::uint32_t garbage_ = 0;        ///< bytes_ no entry points at
  std::uint32_t changes_ = 0;        ///< ChunkChangeFlag bits since the last report
  std::uint64_t version_ = 0;        ///< bumped on every content change
};

/// What changed in a chunk since the last report (ChunkStore::onChanges).
enum ChunkChangeFlag : std::uint32_t {
  kChunkLoaded = 1u << 0,        ///< a stored copy, or a seed, was applied
  kChunkVoxels = 1u << 1,        ///< voxel types changed
  kChunkStates = 1u << 2,        ///< voxel states or the overlay changed
  /// Evicted by pruneBeyond(), pruneIf() or evict(): drop what you built from it.
  kChunkUnloaded = 1u << 3,
  /// A neighbour changed a voxel on the face the two share: rebuild this chunk's border
  /// there (the face bits below say which).
  kChunkNeighborEdge = 1u << 4,
  kChunkFaceNegX = 1u << 8,
  kChunkFacePosX = 1u << 9,
  kChunkFaceNegY = 1u << 10,
  kChunkFacePosY = 1u << 11,
  kChunkFaceNegZ = 1u << 12,
  kChunkFacePosZ = 1u << 13,
};

/// One chunk's changes since the last report: ChunkChangeFlag bits.
struct ChunkChange {
  ChunkCoord coord{};
  std::uint32_t flags = 0;
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
  /// When the server recorded this value (epoch ms; ck-api v2.69.0's `updatedAt`): the
  /// edit's time, or for a state in the chunk's own row, that row's write. 0 when
  /// unknown (an older server, or a source that does not say): the entry is then as old
  /// as the stored row.
  std::int64_t updatedAtMs = 0;
};

/// One stored chunk, as a chunk source reports it.
struct StoredChunk {
  ChunkCoord coord{};
  /// Dense 16^3 voxel types (kChunkVolume bytes); any other size is ignored.
  std::vector<std::uint8_t> voxels;
  /// Applied over `voxels` on load: each entry's type at its voxel, and its state
  /// (in the overlay for an entry the grid cannot hold).
  std::vector<StoredVoxelState> voxelStates;
  /// The server cut `voxelStates` short; see ChunkData::voxelStatesTruncated.
  bool voxelStatesTruncated = false;
  /// When the server last wrote the stored row (epoch ms; 0 when unknown): any write,
  /// a chunk-state or LOD write included. Orders copies of the chunk.
  std::int64_t updatedAtMs = 0;
  /// When the chunk's voxels were last written whole (epoch ms; ck-api v2.70.0's
  /// `voxelsUpdatedAt`, an updateChunk or world.set_chunk): the time of every voxel the
  /// copy has no entry for. 0 when the server says the chunk was not written whole since
  /// v2.70.0 (then the copy and later changes are ordered by arrival). Unset for a source
  /// that does not say, whose copy is dated by `updatedAtMs` as before 0.85.0.
  std::optional<std::int64_t> voxelsUpdatedAtMs;
};

/// The voxel edits recorded in one chunk, as IChunkSource::editsInBox reports them.
struct StoredChunkEdits {
  ChunkCoord coord{};
  std::vector<StoredVoxelState> edits;
};

/// Where a ChunkStore loads from and writes back to. The Game API's chunks surface
/// is the default (the ChunksAPI constructor); an engine or a language binding
/// supplies its own transport. Since 0.82.0 every method runs on the store's I/O
/// thread (or the thread that calls runIo()), one call at a time, never on the game
/// thread.
class IChunkSource {
 public:
  virtual ~IChunkSource() = default;
  /// Every stored chunk within `distance` (Chebyshev, 0-8) of `center`, with its
  /// `voxelStates`: a source that leaves them out loses every edit a hub or mod
  /// made there. May throw; the exception fails the load (the chunks are asked for
  /// again by a later ensureAround).
  virtual std::vector<StoredChunk> chunksAround(const std::string& appId,
                                                const ChunkCoord& center, int distance) = 0;
  /// The stored chunks of a box (both corners inclusive, at most 4,913 chunks), with
  /// their `voxelStates`; with `sinceIso`, only those whose stored row was written at
  /// or after it. The default reads the box's bounding cube with chunksAround() and
  /// keeps the box (and every chunk, whatever `sinceIso`); the Game API's source reads
  /// the box alone, in pages (ck-api v2.67.0).
  virtual std::vector<StoredChunk> chunksInBox(const std::string& appId, const ChunkCoord& low,
                                               const ChunkCoord& high,
                                               const std::string& sinceIso);
  /// The voxel edits recorded in a box since `sinceIso`, grouped per chunk, for a
  /// catch-up after the connection was down. The default has none; the Game API's
  /// source reads them with listVoxelUpdatesByDistance when it has the voxels domain.
  virtual std::vector<StoredChunkEdits> editsInBox(const std::string& appId,
                                                   const ChunkCoord& low, const ChunkCoord& high,
                                                   const std::string& sinceIso) {
    (void)appId;
    (void)low;
    (void)high;
    (void)sinceIso;
    return {};
  }
  /// The one stored chunk at `coord` with its `voxelStates`, or nullopt when it was
  /// never stored. The default keeps `coord`'s chunk out of chunksAround(coord, 0);
  /// the Game API's source reads that chunk alone. May throw, as chunksAround.
  virtual std::optional<StoredChunk> chunkAt(const std::string& appId, const ChunkCoord& coord) {
    for (StoredChunk& chunk : chunksAround(appId, coord, 0)) {
      if (chunk.coord == coord) return std::move(chunk);
    }
    return std::nullopt;
  }
  /// Persist one chunk's voxels. The outcome classifies a failure exactly as a
  /// ChunksAPI write does: refused (dropped after one attempt) or able to clear
  /// (retried with backoff).
  virtual graphql::GraphQLOutcome writeChunk(const std::string& appId, const ChunkCoord& coord,
                                             Bytes voxels) = 0;
};

/// What one stored-chunk reply may hold (0.85.0). The defaults are the Game API's own
/// limits, so only a broken or hostile server's reply passes them; what passes is
/// dropped before it is decoded.
struct ChunkReadLimits {
  /// The most voxel states kept for one chunk (the Game API applies 65,536); the rest
  /// are dropped and the chunk is marked voxelStatesTruncated.
  std::size_t maxStatesPerChunk = 65536;
  /// The most voxel states or recorded edits kept across one reply (the Game API
  /// applies 262,144 to one getChunksByDistance).
  std::size_t maxStatesPerReply = 262144;
  /// The largest voxel state kept, decoded; a larger one is dropped and its type kept.
  std::size_t maxStateBytes = std::size_t{64} * 1024;
};

/// The Game API's chunk source: box reads paged by the limit the server applies (a
/// chunk two pages return is kept once), one-chunk reads with getChunk, edits since a
/// time with listVoxelUpdatesByDistance (when `voxels` is given), write-back with
/// updateChunk. `chunks` (and `voxels`) must outlive it. A grid is kept only when it
/// decodes to kChunkVolume bytes, and `limits` bounds the rest of a reply.
std::unique_ptr<IChunkSource> makeGameApiChunkSource(domains::ChunksAPI& chunks,
                                                     domains::VoxelsAPI* voxels = nullptr,
                                                     ChunkReadLimits limits = {});

/// A box of chunks, both corners inclusive.
struct ChunkBox {
  ChunkCoord low{};
  ChunkCoord high{};
};

/// One GraphQL request an IAsyncChunkSource carries for the store: a POST of `body`
/// (JSON with `query`, `variables` and `operationName`) to the Game API's `/graphql`,
/// sent with the player's token as `Authorization: Bearer`, `Content-Type:
/// application/json`.
struct ChunkSourceRequest {
  /// The operation's name, for logs and metrics.
  std::string operationName;
  std::string body;
};

/// The answer to a ChunkSourceRequest, unparsed: the store reads it on its I/O thread.
struct ChunkSourceReply {
  /// The HTTP status; 0 when no response came (network failure, timeout, cancelled).
  int httpStatus = 0;
  /// The raw response body.
  std::string body;
};

/// A chunk source with no blocking call (0.84.0), for an engine whose HTTP client is
/// asynchronous. The store builds every request itself (box reads paged by the limit the
/// server applies, one-chunk reads, edits since a time, write-back) and parses every
/// answer on its I/O thread; the source only carries bytes.
class IAsyncChunkSource {
 public:
  virtual ~IAsyncChunkSource() = default;
  /// Send `request` and call `done` once with its answer, from any thread, at any time
  /// (inline included). Called on the store's I/O thread (or the thread that calls
  /// runIo()). An answer that arrives after the store is gone is dropped.
  virtual void request(ChunkSourceRequest request,
                       std::function<void(ChunkSourceReply)> done) = 0;
};

/// What ChunkStore::flush() queued, or flushAndWait() did.
struct ChunkFlushResult {
  /// Chunks queued for write-back (flush()), or persisted (flushAndWait()).
  std::size_t persisted = 0;
  /// Write-backs dropped along the way (flushAndWait(); also reported through
  /// onWriteBackFailed).
  std::vector<ChunkWriteBackFailure> dropped;
};

/// How a setVoxel() ended.
enum class EditResult : std::uint8_t {
  /// The server applied it and echoed it back.
  Echoed,
  /// The server refused it (a GENERIC_ERROR for its sequence number: `errorCode`,
  /// Unauthorized for a voxel the player may not edit).
  Refused,
  /// The connection did not take it (not connected, a full queue): never sent.
  NotSent,
  /// No echo within Options::editTimeoutMs: it may have been applied with its echo lost.
  TimedOut,
};

/// A setVoxel()'s outcome, for its callback and onEditRefused.
struct EditOutcome {
  std::uint64_t editId = 0;
  ChunkCoord coord{};
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::int16_t z = 0;
  std::int16_t voxelType = 0;
  EditResult result = EditResult::Echoed;
  /// The server's code for Refused.
  wire::ErrorCode errorCode = wire::ErrorCode::NoError;
  /// Whether the voxel stopped showing this edit: it shows the next unanswered edit of
  /// the voxel, or the confirmed value (not when a newer edit of the voxel is what it
  /// shows, or the edit was not optimistic).
  bool restored = false;
};

class ChunkStore {
 public:
  struct Options {
    /// Persist locally generated or seeded chunks back through chunks.update, at
    /// most one chunk per writeBackIntervalMs (0 disables write-back). Realtime
    /// edits (setVoxel) are recorded by the server and are not written back.
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
    /// Unused since 0.82.0: nothing in the store sleeps.
    std::function<void(std::int64_t ms)> sleep;
    /// Monotonic milliseconds (defaults to core::systemClock()).
    std::function<std::int64_t()> now;
    /// Run the durable store's calls on a thread the store owns. Off: call runIo()
    /// from a thread of yours (an engine's worker), or nothing loads.
    bool ioThread = true;
    /// How long one tick() spends applying loaded chunks at most; the rest wait for
    /// the next tick.
    std::int64_t adoptBudgetUs = 2000;
    /// Edits sent and not yet answered (echoed, refused or timed out), 1-128; the
    /// rest wait in order.
    int maxEditsInFlight = 128;
    /// Edits sent per second at most (a quarter second's worth may go at once):
    /// half of the 500 messages a second the server takes from a session.
    int maxEditsPerSecond = 250;
    /// How long a sent edit waits for its echo before it is undone (TimedOut).
    std::int64_t editTimeoutMs = 10000;
    /// Read a held chunk again when the server reports it was replaced as a whole
    /// (chunkChanged: a hub's or mod's world.set_chunk).
    bool reloadReplacedChunks = true;
    /// When the connection comes back, catch up (catchUp()) from 30 s before it dropped.
    bool catchUpOnReconnect = true;
    /// A server time more than this far past this machine's clock counts as this
    /// machine's clock plus this much, so that one wrong clock cannot hold a voxel
    /// against every later change.
    std::int64_t maxFutureSkewMs = 5 * 60 * 1000;
    /// What one reply may hold, for the Game API's source the ChunksAPI constructor
    /// makes and for IAsyncChunkSource replies (0.85.0).
    ChunkReadLimits readLimits;
    /// The largest IAsyncChunkSource reply body accepted (0.85.0); a larger one fails
    /// its read unparsed, as GraphQLClient refuses one past maxResponseBytes.
    std::size_t maxReplyBytes = graphql::kDefaultMaxResponseBytes;
    /// Sends one edit and returns its sequence number; defaults to the connection's
    /// sendVoxelUpdate. A custom transport (or a test) replaces it.
    std::function<Result<std::uint8_t>(const ChunkCoord& chunk, const core::ActorUuid& uuid,
                                       std::int16_t x, std::int16_t y, std::int16_t z,
                                       std::int16_t voxelType, Bytes state,
                                       std::uint8_t distance)>
        send;
  };

  /// An edit's own callback, on the game thread.
  using EditDone = std::function<void(const EditOutcome&)>;
  /// A hydration's callback, on the game thread: whether a stored copy was applied.
  using HydrateDone = std::function<void(bool applied)>;
  /// Sends one edit and returns its sequence number (Options::send).
  using SendFn = std::function<Result<std::uint8_t>(
      const ChunkCoord& chunk, const core::ActorUuid& uuid, std::int16_t x, std::int16_t y,
      std::int16_t z, std::int16_t voxelType, Bytes state, std::uint8_t distance)>;

  /// Load and write back through the Game API. `chunksApi` may be null (offline /
  /// tests): then there is no durable store.
  ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi, std::string appId,
             Options options);
  /// Load and write back through `source` (null: no durable store). The source must
  /// outlive the store.
  ChunkStore(replication::Connection& conn, IChunkSource* source, std::string appId,
             Options options);
  /// Load and write back through an asynchronous source (0.84.0).
  ChunkStore(replication::Connection& conn, IAsyncChunkSource* source, std::string appId,
             Options options);
  /// No durable store; keeps a literal `nullptr` unambiguous between the ones above.
  ChunkStore(replication::Connection& conn, std::nullptr_t, std::string appId, Options options)
      : ChunkStore(conn, static_cast<IChunkSource*>(nullptr), std::move(appId),
                   std::move(options)) {}
  /// Without a replication::Connection (0.84.0): every edit goes out through `send`, for
  /// an app with its own realtime transport. Feed it notifications, errors and the
  /// connection's state with ingest(), ingestError() and connectionState().
  ChunkStore(SendFn send, IChunkSource* source, std::string appId, Options options);
  ChunkStore(SendFn send, IAsyncChunkSource* source, std::string appId, Options options);
  ChunkStore(SendFn send, std::nullptr_t, std::string appId, Options options)
      : ChunkStore(std::move(send), static_cast<IChunkSource*>(nullptr), std::move(appId),
                   std::move(options)) {}
  ~ChunkStore();
  ChunkStore(const ChunkStore&) = delete;
  ChunkStore& operator=(const ChunkStore&) = delete;

  /// Ask for the stored chunks within `distance` (Chebyshev, clamped to 0-8) of
  /// `center` that the store does not hold or have on the way, as boxes: a step
  /// along one axis at range 8 asks for the 289 chunks it uncovers. Returns at once
  /// with the number of chunks asked for; tick() applies them as they arrive. Each
  /// chunk's `voxelStates` go over its dense grid (an entry the grid cannot hold to
  /// the overlay); a chunk that holds a seeded or generated grid not yet written back
  /// is never overwritten, and this client's unanswered edits are put back over a
  /// copy that is applied. Does nothing without a durable store.
  std::size_t ensureAroundAsync(const ChunkCoord& center, int distance);
  /// ensureAroundAsync(), then wait for those loads and apply them all: blocks the
  /// calling thread. Returns the chunks applied.
  std::size_t ensureAround(const ChunkCoord& center, int distance);
  /// Chunks asked for and not yet applied.
  std::size_t loadsInFlight() const { return loading_.size(); }
  /// Chunks two pages of one box read returned (each applied once).
  std::uint64_t duplicatePageChunks() const { return duplicatePageChunks_; }

  /// Read one chunk again from the durable store (the Game API's source reads it
  /// alone, with getChunk), merged as a load is. A dirty chunk is left alone.
  /// hydrateAsync() returns at once (false without a durable store or for a dirty
  /// chunk: `done` is then never called). `done` fires on the game thread once a read
  /// asked for after this call has ended: true when its stored copy was applied, false
  /// when it failed, the chunk was never stored, or the chunk was evicted or became
  /// dirty meanwhile. A call while a read of the chunk is out waits for the next read.
  /// hydrate() waits and returns 1 when a stored copy was applied, 0 otherwise.
  bool hydrateAsync(const ChunkCoord& coord, HydrateDone done = {});
  std::size_t hydrate(const ChunkCoord& coord);

  /// Read what changed since `sinceIso` in every chunk the store holds or knows was
  /// never stored (0.84.0; the last area before): the recorded edits
  /// (IChunkSource::editsInBox), merged by their times, and the chunks whose stored row
  /// was written since then, merged again. A chunk never stored that has edits now
  /// gets a slot. The area is read in boxes of at most 17^3 chunks. The store calls it
  /// when the connection comes back, from 30 s before it dropped. Returns whether it
  /// was queued.
  bool catchUp(const std::string& sinceIso);
  /// catchUp() over `boxes` (each at most 17^3 chunks; a larger one is cut). Edits in a
  /// chunk the store does not hold give it a slot only when the chunk was never stored.
  bool catchUp(const std::string& sinceIso, const std::vector<ChunkBox>& boxes);
  /// Merge recorded edits a caller read itself (a catch-up it runs over its own transport),
  /// each at its `updatedAtMs` as a catch-up merges them; a chunk the store does not hold
  /// gets a slot. Returns whether what the cache shows changed.
  bool mergeEdits(const StoredChunkEdits& edits);

  /// When the server confirmed the value the store holds for a voxel (epoch ms): the last
  /// change it merged there, or the stored row's write. 0 when unknown. An unanswered
  /// local edit shows over that value and does not change it.
  std::int64_t confirmedAtMs(const ChunkCoord& coord, int x, int y, int z) const;

  /// Look up a cached chunk (nullptr when absent). Stable until the chunk is pruned.
  const ChunkData* find(const ChunkCoord& coord) const {
    auto it = slotOf_.find(coord);
    return it == slotOf_.end() ? nullptr : &slots_[it->second];
  }

  /// Snapshot of every cached chunk.
  std::vector<const ChunkData*> list() const;

  /// The cached voxel type at a local coordinate: the overlay's when it holds that
  /// voxel, else the dense grid's (0 when the chunk is not cached, or for a position
  /// outside the grid with no overlay entry).
  std::int16_t voxelTypeAt(const ChunkCoord& coord, int x, int y, int z) const;

  /// The cached per-voxel state at a local coordinate, the overlay's first (nullopt
  /// when none). The view is valid until the chunk next changes.
  std::optional<VoxelStateView> voxelStateAt(const ChunkCoord& coord, int x, int y, int z) const;

  /// Mark a chunk dirty so the write-back loop persists its grid.
  void markDirty(const ChunkCoord& coord);

  /// Alias of insertGenerated (worldgen naming parity).
  const ChunkData& seed(const ChunkCoord& coord,
                        const std::array<std::uint8_t, kChunkVolume>& voxels) {
    return insertGenerated(coord, voxels);
  }

  /// Insert a locally generated chunk (worldgen write-back pattern: chunks the
  /// server has never stored are generated client-side and persisted so the world
  /// stays identical for everyone).
  const ChunkData& insertGenerated(const ChunkCoord& coord,
                                   const std::array<std::uint8_t, kChunkVolume>& voxels);

  /// Evict cached chunks farther than `distance` (Chebyshev) from `center`, each
  /// reported with kChunkUnloaded. A dirty chunk stays until its write-back ends
  /// (queued now); returns the chunks evicted.
  std::size_t pruneBeyond(const ChunkCoord& center, int distance);
  /// Evict one chunk (0.84.0), as pruneBeyond() does, and forget that it was never
  /// stored. Returns whether it was evicted: false when the store does not hold it, or
  /// it is dirty (its write-back is queued and it stays until that ends).
  bool evict(const ChunkCoord& coord);
  /// Evict every chunk `drop` returns true for (0.84.0), for a view that is not a cube
  /// (a sphere, a column, several players). Returns the chunks evicted.
  std::size_t pruneIf(const std::function<bool(const ChunkData&)>& drop);

  /// Queue every dirty chunk's write-back now, ignoring the throttle; returns at
  /// once (`persisted` is the number queued). Outcomes arrive in tick().
  ChunkFlushResult flush();
  /// Persist every dirty chunk and wait for the outcomes, backoff included, for at
  /// most `timeoutMs` (shutdown; it blocks). Drives tick() itself.
  ChunkFlushResult flushAndWait(std::int64_t timeoutMs = 30000);

  /// Optimistic edit: apply locally and replicate over UDP. Sends are paced
  /// (Options::maxEditsInFlight, maxEditsPerSecond): the edit shows at once and
  /// goes out in turn. Returns the edit's id; `done` (and onEditRefused for anything
  /// but Echoed) fires on the game thread when it ends. An edit the server refuses,
  /// the connection does not take, or whose echo does not come back in time is
  /// undone. A position that does not fit in 16 bits, or a state over
  /// wire::voxel::kMaxStateSize (1,024 bytes), is InvalidArgument and changes nothing.
  ///
  /// The server delivers every accepted edit back to its sender (Buddy v0.37.0);
  /// ingest() matches that echo to this edit (sender uuid + sequence + voxel) and does
  /// not apply it again. `optimistic` false changes nothing locally: the edit is
  /// applied when its echo arrives, so the cache shows only what the server accepted.
  Result<std::uint64_t> setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                 std::int16_t voxelType, Bytes voxelState,
                                 const core::ActorUuid& uuid, bool optimistic = true,
                                 EditDone done = {});

  /// Merge an inbound VOXEL_UPDATE_NOTIFICATION (into the overlay when the grid
  /// cannot hold it) at the notification's time; one older than the value the store
  /// holds is dropped. Returns whether it changed what the cache shows. The echo of
  /// this client's own setVoxel (its sender, sequence number, voxel, type and state)
  /// confirms that edit; a change under one of this client's unanswered edits is kept
  /// as the confirmed value, which the voxel shows if the edit is refused.
  bool ingest(const replication::SpatialNotification& n, const wire::VoxelPayloadView& voxel);
  /// A GENERIC_ERROR: when its sequence number is an unanswered edit's, that edit is
  /// refused (undone, reported). Returns whether it was. Sequence numbers are one
  /// byte and every send of the connection shares them, so this is best effort: an
  /// edit undone for another send's error comes back with its echo.
  bool ingestError(const replication::GenericError& error);
  /// A chunkChanged (Buddy v0.45.0): the chunk is read again unless the store's copy is
  /// that new. A seeded chunk's queued write-back gives way to the server's copy.
  void ingestChunkChanged(const ChunkCoord& coord, std::int64_t updatedAtMs);
  /// The connection's state, for the catch-up after it comes back (the session
  /// forwards it).
  void connectionState(replication::ConnState state);

  /// Coalesced changes, once per tick() that had any: every chunk that changed with
  /// what changed (ChunkChangeFlag), and the face neighbours of a border edit.
  void onChanges(std::function<void(const std::vector<ChunkChange>&)> cb) {
    onChanges_ = std::move(cb);
  }
  /// Every change as it happens (each merge, edit and load).
  void onChunkChanged(std::function<void(const ChunkData&)> cb) {
    onChunkChanged_ = std::move(cb);
  }
  /// Observe write-backs the store gave up on (refused, or out of attempts).
  void onWriteBackFailed(std::function<void(const ChunkWriteBackFailure&)> cb) {
    onWriteBackFailed_ = std::move(cb);
  }
  /// Every edit the store undid or could not send (Refused, NotSent, TimedOut).
  void onEditRefused(std::function<void(const EditOutcome&)> cb) {
    onEditRefused_ = std::move(cb);
  }
  /// Every edit as it goes out, with the connection's sequence number for it.
  void onEditSent(std::function<void(std::uint64_t editId, std::uint8_t sequence)> cb) {
    onEditSent_ = std::move(cb);
  }
  /// The sequence number an edit went out with, while it waits for its answer.
  std::optional<std::uint8_t> sequenceOf(std::uint64_t editId) const;
  /// Where WorldSession records each send for its error attribution (apart from onEditSent,
  /// which is the game's).
  void recordSendsTo(std::function<void(std::uint8_t sequence)> recorder) {
    recordSend_ = std::move(recorder);
  }

  /// Adopt finished loads (within adoptBudgetUs) and write-backs, send queued
  /// edits, time out unanswered ones, queue the next write-back, and report the
  /// changes. Call from the session tick, on the game thread.
  void tick(std::int64_t nowMs);
  /// Run up to `maxJobs` queued durable-store calls on this thread; for
  /// Options::ioThread false. Returns the calls run.
  std::size_t runIo(std::size_t maxJobs = SIZE_MAX);

  std::size_t size() const { return slotOf_.size(); }
  std::uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }
  std::size_t pendingWriteBacks() const {
    return pendingWriteBacks_.load(std::memory_order_relaxed);
  }
  /// Edits sent and not yet answered.
  std::size_t editsInFlight() const { return inFlight_.size(); }
  /// Edits waiting for their turn to be sent.
  std::size_t queuedEdits() const { return sendQueue_.size(); }

  /// How long a sent edit waits for its echo by default.
  static constexpr std::int64_t kPendingEditTtlMs = 10000;

 private:
  struct Box {
    ChunkCoord lo{};
    ChunkCoord hi{};
  };
  struct PendingEdit {
    std::uint64_t id = 0;
    core::ActorUuid uuid{};
    int sequence = -1;  ///< -1 while queued
    std::int64_t sentAtMs = 0;
    bool optimistic = true;
    std::int16_t voxelType = 0;
    std::vector<std::uint8_t> state;
    EditDone done;
  };
  /// A voxel with unanswered local edits: the edits, oldest first, over the value the
  /// server last confirmed there and its time.
  struct PendingCell {
    std::vector<PendingEdit> edits;
    std::int16_t baseType = 0;
    std::vector<std::uint8_t> baseState;
    std::int64_t baseTimeMs = 0;
    std::uint64_t baseArrival = 0;
  };
  struct EditRef {
    ChunkCoord coord{};
    std::uint64_t key = 0;
    std::uint64_t id = 0;
  };
  using PendingByVoxel = std::unordered_map<std::uint64_t, PendingCell>;

  struct IoJob {
    enum class Kind : std::uint8_t { LoadBox, LoadChunk, Write, CatchUp } kind = Kind::LoadBox;
    Box box{};
    /// CatchUp: the boxes it reads.
    std::vector<Box> boxes;
    ChunkCoord coord{};
    std::vector<std::uint8_t> voxels;
    std::uint64_t version = 0;
    std::string since;
    /// The merge count when the read was asked for: without server times, a change merged
    /// after it is newer than what the read returns.
    std::uint64_t requestedSeq = 0;
  };
  struct IoResult {
    IoJob job;
    bool failed = false;
    std::vector<StoredChunk> chunks;
    std::optional<StoredChunk> chunk;
    std::vector<StoredChunkEdits> edits;
    graphql::GraphQLOutcome outcome;
  };
  /// A chunk's read in flight (hydrateAsync): the callbacks it settles, and those waiting
  /// for the read after it.
  struct Reload {
    bool again = false;
    bool evicted = false;
    std::vector<HydrateDone> waiting;
    std::vector<HydrateDone> next;
  };
  /// An IAsyncChunkSource job in flight: where its paging got to.
  struct AsyncCall {
    IoResult result;
    std::size_t box = 0;
    bool edits = false;  ///< CatchUp: reading the box's edits (after its chunks)
    std::int64_t skip = 0;
  };
  /// Where an IAsyncChunkSource's answers go; outlives the store, which detaches it.
  struct ReplySink {
    std::mutex mutex;
    ChunkStore* store = nullptr;
  };

  static constexpr bool fitsInt16(int v) { return v >= -32768 && v <= 32767; }
  static constexpr bool inGrid(int x, int y, int z) {
    return x >= 0 && x < kChunkSize && y >= 0 && y < kChunkSize && z >= 0 && z < kChunkSize;
  }
  static constexpr bool fitsDenseGrid(int x, int y, int z, std::int16_t voxelType) {
    return inGrid(x, y, z) && voxelType >= 0 && voxelType <= 255;
  }

  std::int64_t nowMs() const {
    return options_.now ? options_.now() : core::systemClock().monotonicMillis();
  }
  ChunkData* slot(const ChunkCoord& coord) {
    auto it = slotOf_.find(coord);
    return it == slotOf_.end() ? nullptr : &slots_[it->second];
  }
  bool durable() const { return source_ != nullptr || asyncSource_ != nullptr; }
  ChunkData& slotFor(const ChunkCoord& coord);
  void dropSlot(const ChunkCoord& coord);
  void init();

  void putVoxel(ChunkData& c, int x, int y, int z, std::int16_t voxelType, Bytes state);
  /// Show a value at a voxel, reporting what changed; returns whether anything did.
  bool show(ChunkData& c, int x, int y, int z, std::int16_t voxelType, Bytes state);
  /// Merge a confirmed value with the server's time for it. Without one (0), it is new
  /// unless it came from a read (`readSeq`) and the voxel changed after that read was
  /// asked for. Creates the chunk's slot when `create`. Returns whether what the cache
  /// shows changed.
  bool confirm(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
               Bytes state, std::int64_t timeMs, bool create, std::uint64_t readSeq = 0);
  std::int64_t clampTime(std::int64_t timeMs) const;
  void applyStored(const StoredChunk& stored, std::int64_t nowMs, std::uint64_t requestedSeq);
  bool queueCatchUp(const std::string& sinceIso, std::vector<Box> boxes);
  void changed(ChunkData& c, std::uint32_t flags);
  void borderNeighbours(const ChunkCoord& coord, int x, int y, int z);
  void setDirty(ChunkData& chunk, bool dirty);

  PendingCell* cellOf(const ChunkCoord& coord, std::uint64_t key);
  const PendingCell* cellOf(const ChunkCoord& coord, std::uint64_t key) const;
  PendingEdit* pendingById(const EditRef& ref, PendingCell** cell = nullptr);
  /// Show what a pending voxel holds: its newest optimistic edit, else its confirmed
  /// value. Returns whether what the cache shows changed.
  bool showPending(const ChunkCoord& coord, std::uint64_t key, PendingCell& cell);
  /// End an edit; an echo's time makes it the confirmed value unless that is newer.
  /// Returns whether what the cache shows changed.
  bool finishEdit(const EditRef& ref, EditResult result, wire::ErrorCode code,
                  std::int64_t echoTimeMs = 0);
  void sendQueued();
  void expireEdits();

  void enqueue(IoJob job);
  IoResult runJob(const IoJob& job);
  void ioLoop();
  /// Run queued jobs and answers on this thread; returns how many.
  std::size_t serviceIo(std::size_t maxJobs);
  void startAsync(IoJob job);
  void sendAsync(std::uint64_t id, AsyncCall& call);
  void onAsyncReply(std::uint64_t id, ChunkSourceReply reply);
  void finishAsync(std::uint64_t id);
  void postReply(std::uint64_t id, ChunkSourceReply reply);
  void startReload(const ChunkCoord& coord);
  /// Apply a result; a load stops when `deadline` passes and resumes next time.
  /// Returns whether it finished.
  bool adopt(IoResult& result, std::int64_t nowMs,
             std::chrono::steady_clock::time_point deadline);
  void finishLoad(const IoResult& result);
  bool adoptResults(std::int64_t nowMs, std::int64_t budgetUs);
  void queueWriteBack(ChunkData& chunk);
  void waitForIo(const std::function<bool()>& done, std::int64_t timeoutMs);
  void report();

  replication::Connection* conn_ = nullptr;  ///< null: Options::send carries every edit
  std::unique_ptr<IChunkSource> ownedSource_;
  IChunkSource* source_ = nullptr;  // null (offline / tests): no load/write-back
  IAsyncChunkSource* asyncSource_ = nullptr;
  std::string appId_;
  Options options_;

  std::deque<ChunkData> slots_;
  std::vector<std::uint32_t> freeSlots_;
  std::unordered_map<ChunkCoord, std::uint32_t, ChunkCoordHash> slotOf_;
  std::vector<std::uint32_t> changedSlots_;
  std::vector<ChunkChange> unloaded_;

  std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_;
  std::unordered_set<ChunkCoord, ChunkCoordHash> loading_;
  std::unordered_set<ChunkCoord, ChunkCoordHash> missing_;
  std::unordered_map<ChunkCoord, Reload, ChunkCoordHash> reloading_;
  /// A load result being applied across ticks (adoptBudgetUs), and how far it got.
  std::optional<IoResult> adopting_;
  std::size_t adoptAt_ = 0;
  std::unordered_set<ChunkCoord, ChunkCoordHash> adoptSeen_;
  std::int64_t disconnectedAtMs_ = -1;
  bool wasConnected_ = false;
  std::uint64_t duplicatePageChunks_ = 0;

  /// Write-back: failed attempts so far, when the next may go, and chunks queued.
  std::unordered_map<ChunkCoord, int, ChunkCoordHash> writeBackAttempts_;
  std::unordered_map<ChunkCoord, std::int64_t, ChunkCoordHash> writeBackDueAt_;
  std::unordered_set<ChunkCoord, ChunkCoordHash> writing_;
  std::vector<ChunkWriteBackFailure>* collectDropped_ = nullptr;
  std::size_t* collectPersisted_ = nullptr;
  std::int64_t lastWriteBackMs_ = 0;
  std::int64_t lastTickMs_ = 0;

  /// This client's edits not yet echoed back, oldest first, per chunk and voxelKey.
  std::unordered_map<ChunkCoord, PendingByVoxel, ChunkCoordHash> pending_;
  std::deque<EditRef> sendQueue_;
  std::unordered_map<std::uint8_t, EditRef> inFlight_;
  std::uint64_t nextEditId_ = 1;
  /// Confirmed values merged so far (CellTime::arrival, IoJob::requestedSeq).
  std::uint64_t mergeSeq_ = 0;
  double sendTokens_ = 0;
  std::int64_t tokensAtMs_ = -1;
  bool sending_ = false;
  std::uint64_t appliedCount_ = 0;

  std::function<void(const std::vector<ChunkChange>&)> onChanges_;
  std::function<void(const ChunkData&)> onChunkChanged_;
  std::function<void(const ChunkWriteBackFailure&)> onWriteBackFailed_;
  std::function<void(const EditOutcome&)> onEditRefused_;
  std::function<void(std::uint64_t, std::uint8_t)> onEditSent_;
  std::function<void(std::uint8_t)> recordSend_;
  std::atomic<std::uint64_t> revision_{0};
  std::atomic<std::size_t> pendingWriteBacks_{0};

  /// The I/O queue, shared with the I/O thread (everything else is the game thread's).
  std::mutex ioMutex_;
  std::condition_variable ioWake_;
  std::condition_variable ioDone_;
  std::deque<IoJob> jobs_;
  std::deque<IoResult> results_;
  /// IAsyncChunkSource answers waiting to be parsed.
  std::deque<std::pair<std::uint64_t, ChunkSourceReply>> replies_;
  std::size_t jobsRunning_ = 0;
  bool stopping_ = false;
  std::thread io_;
  /// IAsyncChunkSource jobs in flight; only the thread that services I/O touches it.
  std::unordered_map<std::uint64_t, AsyncCall> calls_;
  std::uint64_t nextCallId_ = 1;
  std::shared_ptr<ReplySink> sink_;
};

}  // namespace crowdy::session
