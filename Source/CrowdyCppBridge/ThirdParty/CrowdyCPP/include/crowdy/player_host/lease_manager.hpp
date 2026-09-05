#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "crowdy/core/clock.hpp"
#include "crowdy/player_host/adapter.hpp"
#include "crowdy/player_host/schemas.hpp"

namespace crowdy::player_host {

struct AgentControlDispatchV1 {
  std::string tool_call_id;
  std::string client_epoch;
  std::optional<std::string> lease_id;
  std::optional<std::string> approval_grant;
  GameCommandV1 command;
  bool operator==(const AgentControlDispatchV1&) const = default;
};

struct AgentObservationDispatchV1 {
  std::string client_epoch;
  std::optional<std::string> lease_id;
  bool operator==(const AgentObservationDispatchV1&) const = default;
};

struct AgentControlLeaseSnapshotV1 {
  bool connected = false;
  std::optional<std::string> client_epoch;
  std::optional<AgentControlLeaseV1> lease;
  std::optional<PlayerHostCapabilitiesV1> capabilities;
  std::optional<PreemptionReasonV1> last_preemption;
  bool operator==(const AgentControlLeaseSnapshotV1&) const = default;
};

struct AgentControlLeaseManagerOptionsV1 {
  const core::IClock* clock = nullptr;
  std::function<std::string()> context_version;
  std::uint32_t max_lease_seconds = 600;
  std::uint32_t heartbeat_timeout_ms = 5'000;
  std::size_t max_remembered_calls = 2'048;
  std::size_t max_remembered_observations = 32;

  /**
   * Optional host policy for CONDITIONAL commands. The safe default requires
   * approval for combat against a player target. Policy predicates are
   * serialized with command admission and must be side-effect-free,
   * non-blocking, and non-reentrant.
   */
  std::function<bool(const GameCommandV1&, const GameObservationV1&)>
      conditional_approval_required;

  /**
   * Validates an opaque server grant against the exact dispatch. When absent,
   * a non-empty grant is accepted because exact hash binding remains server
   * authority. This predicate follows the same non-reentrant rule.
   */
  std::function<bool(const AgentControlDispatchV1&)> validate_approval_grant;
  std::function<void(const AgentControlLeaseSnapshotV1&)> on_change;
};

/**
 * Game-thread-friendly Play authority gate. Call tick() from the native game
 * loop; no browser API and no hidden timer thread are required.
 */
class AgentControlLeaseManager {
 public:
  explicit AgentControlLeaseManager(
      PlayerHostAdapterV1& adapter,
      AgentControlLeaseManagerOptionsV1 options = {});
  ~AgentControlLeaseManager();

  AgentControlLeaseManager(const AgentControlLeaseManager&) = delete;
  AgentControlLeaseManager& operator=(const AgentControlLeaseManager&) = delete;
  AgentControlLeaseManager(AgentControlLeaseManager&&) noexcept;
  AgentControlLeaseManager& operator=(AgentControlLeaseManager&&) noexcept;

  AgentControlLeaseSnapshotV1 snapshot() const;

  std::optional<AgentErrorV1> attach(std::string client_epoch);
  void disconnect();

  void refreshCapabilities(CapabilitiesCallbackV1 callback);
  std::optional<AgentErrorV1> grantLease(const AgentControlLeaseV1& lease);
  std::optional<AgentErrorV1> heartbeat(
      const AgentControlHeartbeatV1& heartbeat);

  void observe(const ObserveRequestV1& request,
               std::optional<AgentObservationDispatchV1> dispatch,
               ObservationCallbackV1 callback);
  void dispatch(AgentControlDispatchV1 input, CommandCallbackV1 callback);

  /** Checks absolute lease expiry and monotonic heartbeat freshness. */
  void tick();

  /**
   * Synchronous local takeover. clearAgentIntent() is called before lease
   * state, rates, observations, or callbacks are released.
   */
  void preempt(PreemptionReasonV1 reason);
  /// Idiomatic CrowdyJS parity spelling for explicit lease revocation.
  /// Semantics are exactly the same synchronous intent-first takeover path.
  void revoke(
      PreemptionReasonV1 reason = PreemptionReasonV1::HUMAN_STOP) {
    preempt(reason);
  }
  void onHumanInput() { preempt(PreemptionReasonV1::HUMAN_INPUT); }
  void onEscape() { preempt(PreemptionReasonV1::ESCAPE); }
  void onDeath() { preempt(PreemptionReasonV1::DEATH); }
  void onPermissionChanged() {
    preempt(PreemptionReasonV1::PERMISSION_CHANGED);
  }
  void onAdmissionChanged() {
    preempt(PreemptionReasonV1::ADMISSION_CHANGED);
  }
  void onContextChanged() {
    preempt(PreemptionReasonV1::CONTEXT_CHANGED);
  }
  void onControlledEntityChanged() {
    preempt(PreemptionReasonV1::CONTROL_TARGET_CHANGED);
  }

  /** Execute-once records live for one attached session. */
  void clearClosedSession();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::player_host
