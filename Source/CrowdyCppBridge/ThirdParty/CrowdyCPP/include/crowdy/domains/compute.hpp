#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.compute() — Compute Modules: server-side Rust/WebAssembly logic on
/// the Game API. Studios upload Rust source; the platform compiles it to WASM
/// and runs it sandboxed on the game servers (fuel metering, tick/event/invoke
/// triggers, an app-scoped host data API, replication egress). Modules are
/// SERVER-ONLY — this client manages, invokes, and observes them.
///
/// Authoring mutations require the org `manage_compute` permission; monitoring
/// queries require `view_compute_diagnostics`; `invoke` is gated per-export by
/// its invoke policy (null policy = compute admins only). Requires a
/// cks-game-api build that serves the compute* root fields.
/// See https://docs.crowdedkingdoms.com/game-api/compute-modules and
/// https://docs.crowdedkingdoms.com/game-api/compute-host-api.
namespace crowdy::domains {

/// The crowdy-compute-sdk version the platform toolchain currently pins.
inline constexpr std::string_view kComputeSdkVersion = "0.1.0";
/// The guest ABI version the platform currently supports.
inline constexpr std::int64_t kComputeAbiVersion = 0;

class ComputeAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  // ----- Authoring (requires manage_compute) ---------------------------------

  /// Create or update a module's metadata (upsert key: appId + name). New
  /// modules start disabled.
  ///
  /// NOTHING RUNS FOR AN APP WITH NO PLAYER IN IT. A module ticks only while its
  /// app has at least one player connected somewhere in the fleet, and stops when
  /// the last one leaves. `alwaysOn` is retired (2026-09-01) and passing true is
  /// REFUSED -- this comment used to tell you to set it for world simulation that
  /// must run without connected players, which is now the one thing it cannot do.
  ///
  /// A world that must advance while empty should compute the elapsed time on its
  /// first tick after a player returns: cheaper than ticking an empty world, and
  /// the same answer. Synchronous invoke is unaffected -- it is a request, not a
  /// scheduled tick.
  graphql::Json upsertModule(const graphql::JVal& input) const {
    return byInput("ComputeUpsertModule", input);
  }
  void upsertModuleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("ComputeUpsertModule", input, std::move(cb));
  }

  /// Upload an immutable source version (input: appId, moduleName,
  /// sourceFilesJson, sdkVersion, abiVersion) and make it the deployed
  /// version; it parks as compileStatus="pending" until a game server
  /// compiles it — poll moduleVersions. Prefer deploySource() which builds
  /// the input from a JSON source map and defaults the version pins.
  graphql::Json deployVersion(const graphql::JVal& input) const {
    return byInput("ComputeDeployVersion", input);
  }
  void deployVersionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("ComputeDeployVersion", input, std::move(cb));
  }

  /// Convenience deploy: sourceFiles is a JSON object mapping relative paths
  /// (Cargo.toml, src/*.rs) to file contents; SDK/ABI pins default to the
  /// current platform pins.
  graphql::Json deploySource(std::string_view appId, std::string_view moduleName,
                             const graphql::JVal& sourceFiles,
                             std::string_view sdkVersion = kComputeSdkVersion,
                             std::int64_t abiVersion = kComputeAbiVersion) const {
    graphql::JVal input;
    input["appId"] = appId;
    input["moduleName"] = moduleName;
    input["sourceFilesJson"] = sourceFiles.dump();
    input["sdkVersion"] = sdkVersion;
    input["abiVersion"] = abiVersion;
    return byInput("ComputeDeployVersion", input);
  }
  void deploySourceAsync(std::string_view appId, std::string_view moduleName,
                         const graphql::JVal& sourceFiles, std::string_view sdkVersion,
                         std::int64_t abiVersion, graphql::GraphQLCallback cb) const {
    graphql::JVal input;
    input["appId"] = appId;
    input["moduleName"] = moduleName;
    input["sourceFilesJson"] = sourceFiles.dump();
    input["sdkVersion"] = sdkVersion;
    input["abiVersion"] = abiVersion;
    byInputAsync("ComputeDeployVersion", input, std::move(cb));
  }

  /// Enable (requires a successfully compiled deployed version; resets the
  /// failure circuit) or disable (stops all scheduling) a module.
  graphql::Json setModuleEnabled(std::string_view appId, std::string_view name,
                                 bool enabled) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    return execUnwrap(gen::compute::documentFor("ComputeSetModuleEnabled"), vars, "ComputeSetModuleEnabled");
  }
  void setModuleEnabledAsync(std::string_view appId, std::string_view name, bool enabled,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    execUnwrapAsync(gen::compute::documentFor("ComputeSetModuleEnabled"), vars, "ComputeSetModuleEnabled",
                    std::move(cb));
  }

  /// Restart a module stopped for exceeding a resource limit, without deploying
  /// a new version.
  ///
  /// setModuleEnabled does NOT do this: a module stopped this way reports
  /// breakerLatchedAt and a breakerReason naming what it exceeded, and enabling
  /// it again leaves that latch in place. Deploying a fix clears it and is the
  /// normal path; use this when the limit itself was wrong or the load is being
  /// accepted deliberately. The reason is recorded with the module's history.
  graphql::Json resetBreaker(std::string_view appId, std::string_view name,
                             std::string_view reason) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["reason"] = reason;
    return execUnwrap(gen::compute::documentFor("ComputeResetBreaker"), vars,
                      "ComputeResetBreaker");
  }
  void resetBreakerAsync(std::string_view appId, std::string_view name,
                         std::string_view reason,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["reason"] = reason;
    execUnwrapAsync(gen::compute::documentFor("ComputeResetBreaker"), vars,
                    "ComputeResetBreaker", std::move(cb));
  }

  /// Delete a module (cascades versions/triggers/lease; run history is
  /// retained). Returns true when a module was deleted. DESTRUCTIVE.
  graphql::Json deleteModule(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::compute::documentFor("ComputeDeleteModule"), vars, "ComputeDeleteModule");
  }
  void deleteModuleAsync(std::string_view appId, std::string_view name,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::compute::documentFor("ComputeDeleteModule"), vars, "ComputeDeleteModule",
                    std::move(cb));
  }

  /// Bind a trigger: tick (tickHz, policy-clamped), event (onEvent:
  /// function_invoked | property_changed | container_created | compute_event
  /// + filters/debounceMs), or invoke (exportName + optional invokePolicyJson
  /// authority tree; null policy = compute admins only).
  graphql::Json upsertTrigger(const graphql::JVal& input) const {
    return byInput("ComputeUpsertTrigger", input);
  }
  void upsertTriggerAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("ComputeUpsertTrigger", input, std::move(cb));
  }

  /// Delete a trigger by id. Returns true when one was deleted.
  graphql::Json deleteTrigger(std::string_view appId, std::string_view triggerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    return execUnwrap(gen::compute::documentFor("ComputeDeleteTrigger"), vars, "ComputeDeleteTrigger");
  }
  void deleteTriggerAsync(std::string_view appId, std::string_view triggerId,
                          graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    execUnwrapAsync(gen::compute::documentFor("ComputeDeleteTrigger"), vars, "ComputeDeleteTrigger",
                    std::move(cb));
  }

  /// Set the app's compute policy (kill switch + module/tick/fuel/memory/
  /// runtime/host-op/egress ceilings + circuit tuning). Omitted fields keep
  /// their current values; values above the platform ceilings are rejected.
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return byInput("ComputeSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("ComputeSetPolicy", input, std::move(cb));
  }

  // ----- Invoke ----------------------------------------------------------------

  /// Synchronous RPC to a module's client-callable export (an invoke
  /// trigger). The result carries resultBase64 (raw bytes), resultJson (set
  /// when the bytes parse as JSON), fuelUsed, and durationUs. Failures
  /// (policy denial, disabled module, sandbox error) throw.
  graphql::Json invoke(std::string_view appId, std::string_view moduleName,
                       std::string_view exportName, std::string_view paramsJson = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["moduleName"] = moduleName;
    vars["exportName"] = exportName;
    if (!paramsJson.empty()) vars["paramsJson"] = paramsJson;
    return execUnwrap(gen::compute::documentFor("ComputeInvoke"), vars, "ComputeInvoke");
  }
  void invokeAsync(std::string_view appId, std::string_view moduleName,
                   std::string_view exportName, std::string_view paramsJson,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["moduleName"] = moduleName;
    vars["exportName"] = exportName;
    if (!paramsJson.empty()) vars["paramsJson"] = paramsJson;
    execUnwrapAsync(gen::compute::documentFor("ComputeInvoke"), vars, "ComputeInvoke", std::move(cb));
  }

  // ----- Monitoring (requires view_compute_diagnostics) ------------------------

  /// List the app's modules.
  graphql::Json modules(std::string_view appId) const {
    return byApp("ComputeModules", appId);
  }
  void modulesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    byAppAsync("ComputeModules", appId, std::move(cb));
  }

  /// Read one module by name.
  graphql::Json module(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::compute::documentFor("ComputeModule"), vars, "ComputeModule");
  }
  void moduleAsync(std::string_view appId, std::string_view name,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::compute::documentFor("ComputeModule"), vars, "ComputeModule", std::move(cb));
  }

  /// List a module's source versions, newest first, including compile
  /// status/log (poll after deploy until compileStatus settles).
  /// The platform's engine-template registry: ready-made engines deployable
  /// by name with deployTemplate().
  graphql::Json templates(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::compute::documentFor("ComputeTemplates"), vars, "ComputeTemplates");
  }
  void templatesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::compute::documentFor("ComputeTemplates"), vars, "ComputeTemplates", std::move(cb));
  }

  /// Deploy a named engine template from the platform registry — one call
  /// instead of the upsert/deploy/trigger/enable sequence. Compilation runs
  /// asynchronously; follow with waitForCompile().
  graphql::Json deployTemplate(std::string_view appId, std::string_view templateName,
                               std::string_view moduleName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["templateName"] = templateName;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    return execUnwrap(gen::compute::documentFor("ComputeDeployTemplate"), vars, "ComputeDeployTemplate");
  }
  void deployTemplateAsync(std::string_view appId, std::string_view templateName,
                           std::string_view moduleName, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["templateName"] = templateName;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    execUnwrapAsync(gen::compute::documentFor("ComputeDeployTemplate"), vars, "ComputeDeployTemplate",
                    std::move(cb));
  }

  graphql::Json moduleVersions(std::string_view appId, std::string_view moduleName,
                               int limit = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["moduleName"] = moduleName;
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    return execUnwrap(gen::compute::documentFor("ComputeModuleVersions"), vars, "ComputeModuleVersions");
  }
  void moduleVersionsAsync(std::string_view appId, std::string_view moduleName, int limit,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["moduleName"] = moduleName;
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    execUnwrapAsync(gen::compute::documentFor("ComputeModuleVersions"), vars, "ComputeModuleVersions",
                    std::move(cb));
  }

  /// Block until the newest version's compile settles. Returns the settled
  /// version on success; throws std::runtime_error on compile failure (the
  /// message includes the compile log) or timeout. Mirrors CrowdyJS
  /// compute.waitForCompile.
  graphql::Json waitForCompile(std::string_view appId, std::string_view moduleName,
                               std::chrono::milliseconds timeout = std::chrono::seconds(120),
                               std::chrono::milliseconds interval = std::chrono::seconds(2)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string status = "no version found";
    for (;;) {
      graphql::Json versions = moduleVersions(appId, moduleName, 1);
      if (versions.size() > 0) {
        graphql::Json latest = versions.at(0);
        status = latest["compileStatus"].asString();
        if (status == "succeeded") return latest;
        if (status == "failed") {
          throw std::runtime_error("compute module '" + std::string(moduleName) +
                                   "' compile failed:\n" +
                                   latest["compileLog"].asString("(no log)"));
        }
      }
      if (std::chrono::steady_clock::now() + interval > deadline) {
        throw std::runtime_error("compute module '" + std::string(moduleName) +
                                 "' compile did not settle within budget (status: " + status +
                                 ")");
      }
      std::this_thread::sleep_for(interval);
    }
  }
  /// Non-blocking compile waiter. Poll responses and the final callback are
  /// delivered through the GraphQL dispatcher when one is configured.
  void waitForCompileAsync(
      std::string_view appId, std::string_view moduleName,
      std::chrono::milliseconds timeout, std::chrono::milliseconds interval,
      graphql::GraphQLCallback cb) const {
    auto state = std::make_shared<CompileWaitState>();
    state->client = gql_;
    state->appId = appId;
    state->moduleName = moduleName;
    state->deadline = std::chrono::steady_clock::now() + timeout;
    state->interval = interval;
    state->callback = std::move(cb);
    pollCompile(std::move(state));
  }
  void waitForCompileAsync(std::string_view appId,
                           std::string_view moduleName,
                           graphql::GraphQLCallback cb) const {
    waitForCompileAsync(appId, moduleName, std::chrono::seconds(120),
                        std::chrono::seconds(2), std::move(cb));
  }

  /// List trigger bindings, optionally filtered to one module.
  graphql::Json moduleTriggers(std::string_view appId, std::string_view moduleName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    return execUnwrap(gen::compute::documentFor("ComputeModuleTriggers"), vars, "ComputeModuleTriggers");
  }
  void moduleTriggersAsync(std::string_view appId, std::string_view moduleName,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    execUnwrapAsync(gen::compute::documentFor("ComputeModuleTriggers"), vars, "ComputeModuleTriggers",
                    std::move(cb));
  }

  /// Read the app's compute policy (platform defaults when unset).
  graphql::Json modulePolicy(std::string_view appId) const {
    return byApp("ComputeModulePolicy", appId);
  }
  void modulePolicyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    byAppAsync("ComputeModulePolicy", appId, std::move(cb));
  }

  /// List runs, newest first (module loads/init, failures, circuit probes;
  /// healthy ticks aggregate into per-minute usage instead of run rows).
  /// vars: appId (required), moduleName, success, limit, offset.
  graphql::Json moduleRuns(const graphql::JVal& vars) const {
    return execUnwrap(gen::compute::documentFor("ComputeModuleRuns"), vars, "ComputeModuleRuns");
  }
  void moduleRunsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::compute::documentFor("ComputeModuleRuns"), vars, "ComputeModuleRuns",
                    std::move(cb));
  }

  /// Aggregate activity over a recent window (default 60 minutes).
  graphql::Json moduleStats(std::string_view appId, int windowMinutes = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    return execUnwrap(gen::compute::documentFor("ComputeModuleStats"), vars, "ComputeModuleStats");
  }
  void moduleStatsAsync(std::string_view appId, int windowMinutes,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    execUnwrapAsync(gen::compute::documentFor("ComputeModuleStats"), vars, "ComputeModuleStats",
                    std::move(cb));
  }

  /// Diagnostic log lines (guest log() output + failure lines), newest first.
  graphql::Json moduleLogs(std::string_view appId, std::string_view moduleName = {},
                           int limit = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    return execUnwrap(gen::compute::documentFor("ComputeModuleLogs"), vars, "ComputeModuleLogs");
  }
  void moduleLogsAsync(std::string_view appId, std::string_view moduleName, int limit,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!moduleName.empty()) vars["moduleName"] = moduleName;
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    execUnwrapAsync(gen::compute::documentFor("ComputeModuleLogs"), vars, "ComputeModuleLogs",
                    std::move(cb));
  }

  /// Snapshot of the app's compute footprint (counts + 24h activity).
  graphql::Json appDiagnostics(std::string_view appId) const {
    return byApp("ComputeAppDiagnostics", appId);
  }
  void appDiagnosticsAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    byAppAsync("ComputeAppDiagnostics", appId, std::move(cb));
  }

 private:
  struct CompileWaitState {
    std::shared_ptr<graphql::GraphQLClient> client;
    std::string appId;
    std::string moduleName;
    std::chrono::steady_clock::time_point deadline;
    std::chrono::milliseconds interval{0};
    std::string lastStatus = "no version found";
    graphql::GraphQLCallback callback;
  };

  static void pollCompile(std::shared_ptr<CompileWaitState> state) {
    graphql::JVal vars;
    vars["appId"] = state->appId;
    vars["moduleName"] = state->moduleName;
    vars["limit"] = std::int64_t{1};
    state->client->requestAsync(
        gen::compute::documentFor("ComputeModuleVersions"), vars,
        "ComputeModuleVersions",
        [state = std::move(state)](
            graphql::GraphQLOutcome outcome) mutable {
          if (!outcome.ok()) {
            finishCompileWait(std::move(state), std::move(outcome));
            return;
          }
          const auto versions = outcome.data["computeModuleVersions"];
          if (versions.isArray() && versions.size() > 0) {
            const auto latest = versions.at(0);
            state->lastStatus = latest["compileStatus"].asString();
            if (state->lastStatus == "succeeded") {
              outcome.data = latest;
              finishCompileWait(std::move(state), std::move(outcome));
              return;
            }
            if (state->lastStatus == "failed") {
              outcome.status = Errc::Rejected;
              outcome.kind = graphql::GraphQLErrorKind::Protocol;
              outcome.errorMessage =
                  "compute module '" + state->moduleName +
                  "' compile failed:\n" +
                  latest["compileLog"].asString("(no log)");
              finishCompileWait(std::move(state), std::move(outcome));
              return;
            }
          }
          if (std::chrono::steady_clock::now() + state->interval >
              state->deadline) {
            outcome.status = Errc::Timeout;
            outcome.kind = graphql::GraphQLErrorKind::Timeout;
            outcome.errorMessage =
                "compute module '" + state->moduleName +
                "' compile did not settle within budget (status: " +
                state->lastStatus + ")";
            finishCompileWait(std::move(state), std::move(outcome));
            return;
          }
          scheduleCompilePoll(std::move(state));
        });
  }

  static void scheduleCompilePoll(
      std::shared_ptr<CompileWaitState> state) {
    std::thread([state = std::move(state)]() mutable {
      if (state->interval.count() > 0) {
        std::this_thread::sleep_for(state->interval);
      }
      if (auto dispatcher = state->client->dispatcher()) {
        dispatcher->post(
            [state = std::move(state)]() mutable {
              pollCompile(std::move(state));
            });
      } else {
        pollCompile(std::move(state));
      }
    }).detach();
  }

  static void finishCompileWait(
      std::shared_ptr<CompileWaitState> state,
      graphql::GraphQLOutcome outcome) {
    auto callback = std::move(state->callback);
    if (callback) callback(std::move(outcome));
  }

  graphql::Json byInput(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::compute::documentFor(op), vars, op);
  }
  void byInputAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::compute::documentFor(op), vars, op, std::move(cb));
  }
  graphql::Json byApp(std::string_view op, std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::compute::documentFor(op), vars, op);
  }
  void byAppAsync(std::string_view op, std::string_view appId,
                  graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::compute::documentFor(op), vars, op, std::move(cb));
  }
};

}  // namespace crowdy::domains
