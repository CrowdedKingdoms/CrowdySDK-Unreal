#include "crowdy/player_host/control_gate.hpp"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "crowdy/agent/controller.hpp"

namespace crowdy::player_host {
namespace {

agent::AgentPreemptionReason agentPreemption(
    PreemptionReasonV1 reason) noexcept {
  const auto converted =
      agent::agentPreemptionReasonFromString(preemptionReasonName(reason));
  return converted.value_or(agent::AgentPreemptionReason::HumanStop);
}

agent::AgentLease publicLease(const AgentControlLeaseV1& source) {
  agent::AgentLease lease;
  lease.leaseId = source.lease_id;
  lease.kind = source.kind == LeaseKindV1::Play
                   ? agent::AgentLeaseKind::Play
                   : agent::AgentLeaseKind::Workspace;
  switch (source.status) {
    case LeaseStatusV1::Active:
      lease.status = agent::AgentLeaseStatus::Active;
      break;
    case LeaseStatusV1::Revoked:
      lease.status = agent::AgentLeaseStatus::Revoked;
      break;
    case LeaseStatusV1::Expired:
      lease.status = agent::AgentLeaseStatus::Expired;
      break;
  }
  lease.clientEpoch = source.client_epoch;
  lease.scopes.reserve(source.scopes.size());
  for (const auto scope : source.scopes) {
    lease.scopes.emplace_back(toString(scope));
  }
  lease.holder = source.holder;
  lease.controlledEntityId = source.controlled_entity_id;
  lease.hostCapabilityRevision = source.host_capability_revision;
  lease.contextVersion = source.context_version;
  lease.grantedAt = source.granted_at;
  lease.expiresAt = source.expires_at;
  if (source.revoked_reason) {
    lease.revokedReason = agentPreemption(*source.revoked_reason);
  }
  return lease;
}

}  // namespace

struct NativePlayerControlGate::Impl
    : std::enable_shared_from_this<NativePlayerControlGate::Impl> {
  struct ControllerOps {
    const void* identity = nullptr;
    std::function<std::vector<agent::AgentLease>()> leases;
    std::function<void(std::string, PreemptionReasonV1)> revoke;
    std::function<void()> pause;
    std::function<void()> stop;

    explicit operator bool() const noexcept {
      return identity != nullptr;
    }
  };

  std::function<void(PreemptionReasonV1)> clear_agent_intent;
  NativePlayerControlGateOptionsV1 options;
  const core::IClock& clock;
  AgentControlLeaseManager* lease_manager = nullptr;
  ControllerOps controller;
  std::unordered_set<std::string> locally_revoked_lease_ids;
  std::unordered_map<std::uint64_t, Listener> listeners;
  mutable std::optional<agent::AgentLease> last_visible_active_lease;
  std::optional<std::int64_t> last_human_input_at;
  std::optional<PreemptionReasonV1> last_preemption;
  std::uint64_t listener_sequence = 0;
  bool offline_stop = false;
  bool destroying = false;
  bool destroyed = false;

  Impl(std::function<void(PreemptionReasonV1)> clear,
       NativePlayerControlGateOptionsV1 config)
      : clear_agent_intent(std::move(clear)),
        options(std::move(config)),
        clock(options.clock ? *options.clock : core::systemClock()) {
    if (!clear_agent_intent) {
      throw std::invalid_argument(
          "NativePlayerControlGate requires a local intent clear callback");
    }
  }

  bool humanInputActive() const noexcept {
    if (!last_human_input_at || options.human_input_active_ms == 0) {
      return false;
    }
    try {
      const auto now = clock.monotonicMillis();
      if (now < *last_human_input_at) return true;
      return now - *last_human_input_at <
             static_cast<std::int64_t>(options.human_input_active_ms);
    } catch (...) {
      return false;
    }
  }

  std::optional<agent::AgentLease> activeLease() const {
    std::optional<std::string> attached_epoch;
    if (lease_manager) {
      const auto manager_snapshot = lease_manager->snapshot();
      attached_epoch = manager_snapshot.client_epoch;
      const auto& local = manager_snapshot.lease;
      if (local && local->kind == LeaseKindV1::Play &&
          local->status == LeaseStatusV1::Active &&
          !locally_revoked_lease_ids.contains(local->lease_id)) {
        last_visible_active_lease = publicLease(*local);
        return last_visible_active_lease;
      }
    }
    if (!controller || !controller.leases) {
      last_visible_active_lease.reset();
      return std::nullopt;
    }
    for (auto& lease : controller.leases()) {
      if (lease.kind == agent::AgentLeaseKind::Play &&
          lease.status == agent::AgentLeaseStatus::Active &&
          (!attached_epoch || lease.clientEpoch == *attached_epoch) &&
          !locally_revoked_lease_ids.contains(lease.leaseId)) {
        last_visible_active_lease = lease;
        return last_visible_active_lease;
      }
    }
    last_visible_active_lease.reset();
    return std::nullopt;
  }

  NativePlayerControlGateSnapshotV1 snapshot() const {
    NativePlayerControlGateSnapshotV1 value;
    value.bound = lease_manager != nullptr;
    try {
      value.active_lease = activeLease();
    } catch (...) {
      // A narrow controller state failure is represented as no visible lease.
    }
    value.last_preemption = last_preemption;
    value.human_input_active = humanInputActive();
    value.offline_stop = offline_stop;
    return value;
  }

  void emit() noexcept {
    NativePlayerControlGateSnapshotV1 value;
    std::vector<Listener> current;
    try {
      value = snapshot();
      current.reserve(listeners.size());
      for (const auto& [id, listener] : listeners) {
        (void)id;
        current.push_back(listener);
      }
    } catch (...) {
      return;
    }
    for (const auto& listener : current) {
      try {
        listener(value);
      } catch (...) {
        // A UI/engine observer cannot interrupt a human input or safety hook.
      }
    }
  }

  void preempt(PreemptionReasonV1 reason, bool notify_server) noexcept {
    if (destroyed) return;

    const void* controller_identity = controller.identity;
    std::optional<agent::AgentLease> lease;
    try {
      lease = activeLease();
      if (lease) locally_revoked_lease_ids.insert(lease->leaseId);
    } catch (...) {
      // Local takeover still proceeds when controller state is unavailable.
      if (last_visible_active_lease &&
          !locally_revoked_lease_ids.contains(
              last_visible_active_lease->leaseId)) {
        lease = last_visible_active_lease;
        try {
          locally_revoked_lease_ids.insert(lease->leaseId);
        } catch (...) {
          lease.reset();
        }
      }
    }

    try {
      if (lease_manager) {
        lease_manager->preempt(reason);
      } else if (clear_agent_intent) {
        clear_agent_intent(reason);
      }
    } catch (...) {
      // AgentControlLeaseManager clears intent/state before observer callbacks.
      // A callback failure cannot interrupt the human event or offline Stop.
    }

    last_preemption = reason;
    if (options.on_preempt) {
      try {
        options.on_preempt(reason);
      } catch (...) {
        // Preemption observers do not own the safety transition.
      }
    }
    emit();

    if (!notify_server || !lease || destroyed ||
        controller.identity != controller_identity || !controller.revoke) {
      return;
    }
    try {
      controller.revoke(lease->leaseId, reason);
    } catch (...) {
      // Remote revocation is best effort after the local lease is suppressed.
    }
  }

  void takeHumanInput(PreemptionReasonV1 reason) noexcept {
    if (destroyed) return;
    try {
      last_human_input_at = clock.monotonicMillis();
    } catch (...) {
      last_human_input_at.reset();
    }

    try {
      if (activeLease()) {
        preempt(reason, true);
      } else {
        emit();
      }
    } catch (...) {
      // If lease inspection fails, fail safe by clearing local agent intent.
      preempt(reason, true);
    }
  }

  Unbind bind(AgentControlLeaseManager& next_manager,
              ControllerOps next_controller) {
    if (destroyed || destroying) {
      throw std::logic_error("cannot bind a destroyed NativePlayerControlGate");
    }
    const bool replacement =
        lease_manager &&
        (lease_manager != &next_manager ||
         controller.identity != next_controller.identity);
    if (replacement) {
      preempt(PreemptionReasonV1::CLIENT_REATTACHED, false);
    }
    lease_manager = &next_manager;
    controller = std::move(next_controller);
    if (replacement) last_visible_active_lease.reset();
    offline_stop = false;
    emit();

    const auto* manager_identity = &next_manager;
    const void* controller_identity = controller.identity;
    const std::weak_ptr<Impl> weak = shared_from_this();
    return [weak, manager_identity, controller_identity] {
      const auto self = weak.lock();
      if (!self || self->destroyed ||
          self->lease_manager != manager_identity ||
          self->controller.identity != controller_identity) {
        return;
      }
      self->preempt(PreemptionReasonV1::DISCONNECTED, false);
      if (self->lease_manager == manager_identity &&
          self->controller.identity == controller_identity) {
        self->unbind();
      }
    };
  }

  void unbind() noexcept {
    lease_manager = nullptr;
    controller = {};
    locally_revoked_lease_ids.clear();
    last_visible_active_lease.reset();
    emit();
  }

  Unbind subscribe(Listener listener) {
    if (!listener) return [] {};
    const auto id = ++listener_sequence;
    listeners.emplace(id, std::move(listener));
    try {
      listeners.at(id)(snapshot());
    } catch (...) {
      // Match later emissions: observer failures are isolated.
    }
    const std::weak_ptr<Impl> weak = shared_from_this();
    return [weak, id] {
      if (const auto self = weak.lock()) self->listeners.erase(id);
    };
  }

  void pause() noexcept {
    if (destroyed) return;
    const void* controller_identity = controller.identity;
    preempt(PreemptionReasonV1::HUMAN_STOP, false);
    if (destroyed || controller.identity != controller_identity ||
        !controller.pause) {
      return;
    }
    try {
      controller.pause();
    } catch (...) {
      // Local Pause already succeeded.
    }
  }

  void stop() noexcept {
    if (destroyed) return;
    const void* controller_identity = controller.identity;
    offline_stop = true;
    preempt(PreemptionReasonV1::HUMAN_STOP, false);
    if (!destroyed && controller.identity == controller_identity &&
        controller.stop) {
      try {
        controller.stop();
      } catch (...) {
        // Offline Stop remains locally effective.
      }
    }
    emit();
  }

  void destroy() noexcept {
    if (destroyed || destroying) return;
    destroying = true;
    preempt(PreemptionReasonV1::DISCONNECTED, false);
    unbind();
    listeners.clear();
    destroyed = true;
    destroying = false;
  }
};

NativePlayerControlGate::NativePlayerControlGate(
    std::function<void(PreemptionReasonV1)> clear_agent_intent,
    NativePlayerControlGateOptionsV1 options)
    : impl_(std::make_shared<Impl>(std::move(clear_agent_intent),
                                  std::move(options))) {}

NativePlayerControlGate::~NativePlayerControlGate() {
  if (impl_) impl_->destroy();
}

NativePlayerControlGate::NativePlayerControlGate(
    NativePlayerControlGate&&) noexcept = default;

NativePlayerControlGate& NativePlayerControlGate::operator=(
    NativePlayerControlGate&& other) noexcept {
  if (this == &other) return *this;
  if (impl_) impl_->destroy();
  impl_ = std::move(other.impl_);
  return *this;
}

NativePlayerControlGate::Unbind NativePlayerControlGate::bind(
    AgentControlLeaseManager& lease_manager) {
  return impl_->bind(lease_manager, {});
}

NativePlayerControlGate::Unbind NativePlayerControlGate::bind(
    AgentControlLeaseManager& lease_manager,
    agent::CrowdyStudioAgentController& controller) {
  Impl::ControllerOps operations;
  operations.identity = &controller;
  operations.leases = [&controller] { return controller.getState().leases; };
  operations.revoke = [&controller](std::string lease_id,
                                    PreemptionReasonV1 reason) {
    controller.revokeLease(std::move(lease_id), agentPreemption(reason));
  };
  operations.pause = [&controller] { controller.pause(); };
  operations.stop = [&controller] { controller.stop(); };
  return impl_->bind(lease_manager, std::move(operations));
}

NativePlayerControlGate::Unbind NativePlayerControlGate::bind(
    AgentControlLeaseManager& lease_manager,
    INativePlayerControlGateController& controller) {
  Impl::ControllerOps operations;
  operations.identity = &controller;
  operations.leases = [&controller] {
    return controller.playerControlLeases();
  };
  operations.revoke = [&controller](std::string lease_id,
                                    PreemptionReasonV1 reason) {
    controller.revokePlayerControlLease(std::move(lease_id), reason);
  };
  operations.pause = [&controller] { controller.pausePlayerControl(); };
  operations.stop = [&controller] { controller.stopPlayerControl(); };
  return impl_->bind(lease_manager, std::move(operations));
}

NativePlayerControlGateSnapshotV1 NativePlayerControlGate::snapshot() const {
  return impl_->snapshot();
}

bool NativePlayerControlGate::humanInputActive() const noexcept {
  return impl_ && impl_->humanInputActive();
}

NativePlayerControlGate::Unbind NativePlayerControlGate::subscribe(
    Listener listener) {
  return impl_->subscribe(std::move(listener));
}

void NativePlayerControlGate::refresh() noexcept {
  if (impl_) impl_->emit();
}

void NativePlayerControlGate::preempt(PreemptionReasonV1 reason,
                                      bool notify_server) noexcept {
  if (impl_) impl_->preempt(reason, notify_server);
}

void NativePlayerControlGate::onHumanKeyboardInput(
    NativePlayerControlKeyboardInputV1 input) noexcept {
  if (!impl_) return;
  impl_->takeHumanInput(
      input == NativePlayerControlKeyboardInputV1::Escape
          ? PreemptionReasonV1::ESCAPE
          : PreemptionReasonV1::HUMAN_INPUT);
}

void NativePlayerControlGate::onHumanPointerInput() noexcept {
  if (impl_) impl_->takeHumanInput(PreemptionReasonV1::HUMAN_INPUT);
}

void NativePlayerControlGate::onHumanMovementInput() noexcept {
  if (impl_) impl_->takeHumanInput(PreemptionReasonV1::HUMAN_INPUT);
}

void NativePlayerControlGate::pause() noexcept {
  if (impl_) impl_->pause();
}

void NativePlayerControlGate::stop() noexcept {
  if (impl_) impl_->stop();
}

void NativePlayerControlGate::onClientReattached() noexcept {
  if (impl_) {
    impl_->preempt(PreemptionReasonV1::CLIENT_REATTACHED, false);
  }
}

void NativePlayerControlGate::onDeath() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::DEATH, true);
}

void NativePlayerControlGate::onDisconnected() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::DISCONNECTED, false);
}

void NativePlayerControlGate::onBackgrounded() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::DISCONNECTED, false);
}

void NativePlayerControlGate::onPermissionChanged() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::PERMISSION_CHANGED, true);
}

void NativePlayerControlGate::onAdmissionChanged() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::ADMISSION_CHANGED, true);
}

void NativePlayerControlGate::onContextChanged() noexcept {
  if (impl_) impl_->preempt(PreemptionReasonV1::CONTEXT_CHANGED, true);
}

void NativePlayerControlGate::onControlTargetChanged() noexcept {
  if (impl_) {
    impl_->preempt(PreemptionReasonV1::CONTROL_TARGET_CHANGED, true);
  }
}

void NativePlayerControlGate::destroy() noexcept {
  if (impl_) impl_->destroy();
}

}  // namespace crowdy::player_host
