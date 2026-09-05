#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/kit/engine.hpp"

/// Matchmaking helpers (Wave 2): rating-bucketed queues with widening
/// windows, party blocks, accept-gated proposals that hand off to the match
/// layer over compute events. After everyone accepts, resolve the created
/// match with MatchesKit::findByProposal. Mirrors CrowdyJS's
/// kit.matchmaking.
namespace crowdy::kit {

class MatchmakingKit {
 public:
  MatchmakingKit(std::string appId, EngineDetector& engines,
                 std::string moduleName = "matchmaking")
      : appId_(std::move(appId)), engines_(engines), moduleName_(std::move(moduleName)) {}

  /// Is the matchmaking engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_.has(moduleName_); }

  /// Join a queue (optionally a party block that must match together, and
  /// an explicit rating for games that keep rating elsewhere).
  graphql::Json queueJoin(std::string_view mode = {}, std::optional<std::int64_t> rating = std::nullopt,
                          const std::vector<std::string>& party = {}) {
    graphql::JVal params;
    if (!mode.empty()) params["mode"] = mode;
    if (rating) params["rating"] = *rating;
    if (!party.empty()) {
      graphql::JArray list;
      for (const auto& member : party) list.push_back(graphql::JVal(member));
      params["party"] = graphql::JVal(std::move(list));
    }
    return invoke("queue_join", params);
  }

  /// Leave your queue (all modes, or one).
  graphql::Json queueLeave(std::string_view mode = {}) {
    graphql::JVal params;
    if (!mode.empty()) params["mode"] = mode;
    return invoke("queue_leave", params);
  }

  /// Your queue/proposal status.
  graphql::Json queueStatus(std::string_view mode = {}) {
    graphql::JVal params;
    if (!mode.empty()) params["mode"] = mode;
    return invoke("queue_status", params);
  }

  /// Accept a proposal; when everyone has, the match handoff fires.
  graphql::Json accept(std::string_view proposalId) {
    graphql::JVal params;
    params["proposalId"] = proposalId;
    return invoke("accept", params);
  }

  /// Report the decided result (Elo-lite rating update + proposal close).
  graphql::Json reportResult(std::string_view proposalId, std::string_view winnerUserId) {
    graphql::JVal params;
    params["proposalId"] = proposalId;
    params["winnerUserId"] = winnerUserId;
    return invoke("report_result", params);
  }

  /// Engine totals (queues/proposals/rated players).
  graphql::Json status() { return invoke("status", graphql::JVal()); }

 private:
  graphql::Json invoke(std::string_view exportName, const graphql::JVal& params) {
    EngineInvokeResult result = engines_.invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("matchmaking." + std::string(exportName) +
                               " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  EngineDetector& engines_;
  std::string moduleName_;
};

}  // namespace crowdy::kit
