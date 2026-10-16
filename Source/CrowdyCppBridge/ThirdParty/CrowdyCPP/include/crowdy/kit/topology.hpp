#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/json.hpp"

/// Composing and deploying an app's tree of starter hubs. CrowdyJS `src/kit/topology.ts`.
namespace crowdy::kit {

/// The crates one build takes at most (the game API's `execBuild`).
inline constexpr std::size_t kKitBuildMaxCrates = 16;

/// The node API scopes a manifest may declare (ck-exec `bounds::NODE_API_SCOPES`).
inline constexpr std::array<std::string_view, 9> kKitNodeApiScopes = {
    "players.read", "players.write", "world.read",   "world.write",  "grids.read",
    "permissions.write", "grids.write", "access.read", "access.write",
};

/// What the execution manager would refuse in `manifest` (`{ root, types }`), in its words
/// (ck-exec `Manifest::problems`, less the digests, which the build fills in): the root is a hub
/// without a parent, every other type has a defined parent, `calls` name defined types or `*`,
/// replicas are for spokes, scopes are known. Empty when it deploys.
std::vector<std::string> kitManifestProblems(const graphql::Json& manifest);

/// What KitTopology::plan composes: a manifest, and the starter crates it builds.
struct KitTopologyPlan {
  std::string root;
  /// The types, by name, as the manifest takes them.
  std::map<std::string, graphql::JVal> types;
  /// The starter crates the types name, each built once.
  std::vector<std::string> crates;
  /// Why it would not deploy; empty when it would.
  std::vector<std::string> problems;

  bool ok() const { return problems.empty(); }
  /// The plan as a manifest, `{ root, types }`.
  graphql::Json manifest() const;
};

/// What KitTopology::deploy did.
struct KitDeployment {
  bool ok = false;
  std::int64_t version = 0;
  std::string buildId;
  /// Why it did not deploy (a failed build's log ends it).
  std::string error;
  /// The build as it ended.
  graphql::Json build;
};

/// An app's tree of kit hubs, composed from the starter packs (`ExecAPI::starters`): pick the
/// node types a game uses, and plan() adds the hubs they hang under and the ones they call and
/// checks the tree; deploy() builds the crates on the platform and deploys them. For the app's
/// developers (the org's `manage_compute`), from an identity session. Blocking, as ExecAPI's
/// blocking calls are, and refused the same way.
class KitTopology {
 public:
  KitTopology(std::string appId, domains::ExecAPI* exec) : appId_(std::move(appId)), exec_(exec) {}

  /// The starter packs this tier's toolchain carries: `{ manifestJson, starters }` (fetched once).
  graphql::Json pack();
  /// The node types the packs offer.
  std::vector<std::string> types();
  /// A manifest with the types in `select`, the types they hang under up to the root, and the
  /// types they call (by name, not `*`), with `overrides` merged into each named type (a seed,
  /// limits, scopes, `client`, ...).
  KitTopologyPlan plan(const std::vector<std::string>& select, const std::map<std::string, graphql::JVal>& overrides = {});
  /// Builds the plan's crates on the platform, waits for the build, and deploys the manifest with it.
  KitDeployment deploy(const KitTopologyPlan& plan, int timeoutMs = 600000);

 private:
  std::string appId_;
  domains::ExecAPI* exec_;
  std::optional<graphql::Json> pack_;
};

}  // namespace crowdy::kit
