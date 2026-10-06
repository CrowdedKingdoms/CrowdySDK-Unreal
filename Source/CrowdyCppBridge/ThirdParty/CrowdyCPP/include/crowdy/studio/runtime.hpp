#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/exec.hpp"
#include "crowdy/domains/player_wallet.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/models.hpp"

namespace crowdy::studio {

enum class CrowdyStudioDeployment { Draft, Live };

struct CrowdyStudioDeploymentPlan {
  std::string expectedRevisionId;
  std::vector<CrowdyStudioTarget> targets;
  std::optional<CrowdyStudioPairingPreference> pairingPreference;
  std::optional<std::string> projectContentHash;
};

/// What a deploy names. Both targets build `files` on the platform: the SERVER
/// target as the project's ck-exec mod, the CLIENT target as that mod's CLIENT
/// half (a `crowdy-client-sdk` crate).
struct CrowdyStudioDeployTargetInput {
  CrowdyStudioProjectScope scope;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string moduleName;
  /// The mod the project runs as: its SERVER module's name, or for a
  /// CLIENT-only project the CLIENT module's. Empty means `moduleName`.
  std::string modName;
  /// The project has no SERVER target, so the mod its CLIENT half rides is the
  /// runtime's to deploy (from the mod starter) and switch on.
  bool clientOnly = false;
  std::string projectId;
  /// The bound project's mirror commit (`project.github->sha`); empty for a
  /// Studio project.
  std::optional<std::string> commitSha;
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  /// The target's project files.
  std::vector<CrowdyStudioProjectFile> files;
};

struct CrowdyStudioDeploySubmission {
  std::string versionId;
};

struct CrowdyStudioRuntimeVersion {
  std::string versionId;
  std::string compileStatus;
  std::optional<std::string> compileLog;
};

/// The project's CLIENT half as the API served it for this player, attached to
/// `modName` and checked against its digest, ABI and capability summary.
struct CrowdyStudioClientArtifact {
  /// The CLIENT build that was attached.
  std::string versionId;
  std::string modName;
  domains::ExecModClientArtifactBytes module;
};

/// A mod endpoint's decoded reply as JSON, and the call's round trip.
struct CrowdyStudioInvokeResult {
  std::string resultJson;
  std::int64_t durationUs = 0;
};

/// One line the SERVER target's mod logged (`ctx.log`).
struct CrowdyStudioLogLine {
  std::string id;
  std::string moduleName;
  /// "error", "warn", "info" or "debug".
  std::string level;
  std::string at;
  std::string text;
};

struct CrowdyStudioWalletSnapshot {
  /// Micro-USD (1 USD = 1,000,000) as a decimal string; the unit of account
  /// since the lossless ledger (ck-api 2026-09-11). Spendable = balance - holds.
  std::string balanceMicrousd;
  std::string holdsMicrousd;
  /// Deprecated: balanceMicrousd / 10,000 truncated toward zero.
  std::string balanceCents;
  std::string currency;

  bool operator==(const CrowdyStudioWalletSnapshot&) const = default;
};

/// Optional, viewer-scoped wallet observation seam. It deliberately exposes
/// only the caller's current balance and cannot spend, recharge, mutate billing
/// policy, or act for another user.
class ICrowdyStudioWalletProvider {
 public:
  virtual ~ICrowdyStudioWalletProvider() = default;
  virtual CrowdyStudioWalletSnapshot balance() = 0;
};

/// Engine-owned execution of a served CLIENT half: run `module.bytes` with
/// `module.fuelPerDispatch` in its `ck_fuel` global, a tick every
/// `module.tickIntervalMs`, and only `module.capabilitySummary.hostFunctions`
/// reachable. Rendering, host-call routing and the sandbox stay outside CrowdyCPP.
class ICrowdyStudioClientRuntime {
 public:
  virtual ~ICrowdyStudioClientRuntime() = default;
  virtual void start(const CrowdyStudioClientArtifact& artifact) = 0;
  virtual void stop() = 0;
};

/// Injectable runtime seam used by the headless controller: the SERVER target
/// as a ck-exec mod, the CLIENT target as that mod's CLIENT half. Fakes can
/// implement this directly; CrowdyStudioModRuntime is the production adapter
/// over CrowdyClient::exec().
class ICrowdyStudioRuntime {
 public:
  virtual ~ICrowdyStudioRuntime() = default;

  virtual CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) = 0;
  virtual std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName) = 0;
  virtual void setEnabled(const CrowdyStudioProjectScope& scope,
                          std::string_view moduleName, bool enabled) = 0;
  virtual void startClient(const CrowdyStudioProjectScope& scope,
                           std::string_view moduleName,
                           std::string_view versionId) = 0;
  virtual void stopClient() = 0;
  virtual CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view method,
      const std::optional<std::string>& paramsJson) = 0;

  virtual std::vector<CrowdyStudioLogLine> logs(
      const CrowdyStudioProjectScope&, std::string_view) {
    return {};
  }
};

/// What a CLIENT target that still builds on legacy player compute is told.
/// CrowdyJS 18's words.
inline constexpr std::string_view kCrowdyStudioLegacyClientCrate =
    "This CLIENT crate depends on crowdy-compute-sdk, legacy player compute. On ck-exec a CLIENT "
    "target is a mod\xE2\x80\x99s CLIENT half, one crowdy-client-sdk crate: in Cargo.toml replace the "
    "crowdy-compute-sdk line with crowdy-client-sdk = \"0.1.0\", and in src/ use crowdy_client_sdk "
    "in place of crowdy_compute_sdk. Its host calls are the same less the Game Model, sessions and "
    "grid state (grid_state_get / grid_state_set): the mod\xE2\x80\x99s server half, a hub keyed by the "
    "grid, holds grid state now.";

/// Whether a CLIENT target's Cargo.toml depends on crowdy-compute-sdk (a line
/// starting `crowdy-compute-sdk =`).
inline bool crowdyStudioIsLegacyClientCrate(std::string_view cargoToml) {
  std::size_t at = 0;
  while (at <= cargoToml.size()) {
    std::size_t end = cargoToml.find('\n', at);
    if (end == std::string_view::npos) end = cargoToml.size();
    std::string_view line = cargoToml.substr(at, end - at);
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string_view::npos) {
      line.remove_prefix(first);
      constexpr std::string_view kKey = "crowdy-compute-sdk";
      if (line.substr(0, kKey.size()) == kKey) {
        const std::size_t eq = line.find_first_not_of(" \t", kKey.size());
        if (eq != std::string_view::npos && line[eq] == '=') return true;
      }
    }
    at = end + 1;
  }
  return false;
}

struct CrowdyStudioLiveApprovalRequest {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  std::string projectRevisionId;
  std::string projectContentHash;
  std::vector<CrowdyStudioTarget> targets;
  CrowdyStudioPairingPreference pairingPreference =
      CrowdyStudioPairingPreference::None;
};

struct CrowdyStudioRestoreApprovalRequest {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  std::string checkpointId;
  std::string expectedRevisionId;
};

/// Approval authority is deliberately injected from the durable agent layer.
/// The Studio controller cannot mint, weaken, or infer an approval.
class ICrowdyStudioApprovalGate {
 public:
  virtual ~ICrowdyStudioApprovalGate() = default;
  virtual void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view approvalGrant) = 0;
  virtual void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest& request,
      std::string_view approvalGrant) = 0;
};

/// Minimal production adapter over the public viewer-scoped PlayerWalletAPI
/// balance read. No billing/provider authority crosses this interface.
class CrowdyStudioPlayerWalletProvider final
    : public ICrowdyStudioWalletProvider {
 public:
  explicit CrowdyStudioPlayerWalletProvider(
      domains::PlayerWalletAPI& playerWallet)
      : playerWallet_(&playerWallet) {}

  explicit CrowdyStudioPlayerWalletProvider(
      std::shared_ptr<domains::PlayerWalletAPI> playerWallet)
      : playerWalletOwner_(std::move(playerWallet)),
        playerWallet_(playerWalletOwner_.get()) {
    if (!playerWallet_) {
      throw std::invalid_argument(
          "Crowdy Studio wallet provider requires PlayerWalletAPI");
    }
  }

  CrowdyStudioWalletSnapshot balance() override {
    const graphql::Json value = playerWallet_->balance();
    CrowdyStudioWalletSnapshot snapshot;
    snapshot.balanceMicrousd = scalarString(value["balanceMicrousd"]);
    snapshot.holdsMicrousd = scalarString(value["holdsMicrousd"]);
    snapshot.balanceCents = scalarString(value["balanceCents"]);
    snapshot.currency = value["currency"].asString();
    if (snapshot.balanceMicrousd.empty() || snapshot.currency.empty()) {
      throw std::runtime_error(
          "Player wallet balance response is incomplete");
    }
    return snapshot;
  }

 private:
  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    if (value.isNumber()) return std::to_string(value.asInt64());
    return {};
  }

  std::shared_ptr<domains::PlayerWalletAPI> playerWalletOwner_;
  domains::PlayerWalletAPI* playerWallet_ = nullptr;
};

/// The production runtime. The SERVER target is the grid's ck-exec mod
/// (`mod:<name>`, keyed by the grid), built from the target's crate files with
/// modBuild, deployed when the build succeeds and switched with
/// modSetEnabled; Invoke calls one of its endpoints over an exec connection
/// and Logs are its `ctx.log` lines. The CLIENT target is that mod's CLIENT
/// half: a `crowdy-client-sdk` crate built with modClientBuild, attached with
/// modClientDeploy, consented to as its author, fetched with
/// modClientArtifactBytes and run in the engine-owned client runtime. A
/// CLIENT-only project's CLIENT half rides the mod named for its CLIENT module,
/// which the runtime deploys from the mod starter when the grid has none.
///
/// `pump` runs while a mod call waits for its reply: pass what drains the
/// client's dispatcher (CrowdyClient::poll), since exec callbacks run there.
class CrowdyStudioModRuntime final : public ICrowdyStudioRuntime {
 public:
  explicit CrowdyStudioModRuntime(domains::ExecAPI& exec,
                                  ICrowdyStudioClientRuntime* clientRuntime = nullptr,
                                  std::function<void()> pump = {})
      : exec_(exec), clientRuntime_(clientRuntime), pump_(std::move(pump)) {}

  explicit CrowdyStudioModRuntime(std::shared_ptr<domains::ExecAPI> exec,
                                  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntime = {},
                                  std::function<void()> pump = {})
      : execOwner_(std::move(exec)),
        clientRuntimeOwner_(std::move(clientRuntime)),
        exec_(require(execOwner_, "ExecAPI")),
        clientRuntime_(clientRuntimeOwner_.get()),
        pump_(std::move(pump)) {}

  ~CrowdyStudioModRuntime() override {
    for (auto& [name, connection] : connections_) {
      if (connection) connection->close();
    }
  }

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    if (input.target == CrowdyStudioTarget::Server) return buildMod(input);
    return buildClientHalf(input);
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    std::vector<CrowdyStudioRuntimeVersion> mapped;
    const auto server = modBuilds_.find(std::string(moduleName));
    if (server != modBuilds_.end()) mapped.push_back(modBuildStatus(scope, server->first, server->second));
    const auto client = clientBuilds_.find(std::string(moduleName));
    if (client != clientBuilds_.end()) mapped.push_back(buildStatus(scope, client->second.buildId));
    return mapped;
  }

  void setEnabled(const CrowdyStudioProjectScope& scope,
                  std::string_view moduleName, bool enabled) override {
    (void)exec_.modSetEnabled(scope.appId, scope.gridId, std::string(moduleName), enabled);
  }

  void startClient(const CrowdyStudioProjectScope& scope,
                   std::string_view moduleName,
                   std::string_view versionId) override {
    if (!clientRuntime_) {
      throw std::runtime_error(
          "CLIENT execution requires an engine-owned artifact runtime");
    }
    ClientBuild build;
    const auto known = clientBuilds_.find(std::string(moduleName));
    if (known != clientBuilds_.end()) build = known->second;
    if (build.modName.empty()) build.modName = std::string(moduleName);
    const std::string& mod = build.modName;
    if (build.clientOnly) ensureClientOnlyMod(scope, mod);

    const graphql::Json attached =
        exec_.modClientDeploy(scope.appId, scope.gridId, mod, std::string(versionId));
    const std::string modId = attached["modId"].asString();
    // The API serves a CLIENT half only to a player who consented to it or
    // trusts its author, its author included.
    (void)exec_.consentClientMod(scope.appId, modId, attached["capabilityHash"].asString());
    CrowdyStudioClientArtifact artifact;
    artifact.versionId = std::string(versionId);
    artifact.modName = mod;
    try {
      artifact.module = exec_.modClientArtifactBytes(scope.appId, modId);
    } catch (const graphql::CrowdyGraphQLError& error) {
      if (error.code() == "NOT_FOUND") {
        throw std::runtime_error(
            std::string(moduleName) + " is attached to mod '" + mod +
            "', but its preview did not load: the API serves a CLIENT half only while its mod is "
            "switched on and not held, to a player with run_client_code who stands in grid " +
            scope.gridId);
      }
      if (error.code() == "RATE_LIMITED") {
        throw std::runtime_error(std::string(moduleName) + " is attached to mod '" + mod +
                                 "', but its preview was fetched too often (12 a minute); deploy "
                                 "again in a minute");
      }
      throw;
    }
    if (artifact.module.digest != attached["digest"].asString() ||
        artifact.module.clientVersion != attached["clientVersion"].asInt64()) {
      throw std::runtime_error("The served CLIENT half is not the one just attached; deploy again");
    }
    clientRuntime_->start(artifact);
  }

  void stopClient() override {
    if (clientRuntime_) clientRuntime_->stop();
  }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view method,
      const std::optional<std::string>& paramsJson) override {
    // MessagePack nil: a call with no arguments.
    std::string payload(1, static_cast<char>(0xc0));
    if (paramsJson && paramsJson->find_first_not_of(" \t\r\n") != std::string::npos) {
      const graphql::Json args = graphql::Json::parse(*paramsJson);
      if (!args.ok()) {
        throw std::invalid_argument("The call arguments must be JSON");
      }
      payload = args.toMsgpack();
    }
    const std::string nodeType = domains::execModType(moduleName);
    auto connection = modConnection(scope, moduleName);
    auto reply = std::make_shared<std::promise<domains::ExecReply>>();
    std::future<domains::ExecReply> replied = reply->get_future();
    const auto started = std::chrono::steady_clock::now();
    connection->callRaw(nodeType, scope.gridId, std::string(method), std::move(payload),
                        [reply](domains::ExecReply value) { reply->set_value(std::move(value)); });
    // The connection answers DeadlineExceeded after its own call timeout; this
    // bound only stops a wait whose callbacks nothing is draining.
    const auto giveUp = started + std::chrono::seconds(30);
    while (replied.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) {
      if (pump_) pump_();
      if (std::chrono::steady_clock::now() > giveUp) {
        throw std::runtime_error("DeadlineExceeded: the mod call got no reply");
      }
    }
    const domains::ExecReply value = replied.get();
    if (!value.ok()) {
      throw std::runtime_error(std::string(domains::execStatusName(value.status)) + ": " +
                               value.message());
    }
    CrowdyStudioInvokeResult result;
    const graphql::Json decoded = value.value();
    result.resultJson = decoded.ok() ? decoded.dump() : "null";
    result.durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    return result;
  }

  std::vector<CrowdyStudioLogLine> logs(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    domains::ExecLogsQuery query;
    query.limit = 50;
    const graphql::Json response =
        exec_.modLogs(scope.appId, scope.gridId, std::string(moduleName), query);
    static constexpr std::string_view kLevels[] = {"error", "warn", "info", "debug"};
    std::vector<CrowdyStudioLogLine> lines;
    response.forEach([&](const graphql::Json& value) {
      CrowdyStudioLogLine line;
      line.id = value["id"].asString();
      line.moduleName = std::string(moduleName);
      const std::int64_t level = value["level"].asInt64(3);
      line.level = std::string(level >= 0 && level < 4 ? kLevels[level] : kLevels[3]);
      line.at = value["at"].asString();
      line.text = value["text"].asString();
      lines.push_back(std::move(line));
    });
    return lines;
  }

 private:
  struct ModBuild {
    std::string buildId;
    bool deployed = false;
  };

  struct ClientBuild {
    std::string buildId;
    std::string modName;
    bool clientOnly = false;
  };

  CrowdyStudioDeploySubmission buildMod(const CrowdyStudioDeployTargetInput& input) {
    const std::string& name = input.moduleName;
    if (!isModName(name)) {
      throw std::invalid_argument(
          "The server module name '" + name +
          "' must be 1-48 lowercase letters, digits, - or _ to run as a mod");
    }
    const std::string buildId = queueBuild(input.scope.appId, crateOf(name, input.files), false);
    modBuilds_[name] = {buildId, false};
    return {buildId};
  }

  CrowdyStudioDeploySubmission buildClientHalf(const CrowdyStudioDeployTargetInput& input) {
    const std::string& name = input.moduleName;
    const std::string mod = input.modName.empty() ? name : input.modName;
    if (!isModName(mod)) {
      throw std::invalid_argument(
          input.clientOnly
              ? "The CLIENT module name '" + mod +
                    "' names the mod its CLIENT half rides, so it must be 1-48 lowercase letters, "
                    "digits, - or _"
              : "The server module name '" + mod +
                    "' must be 1-48 lowercase letters, digits, - or _ to run as a mod");
    }
    for (const auto& file : input.files) {
      if (file.path == "Cargo.toml" && crowdyStudioIsLegacyClientCrate(file.content)) {
        throw std::invalid_argument(std::string(kCrowdyStudioLegacyClientCrate));
      }
    }
    const std::string buildId = queueBuild(input.scope.appId, crateOf(name, input.files), true);
    clientBuilds_[name] = {buildId, mod, input.clientOnly};
    return {buildId};
  }

  /// The crate a build takes: Cargo.toml, README.md and Rust under src/, named
  /// `mod-<name>` when the name does not start with a letter.
  static domains::ExecCrate crateOf(const std::string& name,
                                    const std::vector<CrowdyStudioProjectFile>& files) {
    domains::ExecCrate crate;
    crate.name = crateName(name);
    for (const auto& file : files) {
      const std::string& path = file.path;
      const bool rust = path.rfind("src/", 0) == 0 && path.size() > 3 &&
                        path.compare(path.size() - 3, 3, ".rs") == 0;
      if (path == "Cargo.toml" || path == "README.md" || rust) {
        crate.files.emplace_back(path, file.content);
      }
    }
    return crate;
  }

  static std::string crateName(const std::string& name) {
    return !name.empty() && name.front() >= 'a' && name.front() <= 'z' ? name : "mod-" + name;
  }

  std::string queueBuild(const std::string& appId, const domains::ExecCrate& crate, bool client) {
    const graphql::Json queued = client ? exec_.modClientBuild(appId, crate) : exec_.modBuild(appId, crate);
    const std::string buildId = queued["buildId"].asString();
    if (buildId.empty()) {
      throw std::runtime_error(std::string(client ? "execModClientBuild" : "execModBuild") +
                               " returned no build id");
    }
    return buildId;
  }

  CrowdyStudioRuntimeVersion buildStatus(const CrowdyStudioProjectScope& scope, const std::string& buildId) {
    const graphql::Json status = exec_.modBuildStatus(scope.appId, buildId);
    CrowdyStudioRuntimeVersion version;
    version.versionId = buildId;
    version.compileStatus = status["status"].asString();
    if (status["log"].ok() && !status["log"].isNull()) version.compileLog = status["log"].asString();
    return version;
  }

  CrowdyStudioRuntimeVersion modBuildStatus(const CrowdyStudioProjectScope& scope,
                                            const std::string& name, ModBuild& build) {
    CrowdyStudioRuntimeVersion version = buildStatus(scope, build.buildId);
    if (version.compileStatus == "succeeded" && !build.deployed) {
      // A new mod starts switched off; the controller enables it next.
      (void)exec_.modDeploy(scope.appId, scope.gridId, name, build.buildId);
      build.deployed = true;
    }
    return version;
  }

  /// A CLIENT half rides a mod: with no mod of this name of the player's on the
  /// grid, the mod starter is deployed under it first. The mod is switched on,
  /// since only a running mod's CLIENT half is served.
  void ensureClientOnlyMod(const CrowdyStudioProjectScope& scope, const std::string& name) {
    graphql::Json mine;
    exec_.myMods(scope.appId).forEach([&](const graphql::Json& mod) {
      if (mod["gridId"].asBigIntString() == scope.gridId && mod["name"].asString() == name) mine = mod;
    });
    if (!mine.isObject()) {
      const graphql::Json starter = exec_.modStarter(scope.appId);
      domains::ExecCrate crate;
      crate.name = crateName(name);
      starter["files"].forEach([&](const graphql::Json& file) {
        crate.files.emplace_back(file["path"].asString(), file["content"].asString());
      });
      const std::string buildId = queueBuild(scope.appId, crate, false);
      const graphql::Json built = exec_.waitForModBuild(scope.appId, buildId, 1500);
      if (built["status"].asString() != "succeeded") {
        throw std::runtime_error("The mod starter did not build, so the CLIENT half has no mod '" + name +
                                 "' to ride");
      }
      (void)exec_.modDeploy(scope.appId, scope.gridId, name, buildId);
    }
    if (!mine.isObject() || !mine["enabled"].asBool()) {
      (void)exec_.modSetEnabled(scope.appId, scope.gridId, name, true);
    }
  }

  std::shared_ptr<domains::ExecConnection> modConnection(const CrowdyStudioProjectScope& scope,
                                                         std::string_view moduleName) {
    const std::string key = scope.appId + "/" + scope.gridId + "/" + std::string(moduleName);
    auto& connection = connections_[key];
    if (!connection) {
      domains::ExecConnectOptions options;
      options.nodeType = domains::execModType(moduleName);
      options.key = scope.gridId;
      connection = exec_.connect(scope.appId, std::move(options));
    }
    return connection;
  }

  /// As ck-exec's mod names: 1-48 lowercase letters, digits, - or _.
  static bool isModName(std::string_view name) {
    if (name.empty() || name.size() > 48) return false;
    for (const char c : name) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      if (!ok) return false;
    }
    return true;
  }

  template <typename T>
  static T& require(const std::shared_ptr<T>& value, const char* what) {
    if (!value) {
      throw std::invalid_argument(std::string("Crowdy Studio runtime requires ") + what);
    }
    return *value;
  }

  std::shared_ptr<domains::ExecAPI> execOwner_;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntimeOwner_;
  domains::ExecAPI& exec_;
  ICrowdyStudioClientRuntime* clientRuntime_;
  std::function<void()> pump_;
  std::map<std::string, ModBuild> modBuilds_;
  std::map<std::string, ClientBuild> clientBuilds_;
  std::map<std::string, std::shared_ptr<domains::ExecConnection>> connections_;
};

}  // namespace crowdy::studio
