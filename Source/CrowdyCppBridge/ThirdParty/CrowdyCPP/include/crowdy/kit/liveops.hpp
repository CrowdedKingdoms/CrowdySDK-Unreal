#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// Liveops helpers (Wave 3): event windows (admin CRUD + active reads —
/// engine-backed when the liveops-scheduler is deployed), seasons with
/// battle-pass composition, and the type-98 zone-change parser. Mirrors
/// CrowdyJS's kit.liveops.
namespace crowdy::kit {

/// A parsed event window.
struct KitEventWindow {
  std::string containerId;
  std::string windowId;
  bool active = false;
  std::int64_t opensAtMs = 0;
  std::int64_t closesAtMs = 0;
  graphql::Json modifiers;
};

/// A parsed season definition (+ its battle-pass composition).
struct KitSeason {
  std::string containerId;
  std::string seasonId;
  bool active = false;
  std::int64_t startsAtMs = 0;
  std::int64_t endsAtMs = 0;
  std::string passTrack;
  std::vector<std::string> passFeatures;
};

class LiveopsKit {
 public:
  LiveopsKit(std::string appId, domains::GameModelAPI& gameModel,
             std::string_view typePrefix = {}, EngineDetector* engines = nullptr,
             std::string_view moduleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(moduleName.empty() ? std::string("liveops-scheduler")
                                       : std::string(moduleName)) {
    const std::string prefix(typePrefix);
    windowType_ = prefix + "EventWindow";
    seasonType_ = prefix + "SeasonDef";
    const std::string fnPrefix = prefix.empty() ? "" : toSnakeCase(prefix) + "_";
    openWindowFn_ = fnPrefix + "open_window";
    closeWindowFn_ = fnPrefix + "close_window";
    activateSeasonFn_ = fnPrefix + "activate_season";
  }

  /// Is the liveops-scheduler engine deployed + enabled (cached)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(moduleName_); }

  /// STUDIO (admin) — create an event window.
  Json defineWindow(std::string_view windowId, const graphql::Json& modifiers,
                    std::optional<std::int64_t> opensAtMs = std::nullopt,
                    std::optional<std::int64_t> closesAtMs = std::nullopt,
                    std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = windowType_;
    input["displayName"] =
        displayName.empty() ? ("Window " + std::string(windowId)) : std::string(displayName);
    JArray properties;
    properties.push_back(property("window_id", "string", JVal(windowId).dump()));
    if (opensAtMs) properties.push_back(property("opens_at_ms", "int", std::to_string(*opensAtMs)));
    if (closesAtMs) {
      properties.push_back(property("closes_at_ms", "int", std::to_string(*closesAtMs)));
    }
    properties.push_back(property("modifiers", "string", JVal(modifiers.dump()).dump()));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Every window (model read).
  std::vector<KitEventWindow> windows() {
    std::vector<KitEventWindow> out;
    Json containers = gameModel_.containers(appId_, windowType_);
    containers.forEach([&](Json c) {
      const std::string id = c["containerId"].asString();
      Json props = kitContainerProperties(gameModel_, appId_, id);
      KitEventWindow window;
      window.containerId = id;
      window.windowId = props["window_id"].asString();
      window.active = props["active"].asBool();
      window.opensAtMs = props["opens_at_ms"].asInt64();
      window.closesAtMs = props["closes_at_ms"].asInt64();
      window.modifiers = graphql::Json::parse(props["modifiers"].asString());
      out.push_back(std::move(window));
    });
    return out;
  }

  /// The ACTIVE windows (engine-authoritative when the scheduler runs).
  std::vector<KitEventWindow> activeWindows() {
    auto all = windows();
    if (engines_ != nullptr && engineAvailable()) {
      EngineInvokeResult result = engines_->invoke(moduleName_, "active_windows", JVal());
      if (result.success) {
        std::vector<std::string> activeIds;
        result.body["windows"].forEach(
            [&](Json w) { activeIds.push_back(w["windowId"].asString()); });
        std::vector<KitEventWindow> filtered;
        for (auto& window : all) {
          for (const auto& id : activeIds) {
            if (window.windowId == id) {
              filtered.push_back(window);
              break;
            }
          }
        }
        return filtered;
      }
    }
    std::vector<KitEventWindow> active;
    for (auto& window : all) {
      if (window.active) active.push_back(window);
    }
    return active;
  }

  /// STUDIO (admin/automation) — open a window through the model function.
  KitInvokeResult openWindow(std::string_view windowContainerId, std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, openWindowFn_, windowContainerId, JVal(), sessionId);
  }

  /// STUDIO (admin/automation) — close a window through the model function.
  KitInvokeResult closeWindow(std::string_view windowContainerId, std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, closeWindowFn_, windowContainerId, JVal(), sessionId);
  }

  /// STUDIO (admin) — create a season (+ battle-pass composition).
  Json defineSeason(std::string_view seasonId, std::string_view passTrack = {},
                    const std::vector<std::string>& passFeatures = {},
                    std::optional<std::int64_t> startsAtMs = std::nullopt,
                    std::optional<std::int64_t> endsAtMs = std::nullopt,
                    std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = seasonType_;
    input["displayName"] =
        displayName.empty() ? ("Season " + std::string(seasonId)) : std::string(displayName);
    JArray properties;
    properties.push_back(property("season_id", "string", JVal(seasonId).dump()));
    if (!passTrack.empty()) {
      properties.push_back(property("pass_track", "string", JVal(passTrack).dump()));
    }
    JArray features;
    for (const auto& feature : passFeatures) features.push_back(JVal(feature));
    properties.push_back(property("pass_features", "string", JVal(JVal(std::move(features)).dump()).dump()));
    if (startsAtMs) properties.push_back(property("starts_at_ms", "int", std::to_string(*startsAtMs)));
    if (endsAtMs) properties.push_back(property("ends_at_ms", "int", std::to_string(*endsAtMs)));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Every season (model read).
  std::vector<KitSeason> seasons() {
    std::vector<KitSeason> out;
    Json containers = gameModel_.containers(appId_, seasonType_);
    containers.forEach([&](Json c) {
      const std::string id = c["containerId"].asString();
      Json props = kitContainerProperties(gameModel_, appId_, id);
      KitSeason season;
      season.containerId = id;
      season.seasonId = props["season_id"].asString();
      season.active = props["active"].asBool();
      season.startsAtMs = props["starts_at_ms"].asInt64();
      season.endsAtMs = props["ends_at_ms"].asInt64();
      season.passTrack = props["pass_track"].asString();
      graphql::Json::parse(props["pass_features"].asString())
          .forEach([&](Json f) { season.passFeatures.push_back(f.asString()); });
      out.push_back(std::move(season));
    });
    return out;
  }

  /// The active season, when one exists.
  std::optional<KitSeason> currentSeason() {
    for (auto& season : seasons()) {
      if (season.active) return season;
    }
    return std::nullopt;
  }

  /// STUDIO (admin/automation) — activate a season through the model function.
  KitInvokeResult activateSeason(std::string_view seasonContainerId,
                                 std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, activateSeasonFn_, seasonContainerId, JVal(), sessionId);
  }

  /// Parse a type-98 zone-change server event (BR circles, event areas).
  std::optional<ZoneChangeEvent> parseZoneChange(const std::uint8_t* bytes, std::size_t len) {
    return parseZoneChangeEvent(bytes, len);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector* engines_ = nullptr;
  std::string moduleName_;
  std::string windowType_;
  std::string seasonType_;
  std::string openWindowFn_;
  std::string closeWindowFn_;
  std::string activateSeasonFn_;
};

}  // namespace crowdy::kit
