#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crowdy/graphql/errors.hpp"

/// Reading a datacenter redirect out of a GraphQL error array.
///
/// An app lives in ONE datacenter, because Citus distributes on app_id and that
/// is where its shards are. A request answered anywhere else crosses a WAN
/// silently, so the server refuses instead of serving: WRONG_DATACENTER says
/// "go here", APP_UNAVAILABLE says "nowhere to go right now".
///
/// Mirrors CrowdyJS src/datacenter-redirect.ts.
namespace crowdy::graphql {

/// Where a WRONG_DATACENTER error says to go.
struct DatacenterMove {
  std::string gameApiUrl;    ///< never empty in a valid move
  std::string gameApiWsUrl;  ///< may be empty; derive from gameApiUrl
  std::string appId;
  std::string appDatacenter;
};

/// A move from ONE error, or nullopt.
///
/// Requires a non-empty gameApiUrl as well as the code. A WRONG_DATACENTER
/// without a target is not a move — it is a refusal the caller must surface,
/// and treating it as a move would silently "succeed" by staying put.
inline std::optional<DatacenterMove> moveFromError(
    const GraphQLErrorDetail& error) {
  if (error.code != kWrongDatacenterCode) return std::nullopt;
  if (error.gameApiUrl.empty()) return std::nullopt;
  DatacenterMove move;
  move.gameApiUrl = error.gameApiUrl;
  move.gameApiWsUrl = error.gameApiWsUrl;
  move.appId = error.appId;
  move.appDatacenter = error.appDatacenter;
  return move;
}

/// The first move in an error array, or nullopt.
///
/// Scans EVERY entry rather than just the first. A GraphQL response can carry
/// several errors, and the routing one is not guaranteed to lead — a partially
/// resolved query reports whatever failed first, which may be a field error
/// while the redirect sits behind it.
inline std::optional<DatacenterMove> moveFromErrors(
    const std::vector<GraphQLErrorDetail>& errors) {
  for (const auto& error : errors) {
    if (auto move = moveFromError(error)) return move;
  }
  return std::nullopt;
}

/// True when any error says the app cannot be served at all. Checked BEFORE a
/// move: APP_UNAVAILABLE deliberately carries no endpoint, so it must not be
/// mistaken for a redirect that happens to be missing its target.
inline bool isAppUnavailable(const std::vector<GraphQLErrorDetail>& errors) {
  for (const auto& error : errors) {
    if (error.code == kAppUnavailableCode) return true;
  }
  return false;
}

}  // namespace crowdy::graphql
