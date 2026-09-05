#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>

#include "crowdy/graphql/json.hpp"

/// Optimistic-action helper (CrowdyJS `runOptimisticAction` parity): the
/// packaged snapshot -> optimistic apply -> referee invoke -> confirm or
/// rollback loop that server-refereed action clients otherwise hand-roll.
///
/// The referee side (compute-module invoke export or Model function) is
/// expected to be idempotent on `actionId`: one is generated per attempt so
/// a retry of the same action returns the prior verdict instead of
/// double-applying (the Blocks-with-Friends receipt pattern).
namespace crowdy::kit {

/// Outcome of run_optimistic_action.
struct OptimisticActionOutcome {
  /// True when the referee accepted the action (optimistic state stands).
  bool ok = false;
  /// The referee's result when `ok` (or the denial payload when not).
  graphql::Json result;
  /// Failure reason when not `ok` (rollback already ran).
  std::string error_message;
  /// The actionId used for the attempt (logging / retry correlation).
  std::string action_id;
};

struct OptimisticActionSpec {
  /// Apply the optimistic local change (before the referee round-trip).
  std::function<void()> apply;
  /// Undo it (runs on denial or error).
  std::function<void()> rollback;
  /// The referee round-trip; include the actionId in the request.
  std::function<graphql::Json(const std::string& action_id)> invoke;
  /// Accept/deny decision. Default: a result with `success == false` denies.
  std::function<bool(const graphql::Json&)> validate;
  /// Post-acceptance hook (refresh inventories, play effects, ...).
  std::function<void(const graphql::Json&)> confirm;
  /// Override the generated actionId (deliberate retry of a prior attempt).
  std::string action_id;
};

inline std::string makeActionId() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "a-%llx-%016llx",
                static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(rng()));
  return buf;
}

/// Run one optimistic, server-refereed action. Never throws — failures come
/// back as `{ok = false}` with the rollback already executed.
inline OptimisticActionOutcome run_optimistic_action(
    const OptimisticActionSpec& spec) {
  OptimisticActionOutcome out;
  out.action_id = spec.action_id.empty() ? makeActionId() : spec.action_id;

  try {
    if (spec.apply) spec.apply();
  } catch (const std::exception& e) {
    out.error_message = e.what();
    return out;
  }

  try {
    graphql::Json result =
        spec.invoke ? spec.invoke(out.action_id) : graphql::Json{};
    const bool accepted =
        spec.validate ? spec.validate(result)
                      : !(result["success"].isBool() &&
                          !result["success"].asBool(true));
    if (!accepted) {
      if (spec.rollback) spec.rollback();
      out.result = result;
      const auto reason = result["reason"];
      const auto message = result["errorMessage"];
      out.error_message = message.isString()  ? message.asString()
                          : reason.isString() ? reason.asString()
                                              : "action rejected";
      return out;
    }
    if (spec.confirm) spec.confirm(result);
    out.ok = true;
    out.result = result;
    return out;
  } catch (const std::exception& e) {
    if (spec.rollback) spec.rollback();
    out.error_message = e.what();
    return out;
  }
}

}  // namespace crowdy::kit
