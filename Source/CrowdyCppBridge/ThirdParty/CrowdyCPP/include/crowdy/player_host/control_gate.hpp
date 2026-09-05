#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/agent/types.hpp"
#include "crowdy/player_host/lease_manager.hpp"

namespace crowdy::agent {
class CrowdyStudioAgentController;
}

namespace crowdy::player_host {

enum class NativePlayerControlKeyboardInputV1 {
  Input,
  Escape,
};

struct NativePlayerControlGateSnapshotV1 {
  bool bound = false;
  std::optional<agent::AgentLease> active_lease;
  std::optional<PreemptionReasonV1> last_preemption;
  bool human_input_active = false;
  bool offline_stop = false;
};

struct NativePlayerControlGateOptionsV1 {
  const core::IClock* clock = nullptr;
  std::uint32_t human_input_active_ms = 150;
  std::function<void(PreemptionReasonV1)> on_preempt;
};

/**
 * Optional narrow controller seam for engines that wrap or own their agent
 * controller. Operations only begin best-effort remote persistence; the gate
 * deliberately ignores their eventual outcome after local intent is clear.
 */
class INativePlayerControlGateController {
 public:
  virtual ~INativePlayerControlGateController() = default;

  virtual std::vector<agent::AgentLease> playerControlLeases() const = 0;
  virtual void revokePlayerControlLease(std::string lease_id,
                                        PreemptionReasonV1 reason) = 0;
  virtual void pausePlayerControl() = 0;
  virtual void stopPlayerControl() = 0;
};

/**
 * Native equivalent of CrowdyJS 12.1 PlayerControlGate.
 *
 * Every imperative engine hook is synchronous and noexcept. Local agent intent
 * is cleared before a best-effort controller revoke, Pause, or Stop begins.
 * Human keyboard, pointer, and movement hooks only observe takeover intent:
 * they never consume, mutate, or synthesize an engine input event.
 *
 * The gate is game-thread-owned and has no timer or hidden thread.
 * humanInputActive() is evaluated from the injected monotonic clock, so tick()
 * is not required. Bound objects must outlive their returned unbind function
 * and the gate, or be explicitly unbound first.
 */
class NativePlayerControlGate {
 public:
  using ClearAgentIntent =
      std::function<void(PreemptionReasonV1)>;
  using Listener =
      std::function<void(const NativePlayerControlGateSnapshotV1&)>;
  using Unbind = std::function<void()>;

  explicit NativePlayerControlGate(
      ClearAgentIntent clear_agent_intent,
      NativePlayerControlGateOptionsV1 options = {});
  ~NativePlayerControlGate();

  NativePlayerControlGate(const NativePlayerControlGate&) = delete;
  NativePlayerControlGate& operator=(const NativePlayerControlGate&) = delete;
  NativePlayerControlGate(NativePlayerControlGate&&) noexcept;
  NativePlayerControlGate& operator=(NativePlayerControlGate&&) noexcept;

  /**
   * Bind the exact native lease manager without a remote Agent controller.
   * Human takeover still clears local host authority synchronously; Pause and
   * Stop remain local-only until a controller is bound.
   */
  Unbind bind(AgentControlLeaseManager& lease_manager);

  /** Bind the exact native lease manager and production controller. */
  Unbind bind(AgentControlLeaseManager& lease_manager,
              agent::CrowdyStudioAgentController& controller);

  /** Bind the exact native lease manager and an engine-owned narrow bridge. */
  Unbind bind(AgentControlLeaseManager& lease_manager,
              INativePlayerControlGateController& controller);

  NativePlayerControlGateSnapshotV1 snapshot() const;
  bool humanInputActive() const noexcept;
  Unbind subscribe(Listener listener);
  /** Re-read bound lease/controller state and notify snapshot listeners. */
  void refresh() noexcept;

  void preempt(PreemptionReasonV1 reason,
               bool notify_server = true) noexcept;

  void onHumanKeyboardInput(
      NativePlayerControlKeyboardInputV1 input =
          NativePlayerControlKeyboardInputV1::Input) noexcept;
  void onHumanPointerInput() noexcept;
  void onHumanMovementInput() noexcept;

  void pause() noexcept;
  void stop() noexcept;
  void onClientReattached() noexcept;
  void onDeath() noexcept;
  void onDisconnected() noexcept;
  void onBackgrounded() noexcept;
  void onPermissionChanged() noexcept;
  void onAdmissionChanged() noexcept;
  void onContextChanged() noexcept;
  void onControlTargetChanged() noexcept;

  /** Idempotent explicit teardown; destruction calls this automatically. */
  void destroy() noexcept;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::player_host
