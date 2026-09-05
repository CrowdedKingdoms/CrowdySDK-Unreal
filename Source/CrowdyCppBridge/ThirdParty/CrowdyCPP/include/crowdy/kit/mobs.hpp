#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// Runtime helpers for compute-module mob engines (the Wave 1 mob-engine
/// template / BWF's bwf-mobs): definition + slot reads off the model, the
/// refereed attack_mob invoke, and the type-77 contact-damage parser.
/// Mirrors CrowdyJS's kit.mobs.
///
/// Live mob poses arrive on the actor stream (kFlagMob, container-id
/// suffix) — decode them with kit/wire; this kit reads the durable side.
namespace crowdy::kit {

/// A parsed mob definition container.
struct KitMobDef {
  std::string containerId;
  std::string displayName;
  std::string mobId;
  std::int64_t maxHealth = 0;
  std::int64_t damage = 0;
  double speed = 0;
  bool hostile = false;
  std::string spawnTime;
};

/// A parsed mob slot container (durable state; live poses ride the lane).
struct KitMobSlot {
  std::string containerId;
  std::string displayName;
  std::string mobId;
  std::string actorUuid;
  std::int64_t health = 0;
  double x = 0, y = 0, z = 0;
  bool alive = false;
};

/// The referee's verdict for an accepted/denied attack.
struct KitAttackResult {
  bool success = false;
  std::optional<std::int64_t> health;
  std::optional<bool> killed;
  std::string reason;
};

class MobsKit {
 public:
  MobsKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
          std::string moduleName = "mob-engine", std::string defTypeName = "MobDef",
          std::string slotTypeName = "Mob")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        defTypeName_(std::move(defTypeName)),
        slotTypeName_(std::move(slotTypeName)) {}

  /// Is the mob engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Attack a live mob slot through the server referee: presence, range and
  /// damage clamps are validated engine-side before health moves. Denials
  /// resolve with success=false and the referee's reason.
  KitAttackResult attack(std::string_view containerId, std::int64_t amount = 1) {
    graphql::JVal params;
    params["containerId"] = containerId;
    params["amount"] = amount;
    EngineInvokeResult result = engines_.invoke(moduleName_, "attack_mob", params);
    KitAttackResult out;
    out.success = result.success;
    out.reason = result.reason;
    if (result.body["health"].isNumber()) out.health = result.body["health"].asInt64();
    if (result.body["killed"].isBool()) out.killed = result.body["killed"].asBool();
    return out;
  }

  /// The engine's status snapshot (mob/def counts, tick counter).
  EngineInvokeResult status() { return engines_.invoke(moduleName_, "status"); }

  /// List mob definitions with parsed stats.
  std::vector<KitMobDef> defs() {
    std::vector<KitMobDef> out;
    graphql::Json containers = gameModel_.containers(appId_, defTypeName_);
    containers.forEach([&](graphql::Json c) {
      graphql::Json props =
          kitContainerProperties(gameModel_, appId_, c["containerId"].asString());
      KitMobDef def;
      def.containerId = c["containerId"].asString();
      def.displayName = c["displayName"].asString();
      def.mobId = props["mob_id"].asString();
      def.maxHealth = props["max_health"].asInt64();
      def.damage = props["damage"].asInt64();
      def.speed = props["speed"].asDouble();
      def.hostile = props["hostile"].asBool() || props["hostile"].asString() == "true";
      def.spawnTime = props["spawn_time"].asString("any");
      out.push_back(std::move(def));
    });
    return out;
  }

  /// List mob slots (durable positions/health; alive = health > 0).
  std::vector<KitMobSlot> slots() {
    std::vector<KitMobSlot> out;
    graphql::Json containers = gameModel_.containers(appId_, slotTypeName_);
    containers.forEach([&](graphql::Json c) {
      graphql::Json props =
          kitContainerProperties(gameModel_, appId_, c["containerId"].asString());
      KitMobSlot slot;
      slot.containerId = c["containerId"].asString();
      slot.displayName = c["displayName"].asString();
      slot.mobId = props["mob_id"].asString();
      slot.actorUuid = props["actor_uuid"].asString();
      slot.health = props["health"].asInt64();
      slot.x = props["x"].asDouble();
      slot.y = props["y"].asDouble();
      slot.z = props["z"].asDouble();
      slot.alive = slot.health > 0;
      out.push_back(std::move(slot));
    });
    return out;
  }

  /// Parse a server-event payload as engine contact damage (type 77), or
  /// nullopt when it is another event type.
  static std::optional<ContactDamageEvent> parseContactDamage(const std::uint8_t* bytes,
                                                                    std::size_t len) {
    return parseContactDamage(bytes, len);
  }

 private:
  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
  std::string defTypeName_;
  std::string slotTypeName_;
};

}  // namespace crowdy::kit
