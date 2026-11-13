#include "crowdy/session/chunk_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "crowdy/core/base64.hpp"
#include "crowdy/domains/world_data.hpp"

namespace crowdy::session {

namespace {

/// A shared grid of one type, for ChunkData::grid() of a uniform chunk: made once per
/// type and kept for the process.
const std::uint8_t* uniformGrid(std::uint8_t type) {
  static std::array<std::atomic<const std::uint8_t*>, 256> grids{};
  const std::uint8_t* grid = grids[type].load(std::memory_order_acquire);
  if (grid) return grid;
  auto* made = new std::uint8_t[kChunkVolume];
  std::memset(made, type, kChunkVolume);
  const std::uint8_t* expected = nullptr;
  if (!grids[type].compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
    delete[] made;
    return expected;
  }
  return made;
}

/// Epoch milliseconds as an ISO-8601 UTC time ("2026-11-11T20:00:00.000Z").
std::string isoUtc(std::int64_t epochMs) {
  const std::int64_t ms = ((epochMs % 1000) + 1000) % 1000;
  std::int64_t secs = (epochMs - ms) / 1000;
  std::int64_t days = secs / 86400;
  std::int64_t rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  // Howard Hinnant's civil_from_days.
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char out[64];
  std::snprintf(out, sizeof(out), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d),
                static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                static_cast<long long>(rem % 60), static_cast<long long>(ms));
  return out;
}

std::int64_t wallNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t volumeOf(const ChunkCoord& lo, const ChunkCoord& hi) {
  return (hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
}

/// The widest box one read covers along an axis: ck-api reads at most 17^3 chunks.
constexpr std::int64_t kBoxSide = 17;

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

/// Boxes covering `coords`, each at most twice as large as the chunks it holds, found by
/// cutting where the two halves' boxes are smallest: a step uncovers one slab per axis
/// moved, so it is one box per slab. A set too scattered for 8 boxes is read as its
/// bounding box (an area is at most 17^3). Mirrors CrowdyJS's coverWithBoxes.
std::vector<std::pair<ChunkCoord, ChunkCoord>> coverWithBoxes(const std::vector<ChunkCoord>& coords) {
  constexpr std::size_t kMaxBoxes = 8;
  constexpr std::int64_t kMaxBox = kBoxSide * kBoxSide * kBoxSide;
  using BoxPair = std::pair<ChunkCoord, ChunkCoord>;
  auto bounds = [](const std::vector<ChunkCoord>& set) {
    ChunkCoord lo = set.front(), hi = set.front();
    for (const ChunkCoord& c : set) {
      lo.x = std::min(lo.x, c.x);
      lo.y = std::min(lo.y, c.y);
      lo.z = std::min(lo.z, c.z);
      hi.x = std::max(hi.x, c.x);
      hi.y = std::max(hi.y, c.y);
      hi.z = std::max(hi.z, c.z);
    }
    return BoxPair{lo, hi};
  };
  std::vector<BoxPair> boxes;
  std::function<bool(const std::vector<ChunkCoord>&)> split =
      [&](const std::vector<ChunkCoord>& set) -> bool {
    if (set.empty()) return true;
    const BoxPair box = bounds(set);
    const std::int64_t volume = volumeOf(box.first, box.second);
    if (volume <= kMaxBox && volume <= static_cast<std::int64_t>(set.size()) * 2) {
      boxes.push_back(box);
      return boxes.size() <= kMaxBoxes;
    }
    int bestAxis = -1;
    std::int64_t bestCut = 0, bestCost = 0;
    auto axisOf = [](const ChunkCoord& c, int axis) { return axis == 0 ? c.x : axis == 1 ? c.y : c.z; };
    for (int axis = 0; axis < 3; ++axis) {
      for (std::int64_t cut = axisOf(box.first, axis); cut < axisOf(box.second, axis); ++cut) {
        std::vector<ChunkCoord> low, high;
        for (const ChunkCoord& c : set) (axisOf(c, axis) <= cut ? low : high).push_back(c);
        if (low.empty() || high.empty()) continue;
        const BoxPair a = bounds(low), b = bounds(high);
        const std::int64_t cost = volumeOf(a.first, a.second) + volumeOf(b.first, b.second);
        if (bestAxis < 0 || cost < bestCost) {
          bestAxis = axis;
          bestCut = cut;
          bestCost = cost;
        }
      }
    }
    if (bestAxis < 0) {
      boxes.push_back(box);
      return boxes.size() <= kMaxBoxes;
    }
    std::vector<ChunkCoord> low, high;
    for (const ChunkCoord& c : set) (axisOf(c, bestAxis) <= bestCut ? low : high).push_back(c);
    return split(low) && split(high);
  };
  if (split(coords)) return boxes;
  return {bounds(coords)};
}

/// `coords` read in boxes of at most 17^3: the bounds of the coords in each cell of a
/// 17-chunk lattice.
std::vector<std::pair<ChunkCoord, ChunkCoord>> tileBoxes(const std::vector<ChunkCoord>& coords) {
  std::unordered_map<ChunkCoord, std::pair<ChunkCoord, ChunkCoord>, ChunkCoordHash> tiles;
  for (const ChunkCoord& c : coords) {
    const ChunkCoord tile{floorDiv(c.x, kBoxSide), floorDiv(c.y, kBoxSide), floorDiv(c.z, kBoxSide)};
    auto [it, fresh] = tiles.try_emplace(tile, c, c);
    if (fresh) continue;
    ChunkCoord& lo = it->second.first;
    ChunkCoord& hi = it->second.second;
    lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
    hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
  }
  std::vector<std::pair<ChunkCoord, ChunkCoord>> out;
  out.reserve(tiles.size());
  for (const auto& [tile, box] : tiles) out.push_back(box);
  return out;
}

std::int64_t isoMillis(const graphql::Json& value) {
  const std::string_view iso = value.asStringView();
  return iso.empty() ? 0 : core::parseIso8601Millis(iso.data(), iso.size());
}

StoredChunk storedChunkOf(const graphql::Json& chunkJson) {
  StoredChunk chunk;
  chunk.coord = {chunkJson["coordinates"]["x"].asBigInt(), chunkJson["coordinates"]["y"].asBigInt(),
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
    voxel.updatedAtMs = isoMillis(entry["updatedAt"]);
    chunk.voxelStates.push_back(std::move(voxel));
  });
  chunk.voxelStatesTruncated = chunkJson["voxelStatesTruncated"].asBool();
  chunk.updatedAtMs = isoMillis(chunkJson["updatedAt"]);
  return chunk;
}

/// The page size the store asks for; the server may apply a smaller one.
constexpr std::int64_t kPage = 1000;

/// Where a page of a paged read leaves it: whether another follows, and its skip.
struct PageEnd {
  bool more = false;
  std::int64_t nextSkip = 0;
};

PageEnd pageEnd(const graphql::Json& page, std::int64_t rows, std::int64_t skip) {
  const std::int64_t applied = page["limit"].asInt64(kPage);
  if (rows == 0 || applied <= 0 || rows < applied) return {};
  return {true, page["skip"].asInt64(skip) + applied};
}

/// One page of getChunksByDistance: its chunks appended to `out`.
PageEnd chunkPage(const graphql::Json& page, std::int64_t skip, std::vector<StoredChunk>& out) {
  std::int64_t rows = 0;
  page["chunks"].forEach([&](graphql::Json chunkJson) {
    ++rows;
    out.push_back(storedChunkOf(chunkJson));
  });
  return pageEnd(page, rows, skip);
}

/// One page of listVoxelUpdatesByDistance (with createdAt): its groups appended to `out`.
PageEnd editsPage(const graphql::Json& page, std::int64_t skip, std::vector<StoredChunkEdits>& out) {
  std::int64_t rows = 0;
  page["chunks"].forEach([&](graphql::Json group) {
    ++rows;
    StoredChunkEdits edits;
    edits.coord = {group["coordinates"]["x"].asBigInt(), group["coordinates"]["y"].asBigInt(),
                   group["coordinates"]["z"].asBigInt()};
    group["voxels"].forEach([&](graphql::Json v) {
      StoredVoxelState edit;
      edit.x = static_cast<int>(v["location"]["x"].asInt64(-1));
      edit.y = static_cast<int>(v["location"]["y"].asInt64(-1));
      edit.z = static_cast<int>(v["location"]["z"].asInt64(-1));
      edit.voxelType = static_cast<std::int16_t>(v["voxelType"].asInt64());
      if (auto state = core::base64Decode(v["state"].asStringView())) {
        edit.state.assign(state->begin(), state->end());
      }
      edit.updatedAtMs = isoMillis(v["createdAt"]);
      edits.edits.push_back(std::move(edit));
    });
    out.push_back(std::move(edits));
  });
  return pageEnd(page, rows, skip);
}

domains::ChunkRegion boxRegion(const ChunkCoord& low, const ChunkCoord& high, std::int64_t skip,
                               const std::string& sinceIso) {
  domains::ChunkRegion area = domains::ChunkRegion::box(domains::ChunkRef{low.x, low.y, low.z},
                                                        domains::ChunkRef{high.x, high.y, high.z});
  area.limit(kPage).skip(skip);
  if (!sinceIso.empty()) area.since(sinceIso);
  return area;
}

/// The value of a response's one root field (`data.getChunk`, ...).
graphql::Json rootField(const graphql::Json& data) {
  graphql::Json single;
  if (data.isObject() && data.size() == 1) {
    data.forEachMember([&](std::string_view, graphql::Json v) { single = v; });
  }
  return single;
}

/// IChunkSource over the Game API's chunks (and voxels) surface.
class GameApiChunkSource final : public IChunkSource {
 public:
  GameApiChunkSource(domains::ChunksAPI& chunks, domains::VoxelsAPI* voxels)
      : chunks_(chunks), voxels_(voxels) {}

  std::vector<StoredChunk> chunksAround(const std::string& appId, const ChunkCoord& center,
                                        int distance) override {
    const ChunkCoord lo{center.x - distance, center.y - distance, center.z - distance};
    const ChunkCoord hi{center.x + distance, center.y + distance, center.z + distance};
    return chunksInBox(appId, lo, hi, {});
  }

  /// One box, page by page by the limit the server applied; a chunk two pages return
  /// comes back twice, for the store to count and keep once.
  std::vector<StoredChunk> chunksInBox(const std::string& appId, const ChunkCoord& low,
                                       const ChunkCoord& high, const std::string& sinceIso) override {
    std::vector<StoredChunk> out;
    for (PageEnd at{true, 0}; at.more;) {
      at = chunkPage(chunks_.region(appId, boxRegion(low, high, at.nextSkip, sinceIso)),
                     at.nextSkip, out);
    }
    return out;
  }

  std::vector<StoredChunkEdits> editsInBox(const std::string& appId, const ChunkCoord& low,
                                           const ChunkCoord& high,
                                           const std::string& sinceIso) override {
    std::vector<StoredChunkEdits> out;
    if (!voxels_) return out;
    for (PageEnd at{true, 0}; at.more;) {
      at = editsPage(
          voxels_->listInRegionCellsTimed(appId, boxRegion(low, high, at.nextSkip, sinceIso)),
          at.nextSkip, out);
    }
    return out;
  }

  std::optional<StoredChunk> chunkAt(const std::string& appId, const ChunkCoord& coord) override {
    graphql::Json chunk = chunks_.get(appId, domains::ChunkRef{coord.x, coord.y, coord.z});
    if (!chunk.isObject()) return std::nullopt;
    return storedChunkOf(chunk);
  }

  graphql::GraphQLOutcome writeChunk(const std::string& appId, const ChunkCoord& coord,
                                     Bytes voxels) override {
    graphql::JVal input;
    input["appId"] = appId;
    input["coordinates"] = domains::ChunkRef{coord.x, coord.y, coord.z}.toInput();
    input["voxels"] = core::base64Encode(voxels);
    return chunks_.updateOutcome(input);
  }

 private:
  domains::ChunksAPI& chunks_;
  domains::VoxelsAPI* voxels_;
};

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
    case graphql::GraphQLErrorKind::Cancelled:
      // Whoever cancelled it decided; sending it again would overrule them.
      return false;
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

bool sameBytes(Bytes a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

}  // namespace

std::unique_ptr<IChunkSource> makeGameApiChunkSource(domains::ChunksAPI& chunks,
                                                     domains::VoxelsAPI* voxels) {
  return std::make_unique<GameApiChunkSource>(chunks, voxels);
}

std::vector<StoredChunk> IChunkSource::chunksInBox(const std::string& appId, const ChunkCoord& low,
                                                   const ChunkCoord& high, const std::string&) {
  const ChunkCoord center{low.x + (high.x - low.x) / 2, low.y + (high.y - low.y) / 2,
                          low.z + (high.z - low.z) / 2};
  const std::int64_t radius = std::max({high.x - center.x, center.x - low.x, high.y - center.y,
                                        center.y - low.y, high.z - center.z, center.z - low.z});
  std::vector<StoredChunk> out;
  for (StoredChunk& chunk : chunksAround(appId, center, static_cast<int>(radius))) {
    const ChunkCoord& c = chunk.coord;
    if (c.x >= low.x && c.x <= high.x && c.y >= low.y && c.y <= high.y && c.z >= low.z &&
        c.z <= high.z) {
      out.push_back(std::move(chunk));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// ChunkData
// ---------------------------------------------------------------------------

const std::uint8_t* ChunkData::grid() const {
  return grid_.empty() ? uniformGrid(uniform_) : grid_.data();
}

std::optional<VoxelStateView> ChunkData::stateAt(int index) const {
  auto it = std::lower_bound(states_.begin(), states_.end(), index,
                             [](const StateRef& s, int i) { return s.index < i; });
  if (it == states_.end() || it->index != index) return std::nullopt;
  return VoxelStateView{it->voxelType, bytesOf(it->offset, it->length)};
}

std::optional<OverlayVoxel> ChunkData::overlayAt(int x, int y, int z) const {
  if (overlay_.empty() || x < -32768 || x > 32767 || y < -32768 || y > 32767 || z < -32768 ||
      z > 32767) {
    return std::nullopt;
  }
  const std::uint64_t key = voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                     static_cast<std::int16_t>(z));
  auto it = std::lower_bound(overlay_.begin(), overlay_.end(), key,
                             [](const WideRef& w, std::uint64_t k) { return w.key < k; });
  if (it == overlay_.end() || it->key != key) return std::nullopt;
  return OverlayVoxel{it->x, it->y, it->z, it->voxelType, bytesOf(it->offset, it->length)};
}

void ChunkData::reset() {
  coord = {};
  storedOnServer = false;
  dirty = false;
  hydratedAtMs = 0;
  voxelStatesTruncated = false;
  storedAtMs = 0;
  std::vector<std::uint8_t>().swap(grid_);
  uniform_ = 0;
  states_.clear();
  overlay_.clear();
  bytes_.clear();
  times_.clear();
  arrivals_.clear();
  garbage_ = 0;
  changes_ = 0;
  ++version_;
}

void ChunkData::setType(int index, std::uint8_t type) {
  if (grid_.empty()) {
    if (type == uniform_) return;
    grid_.assign(kChunkVolume, uniform_);
  }
  grid_[static_cast<std::size_t>(index)] = type;
}

void ChunkData::setGrid(const std::uint8_t* grid) {
  const std::uint8_t first = grid[0];
  if (std::all_of(grid, grid + kChunkVolume, [first](std::uint8_t v) { return v == first; })) {
    setUniform(first);
    return;
  }
  grid_.assign(grid, grid + kChunkVolume);
}

void ChunkData::setUniform(std::uint8_t type) {
  std::vector<std::uint8_t>().swap(grid_);
  uniform_ = type;
}

std::uint32_t ChunkData::append(Bytes state) {
  const auto offset = static_cast<std::uint32_t>(bytes_.size());
  bytes_.insert(bytes_.end(), state.begin(), state.end());
  return offset;
}

void ChunkData::dropBytes(std::uint32_t length) {
  garbage_ += length;
  // Compact once most of the buffer is dead: copy the live states into a new one.
  if (garbage_ < 4096 || garbage_ * 2 < bytes_.size()) return;
  std::vector<std::uint8_t> live;
  live.reserve(bytes_.size() - garbage_);
  for (StateRef& s : states_) {
    const auto offset = static_cast<std::uint32_t>(live.size());
    live.insert(live.end(), bytes_.begin() + s.offset, bytes_.begin() + s.offset + s.length);
    s.offset = offset;
  }
  for (WideRef& w : overlay_) {
    const auto offset = static_cast<std::uint32_t>(live.size());
    live.insert(live.end(), bytes_.begin() + w.offset, bytes_.begin() + w.offset + w.length);
    w.offset = offset;
  }
  bytes_.swap(live);
  garbage_ = 0;
}

void ChunkData::putState(int index, std::int16_t voxelType, Bytes state) {
  auto it = std::lower_bound(states_.begin(), states_.end(), index,
                             [](const StateRef& s, int i) { return s.index < i; });
  const bool found = it != states_.end() && it->index == index;
  if (state.empty()) {
    if (!found) return;
    const std::uint32_t length = it->length;
    states_.erase(it);
    dropBytes(length);
    return;
  }
  if (found) {
    it->voxelType = voxelType;
    if (state.size() <= it->length) {
      std::memcpy(bytes_.data() + it->offset, state.data(), state.size());
      const std::uint32_t freed = it->length - static_cast<std::uint32_t>(state.size());
      it->length = static_cast<std::uint32_t>(state.size());
      if (freed) dropBytes(freed);
      return;
    }
    const std::uint32_t old = it->length;
    const std::size_t at = static_cast<std::size_t>(it - states_.begin());
    const std::uint32_t offset = append(state);
    states_[at].offset = offset;
    states_[at].length = static_cast<std::uint32_t>(state.size());
    dropBytes(old);
    return;
  }
  const std::size_t at = static_cast<std::size_t>(it - states_.begin());
  const std::uint32_t offset = append(state);
  states_.insert(states_.begin() + static_cast<std::ptrdiff_t>(at),
                 StateRef{static_cast<std::uint16_t>(index), voxelType, offset,
                          static_cast<std::uint32_t>(state.size())});
}

void ChunkData::putWide(std::int16_t x, std::int16_t y, std::int16_t z, std::int16_t voxelType,
                        Bytes state) {
  const std::uint64_t key = voxelKey(x, y, z);
  auto it = std::lower_bound(overlay_.begin(), overlay_.end(), key,
                             [](const WideRef& w, std::uint64_t k) { return w.key < k; });
  if (it != overlay_.end() && it->key == key) {
    it->voxelType = voxelType;
    if (state.size() <= it->length) {
      if (!state.empty()) std::memcpy(bytes_.data() + it->offset, state.data(), state.size());
      const std::uint32_t freed = it->length - static_cast<std::uint32_t>(state.size());
      it->length = static_cast<std::uint32_t>(state.size());
      if (freed) dropBytes(freed);
      return;
    }
    const std::uint32_t old = it->length;
    const std::size_t at = static_cast<std::size_t>(it - overlay_.begin());
    const std::uint32_t offset = append(state);
    overlay_[at].offset = offset;
    overlay_[at].length = static_cast<std::uint32_t>(state.size());
    dropBytes(old);
    return;
  }
  const std::size_t at = static_cast<std::size_t>(it - overlay_.begin());
  const std::uint32_t offset = state.empty() ? 0 : append(state);
  overlay_.insert(overlay_.begin() + static_cast<std::ptrdiff_t>(at),
                  WideRef{key, x, y, z, voxelType, offset, static_cast<std::uint32_t>(state.size())});
}

bool ChunkData::eraseWide(std::uint64_t key) {
  auto it = std::lower_bound(overlay_.begin(), overlay_.end(), key,
                             [](const WideRef& w, std::uint64_t k) { return w.key < k; });
  if (it == overlay_.end() || it->key != key) return false;
  const std::uint32_t length = it->length;
  overlay_.erase(it);
  dropBytes(length);
  return true;
}

std::int64_t ChunkData::timeOf(std::uint64_t key) const {
  auto it = std::lower_bound(times_.begin(), times_.end(), key,
                             [](const CellTime& t, std::uint64_t k) { return t.key < k; });
  return it != times_.end() && it->key == key ? it->timeMs : storedAtMs;
}

std::uint64_t ChunkData::arrivalOf(std::uint64_t key) const {
  auto it = std::lower_bound(arrivals_.begin(), arrivals_.end(), key,
                             [](const CellArrival& a, std::uint64_t k) { return a.key < k; });
  return it != arrivals_.end() && it->key == key ? it->seq : 0;
}

void ChunkData::setTime(std::uint64_t key, std::int64_t timeMs, std::uint64_t arrival) {
  auto t = std::lower_bound(times_.begin(), times_.end(), key,
                            [](const CellTime& c, std::uint64_t k) { return c.key < k; });
  if (t != times_.end() && t->key == key) {
    t->timeMs = timeMs;
  } else if (timeMs > storedAtMs) {
    times_.insert(t, CellTime{key, timeMs});
  }
  auto a = std::lower_bound(arrivals_.begin(), arrivals_.end(), key,
                            [](const CellArrival& c, std::uint64_t k) { return c.key < k; });
  const bool found = a != arrivals_.end() && a->key == key;
  if (arrival == 0) {
    if (found) arrivals_.erase(a);
  } else if (found) {
    a->seq = arrival;
  } else {
    arrivals_.insert(a, CellArrival{key, arrival});
  }
}

// ---------------------------------------------------------------------------
// ChunkStore: construction, slots, reads
// ---------------------------------------------------------------------------

ChunkStore::ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi,
                       std::string appId, Options options)
    : conn_(&conn), appId_(std::move(appId)), options_(std::move(options)) {
  if (chunksApi) {
    ownedSource_ = makeGameApiChunkSource(*chunksApi);
    source_ = ownedSource_.get();
  }
  init();
}

ChunkStore::ChunkStore(replication::Connection& conn, IChunkSource* source, std::string appId,
                       Options options)
    : conn_(&conn), source_(source), appId_(std::move(appId)), options_(std::move(options)) {
  init();
}

ChunkStore::ChunkStore(replication::Connection& conn, IAsyncChunkSource* source,
                       std::string appId, Options options)
    : conn_(&conn), asyncSource_(source), appId_(std::move(appId)), options_(std::move(options)) {
  init();
}

ChunkStore::ChunkStore(SendFn send, IChunkSource* source, std::string appId, Options options)
    : source_(source), appId_(std::move(appId)), options_(std::move(options)) {
  options_.send = std::move(send);
  init();
}

ChunkStore::ChunkStore(SendFn send, IAsyncChunkSource* source, std::string appId, Options options)
    : asyncSource_(source), appId_(std::move(appId)), options_(std::move(options)) {
  options_.send = std::move(send);
  init();
}

void ChunkStore::init() {
  if (asyncSource_) {
    sink_ = std::make_shared<ReplySink>();
    sink_->store = this;
  }
  if (durable() && options_.ioThread) io_ = std::thread([this] { ioLoop(); });
}

ChunkStore::~ChunkStore() {
  if (sink_) {
    std::lock_guard lock(sink_->mutex);
    sink_->store = nullptr;
  }
  {
    std::lock_guard lock(ioMutex_);
    stopping_ = true;
  }
  ioWake_.notify_all();
  ioDone_.notify_all();
  if (io_.joinable()) io_.join();
}

ChunkData& ChunkStore::slotFor(const ChunkCoord& coord) {
  auto it = slotOf_.find(coord);
  if (it != slotOf_.end()) return slots_[it->second];
  std::uint32_t index;
  if (!freeSlots_.empty()) {
    index = freeSlots_.back();
    freeSlots_.pop_back();
  } else {
    index = static_cast<std::uint32_t>(slots_.size());
    slots_.emplace_back();
  }
  slotOf_.emplace(coord, index);
  ChunkData& c = slots_[index];
  c.reset();
  c.coord = coord;
  return c;
}

void ChunkStore::dropSlot(const ChunkCoord& coord) {
  auto it = slotOf_.find(coord);
  if (it == slotOf_.end()) return;
  const std::uint32_t index = it->second;
  ChunkData& c = slots_[index];
  if (c.dirty) setDirty(c, false);
  writeBackAttempts_.erase(coord);
  writeBackDueAt_.erase(coord);
  // A read on the way would bring it back.
  loading_.erase(coord);
  auto reload = reloading_.find(coord);
  if (reload != reloading_.end()) reload->second.evicted = true;
  slotOf_.erase(it);
  c.reset();
  freeSlots_.push_back(index);
  unloaded_.push_back(ChunkChange{coord, kChunkUnloaded});
  revision_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<const ChunkData*> ChunkStore::list() const {
  std::vector<const ChunkData*> out;
  out.reserve(slotOf_.size());
  for (const auto& [coord, index] : slotOf_) out.push_back(&slots_[index]);
  return out;
}

std::int16_t ChunkStore::voxelTypeAt(const ChunkCoord& coord, int x, int y, int z) const {
  const ChunkData* c = find(coord);
  if (!c) return 0;
  if (auto wide = c->overlayAt(x, y, z)) return wide->voxelType;
  if (!inGrid(x, y, z)) return 0;
  return c->typeAt(voxelIndex(x, y, z));
}

std::optional<VoxelStateView> ChunkStore::voxelStateAt(const ChunkCoord& coord, int x, int y,
                                                       int z) const {
  const ChunkData* c = find(coord);
  if (!c) return std::nullopt;
  if (auto wide = c->overlayAt(x, y, z)) {
    if (wide->state.empty()) return std::nullopt;
    return VoxelStateView{wide->voxelType, wide->state};
  }
  if (!inGrid(x, y, z)) return std::nullopt;
  return c->stateAt(voxelIndex(x, y, z));
}

std::int64_t ChunkStore::confirmedAtMs(const ChunkCoord& coord, int x, int y, int z) const {
  if (!fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return 0;
  const std::uint64_t key = voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                     static_cast<std::int16_t>(z));
  if (const PendingCell* cell = cellOf(coord, key)) return cell->baseTimeMs;
  const ChunkData* c = find(coord);
  return c ? c->timeOf(key) : 0;
}

// ---------------------------------------------------------------------------
// Writes to the cache, and change reports
// ---------------------------------------------------------------------------

void ChunkStore::putVoxel(ChunkData& c, int x, int y, int z, std::int16_t voxelType, Bytes state) {
  const auto sx = static_cast<std::int16_t>(x), sy = static_cast<std::int16_t>(y),
             sz = static_cast<std::int16_t>(z);
  if (fitsDenseGrid(x, y, z, voxelType)) {
    c.eraseWide(voxelKey(sx, sy, sz));
    const int index = voxelIndex(x, y, z);
    c.setType(index, static_cast<std::uint8_t>(voxelType));
    c.putState(index, voxelType, state);
    return;
  }
  if (inGrid(x, y, z)) {
    const int index = voxelIndex(x, y, z);
    c.setType(index, 0);
    c.putState(index, 0, Bytes());
  }
  c.putWide(sx, sy, sz, voxelType, state);
}

void ChunkStore::changed(ChunkData& c, std::uint32_t flags) {
  if (flags != 0) {
    if (c.changes_ == 0) changedSlots_.push_back(slotOf_.at(c.coord));
    c.changes_ |= flags;
  }
  ++c.version_;
  revision_.fetch_add(1, std::memory_order_relaxed);
  if (onChunkChanged_) onChunkChanged_(c);
}

void ChunkStore::borderNeighbours(const ChunkCoord& coord, int x, int y, int z) {
  auto flag = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz, std::uint32_t face) {
    ChunkData* n = slot({coord.x + dx, coord.y + dy, coord.z + dz});
    if (!n) return;
    if (n->changes_ == 0) changedSlots_.push_back(slotOf_.at(n->coord));
    n->changes_ |= kChunkNeighborEdge | face;
  };
  if (x == 0) flag(-1, 0, 0, kChunkFacePosX);
  if (x == kChunkSize - 1) flag(1, 0, 0, kChunkFaceNegX);
  if (y == 0) flag(0, -1, 0, kChunkFacePosY);
  if (y == kChunkSize - 1) flag(0, 1, 0, kChunkFaceNegY);
  if (z == 0) flag(0, 0, -1, kChunkFacePosZ);
  if (z == kChunkSize - 1) flag(0, 0, 1, kChunkFaceNegZ);
}

bool ChunkStore::show(ChunkData& c, int x, int y, int z, std::int16_t voxelType, Bytes state) {
  const std::int16_t before = voxelTypeAt(c.coord, x, y, z);
  const std::optional<VoxelStateView> had = voxelStateAt(c.coord, x, y, z);
  const bool sameState =
      had ? (had->state.size() == state.size() &&
             (state.empty() || std::memcmp(had->state.data(), state.data(), state.size()) == 0))
          : state.empty();
  if (before == voxelType && sameState) return false;
  putVoxel(c, x, y, z, voxelType, state);
  std::uint32_t flags = 0;
  if (before != voxelType) flags |= kChunkVoxels;
  if (!sameState || !fitsDenseGrid(x, y, z, voxelType)) flags |= kChunkStates;
  changed(c, flags);
  if (before != voxelType) borderNeighbours(c.coord, x, y, z);
  return true;
}

std::int64_t ChunkStore::clampTime(std::int64_t timeMs) const {
  if (timeMs <= 0) return 0;
  return std::min(timeMs, wallNowMs() + std::max<std::int64_t>(0, options_.maxFutureSkewMs));
}

bool ChunkStore::confirm(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
                         Bytes state, std::int64_t timeMs, bool create, std::uint64_t readSeq) {
  if (!fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return false;
  const std::uint64_t key = voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                     static_cast<std::int16_t>(z));
  const std::int64_t t = clampTime(timeMs);
  // Without a time, a value a read returned loses to one merged after that read was asked for.
  auto older = [&](std::int64_t heldMs, std::uint64_t heldArrival) {
    return t > 0 ? t < heldMs : (readSeq > 0 && heldArrival > readSeq);
  };
  if (PendingCell* cell = cellOf(coord, key)) {
    if (older(cell->baseTimeMs, cell->baseArrival)) return false;
    cell->baseType = voxelType;
    cell->baseState.assign(state.begin(), state.end());
    if (t > 0) cell->baseTimeMs = t;
    cell->baseArrival = ++mergeSeq_;
    if (create) slotFor(coord);
    return showPending(coord, key, *cell);
  }
  ChunkData* c = create ? &slotFor(coord) : slot(coord);
  if (!c) return false;
  const std::int64_t heldMs = c->timeOf(key);
  if (older(heldMs, c->arrivalOf(key))) return false;
  c->setTime(key, t > 0 ? t : heldMs, ++mergeSeq_);
  return show(*c, x, y, z, voxelType, state);
}

void ChunkStore::setDirty(ChunkData& chunk, bool dirty) {
  if (chunk.dirty == dirty) return;
  chunk.dirty = dirty;
  if (dirty) {
    dirty_.insert(chunk.coord);
    pendingWriteBacks_.fetch_add(1, std::memory_order_relaxed);
  } else {
    dirty_.erase(chunk.coord);
    pendingWriteBacks_.fetch_sub(1, std::memory_order_relaxed);
  }
}

void ChunkStore::applyStored(const StoredChunk& stored, std::int64_t nowMs,
                             std::uint64_t requestedSeq) {
  ++appliedCount_;
  ChunkData& c = slotFor(stored.coord);
  auto pendingChunk = pending_.find(stored.coord);
  PendingByVoxel* pending = pendingChunk == pending_.end() ? nullptr : &pendingChunk->second;
  // Every server time is held to Options::maxFutureSkewMs past this machine's clock.
  const std::int64_t rowMs = clampTime(stored.updatedAtMs);
  auto keyOf = [](const StoredVoxelState& e) {
    return voxelKey(static_cast<std::int16_t>(e.x), static_cast<std::int16_t>(e.y),
                    static_cast<std::int16_t>(e.z));
  };
  auto fits = [](const StoredVoxelState& e) {
    return fitsInt16(e.x) && fitsInt16(e.y) && fitsInt16(e.z);
  };

  // The copy's time for each voxel: its entry's, else the row's (0: the copy has none).
  std::vector<std::pair<std::uint64_t, std::int64_t>> entryTimes;
  const bool merging = !c.arrivals_.empty() || pending != nullptr;
  if (merging) {
    entryTimes.reserve(stored.voxelStates.size());
    for (const StoredVoxelState& e : stored.voxelStates) {
      if (fits(e)) {
        const std::int64_t entryMs = clampTime(e.updatedAtMs);
        entryTimes.emplace_back(keyOf(e), entryMs > 0 ? entryMs : rowMs);
      }
    }
    std::sort(entryTimes.begin(), entryTimes.end());
  }
  // A voxel the copy lists twice counts at its latest time.
  auto copyTime = [&](std::uint64_t key) {
    auto it = std::upper_bound(entryTimes.begin(), entryTimes.end(), key,
                               [](std::uint64_t k, const std::pair<std::uint64_t, std::int64_t>& e) {
                                 return k < e.first;
                               });
    if (it != entryTimes.begin() && std::prev(it)->first == key) return std::prev(it)->second;
    return rowMs;
  };
  // Whether the value held at a voxel is newer than the copy's.
  auto heldIsNewer = [&](std::uint64_t key, std::int64_t heldMs, std::uint64_t heldArrival) {
    const std::int64_t copyMs = copyTime(key);
    return copyMs > 0 ? heldMs > copyMs : heldArrival > requestedSeq;
  };

  // Values merged since the copy before that are newer than this copy stay.
  struct Kept {
    std::uint64_t key = 0;
    std::int64_t timeMs = 0;
    std::uint64_t arrival = 0;
    std::int16_t voxelType = 0;
    std::vector<std::uint8_t> state;
  };
  std::vector<Kept> kept;
  for (const ChunkData::CellArrival& a : c.arrivals_) {
    if (pending && pending->count(a.key)) continue;
    const std::int64_t heldMs = c.timeOf(a.key);
    if (!heldIsNewer(a.key, heldMs, a.seq)) continue;
    const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(a.key >> 32));
    const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(a.key >> 16));
    const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(a.key));
    Kept k;
    k.key = a.key;
    k.timeMs = heldMs;
    k.arrival = a.seq;
    k.voxelType = voxelTypeAt(c.coord, x, y, z);
    if (auto s = voxelStateAt(c.coord, x, y, z)) k.state.assign(s->state.begin(), s->state.end());
    kept.push_back(std::move(k));
  }

  c.storedOnServer = true;
  c.hydratedAtMs = nowMs > 0 ? nowMs : 1;
  c.voxelStatesTruncated = stored.voxelStatesTruncated;
  if (rowMs > 0) c.storedAtMs = rowMs;
  // The stored copy replaces the chunk's states and overlay (a world.set_chunk drops the
  // older edits); a copy without a grid keeps the one cached.
  c.states_.clear();
  c.overlay_.clear();
  c.bytes_.clear();
  c.garbage_ = 0;
  c.times_.clear();
  c.arrivals_.clear();
  if (stored.voxels.size() == static_cast<std::size_t>(kChunkVolume)) c.setGrid(stored.voxels.data());
  for (const StoredVoxelState& entry : stored.voxelStates) {
    if (!fits(entry)) continue;
    putVoxel(c, entry.x, entry.y, entry.z, entry.voxelType,
             Bytes(entry.state.data(), entry.state.size()));
    const std::int64_t entryMs = clampTime(entry.updatedAtMs);
    if (entryMs > c.storedAtMs) c.times_.push_back({keyOf(entry), entryMs});
  }
  std::sort(c.times_.begin(), c.times_.end(),
            [](const ChunkData::CellTime& a, const ChunkData::CellTime& b) {
              return a.key != b.key ? a.key < b.key : a.timeMs < b.timeMs;
            });
  // A voxel the copy lists twice keeps its latest time.
  std::size_t unique = 0;
  for (std::size_t i = 0; i < c.times_.size(); ++i) {
    if (i + 1 < c.times_.size() && c.times_[i + 1].key == c.times_[i].key) continue;
    c.times_[unique++] = c.times_[i];
  }
  c.times_.resize(unique);
  for (const Kept& k : kept) {
    const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(k.key >> 32));
    const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(k.key >> 16));
    const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(k.key));
    putVoxel(c, x, y, z, k.voxelType, Bytes(k.state.data(), k.state.size()));
    c.setTime(k.key, k.timeMs, k.arrival);
  }

  // This client's unanswered edits show over the copy; what the copy holds under them is
  // their confirmed value, unless the one held is newer.
  if (pending) {
    for (auto& [key, cell] : *pending) {
      const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 32));
      const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 16));
      const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(key));
      if (!heldIsNewer(key, cell.baseTimeMs, cell.baseArrival)) {
        cell.baseType = voxelTypeAt(c.coord, x, y, z);
        cell.baseState.clear();
        if (auto s = voxelStateAt(c.coord, x, y, z)) cell.baseState.assign(s->state.begin(), s->state.end());
        if (const std::int64_t copyMs = copyTime(key); copyMs > 0) cell.baseTimeMs = copyMs;
        cell.baseArrival = 0;
      }
      const PendingEdit* shown = nullptr;
      for (auto it = cell.edits.rbegin(); it != cell.edits.rend(); ++it) {
        if (it->optimistic) {
          shown = &*it;
          break;
        }
      }
      if (shown) {
        putVoxel(c, x, y, z, shown->voxelType, Bytes(shown->state.data(), shown->state.size()));
      } else {
        putVoxel(c, x, y, z, cell.baseType, Bytes(cell.baseState.data(), cell.baseState.size()));
      }
    }
  }
  changed(c, kChunkLoaded | kChunkVoxels | kChunkStates);
}

const ChunkData& ChunkStore::insertGenerated(const ChunkCoord& coord,
                                             const std::array<std::uint8_t, kChunkVolume>& voxels) {
  ChunkData& c = slotFor(coord);
  c.setGrid(voxels.data());
  c.storedOnServer = false;
  setDirty(c, options_.writeBackIntervalMs > 0 && durable());
  changed(c, kChunkLoaded | kChunkVoxels);
  return c;
}

void ChunkStore::markDirty(const ChunkCoord& coord) {
  ChunkData* c = slot(coord);
  if (c && options_.writeBackIntervalMs > 0 && !c->dirty) {
    setDirty(*c, true);
    changed(*c, 0);
  }
}

bool ChunkStore::evict(const ChunkCoord& coord) {
  missing_.erase(coord);
  ChunkData* c = slot(coord);
  if (!c) return false;
  if (c->dirty && durable()) {
    // It stays until its write-back ends; a later eviction takes it.
    if (!writing_.count(coord)) queueWriteBack(*c);
    return false;
  }
  dropSlot(coord);
  return true;
}

std::size_t ChunkStore::pruneIf(const std::function<bool(const ChunkData&)>& drop) {
  std::vector<ChunkCoord> chosen;
  for (const auto& [coord, index] : slotOf_) {
    if (drop(slots_[index])) chosen.push_back(coord);
  }
  std::size_t pruned = 0;
  for (const ChunkCoord& coord : chosen) {
    if (evict(coord)) ++pruned;
  }
  return pruned;
}

std::size_t ChunkStore::pruneBeyond(const ChunkCoord& center, int distance) {
  const std::size_t pruned =
      pruneIf([&](const ChunkData& c) { return chebyshev(c.coord, center) > distance; });
  for (auto it = missing_.begin(); it != missing_.end();) {
    it = chebyshev(*it, center) > distance ? missing_.erase(it) : std::next(it);
  }
  return pruned;
}

void ChunkStore::report() {
  if (changedSlots_.empty() && unloaded_.empty()) return;
  std::vector<ChunkChange> changes;
  changes.reserve(changedSlots_.size() + unloaded_.size());
  for (std::uint32_t index : changedSlots_) {
    ChunkData& c = slots_[index];
    if (c.changes_ == 0) continue;
    auto it = slotOf_.find(c.coord);
    if (it != slotOf_.end() && it->second == index) changes.push_back(ChunkChange{c.coord, c.changes_});
    c.changes_ = 0;
  }
  changes.insert(changes.end(), unloaded_.begin(), unloaded_.end());
  changedSlots_.clear();
  unloaded_.clear();
  if (onChanges_ && !changes.empty()) onChanges_(changes);
}

// ---------------------------------------------------------------------------
// Realtime edits: pacing, echoes, refusals, timeouts
// ---------------------------------------------------------------------------

ChunkStore::PendingCell* ChunkStore::cellOf(const ChunkCoord& coord, std::uint64_t key) {
  auto chunk = pending_.find(coord);
  if (chunk == pending_.end()) return nullptr;
  auto voxel = chunk->second.find(key);
  return voxel == chunk->second.end() ? nullptr : &voxel->second;
}

const ChunkStore::PendingCell* ChunkStore::cellOf(const ChunkCoord& coord,
                                                  std::uint64_t key) const {
  auto chunk = pending_.find(coord);
  if (chunk == pending_.end()) return nullptr;
  auto voxel = chunk->second.find(key);
  return voxel == chunk->second.end() ? nullptr : &voxel->second;
}

ChunkStore::PendingEdit* ChunkStore::pendingById(const EditRef& ref, PendingCell** cell) {
  PendingCell* found = cellOf(ref.coord, ref.key);
  if (!found) return nullptr;
  for (PendingEdit& e : found->edits) {
    if (e.id == ref.id) {
      if (cell) *cell = found;
      return &e;
    }
  }
  return nullptr;
}

bool ChunkStore::showPending(const ChunkCoord& coord, std::uint64_t key, PendingCell& cell) {
  ChunkData* c = slot(coord);
  if (!c) return false;
  const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 32));
  const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 16));
  const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(key));
  for (auto it = cell.edits.rbegin(); it != cell.edits.rend(); ++it) {
    if (!it->optimistic) continue;
    // Copied: a callback of show() may edit the voxel again.
    const std::int16_t type = it->voxelType;
    const std::vector<std::uint8_t> state = it->state;
    return show(*c, x, y, z, type, Bytes(state.data(), state.size()));
  }
  const std::int16_t type = cell.baseType;
  const std::vector<std::uint8_t> state = cell.baseState;
  return show(*c, x, y, z, type, Bytes(state.data(), state.size()));
}

Result<std::uint64_t> ChunkStore::setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                           std::int16_t voxelType, Bytes voxelState,
                                           const core::ActorUuid& uuid, bool optimistic,
                                           EditDone done) {
  if (!fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return Errc::InvalidArgument;
  if (voxelState.size() > wire::voxel::kMaxStateSize) return Errc::InvalidArgument;
  const std::uint64_t key = voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                     static_cast<std::int16_t>(z));
  PendingCell& cell = pending_[coord][key];
  if (cell.edits.empty()) {
    // The value under the edits: what the cache holds now, as the server confirmed it.
    cell.baseType = voxelTypeAt(coord, x, y, z);
    cell.baseState.clear();
    if (auto s = voxelStateAt(coord, x, y, z)) cell.baseState.assign(s->state.begin(), s->state.end());
    cell.baseTimeMs = 0;
    cell.baseArrival = 0;
    if (const ChunkData* c = find(coord)) {
      cell.baseTimeMs = c->timeOf(key);
      cell.baseArrival = c->arrivalOf(key);
    }
  }
  PendingEdit edit;
  edit.id = nextEditId_++;
  edit.uuid = uuid;
  edit.optimistic = optimistic;
  edit.voxelType = voxelType;
  edit.state.assign(voxelState.begin(), voxelState.end());
  edit.done = std::move(done);
  const std::uint64_t id = edit.id;
  cell.edits.push_back(std::move(edit));
  // Queued before it shows: an edit a callback makes goes out after this one.
  sendQueue_.push_back(EditRef{coord, key, id});
  if (optimistic) show(slotFor(coord), x, y, z, voxelType, voxelState);
  sendQueued();
  return id;
}

void ChunkStore::sendQueued() {
  // An edit a callback below makes is sent by this loop, not a nested one.
  if (sending_) return;
  sending_ = true;
  const std::int64_t now = nowMs();
  const double rate = std::max(1, options_.maxEditsPerSecond);
  const double burst = std::max(1.0, rate / 4.0);
  if (tokensAtMs_ < 0) {
    sendTokens_ = burst;
  } else {
    sendTokens_ = std::min(burst, sendTokens_ + static_cast<double>(now - tokensAtMs_) * rate / 1000.0);
  }
  tokensAtMs_ = now;
  const std::size_t maxInFlight =
      static_cast<std::size_t>(std::clamp(options_.maxEditsInFlight, 1, 128));
  while (!sendQueue_.empty() && inFlight_.size() < maxInFlight && sendTokens_ >= 1.0) {
    const EditRef ref = sendQueue_.front();
    sendQueue_.pop_front();
    PendingEdit* edit = pendingById(ref);
    if (!edit) continue;  // undone while it waited
    sendTokens_ -= 1.0;
    const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key >> 32));
    const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key >> 16));
    const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key));
    const Bytes state(edit->state.data(), edit->state.size());
    Result<std::uint8_t> seq = Errc::NotConnected;
    if (options_.send) {
      seq = options_.send(ref.coord, edit->uuid, x, y, z, edit->voxelType, state,
                          options_.voxelSendDistance);
    } else if (conn_) {
      seq = conn_->sendVoxelUpdate(ref.coord, edit->uuid, x, y, z, edit->voxelType, state,
                                   options_.voxelSendDistance);
    }
    if (!seq.ok()) {
      finishEdit(ref, EditResult::NotSent, wire::ErrorCode::NoError);
      continue;
    }
    // A sequence number still in flight was sent 256 sends ago: that edit can no longer
    // be told apart, so it ends as timed out.
    auto taken = inFlight_.find(seq.value());
    if (taken != inFlight_.end()) {
      const EditRef old = taken->second;
      finishEdit(old, EditResult::TimedOut, wire::ErrorCode::NoError);
      edit = pendingById(ref);
      if (!edit) continue;
    }
    edit->sequence = seq.value();
    edit->sentAtMs = now;
    inFlight_[seq.value()] = ref;
    if (recordSend_) recordSend_(seq.value());
    if (onEditSent_) onEditSent_(ref.id, seq.value());
  }
  sending_ = false;
}

std::optional<std::uint8_t> ChunkStore::sequenceOf(std::uint64_t editId) const {
  for (const auto& [seq, ref] : inFlight_) {
    if (ref.id == editId) return seq;
  }
  return std::nullopt;
}

bool ChunkStore::finishEdit(const EditRef& ref, EditResult result, wire::ErrorCode code,
                            std::int64_t echoTimeMs) {
  PendingCell* cell = nullptr;
  PendingEdit* found = pendingById(ref, &cell);
  if (!found) return false;
  // Whether the voxel shows this edit: the newest optimistic one is what it shows.
  bool wasShown = false;
  for (auto it = cell->edits.rbegin(); it != cell->edits.rend(); ++it) {
    if (!it->optimistic) continue;
    wasShown = &*it == found;
    break;
  }
  const auto index = static_cast<std::size_t>(found - cell->edits.data());
  PendingEdit edit = std::move(*found);
  cell->edits.erase(cell->edits.begin() + static_cast<std::ptrdiff_t>(index));
  if (edit.sequence >= 0) {
    auto it = inFlight_.find(static_cast<std::uint8_t>(edit.sequence));
    if (it != inFlight_.end() && it->second.id == ref.id) inFlight_.erase(it);
  }
  if (result == EditResult::Echoed) {
    // The server applied it: it is the confirmed value, unless the one held is newer.
    const std::int64_t t = clampTime(echoTimeMs);
    if (t <= 0 || t >= cell->baseTimeMs) {
      cell->baseType = edit.voxelType;
      cell->baseState = edit.state;
      if (t > 0) cell->baseTimeMs = t;
      cell->baseArrival = ++mergeSeq_;
    }
  }
  EditOutcome out;
  out.editId = ref.id;
  out.coord = ref.coord;
  out.x = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key >> 32));
  out.y = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key >> 16));
  out.z = static_cast<std::int16_t>(static_cast<std::uint16_t>(ref.key));
  out.voxelType = edit.voxelType;
  out.result = result;
  out.errorCode = code;
  ChunkData* c = slot(ref.coord);
  out.restored = result != EditResult::Echoed && wasShown && c != nullptr;
  bool changedShown = false;
  if (cell->edits.empty()) {
    // Settled: the voxel holds its confirmed value. Done with the lists before any
    // callback, which may edit again.
    PendingCell settled = std::move(*cell);
    auto chunk = pending_.find(ref.coord);
    chunk->second.erase(ref.key);
    if (chunk->second.empty()) pending_.erase(chunk);
    if (c) {
      c->setTime(ref.key, std::max(settled.baseTimeMs, c->timeOf(ref.key)), settled.baseArrival);
      changedShown = show(*c, out.x, out.y, out.z, settled.baseType,
                          Bytes(settled.baseState.data(), settled.baseState.size()));
    }
  } else if (c) {
    changedShown = showPending(ref.coord, ref.key, *cell);
  }
  const bool reload = result == EditResult::TimedOut && find(ref.coord) != nullptr;
  if (edit.done) edit.done(out);
  if (result != EditResult::Echoed && onEditRefused_) onEditRefused_(out);
  // The edit may have been applied with its echo lost: the server's copy settles it.
  if (reload) hydrateAsync(ref.coord);
  return changedShown;
}

bool ChunkStore::ingest(const replication::SpatialNotification& n,
                        const wire::VoxelPayloadView& voxel) {
  const std::uint64_t key = voxelKey(voxel.x, voxel.y, voxel.z);
  if (PendingCell* cell = cellOf(n.chunk, key)) {
    // This client's own edit comes back with its sender, sequence number and value.
    const core::ActorUuid sender = n.uuidArray();
    for (const PendingEdit& e : cell->edits) {
      if (e.sequence != static_cast<int>(n.sequence) || e.uuid != sender ||
          e.voxelType != voxel.voxelType || !sameBytes(voxel.state, e.state)) {
        continue;
      }
      const bool changed = finishEdit(EditRef{n.chunk, key, e.id}, EditResult::Echoed,
                                      wire::ErrorCode::NoError, n.epochMillis);
      sendQueued();
      return changed;
    }
  }
  return confirm(n.chunk, voxel.x, voxel.y, voxel.z, voxel.voxelType, voxel.state, n.epochMillis,
                 /*create=*/true);
}

bool ChunkStore::ingestError(const replication::GenericError& error) {
  auto it = inFlight_.find(error.sequence);
  if (it == inFlight_.end()) return false;
  const EditRef ref = it->second;
  finishEdit(ref, EditResult::Refused, error.code);
  sendQueued();
  return true;
}

void ChunkStore::expireEdits() {
  const std::int64_t cutoff = nowMs() - std::max<std::int64_t>(1, options_.editTimeoutMs);
  std::vector<EditRef> expired;
  for (const auto& [seq, ref] : inFlight_) {
    const PendingEdit* edit = pendingById(ref);
    if (!edit || edit->sentAtMs < cutoff) expired.push_back(ref);
  }
  for (const EditRef& ref : expired) {
    if (!pendingById(ref)) {
      for (auto it = inFlight_.begin(); it != inFlight_.end(); ++it) {
        if (it->second.id == ref.id) {
          inFlight_.erase(it);
          break;
        }
      }
      continue;
    }
    finishEdit(ref, EditResult::TimedOut, wire::ErrorCode::NoError);
  }
}

void ChunkStore::ingestChunkChanged(const ChunkCoord& coord, std::int64_t updatedAtMs) {
  if (!options_.reloadReplacedChunks) return;
  ChunkData* c = slot(coord);
  if (!c) return;
  const std::int64_t at = clampTime(updatedAtMs);
  if (c->storedAtMs > 0 && at > 0 && c->storedAtMs >= at) return;
  if (c->dirty) {
    // The server's copy is newer than a seed waiting to be written back over it.
    setDirty(*c, false);
    writeBackAttempts_.erase(coord);
    writeBackDueAt_.erase(coord);
  }
  hydrateAsync(coord);
}

void ChunkStore::connectionState(replication::ConnState state) {
  if (state == replication::ConnState::Connected) {
    if (wasConnected_ && disconnectedAtMs_ >= 0 && options_.catchUpOnReconnect) {
      catchUp(isoUtc(disconnectedAtMs_ - 30000));
    }
    disconnectedAtMs_ = -1;
    wasConnected_ = true;
  } else if (wasConnected_ && disconnectedAtMs_ < 0) {
    disconnectedAtMs_ = wallNowMs();
  }
}

// ---------------------------------------------------------------------------
// The durable store: queue, I/O thread, adoption
// ---------------------------------------------------------------------------

void ChunkStore::enqueue(IoJob job) {
  {
    std::lock_guard lock(ioMutex_);
    jobs_.push_back(std::move(job));
  }
  ioWake_.notify_one();
}

ChunkStore::IoResult ChunkStore::runJob(const IoJob& job) {
  IoResult r;
  r.job = job;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
#endif
    switch (job.kind) {
      case IoJob::Kind::LoadBox:
        r.chunks = source_->chunksInBox(appId_, job.box.lo, job.box.hi, job.since);
        break;
      case IoJob::Kind::LoadChunk:
        r.chunk = source_->chunkAt(appId_, job.coord);
        break;
      case IoJob::Kind::Write:
        r.outcome = source_->writeChunk(appId_, job.coord, Bytes(job.voxels.data(), job.voxels.size()));
        break;
      case IoJob::Kind::CatchUp:
        for (const Box& box : job.boxes) {
          for (StoredChunk& chunk : source_->chunksInBox(appId_, box.lo, box.hi, job.since)) {
            r.chunks.push_back(std::move(chunk));
          }
          for (StoredChunkEdits& edits : source_->editsInBox(appId_, box.lo, box.hi, job.since)) {
            r.edits.push_back(std::move(edits));
          }
        }
        break;
    }
#ifndef CROWDY_NO_EXCEPTIONS
  } catch (...) {
    r.failed = true;
    r.outcome.kind = graphql::GraphQLErrorKind::Network;
  }
#endif
  return r;
}

void ChunkStore::ioLoop() {
  for (;;) {
    {
      std::unique_lock lock(ioMutex_);
      ioWake_.wait(lock, [this] { return stopping_ || !jobs_.empty() || !replies_.empty(); });
      if (stopping_) return;
    }
    serviceIo(SIZE_MAX);
  }
}

std::size_t ChunkStore::runIo(std::size_t maxJobs) { return serviceIo(maxJobs); }

std::size_t ChunkStore::serviceIo(std::size_t maxJobs) {
  std::size_t ran = 0;
  while (ran < maxJobs) {
    std::optional<std::pair<std::uint64_t, ChunkSourceReply>> reply;
    IoJob job;
    {
      std::lock_guard lock(ioMutex_);
      if (stopping_) break;
      // Answers first: they finish work already under way.
      if (!replies_.empty()) {
        reply.emplace(std::move(replies_.front()));
        replies_.pop_front();
      } else if (!jobs_.empty()) {
        job = std::move(jobs_.front());
        jobs_.pop_front();
        ++jobsRunning_;
      } else {
        break;
      }
    }
    ++ran;
    if (reply) {
      onAsyncReply(reply->first, std::move(reply->second));
      continue;
    }
    if (asyncSource_) {
      startAsync(std::move(job));
      std::lock_guard lock(ioMutex_);
      --jobsRunning_;
      continue;
    }
    IoResult result = runJob(job);
    {
      std::lock_guard lock(ioMutex_);
      results_.push_back(std::move(result));
      --jobsRunning_;
    }
    ioDone_.notify_all();
  }
  return ran;
}

void ChunkStore::startAsync(IoJob job) {
  const std::uint64_t id = nextCallId_++;
  AsyncCall& call = calls_[id];
  call.result.job = std::move(job);
  sendAsync(id, call);
}

void ChunkStore::sendAsync(std::uint64_t id, AsyncCall& call) {
  const IoJob& job = call.result.job;
  graphql::JVal vars;
  std::string_view document;
  ChunkSourceRequest request;
  switch (job.kind) {
    case IoJob::Kind::LoadBox:
      vars["input"] = boxRegion(job.box.lo, job.box.hi, call.skip, job.since).toInput(appId_);
      document = gen::chunks::kGetChunksByDistanceDocument;
      request.operationName = "GetChunksByDistance";
      break;
    case IoJob::Kind::LoadChunk:
      vars["input"]["appId"] = appId_;
      vars["input"]["coordinates"] = domains::ChunkRef{job.coord.x, job.coord.y, job.coord.z}.toInput();
      document = gen::chunks::kGetChunkDocument;
      request.operationName = "GetChunk";
      break;
    case IoJob::Kind::Write:
      vars["input"]["appId"] = appId_;
      vars["input"]["coordinates"] = domains::ChunkRef{job.coord.x, job.coord.y, job.coord.z}.toInput();
      vars["input"]["voxels"] = core::base64Encode(Bytes(job.voxels.data(), job.voxels.size()));
      document = gen::chunks::kUpdateChunkDocument;
      request.operationName = "UpdateChunk";
      break;
    case IoJob::Kind::CatchUp: {
      const Box& box = job.boxes[call.box];
      vars["input"] = boxRegion(box.lo, box.hi, call.skip, job.since).toInput(appId_);
      if (call.edits) {
        document = gen::voxels::kListVoxelUpdatesByDistanceCellsTimedDocument;
        request.operationName = "ListVoxelUpdatesByDistanceCellsTimed";
      } else {
        document = gen::chunks::kGetChunksByDistanceDocument;
        request.operationName = "GetChunksByDistance";
      }
      break;
    }
  }
  request.body = graphql::requestBody(document, vars, request.operationName);
  std::shared_ptr<ReplySink> sink = sink_;
  asyncSource_->request(std::move(request), [sink, id](ChunkSourceReply reply) {
    std::lock_guard lock(sink->mutex);
    if (sink->store) sink->store->postReply(id, std::move(reply));
  });
}

void ChunkStore::postReply(std::uint64_t id, ChunkSourceReply reply) {
  {
    std::lock_guard lock(ioMutex_);
    replies_.emplace_back(id, std::move(reply));
  }
  ioWake_.notify_one();
  ioDone_.notify_all();
}

void ChunkStore::onAsyncReply(std::uint64_t id, ChunkSourceReply reply) {
  auto it = calls_.find(id);
  if (it == calls_.end()) return;
  AsyncCall& call = it->second;
  IoResult& r = call.result;
  graphql::GraphQLOutcome out;
  if (reply.httpStatus == 0) {
    out.status = Errc::SocketError;
    out.kind = graphql::GraphQLErrorKind::Network;
    out.errorMessage = std::move(reply.body);
  } else {
    out = graphql::interpretResponse(reply.httpStatus, reply.body);
  }
  if (r.job.kind == IoJob::Kind::Write) {
    r.outcome = std::move(out);
    finishAsync(id);
    return;
  }
  if (!out.ok()) {
    r.failed = true;
    r.outcome = std::move(out);
    finishAsync(id);
    return;
  }
  const graphql::Json root = rootField(out.data);
  switch (r.job.kind) {
    case IoJob::Kind::LoadBox: {
      const PageEnd end = chunkPage(root, call.skip, r.chunks);
      if (end.more) {
        call.skip = end.nextSkip;
        sendAsync(id, call);
        return;
      }
      break;
    }
    case IoJob::Kind::LoadChunk:
      if (root.isObject()) r.chunk = storedChunkOf(root);
      break;
    case IoJob::Kind::CatchUp: {
      const PageEnd end =
          call.edits ? editsPage(root, call.skip, r.edits) : chunkPage(root, call.skip, r.chunks);
      if (end.more) {
        call.skip = end.nextSkip;
        sendAsync(id, call);
        return;
      }
      // A box's chunks, then its edits, then the next box.
      call.skip = 0;
      if (!call.edits) {
        call.edits = true;
        sendAsync(id, call);
        return;
      }
      call.edits = false;
      if (++call.box < r.job.boxes.size()) {
        sendAsync(id, call);
        return;
      }
      break;
    }
    case IoJob::Kind::Write:
      break;
  }
  finishAsync(id);
}

void ChunkStore::finishAsync(std::uint64_t id) {
  auto it = calls_.find(id);
  if (it == calls_.end()) return;
  {
    std::lock_guard lock(ioMutex_);
    results_.push_back(std::move(it->second.result));
  }
  calls_.erase(it);
  ioDone_.notify_all();
}

void ChunkStore::finishLoad(const IoResult& result) {
  const Box& box = result.job.box;
  for (std::int64_t x = box.lo.x; x <= box.hi.x; ++x) {
    for (std::int64_t y = box.lo.y; y <= box.hi.y; ++y) {
      for (std::int64_t z = box.lo.z; z <= box.hi.z; ++z) {
        const ChunkCoord coord{x, y, z};
        if (!loading_.erase(coord)) continue;
        // Asked for and not stored on the server: not asked for again until pruned.
        if (!result.failed && !adoptSeen_.count(coord) && !find(coord)) missing_.insert(coord);
      }
    }
  }
}

bool ChunkStore::adopt(IoResult& result, std::int64_t nowMs,
                       std::chrono::steady_clock::time_point deadline) {
  switch (result.job.kind) {
    case IoJob::Kind::LoadBox: {
      std::size_t& at = adoptAt_;
      const std::size_t first = at;
      for (; at < result.chunks.size(); ++at) {
        // At least one chunk a tick, however small the budget.
        if (at > first && std::chrono::steady_clock::now() >= deadline) return false;
        const StoredChunk& stored = result.chunks[at];
        if (!adoptSeen_.insert(stored.coord).second) {
          ++duplicatePageChunks_;
          continue;
        }
        if (!loading_.count(stored.coord)) continue;
        const ChunkData* cached = find(stored.coord);
        // A chunk the store holds from the durable store, or a grid not written back
        // yet, keeps what it has.
        if (cached && (cached->hydratedAtMs > 0 || cached->dirty)) continue;
        applyStored(stored, nowMs, result.job.requestedSeq);
      }
      finishLoad(result);
      return true;
    }
    case IoJob::Kind::LoadChunk: {
      const ChunkCoord coord = result.job.coord;
      Reload reload;
      if (auto r = reloading_.find(coord); r != reloading_.end()) {
        reload = std::move(r->second);
        reloading_.erase(r);
      }
      bool applied = false;
      if (!result.failed && result.chunk && !reload.evicted) {
        const ChunkData* cached = find(coord);
        const std::int64_t rowMs = clampTime(result.chunk->updatedAtMs);
        if (cached && cached->hydratedAtMs > 0 && rowMs > 0 && cached->storedAtMs > rowMs) {
          // A newer copy came first (a catch-up's): the store already holds this one's news.
          applied = !cached->dirty;
        } else if (!(cached && cached->dirty)) {
          applyStored(*result.chunk, nowMs, result.job.requestedSeq);
          applied = true;
        }
      }
      if (reload.again) {
        // Asked for while this read was out: a read of its own.
        const ChunkData* cached = find(coord);
        if (cached && cached->dirty) {
          for (HydrateDone& done : reload.next) {
            if (done) done(false);
          }
        } else {
          reloading_[coord].waiting = std::move(reload.next);
          startReload(coord);
        }
      }
      for (HydrateDone& done : reload.waiting) {
        if (done) done(applied);
      }
      return true;
    }
    case IoJob::Kind::Write: {
      const ChunkCoord coord = result.job.coord;
      writing_.erase(coord);
      ChunkData* chunk = slot(coord);
      if (!chunk || !chunk->dirty) return true;
      if (result.outcome.ok()) {
        writeBackAttempts_.erase(coord);
        writeBackDueAt_.erase(coord);
        if (collectPersisted_) ++*collectPersisted_;
        // A grid changed while the write was out goes out again.
        if (chunk->version_ == result.job.version) {
          setDirty(*chunk, false);
          chunk->storedOnServer = true;
          changed(*chunk, 0);
        }
        return true;
      }
      const int attempts = writeBackAttempts_[coord] + 1;
      const bool retryable = writeBackRetryable(result.outcome);
      if (retryable && attempts < options_.writeBackAttempts) {
        writeBackAttempts_[coord] = attempts;
        writeBackDueAt_[coord] = lastTickMs_ + (options_.writeBackBackoffMs << (attempts - 1));
        return true;
      }
      writeBackAttempts_.erase(coord);
      writeBackDueAt_.erase(coord);
      setDirty(*chunk, false);
      ChunkWriteBackFailure failure;
      failure.coord = coord;
      failure.reason = retryable ? ChunkWriteBackDrop::Exhausted : ChunkWriteBackDrop::Refused;
      failure.attempts = attempts;
      failure.error = std::move(result.outcome);
      if (collectDropped_) collectDropped_->push_back(failure);
      if (onWriteBackFailed_) onWriteBackFailed_(failure);
      return true;
    }
    case IoJob::Kind::CatchUp: {
      if (result.failed) return true;
      const std::uint64_t readSeq = result.job.requestedSeq;
      for (const StoredChunkEdits& group : result.edits) {
        // A chunk the store does not hold gets a slot only when it was never stored: one
        // that was is read whole when it is asked for.
        const bool create = !find(group.coord) && missing_.count(group.coord) > 0;
        if (!create && !find(group.coord)) continue;
        for (const StoredVoxelState& e : group.edits) {
          confirm(group.coord, e.x, e.y, e.z, e.voxelType, Bytes(e.state.data(), e.state.size()),
                  e.updatedAtMs, create, readSeq);
        }
        if (create) {
          missing_.erase(group.coord);
          if (ChunkData* made = slot(group.coord)) made->hydratedAtMs = nowMs > 0 ? nowMs : 1;
        }
      }
      for (const StoredChunk& stored : result.chunks) {
        const ChunkData* c = find(stored.coord);
        if (!c || c->dirty) continue;
        const std::int64_t rowMs = clampTime(stored.updatedAtMs);
        if (c->hydratedAtMs > 0 && rowMs > 0 && c->storedAtMs >= rowMs) continue;
        applyStored(stored, nowMs, readSeq);
      }
      return true;
    }
  }
  return true;
}

bool ChunkStore::adoptResults(std::int64_t nowMs, std::int64_t budgetUs) {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = budgetUs > 0 ? start + std::chrono::microseconds(budgetUs)
                                     : std::chrono::steady_clock::time_point::max();
  for (;;) {
    if (!adopting_) {
      std::lock_guard lock(ioMutex_);
      if (results_.empty()) return false;
      adopting_.emplace(std::move(results_.front()));
      results_.pop_front();
      adoptAt_ = 0;
      adoptSeen_.clear();
    }
    if (!adopt(*adopting_, nowMs, deadline)) return true;
    adopting_.reset();
    if (std::chrono::steady_clock::now() >= deadline) {
      std::lock_guard lock(ioMutex_);
      return !results_.empty();
    }
  }
}

void ChunkStore::queueWriteBack(ChunkData& chunk) {
  writing_.insert(chunk.coord);
  IoJob job;
  job.kind = IoJob::Kind::Write;
  job.coord = chunk.coord;
  const std::uint8_t* grid = chunk.grid();
  job.voxels.assign(grid, grid + kChunkVolume);
  job.version = chunk.version_;
  enqueue(std::move(job));
}

void ChunkStore::waitForIo(const std::function<bool()>& done, std::int64_t timeoutMs) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!done()) {
    if (!options_.ioThread) runIo();
    adoptResults(nowMs(), 0);
    if (done() || std::chrono::steady_clock::now() >= until) break;
    std::unique_lock lock(ioMutex_);
    ioDone_.wait_for(lock, std::chrono::milliseconds(10), [this] {
      return !results_.empty() || (!options_.ioThread && !replies_.empty()) || stopping_;
    });
  }
  report();
}

// ---------------------------------------------------------------------------
// Loads, catch-up, write-back
// ---------------------------------------------------------------------------

std::size_t ChunkStore::ensureAroundAsync(const ChunkCoord& center, int distance) {
  if (!durable()) return 0;
  const int d = std::clamp(distance, 0, 8);
  std::vector<ChunkCoord> wanted;
  for (std::int64_t dx = -d; dx <= d; ++dx) {
    for (std::int64_t dy = -d; dy <= d; ++dy) {
      for (std::int64_t dz = -d; dz <= d; ++dz) {
        const ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz};
        if (loading_.count(coord) || missing_.count(coord)) continue;
        const ChunkData* cached = find(coord);
        if (cached && (cached->hydratedAtMs > 0 || cached->dirty)) continue;
        wanted.push_back(coord);
      }
    }
  }
  if (wanted.empty()) return 0;
  for (const ChunkCoord& coord : wanted) loading_.insert(coord);
  for (const auto& [lo, hi] : coverWithBoxes(wanted)) {
    IoJob job;
    job.kind = IoJob::Kind::LoadBox;
    job.box = Box{lo, hi};
    job.requestedSeq = mergeSeq_;
    enqueue(std::move(job));
  }
  return wanted.size();
}

std::size_t ChunkStore::ensureAround(const ChunkCoord& center, int distance) {
  if (!durable()) return 0;
  const std::uint64_t before = appliedCount_;
  ensureAroundAsync(center, distance);
  const int d = std::clamp(distance, 0, 8);
  auto settled = [&] {
    if (adopting_) return false;
    for (std::int64_t dx = -d; dx <= d; ++dx)
      for (std::int64_t dy = -d; dy <= d; ++dy)
        for (std::int64_t dz = -d; dz <= d; ++dz)
          if (loading_.count({center.x + dx, center.y + dy, center.z + dz})) return false;
    return true;
  };
  waitForIo(settled, 600000);
  return static_cast<std::size_t>(appliedCount_ - before);
}

void ChunkStore::startReload(const ChunkCoord& coord) {
  IoJob job;
  job.kind = IoJob::Kind::LoadChunk;
  job.coord = coord;
  job.requestedSeq = mergeSeq_;
  enqueue(std::move(job));
}

bool ChunkStore::hydrateAsync(const ChunkCoord& coord, HydrateDone done) {
  if (!durable()) return false;
  const ChunkData* cached = find(coord);
  if (cached && cached->dirty) return false;
  auto it = reloading_.find(coord);
  if (it != reloading_.end()) {
    // The read on the way was asked for before this call: the next one answers it.
    it->second.again = true;
    if (done) it->second.next.push_back(std::move(done));
    return true;
  }
  Reload& reload = reloading_[coord];
  if (done) reload.waiting.push_back(std::move(done));
  missing_.erase(coord);
  startReload(coord);
  return true;
}

std::size_t ChunkStore::hydrate(const ChunkCoord& coord) {
  // Shared: the callback outlives a wait that times out.
  auto state = std::make_shared<std::pair<bool, bool>>(false, false);
  if (!hydrateAsync(coord, [state](bool applied) { *state = {true, applied}; })) return 0;
  waitForIo([&] { return state->first; }, 600000);
  return state->second ? 1 : 0;
}

bool ChunkStore::queueCatchUp(const std::string& sinceIso, std::vector<Box> boxes) {
  if (!durable() || boxes.empty()) return false;
  IoJob job;
  job.kind = IoJob::Kind::CatchUp;
  job.boxes = std::move(boxes);
  job.since = sinceIso;
  job.requestedSeq = mergeSeq_;
  enqueue(std::move(job));
  return true;
}

bool ChunkStore::catchUp(const std::string& sinceIso) {
  if (!durable()) return false;
  std::vector<ChunkCoord> coords;
  coords.reserve(slotOf_.size() + missing_.size());
  for (const auto& [coord, index] : slotOf_) coords.push_back(coord);
  for (const ChunkCoord& coord : missing_) {
    if (!slotOf_.count(coord)) coords.push_back(coord);
  }
  if (coords.empty()) return false;
  std::vector<Box> boxes;
  for (const auto& [lo, hi] : tileBoxes(coords)) boxes.push_back(Box{lo, hi});
  return queueCatchUp(sinceIso, std::move(boxes));
}

bool ChunkStore::catchUp(const std::string& sinceIso, const std::vector<ChunkBox>& boxes) {
  // More than this many boxes of 17^3 is not a view: refused rather than read.
  constexpr std::size_t kMaxBoxes = 64;
  std::vector<Box> cut;
  for (const ChunkBox& b : boxes) {
    const ChunkCoord lo{std::min(b.low.x, b.high.x), std::min(b.low.y, b.high.y),
                        std::min(b.low.z, b.high.z)};
    const ChunkCoord hi{std::max(b.low.x, b.high.x), std::max(b.low.y, b.high.y),
                        std::max(b.low.z, b.high.z)};
    const std::int64_t pieces = ((hi.x - lo.x) / kBoxSide + 1) * ((hi.y - lo.y) / kBoxSide + 1) *
                                ((hi.z - lo.z) / kBoxSide + 1);
    if (pieces < 0 || cut.size() + static_cast<std::size_t>(pieces) > kMaxBoxes) return false;
    for (std::int64_t x = lo.x; x <= hi.x; x += kBoxSide) {
      for (std::int64_t y = lo.y; y <= hi.y; y += kBoxSide) {
        for (std::int64_t z = lo.z; z <= hi.z; z += kBoxSide) {
          cut.push_back(Box{{x, y, z},
                            {std::min(hi.x, x + kBoxSide - 1), std::min(hi.y, y + kBoxSide - 1),
                             std::min(hi.z, z + kBoxSide - 1)}});
        }
      }
    }
  }
  return queueCatchUp(sinceIso, std::move(cut));
}

bool ChunkStore::mergeEdits(const StoredChunkEdits& edits) {
  bool changed = false;
  for (const StoredVoxelState& e : edits.edits) {
    changed |= confirm(edits.coord, e.x, e.y, e.z, e.voxelType,
                       Bytes(e.state.data(), e.state.size()), e.updatedAtMs, /*create=*/true);
  }
  return changed;
}

ChunkFlushResult ChunkStore::flush() {
  ChunkFlushResult result;
  if (!durable()) return result;
  for (const ChunkCoord& coord : std::vector<ChunkCoord>(dirty_.begin(), dirty_.end())) {
    ChunkData* chunk = slot(coord);
    if (!chunk || writing_.count(coord)) continue;
    queueWriteBack(*chunk);
    ++result.persisted;
  }
  return result;
}

ChunkFlushResult ChunkStore::flushAndWait(std::int64_t timeoutMs) {
  ChunkFlushResult result;
  if (!durable()) return result;
  collectDropped_ = &result.dropped;
  collectPersisted_ = &result.persisted;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  flush();
  while ((!dirty_.empty() || !writing_.empty()) && std::chrono::steady_clock::now() < until) {
    if (!options_.ioThread) runIo();
    adoptResults(nowMs(), 0);
    // A write-back backing off goes again once due (this wait is its tick).
    const std::int64_t now = lastTickMs_ = nowMs();
    for (const ChunkCoord& coord : std::vector<ChunkCoord>(dirty_.begin(), dirty_.end())) {
      if (writing_.count(coord)) continue;
      auto due = writeBackDueAt_.find(coord);
      if (due != writeBackDueAt_.end() && due->second > now) continue;
      if (ChunkData* chunk = slot(coord)) queueWriteBack(*chunk);
    }
    std::unique_lock lock(ioMutex_);
    ioDone_.wait_for(lock, std::chrono::milliseconds(10), [this] {
      return !results_.empty() || (!options_.ioThread && !replies_.empty()) || stopping_;
    });
  }
  collectDropped_ = nullptr;
  collectPersisted_ = nullptr;
  report();
  return result;
}

void ChunkStore::tick(std::int64_t nowMs) {
  lastTickMs_ = nowMs;
  adoptResults(this->nowMs(), options_.adoptBudgetUs);
  expireEdits();
  sendQueued();
  if (durable() && options_.writeBackIntervalMs > 0 &&
      nowMs - lastWriteBackMs_ >= options_.writeBackIntervalMs) {
    // The first dirty chunk that is due: one waiting out a backoff does not hold up the
    // others.
    for (const ChunkCoord& coord : dirty_) {
      if (writing_.count(coord)) continue;
      auto due = writeBackDueAt_.find(coord);
      if (due != writeBackDueAt_.end() && due->second > nowMs) continue;
      if (ChunkData* chunk = slot(coord)) {
        lastWriteBackMs_ = nowMs;
        queueWriteBack(*chunk);
        break;  // at most one chunk per interval
      }
    }
  }
  report();
}

}  // namespace crowdy::session
