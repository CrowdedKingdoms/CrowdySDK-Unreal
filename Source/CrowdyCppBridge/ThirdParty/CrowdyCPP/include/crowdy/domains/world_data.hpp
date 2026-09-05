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

/// client.chunks() — durable chunk storage. `voxels` blobs are base64 4096-
/// byte dense grids (index x + y*16 + z*256).
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

  graphql::Json getLods(std::string_view appId, const ChunkRef& chunk,
                        const graphql::JVal& lodLevels = graphql::JVal()) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    if (!lodLevels.isNull()) vars["input"]["lodLevels"] = lodLevels;
    return execUnwrap(gen::chunks::kGetChunkLodsDocument, vars);
  }

  void getLodsAsync(std::string_view appId, const ChunkRef& chunk, const graphql::JVal& lodLevels,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["coordinates"] = chunk.toInput();
    if (!lodLevels.isNull()) vars["input"]["lodLevels"] = lodLevels;
    execUnwrapAsync(gen::chunks::kGetChunkLodsDocument, vars, {}, std::move(cb));
  }

  /// Bulk-load every stored chunk within `maxDistance` of `center`
  /// (Chebyshev, 1-8).
  graphql::Json byDistance(std::string_view appId, const ChunkRef& center, int maxDistance) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["centerCoordinate"] = center.toInput();
    vars["input"]["maxDistance"] = std::int64_t{maxDistance};
    return execUnwrap(gen::chunks::kGetChunksByDistanceDocument, vars);
  }

  void byDistanceAsync(std::string_view appId, const ChunkRef& center, int maxDistance,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["centerCoordinate"] = center.toInput();
    vars["input"]["maxDistance"] = std::int64_t{maxDistance};
    execUnwrapAsync(gen::chunks::kGetChunksByDistanceDocument, vars, {}, std::move(cb));
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

  graphql::Json history(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::documentFor("VoxelUpdateHistory"), vars,
                      "VoxelUpdateHistory");
  }

  void historyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::voxels::documentFor("VoxelUpdateHistory"), vars, "VoxelUpdateHistory",
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

}  // namespace crowdy::domains
