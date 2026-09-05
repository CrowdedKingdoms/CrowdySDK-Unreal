#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerCompute() — player-authored Rust/WASM bound to player-owned
/// grids. Deploying requires current ownership plus target-specific write
/// permission at both app-tier and grid ACL layers. Enabling separately
/// requires run permission, successful compilation, and admission when the app
/// uses strict allow-list mode. Targets the Game API.
namespace crowdy::domains {

class PlayerComputeAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;
  using ArtifactBytesCallback = std::function<void(
      graphql::GraphQLOutcome, ClientArtifactBytes)>;

  /// Create/update a grid-bound module and publish an immutable pending source
  /// version. Compilation is asynchronous.
  graphql::Json deploy(const graphql::JVal& input) const {
    return byInput("PlayerComputeDeploy", input);
  }
  void deployAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerComputeDeploy", input, std::move(cb));
  }

  /// Request activation or stop execution. Enabling checks ownership, target
  /// run permission, compile success, and app admission.
  graphql::Json setEnabled(std::string_view appId, std::string_view gridId,
                           std::string_view name, bool enabled) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    return run("PlayerComputeSetEnabled", vars);
  }
  void setEnabledAsync(std::string_view appId, std::string_view gridId,
                       std::string_view name, bool enabled,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    runAsync("PlayerComputeSetEnabled", vars, std::move(cb));
  }

  /// Set or clear the required CLIENT companion for the current immutable
  /// SERVER version. Empty requiredClientName clears the edge.
  graphql::Json setRequires(std::string_view appId, std::string_view gridId,
                            std::string_view serverName,
                            std::string_view requiredClientName = "") const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["serverName"] = serverName;
    if (!requiredClientName.empty()) {
      vars["requiredClientName"] = requiredClientName;
    }
    return run("PlayerComputeSetRequires", vars);
  }
  void setRequiresAsync(std::string_view appId, std::string_view gridId,
                        std::string_view serverName,
                        std::string_view requiredClientName,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["serverName"] = serverName;
    if (!requiredClientName.empty()) {
      vars["requiredClientName"] = requiredClientName;
    }
    runAsync("PlayerComputeSetRequires", vars, std::move(cb));
  }

  /// List modules authored by the caller or installed on grids they currently
  /// own. Closed source is not returned by this module-level operation.
  graphql::Json myModules(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeMyModules", vars);
  }
  void myModulesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeMyModules", vars, std::move(cb));
  }

  /// List immutable versions newest-first. Source and compile logs are
  /// redacted unless the caller is the personal author or source is open.
  graphql::Json versions(std::string_view appId, std::string_view gridId,
                         std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    return run("PlayerComputeVersions", vars);
  }
  void versionsAsync(std::string_view appId, std::string_view gridId,
                     std::string_view name, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    runAsync("PlayerComputeVersions", vars, std::move(cb));
  }

  /// Delete a self-authored module and its versions. The caller must still own
  /// the grid. Returns false when no matching module exists.
  graphql::Json remove(std::string_view appId, std::string_view gridId,
                       std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    return run("PlayerComputeDelete", vars);
  }
  void removeAsync(std::string_view appId, std::string_view gridId,
                   std::string_view name, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    runAsync("PlayerComputeDelete", vars, std::move(cb));
  }

  /// Synchronously invoke an enabled/admitted server module as the grid owner.
  graphql::Json invoke(std::string_view appId, std::string_view gridId,
                       std::string_view moduleName,
                       std::string_view exportName,
                       std::string_view paramsJson = "{}") const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["moduleName"] = moduleName;
    vars["exportName"] = exportName;
    vars["paramsJson"] = paramsJson;
    return run("PlayerComputeInvoke", vars);
  }
  void invokeAsync(std::string_view appId, std::string_view gridId,
                   std::string_view moduleName,
                   std::string_view exportName,
                   std::string_view paramsJson,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["moduleName"] = moduleName;
    vars["exportName"] = exportName;
    vars["paramsJson"] = paramsJson;
    runAsync("PlayerComputeInvoke", vars, std::move(cb));
  }

  /// The caller's spend/quota view for one app (P2): current hour/day compute
  /// units vs the effective policy caps, compile-quota utilization, and the
  /// wallet/spend-cap gate state with its typed reason.
  graphql::Json usage(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeUsage", vars);
  }
  void usageAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeUsage", vars, std::move(cb));
  }

  /// Executions on an owned grid, newest first (attributed to the grid owner).
  graphql::Json runs(std::string_view appId, std::string_view gridId,
                     const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerComputeRuns", vars);
  }
  void runsAsync(std::string_view appId, std::string_view gridId,
                 const graphql::JVal& options,
                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("PlayerComputeRuns", vars, std::move(cb));
  }

  /// Failed-run diagnostics on an owned grid, newest first.
  graphql::Json logs(std::string_view appId, std::string_view gridId,
                     const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerComputeLogs", vars);
  }
  void logsAsync(std::string_view appId, std::string_view gridId,
                 const graphql::JVal& options,
                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("PlayerComputeLogs", vars, std::move(cb));
  }

  /// Throw or release a kill-ladder switch at player/grid/app scope (studio,
  /// requires manage_compute). Quota state is retained across a kill.
  graphql::Json setSwitch(std::string_view appId, std::string_view scope,
                          bool disabled,
                          const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    vars["disabled"] = disabled;
    return run("PlayerComputeSetSwitch", vars);
  }
  void setSwitchAsync(std::string_view appId, std::string_view scope,
                      bool disabled, const graphql::JVal& options,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    vars["disabled"] = disabled;
    runAsync("PlayerComputeSetSwitch", vars, std::move(cb));
  }

  /// Active kill-ladder switches (studio, requires view_compute_diagnostics).
  graphql::Json switches(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeSwitches", vars);
  }
  void switchesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeSwitches", vars, std::move(cb));
  }

  /// Fetch a compiled CLIENT artifact + metadata (P3). Fail-closed server-side
  /// (ownership, authorship, run_client_code, admission). Native clients get
  /// the metadata + gas-injected bytes; the browser broker itself is
  /// JS-only (04 §7), so CrowdyCPP wraps the fetch surface, not a sandbox.
  graphql::Json artifact(std::string_view appId, std::string_view gridId,
                         std::string_view name,
                         std::string_view versionId = "") const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    if (!versionId.empty()) vars["versionId"] = versionId;
    return run("PlayerComputeArtifact", vars);
  }
  void artifactAsync(std::string_view appId, std::string_view gridId,
                     std::string_view name, std::string_view versionId,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    if (!versionId.empty()) vars["versionId"] = versionId;
    runAsync("PlayerComputeArtifact", vars, std::move(cb));
  }

  /// Fetch and base64-decode a CLIENT artifact for a native sandbox/runtime.
  /// fuelPerDispatch remains a decimal string to preserve GraphQL BigInt.
  ClientArtifactBytes artifactBytes(
      std::string_view appId, std::string_view gridId,
      std::string_view name, std::string_view versionId = "") const {
    return requireArtifactBytes(artifact(appId, gridId, name, versionId));
  }
  void artifactBytesAsync(
      std::string_view appId, std::string_view gridId,
      std::string_view name, std::string_view versionId,
      ArtifactBytesCallback cb) const {
    artifactAsync(
        appId, gridId, name, versionId,
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          ClientArtifactBytes decoded;
          if (outcome.ok()) {
            auto value = decodeClientArtifactBytes(outcome.data);
            if (value) {
              decoded = std::move(*value);
            } else {
              outcome.status = Errc::Malformed;
              outcome.kind = graphql::GraphQLErrorKind::Protocol;
              outcome.errorMessage =
                  "playerComputeArtifact returned invalid artifact bytes";
            }
          }
          cb(std::move(outcome), std::move(decoded));
        });
  }
  void artifactBytesAsync(
      std::string_view appId, std::string_view gridId,
      std::string_view name, ArtifactBytesCallback cb) const {
    artifactBytesAsync(appId, gridId, name, {}, std::move(cb));
  }

 private:
  static ClientArtifactBytes requireArtifactBytes(
      const graphql::Json& artifact) {
    auto decoded = decodeClientArtifactBytes(artifact);
    if (decoded) return std::move(*decoded);
#ifndef CROWDY_NO_EXCEPTIONS
    throw graphql::CrowdyProtocolError(
        "playerComputeArtifact returned invalid artifact bytes");
#else
    return {};
#endif
  }

  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerCompute::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::playerCompute::documentFor(op), vars, op,
                    std::move(cb));
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

}  // namespace crowdy::domains
