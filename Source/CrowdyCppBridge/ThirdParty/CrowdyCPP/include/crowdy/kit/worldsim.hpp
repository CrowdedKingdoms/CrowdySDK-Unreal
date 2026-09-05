#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// World simulation — day/night + weather (a WorldState singleton),
/// regenerating ResourceNodes, growing Crops / production jobs, and
/// WaveSpawner counters — all driven by interval automations on the automations
/// simulation tier (seconds, never per-frame). Mirrors CrowdyJS's
/// worldsimBlueprint / kit.worldsim.
///
/// PRESENCE-GATED (2026-09-01): these automations run only while the app has a
/// player in it. Scheduled work that comes due while the app is empty is skipped
/// and rescheduled from the moment a player returns; missed runs are never made
/// up. Write each tick to be IDEMPOTENT IN ELAPSED TIME -- advance by
/// `now - lastTick` rather than one fixed step -- or the world silently stalls
/// while nobody is playing.
namespace crowdy::kit {

/// Day/night + weather: an interval automation advances time_of_day
/// (wrapping at hoursPerDay, bumping day), optionally re-rolls the weather,
/// and emits a spatial notification at the world anchor chunk so nearby
/// clients update the sky without polling.
struct WorldsimTimeOptions {
  int intervalMs = 60000;
  int hoursPerDay = 24;
  bool weather = true;
  /// Replication radius of the time-changed ping (chunks, 0-8).
  int notifyDistance = 8;
};

/// Resource-node regeneration: an interval automation raises amount toward
/// max_amount on depleted nodes; players gather_node into a matching stack.
struct WorldsimNodesOptions {
  int intervalMs = 60000;
};

/// Crop growth: an interval automation advances stage toward max_stage;
/// owners harvest grown crops into a matching stack.
struct WorldsimCropsOptions {
  int intervalMs = 60000;
};

/// Wave spawner counters: an interval automation bumps wave and grows
/// next_wave_size — the actual entity spawning stays host-side on the
/// replication plane.
struct WorldsimWavesOptions {
  int intervalMs = 60000;
  int growth = 1;  ///< added to next_wave_size each wave
};

/// Options for worldsimBlueprint. The time/nodes/crops sections default to
/// enabled with a 60s tick (set the optional to std::nullopt to omit one);
/// waves is off by default.
struct WorldsimBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  std::optional<WorldsimTimeOptions> time = WorldsimTimeOptions{};
  std::optional<WorldsimNodesOptions> nodes = WorldsimNodesOptions{};
  std::optional<WorldsimCropsOptions> crops = WorldsimCropsOptions{};
  std::optional<WorldsimWavesOptions> waves;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by worldsimBlueprint for a given prefix.
struct WorldsimNames {
  std::string worldStateType;
  std::string nodeType;
  std::string cropType;
  std::string spawnerType;
  std::string advanceTimeFn;
  std::string setWeatherFn;
  std::string regenNodeFn;
  std::string gatherNodeFn;
  std::string growCropFn;
  std::string harvestFn;
  std::string spawnWaveFn;
  std::string clockAutomation;
  std::string regenAutomation;
  std::string growthAutomation;
  std::string waveAutomation;
};

/// Compute the type/function names a worldsim blueprint (and its runtime
/// helper) uses.
inline WorldsimNames worldsimNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (char& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  WorldsimNames n;
  n.worldStateType = std::string(typePrefix) + "WorldState";
  n.nodeType = std::string(typePrefix) + "ResourceNode";
  n.cropType = std::string(typePrefix) + "Crop";
  n.spawnerType = std::string(typePrefix) + "WaveSpawner";
  n.advanceTimeFn = fnPrefix + "advance_time";
  n.setWeatherFn = fnPrefix + "set_weather";
  n.regenNodeFn = fnPrefix + "regen_node";
  n.gatherNodeFn = fnPrefix + "gather_node";
  n.growCropFn = fnPrefix + "grow_crop";
  n.harvestFn = fnPrefix + "harvest";
  n.spawnWaveFn = fnPrefix + "spawn_wave";
  n.clockAutomation = autoPrefix + "world-clock";
  n.regenAutomation = autoPrefix + "node-regen";
  n.growthAutomation = autoPrefix + "crop-growth";
  n.waveAutomation = autoPrefix + "wave-spawner";
  return n;
}

/// Blueprint for world simulation: day/night + weather (WorldState
/// singleton), regenerating ResourceNodes, growing Crops / production jobs,
/// and WaveSpawner counters — all driven by interval automations on the
/// automations simulation tier (seconds, never per-frame).
///
/// PRESENCE-GATED (2026-09-01): these automations run only while the app has a
/// player in it. Scheduled work that comes due while the app is empty is skipped
/// and rescheduled from the moment a player returns; missed runs are never made
/// up. Write each tick to be IDEMPOTENT IN ELAPSED TIME -- advance by
/// `now - lastTick` rather than one fixed step -- or the world silently stalls
/// while nobody is playing.
///
/// The world clock emits a spatial notification at the world anchor chunk
/// each tick, so nearby clients update the sky push-style instead of
/// polling; everything else is pull-on-demand (or wire your own
/// notify-to-pull pings). Wave spawners only advance counters — actual
/// entity spawning belongs on the replication plane under host authority.
///
/// Runtime counterpart: WorldsimKit.
inline KitBlueprint worldsimBlueprint(const WorldsimBlueprintOptions& options = {}) {
  const OwnerIdKind kind = options.ownerIdKind;
  const WorldsimNames names = worldsimNames(options.typePrefix);
  const std::string automationOnly = kitPolicyJson(isAutomationPolicy());

  auto containerType = [](const std::string& typeName, const char* instantiableBy,
                          const char* description) {
    JVal t;
    t["typeName"] = typeName;
    t["displayName"] = typeName;
    t["instantiableBy"] = instantiableBy;
    t["description"] = description;
    return t;
  };
  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        std::string defaultJson, std::string description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = std::move(defaultJson);
    p["description"] = std::move(description);
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
  auto mutation = [](std::string_view target, const char* property, std::string expression) {
    JVal m;
    m["target"] = target;
    m["property"] = property;
    m["expression"] = std::move(expression);
    return m;
  };
  auto notifyArg = [](const char* name, std::string expression) {
    JVal a;
    a["name"] = name;
    a["expression"] = std::move(expression);
    return a;
  };
  auto intervalAutomation = [](const std::string& name, const std::string& functionName,
                               const std::string& targetTypeName, int intervalMs, int maxTargets,
                               const char* description) {
    JVal a;
    a["name"] = name;
    a["functionName"] = functionName;
    a["targetMode"] = "type";
    a["targetTypeName"] = targetTypeName;
    a["triggerType"] = "schedule";
    a["scheduleKind"] = "interval";
    a["intervalMs"] = intervalMs;
    a["maxTargets"] = maxTargets;
    a["description"] = description;
    return a;
  };
  auto lessThanSelector = [](const char* key, const char* value) {
    JVal predicate;
    predicate["key"] = key;
    predicate["op"] = "<";
    predicate["value"] = value;
    JVal selector;
    selector["selfWhere"] = JVal::array({std::move(predicate)});
    return selector.dump();
  };

  KitBlueprint bp;
  bp.name = names.worldStateType;

  // --- Day/night + weather ---------------------------------------------------
  if (options.time) {
    const WorldsimTimeOptions& time = *options.time;
    const int lastHour = time.hoursPerDay - 1;

    bp.containerTypes.push_back(containerType(
        names.worldStateType, "admin",
        "Singleton world clock/weather state (create one via kit.worldsim.ensureWorld)."));
    bp.propertyDefinitions.push_back(propertyDef(
        names.worldStateType, "time_of_day", "int", "0",
        "In-game hour, 0.." + std::to_string(lastHour) +
            ", advanced by the world-clock automation."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.worldStateType, "day", "int", "0",
                    "In-game day counter (bumps when the clock wraps)."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.worldStateType, "weather", "string", "\"clear\"",
                    "Current weather: 'clear' | 'rain' | 'storm'."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.worldStateType, "cx", "int", "0",
                    "Anchor chunk of the time-changed spatial notification."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.worldStateType, "cy", "int", "0", "Anchor chunk (y)."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.worldStateType, "cz", "int", "0", "Anchor chunk (z)."));

    {
      JVal fn;
      fn["name"] = names.advanceTimeFn;
      fn["containerTypeName"] = names.worldStateType;
      fn["returnType"] = "int";
      JArray mutations;
      mutations.push_back(mutation("self", "day",
                                   "if(self.time_of_day >= " + std::to_string(lastHour) +
                                       ", self.day + 1, self.day)"));
      mutations.push_back(mutation(
          "self", "time_of_day",
          "(self.time_of_day + 1) % " + std::to_string(time.hoursPerDay)));
      if (time.weather) {
        mutations.push_back(
            mutation("self", "weather",
                     "if(rand() < 0.7, \"clear\", if(rand() < 0.5, \"rain\", \"storm\"))"));
      }
      fn["mutations"] = JVal(std::move(mutations));
      fn["returnExpression"] = "self.time_of_day";
      fn["invokePolicyJson"] = automationOnly;
      fn["autonomousInvocable"] = true;
      JVal notification;
      notification["kind"] = "spatial";
      notification["args"] = JVal::array(
          {notifyArg("chunk_x", "self.cx"), notifyArg("chunk_y", "self.cy"),
           notifyArg("chunk_z", "self.cz"), notifyArg("event_type", "1"),
           notifyArg("state", "to_string(self.time_of_day)"),
           notifyArg("distance", std::to_string(time.notifyDistance))});
      fn["notifications"] = JVal::array({std::move(notification)});
      fn["description"] =
          "World-clock tick (automation-only): advances the hour/day, re-rolls weather, and "
          "pushes a spatial time-changed ping so nearby clients skip polling.";
      bp.functions.push_back(std::move(fn));
    }
    if (time.weather) {
      JVal fn;
      fn["name"] = names.setWeatherFn;
      fn["containerTypeName"] = names.worldStateType;
      fn["returnType"] = "string";
      fn["parameters"] =
          JVal::array({param("weather", "string", "The weather to force (e.g. 'storm').")});
      fn["mutations"] = JVal::array({mutation("self", "weather", "$weather")});
      fn["returnExpression"] = "self.weather";
      // not(allow) denies everyone — app admins bypass invoke policies,
      // making this an admin-only lever.
      fn["invokePolicyJson"] = kitPolicyJson(notPolicy(allowPolicy()));
      fn["description"] = "Force the weather (app admins only — everyone else is denied).";
      bp.functions.push_back(std::move(fn));
    }
    bp.automations.push_back(intervalAutomation(
        names.clockAutomation, names.advanceTimeFn, names.worldStateType, time.intervalMs, 1,
        "Advances the world clock (and weather) every tick."));
  }

  // --- Resource nodes ----------------------------------------------------------
  if (options.nodes) {
    const WorldsimNodesOptions& nodes = *options.nodes;
    bp.containerTypes.push_back(containerType(
        names.nodeType, "admin",
        "A shared harvestable resource node that regenerates server-side."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "node_id", "string", "\"\"", "Stable node identifier."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "resource_item_id", "string", "\"\"",
                    "The item gathering yields (matched against the stack)."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "amount", "int", "0", "Units currently available."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "max_amount", "int", "100", "Regeneration ceiling."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "regen_rate", "int", "1", "Units regenerated per tick."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "x", "float", "0", "World position (informational)."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "y", "float", "0", "World position."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.nodeType, "z", "float", "0", "World position."));

    {
      JVal fn;
      fn["name"] = names.regenNodeFn;
      fn["containerTypeName"] = names.nodeType;
      fn["returnType"] = "int";
      fn["mutations"] = JVal::array(
          {mutation("self", "amount", "min(self.max_amount, self.amount + self.regen_rate)")});
      fn["returnExpression"] = "self.amount";
      fn["invokePolicyJson"] = automationOnly;
      fn["autonomousInvocable"] = true;
      fn["description"] = "Server-driven node regeneration tick (automation-only).";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal fn;
      fn["name"] = names.gatherNodeFn;
      fn["containerTypeName"] = names.nodeType;
      fn["returnType"] = "int";
      fn["parameters"] = JVal::array(
          {param("amount", "int", "Units to gather (must not exceed the node amount)."),
           param("to_stack_id", "container_ref",
                 "A caller-owned stack of the node resource that receives the units.")});
      fn["mutations"] = JVal::array(
          {mutation("self", "amount", "self.amount - $amount"),
           mutation("ref($to_stack_id)", "quantity", "ref($to_stack_id).quantity + $amount")});
      fn["returnExpression"] = "self.amount";
      fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(
          "$amount > 0 && self.amount >= $amount && "
          "ref($to_stack_id).item_id == self.resource_item_id && " +
          ownerEqualsCaller("ref($to_stack_id).owner_user_id", kind)));
      fn["description"] =
          "Gather from a shared node into your stack: the node decrement and the grant commit "
          "atomically (no over-gathering races).";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal a = intervalAutomation(names.regenAutomation, names.regenNodeFn, names.nodeType,
                                  nodes.intervalMs, 64, "Regenerates depleted resource nodes.");
      a["selectorJson"] = lessThanSelector("amount", "self.max_amount");
      bp.automations.push_back(std::move(a));
    }
  }

  // --- Crops / production jobs -------------------------------------------------
  if (options.crops) {
    const WorldsimCropsOptions& crops = *options.crops;
    bp.containerTypes.push_back(containerType(
        names.cropType, "member", "A planted crop / production job that matures server-side."));
    bp.propertyDefinitions.push_back(ownerMirrorProperty(names.cropType, kind));
    bp.propertyDefinitions.push_back(
        propertyDef(names.cropType, "stage", "int", "0",
                    "Growth stage, advanced by the crop-growth automation."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.cropType, "max_stage", "int", "3",
                    "Stage at which the crop is harvestable."));
    bp.propertyDefinitions.push_back(propertyDef(names.cropType, "output_item_id", "string",
                                                 "\"\"", "The item harvesting yields."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.cropType, "output_qty", "int", "1", "Units yielded per harvest."));

    {
      JVal fn;
      fn["name"] = names.growCropFn;
      fn["containerTypeName"] = names.cropType;
      fn["returnType"] = "int";
      fn["mutations"] =
          JVal::array({mutation("self", "stage", "min(self.stage + 1, self.max_stage)")});
      fn["returnExpression"] = "self.stage";
      fn["invokePolicyJson"] = automationOnly;
      fn["autonomousInvocable"] = true;
      fn["description"] = "Server-driven growth tick (automation-only).";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal fn;
      fn["name"] = names.harvestFn;
      fn["containerTypeName"] = names.cropType;
      fn["returnType"] = "int";
      fn["parameters"] = JVal::array(
          {param("to_stack_id", "container_ref",
                 "A caller-owned stack of the output item that receives the yield.")});
      fn["mutations"] = JVal::array(
          {mutation("self", "stage", "0"),
           mutation("ref($to_stack_id)", "quantity",
                    "ref($to_stack_id).quantity + self.output_qty")});
      fn["returnExpression"] = "self.output_qty";
      fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
          {ownerOfSelfPolicy(),
           conditionPolicy("self.stage >= self.max_stage && "
                           "ref($to_stack_id).item_id == self.output_item_id && " +
                           ownerEqualsCaller("ref($to_stack_id).owner_user_id", kind))}));
      fn["description"] =
          "Harvest a grown crop: the stage reset and the yield grant commit atomically; regrows "
          "via the automation.";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal a =
          intervalAutomation(names.growthAutomation, names.growCropFn, names.cropType,
                             crops.intervalMs, 128, "Advances growing crops one stage per tick.");
      a["selectorJson"] = lessThanSelector("stage", "self.max_stage");
      bp.automations.push_back(std::move(a));
    }
  }

  // --- Wave spawners -------------------------------------------------------------
  if (options.waves) {
    const WorldsimWavesOptions& waves = *options.waves;
    bp.containerTypes.push_back(containerType(
        names.spawnerType, "admin",
        "A wave counter the server advances; entity spawning stays host-side on the replication "
        "plane."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.spawnerType, "wave", "int", "0", "Current wave number."));
    bp.propertyDefinitions.push_back(
        propertyDef(names.spawnerType, "next_wave_size", "int", "5",
                    "Entities the host should spawn for the next wave."));

    {
      JVal fn;
      fn["name"] = names.spawnWaveFn;
      fn["containerTypeName"] = names.spawnerType;
      fn["returnType"] = "int";
      fn["mutations"] = JVal::array(
          {mutation("self", "wave", "self.wave + 1"),
           mutation("self", "next_wave_size",
                    "self.next_wave_size + " + std::to_string(waves.growth))});
      fn["returnExpression"] = "self.wave";
      fn["invokePolicyJson"] = automationOnly;
      fn["autonomousInvocable"] = true;
      fn["description"] =
          "Server-driven wave advance (automation-only): bumps the counters the host reads to "
          "spawn entities.";
      bp.functions.push_back(std::move(fn));
    }
    bp.automations.push_back(intervalAutomation(
        names.waveAutomation, names.spawnWaveFn, names.spawnerType, waves.intervalMs, 16,
        "Advances wave counters on schedule."));
  }

  if (bp.containerTypes.empty()) {
    throw std::invalid_argument(
        "worldsimBlueprint has every feature disabled — nothing to build");
  }

  return bp;
}

/// A parsed view of the world clock/weather state.
struct KitWorldState {
  std::string containerId;
  std::int64_t timeOfDay = 0;
  std::int64_t day = 0;
  std::string weather;
};

/// A parsed view of one resource node.
struct KitResourceNode {
  std::string containerId;
  std::string displayName;
  std::string nodeId;
  std::string resourceItemId;
  std::int64_t amount = 0;
  std::int64_t maxAmount = 0;
  std::int64_t regenRate = 0;
  double x = 0;
  double y = 0;
  double z = 0;
};

/// A parsed view of one crop / production job.
struct KitCrop {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when the crop has no owner
  std::int64_t stage = 0;
  std::int64_t maxStage = 0;
  std::string outputItemId;
  std::int64_t outputQty = 0;
  bool ready = false;
};

/// A parsed view of one wave spawner.
struct KitWaveSpawner {
  std::string containerId;
  std::string displayName;
  std::int64_t wave = 0;
  std::int64_t nextWaveSize = 0;
};

/// The chunk where the world clock's spatial time-changed ping is emitted.
struct KitWorldAnchorChunk {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;
};

/// Input for WorldsimKit::createNode.
struct KitResourceNodeCreateInput {
  std::string displayName;
  std::string nodeId;
  std::string resourceItemId;
  std::optional<std::int64_t> amount;  ///< defaults to maxAmount
  std::int64_t maxAmount = 100;
  std::int64_t regenRate = 1;
  struct Position {
    double x = 0;
    double y = 0;
    double z = 0;
  };
  std::optional<Position> position;
  /// Extra SeedPropertyInput objects appended after the generated ones.
  JArray properties;
};

/// Input for WorldsimKit::plant.
struct KitCropPlantInput {
  std::string ownerUserId;
  std::string outputItemId;
  std::int64_t outputQty = 1;
  std::int64_t maxStage = 3;
  std::string displayName;  ///< empty defaults to "Crop <outputItemId>"
  std::string sessionId;
};

/// Runtime helpers for the worldsimBlueprint conventions: the world
/// clock/weather singleton, regenerating resource nodes players gather from,
/// crops that mature server-side, and wave counters the host reads. The
/// simulation itself runs in automations — these helpers create/read state
/// and drive the player-facing functions.
///
/// Obtained via GameKitClient::worldsim().
/// A world engine's forecast (current front + day phase).
struct KitForecast {
  std::string weather;
  std::optional<double> dayPhase;  ///< in [0, 1) when the engine reports it
  std::optional<bool> isNight;
  std::optional<std::int64_t> remainingMs;  ///< until the current front rolls
  Json body;
};

class WorldsimKit {
 public:
  WorldsimKit(std::string appId, domains::GameModelAPI& gameModel,
              std::string_view typePrefix = {}, EngineDetector* engines = nullptr,
              std::string_view moduleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        names_(worldsimNames(typePrefix)),
        engines_(engines),
        moduleName_(moduleName.empty() ? std::string("world-engine")
                                       : std::string(moduleName)) {}

  const WorldsimNames& names() const { return names_; }

  /// Is a world compute engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(moduleName_); }

  /// The world engine's forecast invoke: the current weather front plus
  /// day-phase fields. Late joiners call this once, then track transitions
  /// from the type-90 event stream (parseWeatherEvent). Throws when no
  /// engine is available.
  KitForecast forecast() {
    EngineInvokeResult result = engines_ != nullptr
                                    ? engines_->invoke(moduleName_, "forecast")
                                    : EngineInvokeResult{false, "compute domain unavailable", {}};
    if (!result.success) {
      throw std::runtime_error("forecast unavailable: " +
                               (result.reason.empty() ? "engine missing" : result.reason));
    }
    KitForecast out;
    out.weather = result.body["weather"].asString();
    if (result.body["dayPhase"].isNumber()) out.dayPhase = result.body["dayPhase"].asDouble();
    if (result.body["isNight"].isBool()) out.isNight = result.body["isNight"].asBool();
    if (result.body["remainingMs"].isNumber()) {
      out.remainingMs = result.body["remainingMs"].asInt64();
    }
    out.body = result.body;
    return out;
  }

  /// Parse a server-event payload as a world-engine weather transition
  /// (type 90), or nullopt when it is another event type.
  static std::optional<WeatherEvent> parseWeather(const std::uint8_t* bytes, std::size_t len) {
    return parseWeatherEvent(bytes, len);
  }

  /// Find-or-create the WorldState singleton (admin). anchorChunk is where
  /// the clock's spatial time-changed ping is emitted.
  Json ensureWorld(const std::optional<KitWorldAnchorChunk>& anchorChunk = std::nullopt,
                   std::string_view displayName = {}) {
    Json existing = gameModel_.containers(appId_, names_.worldStateType);
    if (existing.size() > 0) return existing.at(0);

    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.worldStateType;
    input["displayName"] = displayName.empty() ? std::string("World") : std::string(displayName);
    JArray properties;
    if (anchorChunk) {
      properties.push_back(property("cx", "int", std::to_string(anchorChunk->x)));
      properties.push_back(property("cy", "int", std::to_string(anchorChunk->y)));
      properties.push_back(property("cz", "int", std::to_string(anchorChunk->z)));
    }
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Read the world clock/weather state. Throws std::runtime_error when no
  /// WorldState exists yet (run ensureWorld() as admin first).
  KitWorldState worldState() {
    Json containers = gameModel_.containers(appId_, names_.worldStateType);
    if (containers.size() == 0) {
      throw std::runtime_error("No " + names_.worldStateType +
                               " exists yet — run kit.worldsim.ensureWorld() as admin");
    }
    Json first = containers.at(0);
    Json props = kitContainerProperties(gameModel_, appId_, first["containerId"].asStringView());
    KitWorldState state;
    state.containerId = first["containerId"].asString();
    state.timeOfDay = props["time_of_day"].asInt64();
    state.day = props["day"].asInt64();
    state.weather = props["weather"].asString("clear");
    return state;
  }

  /// Force the weather (app admins only — the policy denies everyone else).
  KitInvokeResult setWeather(std::string_view weather) {
    const KitWorldState state = worldState();
    JVal params;
    params["weather"] = weather;
    return kitInvoke(gameModel_, appId_, names_.setWeatherFn, state.containerId, params);
  }

  /// Create a resource node (admin).
  Json createNode(const KitResourceNodeCreateInput& input) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.nodeType;
    vars["displayName"] = input.displayName;
    JArray properties;
    properties.push_back(property("node_id", "string", JVal(input.nodeId).dump()));
    properties.push_back(
        property("resource_item_id", "string", JVal(input.resourceItemId).dump()));
    properties.push_back(
        property("amount", "int", std::to_string(input.amount.value_or(input.maxAmount))));
    properties.push_back(property("max_amount", "int", std::to_string(input.maxAmount)));
    properties.push_back(property("regen_rate", "int", std::to_string(input.regenRate)));
    if (input.position) {
      properties.push_back(property("x", "float", JVal(input.position->x).dump()));
      properties.push_back(property("y", "float", JVal(input.position->y).dump()));
      properties.push_back(property("z", "float", JVal(input.position->z).dump()));
    }
    for (const auto& p : input.properties) properties.push_back(p);
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List resource nodes with parsed state.
  std::vector<KitResourceNode> nodes() {
    Json containers = gameModel_.containers(appId_, names_.nodeType);
    std::vector<KitResourceNode> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitResourceNode node;
      node.containerId = c["containerId"].asString();
      node.displayName = c["displayName"].asString();
      node.nodeId = props["node_id"].asString();
      node.resourceItemId = props["resource_item_id"].asString();
      node.amount = props["amount"].asInt64();
      node.maxAmount = props["max_amount"].asInt64();
      node.regenRate = props["regen_rate"].asInt64();
      node.x = props["x"].asDouble();
      node.y = props["y"].asDouble();
      node.z = props["z"].asDouble();
      out.push_back(std::move(node));
    });
    return out;
  }

  /// Gather from a node into a caller-owned stack of the node's resource;
  /// the node decrement and the grant commit atomically. Resolves with the
  /// node's remaining amount.
  KitInvokeResult gather(std::string_view nodeId, std::int64_t amount,
                         std::string_view toStackId) {
    JVal params;
    params["amount"] = amount;
    params["to_stack_id"] = toStackId;
    return kitInvoke(gameModel_, appId_, names_.gatherNodeFn, nodeId, params);
  }

  /// Plant a crop / start a production job (member).
  Json plant(const KitCropPlantInput& input) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.cropType;
    vars["displayName"] =
        input.displayName.empty() ? "Crop " + input.outputItemId : input.displayName;
    if (!input.sessionId.empty()) vars["sessionId"] = input.sessionId;
    JArray properties;
    properties.push_back(property("owner_user_id", "int", input.ownerUserId));
    properties.push_back(property("output_item_id", "string", JVal(input.outputItemId).dump()));
    properties.push_back(property("output_qty", "int", std::to_string(input.outputQty)));
    properties.push_back(property("max_stage", "int", std::to_string(input.maxStage)));
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List a player's crops (all crops when ownerUserId is empty).
  std::vector<KitCrop> crops(std::string_view ownerUserId = {}) {
    Json containers = gameModel_.containers(appId_, names_.cropType);
    std::vector<KitCrop> out;
    containers.forEach([&](Json c) {
      Json owner = c["ownerUserId"];
      if (!ownerUserId.empty()) {
        if (!owner.ok() || owner.isNull()) return;
        if (owner.asString() != ownerUserId) return;
      }
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitCrop crop;
      crop.containerId = c["containerId"].asString();
      crop.displayName = c["displayName"].asString();
      if (owner.ok() && !owner.isNull()) crop.ownerUserId = owner.asString();
      crop.stage = props["stage"].asInt64();
      crop.maxStage = props["max_stage"].asInt64();
      crop.outputItemId = props["output_item_id"].asString();
      crop.outputQty = props["output_qty"].asInt64();
      crop.ready = crop.maxStage > 0 && crop.stage >= crop.maxStage;
      out.push_back(std::move(crop));
    });
    return out;
  }

  /// Harvest a grown crop into a caller-owned stack of the output item;
  /// resets the stage for regrowth. Resolves with the yield quantity.
  KitInvokeResult harvest(std::string_view cropId, std::string_view toStackId) {
    JVal params;
    params["to_stack_id"] = toStackId;
    return kitInvoke(gameModel_, appId_, names_.harvestFn, cropId, params);
  }

  /// Create a wave spawner (admin; blueprint deployed with waves enabled).
  Json createSpawner(std::string_view displayName, std::int64_t nextWaveSize = 5) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.spawnerType;
    vars["displayName"] = displayName;
    JArray properties;
    properties.push_back(property("next_wave_size", "int", std::to_string(nextWaveSize)));
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List wave spawners (the host reads these to spawn entities).
  std::vector<KitWaveSpawner> spawners() {
    Json containers = gameModel_.containers(appId_, names_.spawnerType);
    std::vector<KitWaveSpawner> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitWaveSpawner spawner;
      spawner.containerId = c["containerId"].asString();
      spawner.displayName = c["displayName"].asString();
      spawner.wave = props["wave"].asInt64();
      spawner.nextWaveSize = props["next_wave_size"].asInt64();
      out.push_back(std::move(spawner));
    });
    return out;
  }

  /// Run one of the worldsim automations immediately (admin; for testing).
  Json runNow(std::string_view automationName) {
    return gameModel_.runAutomation(appId_, automationName);
  }

  /// Pause or resume one of the worldsim automations (admin).
  Json setEnabled(std::string_view automationName, bool enabled) {
    return gameModel_.setAutomationEnabled(appId_, automationName, enabled);
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
  WorldsimNames names_;
  EngineDetector* engines_ = nullptr;
  std::string moduleName_;
};

}  // namespace crowdy::kit
