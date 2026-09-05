#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// Realtime ability casts (Wave 3): server-side cooldown/resource/range
/// books, tick-stepped projectiles, delayed AoEs. The caster's position is
/// their live pose — `cast()` only carries the TARGET. Mirrors CrowdyJS's
/// kit.abilities.
namespace crowdy::kit {

/// One ability as the engine's loadout reports it.
struct KitAbility {
  std::string abilityId;
  std::int64_t cooldownMs = 0;
  std::int64_t resourceCost = 0;
  double range = 0.0;
  std::string kind;  ///< "instant" | "projectile" | "aoe"
};

class AbilitiesKit {
 public:
  AbilitiesKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
               std::string moduleName = "abilities-engine", std::string defTypeName = "AbilityDef")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        defTypeName_(std::move(defTypeName)) {}

  /// Is the abilities engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// STUDIO (admin) — create an ability definition container.
  Json defineAbility(std::string_view abilityId, const graphql::Json& spec,
                     std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = defTypeName_;
    input["displayName"] =
        displayName.empty() ? ("ability-" + std::string(abilityId)) : std::string(displayName);
    JArray properties;
    JVal idProp;
    idProp["key"] = "ability_id";
    idProp["valueType"] = "string";
    idProp["valueJson"] = JVal(abilityId).dump();
    properties.push_back(std::move(idProp));
    JVal specProp;
    specProp["key"] = "spec";
    specProp["valueType"] = "string";
    specProp["valueJson"] = JVal(spec.dump()).dump();
    properties.push_back(std::move(specProp));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Cast at a target point (your position comes from your live pose).
  Json cast(std::string_view abilityId, double targetX, double targetZ) {
    JVal params;
    params["abilityId"] = abilityId;
    params["targetX"] = targetX;
    params["targetZ"] = targetZ;
    return invoke("cast", params);
  }

  /// The deployed ability loadout.
  std::vector<KitAbility> loadout() {
    Json body = invoke("loadout", JVal());
    std::vector<KitAbility> out;
    body["abilities"].forEach([&](Json a) {
      KitAbility ability;
      ability.abilityId = a["abilityId"].asString();
      ability.cooldownMs = a["cooldownMs"].asInt64();
      ability.resourceCost = a["resourceCost"].asInt64();
      ability.range = a["range"].asDouble();
      ability.kind = a["kind"].asString();
      out.push_back(std::move(ability));
    });
    return out;
  }

  /// YOUR caster book: resource pool + live cooldowns.
  Json book() { return invoke("book", JVal()); }

  /// Engine totals (casters, live projectiles, hits).
  Json status() { return invoke("status", JVal()); }

  /// Parse a type-94 cast/impact server event.
  std::optional<AbilityEvent> parseAbilityEvent(const std::uint8_t* bytes, std::size_t len) {
    return crowdy::kit::parseAbilityEvent(bytes, len);
  }

 private:
  Json invoke(std::string_view exportName, const JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("abilities." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
  std::string defTypeName_;
};

}  // namespace crowdy::kit
