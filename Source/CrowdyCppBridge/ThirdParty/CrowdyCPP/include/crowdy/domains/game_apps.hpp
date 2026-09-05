#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.gameApps() — app grids + grid runtime-permission administration
/// (land claims, zoning, enforced voxel/voice permissions). Targets the Game
/// API. See https://docs.crowdedkingdoms.com/game-api/grids-and-permissions.
namespace crowdy::domains {

class GameAppsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Read the current first-class title record. Returns null when the grid has
  /// no current or unexpired owner. Requires authentication.
  graphql::Json ownership(std::string_view appId, std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("GridOwnership", vars);
  }
  void ownershipAsync(std::string_view appId, std::string_view gridId,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("GridOwnership", vars, std::move(cb));
  }

  /// Bootstrap an unowned grid to one user. Requires manage_apps and assigns
  /// title only; runtime permissions must be granted separately.
  graphql::Json assignOwnership(const graphql::JVal& input) const {
    return byInput("AssignGridOwnership", input);
  }
  void assignOwnershipAsync(const graphql::JVal& input,
                            graphql::GraphQLCallback cb) const {
    byInputAsync("AssignGridOwnership", input, std::move(cb));
  }

  /// Transfer title as current user owner or app admin. Security-sensitive:
  /// disables player modules, wipes state, removes old direct grants, and
  /// grants the new owner no implicit permissions.
  graphql::Json transferOwnership(const graphql::JVal& input) const {
    return byInput("TransferGridOwnership", input);
  }
  void transferOwnershipAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    byInputAsync("TransferGridOwnership", input, std::move(cb));
  }

  graphql::Json userPermissions(std::string_view appId, std::string_view gridId,
                                std::string_view userId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["userId"] = userId;
    return run("GridUserPermissions", vars);
  }

  void userPermissionsAsync(std::string_view appId, std::string_view gridId,
                            std::string_view userId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["userId"] = userId;
    runAsync("GridUserPermissions", vars, std::move(cb));
  }

  graphql::Json nearbyPermissions(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("NearbyGridPermissions", vars);
  }

  void nearbyPermissionsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("NearbyGridPermissions", vars, std::move(cb));
  }

  graphql::Json permissionLimits(std::string_view appId, std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("GridPermissionLimits", vars);
  }

  void permissionLimitsAsync(std::string_view appId, std::string_view gridId,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("GridPermissionLimits", vars, std::move(cb));
  }

  graphql::Json groupGrants(std::string_view appId, std::string_view gridId,
                            std::string_view groupId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["groupId"] = groupId;
    return run("GridGroupGrants", vars);
  }

  void groupGrantsAsync(std::string_view appId, std::string_view gridId, std::string_view groupId,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["groupId"] = groupId;
    runAsync("GridGroupGrants", vars, std::move(cb));
  }

  graphql::Json createGrid(const graphql::JVal& input) const { return byInput("CreateGrid", input); }

  void createGridAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("CreateGrid", input, std::move(cb));
  }

  /// Destructive; input accepts an idempotencyKey.
  graphql::Json deleteGrid(const graphql::JVal& input) const { return byInput("DeleteGrid", input); }

  void deleteGridAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("DeleteGrid", input, std::move(cb));
  }

  graphql::Json grantPermissions(const graphql::JVal& input) const {
    return byInput("GrantGridPermissions", input);
  }
  void grantPermissionsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("GrantGridPermissions", input, std::move(cb));
  }
  graphql::Json revokePermissions(const graphql::JVal& input) const {
    return byInput("RevokeGridPermissions", input);
  }
  void revokePermissionsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("RevokeGridPermissions", input, std::move(cb));
  }
  graphql::Json setPermissionLimits(const graphql::JVal& input) const {
    return byInput("SetGridPermissionLimits", input);
  }
  void setPermissionLimitsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("SetGridPermissionLimits", input, std::move(cb));
  }
  graphql::Json assignGroup(const graphql::JVal& input) const {
    return byInput("AssignGroupToGrid", input);
  }
  void assignGroupAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("AssignGroupToGrid", input, std::move(cb));
  }
  graphql::Json revokeGroup(const graphql::JVal& input) const {
    return byInput("RevokeGroupFromGrid", input);
  }
  void revokeGroupAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("RevokeGroupFromGrid", input, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::gameApps::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameApps::documentFor(op), vars, op, std::move(cb));
  }
  graphql::Json byInput(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run(op, vars);
  }
  void byInputAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync(op, vars, std::move(cb));
  }
};

/// client.platform() — public platform configuration.
class PlatformAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;
  graphql::Json config() const { return execUnwrap(gen::platform::kPlatformConfigDocument); }

  void configAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::platform::kPlatformConfigDocument, graphql::JVal(), {}, std::move(cb));
  }
};

}  // namespace crowdy::domains
