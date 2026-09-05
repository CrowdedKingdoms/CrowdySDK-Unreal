#pragma once

#include <exception>
#include <map>
#include <optional>
#include <string>

#include "crowdy/domains/compute.hpp"
#include "crowdy/graphql/json.hpp"

/// Compute-engine capability detection + invoke envelope for the kit
/// helpers — mirrors CrowdyJS's kit/engine. Games deploy engines
/// (mob-engine, world-engine, bwf-mobs, ...) as compute modules; kit helpers
/// probe once per module per client and route through the engine when it is
/// present, falling back to the model/automation path when it is not.
namespace crowdy::kit {

/// The `{success, ...}` envelope engine invoke exports resolve with.
struct EngineInvokeResult {
  bool success = false;
  /// The engine's denial reason when success is false.
  std::string reason;
  /// The parsed result body (includes any extra fields the export returns).
  graphql::Json body;
};

/// Per-client cached probe of which compute modules an app has enabled.
class EngineDetector {
 public:
  EngineDetector(std::string appId, domains::ComputeAPI* compute)
      : appId_(std::move(appId)), compute_(compute) {}

  /// Is `moduleName` deployed + enabled? Cached per module for the
  /// detector's lifetime (reset() to re-probe after deploys). False when no
  /// compute domain is wired.
  bool has(std::string_view moduleName) {
    if (!compute_) return false;
    auto it = cache_.find(std::string(moduleName));
    if (it != cache_.end()) return it->second;
    bool present = probe(moduleName);
    cache_.emplace(std::string(moduleName), present);
    return present;
  }

  /// Drop cached probes (call after deploying/enabling modules).
  void reset() { cache_.clear(); }

  /// Invoke an engine export and parse the `{success, ...}` envelope.
  /// Envelope failures (denials) return — transport errors are mapped to
  /// `{success: false, reason}` too, keeping call sites branch-free.
  EngineInvokeResult invoke(std::string_view moduleName, std::string_view exportName,
                            const graphql::JVal& params = graphql::JVal()) {
    EngineInvokeResult out;
    if (!compute_) {
      out.reason = "compute domain unavailable";
      return out;
    }
    try {
      graphql::Json result = compute_->invoke(
          appId_, moduleName, exportName, params.isNull() ? std::string() : params.dump());
      out.body = graphql::Json::parse(result["resultJson"].asString("{}"));
      out.success = out.body["success"].asBool(true);
      out.reason = out.body["reason"].asString();
      return out;
    } catch (const std::exception& error) {
      out.success = false;
      out.reason = error.what();
      return out;
    }
  }

 private:
  bool probe(std::string_view moduleName) {
    // Diagnostics read first (admin tokens); player tokens fall through to a
    // status invoke (engines all export one).
    try {
      graphql::Json module = compute_->module(appId_, moduleName);
      return module["enabled"].asBool(false);
    } catch (const std::exception&) {
      // FORBIDDEN (no view_compute_diagnostics) or missing — try the invoke.
    }
    EngineInvokeResult result = invoke(moduleName, "status");
    if (result.success) return true;
    const std::string& reason = result.reason;
    return !(reason.find("not found") != std::string::npos ||
             reason.find("No compute module") != std::string::npos ||
             reason.find("not enabled") != std::string::npos);
  }

  std::string appId_;
  domains::ComputeAPI* compute_;
  std::map<std::string, bool, std::less<>> cache_;
};

}  // namespace crowdy::kit
