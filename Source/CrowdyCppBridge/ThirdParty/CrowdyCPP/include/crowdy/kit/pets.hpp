#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Runtime helpers for engine-driven pets (the Wave 1 npc-engine template):
/// Pet containers hold species/name/owner/bond; the engine walks active pets
/// after their owner (kit-ai follow_owner) and streams kFlagNpc actor poses
/// with the pet's container id as the payload suffix. Mirrors CrowdyJS's
/// kit.pets.
namespace crowdy::kit {

/// A parsed pet container.
struct KitPet {
  std::string containerId;
  std::string displayName;
  std::string species;
  std::string name;
  std::string ownerUserId;
  std::int64_t bond = 0;
  bool active = true;
  std::string actorUuid;
  double x = 0, y = 0, z = 0;
};

/// An engine verdict for a pet invoke.
struct KitPetResult {
  bool success = false;
  std::string reason;
};

class PetsKit {
 public:
  PetsKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
          std::string moduleName = "npc-engine", std::string typeName = "Pet")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        typeName_(std::move(typeName)) {}

  /// Is the pet-driving npc engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Adopt a pet: creates the caller-owned Pet container (active). Member
  /// instantiation defaults the owner to the caller; admin tokens must pass
  /// ownerUserId explicitly (the engine validates ownership on every
  /// summon/dismiss/rename).
  graphql::Json adopt(std::string_view species, std::string_view name,
                      std::optional<std::array<double, 3>> position = std::nullopt,
                      std::string_view ownerUserId = {}) {
    graphql::JVal input;
    input["appId"] = appId_;
    input["typeName"] = typeName_;
    input["displayName"] = name;
    if (!ownerUserId.empty()) input["ownerUserId"] = ownerUserId;
    graphql::JArray properties;
    properties.push_back(property("species", "string", graphql::JVal(species).dump()));
    properties.push_back(property("name", "string", graphql::JVal(name).dump()));
    properties.push_back(property("bond", "int", "0"));
    properties.push_back(property("active", "string", "\"true\""));
    if (position) {
      properties.push_back(
          property("x", "int", std::to_string(static_cast<std::int64_t>(std::llround((*position)[0])))));
      properties.push_back(
          property("y", "int", std::to_string(static_cast<std::int64_t>(std::llround((*position)[1])))));
      properties.push_back(
          property("z", "int", std::to_string(static_cast<std::int64_t>(std::llround((*position)[2])))));
    }
    input["properties"] = graphql::JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// List pets (all, or one owner's when ownerUserId is non-empty).
  std::vector<KitPet> list(std::string_view ownerUserId = {}) {
    std::vector<KitPet> out;
    graphql::Json containers = gameModel_.containers(appId_, typeName_);
    containers.forEach([&](graphql::Json c) {
      std::string owner = c["ownerUserId"].isNull()
                              ? std::string()
                              : std::to_string(c["ownerUserId"].asBigInt());
      if (!ownerUserId.empty() && owner != ownerUserId) return;
      graphql::Json props =
          kitContainerProperties(gameModel_, appId_, c["containerId"].asString());
      KitPet pet;
      pet.containerId = c["containerId"].asString();
      pet.displayName = c["displayName"].asString();
      pet.species = props["species"].asString();
      pet.name = props["name"].asString(pet.displayName);
      pet.ownerUserId = owner;
      pet.bond = props["bond"].asInt64();
      pet.active = props["active"].asString("true") != "false";
      pet.actorUuid = props["actor_uuid"].asString();
      pet.x = props["x"].asDouble();
      pet.y = props["y"].asDouble();
      pet.z = props["z"].asDouble();
      out.push_back(std::move(pet));
    });
    return out;
  }

  /// Summon your pet: it starts following you (owner-validated engine-side).
  KitPetResult summon(std::string_view containerId) {
    return petInvoke("summon", containerId);
  }

  /// Dismiss your pet: it stops simulating until summoned again.
  KitPetResult dismiss(std::string_view containerId) {
    return petInvoke("dismiss", containerId);
  }

  /// Rename your pet (1-32 chars; owner-validated engine-side).
  KitPetResult rename(std::string_view containerId, std::string_view name) {
    graphql::JVal params;
    params["containerId"] = containerId;
    params["name"] = name;
    EngineInvokeResult result = engines_.invoke(moduleName_, "rename_pet", params);
    return { result.success, result.reason };
  }

 private:
  static graphql::JVal property(const char* key, const char* valueType, std::string valueJson) {
    graphql::JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitPetResult petInvoke(std::string_view exportName, std::string_view containerId) {
    graphql::JVal params;
    params["containerId"] = containerId;
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    return { result.success, result.reason };
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
  std::string typeName_;
};

}  // namespace crowdy::kit
