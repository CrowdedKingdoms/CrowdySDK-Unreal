#include "crowdy/session/world_session.hpp"

#include <chrono>
#include <string_view>
#include <thread>

#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"

namespace crowdy::session {

namespace {

/// IChunkSource over the Game API's chunks surface: the store's behaviour before
/// the source became injectable.
class ChunksApiSource final : public IChunkSource {
 public:
  explicit ChunksApiSource(domains::ChunksAPI& api) : api_(api) {}

  std::vector<StoredChunk> chunksAround(const std::string& appId, const ChunkCoord& center,
                                        int distance) override {
    graphql::Json page =
        api_.byDistance(appId, domains::ChunkRef{center.x, center.y, center.z}, distance);
    std::vector<StoredChunk> out;
    page["chunks"].forEach([&](graphql::Json chunkJson) {
      StoredChunk chunk;
      chunk.coord = {chunkJson["coordinates"]["x"].asBigInt(),
                     chunkJson["coordinates"]["y"].asBigInt(),
                     chunkJson["coordinates"]["z"].asBigInt()};
      auto voxels = core::base64Decode(chunkJson["voxels"].asStringView());
      if (voxels) chunk.voxels.assign(voxels->begin(), voxels->end());
      chunkJson["voxelStates"].forEach([&](graphql::Json entry) {
        StoredVoxelState voxel;
        voxel.x = static_cast<int>(entry["voxelCoord"]["x"].asInt64(-1));
        voxel.y = static_cast<int>(entry["voxelCoord"]["y"].asInt64(-1));
        voxel.z = static_cast<int>(entry["voxelCoord"]["z"].asInt64(-1));
        voxel.voxelType = static_cast<std::int16_t>(entry["voxelType"].asInt64());
        // A state that is not base64 is dropped; the type still applies.
        if (auto state = core::base64Decode(entry["state"].asStringView())) {
          voxel.state.assign(state->begin(), state->end());
        }
        chunk.voxelStates.push_back(std::move(voxel));
      });
      out.push_back(std::move(chunk));
    });
    return out;
  }

  graphql::GraphQLOutcome writeChunk(const std::string& appId, const ChunkCoord& coord,
                                     Bytes voxels) override {
    graphql::JVal input;
    input["appId"] = appId;
    input["coordinates"] = domains::ChunkRef{coord.x, coord.y, coord.z}.toInput();
    input["voxels"] = core::base64Encode(voxels);
    return api_.updateOutcome(input);
  }

 private:
  domains::ChunksAPI& api_;
};

/// IHostElection over the Game API's host surface.
class HostApiElection final : public IHostElection {
 public:
  explicit HostApiElection(CrowdyClient& client) : client_(client) {}

  Beat heartbeat(const std::string& appId) override {
    Beat beat;
#ifndef CROWDY_NO_EXCEPTIONS
    try {
#endif
      graphql::Json host = client_.host().heartbeat(appId);
      if (host.ok()) {
        beat.ok = true;
        beat.amIHost = client_.host().amIHost(appId);
        beat.hostUserId = host["hostUserId"].asString();
      }
#ifndef CROWDY_NO_EXCEPTIONS
    } catch (const std::exception&) {
      // Host tracking is best-effort; the next interval retries.
    }
#endif
    return beat;
  }

 private:
  CrowdyClient& client_;
};

}  // namespace

ChunkStore::ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi,
                       std::string appId, Options options)
    : conn_(conn), appId_(std::move(appId)), options_(std::move(options)) {
  if (chunksApi) {
    ownedSource_ = std::make_unique<ChunksApiSource>(*chunksApi);
    source_ = ownedSource_.get();
  }
}

WorldSession::WorldSession(std::shared_ptr<replication::Connection> conn, CrowdyClient* client,
                           WorldSessionConfig config)
    : conn_(std::move(conn)), config_(std::move(config)) {
  if (client) {
    ownedChunkSource_ = std::make_unique<ChunksApiSource>(client->chunks());
    ownedHost_ = std::make_unique<HostApiElection>(*client);
    host_ = ownedHost_.get();
  }
  init(ownedChunkSource_.get());
}

WorldSession::WorldSession(std::shared_ptr<replication::Connection> conn,
                           WorldSessionServices services, WorldSessionConfig config)
    : conn_(std::move(conn)), config_(std::move(config)), host_(services.host) {
  init(services.chunks);
}

void WorldSession::init(IChunkSource* chunks) {
  if (config_.actorUuid.size() == uuid_.size()) {
    core::actorUuidFromString(config_.actorUuid, uuid_);
  } else {
    uuid_ = core::generateActorUuid();
  }

  self_ = std::make_unique<LocalActorStore>(*conn_, uuid_, config_.self);
  actors_ = std::make_unique<RemoteActorStore>(uuid_, config_.actors);
  chunks_ = std::make_unique<ChunkStore>(*conn_, chunks, config_.appId, config_.chunks);
  installHandlers();
}

WorldSession::~WorldSession() = default;

void WorldSession::installHandlers() {
  replication::Handlers handlers;
  handlers.actorUpdate = [this](const replication::SpatialNotification& n) {
    if (n.uuidArray() == uuid_) {
      self_->recordAck(n.sequence, n.epochMillis, n.payload,
                       core::systemClock().monotonicMillis());
      return;
    }
    actors_->ingest(n, core::systemClock().monotonicMillis());
  };
  handlers.voxelUpdate = [this](const replication::SpatialNotification& n,
                                const wire::VoxelPayloadView& voxel) {
    chunks_->ingest(n, voxel);
    if (config_.onVoxel) config_.onVoxel(n, voxel);
  };
  handlers.clientEvent = [this](const replication::SpatialNotification& n,
                                const wire::EventPayloadView& payload) {
    events_.ingest(n, payload, /*fromServer=*/false, core::systemClock().monotonicMillis());
  };
  handlers.serverEvent = [this](const replication::SpatialNotification& n,
                                const wire::EventPayloadView& payload) {
    events_.ingest(n, payload, /*fromServer=*/true, core::systemClock().monotonicMillis());
  };
  // Media has no store; hand it to the game (OQ5: the session used to swallow it).
  if (config_.onAudio) handlers.audio = config_.onAudio;
  if (config_.onVideo) handlers.video = config_.onVideo;
  if (config_.onText) handlers.text = config_.onText;
  if (config_.onGenericSpatial) handlers.genericSpatial = config_.onGenericSpatial;
  if (config_.onChannelAudio) handlers.channelAudio = config_.onChannelAudio;
  // The server's departure notice removes the actor now (onLeave fires from the
  // store) instead of after the staleAfterMs reap, then the game is told too.
  handlers.actorLeft = [this](const replication::SpatialNotification& n, std::uint8_t reason) {
    const auto uuid = n.uuidArray();
    if (uuid == uuid_) return;
    actors_->remove(uuid);
    if (config_.onActorLeft) config_.onActorLeft(uuid, reason);
  };
  handlers.genericError = [this](const replication::GenericError& e) {
    const auto now = core::systemClock().monotonicMillis();
    self_->recordError(e, now);
    errors_.ingest(e, now);
  };
  handlers.channelMessage = [this](const replication::ChannelNotification& n) {
    InboxMessage m;
    m.channelId = n.channelId;
    std::memcpy(m.senderUuid.data(), n.senderUuid, m.senderUuid.size());
    m.payload.assign(n.payload.begin(), n.payload.end());
    m.serverEpochMs = n.epochMillis;
    m.receivedAtMs = core::systemClock().monotonicMillis();
    channelInbox_.push(std::move(m));
  };
  handlers.singleActorMessage = [this](const replication::SpatialNotification& n) {
    InboxMessage m;
    m.channelId = 0;
    m.senderUuid = n.uuidArray();  // the TARGET uuid (you); sender is app-defined in payload
    m.payload.assign(n.payload.begin(), n.payload.end());
    m.serverEpochMs = n.epochMillis;
    m.receivedAtMs = core::systemClock().monotonicMillis();
    directInbox_.push(std::move(m));
  };
  conn_->setHandlers(std::move(handlers));
}

void WorldSession::tick() {
  const std::int64_t nowMs = core::systemClock().monotonicMillis();

  // 1) Drain inbound notifications into the stores.
  conn_->poll();

  // 2) Presence send loop.
  self_->tick(nowMs);
  if (auto seq = self_->lastSequence()) {
    errors_.recordSend(*seq, SendKind::ActorUpdate, self_->uuid());
  }

  // 3) Staleness reaping.
  if (nowMs - lastReapMs_ >= config_.reapIntervalMs) {
    actors_->reap(nowMs);
    lastReapMs_ = nowMs;
  }

  // 4) Durable chunk write-back (throttled).
  chunks_->tick(nowMs);

  // 5) Host heartbeat (a blocking call on this thread; opt out via interval 0).
  if (host_ && config_.hostHeartbeatIntervalMs > 0 &&
      nowMs - lastHostBeatMs_ >= config_.hostHeartbeatIntervalMs) {
    lastHostBeatMs_ = nowMs;
    IHostElection::Beat beat = host_->heartbeat(config_.appId);
    if (beat.ok) {
      amIHost_ = beat.amIHost;
      if (beat.hostUserId != hostUserId_) {
        hostUserId_ = std::move(beat.hostUserId);
        if (onHostChanged_) onHostChanged_(hostUserId_);
      }
    }
  }

  // 6) A tick is a frame boundary: whatever this frame sent (presence above,
  //    plus anything the game sent through the connection since the last
  //    tick) leaves in one datagram now rather than at the end of the bundle
  //    window. Matters most under manualPump, where nothing else would flush
  //    until the next pump(). No-op when Config::bundleSends is off.
  (void)conn_->flushSends();
}

void WorldSession::dispose() {
  if (conn_) conn_->disconnect();
}

// ---------------------------------------------------------------------------
// ChunkStore GraphQL paths (here to keep the header free of domain includes)
// ---------------------------------------------------------------------------

std::size_t ChunkStore::ensureAround(const ChunkCoord& center, int distance) {
  if (!source_) return 0;
  // The durable byDistance query accepts a Chebyshev radius of 1-8.
  if (distance < 1) distance = 1;
  if (distance > 8) distance = 8;
  std::vector<StoredChunk> stored = source_->chunksAround(appId_, center, distance);
  std::size_t hydrated = 0;
  const std::int64_t nowMs = core::systemClock().monotonicMillis();
  for (const StoredChunk& chunk : stored) {
    ChunkData& c = chunks_[chunk.coord];
    c.coord = chunk.coord;
    c.storedOnServer = true;
    c.hydratedAtMs = nowMs;
    if (chunk.voxels.size() == c.voxels.size()) {
      std::memcpy(c.voxels.data(), chunk.voxels.data(), c.voxels.size());
    }
    for (const StoredVoxelState& entry : chunk.voxelStates) {
      if (!fitsInt16(entry.x) || !fitsInt16(entry.y) || !fitsInt16(entry.z)) continue;
      putVoxel(c, entry.x, entry.y, entry.z, entry.voxelType,
               Bytes(entry.state.data(), entry.state.size()));
    }
    touch(c);
    ++hydrated;
  }
  return hydrated;
}

namespace {

bool isWriteBackRefusalCode(std::string_view code) {
  static constexpr std::string_view kCodes[] = {
      "FORBIDDEN",      "SCOPE_MISSING",   "NOT_ALLOWED",
      "BAD_REQUEST",    "BAD_USER_INPUT",  "INVALID_REQUEST",
      "GRAPHQL_VALIDATION_FAILED",         "NOT_FOUND",
  };
  for (std::string_view refused : kCodes) {
    if (code == refused) return true;
  }
  return false;
}

bool isWriteBackRefusalStatus(int status) {
  return status == 400 || status == 403 || status == 404 || status == 413 || status == 422;
}

/// Whether a failed write-back can succeed if sent again unchanged. A permission or
/// validation refusal cannot; a busy platform, an expired session, a network drop, a
/// timeout or a server error can. Mirrors CrowdyJS `writeBackRetryable`.
bool writeBackRetryable(const graphql::GraphQLOutcome& out) {
  switch (out.kind) {
    case graphql::GraphQLErrorKind::Network:
    case graphql::GraphQLErrorKind::Timeout:
    case graphql::GraphQLErrorKind::Protocol:
      return true;
    case graphql::GraphQLErrorKind::Http:
      return !isWriteBackRefusalStatus(out.httpStatus);
    case graphql::GraphQLErrorKind::GraphQL:
      break;
    case graphql::GraphQLErrorKind::None:
      return true;
  }
  for (const auto& error : out.errors) {
    if (error.code == "PLATFORM_BUSY" || error.code == "UNAUTHENTICATED") continue;
    if (!error.retryable) return false;
    if (isWriteBackRefusalCode(error.code)) return false;
    if (error.httpStatus && isWriteBackRefusalStatus(*error.httpStatus)) return false;
  }
  return true;
}

}  // namespace

ChunkStore::WriteBack ChunkStore::attemptWriteBack(ChunkData& chunk, std::int64_t nowMs,
                                                   std::vector<ChunkWriteBackFailure>* dropped) {
  graphql::GraphQLOutcome out =
      source_->writeChunk(appId_, chunk.coord, Bytes(chunk.voxels.data(), chunk.voxels.size()));
  if (out.ok()) {
    forgetWriteBack(chunk.coord);
    setDirty(chunk, false);
    chunk.storedOnServer = true;
    touch(chunk);
    return WriteBack::Persisted;
  }

  const int attempts = writeBackAttempts_[chunk.coord] + 1;
  const bool retryable = writeBackRetryable(out);
  if (retryable && attempts < options_.writeBackAttempts) {
    writeBackAttempts_[chunk.coord] = attempts;
    writeBackDueAt_[chunk.coord] =
        nowMs + (options_.writeBackBackoffMs << (attempts - 1));
    return WriteBack::Retry;
  }

  forgetWriteBack(chunk.coord);
  setDirty(chunk, false);
  ChunkWriteBackFailure failure;
  failure.coord = chunk.coord;
  failure.reason = retryable ? ChunkWriteBackDrop::Exhausted : ChunkWriteBackDrop::Refused;
  failure.attempts = attempts;
  failure.error = std::move(out);
  if (onWriteBackFailed_) onWriteBackFailed_(failure);
  if (dropped) dropped->push_back(std::move(failure));
  return WriteBack::Dropped;
}

ChunkFlushResult ChunkStore::flush() {
  ChunkFlushResult result;
  if (!source_) return result;
  std::vector<ChunkCoord> dirty;
  for (const auto& [coord, chunk] : chunks_) {
    if (chunk.dirty) dirty.push_back(coord);
  }
  for (const ChunkCoord& coord : dirty) {
    for (;;) {
      auto it = chunks_.find(coord);
      if (it == chunks_.end() || !it->second.dirty) break;
      auto pending = writeBackAttempts_.find(coord);
      if (pending != writeBackAttempts_.end() && pending->second > 0) {
        const std::int64_t waitMs = options_.writeBackBackoffMs << (pending->second - 1);
        if (options_.sleep) {
          options_.sleep(waitMs);
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
      }
      const WriteBack done = attemptWriteBack(it->second, lastTickMs_, &result.dropped);
      if (done == WriteBack::Persisted) ++result.persisted;
      if (done != WriteBack::Retry) break;
    }
  }
  return result;
}

void ChunkStore::tick(std::int64_t nowMs) {
  lastTickMs_ = nowMs;
  if (!source_ || options_.writeBackIntervalMs <= 0) return;
  if (nowMs - lastWriteBackMs_ < options_.writeBackIntervalMs) return;

  // The first dirty chunk that is due: one waiting out a backoff does not hold up
  // the others.
  for (auto& [coord, chunk] : chunks_) {
    if (!chunk.dirty) continue;
    auto due = writeBackDueAt_.find(coord);
    if (due != writeBackDueAt_.end() && due->second > nowMs) continue;
    lastWriteBackMs_ = nowMs;
    attemptWriteBack(chunk, nowMs, nullptr);
    break;  // at most one chunk per interval
  }
}

}  // namespace crowdy::session
