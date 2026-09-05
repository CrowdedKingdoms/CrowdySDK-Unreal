#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/keys.hpp"

/// Session stores — the C++ analog of CrowdyJS's World Stores: the
/// source-of-truth data structures every game otherwise hand-writes, driven
/// by ONE replication connection and one tick.
///
/// Threading: all stores are single-threaded by design. WorldSession::tick()
/// polls the connection (dispatching handlers into the stores) on the calling
/// thread — the same thread that reads them. Reads are plain snapshots.
namespace crowdy::session {

/// Maximum actor-state payload the stores handle (well under the ~1.1 KB
/// spatial payload budget; keep real poses far smaller).
constexpr std::size_t kMaxStateBytes = 256;

struct StateBlob {
  std::uint8_t data[kMaxStateBytes];
  std::uint16_t len = 0;

  Bytes bytes() const { return Bytes(data, len); }
  void assign(Bytes b) {
    len = static_cast<std::uint16_t>(b.size() > kMaxStateBytes ? kMaxStateBytes : b.size());
    if (len > 0) std::memcpy(data, b.data(), len);
  }
  bool equals(Bytes b) const {
    return b.size() == len && (len == 0 || std::memcmp(data, b.data(), len) == 0);
  }
};

// ---------------------------------------------------------------------------
// LocalActorStore — your own actor: presence loop with send-on-change
// ---------------------------------------------------------------------------

enum class LocalActorSendReason {
  Join,
  Move,
  Interval,
  Keyframe,
  Manual,
  Refresh,
};

enum class LocalActorStatus { Idle, Pending, Acked, Error };

struct SentActorUpdate {
  StateBlob state;
  ChunkCoord chunk{};
  std::optional<std::uint8_t> sequence;
  std::int64_t sentAtMs = 0;
  LocalActorSendReason reason = LocalActorSendReason::Interval;
};

struct LocalActorSendError {
  Status status = Errc::Rejected;
  std::optional<wire::ErrorCode> serverCode;
  std::optional<std::uint8_t> sequence;
  std::int64_t receivedAtMs = 0;
};

/// Drives the local actor's presence: joins by sending the first actor
/// update, then re-sends at a fixed rate with send-on-change dedup and
/// periodic keyframes, and falls back to cheap heartbeats while idle.
class LocalActorStore {
 public:
  struct Options {
    int sendHz = 5;
    /// Force a full send at least this often even when unchanged (keeps
    /// presence fresh and repairs lost packets).
    std::int64_t keyframeIntervalMs = 3000;
    /// While unchanged, send CLIENT_ACTOR_HEARTBEAT at this interval instead
    /// of a full update (0 disables heartbeats; keyframes still flow).
    std::int64_t heartbeatIntervalMs = 2000;
    std::uint8_t distance = 8;
    wire::DecayRate decay = wire::DecayRate::Exponential;
  };

  LocalActorStore(replication::Connection& conn, core::ActorUuid uuid, Options options)
      : conn_(conn), uuid_(uuid), options_(options) {}

  const core::ActorUuid& uuid() const { return uuid_; }
  const ChunkCoord& chunk() const { return chunk_; }
  bool joined() const { return joined_; }

  /// Join the world: set the starting chunk and send the first actor update.
  Status join(const ChunkCoord& chunk, Bytes initialState) {
    {
      std::lock_guard lock(observationMutex_);
      chunk_ = chunk;
      state_.assign(initialState);
      dirty_ = true;
      joined_ = true;
    }
    return sendNow(/*nowMs=*/0, LocalActorSendReason::Join);
  }

  /// Update the actor's chunk (as it moves across chunk boundaries).
  void setChunk(const ChunkCoord& chunk) {
    std::lock_guard lock(observationMutex_);
    if (!(chunk == chunk_)) {
      chunk_ = chunk;
      dirty_ = true;
    }
  }

  /// Move to a chunk and send immediately (does not wait for the next tick
  /// slot) — crossing a chunk boundary should never lag a send interval.
  Status moveTo(const ChunkCoord& chunk) {
    setChunk(chunk);
    std::int64_t now = 0;
    {
      std::lock_guard lock(observationMutex_);
      now = lastSendMs_;
    }
    return sendNow(now, LocalActorSendReason::Move);
  }

  /// Force a full resend now (e.g. after regaining focus or reconnecting).
  Status refresh() {
    std::int64_t now = 0;
    {
      std::lock_guard lock(observationMutex_);
      now = lastSendMs_;
    }
    return sendNow(now, LocalActorSendReason::Refresh);
  }

  /// Replace the replicated state; a change is sent on the next tick slot.
  void setState(Bytes state) {
    std::lock_guard lock(observationMutex_);
    if (!state_.equals(state)) {
      state_.assign(state);
      dirty_ = true;
    }
  }

  /// The last update the server echoed back to us (sequence + server time).
  struct Ack {
    std::uint8_t sequence = 0;
    std::int64_t serverEpochMs = 0;
    std::int64_t receivedAtMs = 0;
    StateBlob state;
  };
  std::optional<Ack> lastAck() const {
    std::lock_guard lock(observationMutex_);
    return lastAck_;
  }

  /// Thread-safe immutable snapshots for render/diagnostic threads.
  StateBlob state() const {
    std::lock_guard lock(observationMutex_);
    return state_;
  }
  std::optional<SentActorUpdate> lastSent() const {
    std::lock_guard lock(observationMutex_);
    return lastSent_;
  }
  std::optional<LocalActorSendError> lastError() const {
    std::lock_guard lock(observationMutex_);
    return lastError_;
  }
  LocalActorStatus status() const {
    std::lock_guard lock(observationMutex_);
    if (!lastSent_) return LocalActorStatus::Idle;
    if (lastError_ &&
        lastError_->receivedAtMs >= lastSent_->sentAtMs) {
      return LocalActorStatus::Error;
    }
    if (lastAck_ && lastAck_->receivedAtMs >= lastSent_->sentAtMs) {
      return LocalActorStatus::Acked;
    }
    return LocalActorStatus::Pending;
  }

  /// Record a self-echo notification (called by WorldSession when an actor
  /// update for our own uuid arrives).
  void recordAck(std::uint8_t sequence, std::int64_t serverEpochMs,
                 Bytes state, std::int64_t receivedAtMs) {
    std::lock_guard lock(observationMutex_);
    Ack ack;
    ack.sequence = sequence;
    ack.serverEpochMs = serverEpochMs;
    ack.receivedAtMs = receivedAtMs;
    ack.state.assign(state);
    lastAck_ = std::move(ack);
    inFlight_[sequence] = false;
  }

  void recordError(const replication::GenericError& error,
                   std::int64_t receivedAtMs) {
    std::lock_guard lock(observationMutex_);
    if (!inFlight_[error.sequence]) return;
    inFlight_[error.sequence] = false;
    lastError_ = LocalActorSendError{
        .status = Errc::Rejected,
        .serverCode = error.code,
        .sequence = error.sequence,
        .receivedAtMs = receivedAtMs};
  }

  /// Drive the send loop. Call every frame with a monotonic clock.
  void tick(std::int64_t nowMs) {
    bool joined = false;
    bool dirty = false;
    std::int64_t lastSend = 0;
    std::int64_t lastFullSend = 0;
    std::int64_t lastHeartbeat = 0;
    {
      std::lock_guard lock(observationMutex_);
      joined = joined_;
      dirty = dirty_;
      lastSend = lastSendMs_;
      lastFullSend = lastFullSendMs_;
      lastHeartbeat = lastHeartbeatMs_;
    }
    if (!joined) return;
    const std::int64_t interval = 1000 / (options_.sendHz > 0 ? options_.sendHz : 5);
    if (nowMs - lastSend < interval) return;

    const bool keyframeDue =
        nowMs - lastFullSend >= options_.keyframeIntervalMs;
    if (dirty || keyframeDue) {
      sendNow(nowMs, dirty ? LocalActorSendReason::Interval
                           : LocalActorSendReason::Keyframe);
    } else if (options_.heartbeatIntervalMs > 0 &&
               nowMs - lastHeartbeat >= options_.heartbeatIntervalMs) {
      ChunkCoord chunk;
      {
        std::lock_guard lock(observationMutex_);
        chunk = chunk_;
      }
      // Only record the heartbeat once it has actually gone out. Marking a
      // deferred or failed heartbeat as sent silently swallows the interval,
      // and presence lapses while every counter still reads healthy.
      if (conn_.sendHeartbeat(chunk, uuid_).ok()) {
        std::lock_guard lock(observationMutex_);
        lastHeartbeatMs_ = nowMs;
        lastSendMs_ = nowMs;
      }
    }
  }

  std::optional<std::uint8_t> lastSequence() const {
    std::lock_guard lock(observationMutex_);
    return lastSequence_;
  }

 private:
  Status sendNow(std::int64_t nowMs, LocalActorSendReason reason) {
    ChunkCoord chunk;
    StateBlob state;
    {
      std::lock_guard lock(observationMutex_);
      chunk = chunk_;
      state = state_;
    }
    replication::SpatialSend p;
    p.chunk = chunk;
    p.uuid = uuid_;
    p.payload = state.bytes();
    p.distance = options_.distance;
    p.decay = options_.decay;
    auto seq = conn_.sendActorUpdate(p);
    std::lock_guard lock(observationMutex_);
    lastSent_ = SentActorUpdate{
        state, chunk,
        seq.ok() ? std::optional<std::uint8_t>(seq.value())
                 : std::nullopt,
        nowMs, reason};
    if (seq.ok()) {
      lastSequence_ = seq.value();
      inFlight_[seq.value()] = true;
      dirty_ = false;
      lastSendMs_ = nowMs;
      lastFullSendMs_ = nowMs;
    } else if (seq.error() != Errc::WouldBlock) {
      lastError_ = LocalActorSendError{
          .status = Status(seq.error()),
          .serverCode = std::nullopt,
          .sequence = std::nullopt,
          .receivedAtMs = nowMs};
    }
    // A deferred send is backpressure, not an error: the datagram never left,
    // so dirty_ stays set and the next tick retries it. Recording it in
    // lastError_ would make status() report Error for a merely busy socket.
    // Connection::stats().sendsDeferred is where saturation is observable.
    return seq.ok() ? Status(Errc::Ok) : Status(seq.error());
  }

  replication::Connection& conn_;
  core::ActorUuid uuid_;
  Options options_;
  ChunkCoord chunk_{};
  StateBlob state_{};
  bool joined_ = false;
  bool dirty_ = false;
  std::int64_t lastSendMs_ = 0;
  std::int64_t lastFullSendMs_ = 0;
  std::int64_t lastHeartbeatMs_ = 0;
  std::optional<std::uint8_t> lastSequence_;
  std::optional<Ack> lastAck_;
  std::optional<SentActorUpdate> lastSent_;
  std::optional<LocalActorSendError> lastError_;
  std::array<bool, 256> inFlight_{};
  mutable std::mutex observationMutex_;
};

// ---------------------------------------------------------------------------
// RemoteActorStore — everyone else: registry with staleness + sample history
// ---------------------------------------------------------------------------

struct ActorSample {
  StateBlob state;
  std::int64_t serverEpochMs = 0;
  std::int64_t receivedAtMs = 0;
};

struct RemoteActor {
  core::ActorUuid uuid{};
  ChunkCoord chunk{};
  std::int64_t lastSeenMs = 0;         ///< monotonic, for staleness
  std::int64_t lastServerEpochMs = 0;  ///< server time of the latest update
  /// Newest-first ring of recent samples (for interpolation).
  std::vector<ActorSample> samples;

  Bytes state() const { return samples.empty() ? Bytes() : samples.front().state.bytes(); }
};

/// One lane of the remote-actor registry: a filtered sub-registry so
/// different actor kinds (players vs mobs) can be tracked/decoded once and
/// read separately. Lanes are created via RemoteActorStore::lane and fed by
/// its ingest path.
class RemoteActorLane {
 public:
  struct Options {
    /// Include an update in this lane? (e.g. discriminate by a payload tag
    /// byte). Empty = accept everything.
    std::function<bool(const replication::SpatialNotification&)> filter;
    std::int64_t staleAfterMs = 12000;
    int historySize = 2;
  };

  explicit RemoteActorLane(Options options) : options_(std::move(options)) {}
  RemoteActorLane(const RemoteActorLane&) = delete;
  RemoteActorLane& operator=(const RemoteActorLane&) = delete;
  RemoteActorLane(RemoteActorLane&& other) noexcept
      : options_(std::move(other.options_)),
        actors_(std::move(other.actors_)),
        onJoin_(std::move(other.onJoin_)),
        onLeave_(std::move(other.onLeave_)),
        onUpdate_(std::move(other.onUpdate_)),
        revision_(other.revision_.load(std::memory_order_relaxed)) {}
  RemoteActorLane& operator=(RemoteActorLane&& other) noexcept {
    if (this != &other) {
      options_ = std::move(other.options_);
      actors_ = std::move(other.actors_);
      onJoin_ = std::move(other.onJoin_);
      onLeave_ = std::move(other.onLeave_);
      onUpdate_ = std::move(other.onUpdate_);
      revision_.store(other.revision_.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }
    return *this;
  }

  /// Feed one (already self-filtered) update; returns true when accepted.
  bool apply(const replication::SpatialNotification& n, std::int64_t nowMs) {
    if (options_.filter && !options_.filter(n)) return false;
    const core::ActorUuid uuid = n.uuidArray();
    auto [it, inserted] = actors_.try_emplace(uuid);
    RemoteActor& actor = it->second;
    actor.uuid = uuid;
    actor.chunk = n.chunk;
    actor.lastSeenMs = nowMs;
    actor.lastServerEpochMs = n.epochMillis;

    ActorSample sample;
    sample.state.assign(n.payload);
    sample.serverEpochMs = n.epochMillis;
    sample.receivedAtMs = nowMs;
    actor.samples.insert(actor.samples.begin(), std::move(sample));
    if (actor.samples.size() > static_cast<std::size_t>(options_.historySize))
      actor.samples.resize(static_cast<std::size_t>(options_.historySize));

    revision_.fetch_add(1, std::memory_order_relaxed);
    if (inserted && onJoin_) onJoin_(actor);
    if (onUpdate_) onUpdate_(actor);
    return true;
  }

  /// Drop actors not seen within staleAfterMs (fires onLeave per reaped).
  void reap(std::int64_t nowMs) {
    for (auto it = actors_.begin(); it != actors_.end();) {
      if (nowMs - it->second.lastSeenMs > options_.staleAfterMs) {
        if (onLeave_) onLeave_(it->second);
        it = actors_.erase(it);
        revision_.fetch_add(1, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }

  const RemoteActor* find(const core::ActorUuid& uuid) const {
    auto it = actors_.find(uuid);
    return it == actors_.end() ? nullptr : &it->second;
  }

  /// Snapshot of every live actor in this lane.
  std::vector<const RemoteActor*> list() const {
    std::vector<const RemoteActor*> out;
    out.reserve(actors_.size());
    for (const auto& [uuid, actor] : actors_) out.push_back(&actor);
    return out;
  }

  void clear() {
    const auto removed = actors_.size();
    actors_.clear();
    revision_.fetch_add(removed, std::memory_order_relaxed);
  }
  std::size_t size() const { return actors_.size(); }
  std::uint64_t revision() const {
    return revision_.load(std::memory_order_relaxed);
  }

  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (const auto& [uuid, actor] : actors_) fn(actor);
  }

  void onJoin(std::function<void(const RemoteActor&)> cb) { onJoin_ = std::move(cb); }
  void onLeave(std::function<void(const RemoteActor&)> cb) { onLeave_ = std::move(cb); }
  void onUpdate(std::function<void(const RemoteActor&)> cb) { onUpdate_ = std::move(cb); }

 private:
  Options options_;
  std::unordered_map<core::ActorUuid, RemoteActor, ActorUuidHash> actors_;
  std::function<void(const RemoteActor&)> onJoin_;
  std::function<void(const RemoteActor&)> onLeave_;
  std::function<void(const RemoteActor&)> onUpdate_;
  std::atomic<std::uint64_t> revision_{0};
};

/// The remote-actor registry: a default lane holding everyone, plus optional
/// named lanes with filters (players vs mobs). Self-filtered.
class RemoteActorStore {
 public:
  struct Options {
    std::int64_t staleAfterMs = 12000;
    int historySize = 2;
  };

  RemoteActorStore(core::ActorUuid selfUuid, Options options)
      : selfUuid_(selfUuid),
        options_(options),
        defaultLane_(RemoteActorLane::Options{nullptr, options.staleAfterMs,
                                              options.historySize}) {}

  /// Create/fetch a named lane. New lanes inherit the store's staleness and
  /// history settings unless overridden.
  RemoteActorLane& lane(const std::string& name, RemoteActorLane::Options options = {}) {
    auto it = lanes_.find(name);
    if (it == lanes_.end()) {
      if (options.staleAfterMs == 12000) options.staleAfterMs = options_.staleAfterMs;
      if (options.historySize == 2) options.historySize = options_.historySize;
      it = lanes_.emplace(name, RemoteActorLane(std::move(options))).first;
    }
    return it->second;
  }

  /// Ingest an ACTOR_UPDATE_NOTIFICATION (self-filtered) into the default
  /// lane and every named lane whose filter accepts it.
  void ingest(const replication::SpatialNotification& n, std::int64_t nowMs) {
    if (n.uuidArray() == selfUuid_) return;
    defaultLane_.apply(n, nowMs);
    for (auto& [name, lane] : lanes_) lane.apply(n, nowMs);
  }

  /// Drop actors not seen within staleAfterMs across all lanes.
  void reap(std::int64_t nowMs) {
    defaultLane_.reap(nowMs);
    for (auto& [name, lane] : lanes_) lane.reap(nowMs);
  }

  const RemoteActor* find(const core::ActorUuid& uuid) const { return defaultLane_.find(uuid); }
  std::vector<const RemoteActor*> list() const { return defaultLane_.list(); }
  void clear() {
    defaultLane_.clear();
    for (auto& [name, lane] : lanes_) lane.clear();
  }
  std::size_t size() const { return defaultLane_.size(); }
  std::uint64_t revision() const {
    std::uint64_t value = defaultLane_.revision();
    for (const auto& [name, lane] : lanes_) {
      (void)name;
      value += lane.revision();
    }
    return value;
  }

  template <typename Fn>
  void forEach(Fn&& fn) const {
    defaultLane_.forEach(std::forward<Fn>(fn));
  }

  void onJoin(std::function<void(const RemoteActor&)> cb) { defaultLane_.onJoin(std::move(cb)); }
  void onLeave(std::function<void(const RemoteActor&)> cb) {
    defaultLane_.onLeave(std::move(cb));
  }
  void onUpdate(std::function<void(const RemoteActor&)> cb) {
    defaultLane_.onUpdate(std::move(cb));
  }

 private:
  core::ActorUuid selfUuid_;
  Options options_;
  RemoteActorLane defaultLane_;
  std::map<std::string, RemoteActorLane> lanes_;
};

// ---------------------------------------------------------------------------
// ErrorStore — server-reported send errors attributed to what you sent
// ---------------------------------------------------------------------------

enum class SendKind : std::uint8_t {
  Unknown = 0,
  ActorUpdate,
  VoxelUpdate,
  Audio,
  Text,
  ClientEvent,
  GenericSpatial,
  SingleActor,
  Channel,
  Heartbeat,
};

struct AttributedError {
  wire::ErrorCode code;
  std::uint8_t sequence;
  SendKind kind;
  std::optional<core::ActorUuid> actorUuid;
  std::int64_t atMs;
};

/// Correlates GENERIC_ERROR_MESSAGE frames (sequence-numbered, uint8 wrap)
/// with the kind of send that used that sequence.
class ErrorStore {
 public:
  struct SentPacketRecord {
    SendKind kind = SendKind::Unknown;
    std::optional<core::ActorUuid> actorUuid;
  };

  void recordSend(std::uint8_t sequence, SendKind kind,
                  std::optional<core::ActorUuid> actorUuid = std::nullopt) {
    sent_[sequence] = SentPacketRecord{kind, std::move(actorUuid)};
  }

  void ingest(const replication::GenericError& e, std::int64_t nowMs) {
    const auto& send = sent_[e.sequence];
    AttributedError err{e.code, e.sequence,
                        send ? send->kind : SendKind::Unknown,
                        send ? send->actorUuid
                             : std::optional<core::ActorUuid>{},
                        nowMs};
    recent_.push_front(err);
    if (recent_.size() > kMaxErrors) recent_.pop_back();
    total_.fetch_add(1, std::memory_order_relaxed);
    if (onError_) onError_(recent_.front());
  }

  /// Most recent errors first.
  const std::deque<AttributedError>& recent() const { return recent_; }
  std::vector<AttributedError> recent(std::size_t limit) const {
    const auto count = std::min(limit, recent_.size());
    return {recent_.begin(), recent_.begin() +
                                 static_cast<std::ptrdiff_t>(count)};
  }

  /// The single most recent error, if any.
  const AttributedError* last() const {
    return recent_.empty() ? nullptr : &recent_.front();
  }

  /// Native convenience: the most recent error for a send family.
  const AttributedError* lastFor(SendKind kind) const {
    for (const auto& error : recent_) {
      if (error.kind == kind) return &error;
    }
    return nullptr;
  }

  /// The most recent error attributed to one actor UUID.
  const AttributedError* lastFor(const core::ActorUuid& uuid) const {
    for (const auto& error : recent_) {
      if (error.actorUuid && *error.actorUuid == uuid) return &error;
    }
    return nullptr;
  }

  /// Observe every attributed error as it arrives (poll-thread callback).
  void onError(std::function<void(const AttributedError&)> cb) { onError_ = std::move(cb); }

  void clear() {
    recent_.clear();
  }
  /// Lifetime count; clear() intentionally only clears the retained ring.
  std::uint64_t total() const {
    return total_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr std::size_t kMaxErrors = 64;
  std::optional<SentPacketRecord> sent_[256];
  std::deque<AttributedError> recent_;
  std::function<void(const AttributedError&)> onError_;
  std::atomic<std::uint64_t> total_{0};
};

// ---------------------------------------------------------------------------
// Inboxes — channel + direct messages
// ---------------------------------------------------------------------------

struct InboxMessage {
  std::int64_t channelId = 0;  ///< 0 for direct (single-actor) messages
  core::ActorUuid senderUuid{};  ///< for direct messages: the TARGET uuid (you)
  std::vector<std::uint8_t> payload;
  std::int64_t serverEpochMs = 0;
  std::int64_t receivedAtMs = 0;
};

class Inbox {
 public:
  explicit Inbox(std::size_t maxMessages = 256) : max_(maxMessages) {}

  void push(InboxMessage message) {
    messages_.push_back(std::move(message));
    if (messages_.size() > max_) messages_.pop_front();
    channels_.insert(messages_.back().channelId);
    if (onMessage_) onMessage_(messages_.back());
  }

  /// Drain up to `max` messages (oldest first).
  std::vector<InboxMessage> drain(std::size_t max = SIZE_MAX) {
    std::vector<InboxMessage> out;
    while (!messages_.empty() && out.size() < max) {
      out.push_back(std::move(messages_.front()));
      messages_.pop_front();
    }
    return out;
  }

  /// Non-consuming view of the retained messages (oldest first). Optionally
  /// filtered by channel id (0 = direct messages).
  std::vector<const InboxMessage*> messages(std::optional<std::int64_t> channelId
                                            = std::nullopt) const {
    std::vector<const InboxMessage*> out;
    for (const auto& m : messages_) {
      if (!channelId || m.channelId == *channelId) out.push_back(&m);
    }
    return out;
  }

  /// Channel ids seen so far (0 = direct messages).
  std::vector<std::int64_t> channels() const {
    return std::vector<std::int64_t>(channels_.begin(), channels_.end());
  }

  /// Observe every message as it arrives (poll-thread callback), in addition
  /// to it being retained for drain()/messages().
  void onMessage(std::function<void(const InboxMessage&)> cb) { onMessage_ = std::move(cb); }

  /// Publish to a channel (channel-inbox flavor; requires membership +
  /// send_messages, enforced server-side).
  Result<std::uint8_t> send(replication::Connection& conn, std::int64_t channelId,
                            const core::ActorUuid& senderUuid, Bytes payload) {
    return conn.sendChannelMessage(channelId, senderUuid, payload);
  }

  /// Direct message to one actor (direct-inbox flavor; fire-and-forget).
  Result<std::uint8_t> send(replication::Connection& conn, const ChunkCoord& targetChunk,
                            const core::ActorUuid& targetUuid, Bytes payload) {
    return conn.sendSingleActorMessage(targetChunk, targetUuid, payload);
  }

  void clear() { messages_.clear(); }
  std::size_t size() const { return messages_.size(); }

 private:
  std::size_t max_;
  std::deque<InboxMessage> messages_;
  std::set<std::int64_t> channels_;
  std::function<void(const InboxMessage&)> onMessage_;
};

// ---------------------------------------------------------------------------
// EventRouter — typed client/server-event routing
// ---------------------------------------------------------------------------

/// Routes client/server event notifications ([2B eventType][state...]) to
/// per-type handlers, with a retained last-event per type. The C++ analog of
/// the World Stores event router.
class EventRouter {
 public:
  struct Event {
    std::uint16_t eventType = 0;
    bool fromServer = false;
    core::ActorUuid senderUuid{};
    ChunkCoord chunk{};
    std::vector<std::uint8_t> state;
    std::int64_t serverEpochMs = 0;
    std::int64_t receivedAtMs = 0;
  };

  /// Register a handler for one eventType (replaces any previous handler).
  void on(std::uint16_t eventType, std::function<void(const Event&)> handler) {
    handlers_[eventType] = std::move(handler);
  }

  /// The most recent event of a type, if any has arrived.
  const Event* lastEvent(std::uint16_t eventType) const {
    auto it = last_.find(eventType);
    return it == last_.end() ? nullptr : &it->second;
  }

  /// Send a typed client event through the connection.
  Result<std::uint8_t> send(replication::Connection& conn, const ChunkCoord& chunk,
                            const core::ActorUuid& uuid, std::uint16_t eventType, Bytes state,
                            std::uint8_t distance = 8,
                            wire::DecayRate decay = wire::DecayRate::None) {
    return conn.sendClientEvent(chunk, uuid, eventType, state, distance, decay);
  }

  /// Ingest a client/server event notification (called by WorldSession).
  void ingest(const replication::SpatialNotification& n, const wire::EventPayloadView& payload,
              bool fromServer, std::int64_t nowMs) {
    Event e;
    e.eventType = payload.eventType;
    e.fromServer = fromServer;
    e.senderUuid = n.uuidArray();
    e.chunk = n.chunk;
    e.state.assign(payload.state.begin(), payload.state.end());
    e.serverEpochMs = n.epochMillis;
    e.receivedAtMs = nowMs;
    auto it = handlers_.find(e.eventType);
    Event& stored = last_[e.eventType] = std::move(e);
    if (it != handlers_.end() && it->second) it->second(stored);
  }

 private:
  std::unordered_map<std::uint16_t, std::function<void(const Event&)>> handlers_;
  std::unordered_map<std::uint16_t, Event> last_;
};

}  // namespace crowdy::session
