#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/models.hpp"

namespace crowdy::domains {
class CrowdyStudioAPI;
class ExecAPI;
}  // namespace crowdy::domains

namespace crowdy::studio {

/// crowdy-client-sdk, what a ck-exec mod's CLIENT half builds on. The build points the
/// dependency at the platform's own copy, so this is the version a starter names.
inline constexpr std::string_view kCrowdyStudioClientSdkVersion = "0.1.0";

struct CrowdyStudioStarterFile {
  std::string path;
  std::string content;
};

/// ck-exec's mod starter crate (`execModStarter`): `Cargo.toml` and `src/**` of a
/// `ckx-sdk` crate.
struct CrowdyStudioModStarter {
  std::vector<CrowdyStudioStarterFile> files;
};

/// The files of `exec().modStarter(appId)`'s answer.
CrowdyStudioModStarter crowdyStudioModStarterOf(const graphql::Json& starter);

struct CrowdyStudioNewProjectOptions {
  std::string appId;
  std::string gridId;
  std::string name;
  CrowdyStudioProjectKind kind = CrowdyStudioProjectKind::Server;
  std::optional<std::string> description;
  /// The SERVER target's crate; required when the kind has a SERVER target.
  std::optional<CrowdyStudioModStarter> modStarter;
  /// The CLIENT target's crate (`exec().modClientStarter(appId)`, Game API v2.73.0), its
  /// package renamed to the CLIENT module. Without it the CLIENT target is
  /// crowdyStudioClientStarterFiles, the same crate.
  std::optional<CrowdyStudioModStarter> clientStarter;
};

/// A ck-exec mod's CLIENT half: one crowdy-client-sdk crate (`Cargo.toml` and
/// `src/lib.rs`) within the build's rules, its package named `moduleName`. CrowdyJS's
/// CLIENT starter byte for byte (`tools/parity/fixtures/crowdy-studio-starter.v1.json`).
std::vector<CrowdyStudioProjectFile> crowdyStudioClientStarterFiles(
    std::string_view moduleName);

/// CrowdyJS's createCrowdyStudioStarterProject. The project is a ck-exec mod: the SERVER
/// target is the mod starter's crate with its package renamed, the CLIENT target
/// `clientStarter` renamed the same way (or the starter above), the module names
/// `<base>-server` and `<base>-client` (a slug of `name` that fits a mod's 48 characters
/// and starts with a letter), and the pairing `NONE`, since a mod has no client pairing.
/// Throws std::invalid_argument for a SERVER target without a mod starter, or a starter
/// without `Cargo.toml` or `src/lib.rs`.
CreateCrowdyStudioProjectInput createCrowdyStudioStarterProject(
    const CrowdyStudioNewProjectOptions& options);

using CrowdyStudioCreatedCallback =
    std::function<void(graphql::GraphQLOutcome, CrowdyStudioProject)>;

/// Create a starter project without blocking: for a SERVER target the mod starter is
/// fetched first (`modStarterAsync`) and for a CLIENT target the CLIENT half starter
/// (`modClientStarterAsync`), unless `options` carries them, then the project is created
/// with both targets' files (`createProjectAsync`). An API host that answers the CLIENT half
/// starter with HTTP status 503 (its toolchain predates it) leaves the SDK's own copy.
/// `done` runs once, from the client's dispatcher; a fetched starter the project cannot
/// start from arrives as a Protocol outcome. A starter passed without `Cargo.toml` or
/// `src/lib.rs` throws std::invalid_argument at once. `exec` and `projects` must outlive the
/// call (the client's own domains do).
void createCrowdyStudioStarterProjectAsync(domains::ExecAPI& exec,
                                           domains::CrowdyStudioAPI& projects,
                                           CrowdyStudioNewProjectOptions options,
                                           CrowdyStudioCreatedCallback done);

}  // namespace crowdy::studio
