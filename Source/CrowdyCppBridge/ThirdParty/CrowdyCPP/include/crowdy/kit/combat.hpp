#pragma once

#include <optional>
#include <random>
#include <vector>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Combat — server-authoritative combat (the turn-based / MMO-durable tier):
/// Combatant containers (hp / attack / defense / alive), an attack function
/// whose damage formula and death flip run entirely server-side, status
/// effects as StatusEffect containers ticked by an interval automation
/// (damage-over-time between requests), and respawn/revive.
///
/// The effect tick runs only while the app has a player in it (2026-09-01),
/// which for combat is usually what you want -- nothing is fighting in an empty
/// world. Store an expiry TIMESTAMP on each effect rather than a remaining-ticks
/// counter, so an effect that should have lapsed while the app was empty is
/// treated as lapsed instead of resuming with time left on it.
///
/// The effect tick uses the selector join pattern: each combatant carries a
/// unique combat_key, effects record a target_key, and the automation's
/// selector binds the matching combatant as a $target ref param
/// (where combat_key == self.target_key) — automations cannot follow property
/// refs directly.
///
/// For fast-twitch combat keep the per-frame simulation on the replication
/// plane under host authority and set hostSynced to get the is_host-gated
/// durable sync function. Mirrors CrowdyJS's combatBlueprint / kit.combat.
namespace crowdy::kit {

/// Optional revive gate: a revive function usable on any downed combatant by
/// holders of this team/group permission (e.g. a healer role).
struct CombatReviveGroup {
  std::string groupId;
  std::string permission;  ///< empty = any member of the group
};

struct CombatBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  /// Turn-based mode: adds is_current_turn to the attack/apply_effect
  /// policies, so only the player whose session turn it is may act.
  bool turnBased = false;
  /// Host-synced mode for fast combat: adds a sync_combatant function gated
  /// is_host so the elected host client can run smooth per-frame combat on
  /// the replication plane and periodically write the durable hp back.
  bool hostSynced = false;
  /// Interval of the status-effect tick automation, in ms (dispatcher floor
  /// is seconds). Defaults to 5000.
  int effectTickIntervalMs = 5000;
  /// Who may instantiate combatants: "member" (player characters; the
  /// default) or "admin" (studio-spawned mobs).
  std::string combatantInstantiableBy = "member";
  /// When set, adds a revive function gated on this team/group permission,
  /// usable on any downed combatant.
  std::optional<CombatReviveGroup> reviveGroup;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by combatBlueprint for a given prefix.
struct CombatNames {
  std::string combatantType;
  std::string effectType;
  std::string attackFn;
  std::string applyEffectFn;
  std::string effectTickFn;
  std::string respawnFn;
  std::string reviveFn;
  std::string syncFn;
  std::string effectTickAutomation;
};

/// Compute the type/function names a combat blueprint (and its runtime
/// helper) uses.
inline CombatNames combatNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (auto& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  CombatNames n;
  n.combatantType = std::string(typePrefix) + "Combatant";
  n.effectType = std::string(typePrefix) + "StatusEffect";
  n.attackFn = fnPrefix + "attack";
  n.applyEffectFn = fnPrefix + "apply_effect";
  n.effectTickFn = fnPrefix + "effect_tick";
  n.respawnFn = fnPrefix + "respawn";
  n.reviveFn = fnPrefix + "revive";
  n.syncFn = fnPrefix + "sync_combatant";
  n.effectTickAutomation = autoPrefix + "effect-tick";
  return n;
}

/// Build the combat blueprint. Runtime counterpart: CombatKit.
inline KitBlueprint combatBlueprint(const CombatBlueprintOptions& options = {}) {
  const CombatNames names = combatNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;

  auto actorPolicy = [&](std::string_view condition) {
    JArray rules;
    rules.push_back(ownerOfSelfPolicy());
    if (options.turnBased) rules.push_back(isCurrentTurnPolicy());
    rules.push_back(conditionPolicy(condition));
    return kitPolicyJson(andPolicy(std::move(rules)));
  };

  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        const char* defaultJson, const char* description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = defaultJson;
    p["description"] = description;
    return p;
  };
  auto param = [](const char* name, const char* valueType, const char* description) {
    JVal p;
    p["name"] = name;
    p["valueType"] = valueType;
    p["required"] = true;
    p["description"] = description;
    return p;
  };
  auto mutation = [](const char* target, const char* property, const char* expression) {
    JVal m;
    m["target"] = target;
    m["property"] = property;
    m["expression"] = expression;
    return m;
  };

  KitBlueprint bp;
  bp.name = names.combatantType;

  {
    JVal t;
    t["typeName"] = names.combatantType;
    t["displayName"] = names.combatantType;
    t["instantiableBy"] = options.combatantInstantiableBy;
    t["description"] = "A combat participant with server-authoritative hp/stats.";
    bp.containerTypes.push_back(std::move(t));
  }
  {
    JVal t;
    t["typeName"] = names.effectType;
    t["displayName"] = names.effectType;
    t["instantiableBy"] = "member";
    t["description"] = "An armed status effect ticked by the server automation.";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.combatantType, kind));
  bp.propertyDefinitions.push_back(propertyDef(
      names.combatantType, "combat_key", "string", "\"\"",
      "Unique join key the status-effect automation selector matches against effect "
      "target_key."));
  bp.propertyDefinitions.push_back(propertyDef(names.combatantType, "hp", "int", "100",
                                               "Current hit points (never below 0)."));
  bp.propertyDefinitions.push_back(propertyDef(names.combatantType, "max_hp", "int", "100",
                                               "Hit point ceiling (respawn/heal target)."));
  bp.propertyDefinitions.push_back(propertyDef(names.combatantType, "attack", "int", "10",
                                               "Attack stat fed into the damage formula."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.combatantType, "defense", "int", "0", "Damage reduction stat."));
  bp.propertyDefinitions.push_back(propertyDef(names.combatantType, "alive", "bool", "true",
                                               "Flipped server-side when hp reaches 0."));
  bp.propertyDefinitions.push_back(propertyDef(
      names.combatantType, "respawn_x", "float", "0",
      "Respawn anchor (informational; movement is the replication plane)."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.combatantType, "respawn_y", "float", "0", "Respawn anchor."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.combatantType, "respawn_z", "float", "0", "Respawn anchor."));
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.effectType, kind));
  bp.propertyDefinitions.push_back(propertyDef(names.effectType, "effect_id", "string", "\"\"",
                                               "Effect identifier (e.g. 'poison', 'burn')."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.effectType, "target_key", "string", "\"\"",
                  "The target combatant's combat_key (selector join key)."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.effectType, "magnitude", "int", "0", "Damage applied per tick."));
  bp.propertyDefinitions.push_back(propertyDef(
      names.effectType, "ticks_left", "int", "0",
      "Remaining ticks; the automation only selects effects above 0."));

  {
    JVal fn;
    fn["name"] = names.attackFn;
    fn["containerTypeName"] = names.combatantType;
    fn["returnType"] = "int";
    fn["parameters"] =
        JVal::array({param("target_id", "container_ref", "The combatant being attacked.")});
    fn["mutations"] = JVal::array(
        {mutation("ref($target_id)", "hp",
                  "max(0, ref($target_id).hp - max(1, self.attack - ref($target_id).defense))"),
         mutation("ref($target_id)", "alive", "ref($target_id).hp > 0")});
    fn["returnExpression"] = "ref($target_id).hp";
    fn["invokePolicyJson"] = actorPolicy("self.alive && ref($target_id).alive");
    fn["description"] =
        "Attack a target: the damage formula (attack vs defense, min 1) and the death flip run "
        "server-side in one transaction.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.applyEffectFn;
    fn["containerTypeName"] = names.effectType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("target_key", "string", "The target combatant's combat_key."),
         param("effect_id", "string", "Effect identifier."),
         param("magnitude", "int", "Damage per tick."),
         param("ticks", "int", "Number of ticks the effect lasts.")});
    fn["mutations"] =
        JVal::array({mutation("self", "effect_id", "$effect_id"),
                     mutation("self", "target_key", "$target_key"),
                     mutation("self", "magnitude", "max(0, $magnitude)"),
                     mutation("self", "ticks_left", "$ticks")});
    fn["returnExpression"] = "self.ticks_left";
    fn["invokePolicyJson"] = actorPolicy("$ticks > 0 && self.ticks_left == 0");
    fn["description"] =
        "Arm a status effect against a target combat_key; the interval automation applies it "
        "tick by tick.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.effectTickFn;
    fn["containerTypeName"] = names.effectType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param(
        "target", "container_ref",
        "The affected combatant — bound by the automation selector join (combat_key == "
        "target_key).")});
    fn["mutations"] = JVal::array(
        {mutation("ref($target)", "hp", "max(0, ref($target).hp - self.magnitude)"),
         mutation("ref($target)", "alive", "ref($target).hp > 0"),
         mutation("self", "ticks_left", "self.ticks_left - 1")});
    fn["returnExpression"] = "self.ticks_left";
    fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
    fn["autonomousInvocable"] = true;
    fn["description"] =
        "Server-driven damage-over-time tick (automation-only): applies magnitude to the joined "
        "target and decrements ticks_left.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.respawnFn;
    fn["containerTypeName"] = names.combatantType;
    fn["returnType"] = "int";
    fn["mutations"] = JVal::array({mutation("self", "hp", "self.max_hp"),
                                   mutation("self", "alive", "true")});
    fn["returnExpression"] = "self.hp";
    fn["invokePolicyJson"] = kitPolicyJson(
        andPolicy({ownerOfSelfPolicy(), conditionPolicy("not(self.alive)")}));
    fn["description"] =
        "Respawn your downed combatant at full hp (move the actor to the respawn anchor "
        "client-side).";
    bp.functions.push_back(std::move(fn));
  }

  if (options.reviveGroup) {
    JVal fn;
    fn["name"] = names.reviveFn;
    fn["containerTypeName"] = names.combatantType;
    fn["returnType"] = "int";
    fn["mutations"] = JVal::array({mutation("self", "hp", "self.max_hp"),
                                   mutation("self", "alive", "true")});
    fn["returnExpression"] = "self.hp";
    fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
        {groupPermissionPolicy(options.reviveGroup->groupId, options.reviveGroup->permission),
         conditionPolicy("not(self.alive)")}));
    fn["description"] = "Revive any downed combatant (team/group-permission gated).";
    bp.functions.push_back(std::move(fn));
  }

  if (options.hostSynced) {
    JVal fn;
    fn["name"] = names.syncFn;
    fn["containerTypeName"] = names.combatantType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("hp", "int", "The host-simulated hp to persist (clamped to 0..max_hp).")});
    fn["mutations"] = JVal::array({mutation("self", "hp", "clamp($hp, 0, self.max_hp)"),
                                   mutation("self", "alive", "self.hp > 0")});
    fn["returnExpression"] = "self.hp";
    fn["invokePolicyJson"] = kitPolicyJson(isHostPolicy());
    fn["description"] =
        "Persist host-simulated combat state (is_host enforced): the fast tier runs on the "
        "replication plane, durable hp syncs here at low frequency.";
    bp.functions.push_back(std::move(fn));
  }

  {
    JVal selector;
    {
      JVal w;
      w["key"] = "ticks_left";
      w["op"] = ">";
      w["value"] = 0;
      selector["selfWhere"] = JVal::array({std::move(w)});
    }
    selector["ofType"] = names.combatantType;
    {
      JVal w;
      w["key"] = "combat_key";
      w["op"] = "==";
      w["value"] = "self.target_key";
      selector["where"] = JVal::array({std::move(w)});
    }
    selector["bindAs"]["ref"] = "target";
    JVal a;
    a["name"] = names.effectTickAutomation;
    a["functionName"] = names.effectTickFn;
    a["targetMode"] = "type";
    a["targetTypeName"] = names.effectType;
    a["triggerType"] = "schedule";
    a["scheduleKind"] = "interval";
    a["intervalMs"] = options.effectTickIntervalMs;
    a["maxTargets"] = 32;
    a["selectorJson"] = selector.dump();
    a["description"] =
        "Ticks active status effects: joins each effect to its target combatant and applies "
        "damage-over-time.";
    bp.automations.push_back(std::move(a));
  }

  return bp;
}

/// A parsed view of one combatant.
struct KitCombatant {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when unowned
  std::string combatKey;
  std::int64_t hp = 0;
  std::int64_t maxHp = 0;
  std::int64_t attack = 0;
  std::int64_t defense = 0;
  bool alive = false;
};

/// A parsed view of one status effect.
struct KitStatusEffect {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when unowned
  std::string effectId;
  std::string targetKey;
  std::int64_t magnitude = 0;
  std::int64_t ticksLeft = 0;
};

/// Input for CombatKit::spawnCombatant.
struct KitCombatantSpawn {
  std::string ownerUserId;
  std::string displayName;
  std::optional<std::int64_t> hp;  ///< defaults to maxHp
  std::int64_t maxHp = 100;
  std::int64_t attack = 10;
  std::int64_t defense = 0;
  std::string combatKey;  ///< empty = derived from ownerUserId + a random suffix
  std::string sessionId;
  JArray properties;  ///< extra SeedPropertyInput objects
};

/// Runtime helpers for the combatBlueprint conventions: spawn combatants,
/// attack (server-side damage formula + death flip), arm status effects the
/// tick automation applies over time, respawn, and — with hostSynced —
/// persist host-simulated hp. Denials resolve with success=false.
/// The verdict of a routed attack (CombatKit::attackRouted).
struct KitRoutedAttack {
  bool success = false;
  std::optional<std::int64_t> health;  ///< remaining health/hp after an accepted hit
  std::optional<bool> killed;
  std::string reason;  ///< denial reason (engine) or error message (model)
  bool viaEngine = false;
};

class CombatKit {
 public:
  CombatKit(std::string appId, domains::GameModelAPI& gameModel,
            std::string_view typePrefix = {}, EngineDetector* engines = nullptr,
            std::string_view engineModuleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        names_(combatNames(typePrefix)),
        engines_(engines),
        engineModuleName_(engineModuleName.empty() ? std::string("mob-engine")
                                                   : std::string(engineModuleName)) {}

  const CombatNames& names() const { return names_; }

  /// Route an attack through the strongest available authority: the compute
  /// referee (attack_mob on the mob engine — presence/range validated
  /// server-side) when the engine is deployed (capability-detected, cached),
  /// else today's model attack function. One call, both deployments.
  /// attackerId is required for the model path only.
  KitRoutedAttack attackRouted(std::string_view targetId, std::string_view attackerId = {},
                               std::int64_t amount = 1) {
    KitRoutedAttack out;
    if (engines_ != nullptr && engines_->has(engineModuleName_)) {
      JVal params;
      params["containerId"] = targetId;
      params["amount"] = amount;
      EngineInvokeResult result = engines_->invoke(engineModuleName_, "attack_mob", params);
      out.success = result.success;
      out.reason = result.reason;
      if (result.body["health"].isNumber()) out.health = result.body["health"].asInt64();
      if (result.body["killed"].isBool()) out.killed = result.body["killed"].asBool();
      out.viaEngine = true;
      return out;
    }
    if (attackerId.empty()) {
      out.reason = "attackerId is required for the model combat path";
      return out;
    }
    KitInvokeResult result = attack(attackerId, targetId);
    out.success = result.success;
    out.reason = result.errorMessage;
    if (result.returnValue.isNumber()) out.health = result.returnValue.asInt64();
    return out;
  }

  /// Spawn a combatant with a unique combat_key (the status-effect join key)
  /// and the owner_user_id mirror.
  Json spawnCombatant(const KitCombatantSpawn& input) {
    std::string combatKey = input.combatKey;
    if (combatKey.empty()) {
      combatKey = "ck-" + input.ownerUserId + "-" + randomHexSuffix();
    }
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.combatantType;
    vars["displayName"] = input.displayName;
    if (!input.sessionId.empty()) vars["sessionId"] = input.sessionId;
    JArray properties;
    properties.push_back(property("owner_user_id", "int", input.ownerUserId));
    properties.push_back(property("combat_key", "string", JVal(combatKey).dump()));
    properties.push_back(
        property("hp", "int", std::to_string(input.hp ? *input.hp : input.maxHp)));
    properties.push_back(property("max_hp", "int", std::to_string(input.maxHp)));
    properties.push_back(property("attack", "int", std::to_string(input.attack)));
    properties.push_back(property("defense", "int", std::to_string(input.defense)));
    for (const auto& p : input.properties) properties.push_back(p);
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// Read one combatant's state.
  KitCombatant state(std::string_view combatantId) {
    Json container = gameModel_.container(appId_, combatantId);
    Json props = kitContainerProperties(gameModel_, appId_, combatantId);
    KitCombatant c;
    c.containerId = container["containerId"].asString();
    c.displayName = container["displayName"].asString();
    c.ownerUserId = container["ownerUserId"].asString();
    c.combatKey = props["combat_key"].asString();
    c.hp = props["hp"].asInt64();
    c.maxHp = props["max_hp"].asInt64();
    c.attack = props["attack"].asInt64();
    c.defense = props["defense"].asInt64();
    c.alive = props["alive"].asBool();
    return c;
  }

  /// Attack a target with your combatant. The damage formula and the death
  /// flip run server-side. Returns the target's remaining hp.
  KitInvokeResult attack(std::string_view attackerId, std::string_view targetId) {
    JVal params;
    params["target_id"] = targetId;
    return kitInvoke(gameModel_, appId_, names_.attackFn, attackerId, params);
  }

  /// Arm a status effect against a target's combat_key: creates the caller's
  /// StatusEffect container (when effectContainerId is empty) and invokes
  /// the gated apply function; the interval automation then ticks it
  /// server-side.
  KitInvokeResult applyEffect(std::string_view targetKey, std::string_view effectId,
                              std::int64_t magnitude, std::int64_t ticks,
                              std::string_view effectContainerId = {},
                              std::string_view sessionId = {}) {
    std::string selfId(effectContainerId);
    if (selfId.empty()) {
      JVal vars;
      vars["appId"] = appId_;
      vars["typeName"] = names_.effectType;
      vars["displayName"] = "Effect " + std::string(effectId);
      if (!sessionId.empty()) vars["sessionId"] = sessionId;
      Json created = gameModel_.createContainer(vars);
      selfId = created["containerId"].asString();
    }
    JVal params;
    params["target_key"] = targetKey;
    params["effect_id"] = effectId;
    params["magnitude"] = magnitude;
    params["ticks"] = ticks;
    return kitInvoke(gameModel_, appId_, names_.applyEffectFn, selfId, params);
  }

  /// List the active status effects, optionally only those targeting one
  /// combat_key.
  std::vector<KitStatusEffect> effects(std::string_view targetKey = {}) {
    Json containers = gameModel_.containers(appId_, names_.effectType);
    std::vector<KitStatusEffect> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitStatusEffect e;
      e.containerId = c["containerId"].asString();
      e.displayName = c["displayName"].asString();
      e.ownerUserId = c["ownerUserId"].asString();
      e.effectId = props["effect_id"].asString();
      e.targetKey = props["target_key"].asString();
      e.magnitude = props["magnitude"].asInt64();
      e.ticksLeft = props["ticks_left"].asInt64();
      if (e.ticksLeft > 0 && (targetKey.empty() || e.targetKey == targetKey))
        out.push_back(std::move(e));
    });
    return out;
  }

  /// Respawn your downed combatant at full hp.
  KitInvokeResult respawn(std::string_view combatantId) {
    return kitInvoke(gameModel_, appId_, names_.respawnFn, combatantId);
  }

  /// Revive a downed combatant (blueprint deployed with reviveGroup).
  KitInvokeResult revive(std::string_view combatantId) {
    return kitInvoke(gameModel_, appId_, names_.reviveFn, combatantId);
  }

  /// Persist host-simulated hp (blueprint deployed with hostSynced; is_host
  /// enforced server-side). Returns the clamped hp.
  KitInvokeResult syncCombatant(std::string_view combatantId, std::int64_t hp) {
    JVal params;
    params["hp"] = hp;
    return kitInvoke(gameModel_, appId_, names_.syncFn, combatantId, params);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  /// 8 random lowercase hex chars (combat_key uniqueness suffix).
  static std::string randomHexSuffix() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(8);
    for (int i = 0; i < 8; ++i) out.push_back(hex[rd() % 16]);
    return out;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  CombatNames names_;
  EngineDetector* engines_ = nullptr;
  std::string engineModuleName_;
};

}  // namespace crowdy::kit
