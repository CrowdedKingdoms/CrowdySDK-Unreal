#pragma once

#include <stdexcept>
#include <string>

#include "crowdy/kit/engine.hpp"

/// Instance helpers (Wave 2): private world slices with lifecycle
/// (open/join/complete), per-run seeds, and reserved disjoint chunk
/// volumes over the instance-engine. Mirrors CrowdyJS's kit.instances.
namespace crowdy::kit {

class InstancesKit {
 public:
  InstancesKit(std::string appId, EngineDetector& engines,
               std::string moduleName = "instance-engine")
      : appId_(std::move(appId)), engines_(engines), moduleName_(std::move(moduleName)) {}

  /// Is the instance engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Open a new instance (caller becomes creator + first member).
  graphql::Json open(std::string_view name = {}, std::string_view seed = {},
                     std::string_view sessionId = {}) {
    graphql::JVal params;
    if (!name.empty()) params["name"] = name;
    if (!seed.empty()) params["seed"] = seed;
    if (!sessionId.empty()) params["sessionId"] = sessionId;
    return invoke("open", params);
  }

  /// Join an open instance.
  graphql::Json join(std::string_view instanceId) {
    graphql::JVal params;
    params["instanceId"] = instanceId;
    return invoke("join", params);
  }

  /// Complete an instance (member-only; announces instance_completed).
  graphql::Json complete(std::string_view instanceId, std::string_view outcome = {}) {
    graphql::JVal params;
    params["instanceId"] = instanceId;
    if (!outcome.empty()) params["outcome"] = outcome;
    return invoke("complete", params);
  }

  /// One instance's state (or engine totals with an empty id).
  graphql::Json state(std::string_view instanceId = {}) {
    graphql::JVal params;
    if (!instanceId.empty()) params["instanceId"] = instanceId;
    return invoke("state", params);
  }

 private:
  graphql::Json invoke(std::string_view exportName, const graphql::JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("instances." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  EngineDetector& engines_;
  std::string moduleName_;
};

}  // namespace crowdy::kit
