#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "crowdy/player_host/types.hpp"

namespace crowdy::player_host {

/**
 * Cooperative cancellation shared with engine adapters. Cancellation fences
 * acceptance in the SDK even when a host cannot interrupt an already-started
 * engine operation.
 */
class CancellationTokenV1 {
 public:
  CancellationTokenV1() = default;
  bool cancelled() const noexcept {
    const auto state = state_.lock();
    return !state || state->cancelled.load(std::memory_order_acquire);
  }

  /**
   * Host adapters call this immediately before an operation can produce an
   * externally visible effect. Dispatchers use the marker to distinguish a
   * safely cancelled queued call from an ambiguous in-flight outcome.
   */
  void markEffectStarted() const noexcept {
    const auto state = state_.lock();
    if (state) state->effect_started.store(true, std::memory_order_release);
  }
  bool effectStarted() const noexcept {
    const auto state = state_.lock();
    return state && state->effect_started.load(std::memory_order_acquire);
  }

 private:
  struct State {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> effect_started{false};
  };

  friend class CancellationSourceV1;
  explicit CancellationTokenV1(const std::shared_ptr<State>& state)
      : state_(state) {}
  std::weak_ptr<State> state_;
};

class CancellationSourceV1 {
 public:
  CancellationSourceV1()
      : state_(std::make_shared<CancellationTokenV1::State>()) {}

  CancellationTokenV1 token() const { return CancellationTokenV1(state_); }
  void cancel() noexcept {
    state_->cancelled.store(true, std::memory_order_release);
  }
  bool cancelled() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
  }
  bool effectStarted() const noexcept {
    return state_->effect_started.load(std::memory_order_acquire);
  }

 private:
  std::shared_ptr<CancellationTokenV1::State> state_;
};

template <typename T>
struct AdapterResultV1 {
  std::optional<T> value;
  std::optional<AgentErrorV1> error;
  bool outcome_unknown = false;

  static AdapterResultV1 success(T result) {
    AdapterResultV1 value;
    value.value = std::move(result);
    return value;
  }
  static AdapterResultV1 failure(AgentErrorV1 failure,
                                 bool ambiguous = false) {
    AdapterResultV1 value;
    value.error = std::move(failure);
    value.outcome_unknown = ambiguous;
    return value;
  }
  bool ok() const noexcept { return value.has_value() && !error.has_value(); }
};

using CapabilitiesCallbackV1 =
    std::function<void(AdapterResultV1<PlayerHostCapabilitiesV1>)>;
using ObservationCallbackV1 =
    std::function<void(AdapterResultV1<GameObservationV1>)>;
using CommandCallbackV1 =
    std::function<void(AdapterResultV1<GameCommandResultV1>)>;

/**
 * Native game integration boundary.
 *
 * Implementations must call the same typed intent services used by human
 * controls. They must not expose a CrowdyClient, GraphQL, UDP, input injection,
 * or a generic host-call escape hatch to an agent.
 *
 * Callbacks may be delivered inline or later. The adapter must invoke each
 * callback at most once and must observe cancellation where its engine allows.
 */
class PlayerHostAdapterV1 {
 public:
  virtual ~PlayerHostAdapterV1() = default;

  virtual std::string_view contractVersion() const noexcept {
    return kPlayerHostContractV1;
  }

  virtual void capabilities(CancellationTokenV1 cancellation,
                            CapabilitiesCallbackV1 callback) = 0;
  virtual void observe(const ObserveRequestV1& request,
                       CancellationTokenV1 cancellation,
                       ObservationCallbackV1 callback) = 0;
  virtual void dispatch(const GameCommandV1& command,
                        const ValidatedGateV1& gate,
                        CancellationTokenV1 cancellation,
                        CommandCallbackV1 callback) = 0;

  /** Must synchronously clear all movement/action intent before returning. */
  virtual void clearAgentIntent(PreemptionReasonV1 reason) noexcept = 0;
};

}  // namespace crowdy::player_host
