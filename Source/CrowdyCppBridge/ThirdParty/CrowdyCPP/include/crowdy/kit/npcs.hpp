#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// NPC archetypes — an admin-instantiable container type holding the NPC's
/// durable state, one autonomousInvocable model function per behavior (gated
/// is_automation so players cannot puppet them), and the automations + event
/// triggers that drive those behaviors on the server. Mirrors CrowdyJS's
/// npcBlueprint / kit.npcs.
namespace crowdy::kit {

/// A trigger for an NPC behavior: a schedule (interval or cron) or a model
/// event. Build with the interval/cron/event factories; the event kind's
/// optional filter fields (functionName, containerTypeName, propertyKey,
/// debounceMs) are set on the returned value.
struct NpcBehaviorTrigger {
  enum class Kind { Interval, Cron, Event };
  Kind kind = Kind::Interval;
  std::int64_t intervalMs = 0;
  std::string cronExpr;
  std::string onEvent;  ///< "function_invoked" | "property_changed" | "container_created"
  std::string functionName;
  std::string containerTypeName;
  std::string propertyKey;
  /// property_changed only: which writes wake the behavior — "direct",
  /// "function", or "any" (empty = unset, leaving the API default). NPC state
  /// is written by behavior functions rather than by clients, so reacting to
  /// one needs "function" or "any".
  std::string writeSource;
  std::optional<std::int64_t> debounceMs;

  static NpcBehaviorTrigger interval(std::int64_t ms) {
    NpcBehaviorTrigger t;
    t.kind = Kind::Interval;
    t.intervalMs = ms;
    return t;
  }
  static NpcBehaviorTrigger cron(std::string expr) {
    NpcBehaviorTrigger t;
    t.kind = Kind::Cron;
    t.cronExpr = std::move(expr);
    return t;
  }
  static NpcBehaviorTrigger event(std::string onEvent) {
    NpcBehaviorTrigger t;
    t.kind = Kind::Event;
    t.onEvent = std::move(onEvent);
    return t;
  }
};

/// One server-driven NPC behavior: a model function plus the automation that
/// drives it.
struct NpcBehaviorSpec {
  /// Automation name (unique per app), e.g. "npc-wander". Also derives the
  /// default function name (npc_wander).
  std::string name;
  /// Entry-point function name. Empty defaults to the snake-cased behavior
  /// name.
  std::string functionName;
  /// The property writes the behavior performs each tick
  /// (FunctionMutationInput objects).
  JArray mutations;
  /// Typed parameters (bind them from a selector or static params) —
  /// FunctionParamInput objects.
  JArray parameters;
  /// What makes the behavior run.
  NpcBehaviorTrigger trigger;
  /// Selector choosing/filtering targets and binding params (see the Game API
  /// "Autonomous Processes → Selectors" guide), including grid-permission
  /// predicates. JSON-encoded at deploy. Null = none.
  JVal selector;
  /// Convenience: only NPCs whose role property equals this act (adds a
  /// selfWhere filter when no explicit selector is given).
  std::optional<std::string> role;
  /// Fan-out cap per run.
  int maxTargets = 8;
  /// Static params merged into every call. Null = none.
  JVal params;
  /// Identity the automation acts as; empty for a trusted server caller.
  std::string runAsUserId;
};

/// Options for npcBlueprint.
struct NpcBlueprintOptions {
  /// NPC container type name. Empty defaults to "Npc".
  std::string typeName;
  /// Extra property definitions beyond the defaults (role, x, y, z,
  /// behavior_state, health). containerTypeName is filled in.
  JArray extraProperties;
  /// The server-driven behaviors this NPC archetype has.
  std::vector<NpcBehaviorSpec> behaviors;
};

/// Compute the function/automation names an NPC behavior deploys under.
inline std::string npcBehaviorFunctionName(const NpcBehaviorSpec& behavior) {
  return behavior.functionName.empty() ? toSnakeCase(behavior.name) : behavior.functionName;
}

/// Blueprint for an NPC archetype: an admin-instantiable container type
/// holding the NPC's durable state, one autonomousInvocable model function
/// per behavior (gated is_automation so players cannot puppet them), and the
/// automations + event triggers that drive those behaviors on the server.
///
/// Runtime counterpart: NpcsKit.
inline KitBlueprint npcBlueprint(const NpcBlueprintOptions& options) {
  const std::string typeName = options.typeName.empty() ? std::string("Npc") : options.typeName;
  if (options.behaviors.empty()) {
    throw std::invalid_argument("npcBlueprint requires at least one behavior");
  }

  auto propertyDef = [&](const char* key, const char* valueType, const char* defaultJson) {
    JVal p;
    p["containerTypeName"] = typeName;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = defaultJson;
    return p;
  };

  KitBlueprint bp;
  bp.name = typeName;

  {
    JVal t;
    t["typeName"] = typeName;
    t["displayName"] = typeName;
    t["instantiableBy"] = "admin";
    t["description"] = "A server-driven non-player character.";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(propertyDef("role", "string", "\"\""));
  bp.propertyDefinitions.push_back(propertyDef("x", "float", "0"));
  bp.propertyDefinitions.push_back(propertyDef("y", "float", "0"));
  bp.propertyDefinitions.push_back(propertyDef("z", "float", "0"));
  bp.propertyDefinitions.push_back(propertyDef("behavior_state", "string", "\"idle\""));
  bp.propertyDefinitions.push_back(propertyDef("health", "int", "100"));
  for (const auto& extra : options.extraProperties) {
    JVal p = extra;
    p["containerTypeName"] = typeName;
    bp.propertyDefinitions.push_back(std::move(p));
  }

  for (const auto& behavior : options.behaviors) {
    const std::string functionName = npcBehaviorFunctionName(behavior);
    {
      JVal fn;
      fn["name"] = functionName;
      fn["containerTypeName"] = typeName;
      if (!behavior.parameters.empty()) fn["parameters"] = JVal(behavior.parameters);
      fn["mutations"] = JVal(behavior.mutations);
      fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
      fn["autonomousInvocable"] = true;
      fn["description"] =
          "Server-driven NPC behavior for the '" + behavior.name + "' automation.";
      bp.functions.push_back(std::move(fn));
    }

    JVal selector = behavior.selector;
    if (selector.isNull() && behavior.role) {
      JVal predicate;
      predicate["key"] = "role";
      predicate["op"] = "==";
      predicate["value"] = *behavior.role;
      selector["selfWhere"] = JVal::array({std::move(predicate)});
    }

    JVal automation;
    automation["name"] = behavior.name;
    automation["functionName"] = functionName;
    automation["targetMode"] = "type";
    automation["targetTypeName"] = typeName;
    automation["maxTargets"] = std::int64_t{behavior.maxTargets};
    if (!selector.isNull()) automation["selectorJson"] = selector.dump();
    if (!behavior.params.isNull()) automation["paramsJson"] = behavior.params.dump();
    if (!behavior.runAsUserId.empty()) automation["runAsUserId"] = behavior.runAsUserId;

    switch (behavior.trigger.kind) {
      case NpcBehaviorTrigger::Kind::Interval:
        automation["triggerType"] = "schedule";
        automation["scheduleKind"] = "interval";
        automation["intervalMs"] = behavior.trigger.intervalMs;
        break;
      case NpcBehaviorTrigger::Kind::Cron:
        automation["triggerType"] = "schedule";
        automation["scheduleKind"] = "cron";
        automation["cronExpr"] = behavior.trigger.cronExpr;
        break;
      case NpcBehaviorTrigger::Kind::Event: {
        automation["triggerType"] = "event";
        JVal trigger;
        trigger["automationName"] = behavior.name;
        trigger["onEvent"] = behavior.trigger.onEvent;
        if (!behavior.trigger.functionName.empty())
          trigger["functionName"] = behavior.trigger.functionName;
        if (!behavior.trigger.containerTypeName.empty())
          trigger["containerTypeName"] = behavior.trigger.containerTypeName;
        if (!behavior.trigger.propertyKey.empty())
          trigger["propertyKey"] = behavior.trigger.propertyKey;
        if (!behavior.trigger.writeSource.empty())
          trigger["writeSource"] = behavior.trigger.writeSource;
        if (behavior.trigger.debounceMs) trigger["debounceMs"] = *behavior.trigger.debounceMs;
        bp.automationTriggers.push_back(std::move(trigger));
        break;
      }
    }

    bp.automations.push_back(std::move(automation));
  }

  return bp;
}

/// A parsed view of one live NPC.
struct KitNpc {
  std::string containerId;
  std::string displayName;
  std::string role;
  double x = 0;
  double y = 0;
  double z = 0;
  std::string behaviorState;
  std::int64_t health = 0;
  /// All visible properties, including any extras your blueprint added.
  Json properties;
};

/// A world position for NpcsKit::spawn.
struct KitNpcPosition {
  double x = 0;
  double y = 0;
  double z = 0;
};

/// Input for NpcsKit::spawn.
struct KitNpcSpawnInput {
  std::string displayName;
  std::optional<std::string> role;
  std::optional<KitNpcPosition> position;
  /// Extra SeedPropertyInput objects appended after the generated ones.
  JArray properties;
  std::string sessionId;
};

/// Runtime helpers for the npcBlueprint conventions: spawn NPC instances,
/// read their server-driven state, and manage/monitor the automations behind
/// them. Behaviors run in the API server — clients only re-read state (or
/// listen for model-driven notifications) and render.
///
/// Spawning and the automation management/monitoring calls are studio/admin
/// operations (manage_apps); reads are player-safe.
///
/// Obtained via GameKitClient::npcs().
class NpcsKit {
 public:
  NpcsKit(std::string appId, domains::GameModelAPI& gameModel, std::string_view typeName = {},
          EngineDetector* engines = nullptr, std::string_view moduleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        typeName_(typeName.empty() ? std::string("Npc") : std::string(typeName)),
        engines_(engines),
        moduleName_(moduleName.empty() ? std::string("npc-engine") : std::string(moduleName)) {}

  const std::string& typeName() const { return typeName_; }

  /// Is an NPC compute engine deployed + enabled (cached per client)? When
  /// true, NPCs stream smooth kFlagNpc actor poses — overlay them with
  /// overlayLivePoses. When false (model-only deployment), the polled
  /// container positions are all there is, exactly as before.
  bool engineAvailable() { return engines_ != nullptr && engines_->has(moduleName_); }

  /// One live engine pose keyed by the NPC's actor uuid, for
  /// overlayLivePoses (fill from your replication layer's actor stream).
  struct LivePose {
    std::string uuid;
    double x = 0, y = 0, z = 0;
  };

  /// Overlay live engine-driven poses onto a polled NPC snapshot (the
  /// generalized BWF NpcService.withLivePoses pattern): each NPC whose
  /// actor_uuid has a fresh pose gets its position replaced; the rest keep
  /// their durable container position.
  static std::vector<KitNpc> overlayLivePoses(std::vector<KitNpc> npcs,
                                              const std::vector<LivePose>& lane) {
    if (lane.empty()) return npcs;
    for (auto& npc : npcs) {
      const std::string uuid = npc.properties["actor_uuid"].asString();
      if (uuid.empty()) continue;
      for (const auto& live : lane) {
        if (live.uuid == uuid) {
          npc.x = live.x;
          npc.y = live.y;
          npc.z = live.z;
          break;
        }
      }
    }
    return npcs;
  }

  /// Spawn a live NPC instance (admin — the type is admin-instantiable).
  Json spawn(const KitNpcSpawnInput& input) {
    JArray properties;
    if (input.role) properties.push_back(property("role", "string", JVal(*input.role).dump()));
    if (input.position) {
      properties.push_back(property("x", "float", JVal(input.position->x).dump()));
      properties.push_back(property("y", "float", JVal(input.position->y).dump()));
      properties.push_back(property("z", "float", JVal(input.position->z).dump()));
    }
    for (const auto& p : input.properties) properties.push_back(p);

    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = typeName_;
    vars["displayName"] = input.displayName;
    if (!input.sessionId.empty()) vars["sessionId"] = input.sessionId;
    if (!properties.empty()) vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List live NPCs with parsed state, optionally filtered by role. Fetches
  /// each NPC's visible properties — fine for the bounded NPC populations
  /// automations are designed around.
  std::vector<KitNpc> list(const std::optional<std::string>& role = std::nullopt,
                           std::string_view sessionId = {}) {
    Json containers = gameModel_.containers(appId_, typeName_, sessionId);
    std::vector<KitNpc> out;
    containers.forEach([&](Json c) {
      KitNpc npc = toNpc(c["containerId"].asString(), c["displayName"].asString());
      if (role && npc.role != *role) return;
      out.push_back(std::move(npc));
    });
    return out;
  }

  /// Read one NPC's current server-side state.
  KitNpc state(std::string_view npcId) {
    Json container = gameModel_.container(appId_, npcId);
    return toNpc(container["containerId"].asString(), container["displayName"].asString());
  }

  /// Run one of the NPC automations immediately (admin; useful for testing).
  Json runNow(std::string_view automationName) {
    return gameModel_.runAutomation(appId_, automationName);
  }

  /// Pause or resume an NPC automation (admin). Re-enabling also resets a
  /// tripped failure circuit.
  Json setEnabled(std::string_view automationName, bool enabled) {
    return gameModel_.setAutomationEnabled(appId_, automationName, enabled);
  }

  /// Aggregate "what are my NPCs doing" stats over a recent window (admin).
  Json stats(int windowMinutes = 0) { return gameModel_.automationStats(appId_, windowMinutes); }

  /// Recent automation run history, newest first (admin).
  Json runs(std::string_view automationName = {}, std::optional<bool> success = std::nullopt,
            int limit = 0) {
    JVal vars;
    vars["appId"] = appId_;
    if (!automationName.empty()) vars["automationName"] = automationName;
    if (success) vars["success"] = *success;
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    return gameModel_.automationRuns(vars);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitNpc toNpc(std::string containerId, std::string displayName) {
    Json props = kitContainerProperties(gameModel_, appId_, containerId);
    KitNpc n;
    n.containerId = std::move(containerId);
    n.displayName = std::move(displayName);
    n.role = props["role"].asString();
    n.x = props["x"].asDouble();
    n.y = props["y"].asDouble();
    n.z = props["z"].asDouble();
    n.behaviorState = props["behavior_state"].asString();
    n.health = props["health"].asInt64();
    n.properties = props;
    return n;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  std::string typeName_;
  EngineDetector* engines_ = nullptr;
  std::string moduleName_;
};

}  // namespace crowdy::kit
