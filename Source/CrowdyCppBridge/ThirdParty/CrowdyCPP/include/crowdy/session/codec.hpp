#pragma once

#include <bit>
#include <cstring>
#include <type_traits>

#include "crowdy/core/bytes.hpp"

/// Actor-state codecs — the C++ analog of CrowdyJS's structCodec. A codec
/// maps a typed state struct to the compact little-endian byte payload
/// replicated to other players. Keep payloads small (the payload budget for a
/// signed spatial message is ~1.1 KB; a pose should be tens of bytes).
///
/// A Codec for T provides:
///   static constexpr std::size_t kSize;
///   static void encode(const T&, std::uint8_t* out);      // writes kSize bytes
///   static bool decode(Bytes in, T& out);                  // false on size mismatch
namespace crowdy::session {

/// Codec for trivially-copyable packed structs on little-endian hosts (every
/// supported target: x86, ARM). The struct layout IS the wire layout — use
/// fixed-width members and static_assert the size.
template <typename T>
struct PodCodec {
  static_assert(std::is_trivially_copyable_v<T>, "state must be trivially copyable");
  static_assert(std::endian::native == std::endian::little,
                "PodCodec requires a little-endian host; write a field codec instead");

  using State = T;
  static constexpr std::size_t kSize = sizeof(T);

  static void encode(const T& state, std::uint8_t* out) { std::memcpy(out, &state, sizeof(T)); }

  static bool decode(Bytes in, T& out) {
    if (in.size() != sizeof(T)) return false;
    std::memcpy(&out, in.data(), sizeof(T));
    return true;
  }
};

/// The 88-byte actor-state layout used by the Crowdy Unreal SDK and the
/// open-source cks-loadtest tool: version byte, position / rotation /
/// velocity as f64 triples, crouch flag, attachment enum.
struct UnrealPose {
  std::uint8_t version = 2;
  std::uint8_t pad_[7] = {};
  double positionX = 0, positionY = 0, positionZ = 0;
  double rotationPitch = 0, rotationYaw = 0, rotationRoll = 0;
  double velocityX = 0, velocityY = 0, velocityZ = 0;
  std::uint8_t crouch = 0;
  std::uint8_t attachments = 0;
  std::uint8_t tail_[6] = {};
};
static_assert(sizeof(UnrealPose) == 88, "UnrealPose must match the 88-byte wire layout");

using UnrealPoseCodec = PodCodec<UnrealPose>;

/// Raw pass-through codec for apps that manage their own bytes.
template <std::size_t N>
struct RawCodec {
  struct State {
    std::uint8_t bytes[N] = {};
  };
  static constexpr std::size_t kSize = N;
  static void encode(const State& s, std::uint8_t* out) { std::memcpy(out, s.bytes, N); }
  static bool decode(Bytes in, State& out) {
    if (in.size() != N) return false;
    std::memcpy(out.bytes, in.data(), N);
    return true;
  }
};

}  // namespace crowdy::session
