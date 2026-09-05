#pragma once

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/wire.hpp"

/// Racing + possession (Wave 3): server-timed gates/laps from the pose
/// stream (type-97 events), course records with ghost replays, and the
/// authoritative sports-lite ball. Mirrors CrowdyJS's kit.racing.
namespace crowdy::kit {

/// Your live run as the engine reports it.
struct KitRaceRun {
  std::string courseId;
  bool started = false;
  std::int64_t lap = 0;
  std::int64_t nextGate = 0;
  std::vector<std::int64_t> splitsMs;
  std::int64_t bestLapMs = 0;
  bool finished = false;
  std::int64_t totalMs = 0;
};

class RacingKit {
 public:
  RacingKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
            std::string moduleName = "racing", std::string possessionModuleName = "possession",
            std::string courseTypeName = "Course")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        possessionModuleName_(std::move(possessionModuleName)),
        courseTypeName_(std::move(courseTypeName)) {}

  /// Is the racing engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Is the possession (ball) engine deployed + enabled?
  bool possessionAvailable() { return engines_.has(possessionModuleName_); }

  /// STUDIO (admin) — create a course (gates = {x, z, radius} triples).
  Json defineCourse(std::string_view courseId,
                    const std::vector<std::array<double, 3>>& gates, std::int64_t laps = 1,
                    std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = courseTypeName_;
    input["displayName"] =
        displayName.empty() ? ("course-" + std::string(courseId)) : std::string(displayName);
    JArray gateRows;
    for (const auto& gate : gates) {
      JArray row;
      row.push_back(JVal(gate[0]));
      row.push_back(JVal(gate[1]));
      row.push_back(JVal(gate[2]));
      gateRows.push_back(JVal(std::move(row)));
    }
    JArray properties;
    properties.push_back(property("course_id", "string", JVal(courseId).dump()));
    properties.push_back(property("gates", "string", JVal(JVal(std::move(gateRows)).dump()).dump()));
    properties.push_back(property("laps", "int", std::to_string(laps)));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Enter a course: your next pose through gate 0 starts the clock.
  Json enter(std::string_view courseId) {
    JVal params;
    params["courseId"] = courseId;
    return invoke(moduleName_, "enter", params);
  }

  /// Your live run (splits, lap, next gate).
  KitRaceRun raceStatus() {
    Json body = invoke(moduleName_, "race_status", JVal());
    KitRaceRun run;
    run.courseId = body["courseId"].asString();
    run.started = body["started"].asBool();
    run.lap = body["lap"].asInt64();
    run.nextGate = body["nextGate"].asInt64();
    body["splitsMs"].forEach([&](Json split) { run.splitsMs.push_back(split.asInt64()); });
    run.bestLapMs = body["bestLapMs"].asInt64();
    run.finished = body["finished"].asBool();
    run.totalMs = body["totalMs"].asInt64();
    return run;
  }

  /// The course record (holder, time, ghost length).
  Json best(std::string_view courseId) {
    JVal params;
    params["courseId"] = courseId;
    return invoke(moduleName_, "best", params);
  }

  /// Replay the course-record ghost onto the actor lane.
  Json ghostPlay(std::string_view courseId) {
    JVal params;
    params["courseId"] = courseId;
    return invoke(moduleName_, "ghost_play", params);
  }

  /// Parse a type-97 race-timing server event.
  std::optional<RaceTimingEvent> parseRaceTiming(const std::uint8_t* bytes, std::size_t len) {
    return parseRaceTimingEvent(bytes, len);
  }

  // -- possession (sports-lite ball) ----------------------------------------

  /// Join the ball match (teams fill by join order).
  Json joinMatch() { return invoke(possessionModuleName_, "join_match", JVal()); }

  /// Claim the free ball (or steal after the protection window).
  Json claim() { return invoke(possessionModuleName_, "claim", JVal()); }

  /// Pass along a direction (holder only).
  Json pass(double dirX, double dirZ) {
    JVal params;
    params["dirX"] = dirX;
    params["dirZ"] = dirZ;
    return invoke(possessionModuleName_, "pass", params);
  }

  /// Shoot at the opposing goal (holder only; the server aims).
  Json shoot() { return invoke(possessionModuleName_, "shoot", JVal()); }

  /// The live ball match (teams, ball, standings, summary).
  Json matchState() { return invoke(possessionModuleName_, "match_state", JVal()); }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  Json invoke(std::string_view moduleName, std::string_view exportName, const JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName, exportName, params);
    if (!result.success) {
      throw std::runtime_error("racing." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
  std::string possessionModuleName_;
  std::string courseTypeName_;
};

}  // namespace crowdy::kit
