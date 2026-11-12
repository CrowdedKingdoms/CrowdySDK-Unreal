#include "crowdy/session/chunk_store.hpp"

#include <algorithm>
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
  char out[32];
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

/// Boxes covering `coords`, each at most twice as large as the chunks it holds, found by
/// cutting where the two halves' boxes are smallest: a step uncovers one slab per axis
/// moved, so it is one box per slab. A set too scattered for 8 boxes is read as its
/// bounding box (an area is at most 17^3). Mirrors CrowdyJS's coverWithBoxes.
std::vector<std::pair<ChunkCoord, ChunkCoord>> coverWithBoxes(const std::vector<ChunkCoord>& coords) {
  constexpr std::size_t kMaxBoxes = 8;
  constexpr std::int64_t kMaxBox = 17 * 17 * 17;
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
    chunk.voxelStates.push_back(std::move(voxel));
  });
  chunk.voxelStatesTruncated = chunkJson["voxelStatesTruncated"].asBool();
  const std::string_view updatedAt = chunkJson["updatedAt"].asStringView();
  if (!updatedAt.empty()) chunk.updatedAtMs = core::parseIso8601Millis(updatedAt.data(), updatedAt.size());
  return chunk;
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
    for (std::int64_t skip = 0;;) {
      domains::ChunkRegion area = domains::ChunkRegion::box(domains::ChunkRef{low.x, low.y, low.z},
                                                            domains::ChunkRef{high.x, high.y, high.z});
      area.limit(kPage).skip(skip);
      if (!sinceIso.empty()) area.since(sinceIso);
      graphql::Json page = chunks_.region(appId, area);
      std::int64_t rows = 0;
      page["chunks"].forEach([&](graphql::Json chunkJson) {
        ++rows;
        out.push_back(storedChunkOf(chunkJson));
      });
      const std::int64_t applied = page["limit"].asInt64(kPage);
      if (rows == 0 || applied <= 0 || rows < applied) break;
      skip = page["skip"].asInt64(skip) + applied;
    }
    return out;
  }

  std::vector<StoredChunkEdits> editsInBox(const std::string& appId, const ChunkCoord& low,
                                           const ChunkCoord& high,
                                           const std::string& sinceIso) override {
    std::vector<StoredChunkEdits> out;
    if (!voxels_) return out;
    for (std::int64_t skip = 0;;) {
      domains::ChunkRegion area = domains::ChunkRegion::box(domains::ChunkRef{low.x, low.y, low.z},
                                                            domains::ChunkRef{high.x, high.y, high.z});
      area.limit(kPage).skip(skip).since(sinceIso);
      graphql::Json page = voxels_->listInRegion(appId, area);
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
          edits.edits.push_back(std::move(edit));
        });
        out.push_back(std::move(edits));
      });
      const std::int64_t applied = page["limit"].asInt64(kPage);
      if (rows == 0 || applied <= 0 || rows < applied) break;
      skip = page["skip"].asInt64(skip) + applied;
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
  static constexpr std::int64_t kPage = 1000;
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

// ---------------------------------------------------------------------------
// ChunkStore: construction, slots, reads
// ---------------------------------------------------------------------------

ChunkStore::ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi,
                       std::string appId, Options options)
    : conn_(conn), appId_(std::move(appId)), options_(std::move(options)) {
  if (chunksApi) {
    ownedSource_ = makeGameApiChunkSource(*chunksApi);
    source_ = ownedSource_.get();
  }
  if (source_ && options_.ioThread) io_ = std::thread([this] { ioLoop(); });
}

ChunkStore::ChunkStore(replication::Connection& conn, IChunkSource* source, std::string appId,
                       Options options)
    : conn_(conn), source_(source), appId_(std::move(appId)), options_(std::move(options)) {
  if (source_ && options_.ioThread) io_ = std::thread([this] { ioLoop(); });
}

ChunkStore::~ChunkStore() {
  {
    std::lock_guard lock(ioMutex_);
    stopping_ = true;
  }
  ioWake_.notify_all();
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

void ChunkStore::evict(const ChunkCoord& coord) {
  auto it = slotOf_.find(coord);
  if (it == slotOf_.end()) return;
  const std::uint32_t index = it->second;
  ChunkData& c = slots_[index];
  if (c.dirty) setDirty(c, false);
  writeBackAttempts_.erase(coord);
  writeBackDueAt_.erase(coord);
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

void ChunkStore::applyLocal(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
                            Bytes state) {
  ChunkData& c = slotFor(coord);
  const std::int16_t before = voxelTypeAt(coord, x, y, z);
  const bool hadState = voxelStateAt(coord, x, y, z).has_value();
  putVoxel(c, x, y, z, voxelType, state);
  std::uint32_t flags = 0;
  if (before != voxelType) flags |= kChunkVoxels;
  if (hadState || !state.empty() || !fitsDenseGrid(x, y, z, voxelType)) flags |= kChunkStates;
  changed(c, flags);
  if (before != voxelType) borderNeighbours(coord, x, y, z);
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

void ChunkStore::applyStored(const StoredChunk& stored, std::int64_t nowMs) {
  ++appliedCount_;
  ChunkData& c = slotFor(stored.coord);
  c.storedOnServer = true;
  c.hydratedAtMs = nowMs > 0 ? nowMs : 1;
  c.voxelStatesTruncated = stored.voxelStatesTruncated;
  if (stored.updatedAtMs > 0) c.storedAtMs = stored.updatedAtMs;
  // The stored copy replaces the chunk's states and overlay (a world.set_chunk drops the
  // older edits); a copy without a grid keeps the one cached.
  c.states_.clear();
  c.overlay_.clear();
  c.bytes_.clear();
  c.garbage_ = 0;
  if (stored.voxels.size() == static_cast<std::size_t>(kChunkVolume)) c.setGrid(stored.voxels.data());
  for (const StoredVoxelState& entry : stored.voxelStates) {
    if (!fitsInt16(entry.x) || !fitsInt16(entry.y) || !fitsInt16(entry.z)) continue;
    putVoxel(c, entry.x, entry.y, entry.z, entry.voxelType,
             Bytes(entry.state.data(), entry.state.size()));
  }
  reapplyPending(c);
  changed(c, kChunkLoaded | kChunkVoxels | kChunkStates);
}

void ChunkStore::reapplyPending(ChunkData& c) {
  auto chunk = pending_.find(c.coord);
  if (chunk == pending_.end()) return;
  for (auto& [key, edits] : chunk->second) {
    const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 32));
    const auto y = static_cast<std::int16_t>(static_cast<std::uint16_t>(key >> 16));
    const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(key));
    PendingEdit* first = nullptr;
    PendingEdit* newest = nullptr;
    for (PendingEdit& e : edits) {
      if (e.stale) continue;
      if (!first) first = &e;
      newest = &e;
    }
    if (!first) continue;
    // What the stored copy holds for the voxel is what an undo restores.
    first->prevType = voxelTypeAt(c.coord, x, y, z);
    first->prevState.clear();
    if (auto s = voxelStateAt(c.coord, x, y, z)) first->prevState.assign(s->state.begin(), s->state.end());
    if (!newest->optimistic) continue;
    putVoxel(c, x, y, z, newest->voxelType, Bytes(newest->state.data(), newest->state.size()));
  }
}

const ChunkData& ChunkStore::insertGenerated(const ChunkCoord& coord,
                                             const std::array<std::uint8_t, kChunkVolume>& voxels) {
  ChunkData& c = slotFor(coord);
  c.setGrid(voxels.data());
  c.storedOnServer = false;
  setDirty(c, options_.writeBackIntervalMs > 0 && source_ != nullptr);
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

std::size_t ChunkStore::pruneBeyond(const ChunkCoord& center, int distance) {
  std::vector<ChunkCoord> distant;
  for (const auto& [coord, index] : slotOf_) {
    if (chebyshev(coord, center) > distance) distant.push_back(coord);
  }
  std::size_t pruned = 0;
  for (const ChunkCoord& coord : distant) {
    ChunkData* c = slot(coord);
    if (!c) continue;
    if (c->dirty && source_) {
      // It stays until its write-back ends; a later prune evicts it.
      if (!writing_.count(coord)) queueWriteBack(*c);
      continue;
    }
    evict(coord);
    ++pruned;
  }
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

std::vector<ChunkStore::PendingEdit>* ChunkStore::pendingOf(const ChunkCoord& coord,
                                                            std::uint64_t key) {
  auto chunk = pending_.find(coord);
  if (chunk == pending_.end()) return nullptr;
  auto voxel = chunk->second.find(key);
  return voxel == chunk->second.end() ? nullptr : &voxel->second;
}

ChunkStore::PendingEdit* ChunkStore::pendingById(const EditRef& ref,
                                                 std::vector<PendingEdit>** list) {
  std::vector<PendingEdit>* edits = pendingOf(ref.coord, ref.key);
  if (!edits) return nullptr;
  for (PendingEdit& e : *edits) {
    if (e.id == ref.id) {
      if (list) *list = edits;
      return &e;
    }
  }
  return nullptr;
}

Result<std::uint64_t> ChunkStore::setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                           std::int16_t voxelType, Bytes voxelState,
                                           const core::ActorUuid& uuid, bool optimistic,
                                           EditDone done) {
  if (!fitsInt16(x) || !fitsInt16(y) || !fitsInt16(z)) return Errc::InvalidArgument;
  if (voxelState.size() > wire::voxel::kMaxStateSize) return Errc::InvalidArgument;
  const std::uint64_t key = voxelKey(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                                     static_cast<std::int16_t>(z));
  PendingEdit edit;
  edit.id = nextEditId_++;
  edit.uuid = uuid;
  edit.stale = !optimistic;
  edit.optimistic = optimistic;
  edit.voxelType = voxelType;
  edit.state.assign(voxelState.begin(), voxelState.end());
  edit.prevType = voxelTypeAt(coord, x, y, z);
  if (auto s = voxelStateAt(coord, x, y, z)) edit.prevState.assign(s->state.begin(), s->state.end());
  edit.done = std::move(done);
  const std::uint64_t id = edit.id;
  if (optimistic) applyLocal(coord, x, y, z, voxelType, voxelState);
  pending_[coord][key].push_back(std::move(edit));
  sendQueue_.push_back(EditRef{coord, key, id});
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
    auto seq = options_.send ? options_.send(ref.coord, edit->uuid, x, y, z, edit->voxelType, state,
                                             options_.voxelSendDistance)
                             : conn_.sendVoxelUpdate(ref.coord, edit->uuid, x, y, z, edit->voxelType,
                                                     state, options_.voxelSendDistance);
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

void ChunkStore::finishEdit(const EditRef& ref, EditResult result, wire::ErrorCode code) {
  std::vector<PendingEdit>* list = nullptr;
  PendingEdit* found = pendingById(ref, &list);
  if (!found) return;
  const auto index = static_cast<std::size_t>(found - list->data());
  PendingEdit edit = std::move(*found);
  list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
  if (edit.sequence >= 0) {
    auto it = inFlight_.find(static_cast<std::uint8_t>(edit.sequence));
    if (it != inFlight_.end() && it->second.id == ref.id) inFlight_.erase(it);
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
  bool restore = false;
  if (result != EditResult::Echoed && edit.optimistic && !edit.stale && find(ref.coord)) {
    if (index < list->size()) {
      // A newer edit of the voxel shows; it now undoes to what this one replaced.
      PendingEdit& next = (*list)[index];
      if (!next.stale) {
        next.prevType = edit.prevType;
        next.prevState = edit.prevState;
      }
    } else {
      restore = true;
    }
  }
  // Done with the lists before any callback, which may edit again.
  if (list->empty()) {
    auto chunk = pending_.find(ref.coord);
    chunk->second.erase(ref.key);
    if (chunk->second.empty()) pending_.erase(chunk);
  }
  if (restore) {
    ChunkData& c = *slot(ref.coord);
    const std::int16_t before = voxelTypeAt(ref.coord, out.x, out.y, out.z);
    putVoxel(c, out.x, out.y, out.z, edit.prevType,
             Bytes(edit.prevState.data(), edit.prevState.size()));
    changed(c, (before != edit.prevType ? static_cast<std::uint32_t>(kChunkVoxels) : 0u) |
                   kChunkStates);
    if (before != edit.prevType) borderNeighbours(ref.coord, out.x, out.y, out.z);
    out.restored = true;
  }
  const bool reload = result == EditResult::TimedOut && find(ref.coord) != nullptr;
  if (edit.done) edit.done(out);
  if (result != EditResult::Echoed && onEditRefused_) onEditRefused_(out);
  // The edit may have been applied with its echo lost: the server's copy settles it.
  if (reload) hydrateAsync(ref.coord);
}

bool ChunkStore::takeEcho(const replication::SpatialNotification& n,
                          const wire::VoxelPayloadView& voxel) {
  const std::uint64_t key = voxelKey(voxel.x, voxel.y, voxel.z);
  std::vector<PendingEdit>* edits = pendingOf(n.chunk, key);
  if (!edits || edits->empty()) return true;
  const core::ActorUuid sender = n.uuidArray();
  auto echoed = std::find_if(edits->begin(), edits->end(), [&](const PendingEdit& e) {
    return e.sequence == static_cast<int>(n.sequence) && e.uuid == sender;
  });
  if (echoed == edits->end()) {
    for (PendingEdit& e : *edits) e.stale = true;
    return true;
  }
  const bool stale = echoed->stale;
  const bool newerLocalEdit = std::next(echoed) != edits->end();
  finishEdit(EditRef{n.chunk, key, echoed->id}, EditResult::Echoed, wire::ErrorCode::NoError);
  sendQueued();
  return stale && !newerLocalEdit;
}

bool ChunkStore::ingest(const replication::SpatialNotification& n,
                        const wire::VoxelPayloadView& voxel) {
  if (!takeEcho(n, voxel)) return false;
  applyLocal(n.chunk, voxel.x, voxel.y, voxel.z, voxel.voxelType, voxel.state);
  return true;
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
  if (c->storedAtMs > 0 && updatedAtMs > 0 && c->storedAtMs >= updatedAtMs) return;
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
        r.chunks = source_->chunksInBox(appId_, job.box.lo, job.box.hi, job.since);
        r.edits = source_->editsInBox(appId_, job.box.lo, job.box.hi, job.since);
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
    IoJob job;
    {
      std::unique_lock lock(ioMutex_);
      ioWake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      ++jobsRunning_;
    }
    IoResult result = runJob(job);
    {
      std::lock_guard lock(ioMutex_);
      results_.push_back(std::move(result));
      --jobsRunning_;
    }
    ioDone_.notify_all();
  }
}

std::size_t ChunkStore::runIo(std::size_t maxJobs) {
  std::size_t ran = 0;
  while (ran < maxJobs) {
    IoJob job;
    {
      std::lock_guard lock(ioMutex_);
      if (jobs_.empty()) break;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      ++jobsRunning_;
    }
    IoResult result = runJob(job);
    {
      std::lock_guard lock(ioMutex_);
      results_.push_back(std::move(result));
      --jobsRunning_;
    }
    ioDone_.notify_all();
    ++ran;
  }
  return ran;
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
      for (; at < result.chunks.size(); ++at) {
        if ((at & 15) == 15 && std::chrono::steady_clock::now() >= deadline) return false;
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
        applyStored(stored, nowMs);
      }
      finishLoad(result);
      return true;
    }
    case IoJob::Kind::LoadChunk: {
      const ChunkCoord coord = result.job.coord;
      reloading_.erase(coord);
      if (!result.failed && result.chunk) {
        const ChunkData* cached = find(coord);
        if (!(cached && cached->dirty)) applyStored(*result.chunk, nowMs);
      }
      if (reloadAgain_.erase(coord)) hydrateAsync(coord);
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
      for (const StoredChunkEdits& group : result.edits) {
        ChunkData* c = slot(group.coord);
        if (!c || c->hydratedAtMs == 0) continue;
        for (const StoredVoxelState& e : group.edits) {
          if (!fitsInt16(e.x) || !fitsInt16(e.y) || !fitsInt16(e.z)) continue;
          // This client's own unanswered edit of the voxel settles with its echo.
          const std::uint64_t key = voxelKey(static_cast<std::int16_t>(e.x), static_cast<std::int16_t>(e.y),
                                             static_cast<std::int16_t>(e.z));
          if (pendingOf(group.coord, key)) continue;
          applyLocal(group.coord, e.x, e.y, e.z, e.voxelType, Bytes(e.state.data(), e.state.size()));
        }
      }
      for (const StoredChunk& stored : result.chunks) {
        const ChunkData* c = find(stored.coord);
        if (!c || c->dirty || c->hydratedAtMs == 0) continue;
        if (stored.updatedAtMs > 0 && c->storedAtMs >= stored.updatedAtMs) continue;
        applyStored(stored, nowMs);
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
    ioDone_.wait_for(lock, std::chrono::milliseconds(10),
                     [this] { return !results_.empty() || stopping_; });
  }
  report();
}

// ---------------------------------------------------------------------------
// Loads, catch-up, write-back
// ---------------------------------------------------------------------------

std::size_t ChunkStore::ensureAroundAsync(const ChunkCoord& center, int distance) {
  if (!source_) return 0;
  const int d = std::clamp(distance, 0, 8);
  lastArea_ = Box{{center.x - d, center.y - d, center.z - d}, {center.x + d, center.y + d, center.z + d}};
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
    enqueue(std::move(job));
  }
  return wanted.size();
}

std::size_t ChunkStore::ensureAround(const ChunkCoord& center, int distance) {
  if (!source_) return 0;
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

bool ChunkStore::hydrateAsync(const ChunkCoord& coord) {
  if (!source_) return false;
  const ChunkData* cached = find(coord);
  if (cached && cached->dirty) return false;
  if (reloading_.count(coord)) {
    reloadAgain_.insert(coord);
    return true;
  }
  reloading_.insert(coord);
  missing_.erase(coord);
  IoJob job;
  job.kind = IoJob::Kind::LoadChunk;
  job.coord = coord;
  enqueue(std::move(job));
  return true;
}

std::size_t ChunkStore::hydrate(const ChunkCoord& coord) {
  const ChunkData* before = find(coord);
  const std::uint64_t version = before ? before->version_ : 0;
  const std::int64_t hydratedAt = before ? before->hydratedAtMs : 0;
  if (!hydrateAsync(coord)) return 0;
  waitForIo([&] { return !reloading_.count(coord) && !adopting_.has_value(); }, 600000);
  const ChunkData* after = find(coord);
  if (!after || after->hydratedAtMs == 0) return 0;
  return (after->version_ != version || after->hydratedAtMs != hydratedAt || !before) ? 1 : 0;
}

bool ChunkStore::catchUp(const std::string& sinceIso) {
  if (!source_ || !lastArea_) return false;
  IoJob job;
  job.kind = IoJob::Kind::CatchUp;
  job.box = *lastArea_;
  job.since = sinceIso;
  enqueue(std::move(job));
  return true;
}

ChunkFlushResult ChunkStore::flush() {
  ChunkFlushResult result;
  if (!source_) return result;
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
  if (!source_) return result;
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
    ioDone_.wait_for(lock, std::chrono::milliseconds(10),
                     [this] { return !results_.empty() || stopping_; });
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
  if (source_ && options_.writeBackIntervalMs > 0 &&
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
