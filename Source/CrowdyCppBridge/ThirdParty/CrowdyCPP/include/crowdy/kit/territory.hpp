#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// Control points + factions (Wave 3): live capture state, faction
/// standings, admin map CRUD. Capture is pure presence; flips announce as
/// type-96 events. Mirrors CrowdyJS's kit.territory.
namespace crowdy::kit {

/// A control point as the engine reports it.
struct KitControlPoint {
  std::string pointId;
  double x = 0.0;
  double z = 0.0;
  double radius = 0.0;
  std::string owner;
  std::string challenger;
  double progress = 0.0;
  std::int64_t incomePerMin = 0;
  bool siegeOpen = true;
};

class TerritoryKit {
 public:
  TerritoryKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
               std::string moduleName = "territory")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)) {}

  /// Is the territory engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Every control point with live capture state.
  std::vector<KitControlPoint> points() {
    Json body = invoke("points", JVal());
    std::vector<KitControlPoint> out;
    body["points"].forEach([&](Json p) {
      KitControlPoint point;
      point.pointId = p["pointId"].asString();
      point.x = p["x"].asDouble();
      point.z = p["z"].asDouble();
      point.radius = p["radius"].asDouble();
      point.owner = p["owner"].asString();
      point.challenger = p["challenger"].asString();
      point.progress = p["progress"].asDouble();
      point.incomePerMin = p["incomePerMin"].asInt64();
      point.siegeOpen = p["siegeOpen"].asBool();
      out.push_back(std::move(point));
    });
    return out;
  }

  /// Faction standings: (factionId, gold) pairs.
  std::vector<std::pair<std::string, std::int64_t>> factions() {
    Json body = invoke("factions", JVal());
    std::vector<std::pair<std::string, std::int64_t>> out;
    body["factions"].forEach(
        [&](Json f) { out.emplace_back(f["factionId"].asString(), f["gold"].asInt64()); });
    return out;
  }

  /// Engine totals (flips, income paid).
  Json status() { return invoke("status", JVal()); }

  /// STUDIO (admin) — create a faction.
  Json defineFaction(std::string_view factionId, std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = "Faction";
    input["displayName"] =
        displayName.empty() ? ("faction-" + std::string(factionId)) : std::string(displayName);
    JArray properties;
    properties.push_back(property("faction_id", "string", JVal(factionId).dump()));
    properties.push_back(property("gold", "int", "0"));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// STUDIO (admin) — enroll a player in a faction.
  Json enroll(std::string_view userId, std::string_view factionId) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = "FactionMember";
    input["displayName"] = "member-" + std::string(userId);
    JArray properties;
    properties.push_back(property("user_id", "string", JVal(userId).dump()));
    properties.push_back(property("faction_id", "string", JVal(factionId).dump()));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// STUDIO (admin) — create a control point.
  Json definePoint(std::string_view pointId, std::int64_t x, std::int64_t z, std::int64_t radius,
                   std::int64_t incomePerMin = 0,
                   std::optional<std::int64_t> siegeFromMin = std::nullopt,
                   std::optional<std::int64_t> siegeToMin = std::nullopt,
                   std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = "ControlPoint";
    input["displayName"] =
        displayName.empty() ? ("cp-" + std::string(pointId)) : std::string(displayName);
    JArray properties;
    properties.push_back(property("point_id", "string", JVal(pointId).dump()));
    properties.push_back(property("x", "int", std::to_string(x)));
    properties.push_back(property("z", "int", std::to_string(z)));
    properties.push_back(property("radius", "int", std::to_string(radius)));
    properties.push_back(property("owner_faction", "string", "\"\""));
    properties.push_back(property("income_per_min", "int", std::to_string(incomePerMin)));
    if (siegeFromMin) {
      properties.push_back(property("siege_from_min", "int", std::to_string(*siegeFromMin)));
    }
    if (siegeToMin) {
      properties.push_back(property("siege_to_min", "int", std::to_string(*siegeToMin)));
    }
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Parse a type-96 control-point server event (flips).
  std::optional<ControlPointEvent> parseControlPoint(const std::uint8_t* bytes, std::size_t len) {
    return parseControlPointEvent(bytes, len);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  Json invoke(std::string_view exportName, const JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("territory." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
};

}  // namespace crowdy::kit
