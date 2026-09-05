#pragma once

#include <cstddef>
#include <cstdint>

/// Public Replication API wire protocol constants.
///
/// Source of truth for external integrators:
///   https://docs.crowdedkingdoms.com/replication-api/wire-formats
///   https://docs.crowdedkingdoms.com/replication-api/hmac
///
/// All multi-byte integers are little-endian. Actor UUIDs are 32 ASCII octets
/// (no null terminator). The 64-character app-scoped token is used as-is as
/// 64 key octets (never hex-decoded).
namespace crowdy::wire {

/// Client-facing UDP message types. Spatial types (128-255) have the high bit
/// set; non-spatial/control types are 0-127.
enum class MessageType : std::uint8_t {
  BadMessage = 0,
  /// Server -> client: several messages packed into one datagram.
  MessageBundle = 2,
  /// Server -> client: [type][seq][errorCode] correlated by sequence number.
  GenericError = 3,
  /// Client -> server: publish to a channel (always HMAC-authenticated).
  ChannelMessageRequest = 17,
  /// Server -> client: a channel message delivered to a member.
  ChannelMessageNotification = 18,
  /// Server -> client: reconnect to a different server (load shedding).
  /// [type][32B HMAC over the type byte]. Always sent un-bundled.
  CommandReconnect = 22,
  /// Client -> server: keep-alive for the client's own actor. Reuses the long
  /// spatial layout; no fan-out.
  ClientActorHeartbeat = 26,

  ActorUpdateRequest = 128,
  ActorUpdateNotification = 130,
  VoxelUpdateRequest = 131,
  VoxelUpdateNotification = 133,
  ClientAudioPacket = 134,
  ClientAudioNotification = 135,
  ClientTextPacket = 136,
  ClientTextNotification = 137,
  /// Client sends and server replicates with the same type.
  ClientEventNotification = 138,
  /// Servers send only.
  ServerEventNotification = 139,
  GenericSpatial1 = 140,
  // 141 (SHORT_SPATIAL_MESSAGE) is reserved and not implemented server-side.
  /// Actor-to-actor message. Long spatial layout; chunk + uuid address the
  /// DESTINATION actor; distance/decay ignored.
  SingleActorMessage = 142,
};

/// Error codes carried by GenericError frames.
enum class ErrorCode : std::uint8_t {
  NoError = 0,
  UnknownError = 1,
  InvalidToken = 5,
  AppNotFound = 6,
  Unauthorized = 7,
  GameTokenWrongSize = 13,
  InvalidRequest = 15,
  /// The packet's appId is not one this token is scoped to.
  InvalidAppId = 18,
  UserNotAuthenticated = 20,
  /// App-scoped token passed expiresAt — refresh or re-mint, then re-assign.
  TokenExpired = 32,
};

/// Replication density across Chebyshev distance rings 1-8.
enum class DecayRate : std::uint8_t {
  None = 0,
  Exponential = 1,
  Linear50 = 2,
  Linear25 = 3,
  Linear10 = 4,
  Linear5 = 5,
};

constexpr std::uint8_t kSpatialTypeBit = 0x80;

inline constexpr bool isSpatialType(std::uint8_t t) noexcept {
  return (t & kSpatialTypeBit) != 0;
}

/// True for the long HMAC spatial layout: types 128-140, SingleActorMessage
/// (142), and the non-spatial ClientActorHeartbeat (26) which reuses it.
inline constexpr bool isLongSpatialLayout(std::uint8_t t) noexcept {
  return (t >= 128 && t <= 140) || t == 142 ||
         t == static_cast<std::uint8_t>(MessageType::ClientActorHeartbeat);
}

// ---- Long form spatial layout offsets --------------------------------------
// [1B type][8B appId][8B chunkX][8B chunkY][8B chunkZ]
// [1B distance][1B decay][1B containsAuth][32B uuid]
// [payload ...]
// [optional 32B HMAC][8B gameTokenId (C->S) / epochMillis (S->C)][1B seq]

namespace offsets {
constexpr std::size_t kType = 0;
constexpr std::size_t kAppId = 1;
constexpr std::size_t kChunkX = 9;
constexpr std::size_t kChunkY = 17;
constexpr std::size_t kChunkZ = 25;
constexpr std::size_t kDistance = 33;
constexpr std::size_t kDecay = 34;
constexpr std::size_t kContainsAuth = 35;
constexpr std::size_t kUuid = 36;
constexpr std::size_t kPayload = 68;
}  // namespace offsets

constexpr std::size_t kUuidSize = 32;
constexpr std::size_t kHmacTagSize = 32;
constexpr std::size_t kTokenOctets = 64;
constexpr std::size_t kLongSpatialHeaderSize = offsets::kPayload;          // 68
constexpr std::size_t kTailNoHmac = 8 + 1;                                 // 9
constexpr std::size_t kTailWithHmac = kHmacTagSize + kTailNoHmac;          // 41
constexpr std::size_t kMinLongSpatialNoHmac = kLongSpatialHeaderSize + kTailNoHmac;    // 77
constexpr std::size_t kMinLongSpatialWithHmac = kLongSpatialHeaderSize + kTailWithHmac;  // 109

/// The servers cap datagrams at the IPv6-safe UDP payload size; keep every
/// message (header + payload + tail) at or under this.
constexpr std::size_t kMaxDatagramSize = 1232;
/// Maximum payload for a signed long-spatial message that fits one datagram.
constexpr std::size_t kMaxLongSpatialPayload =
    kMaxDatagramSize - kLongSpatialHeaderSize - kTailWithHmac;  // 1123

/// Replication distance is clamped to Chebyshev rings 0-8 server-side.
constexpr std::uint8_t kMaxDistance = 8;

// ---- GenericError frame -----------------------------------------------------
// [1B type=3][1B seq][1B errorCode]
constexpr std::size_t kGenericErrorSize = 3;

// ---- CommandReconnect frame -------------------------------------------------
// [1B type=22][32B HMAC-SHA256 over the type byte, keyed on the game token]
constexpr std::size_t kCommandReconnectSize = 1 + kHmacTagSize;

// ---- Channel message layouts ------------------------------------------------
// Request (client -> server):
//   [1B type=17][8B channelId][32B uuid][2B payloadLen][payload]
//   [1B containsAuth=1][32B HMAC][8B gameTokenId][1B seq]
// Notification (server -> client):
//   [1B type=18][8B channelId][32B senderUuid][2B payloadLen][payload]
//   [8B epochMillis][1B seq]
namespace channel {
constexpr std::size_t kHeaderSize = 1 + 8 + kUuidSize + 2;  // 43 (through payloadLen)
constexpr std::size_t kChannelIdOffset = 1;
constexpr std::size_t kUuidOffset = 9;
constexpr std::size_t kPayloadLenOffset = 41;
constexpr std::size_t kPayloadOffset = kHeaderSize;
constexpr std::size_t kRequestTailSize = 1 + kHmacTagSize + 8 + 1;  // 42
constexpr std::size_t kNotificationTailSize = 8 + 1;                // 9
constexpr std::size_t kMinRequestSize = kHeaderSize + kRequestTailSize;            // 85
constexpr std::size_t kMinNotificationSize = kHeaderSize + kNotificationTailSize;  // 52
constexpr std::size_t kMaxPayload = 1024;
}  // namespace channel

// ---- Event payload ----------------------------------------------------------
// Client/server event messages carry [2B eventType][state...] in the payload
// region of the long spatial layout.
constexpr std::size_t kEventTypeSize = 2;

// ---- Voxel payload ----------------------------------------------------------
// [2B voxelX][2B voxelY][2B voxelZ][2B voxelType][2B stateLen][state...]
namespace voxel {
constexpr std::size_t kFixedSize = 10;
constexpr std::size_t kXOffset = 0;
constexpr std::size_t kYOffset = 2;
constexpr std::size_t kZOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kStateLenOffset = 8;
constexpr std::size_t kStateOffset = 10;
}  // namespace voxel

}  // namespace crowdy::wire
