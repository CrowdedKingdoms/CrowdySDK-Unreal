#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_wallet.hpp"
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

struct CrowdyStudioDeployTargetInput {
  CrowdyStudioProjectScope scope;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string moduleName;
  std::vector<CrowdyStudioProjectFile> files;
  std::string sdkVersion;
  int abiVersion = 0;
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
};

struct CrowdyStudioDeploySubmission {
  std::string versionId;
};

struct CrowdyStudioRuntimeVersion {
  std::string versionId;
  std::string compileStatus;
  std::optional<std::string> compileLog;
};

struct CrowdyStudioClientArtifact {
  std::string versionId;
  std::string artifactHash;
  std::vector<std::uint8_t> bytes;
  std::string fuelPerDispatch;
  std::optional<std::string> contractJson;
};

struct CrowdyStudioInvokeResult {
  std::optional<std::string> resultBase64;
  std::optional<std::string> resultJson;
  std::string fuelUsed;
  std::int64_t durationUs = 0;
};

struct CrowdyStudioRun {
  std::string runId;
  std::string moduleName;
  std::string triggerSource;
  std::string startedAt;
  std::int64_t durationUs = 0;
  std::string fuelUsed;
  bool success = false;
  std::optional<std::string> errorMessage;
};

struct CrowdyStudioUsageSnapshot {
  std::string hourUnitsUsed;
  std::string dayUnitsUsed;
  std::optional<std::string> unitsPerHour;
  std::optional<std::string> unitsPerDay;
  int compilesThisHour = 0;
  int maxCompilesPerHour = 0;
  std::string gateStatus;
  std::optional<std::string> gateReason;
};

struct CrowdyStudioWalletSnapshot {
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

/// Engine-owned execution of an already authorized, exact CLIENT artifact.
/// Rendering, host-call routing, and sandbox lifecycle remain outside CrowdyCPP.
class ICrowdyStudioClientRuntime {
 public:
  virtual ~ICrowdyStudioClientRuntime() = default;
  virtual void start(const CrowdyStudioClientArtifact& artifact) = 0;
  virtual void stop() = 0;
};

/// Injectable playerCompute seam used by the headless controller. Fakes can
/// implement this directly; CrowdyStudioPlayerComputeRuntime is the production
/// adapter over CrowdyClient::playerCompute().
class ICrowdyStudioRuntime {
 public:
  virtual ~ICrowdyStudioRuntime() = default;

  virtual CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) = 0;
  virtual std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName) = 0;
  virtual void setEnabled(const CrowdyStudioProjectScope& scope,
                          std::string_view moduleName, bool enabled) = 0;
  virtual void setRequires(
      const CrowdyStudioProjectScope& scope, std::string_view serverName,
      const std::optional<std::string>& requiredClientName) = 0;
  virtual void startClient(const CrowdyStudioProjectScope& scope,
                           std::string_view moduleName,
                           std::string_view versionId) = 0;
  virtual void stopClient() = 0;
  virtual CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view exportName,
      const std::optional<std::string>& paramsJson) = 0;

  virtual std::vector<CrowdyStudioRun> runs(
      const CrowdyStudioProjectScope&, std::string_view) {
    return {};
  }
  virtual std::vector<CrowdyStudioRun> logs(
      const CrowdyStudioProjectScope&, std::string_view) {
    return {};
  }
  virtual std::optional<CrowdyStudioUsageSnapshot> usage(
      std::string_view) {
    return std::nullopt;
  }
};

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
    snapshot.balanceCents = scalarString(value["balanceCents"]);
    snapshot.currency = value["currency"].asString();
    if (snapshot.balanceCents.empty() || snapshot.currency.empty()) {
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

class CrowdyStudioPlayerComputeRuntime final : public ICrowdyStudioRuntime {
 public:
  explicit CrowdyStudioPlayerComputeRuntime(
      domains::PlayerComputeAPI& playerCompute,
      ICrowdyStudioClientRuntime* clientRuntime = nullptr)
      : playerCompute_(playerCompute), clientRuntime_(clientRuntime) {}

  CrowdyStudioPlayerComputeRuntime(
      std::shared_ptr<domains::PlayerComputeAPI> playerCompute,
      std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntime = {})
      : playerComputeOwner_(std::move(playerCompute)),
        clientRuntimeOwner_(std::move(clientRuntime)),
        playerCompute_(requirePlayerCompute(playerComputeOwner_)),
        clientRuntime_(clientRuntimeOwner_.get()) {}

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    graphql::JObject sources;
    for (const auto& file : input.files) {
      if (file.target != input.target) {
        throw std::invalid_argument(
            "Crowdy Studio deploy input crossed target boundaries");
      }
      sources[normalizeCrowdyStudioPath(file.path)] = file.content;
    }
    graphql::JVal variables;
    variables["appId"] = input.scope.appId;
    variables["gridId"] = input.scope.gridId;
    variables["name"] = input.moduleName;
    variables["target"] = toString(input.target);
    variables["sourceFilesJson"] =
        graphql::JVal(std::move(sources)).dump();
    variables["sdkVersion"] = input.sdkVersion;
    variables["abiVersion"] = input.abiVersion;
    if (input.target == CrowdyStudioTarget::Server) {
      variables["tickHz"] = 1;
    }
    variables["draft"] =
        input.deployment == CrowdyStudioDeployment::Draft;
    const graphql::Json response = playerCompute_.deploy(variables);
    return {response["versionId"].asString()};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    const graphql::Json response =
        playerCompute_.versions(scope.appId, scope.gridId, moduleName);
    std::vector<CrowdyStudioRuntimeVersion> mapped;
    response.forEach([&](const graphql::Json& value) {
      CrowdyStudioRuntimeVersion version;
      version.versionId = value["versionId"].asString();
      version.compileStatus = value["compileStatus"].asString();
      if (value["compileLog"].ok() && !value["compileLog"].isNull()) {
        version.compileLog = value["compileLog"].asString();
      }
      mapped.push_back(std::move(version));
    });
    return mapped;
  }

  void setEnabled(const CrowdyStudioProjectScope& scope,
                  std::string_view moduleName, bool enabled) override {
    (void)playerCompute_.setEnabled(scope.appId, scope.gridId, moduleName,
                                    enabled);
  }

  void setRequires(
      const CrowdyStudioProjectScope& scope, std::string_view serverName,
      const std::optional<std::string>& requiredClientName) override {
    (void)playerCompute_.setRequires(
        scope.appId, scope.gridId, serverName,
        requiredClientName ? std::string_view(*requiredClientName)
                           : std::string_view{});
  }

  void startClient(const CrowdyStudioProjectScope& scope,
                   std::string_view moduleName,
                   std::string_view versionId) override {
    if (!clientRuntime_) {
      throw std::runtime_error(
          "CLIENT execution requires an engine-owned artifact runtime");
    }
    const graphql::Json response = playerCompute_.artifact(
        scope.appId, scope.gridId, moduleName, versionId);
    CrowdyStudioClientArtifact artifact;
    artifact.versionId = response["versionId"].asString();
    artifact.artifactHash = response["artifactHash"].asString();
    artifact.fuelPerDispatch =
        scalarString(response["clientFuelPerDispatch"]);
    if (response["contractJson"].ok() &&
        !response["contractJson"].isNull()) {
      artifact.contractJson = response["contractJson"].asString();
    }
    const auto bytes =
        core::base64Decode(response["artifactBase64"].asStringView());
    if (artifact.versionId != versionId || artifact.artifactHash.empty() ||
        !bytes || bytes->empty()) {
      throw std::runtime_error(
          "Client artifact did not match the compiled project version");
    }
    artifact.bytes = *bytes;
    clientRuntime_->start(artifact);
  }

  void stopClient() override {
    if (clientRuntime_) clientRuntime_->stop();
  }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view exportName,
      const std::optional<std::string>& paramsJson) override {
    const graphql::Json response = playerCompute_.invoke(
        scope.appId, scope.gridId, moduleName, exportName,
        paramsJson ? std::string_view(*paramsJson) : std::string_view("{}"));
    CrowdyStudioInvokeResult result;
    result.resultBase64 = optionalString(response["resultBase64"]);
    result.resultJson = optionalString(response["resultJson"]);
    result.fuelUsed = scalarString(response["fuelUsed"]);
    result.durationUs = response["durationUs"].asInt64();
    return result;
  }

  std::vector<CrowdyStudioRun> runs(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    graphql::JVal options;
    if (!moduleName.empty()) options["moduleName"] = moduleName;
    options["limit"] = 50;
    options["offset"] = 0;
    return mapRuns(playerCompute_.runs(scope.appId, scope.gridId, options));
  }

  std::vector<CrowdyStudioRun> logs(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    graphql::JVal options;
    if (!moduleName.empty()) options["moduleName"] = moduleName;
    options["limit"] = 50;
    return mapRuns(playerCompute_.logs(scope.appId, scope.gridId, options));
  }

  std::optional<CrowdyStudioUsageSnapshot> usage(
      std::string_view appId) override {
    const graphql::Json value = playerCompute_.usage(appId);
    CrowdyStudioUsageSnapshot usage;
    usage.hourUnitsUsed = scalarString(value["hourUnitsUsed"]);
    usage.dayUnitsUsed = scalarString(value["dayUnitsUsed"]);
    usage.unitsPerHour = optionalString(value["unitsPerHour"]);
    usage.unitsPerDay = optionalString(value["unitsPerDay"]);
    usage.compilesThisHour =
        static_cast<int>(value["compilesThisHour"].asInt64());
    usage.maxCompilesPerHour =
        static_cast<int>(value["maxCompilesPerHour"].asInt64());
    usage.gateStatus = value["gateStatus"].asString();
    usage.gateReason = optionalString(value["gateReason"]);
    return usage;
  }

 private:
  static domains::PlayerComputeAPI& requirePlayerCompute(
      const std::shared_ptr<domains::PlayerComputeAPI>& playerCompute) {
    if (!playerCompute) {
      throw std::invalid_argument(
          "Crowdy Studio runtime requires PlayerComputeAPI");
    }
    return *playerCompute;
  }

  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    return std::to_string(value.asInt64());
  }

  static std::optional<std::string> optionalString(
      const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return std::nullopt;
    return scalarString(value);
  }

  static std::vector<CrowdyStudioRun> mapRuns(
      const graphql::Json& response) {
    std::vector<CrowdyStudioRun> runs;
    response.forEach([&](const graphql::Json& value) {
      CrowdyStudioRun run;
      run.runId = value["runId"].asString();
      run.moduleName = value["moduleName"].asString();
      run.triggerSource = value["triggerSource"].asString();
      run.startedAt = value["startedAt"].asString();
      run.durationUs = value["durationUs"].asInt64();
      run.fuelUsed = scalarString(value["fuelUsed"]);
      run.success = value["success"].asBool();
      run.errorMessage = optionalString(value["errorMessage"]);
      runs.push_back(std::move(run));
    });
    return runs;
  }

  std::shared_ptr<domains::PlayerComputeAPI> playerComputeOwner_;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntimeOwner_;
  domains::PlayerComputeAPI& playerCompute_;
  ICrowdyStudioClientRuntime* clientRuntime_;
};

}  // namespace crowdy::studio
