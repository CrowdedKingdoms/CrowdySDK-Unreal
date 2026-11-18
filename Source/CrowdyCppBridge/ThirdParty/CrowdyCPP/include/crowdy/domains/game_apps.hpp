#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
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

  /// Player-safe overlapping grids: id, bounds and the caller's own unexpired
  /// `permissionKeys` on each (ck-api v2.67.0; bounds only before). No userId, so no
  /// impersonation. The grid that decides a voxel edit in a chunk is the smallest box
  /// covering it (the lower gridId on a tie), and the edit needs `update_voxel_data`
  /// there. Requires an app-scoped token (ck-api v1.93.0).
  graphql::Json nearbyGrids(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("NearbyGrids", vars);
  }

  void nearbyGridsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("NearbyGrids", vars, std::move(cb));
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

  /// The keys a grid grants every player with active access to the app (an open build
  /// area, a public arena); empty when the grid is not open. Requires manage_apps.
  graphql::Json openPermissions(std::string_view appId, std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("GridOpenPermissions", vars);
  }

  void openPermissionsAsync(std::string_view appId, std::string_view gridId,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("GridOpenPermissions", vars, std::move(cb));
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
  /// Open a grid to every player: replace the keys it grants each player with active access
  /// (`{appId, gridId, permissionKeys}`, within the grid's limits; players who gain access
  /// later get them too), then recompute the effective ACL. The most specific grid covering a
  /// chunk decides a voxel write there, so a zone everyone may build in grants
  /// `update_voxel_data` itself. An empty `permissionKeys` closes it. BAD_REQUEST refuses the
  /// app's world grid, the four player-code keys, an inactive key and a 33rd open grid.
  /// Requires manage_apps.
  graphql::Json setOpenPermissions(const graphql::JVal& input) const {
    return byInput("SetGridOpenPermissions", input);
  }
  void setOpenPermissionsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("SetGridOpenPermissions", input, std::move(cb));
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

  /// The countries Crowded Kingdoms lists (ck-api v2.60.0): those it does not serve ("blocked":
  /// sign-up and play are refused there with TERRITORY_NOT_SUPPORTED, by the country a player
  /// gives or connects from), and those with a minimum age above the platform's. A country not
  /// listed is open at the platform's minimum. Since ck-api v2.61.0 a row may be for one region
  /// of a country (`regionCode`, ISO 3166-2, such as UA-43; null on a country's own row): a
  /// country with a region listed is checked by region, so a connection whose region cannot be
  /// told is refused there, and an organization sends its players' `region`. Public, so a
  /// sign-up form can use it before an account exists; a sign-up screen names no age.
  std::vector<TerritoryPolicy> territoryPolicies() const {
    std::vector<TerritoryPolicy> out;
    execUnwrap(gen::platform::kTerritoryPoliciesDocument).forEach([&](graphql::Json p) {
      out.push_back(TerritoryPolicy::fromJson(p));
    });
    return out;
  }

  void territoryPoliciesAsync(
      std::function<void(graphql::GraphQLOutcome, std::vector<TerritoryPolicy>)> cb) const {
    execUnwrapAsync(gen::platform::kTerritoryPoliciesDocument, graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      std::vector<TerritoryPolicy> value;
                      if (out.ok()) {
                        out.data.forEach([&](graphql::Json p) {
                          value.push_back(TerritoryPolicy::fromJson(p));
                        });
                      }
                      cb(std::move(out), std::move(value));
                    });
  }

  void configAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::platform::kPlatformConfigDocument, graphql::JVal(), {}, std::move(cb));
  }
};

}  // namespace crowdy::domains
