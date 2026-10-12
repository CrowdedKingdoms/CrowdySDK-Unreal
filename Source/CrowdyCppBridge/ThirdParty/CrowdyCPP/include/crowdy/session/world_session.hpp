#pragma once

#include <functional>
#include <memory>
#include <string>

#include "crowdy/session/chunk_store.hpp"
#include "crowdy/session/stores.hpp"

namespace crowdy {
class CrowdyClient;
}

/// WorldSession — SDK-managed game state over ONE replication connection and
/// one tick, mirroring CrowdyJS's createWorldSession:
///
///   - self():    your actor (uuid minted, presence at a fixed Hz with
///                send-on-change + keyframes + idle heartbeats)
///   - actors():  remote-actor registry (self-filtered, staleness-reaped,
///                sample history for interpolation)
///   - chunks():  chunk/voxel cache (bulk hydrate, realtime merge, optimistic
///                edits, worldgen write-back)
///   - errors():  server-reported send errors attributed to send kinds
///   - channelInbox() / directInbox(): message queues
///   - hostTracker: periodic host heartbeat + amIHost cache
///
/// Single-threaded: call tick(nowMs) from your game loop; it polls the
/// connection (dispatching into the stores), drives the send loop, reaps
/// stale actors, writes back dirty chunks, and heartbeats host eligibility.
/// All reads are plain snapshots on the same thread.
namespace crowdy::session {

/// Host election as WorldSession tracks it. CrowdyClient's HostAPI is the
/// default (the CrowdyClient constructor); an engine or a language binding
/// supplies its own. Called from tick() every hostHeartbeatIntervalMs.
class IHostElection {
 public:
  virtual ~IHostElection() = default;
  /// One heartbeat's answer. `ok = false` (a failed call) leaves the cached host
  /// as it was; the next interval tries again.
  struct Beat {
    bool ok = false;
    std::string hostUserId;
    bool amIHost = false;
  };
  /// Heartbeat this player's host eligibility for `appId` and report the elected
  /// host. Must not throw.
  virtual Beat heartbeat(const std::string& appId) = 0;
};

/// The durable services a WorldSession can use; each may be null (that service is
/// then off). Both must outlive the session.
struct WorldSessionServices {
  IChunkSource* chunks = nullptr;
  IHostElection* host = nullptr;
};

struct WorldSessionConfig {
  std::string appId;
  LocalActorStore::Options self;
  RemoteActorStore::Options actors;
  ChunkStore::Options chunks;
  /// Send host.heartbeat over GraphQL this often (0 disables host tracking).
  /// Note: this is a blocking HTTP call on the tick thread; disable it and
  /// run your own if that is unacceptable.
  std::int64_t hostHeartbeatIntervalMs = 3000;
  /// Reap stale remote actors this often.
  std::int64_t reapIntervalMs = 1000;
  /// Persist this actor uuid instead of minting a fresh one.
  std::string actorUuid;
  /// Media the session has no store for, forwarded to the game (v0.30.0). The
  /// session owns the connection's handlers, so without these a WorldSession
  /// user could receive neither voice nor video. `onVideo` receives one
  /// fragment; feed its payload to a media::VideoFrameAssembler.
  std::function<void(const replication::SpatialNotification&)> onAudio;
  std::function<void(const replication::SpatialNotification&)> onVideo;
  /// Proximity text, which the session has no store for either (0.54.0).
  std::function<void(const replication::SpatialNotification&)> onText;
  /// App-defined spatial messages (GENERIC_SPATIAL_1, opcode 140; Connection::sendGenericSpatial
  /// sends one), which the session has no store for (0.60.0).
  std::function<void(const replication::SpatialNotification&)> onGenericSpatial;
  /// Every inbound voxel update, after chunks() has merged it, for games that keep
  /// their own world and need the sender and the state blob of each edit (0.60.0).
  /// It includes the server's echo of this client's own edits (Buddy v0.37.0 delivers
  /// every accepted edit back to its sender): compare the notification's uuid and
  /// sequence with your send's. chunks() applies that echo idempotently.
  std::function<void(const replication::SpatialNotification&, const wire::VoxelPayloadView&)> onVoxel;
  /// Channel audio (opcode 36, Buddy v0.37.0) from another member of a channel this
  /// player belongs to, which the session has no store for (0.60.0). Feed the payload to
  /// a media::VoiceJitterBuffer keyed by channel id and sender uuid.
  std::function<void(const replication::ChannelNotification&)> onChannelAudio;
  /// The server announced a departure (Buddy v0.25.0). The session already
  /// removed the actor from `actors()` and fired its onLeave; this is for state
  /// the game keeps outside the store (voice/video textures, name tags).
  std::function<void(const core::ActorUuid&, std::uint8_t reason)> onActorLeft;
};

class WorldSession {
 public:
  /// Build over an already-connected replication connection. `client` may be
  /// null for offline/tests (chunk hydrate + host tracking are then off).
  WorldSession(std::shared_ptr<replication::Connection> conn, CrowdyClient* client,
               WorldSessionConfig config);
  /// Build over injected durable services instead of a CrowdyClient (0.54.0).
  WorldSession(std::shared_ptr<replication::Connection> conn, WorldSessionServices services,
               WorldSessionConfig config);
  ~WorldSession();

  LocalActorStore& self() { return *self_; }
  RemoteActorStore& actors() { return *actors_; }
  ChunkStore& chunks() { return *chunks_; }
  ErrorStore& errors() { return errors_; }
  EventRouter& events() { return events_; }
  Inbox& channelInbox() { return channelInbox_; }
  Inbox& directInbox() { return directInbox_; }
  replication::Connection& connection() { return *conn_; }

  const core::ActorUuid& actorUuid() const { return uuid_; }
  bool amIHost() const { return amIHost_; }
  /// The elected host's user id ("" while unknown).
  const std::string& hostUserId() const { return hostUserId_; }
  /// Observe host transitions (fires from tick() when the host changes).
  void onHostChanged(std::function<void(const std::string& hostUserId)> cb) {
    onHostChanged_ = std::move(cb);
  }

  /// Join the world (first actor update) at `chunk` with the initial state.
  Status join(const ChunkCoord& chunk, Bytes initialState) {
    return self_->join(chunk, initialState);
  }

  /// Drive everything. Call once per frame from the game loop.
  void tick();

  /// Dispose: disconnects the replication connection.
  void dispose();

 private:
  void init(IChunkSource* chunks);
  void installHandlers();

  std::shared_ptr<replication::Connection> conn_;
  WorldSessionConfig config_;
  /// The adapters the CrowdyClient constructor builds; empty when services were injected.
  std::unique_ptr<IChunkSource> ownedChunkSource_;
  std::unique_ptr<IHostElection> ownedHost_;
  IHostElection* host_ = nullptr;
  core::ActorUuid uuid_{};

  std::unique_ptr<LocalActorStore> self_;
  std::unique_ptr<RemoteActorStore> actors_;
  std::unique_ptr<ChunkStore> chunks_;
  ErrorStore errors_;
  EventRouter events_;
  Inbox channelInbox_;
  Inbox directInbox_;

  bool amIHost_ = false;
  std::string hostUserId_;
  std::function<void(const std::string&)> onHostChanged_;
  std::int64_t lastReapMs_ = 0;
  std::int64_t lastHostBeatMs_ = 0;
};

}  // namespace crowdy::session
