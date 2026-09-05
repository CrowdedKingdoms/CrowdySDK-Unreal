#pragma once

#include <cmath>
#include <cstdint>

#include "crowdy/wire/codec.hpp"

/// Chunk/voxel coordinate math shared by the session layer — the helpers
/// every voxel game re-derives. A chunk is a 16x16x16 voxel cube addressed by
/// signed-int64 chunk coordinates.
namespace crowdy::session {

constexpr int kChunkSize = 16;
constexpr int kChunkVolume = kChunkSize * kChunkSize * kChunkSize;  // 4096

using wire::ChunkCoord;

/// Index into a dense 4096-byte voxel grid (x + y*16 + z*256).
inline constexpr int voxelIndex(int x, int y, int z) {
  return x + y * kChunkSize + z * kChunkSize * kChunkSize;
}

/// The chunk containing a world-space position.
inline ChunkCoord worldToChunk(double x, double y, double z) {
  return {static_cast<std::int64_t>(std::floor(x / kChunkSize)),
          static_cast<std::int64_t>(std::floor(y / kChunkSize)),
          static_cast<std::int64_t>(std::floor(z / kChunkSize))};
}

/// The within-chunk voxel coordinate (0-15 per axis) of a world position.
inline int worldToLocalVoxel(double v) {
  const int m = static_cast<int>(std::floor(v)) % kChunkSize;
  return m < 0 ? m + kChunkSize : m;
}

/// Chebyshev distance between chunk coordinates (the replication metric).
inline std::int64_t chebyshev(const ChunkCoord& a, const ChunkCoord& b) {
  const std::int64_t dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  const std::int64_t dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  const std::int64_t dz = a.z > b.z ? a.z - b.z : b.z - a.z;
  return dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
}

/// Hash/equality for chunk-keyed maps.
struct ChunkCoordHash {
  std::size_t operator()(const ChunkCoord& c) const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::int64_t v) {
      h ^= static_cast<std::uint64_t>(v);
      h *= 1099511628211ull;
    };
    mix(c.x);
    mix(c.y);
    mix(c.z);
    return static_cast<std::size_t>(h);
  }
};

/// Hash for actor-uuid-keyed maps.
struct ActorUuidHash {
  std::size_t operator()(const core::ActorUuid& u) const {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : u) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};

}  // namespace crowdy::session
