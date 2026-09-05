#pragma once

#include <stdexcept>
#include <string>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Director helpers (Wave 2): data-driven encounter direction — wave
/// schedules, spawn budgets, boss phases, party scaling — over the director
/// engine. Mirrors CrowdyJS's kit.director.
namespace crowdy::kit {

class DirectorKit {
 public:
  DirectorKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
              std::string moduleName = "director", std::string defTypeName = "EncounterDef")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        defTypeName_(std::move(defTypeName)) {}

  /// Is the director deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// STUDIO (admin) — create an encounter definition container.
  /// `wavesJson` is the waves array as JSON text (see the engine docs).
  Json defineEncounter(std::string_view encounterId, std::string_view wavesJson,
                       std::optional<std::int64_t> spawnBudget = std::nullopt,
                       std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = defTypeName_;
    input["displayName"] = displayName.empty()
                               ? ("Encounter " + std::string(encounterId))
                               : std::string(displayName);
    JArray properties;
    properties.push_back(property("encounter_id", "string", JVal(encounterId).dump()));
    properties.push_back(property("waves", "string", JVal(wavesJson).dump()));
    if (spawnBudget) {
      properties.push_back(property("spawn_budget", "int", std::to_string(*spawnBudget)));
    }
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Start a run (party size scales unit counts server-side).
  Json startRun(std::string_view encounterId, std::int64_t players = 1) {
    JVal params;
    params["encounterId"] = encounterId;
    params["players"] = players;
    return invoke("start_run", params);
  }

  /// Report kills toward the live wave (trusted callers/engines).
  Json reportKill(std::string_view runId, std::int64_t count = 1) {
    JVal params;
    params["runId"] = runId;
    params["count"] = count;
    return invoke("report_kill", params);
  }

  /// Report boss hp; phase transitions announce on the compute bus.
  Json reportBossHp(std::string_view runId, std::int64_t hp) {
    JVal params;
    params["runId"] = runId;
    params["hp"] = hp;
    return invoke("report_boss_hp", params);
  }

  /// Force-clear the live wave (run creator only).
  Json skipWave(std::string_view runId) {
    JVal params;
    params["runId"] = runId;
    return invoke("skip_wave", params);
  }

  /// One run's state (or engine totals with an empty id).
  Json runState(std::string_view runId = {}) {
    JVal params;
    if (!runId.empty()) params["runId"] = runId;
    return invoke("run_state", params);
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
      throw std::runtime_error("director." + std::string(exportName) + " failed: " + result.reason);
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
