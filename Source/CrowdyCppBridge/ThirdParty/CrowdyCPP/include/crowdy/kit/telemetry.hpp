#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"

/// Telemetry helpers (Wave 3, model-thin): `track()` bumps sampled event
/// counters in ordinary containers under a `<area>.<action>` naming
/// convention; export/BI is a platform concern. Mirrors CrowdyJS's
/// kit.telemetry (the CPP `track` is synchronous — call it off the hot
/// path or from your own worker).
namespace crowdy::kit {

class TelemetryKit {
 public:
  TelemetryKit(std::string appId, domains::GameModelAPI& gameModel,
               std::string counterTypeName = "TelemetryCounter", double sampleRate = 1.0)
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        counterTypeName_(std::move(counterTypeName)),
        sampleRate_(sampleRate) {}

  /// Track one event: bump its sampled counter (errors are swallowed).
  void track(std::string_view name, const graphql::Json& props = graphql::Json()) {
    if (sampleRate_ < 1.0) {
      // Cheap deterministic-ish sampling without seeding a PRNG here.
      if (static_cast<double>(std::hash<std::string>{}(std::string(name)) % 1000) / 1000.0 >
          sampleRate_) {
        return;
      }
    }
    try {
      bump(name, props);
    } catch (...) {
      // fire-and-forget: telemetry never breaks gameplay
    }
  }

  /// The awaitable form of track() for tests/backfills (throws on errors).
  void bump(std::string_view name, const graphql::Json& props = graphql::Json()) {
    const std::string containerId = ensureCounter(name);
    Json current = kitContainerProperties(gameModel_, appId_, containerId);
    setProperty(containerId, "count", "int", std::to_string(current["count"].asInt64() + 1));
    if (!props.isNull()) {
      setProperty(containerId, "last_props", "string",
                  JVal(props.dump().substr(0, 500)).dump());
    }
  }

  /// Read the sampled counters: (eventName, count, lastProps) rows.
  std::vector<std::tuple<std::string, std::int64_t, std::string>> counters() {
    std::vector<std::tuple<std::string, std::int64_t, std::string>> out;
    Json containers = gameModel_.containers(appId_, counterTypeName_);
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asString());
      out.emplace_back(props["event_name"].asString(), props["count"].asInt64(),
                       props["last_props"].asString());
    });
    return out;
  }

 private:
  void setProperty(std::string_view containerId, const char* key, const char* valueType,
                   std::string valueJson) {
    JVal input;
    input["appId"] = appId_;
    input["containerId"] = containerId;
    input["key"] = key;
    input["valueType"] = valueType;
    input["valueJson"] = std::move(valueJson);
    gameModel_.setProperty(input);
  }

  std::string ensureCounter(std::string_view name) {
    auto cached = counterIds_.find(std::string(name));
    if (cached != counterIds_.end()) return cached->second;
    Json containers = gameModel_.containers(appId_, counterTypeName_);
    std::string found;
    containers.forEach([&](Json c) {
      if (!found.empty()) return;
      const std::string id = c["containerId"].asString();
      Json props = kitContainerProperties(gameModel_, appId_, id);
      if (props["event_name"].asString() == name) found = id;
    });
    if (found.empty()) {
      JVal input;
      input["appId"] = appId_;
      input["typeName"] = counterTypeName_;
      input["displayName"] = "telemetry " + std::string(name);
      JArray properties;
      JVal nameProp;
      nameProp["key"] = "event_name";
      nameProp["valueType"] = "string";
      nameProp["valueJson"] = JVal(name).dump();
      properties.push_back(std::move(nameProp));
      JVal countProp;
      countProp["key"] = "count";
      countProp["valueType"] = "int";
      countProp["valueJson"] = "0";
      properties.push_back(std::move(countProp));
      input["properties"] = JVal(std::move(properties));
      found = gameModel_.createContainer(input)["containerId"].asString();
    }
    counterIds_.emplace(std::string(name), found);
    return found;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  std::string counterTypeName_;
  double sampleRate_;
  std::unordered_map<std::string, std::string> counterIds_;
};

}  // namespace crowdy::kit
