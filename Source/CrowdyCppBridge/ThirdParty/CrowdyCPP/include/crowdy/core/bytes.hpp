#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

/// Byte and little-endian helpers shared by every layer.
///
/// The Replication API wire protocol is little-endian throughout
/// (https://docs.crowdedkingdoms.com/replication-api/wire-formats). These
/// helpers are correct on any host endianness; on little-endian hosts they
/// compile to plain loads/stores.
namespace crowdy {

using Bytes = std::span<const std::uint8_t>;
using MutableBytes = std::span<std::uint8_t>;

namespace le {

inline void writeU16(std::uint8_t* dst, std::uint16_t v) {
  dst[0] = static_cast<std::uint8_t>(v & 0xff);
  dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

inline void writeU32(std::uint8_t* dst, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
}

inline void writeU64(std::uint8_t* dst, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
}

inline void writeI16(std::uint8_t* dst, std::int16_t v) {
  writeU16(dst, static_cast<std::uint16_t>(v));
}
inline void writeI64(std::uint8_t* dst, std::int64_t v) {
  writeU64(dst, static_cast<std::uint64_t>(v));
}

inline void writeF32(std::uint8_t* dst, float v) {
  std::uint32_t bits;
  std::memcpy(&bits, &v, 4);
  writeU32(dst, bits);
}

inline void writeF64(std::uint8_t* dst, double v) {
  std::uint64_t bits;
  std::memcpy(&bits, &v, 8);
  writeU64(dst, bits);
}

inline std::uint16_t readU16(const std::uint8_t* src) {
  return static_cast<std::uint16_t>(src[0] | (src[1] << 8));
}

inline std::uint32_t readU32(const std::uint8_t* src) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | src[i];
  return v;
}

inline std::uint64_t readU64(const std::uint8_t* src) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | src[i];
  return v;
}

inline std::int16_t readI16(const std::uint8_t* src) {
  return static_cast<std::int16_t>(readU16(src));
}
inline std::int64_t readI64(const std::uint8_t* src) {
  return static_cast<std::int64_t>(readU64(src));
}

inline float readF32(const std::uint8_t* src) {
  std::uint32_t bits = readU32(src);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

inline double readF64(const std::uint8_t* src) {
  std::uint64_t bits = readU64(src);
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

}  // namespace le

inline Bytes asBytes(std::string_view s) {
  return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

inline std::string_view asStringView(Bytes b) {
  return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

}  // namespace crowdy
