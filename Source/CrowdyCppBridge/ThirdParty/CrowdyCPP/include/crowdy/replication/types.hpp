#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "crowdy/core/uuid.hpp"
#include "crowdy/wire/codec.hpp"

namespace crowdy::replication {

/// App-scoped token material needed for native UDP: the 64-octet token is
/// the HMAC key, the gameTokenId goes in every message tail, and expiry
/// drives proactive refresh.
struct TokenInfo {
  std::string token;               ///< 64-character app-scoped token
  std::int64_t gameTokenId = 0;
  std::int64_t expiresAtEpochMs = 0;  ///< 0 = non-expiring
};

/// A replication-server assignment (from the Game API's
/// serverWithLeastClients, which also installs the UDP session server-side).
struct Assignment {
  std::string ip4;
  std::string ip6;
  int clientPort = 0;
};

enum class ConnState : std::uint8_t {
  Idle = 0,
  Connecting,    ///< assigning a server / waiting for session-ready
  Connected,
  Reconnecting,  ///< re-assigning after COMMAND_RECONNECT / TOKEN_EXPIRED / watchdog
  Failed,
  Closed,
};

inline const char* connStateName(ConnState s) {
  switch (s) {
    case ConnState::Idle: return "Idle";
    case ConnState::Connecting: return "Connecting";
    case ConnState::Connected: return "Connected";
    case ConnState::Reconnecting: return "Reconnecting";
    case ConnState::Failed: return "Failed";
    case ConnState::Closed: return "Closed";
  }
  return "?";
}

struct Config {
  std::int64_t appId = 0;
  TokenInfo token;

  /// Manual pump mode: no SDK threads; you call pump() (network work) and
  /// poll() (callback dispatch) yourself. Default: the SDK owns a net thread
  /// and you only call poll().
  bool manualPump = false;

  /// Prefer the IPv6 assignment address. Default IPv4 (most reachable).
  bool preferIpv6 = false;

  /// Wait after server assignment before the session accepts traffic
  /// (session install is asynchronous server-side).
  int sessionReadyWaitMs = 1500;

  /// Refresh the app token this many ms before expiresAt (0 disables).
  std::int64_t refreshLeadMs = 5 * 60 * 1000;

  /// Verify server HMACs on inbound long-spatial notifications and drop
  /// mismatches (recommended; disable only for benchmarking).
  bool verifyNotifications = true;

  /// Silent-drop watchdog: if >0 and we have received traffic before, going
  /// this long with sends flowing but nothing received triggers reassignment.
  /// Disabled by default (a lone player legitimately receives nothing).
  std::int64_t watchdogSilenceMs = 0;

  /// Capacity (events) of the net->game notification ring.
  std::size_t ringCapacity = 4096;

  /// Kernel receive buffer size hint.
  int socketRecvBufferBytes = 1 << 20;

  /// Kernel send buffer size hint. Raise it for a client that emits many
  /// entities in one frame: the send buffer is what absorbs a burst, and when
  /// it fills, sends return Errc::WouldBlock until it drains.
  int socketSendBufferBytes = 1 << 20;
};

/// Parameters for a spatial send. Payload bytes are copied into the send
/// buffer during the call and need not outlive it.
struct SpatialSend {
  wire::ChunkCoord chunk;
  core::ActorUuid uuid{};
  Bytes payload;
  std::uint8_t distance = 8;
  wire::DecayRate decay = wire::DecayRate::None;
};

/// One dispatched notification. Spans point into the poll buffer and are
/// valid only during the callback — copy anything you keep.
struct SpatialNotification {
  wire::MessageType type;
  std::int64_t appId;
  wire::ChunkCoord chunk;
  const char* uuid;  ///< 32 bytes, not null-terminated
  Bytes payload;
  std::int64_t epochMillis;
  std::uint8_t sequence;

  core::ActorUuid uuidArray() const {
    core::ActorUuid u;
    std::memcpy(u.data(), uuid, wire::kUuidSize);
    return u;
  }
};

struct ChannelNotification {
  std::int64_t channelId;
  const char* senderUuid;  ///< 32 bytes
  Bytes payload;
  std::int64_t epochMillis;
  std::uint8_t sequence;
};

struct GenericError {
  std::uint8_t sequence;
  wire::ErrorCode code;
};

/// Notification handlers, mirroring CrowdyJS's UdpNotificationHandlers.
/// All callbacks fire on the thread that calls poll().
struct Handlers {
  std::function<void(const SpatialNotification&)> actorUpdate;
  std::function<void(const SpatialNotification&, const wire::VoxelPayloadView&)> voxelUpdate;
  std::function<void(const SpatialNotification&)> audio;
  std::function<void(const SpatialNotification&)> text;
  std::function<void(const SpatialNotification&, const wire::EventPayloadView&)> clientEvent;
  std::function<void(const SpatialNotification&, const wire::EventPayloadView&)> serverEvent;
  std::function<void(const SpatialNotification&)> genericSpatial;
  std::function<void(const SpatialNotification&)> singleActorMessage;
  std::function<void(const ChannelNotification&)> channelMessage;
  std::function<void(const GenericError&)> genericError;
  /// Every spatial notification, before the typed handler.
  std::function<void(const SpatialNotification&)> any;
  std::function<void(ConnState)> status;
};

}  // namespace crowdy::replication
