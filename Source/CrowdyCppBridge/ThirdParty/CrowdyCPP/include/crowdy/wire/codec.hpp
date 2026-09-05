#pragma once

#include <cstring>
#include <optional>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/result.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/wire/protocol.hpp"

/// Zero-copy encoder/decoder for the public Replication API wire protocol
/// (https://docs.crowdedkingdoms.com/replication-api/wire-formats).
///
/// Encoding writes directly into a caller-provided buffer; decoding returns
/// views (spans/pointers) into the datagram buffer — nothing is copied and
/// nothing allocates. Views are only valid while the underlying buffer is.
namespace crowdy::wire {

struct ChunkCoord {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;
  friend bool operator==(const ChunkCoord&, const ChunkCoord&) = default;
};

// ---------------------------------------------------------------------------
// HMAC (https://docs.crowdedkingdoms.com/replication-api/hmac)
// tag = HMAC-SHA256(key = token64, message = prefix || token64)
// where prefix is every byte before the 32-byte HMAC field.
// ---------------------------------------------------------------------------

/// The 64-octet app-scoped token used as the HMAC key.
struct Token64 {
  std::uint8_t octets[kTokenOctets] = {};

  static std::optional<Token64> fromString(std::string_view s) {
    if (s.size() != kTokenOctets) return std::nullopt;
    Token64 t;
    std::memcpy(t.octets, s.data(), kTokenOctets);
    return t;
  }
  Bytes bytes() const { return Bytes(octets, kTokenOctets); }
};

/// Compute the spatial HMAC tag over `prefix`. `out` must hold 32 bytes.
///
/// Pass `mac` (from ICrypto::makeHmacSha256 for this token) to sign with a
/// pre-keyed context, which is several times cheaper than the one-shot path and
/// also feeds the two parts straight in rather than concatenating them into a
/// stack buffer. Without it the behaviour is unchanged: same key, same message,
/// same tag.
inline bool spatialHmac(const core::ICrypto& crypto, Bytes prefix, const Token64& token,
                        std::uint8_t* out, const core::IMac* mac = nullptr) {
  if (prefix.size() > kMaxDatagramSize) return false;
  if (mac != nullptr) {
    const Bytes parts[] = {prefix, token.bytes()};
    return mac->compute(parts, 2, out);
  }
  std::uint8_t msg[kMaxDatagramSize + kTokenOctets];
  std::memcpy(msg, prefix.data(), prefix.size());
  std::memcpy(msg + prefix.size(), token.octets, kTokenOctets);
  return crypto.hmacSha256(token.bytes(), Bytes(msg, prefix.size() + kTokenOctets), out);
}

// ---------------------------------------------------------------------------
// Long form spatial messages
// ---------------------------------------------------------------------------

struct LongSpatialParams {
  MessageType type = MessageType::GenericSpatial1;
  std::int64_t appId = 0;
  ChunkCoord chunk;
  std::uint8_t distance = 0;
  DecayRate decay = DecayRate::None;
  core::ActorUuid uuid{};
  Bytes payload;
  std::int64_t gameTokenId = 0;
  std::uint8_t sequence = 0;
};

/// Total encoded size for a signed long-spatial message with this payload.
inline constexpr std::size_t longSpatialSize(std::size_t payloadLen, bool withHmac = true) {
  return kLongSpatialHeaderSize + payloadLen + (withHmac ? kTailWithHmac : kTailNoHmac);
}

/// Encode and sign a long-form spatial message into `out`.
/// Returns the number of bytes written, or an error.
inline Result<std::size_t> encodeLongSpatial(const core::ICrypto& crypto,
                                             const LongSpatialParams& p, const Token64& token,
                                             MutableBytes out, const core::IMac* mac = nullptr) {
  if (!isLongSpatialLayout(static_cast<std::uint8_t>(p.type))) return Errc::InvalidArgument;
  if (p.payload.size() > kMaxLongSpatialPayload) return Errc::InvalidArgument;
  const std::size_t total = longSpatialSize(p.payload.size());
  if (out.size() < total) return Errc::BufferTooSmall;
  const Status cryptoStatus = crypto.availability();
  if (!cryptoStatus.ok()) return cryptoStatus.code;

  std::uint8_t* b = out.data();
  b[offsets::kType] = static_cast<std::uint8_t>(p.type);
  le::writeI64(b + offsets::kAppId, p.appId);
  le::writeI64(b + offsets::kChunkX, p.chunk.x);
  le::writeI64(b + offsets::kChunkY, p.chunk.y);
  le::writeI64(b + offsets::kChunkZ, p.chunk.z);
  b[offsets::kDistance] = p.distance;
  b[offsets::kDecay] = static_cast<std::uint8_t>(p.decay);
  b[offsets::kContainsAuth] = 1;
  std::memcpy(b + offsets::kUuid, p.uuid.data(), kUuidSize);
  if (!p.payload.empty()) std::memcpy(b + offsets::kPayload, p.payload.data(), p.payload.size());

  const std::size_t prefixLen = kLongSpatialHeaderSize + p.payload.size();
  if (!spatialHmac(crypto, Bytes(b, prefixLen), token, b + prefixLen, mac)) return Errc::Malformed;
  le::writeI64(b + prefixLen + kHmacTagSize, p.gameTokenId);
  b[total - 1] = p.sequence;
  return total;
}

/// Parsed view over an inbound long-spatial notification. Payload points into
/// the original datagram buffer.
struct LongSpatialView {
  MessageType type;
  std::int64_t appId;
  ChunkCoord chunk;
  std::uint8_t distance;
  DecayRate decay;
  bool containsAuth;
  const char* uuid;        // 32 bytes, not null-terminated
  Bytes payload;
  std::int64_t epochMillisOrTokenId;  // S->C: epoch millis; C->S: gameTokenId
  std::uint8_t sequence;

  core::ActorUuid uuidArray() const {
    core::ActorUuid u;
    std::memcpy(u.data(), uuid, kUuidSize);
    return u;
  }
};

/// Parse a long-spatial message (no HMAC verification — see
/// verifyLongSpatial). Returns Malformed for short/invalid frames.
inline Result<LongSpatialView> parseLongSpatial(Bytes datagram) {
  if (datagram.size() < kMinLongSpatialNoHmac) return Errc::Malformed;
  const std::uint8_t* b = datagram.data();
  if (!isLongSpatialLayout(b[offsets::kType])) return Errc::Malformed;

  const bool auth = b[offsets::kContainsAuth] != 0;
  const std::size_t tail = auth ? kTailWithHmac : kTailNoHmac;
  if (datagram.size() < kLongSpatialHeaderSize + tail) return Errc::Malformed;

  LongSpatialView v;
  v.type = static_cast<MessageType>(b[offsets::kType]);
  v.appId = le::readI64(b + offsets::kAppId);
  v.chunk = {le::readI64(b + offsets::kChunkX), le::readI64(b + offsets::kChunkY),
             le::readI64(b + offsets::kChunkZ)};
  v.distance = b[offsets::kDistance];
  v.decay = static_cast<DecayRate>(b[offsets::kDecay]);
  v.containsAuth = auth;
  v.uuid = reinterpret_cast<const char*>(b + offsets::kUuid);
  v.payload = datagram.subspan(offsets::kPayload, datagram.size() - kLongSpatialHeaderSize - tail);
  v.epochMillisOrTokenId = le::readI64(b + datagram.size() - kTailNoHmac);
  v.sequence = b[datagram.size() - 1];
  return v;
}

/// Verify the server HMAC on a signed (containsAuth = 1) long-spatial
/// notification, keyed on this client's token. Unsigned messages verify
/// trivially (nothing to check).
inline Status verifyLongSpatial(const core::ICrypto& crypto, Bytes datagram,
                                const Token64& token, const core::IMac* mac = nullptr) {
  if (datagram.size() < kMinLongSpatialNoHmac) return Errc::Malformed;
  if (datagram[offsets::kContainsAuth] == 0) return Errc::Ok;
  const Status cryptoStatus = crypto.availability();
  if (!cryptoStatus.ok()) return cryptoStatus;
  if (datagram.size() < kMinLongSpatialWithHmac) return Errc::Malformed;
  const std::size_t prefixLen = datagram.size() - kTailWithHmac;
  std::uint8_t expected[kHmacTagSize];
  if (!spatialHmac(crypto, datagram.first(prefixLen), token, expected, mac))
    return Errc::Malformed;
  if (!crypto.constantTimeEquals(expected, datagram.data() + prefixLen, kHmacTagSize))
    return Errc::HmacMismatch;
  return Errc::Ok;
}

// ---------------------------------------------------------------------------
// Voxel payload ([2B x][2B y][2B z][2B type][2B stateLen][state...])
// ---------------------------------------------------------------------------

struct VoxelPayloadView {
  std::int16_t x, y, z;
  std::int16_t voxelType;
  Bytes state;
};

inline Result<VoxelPayloadView> parseVoxelPayload(Bytes payload) {
  if (payload.size() < voxel::kFixedSize) return Errc::Malformed;
  const std::uint8_t* b = payload.data();
  VoxelPayloadView v;
  v.x = le::readI16(b + voxel::kXOffset);
  v.y = le::readI16(b + voxel::kYOffset);
  v.z = le::readI16(b + voxel::kZOffset);
  v.voxelType = le::readI16(b + voxel::kTypeOffset);
  const std::uint16_t stateLen = le::readU16(b + voxel::kStateLenOffset);
  if (payload.size() < voxel::kFixedSize + stateLen) return Errc::Malformed;
  v.state = payload.subspan(voxel::kStateOffset, stateLen);
  return v;
}

/// Encode a voxel payload into `out`; returns bytes written.
inline Result<std::size_t> encodeVoxelPayload(std::int16_t x, std::int16_t y, std::int16_t z,
                                              std::int16_t voxelType, Bytes state,
                                              MutableBytes out) {
  const std::size_t total = voxel::kFixedSize + state.size();
  if (out.size() < total) return Errc::BufferTooSmall;
  if (state.size() > 0xffff) return Errc::InvalidArgument;
  std::uint8_t* b = out.data();
  le::writeI16(b + voxel::kXOffset, x);
  le::writeI16(b + voxel::kYOffset, y);
  le::writeI16(b + voxel::kZOffset, z);
  le::writeI16(b + voxel::kTypeOffset, voxelType);
  le::writeU16(b + voxel::kStateLenOffset, static_cast<std::uint16_t>(state.size()));
  if (!state.empty()) std::memcpy(b + voxel::kStateOffset, state.data(), state.size());
  return total;
}

// ---------------------------------------------------------------------------
// Event payload ([2B eventType][state...])
// ---------------------------------------------------------------------------

struct EventPayloadView {
  std::uint16_t eventType;
  Bytes state;
};

inline Result<EventPayloadView> parseEventPayload(Bytes payload) {
  if (payload.size() < kEventTypeSize) return Errc::Malformed;
  return EventPayloadView{le::readU16(payload.data()), payload.subspan(kEventTypeSize)};
}

inline Result<std::size_t> encodeEventPayload(std::uint16_t eventType, Bytes state,
                                              MutableBytes out) {
  const std::size_t total = kEventTypeSize + state.size();
  if (out.size() < total) return Errc::BufferTooSmall;
  le::writeU16(out.data(), eventType);
  if (!state.empty()) std::memcpy(out.data() + kEventTypeSize, state.data(), state.size());
  return total;
}

// ---------------------------------------------------------------------------
// Channel messages
// ---------------------------------------------------------------------------

struct ChannelMessageParams {
  std::int64_t channelId = 0;
  core::ActorUuid uuid{};
  Bytes payload;
  std::int64_t gameTokenId = 0;
  std::uint8_t sequence = 0;
};

inline constexpr std::size_t channelRequestSize(std::size_t payloadLen) {
  return channel::kHeaderSize + payloadLen + channel::kRequestTailSize;
}

/// Encode and sign a CHANNEL_MESSAGE_REQUEST. The HMAC signs all bytes up to
/// and including containsAuth, concatenated with the token octets.
inline Result<std::size_t> encodeChannelMessage(const core::ICrypto& crypto,
                                                const ChannelMessageParams& p,
                                                const Token64& token, MutableBytes out,
                                                const core::IMac* mac = nullptr) {
  if (p.payload.size() > channel::kMaxPayload) return Errc::InvalidArgument;
  const std::size_t total = channelRequestSize(p.payload.size());
  if (out.size() < total) return Errc::BufferTooSmall;
  const Status cryptoStatus = crypto.availability();
  if (!cryptoStatus.ok()) return cryptoStatus.code;

  std::uint8_t* b = out.data();
  b[0] = static_cast<std::uint8_t>(MessageType::ChannelMessageRequest);
  le::writeI64(b + channel::kChannelIdOffset, p.channelId);
  std::memcpy(b + channel::kUuidOffset, p.uuid.data(), kUuidSize);
  le::writeU16(b + channel::kPayloadLenOffset, static_cast<std::uint16_t>(p.payload.size()));
  if (!p.payload.empty())
    std::memcpy(b + channel::kPayloadOffset, p.payload.data(), p.payload.size());

  const std::size_t authOffset = channel::kPayloadOffset + p.payload.size();
  b[authOffset] = 1;  // containsAuth
  const std::size_t prefixLen = authOffset + 1;
  if (!spatialHmac(crypto, Bytes(b, prefixLen), token, b + prefixLen, mac)) return Errc::Malformed;
  le::writeI64(b + prefixLen + kHmacTagSize, p.gameTokenId);
  b[total - 1] = p.sequence;
  return total;
}

struct ChannelNotificationView {
  std::int64_t channelId;
  const char* senderUuid;  // 32 bytes
  Bytes payload;
  std::int64_t epochMillis;
  std::uint8_t sequence;

  core::ActorUuid uuidArray() const {
    core::ActorUuid u;
    std::memcpy(u.data(), senderUuid, kUuidSize);
    return u;
  }
};

inline Result<ChannelNotificationView> parseChannelNotification(Bytes datagram) {
  if (datagram.size() < channel::kMinNotificationSize) return Errc::Malformed;
  const std::uint8_t* b = datagram.data();
  if (b[0] != static_cast<std::uint8_t>(MessageType::ChannelMessageNotification))
    return Errc::Malformed;
  const std::uint16_t payloadLen = le::readU16(b + channel::kPayloadLenOffset);
  if (datagram.size() <
      channel::kHeaderSize + payloadLen + channel::kNotificationTailSize)
    return Errc::Malformed;
  ChannelNotificationView v;
  v.channelId = le::readI64(b + channel::kChannelIdOffset);
  v.senderUuid = reinterpret_cast<const char*>(b + channel::kUuidOffset);
  v.payload = datagram.subspan(channel::kPayloadOffset, payloadLen);
  const std::size_t tailStart = channel::kPayloadOffset + payloadLen;
  v.epochMillis = le::readI64(b + tailStart);
  v.sequence = b[tailStart + 8];
  return v;
}

// ---------------------------------------------------------------------------
// Error + reconnect frames
// ---------------------------------------------------------------------------

struct GenericErrorView {
  std::uint8_t sequence;
  ErrorCode code;
};

inline Result<GenericErrorView> parseGenericError(Bytes datagram) {
  if (datagram.size() < kGenericErrorSize ||
      datagram[0] != static_cast<std::uint8_t>(MessageType::GenericError))
    return Errc::Malformed;
  return GenericErrorView{datagram[1], static_cast<ErrorCode>(datagram[2])};
}

/// Verify a COMMAND_RECONNECT frame: HMAC-SHA256 over the single type byte,
/// keyed on this client's game token. Unverified commands must be ignored.
inline Status verifyCommandReconnect(const core::ICrypto& crypto, Bytes datagram,
                                     const Token64& token) {
  if (datagram.size() != kCommandReconnectSize ||
      datagram[0] != static_cast<std::uint8_t>(MessageType::CommandReconnect))
    return Errc::Malformed;
  const Status cryptoStatus = crypto.availability();
  if (!cryptoStatus.ok()) return cryptoStatus;
  std::uint8_t typeByte = datagram[0];
  std::uint8_t msg[1 + kTokenOctets];
  msg[0] = typeByte;
  std::memcpy(msg + 1, token.octets, kTokenOctets);
  std::uint8_t expected[kHmacTagSize];
  if (!crypto.hmacSha256(token.bytes(), Bytes(msg, sizeof(msg)), expected))
    return Errc::Malformed;
  if (!crypto.constantTimeEquals(expected, datagram.data() + 1, kHmacTagSize))
    return Errc::HmacMismatch;
  return Errc::Ok;
}

// ---------------------------------------------------------------------------
// Bundle iteration
// ---------------------------------------------------------------------------

/// Invoke `fn(Bytes)` for each message in the datagram. A MESSAGE_BUNDLE
/// ([1B type=2]{[2B len LE][msg]}...) yields each member; any other datagram
/// yields itself once. Returns Malformed if a bundle is truncated (members
/// before the truncation are still delivered).
template <typename Fn>
inline Status forEachMessage(Bytes datagram, Fn&& fn) {
  if (datagram.empty()) return Errc::Malformed;
  if (datagram[0] != static_cast<std::uint8_t>(MessageType::MessageBundle)) {
    fn(datagram);
    return Errc::Ok;
  }
  std::size_t offset = 1;
  while (offset < datagram.size()) {
    if (offset + 2 > datagram.size()) return Errc::Malformed;
    const std::uint16_t msgLen = le::readU16(datagram.data() + offset);
    offset += 2;
    if (offset + msgLen > datagram.size()) return Errc::Malformed;
    fn(datagram.subspan(offset, msgLen));
    offset += msgLen;
  }
  return Errc::Ok;
}

}  // namespace crowdy::wire
