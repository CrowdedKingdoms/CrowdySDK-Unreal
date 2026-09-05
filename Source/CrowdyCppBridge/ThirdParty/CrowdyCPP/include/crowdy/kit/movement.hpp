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

/// Movement-warden reads (Wave 3, observe/flag posture): violation books,
/// envelope config, type-95 violation parsing. The warden never corrects —
/// client prediction stays as-is; games decide what flags mean. Mirrors
/// CrowdyJS's kit.movement.
namespace crowdy::kit {

/// A user's violation book as the warden reports it.
struct KitViolations {
  std::string userId;
  std::int64_t speed = 0;
  std::int64_t teleport = 0;
  std::int64_t bounds = 0;
  /// (atMs, kind, detail) rolling log.
  std::vector<std::tuple<std::int64_t, std::string, std::string>> log;
};

class MovementKit {
 public:
  MovementKit(std::string appId, domains::GameModelAPI& gameModel, EngineDetector& engines,
              std::string moduleName = "movement-warden",
              std::string configTypeName = "WardenConfig")
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        engines_(engines),
        moduleName_(std::move(moduleName)),
        configTypeName_(std::move(configTypeName)) {}

  /// Is the warden deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// A user's violation book (your own with an empty argument).
  KitViolations violations(std::string_view userId = {}) {
    JVal params;
    if (!userId.empty()) params["userId"] = userId;
    Json body = invoke("violations", params);
    KitViolations out;
    out.userId = body["userId"].asString();
    out.speed = body["speed"].asInt64();
    out.teleport = body["teleport"].asInt64();
    out.bounds = body["bounds"].asInt64();
    body["log"].forEach([&](Json entry) {
      out.log.emplace_back(entry["atMs"].asInt64(), entry["kind"].asString(),
                           entry["detail"].asString());
    });
    return out;
  }

  /// The live envelope configuration (posture is always "observe" v1).
  Json config() { return invoke("config", JVal()); }

  /// Warden totals (watched actors, flagged users, samples).
  Json status() { return invoke("status", JVal()); }

  /// STUDIO (admin) — create the WardenConfig container.
  Json defineConfig(std::optional<std::array<std::int64_t, 3>> chunk = std::nullopt,
                    std::optional<std::int64_t> radiusXz = std::nullopt,
                    std::optional<std::int64_t> maxSpeed = std::nullopt,
                    std::optional<std::int64_t> maxTeleport = std::nullopt,
                    std::optional<std::array<std::int64_t, 4>> bounds = std::nullopt,
                    std::optional<std::int64_t> tolerancePct = std::nullopt,
                    std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = configTypeName_;
    input["displayName"] = displayName.empty() ? std::string("warden-config") : std::string(displayName);
    JArray properties;
    auto add = [&](const char* key, std::int64_t value) {
      JVal p;
      p["key"] = key;
      p["valueType"] = "int";
      p["valueJson"] = std::to_string(value);
      properties.push_back(std::move(p));
    };
    if (chunk) {
      add("chunk_x", (*chunk)[0]);
      add("chunk_y", (*chunk)[1]);
      add("chunk_z", (*chunk)[2]);
    }
    if (radiusXz) add("radius_xz", *radiusXz);
    if (maxSpeed) add("max_speed", *maxSpeed);
    if (maxTeleport) add("max_teleport", *maxTeleport);
    if (bounds) {
      add("min_x", (*bounds)[0]);
      add("max_x", (*bounds)[1]);
      add("min_z", (*bounds)[2]);
      add("max_z", (*bounds)[3]);
    }
    if (tolerancePct) add("tolerance_pct", *tolerancePct);
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Parse a type-95 movement-violation server event.
  std::optional<MovementViolationEvent> parseViolation(const std::uint8_t* bytes, std::size_t len) {
    return parseMovementViolation(bytes, len);
  }

 private:
  Json invoke(std::string_view exportName, const JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("movement." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector& engines_;
  std::string moduleName_;
  std::string configTypeName_;
};

}  // namespace crowdy::kit
