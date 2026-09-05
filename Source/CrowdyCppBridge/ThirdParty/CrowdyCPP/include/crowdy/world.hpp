#pragma once

#include <memory>
#include <optional>
#include <string>

#include "crowdy/replication/connection.hpp"

/// World facade — thin, appId-pre-bound helpers over the native replication
/// connection, mirroring CrowdyJS's client.world(appId): grab an actor and
/// join/sendState/sendText/sendToActor without touching wire types. Advanced
/// callers use the Connection (or WorldSession) directly.
namespace crowdy::world {

/// One actor's convenience surface. Remembers its chunk so sends do not need
/// to repeat it.
class ActorClient {
 public:
  ActorClient(std::shared_ptr<replication::Connection> conn, core::ActorUuid uuid)
      : conn_(std::move(conn)), uuid_(uuid) {}

  const core::ActorUuid& uuid() const { return uuid_; }
  const wire::ChunkCoord& chunk() const { return chunk_; }
  void setChunk(const wire::ChunkCoord& chunk) { chunk_ = chunk; }

  /// Join the world: send the first actor update at `chunk`.
  Result<std::uint8_t> join(const wire::ChunkCoord& chunk, Bytes initialState = {}) {
    chunk_ = chunk;
    return sendState(initialState);
  }

  /// Replicate this actor's state from its current chunk.
  Result<std::uint8_t> sendState(Bytes state, std::uint8_t distance = 8,
                                 wire::DecayRate decay = wire::DecayRate::Exponential) {
    return conn_->sendActorUpdate({chunk_, uuid_, state, distance, decay});
  }

  /// Proximity text to everyone nearby.
  Result<std::uint8_t> sendText(std::string_view text, std::uint8_t distance = 8) {
    return conn_->sendText({chunk_, uuid_, asBytes(text), distance, wire::DecayRate::None});
  }

  /// Typed client event to everyone nearby.
  Result<std::uint8_t> sendEvent(std::uint16_t eventType, Bytes state = {},
                                 std::uint8_t distance = 8) {
    return conn_->sendClientEvent(chunk_, uuid_, eventType, state, distance);
  }

  /// Direct message to one other actor (you supply its uuid + current chunk;
  /// fire-and-forget, no echo — embed your identity in the payload if the
  /// recipient needs it).
  Result<std::uint8_t> sendToActor(const core::ActorUuid& targetUuid,
                                   const wire::ChunkCoord& targetChunk, Bytes payload) {
    return conn_->sendSingleActorMessage(targetChunk, targetUuid, payload);
  }

  /// Realtime voxel edit at this actor's chunk.
  Result<std::uint8_t> sendVoxelUpdate(std::int16_t x, std::int16_t y, std::int16_t z,
                                       std::int16_t voxelType, Bytes voxelState = {},
                                       std::uint8_t distance = 8) {
    return conn_->sendVoxelUpdate(chunk_, uuid_, x, y, z, voxelType, voxelState, distance);
  }

  /// Idle keep-alive so presence never lapses while not moving.
  Result<std::uint8_t> heartbeat() { return conn_->sendHeartbeat(chunk_, uuid_); }

 private:
  std::shared_ptr<replication::Connection> conn_;
  core::ActorUuid uuid_;
  wire::ChunkCoord chunk_{};
};

/// The world facade for one app: mints ActorClients over a connected native
/// replication connection.
class WorldClient {
 public:
  WorldClient(std::shared_ptr<replication::Connection> conn, std::string appId)
      : conn_(std::move(conn)), appId_(std::move(appId)) {}

  const std::string& appId() const { return appId_; }
  replication::Connection& connection() { return *conn_; }

  /// An actor helper with a fresh uuid (or the one you pass).
  ActorClient actor(std::optional<core::ActorUuid> uuid = std::nullopt) {
    return ActorClient(conn_, uuid ? *uuid : core::generateActorUuid());
  }

 private:
  std::shared_ptr<replication::Connection> conn_;
  std::string appId_;
};

}  // namespace crowdy::world
