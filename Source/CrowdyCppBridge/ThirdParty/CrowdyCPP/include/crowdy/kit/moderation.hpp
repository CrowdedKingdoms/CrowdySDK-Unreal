#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/kit/core.hpp"

/// Moderation helpers (Wave 3, model + client — no compute): file reports,
/// read the admin escalation queue, resolve reports, personal mute lists
/// (client-enforced chat filtering). Enforcement stays on platform surfaces
/// (tier revocation, grid permissions). Mirrors CrowdyJS's kit.moderation.
namespace crowdy::kit {

/// A parsed report row.
struct KitModReport {
  std::string containerId;
  std::string reporterUserId;
  std::string subjectUserId;
  std::string reason;
  std::string detail;
  std::string status;
  std::string resolution;
  std::int64_t filedAtMs = 0;
};

class ModerationKit {
 public:
  ModerationKit(std::string appId, domains::GameModelAPI& gameModel,
                std::string_view typePrefix = {})
      : appId_(std::move(appId)), gameModel_(gameModel) {
    const std::string prefix(typePrefix);
    reportType_ = prefix + "ModReport";
    muteType_ = prefix + "ModMute";
    const std::string fnPrefix = prefix.empty() ? "" : toSnakeCase(prefix) + "_";
    resolveReportFn_ = fnPrefix + "resolve_report";
  }

  /// File a report (creates the caller's report row in the queue).
  Json report(std::string_view reporterUserId, std::string_view subjectUserId,
              std::string_view reason, std::string_view detail = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = reportType_;
    input["displayName"] = "report " + std::string(reason) + " vs " + std::string(subjectUserId);
    JArray properties;
    properties.push_back(property("reporter_user_id", "string", JVal(reporterUserId).dump()));
    properties.push_back(property("subject_user_id", "string", JVal(subjectUserId).dump()));
    properties.push_back(property("reason", "string", JVal(reason).dump()));
    properties.push_back(
        property("detail", "string", JVal(std::string(detail).substr(0, 500)).dump()));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// The escalation queue (admins; filter by status, default "open").
  std::vector<KitModReport> queue(std::string_view status = "open") {
    std::vector<KitModReport> out;
    Json containers = gameModel_.containers(appId_, reportType_);
    containers.forEach([&](Json c) {
      const std::string id = c["containerId"].asString();
      Json props = kitContainerProperties(gameModel_, appId_, id);
      KitModReport row;
      row.containerId = id;
      row.reporterUserId = props["reporter_user_id"].asString();
      row.subjectUserId = props["subject_user_id"].asString();
      row.reason = props["reason"].asString();
      row.detail = props["detail"].asString();
      row.status = props["status"].asString();
      row.resolution = props["resolution"].asString();
      row.filedAtMs = props["filed_at_ms"].asInt64();
      if (status.empty() || row.status == status) out.push_back(std::move(row));
    });
    std::sort(out.begin(), out.end(),
              [](const KitModReport& a, const KitModReport& b) { return a.filedAtMs < b.filedAtMs; });
    return out;
  }

  /// ADMIN — resolve a report with a disposition.
  KitInvokeResult resolve(std::string_view reportContainerId, std::string_view status,
                          std::string_view resolution) {
    JVal params;
    params["status"] = status;
    params["resolution"] = resolution;
    return kitInvoke(gameModel_, appId_, resolveReportFn_, reportContainerId, params);
  }

  /// Mute a player (adds to YOUR client-enforced mute list).
  Json mute(std::string_view ownerUserId, std::string_view mutedUserId) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = muteType_;
    input["displayName"] = "mute " + std::string(mutedUserId);
    JArray properties;
    properties.push_back(property("owner_user_id", "string", JVal(ownerUserId).dump()));
    properties.push_back(property("muted_user_id", "string", JVal(mutedUserId).dump()));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Unmute: delete the matching mute row. Returns false when absent.
  bool unmute(std::string_view ownerUserId, std::string_view mutedUserId) {
    for (const auto& [containerId, muted] : mutes(ownerUserId)) {
      if (muted == mutedUserId) {
        gameModel_.deleteContainer(appId_, containerId);
        return true;
      }
    }
    return false;
  }

  /// The caller's mute list: (containerId, mutedUserId) pairs.
  std::vector<std::pair<std::string, std::string>> mutes(std::string_view ownerUserId) {
    std::vector<std::pair<std::string, std::string>> out;
    Json containers = gameModel_.containers(appId_, muteType_);
    containers.forEach([&](Json c) {
      const std::string id = c["containerId"].asString();
      Json props = kitContainerProperties(gameModel_, appId_, id);
      if (props["owner_user_id"].asString() == ownerUserId) {
        out.emplace_back(id, props["muted_user_id"].asString());
      }
    });
    return out;
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
  std::string reportType_;
  std::string muteType_;
  std::string resolveReportFn_;
};

}  // namespace crowdy::kit
