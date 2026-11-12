#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// World-data domains (Game API): chunks, voxels, actors, avatars, per-user
/// app state, host election, and teleport. Durable storage — realtime edits
/// travel over the replication client instead.
namespace crowdy::domains {

/// The chunks a region read covers, and which page of them: a cube of Chebyshev radius 1-8
/// around a center, or a box between two corners (both inclusive, in either order, at most
/// 4,913 chunks: the cube a radius of 8 reads). A cube comes nearest first, a box by x, then y,
/// then z, and either way each chunk is on exactly one page. Boxes, `since` on chunk reads and
/// stable pages need ck-api v2.67.0.
class ChunkRegion {
 public:
  static ChunkRegion cube(const ChunkRef& center, int maxDistance) {
    ChunkRegion region;
    region.low_ = center;
    region.maxDistance_ = maxDistance;
    return region;
  }

  /// For streaming the world in as a player walks: one step reads only the slab it uncovers.
  static ChunkRegion box(const ChunkRef& low, const ChunkRef& high) {
    ChunkRegion region;
    region.low_ = low;
    region.high_ = high;
    region.box_ = true;
    return region;
  }

  /// At most this many chunks; 0 for the server's default, 1,000.
  ChunkRegion& limit(std::int64_t chunks) {
    limit_ = chunks;
    return *this;
  }

  /// The next page starts where the last one ended: its `skip` plus its `limit`.
  ChunkRegion& skip(std::int64_t chunks) {
    skip_ = chunks;
    return *this;
  }

  /// Only what changed at or after this ISO-8601 time. For chunk reads that is a chunk whose
  /// stored row was written then (updateChunk, updateChunkState, updateChunkLods; a voxel edit
  /// does not move it); for edit reads, the edits.
  ChunkRegion& since(std::string isoTime) {
    since_ = std::move(isoTime);
    return *this;
  }

  /// The by-distance input of both region reads, for `appId`.
  graphql::JVal toInput(std::string_view appId) const {
    graphql::JVal input;
    input["appId"] = appId;
    if (box_) {
      input["lowChunk"] = low_.toInput();
      input["highChunk"] = high_.toInput();
    } else {
      input["centerCoordinate"] = low_.toInput();
      input["maxDistance"] = std::int64_t{maxDistance_};
    }
    if (limit_ > 0) input["limit"] = limit_;
    if (skip_ > 0) input["skip"] = skip_;
    if (!since_.empty()) input["since"] = since_;
    return input;
  }

 private:
  ChunkRef low_;
  ChunkRef high_;
  int maxDistance_ = 0;
  bool box_ = false;
  std::int64_t limit_ = 0;
  std::int64_t skip_ = 0;
  std::string since_;
};

/// client.chunks() — durable chunk storage. `voxels` blobs are base64 4096-
/// byte dense grids (index x + y*16 + z*256). `get` and `byDistance` also
/// select each chunk's `voxelStates`: the states stored with it and every voxel
/// edit recorded for it (a hub's or mod's `world.set_voxels`, `updateVoxel`,
/// realtime voxel updates), which `voxels` does not hold. Apply each entry's
/// `voxelType` over the grid, as ChunkStore does.
class ChunksAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view appId, const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    return execUnwrap(gen::chunks::kGetChunkDocument, vars);
  }

  void getAsync(std::string_view appId, const ChunkRef& chunk, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    execUnwrapAsync(gen::chunks::kGetChunkDocument, vars, {}, std::move(cb));
  }

  /// The chunk's level-of-detail meshes at `lodLevels`, which the schema requires
  /// (`[Int!]!`): pass the levels to read, e.g. `graphql::JVal::array({0, 1})`.
  graphql::Json getLods(std::string_view appId, const ChunkRef& chunk,
                        const graphql::JVal& lodLevels) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    vars["input"]["lodLevels"] = lodLevels;
    return execUnwrap(gen::chunks::kGetChunkLodsDocument, vars);
  }

  void getLodsAsync(std::string_view appId, const ChunkRef& chunk, const graphql::JVal& lodLevels,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    vars["input"]["lodLevels"] = lodLevels;
    execUnwrapAsync(gen::chunks::kGetChunkLodsDocument, vars, {}, std::move(cb));
  }

  /// Bulk-load the stored chunks within `maxDistance` of `center` (Chebyshev, 1-8),
  /// with their `voxelStates`: at most `limit` of them, from `skip` (0 for either:
  /// the server's default, 1,000 chunks from the first). Range 8 holds up to 4,913
  /// chunks, so a caller that needs all of them asks for that many, or pages on by
  /// the `limit` and `skip` the answer echoes.
  graphql::Json byDistance(std::string_view appId, const ChunkRef& center, int maxDistance,
                           std::int64_t limit = 0, std::int64_t skip = 0) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceDocument,
                      byDistanceVars(appId, center, maxDistance, limit, skip));
  }

  void byDistanceAsync(std::string_view appId, const ChunkRef& center, int maxDistance,
                       graphql::GraphQLCallback cb) const {
    byDistanceAsync(appId, center, maxDistance, 0, 0, std::move(cb));
  }

  void byDistanceAsync(std::string_view appId, const ChunkRef& center, int maxDistance,
                       std::int64_t limit, std::int64_t skip, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceDocument,
                    byDistanceVars(appId, center, maxDistance, limit, skip), {}, std::move(cb));
  }

  /// byDistance for readers that draw blocks: each chunk's `coordinates`, `voxels`,
  /// `voxelStates` (`voxelCoord`, `voxelType`, `state`) and `voxelStatesTruncated`,
  /// without its chunk state, LODs, owner or timestamps.
  graphql::Json byDistanceVoxels(std::string_view appId, const ChunkRef& center, int maxDistance,
                                 std::int64_t limit = 0, std::int64_t skip = 0) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceVoxelsDocument,
                      byDistanceVars(appId, center, maxDistance, limit, skip));
  }

  void byDistanceVoxelsAsync(std::string_view appId, const ChunkRef& center, int maxDistance,
                             std::int64_t limit, std::int64_t skip,
                             graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceVoxelsDocument,
                    byDistanceVars(appId, center, maxDistance, limit, skip), {}, std::move(cb));
  }

  /// byDistanceVoxels without the edits' `state` bytes, for a reader that only draws.
  graphql::Json byDistanceVoxelTypes(std::string_view appId, const ChunkRef& center,
                                     int maxDistance, std::int64_t limit = 0,
                                     std::int64_t skip = 0) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceVoxelTypesDocument,
                      byDistanceVars(appId, center, maxDistance, limit, skip));
  }

  void byDistanceVoxelTypesAsync(std::string_view appId, const ChunkRef& center, int maxDistance,
                                 std::int64_t limit, std::int64_t skip,
                                 graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceVoxelTypesDocument,
                    byDistanceVars(appId, center, maxDistance, limit, skip), {}, std::move(cb));
  }

  /// The stored chunks of `area` (a cube or a box, a page of them, optionally only those
  /// written since a time), with their `voxelStates` as byDistance reads them. The answer's
  /// `limit` and `skip` are the ones applied.
  graphql::Json region(std::string_view appId, const ChunkRegion& area) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceDocument, regionVars(appId, area));
  }

  void regionAsync(std::string_view appId, const ChunkRegion& area,
                   graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceDocument, regionVars(appId, area), {},
                    std::move(cb));
  }

  /// region() with byDistanceVoxels' selection, for readers that draw blocks.
  graphql::Json regionVoxels(std::string_view appId, const ChunkRegion& area) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceVoxelsDocument, regionVars(appId, area));
  }

  void regionVoxelsAsync(std::string_view appId, const ChunkRegion& area,
                         graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceVoxelsDocument, regionVars(appId, area),
                    {}, std::move(cb));
  }

  /// region() with byDistanceVoxelTypes' selection: no edit `state` bytes.
  graphql::Json regionVoxelTypes(std::string_view appId, const ChunkRegion& area) const {
    return execUnwrap(gen::chunks::kGetChunksByDistanceVoxelTypesDocument,
                      regionVars(appId, area));
  }

  void regionVoxelTypesAsync(std::string_view appId, const ChunkRegion& area,
                             graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceVoxelTypesDocument,
                    regionVars(appId, area), {}, std::move(cb));
  }

  graphql::Json voxelList(std::string_view appId, const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    return execUnwrap(gen::chunks::kGetVoxelListDocument, vars);
  }

  void voxelListAsync(std::string_view appId, const ChunkRef& chunk,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    execUnwrapAsync(gen::chunks::kGetVoxelListDocument, vars, {}, std::move(cb));
  }

  /// Persist a chunk (input: appId, chunk, voxels base64, optional state).
  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkDocument, vars);
  }

  void updateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::chunks::kUpdateChunkDocument, vars, {}, std::move(cb));
  }

  /// update(), blocking and never throwing: the whole outcome (`data` is the
  /// response's `data`), so a caller can branch on the refusal in either build.
  graphql::GraphQLOutcome updateOutcome(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return gql_->requestOutcome(gen::chunks::kUpdateChunkDocument, vars);
  }

  graphql::Json updateState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkStateDocument, vars);
  }

  void updateStateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::chunks::kUpdateChunkStateDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateLods(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkLodsDocument, vars);
  }

  void updateLodsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::chunks::kUpdateChunkLodsDocument, vars, {}, std::move(cb));
  }

 private:
  static graphql::JVal byDistanceVars(std::string_view appId, const ChunkRef& center,
                                      int maxDistance, std::int64_t limit, std::int64_t skip) {
    return regionVars(appId, ChunkRegion::cube(center, maxDistance).limit(limit).skip(skip));
  }

  static graphql::JVal regionVars(std::string_view appId, const ChunkRegion& area) {
    graphql::JVal vars;
    vars["input"] = area.toInput(appId);
    return vars;
  }
};

/// client.voxels() — voxel reads, durable updates, history + moderation.
class VoxelsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json list(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kListVoxelsDocument, vars);
  }

  void listAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::voxels::kListVoxelsDocument, vars, {}, std::move(cb));
  }

  graphql::Json listByDistance(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kListVoxelUpdatesByDistanceDocument, vars);
  }

  void listByDistanceAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::voxels::kListVoxelUpdatesByDistanceDocument, vars, {}, std::move(cb));
  }

  /// listByDistance over a ChunkRegion: the recorded edits of its chunks, grouped per chunk and
  /// paged over chunks. It reads the edit log alone, so it returns edits in chunks whose grid was
  /// never stored. For a box the answer's `centerCoordinate` is its middle chunk.
  graphql::Json listInRegion(std::string_view appId, const ChunkRegion& area) const {
    return listByDistance(area.toInput(appId));
  }

  void listInRegionAsync(std::string_view appId, const ChunkRegion& area,
                         graphql::GraphQLCallback cb) const {
    listByDistanceAsync(area.toInput(appId), std::move(cb));
  }

  /// Durable single-voxel update (requires update_voxel_data permission).
  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kUpdateVoxelDocument, vars);
  }

  void updateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::voxels::kUpdateVoxelDocument, vars, {}, std::move(cb));
  }

  /// Entries of the immutable edit history (`voxelUpdateHistory`). `args` are the
  /// operation's own variables, sent as they are: `appId`, and optionally `userId`,
  /// `from`, `to`, `limit` and `offset`.
  graphql::Json history(const graphql::JVal& args) const {
    return execUnwrap(gen::voxels::documentFor("VoxelUpdateHistory"), args,
                      "VoxelUpdateHistory");
  }

  void historyAsync(const graphql::JVal& args, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::voxels::documentFor("VoxelUpdateHistory"), args, "VoxelUpdateHistory",
                    std::move(cb));
  }

  /// Relay cursor pagination variant of history().
  graphql::Json historyConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::voxels::documentFor("VoxelUpdateHistoryConnection"), vars,
                      "VoxelUpdateHistoryConnection");
  }

  void historyConnectionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::voxels::documentFor("VoxelUpdateHistoryConnection"), vars, "VoxelUpdateHistoryConnection",
                    std::move(cb));
  }

  /// Moderation: revert a region/user's voxel edits. Accepts idempotencyKey
  /// inside the input.
  graphql::Json rollback(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kRollbackVoxelUpdatesDocument, vars);
  }

  void rollbackAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::voxels::kRollbackVoxelUpdatesDocument, vars, {}, std::move(cb));
  }
};

/// client.actors() — durable actor records (realtime presence is UDP-side).
class ActorsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view uuid) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    return execUnwrap(gen::actors::kActorDocument, vars);
  }

  void getAsync(std::string_view uuid, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    execUnwrapAsync(gen::actors::kActorDocument, vars, {}, std::move(cb));
  }

  graphql::Json list(const graphql::JVal& filter = graphql::JVal()) const {
    graphql::JVal vars;
    if (!filter.isNull()) vars["filter"] = filter;
    return execUnwrap(gen::actors::documentFor("Actors"), vars, "Actors");
  }

  void listAsync(const graphql::JVal& filter, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    if (!filter.isNull()) vars["filter"] = filter;
    execUnwrapAsync(gen::actors::documentFor("Actors"), vars, "Actors", std::move(cb));
  }

  /// Relay cursor pagination variant of list().
  graphql::Json listConnection(int first = 50, std::string_view after = {},
                               const graphql::JVal& filter = graphql::JVal()) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!filter.isNull()) vars["filter"] = filter;
    return execUnwrap(gen::actors::documentFor("ActorsConnection"), vars, "ActorsConnection");
  }

  void listConnectionAsync(int first, std::string_view after, const graphql::JVal& filter,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!filter.isNull()) vars["filter"] = filter;
    execUnwrapAsync(gen::actors::documentFor("ActorsConnection"), vars, "ActorsConnection", std::move(cb));
  }

  graphql::Json batchLookup(const std::vector<std::string>& uuids) const {
    graphql::JArray arr;
    for (const auto& u : uuids) arr.emplace_back(u);
    graphql::JVal vars;
    vars["input"]["uuids"] = graphql::JVal(std::move(arr));
    return execUnwrap(gen::actors::kBatchLookupActorsDocument, vars);
  }

  void batchLookupAsync(const std::vector<std::string>& uuids, graphql::GraphQLCallback cb) const {
    graphql::JArray arr;
    for (const auto& u : uuids) arr.emplace_back(u);
    graphql::JVal vars;
    vars["input"]["uuids"] = graphql::JVal(std::move(arr));
    execUnwrapAsync(gen::actors::kBatchLookupActorsDocument, vars, {}, std::move(cb));
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::actors::kCreateActorDocument, vars);
  }

  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::actors::kCreateActorDocument, vars, {}, std::move(cb));
  }

  graphql::Json update(std::string_view uuid, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    return execUnwrap(gen::actors::kUpdateActorDocument, vars);
  }

  void updateAsync(std::string_view uuid, const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    execUnwrapAsync(gen::actors::kUpdateActorDocument, vars, {}, std::move(cb));
  }

  /// Destructive; pass a stable idempotencyKey to make retries safe.
  graphql::Json remove(std::string_view uuid, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::actors::kDeleteActorDocument, vars);
  }

  void removeAsync(std::string_view uuid, std::string_view idempotencyKey,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(gen::actors::kDeleteActorDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateState(std::string_view uuid, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    return execUnwrap(gen::actors::kUpdateActorStateDocument, vars);
  }

  void updateStateAsync(std::string_view uuid, const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    execUnwrapAsync(gen::actors::kUpdateActorStateDocument, vars, {}, std::move(cb));
  }
};

/// client.avatars() — characters + per-app avatar state.
class AvatarsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine() const {
    return execUnwrap(gen::avatars::documentFor("MyAvatars"), graphql::JVal(), "MyAvatars");
  }

  void mineAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::avatars::documentFor("MyAvatars"), graphql::JVal(), "MyAvatars", std::move(cb));
  }

  graphql::Json listForUser(std::string_view userId) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    return execUnwrap(gen::avatars::documentFor("UserAvatars"), vars, "UserAvatars");
  }

  void listForUserAsync(std::string_view userId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    execUnwrapAsync(gen::avatars::documentFor("UserAvatars"), vars, "UserAvatars", std::move(cb));
  }

  graphql::Json get(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return execUnwrap(gen::avatars::documentFor("AvatarById"), vars, "AvatarById");
  }

  void getAsync(std::string_view id, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    execUnwrapAsync(gen::avatars::documentFor("AvatarById"), vars, "AvatarById", std::move(cb));
  }

  graphql::Json appState(std::string_view appId, std::string_view avatarId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarId"] = avatarId;
    return execUnwrap(gen::avatars::documentFor("AvatarAppState"), vars, "AvatarAppState");
  }

  void appStateAsync(std::string_view appId, std::string_view avatarId,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarId"] = avatarId;
    execUnwrapAsync(gen::avatars::documentFor("AvatarAppState"), vars, "AvatarAppState", std::move(cb));
  }

  graphql::Json appStates(std::string_view appId, const std::vector<std::string>& avatarIds) const {
    graphql::JArray arr;
    for (const auto& id : avatarIds) arr.emplace_back(id);
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarIds"] = graphql::JVal(std::move(arr));
    return execUnwrap(gen::avatars::documentFor("AvatarAppStates"), vars, "AvatarAppStates");
  }

  void appStatesAsync(std::string_view appId, const std::vector<std::string>& avatarIds,
                      graphql::GraphQLCallback cb) const {
    graphql::JArray arr;
    for (const auto& id : avatarIds) arr.emplace_back(id);
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarIds"] = graphql::JVal(std::move(arr));
    execUnwrapAsync(gen::avatars::documentFor("AvatarAppStates"), vars, "AvatarAppStates", std::move(cb));
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::avatars::documentFor("CreateAvatar"), vars, "CreateAvatar");
  }

  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::avatars::documentFor("CreateAvatar"), vars, "CreateAvatar", std::move(cb));
  }

  graphql::Json update(std::string_view id, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    return execUnwrap(gen::avatars::documentFor("UpdateAvatar"), vars, "UpdateAvatar");
  }

  void updateAsync(std::string_view id, const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    execUnwrapAsync(gen::avatars::documentFor("UpdateAvatar"), vars, "UpdateAvatar", std::move(cb));
  }

  graphql::Json remove(std::string_view id, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["id"] = id;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::avatars::documentFor("DeleteAvatar"), vars, "DeleteAvatar");
  }

  void removeAsync(std::string_view id, std::string_view idempotencyKey,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(gen::avatars::documentFor("DeleteAvatar"), vars, "DeleteAvatar", std::move(cb));
  }

  graphql::Json updateState(std::string_view id, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    return execUnwrap(gen::avatars::documentFor("UpdateAvatarState"), vars, "UpdateAvatarState");
  }

  void updateStateAsync(std::string_view id, const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    execUnwrapAsync(gen::avatars::documentFor("UpdateAvatarState"), vars, "UpdateAvatarState", std::move(cb));
  }

  graphql::Json updateAppState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::avatars::documentFor("UpdateAvatarAppState"), vars, "UpdateAvatarAppState");
  }

  void updateAppStateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::avatars::documentFor("UpdateAvatarAppState"), vars, "UpdateAvatarAppState", std::move(cb));
  }
};

/// client.state() — per-user per-app save blob.
class StateAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json getOne(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::state::kUserAppStateDocument, vars);
  }

  void getOneAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::state::kUserAppStateDocument, vars, {}, std::move(cb));
  }

  graphql::Json getAll() const { return execUnwrap(gen::state::kUserAppStatesDocument); }

  void getAllAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::state::kUserAppStatesDocument, graphql::JVal(), {}, std::move(cb));
  }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::state::kUpdateUserAppStateDocument, vars);
  }

  void updateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::state::kUpdateUserAppStateDocument, vars, {}, std::move(cb));
  }

  graphql::Json remove(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::state::kDeleteUserAppStateDocument, vars);
  }

  void removeAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::state::kDeleteUserAppStateDocument, vars, {}, std::move(cb));
  }
};

/// client.host() — host election reads + actor liveness heartbeat. The
/// election is informational; authoritative gating uses the game model's
/// is_host invoke policy.
class HostAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::documentFor("GameHost"), vars, "GameHost");
  }

  void getAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::host::documentFor("GameHost"), vars, "GameHost", std::move(cb));
  }

  bool amIHost(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::documentFor("AmIGameHost"), vars, "AmIGameHost").asBool();
  }

  void amIHostAsync(std::string_view appId,
                    std::function<void(graphql::GraphQLOutcome outcome, bool amHost)> cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::host::documentFor("AmIGameHost"), vars, "AmIGameHost",
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool amHost = false;
                      if (out.ok()) amHost = out.data.asBool();
                      cb(std::move(out), amHost);
                    });
  }

  /// Send ~every 3 s while connected to stay host-eligible; returns the
  /// current host.
  graphql::Json heartbeat(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::documentFor("ActorHeartbeat"), vars, "ActorHeartbeat");
  }

  void heartbeatAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::host::documentFor("ActorHeartbeat"), vars, "ActorHeartbeat", std::move(cb));
  }
};

/// client.teleport() — teleport requests.
class TeleportAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json request(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teleport::kTeleportRequestDocument, vars);
  }

  void requestAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teleport::kTeleportRequestDocument, vars, {}, std::move(cb));
  }
};

/// client.inputLog() — the input log: the client inputs the realtime servers recorded for an
/// app with replay logging on (`App.replayLoggingEnabled`, set with admin().apps().update,
/// which answers INPUT_LOG_FUNDS_NEEDED, with the balance it needs, unless the org wallet can
/// keep one retention period of the app's recent traffic: graphql::inputLogFundsNeededOf).
///
/// Game plane: call it on the app-scoped client for the app; an identity session token is
/// refused. Every player reads the sessions and inputs they sent: on an app with the safety
/// log, that is the player's own records (sessions() without a filter, then messages() for one
/// of them). A moderator (moderate_safety: an appointed moderator or the organization's
/// admins, ck-api v2.60.0) reads only the sessions an open or escalated safety report covers
/// (client.moderation().reports(appId), each report's `sessions`; `filter.userId` narrows
/// them). manage_apps reads every session and may filter by `userId` only on an app for adults
/// (effective minimum age 18), and never a player under 18's (ck-api v2.61.0). Every read of
/// another player's session is audited, and on an app that admits players under 18 every read
/// is, the player's own included. Inputs are kept for the input log's published retention, so
/// an old session can still be listed after its inputs are gone. INPUT_LOG_UNAVAILABLE on a
/// deployment without input logging.
///
/// Recording is best-effort, and a session says what it lost: `missingRecords` counts the
/// inputs the replication server accepted that never reached the log (0 when the recording is
/// complete as far as is known; null on one recorded before the count existed). A lost end
/// cannot be counted: such a session closes as `unrecorded`.
///
/// From the Game API release after v2.40.2, messages() answers
/// INPUT_LOG_TEMPORARILY_UNAVAILABLE when the log cannot be read right now and
/// INPUT_LOG_RATE_LIMITED when a read waited out its few-second deadline behind earlier ones
/// (a server runs one read per user, and two per app and per org, at a time). Both carry
/// `extensions.retryable`: retry with the same cursor after a short back-off, a bounded number
/// of times (graphql::inputLogReadRetryable). A read nobody will see should be cancelled
/// with a graphql::CancelToken (the overloads below), which closes the request and frees its
/// share; the client's timeout still applies.
///
/// \code
/// crowdy::graphql::CancelToken reading;  // cancel() when the user moves on
/// std::string after;
/// for (int retries = 0, backoffMs = 250;;) {
///   crowdy::graphql::Json page;
///   try {
///     page = client.inputLog().messages(appId, gameTokenId, 200, after, {}, reading);
///   } catch (const crowdy::graphql::CrowdyGraphQLError& e) {
///     if (!crowdy::graphql::inputLogReadRetryable(e.errors()) || ++retries > 8) throw;
///     std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
///     backoffMs = std::min(backoffMs * 2, 5000);
///     continue;  // the same cursor: nothing is skipped
///   }
///   retries = 0;
///   backoffMs = 250;
///   for (std::size_t i = 0; i < page["edges"].size(); ++i) handle(page["edges"].at(i)["node"]);
///   if (!page["pageInfo"]["hasNextPage"].asBool()) break;
///   if (page["pageInfo"]["endCursor"].isString()) after = page["pageInfo"]["endCursor"].asString();
/// }
/// \endcode
class InputLogAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Recorded sessions of an app, newest first: an InputLogSessionConnection
  /// (`edges[].node`, `pageInfo.endCursor`, `totalCount`). `filter` is an
  /// InputLogSessionFilter: `userId` (another user's needs manage_apps on an app for adults,
  /// or moderate_safety for a reported session; else FORBIDDEN), `from` / `to`, `messageType`.
  /// Without a filter, a player lists its own sessions. A node's `endReason` is `expired`, `revoked`,
  /// `reconnect` or `released` as the client saw it; `logging_off` (replay logging was turned
  /// off), `shutdown` (the replication server recording it stopped; the client reconnects
  /// elsewhere, the session continues and its end is replaced) or `paused` (recording paused
  /// while the app's logging setting could not be confirmed; a later record reopens the
  /// session), from the Game API release after v2.40.2; `unrecorded` (no end was recorded,
  /// closed an hour after its last input); or null while the session may still run.
  graphql::Json sessions(std::string_view appId, int first = 50, std::string_view after = {},
                         const graphql::JVal& filter = graphql::JVal()) const {
    return execUnwrap(gen::inputLog::kInputLogSessionsDocument,
                      sessionVars(appId, first, after, filter));
  }

  /// sessions(), cancellable with `cancel` (CrowdyCancelledError once cancelled).
  graphql::Json sessions(std::string_view appId, int first, std::string_view after,
                         const graphql::JVal& filter, const graphql::CancelToken& cancel) const {
    return execUnwrap(gen::inputLog::kInputLogSessionsDocument,
                      sessionVars(appId, first, after, filter), {}, withCancel(cancel));
  }

  void sessionsAsync(std::string_view appId, int first, std::string_view after,
                     const graphql::JVal& filter, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::inputLog::kInputLogSessionsDocument,
                    sessionVars(appId, first, after, filter), {}, std::move(cb));
  }

  /// sessionsAsync(), cancellable with `cancel` (delivered as GraphQLErrorKind::Cancelled).
  void sessionsAsync(std::string_view appId, int first, std::string_view after,
                     const graphql::JVal& filter, const graphql::CancelToken& cancel,
                     graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::inputLog::kInputLogSessionsDocument,
                    sessionVars(appId, first, after, filter), {}, withCancel(cancel),
                    std::move(cb));
  }

  /// The recorded inputs of one session (`gameTokenId`, from sessions()), oldest first: an
  /// InputLogMessageConnection. `body` is the message in base64, from its type byte up to
  /// its authentication tail, which is not recorded; `sizeBytes` is what stored input logs
  /// are billed on. KEEP PAGING WHILE pageInfo.hasNextPage IS TRUE, not while `endCursor` is
  /// set: a page can hold fewer than `first` inputs, or none, when it reached the server's
  /// time or scan limit; keep the previous cursor if one comes back null. `filter` is an
  /// InputLogMessageFilter: `from` / `to`, `messageTypes` (at most 64). NOT_FOUND for a
  /// session that is not yours, unless an open or escalated report covers it and you hold
  /// moderate_safety, or (ck-api v2.61.0) the app is for adults, the session is not a player
  /// under 18's and you hold manage_apps; BAD_USER_INPUT for a malformed cursor or, from the
  /// Game API release after v2.40.2, a cursor from another session.
  graphql::Json messages(std::string_view appId, std::string_view gameTokenId, int first = 50,
                         std::string_view after = {},
                         const graphql::JVal& filter = graphql::JVal()) const {
    return execUnwrap(gen::inputLog::kInputLogMessagesDocument,
                      messageVars(appId, gameTokenId, first, after, filter));
  }

  /// messages(), cancellable with `cancel` (CrowdyCancelledError once cancelled).
  graphql::Json messages(std::string_view appId, std::string_view gameTokenId, int first,
                         std::string_view after, const graphql::JVal& filter,
                         const graphql::CancelToken& cancel) const {
    return execUnwrap(gen::inputLog::kInputLogMessagesDocument,
                      messageVars(appId, gameTokenId, first, after, filter), {},
                      withCancel(cancel));
  }

  void messagesAsync(std::string_view appId, std::string_view gameTokenId, int first,
                     std::string_view after, const graphql::JVal& filter,
                     graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::inputLog::kInputLogMessagesDocument,
                    messageVars(appId, gameTokenId, first, after, filter), {}, std::move(cb));
  }

  /// messagesAsync(), cancellable with `cancel` (delivered as GraphQLErrorKind::Cancelled).
  void messagesAsync(std::string_view appId, std::string_view gameTokenId, int first,
                     std::string_view after, const graphql::JVal& filter,
                     const graphql::CancelToken& cancel, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::inputLog::kInputLogMessagesDocument,
                    messageVars(appId, gameTokenId, first, after, filter), {},
                    withCancel(cancel), std::move(cb));
  }

 private:
  static graphql::RequestOptions withCancel(const graphql::CancelToken& cancel) {
    graphql::RequestOptions options;
    options.cancel = cancel;
    return options;
  }

  static graphql::JVal sessionVars(std::string_view appId, int first, std::string_view after,
                                   const graphql::JVal& filter) {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!filter.isNull()) vars["filter"] = filter;
    return vars;
  }

  static graphql::JVal messageVars(std::string_view appId, std::string_view gameTokenId,
                                   int first, std::string_view after,
                                   const graphql::JVal& filter) {
    graphql::JVal vars = sessionVars(appId, first, after, filter);
    vars["gameTokenId"] = gameTokenId;
    return vars;
  }
};

}  // namespace crowdy::domains
