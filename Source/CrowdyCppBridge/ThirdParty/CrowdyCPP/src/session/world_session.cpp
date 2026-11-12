#include "crowdy/session/world_session.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"

namespace crowdy::session {

namespace {

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

WorldSession::WorldSession(std::shared_ptr<replication::Connection> conn, CrowdyClient* client,
                           WorldSessionConfig config)
    : conn_(std::move(conn)), config_(std::move(config)) {
  if (client) {
    ownedChunkSource_ = makeGameApiChunkSource(client->chunks(), &client->voxels());
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
  chunks_->recordSendsTo(
      [this](std::uint8_t sequence) { errors_.recordSend(sequence, SendKind::VoxelUpdate, uuid_); });
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
  handlers.chunkChanged = [this](const replication::SpatialNotification& n,
                                 std::int64_t updatedAtMs) {
    chunks_->ingestChunkChanged(n.chunk, updatedAtMs);
    if (config_.onChunkChanged) config_.onChunkChanged(n.chunk, updatedAtMs);
  };
  handlers.status = [this](replication::ConnState state) { chunks_->connectionState(state); };
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
    chunks_->ingestError(e);
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

  // 4) The chunk store: adopt finished loads, send queued edits, write back (throttled).
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

}  // namespace crowdy::session
