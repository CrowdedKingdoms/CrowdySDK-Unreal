#include "crowdy/player_host/lease_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace crowdy::player_host {
namespace {

AgentErrorV1 error(std::string code, std::string message,
                   bool retryable = false,
                   std::optional<std::string> field = std::nullopt,
                   std::optional<std::string> scope = std::nullopt) {
  AgentErrorV1 value;
  value.code = std::move(code);
  value.message = std::move(message);
  value.retryable = retryable;
  value.field = std::move(field);
  value.required_scope = std::move(scope);
  return value;
}

template <typename T>
void fail(const std::function<void(AdapterResultV1<T>)>& callback,
          AgentErrorV1 failure, bool ambiguous = false) {
  callback(AdapterResultV1<T>::failure(std::move(failure), ambiguous));
}

bool decimalEpoch(std::string_view value) {
  if (value.empty() || value.size() > 40) return false;
  if (value.size() > 1 && value.front() == '0') return false;
  return std::all_of(value.begin(), value.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; });
}

std::string isoTime(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec,
                static_cast<long long>(epoch_ms % 1'000));
  return buffer;
}

bool hasScope(const AgentControlLeaseV1& lease, LeaseScopeV1 scope) {
  return std::find(lease.scopes.begin(), lease.scopes.end(), scope) !=
         lease.scopes.end();
}

GameCommandResultV1 commandFailure(CommandKindV1 kind, AgentErrorV1 failure,
                                   CommandResultStatusV1 status,
                                   std::optional<std::string> observation_id =
                                       std::nullopt) {
  GameCommandResultV1 result;
  result.status = status;
  result.command_kind = kind;
  result.observation_id = std::move(observation_id);
  result.error = std::move(failure);
  return result;
}

bool actorTargetExists(const GameObservationV1& observation,
                       std::string_view target_ref) {
  return std::any_of(
      observation.nearby_actors.begin(), observation.nearby_actors.end(),
      [&](const ObservationActorV1& actor) {
        return actor.actor_id == target_ref;
      });
}

bool exactTarget(const GameObservationV1& observation,
                 std::string_view target_ref) {
  return observation.target && observation.target->target_id == target_ref &&
         observation.target->kind != ObservationTargetKindV1::None;
}

}  // namespace

struct AgentControlLeaseManager::Impl
    : std::enable_shared_from_this<AgentControlLeaseManager::Impl> {
  struct CallRecord {
    AgentControlDispatchV1 input;
    std::vector<CommandCallbackV1> callbacks;
    std::optional<AdapterResultV1<GameCommandResultV1>> result;
    std::shared_ptr<CancellationSourceV1> cancellation;
    bool submitted = false;
  };

  PlayerHostAdapterV1& adapter;
  AgentControlLeaseManagerOptionsV1 options;
  const core::IClock& clock;
  mutable std::mutex mutex;
  bool connected = false;
  std::optional<std::string> client_epoch;
  std::optional<AgentControlLeaseV1> lease;
  std::optional<PlayerHostCapabilitiesV1> capabilities;
  std::optional<PreemptionReasonV1> last_preemption;
  std::int64_t heartbeat_deadline_monotonic = 0;
  std::uint64_t generation = 0;
  std::unordered_map<std::string, GameObservationV1> observations;
  std::deque<std::string> observation_order;
  std::map<CommandKindV1, std::deque<std::int64_t>> rate_windows;
  std::unordered_map<std::string, std::shared_ptr<CallRecord>> calls;
  std::vector<std::weak_ptr<CancellationSourceV1>> active_cancellations;
  bool shutting_down = false;

  Impl(PlayerHostAdapterV1& host, AgentControlLeaseManagerOptionsV1 config)
      : adapter(host),
        options(std::move(config)),
        clock(options.clock ? *options.clock : core::systemClock()) {
    if (adapter.contractVersion() != kPlayerHostContractV1) {
      throw std::invalid_argument(
          "unsupported PlayerHostAdapterV1 contract version");
    }
    if (options.max_lease_seconds == 0 ||
        options.heartbeat_timeout_ms == 0 ||
        options.max_remembered_calls == 0 ||
        options.max_remembered_observations == 0) {
      throw std::invalid_argument(
          "player-host lease manager bounds must be non-zero");
    }
  }

  AgentControlLeaseSnapshotV1 snapshot() const {
    std::lock_guard lock(mutex);
    return snapshotLocked();
  }

  AgentControlLeaseSnapshotV1 snapshotLocked() const {
    AgentControlLeaseSnapshotV1 value;
    value.connected = connected;
    value.client_epoch = client_epoch;
    value.lease = lease;
    value.capabilities = capabilities;
    value.last_preemption = last_preemption;
    return value;
  }

  void emit() {
    if (!options.on_change) return;
    options.on_change(snapshot());
  }

  std::string contextVersion() const {
    return options.context_version ? options.context_version() : std::string{};
  }

  void rememberCancellation(
      const std::shared_ptr<CancellationSourceV1>& cancellation) {
    std::lock_guard lock(mutex);
    active_cancellations.erase(
        std::remove_if(active_cancellations.begin(), active_cancellations.end(),
                       [](const auto& entry) { return entry.expired(); }),
        active_cancellations.end());
    active_cancellations.push_back(cancellation);
  }

  std::optional<AgentErrorV1> attach(std::string epoch) {
    if (!decimalEpoch(epoch)) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "clientEpoch must be a canonical decimal string", false,
                   "clientEpoch");
    }
    std::optional<std::string> previous;
    {
      std::lock_guard lock(mutex);
      previous = client_epoch;
    }
    if (previous && *previous != epoch) {
      preempt(PreemptionReasonV1::CLIENT_REATTACHED);
    }
    {
      std::lock_guard lock(mutex);
      client_epoch = std::move(epoch);
      connected = true;
    }
    emit();
    return std::nullopt;
  }

  void disconnect() {
    {
      std::lock_guard lock(mutex);
      connected = false;
    }
    preempt(PreemptionReasonV1::DISCONNECTED);
  }

  std::optional<AgentErrorV1> grantLease(
      const AgentControlLeaseV1& candidate) {
    auto valid = validateAgentControlLeaseV1(candidate);
    if (!valid) {
      return error("AGENT_TOOL_INPUT_INVALID", valid.issue()->message, false,
                   valid.issue()->field);
    }
    const std::int64_t now = clock.epochMillis();
    const std::int64_t granted = core::parseIso8601Millis(
        candidate.granted_at.data(), candidate.granted_at.size());
    const std::int64_t expiry = core::parseIso8601Millis(
        candidate.expires_at.data(), candidate.expires_at.size());
    const std::string current_context = contextVersion();
    {
      std::lock_guard lock(mutex);
      if (!connected || !client_epoch) {
        return error("AGENT_DISCONNECTED",
                     "cannot grant Play control while disconnected");
      }
      if (!capabilities) {
        return error("AGENT_HOST_UNAVAILABLE",
                     "refresh capabilities before granting control");
      }
      if (candidate.kind != LeaseKindV1::Play ||
          candidate.status != LeaseStatusV1::Active ||
          candidate.client_epoch != *client_epoch) {
        return error("AGENT_CLIENT_EPOCH_STALE",
                     "Play lease is not active for the attached epoch");
      }
      if (!candidate.host_capability_revision ||
          !candidate.controlled_entity_id ||
          *candidate.host_capability_revision != capabilities->revision ||
          *candidate.controlled_entity_id !=
              capabilities->controlled_entity_id) {
        return error("AGENT_HOST_CAPABILITY_CHANGED",
                     "lease capability revision or target is stale");
      }
      if (!current_context.empty() &&
          candidate.context_version != current_context) {
        return error("AGENT_CONTEXT_STALE",
                     "Play lease belongs to a stale game context");
      }
      const std::int64_t maximum =
          static_cast<std::int64_t>(options.max_lease_seconds) * 1'000;
      if (granted > now + 1'000 || granted > expiry || expiry <= now ||
          expiry - now > maximum) {
        return error("AGENT_LEASE_EXPIRED",
                     "lease expiry is invalid or exceeds the host maximum");
      }
      lease = candidate;
      heartbeat_deadline_monotonic =
          clock.monotonicMillis() +
          std::min<std::int64_t>(
              expiry - now,
              static_cast<std::int64_t>(options.heartbeat_timeout_ms));
    }
    emit();
    return std::nullopt;
  }

  std::optional<AgentErrorV1> heartbeat(
      const AgentControlHeartbeatV1& value) {
    const std::string current_context = contextVersion();
    std::optional<PreemptionReasonV1> reason;
    std::optional<AgentErrorV1> failure;
    {
      std::lock_guard lock(mutex);
      if (!lease || lease->status != LeaseStatusV1::Active ||
          value.lease_id != lease->lease_id) {
        return error("AGENT_LEASE_REQUIRED",
                     "heartbeat requires the current visible Play lease");
      }
      if (!connected || !client_epoch ||
          value.client_epoch != *client_epoch ||
          lease->client_epoch != *client_epoch) {
        reason = PreemptionReasonV1::CLIENT_REATTACHED;
        failure = error("AGENT_CLIENT_EPOCH_STALE",
                        "heartbeat belongs to a stale client epoch");
      } else if ((!current_context.empty() &&
                  value.context_version != current_context) ||
                 value.context_version != lease->context_version) {
        reason = PreemptionReasonV1::CONTEXT_CHANGED;
        failure = error("AGENT_CONTEXT_CHANGED",
                        "heartbeat belongs to a stale game context");
      } else if (!capabilities ||
                 value.controlled_entity_id !=
                     capabilities->controlled_entity_id ||
                 value.host_capability_revision != capabilities->revision) {
        reason = PreemptionReasonV1::CONTROL_TARGET_CHANGED;
        failure = error("AGENT_CONTROL_TARGET_CHANGED",
                        "heartbeat target or capability revision changed");
      } else {
        const std::int64_t now = clock.epochMillis();
        const std::int64_t expiry = core::parseIso8601Millis(
            lease->expires_at.data(), lease->expires_at.size());
        if (expiry <= now) {
          reason = PreemptionReasonV1::LEASE_EXPIRED;
          failure = error("AGENT_LEASE_EXPIRED", "Play lease expired");
        } else {
          heartbeat_deadline_monotonic =
              clock.monotonicMillis() +
              std::min<std::int64_t>(
                  expiry - now,
                  static_cast<std::int64_t>(options.heartbeat_timeout_ms));
        }
      }
    }
    if (reason) preempt(*reason);
    return failure;
  }

  void tick() {
    bool expired = false;
    {
      std::lock_guard lock(mutex);
      if (lease) {
        const std::int64_t expiry = core::parseIso8601Millis(
            lease->expires_at.data(), lease->expires_at.size());
        expired = expiry <= clock.epochMillis() ||
                  clock.monotonicMillis() >= heartbeat_deadline_monotonic;
      }
    }
    if (expired) preempt(PreemptionReasonV1::LEASE_EXPIRED);
  }

  void shutdown() {
    std::vector<std::shared_ptr<CancellationSourceV1>> cancellations;
    {
      std::lock_guard lock(mutex);
      shutting_down = true;
      for (const auto& weak : active_cancellations) {
        if (auto value = weak.lock()) cancellations.push_back(std::move(value));
      }
    }
    for (const auto& value : cancellations) value->cancel();
  }

  void preempt(PreemptionReasonV1 reason);
  void refreshCapabilities(CapabilitiesCallbackV1 callback);
  void observe(const ObserveRequestV1& request,
               const std::optional<AgentObservationDispatchV1>& dispatch,
               ObservationCallbackV1 callback);
  void dispatch(AgentControlDispatchV1 input, CommandCallbackV1 callback);
  void completeCall(
      const std::string& tool_call_id,
      AdapterResultV1<GameCommandResultV1> result);
  void clearClosedSession();
};

void AgentControlLeaseManager::Impl::preempt(PreemptionReasonV1 reason) {
  // Host intent is cleared first. Keep this before SDK state and callbacks.
  adapter.clearAgentIntent(reason);

  std::vector<std::shared_ptr<CancellationSourceV1>> cancellations;
  std::vector<std::pair<CommandCallbackV1,
                        AdapterResultV1<GameCommandResultV1>>>
      completions;
  {
    std::lock_guard lock(mutex);
    ++generation;
    lease.reset();
    heartbeat_deadline_monotonic = 0;
    observations.clear();
    observation_order.clear();
    rate_windows.clear();
    last_preemption = reason;
    for (const auto& weak : active_cancellations) {
      if (auto value = weak.lock()) cancellations.push_back(std::move(value));
    }
    for (auto& [id, record] : calls) {
      (void)id;
      if (record->result || !record->submitted) continue;
      const auto* planned = plannedCommand(record->input.command);
      auto result = commandFailure(
          commandKind(record->input.command),
          error("AGENT_TOOL_OUTCOME_UNKNOWN",
                "game command was preempted after host dispatch"),
          CommandResultStatusV1::OutcomeUnknown,
          planned ? std::optional<std::string>(planned->observation_id)
                  : std::nullopt);
      auto adapter_result =
          AdapterResultV1<GameCommandResultV1>::success(std::move(result));
      record->result = adapter_result;
      for (auto& callback : record->callbacks) {
        completions.emplace_back(std::move(callback), adapter_result);
      }
      record->callbacks.clear();
    }
  }
  for (const auto& cancellation : cancellations) cancellation->cancel();
  emit();
  for (auto& [callback, result] : completions) callback(std::move(result));
}

void AgentControlLeaseManager::Impl::refreshCapabilities(
    CapabilitiesCallbackV1 callback) {
  auto cancellation = std::make_shared<CancellationSourceV1>();
  std::uint64_t started_generation = 0;
  {
    std::lock_guard lock(mutex);
    started_generation = generation;
  }
  rememberCancellation(cancellation);
  const std::weak_ptr<Impl> weak = shared_from_this();
  try {
    adapter.capabilities(
        cancellation->token(),
        [weak, callback = std::move(callback), cancellation,
         started_generation](
            AdapterResultV1<PlayerHostCapabilitiesV1> result) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (!result.ok()) {
            callback(std::move(result));
            return;
          }
          const auto validation =
              validatePlayerHostCapabilitiesV1(*result.value);
          if (!validation) {
            fail<PlayerHostCapabilitiesV1>(
                callback,
                error("AGENT_TOOL_OUTPUT_INVALID",
                      validation.issue()->message, false,
                      validation.issue()->field));
            return;
          }
          std::optional<PreemptionReasonV1> reason;
          bool fenced = false;
          bool revision_confused = false;
          {
            std::lock_guard lock(self->mutex);
            fenced = self->shutting_down || cancellation->cancelled() ||
                     started_generation != self->generation;
            if (!fenced && self->capabilities) {
              revision_confused =
                  self->capabilities->revision == result.value->revision &&
                  (self->capabilities->game_id != result.value->game_id ||
                   self->capabilities->controlled_entity_id !=
                       result.value->controlled_entity_id ||
                   self->capabilities->commands != result.value->commands ||
                   self->capabilities->observation !=
                       result.value->observation);
              if (!revision_confused && self->lease &&
                  (self->capabilities->revision != result.value->revision ||
                   self->capabilities->game_id != result.value->game_id ||
                   self->capabilities->controlled_entity_id !=
                       result.value->controlled_entity_id)) {
                reason = self->capabilities->controlled_entity_id !=
                                 result.value->controlled_entity_id
                             ? PreemptionReasonV1::CONTROL_TARGET_CHANGED
                             : PreemptionReasonV1::CONTEXT_CHANGED;
              }
            }
            if (!fenced && !revision_confused) {
              self->capabilities = *result.value;
            }
          }
          if (fenced) {
            fail<PlayerHostCapabilitiesV1>(
                callback,
                error("AGENT_CANCELLED",
                      "capability refresh was fenced by preemption"));
            return;
          }
          if (revision_confused) {
            fail<PlayerHostCapabilitiesV1>(
                callback,
                error("AGENT_HOST_CAPABILITY_CHANGED",
                      "host changed capability content without a new revision"));
            return;
          }
          if (reason) self->preempt(*reason);
          self->emit();
          callback(std::move(result));
        });
  } catch (...) {
    fail<PlayerHostCapabilitiesV1>(
        callback, error("AGENT_HOST_UNAVAILABLE",
                        "native host capability call failed"));
  }
}

void AgentControlLeaseManager::Impl::observe(
    const ObserveRequestV1& request,
    const std::optional<AgentObservationDispatchV1>& dispatch,
    ObservationCallbackV1 callback) {
  const auto request_validation = validateObserveRequestV1(request);
  if (!request_validation) {
    fail<GameObservationV1>(
        callback,
        error("AGENT_TOOL_INPUT_INVALID", request_validation.issue()->message,
              false, request_validation.issue()->field));
    return;
  }
  tick();
  const std::string current_context = contextVersion();
  PlayerHostCapabilitiesV1 current_capabilities;
  std::uint64_t started_generation = 0;
  std::optional<AgentErrorV1> initial_error;
  {
    std::lock_guard lock(mutex);
    if (!connected || !client_epoch) {
      initial_error = error("AGENT_DISCONNECTED",
                            "native player host is disconnected");
    } else if (!capabilities) {
      initial_error =
          error("AGENT_HOST_UNAVAILABLE",
                "capabilities must be refreshed before observation");
    } else if (request.max_nearby_actors >
                   capabilities->observation.max_nearby_actors ||
               request.max_nearby_voxels >
                   capabilities->observation.max_nearby_voxels) {
      initial_error =
          error("AGENT_TOOL_INPUT_INVALID",
                "observation request exceeds host-advertised bounds");
    } else if (dispatch) {
      if (dispatch->client_epoch != *client_epoch) {
        initial_error =
            error("AGENT_CLIENT_EPOCH_STALE",
                  "observation belongs to a stale client epoch");
      } else if (dispatch->lease_id) {
        if (!lease || lease->status != LeaseStatusV1::Active ||
            *dispatch->lease_id != lease->lease_id) {
          initial_error =
              error("AGENT_LEASE_REQUIRED",
                    "Play observation requires the current visible lease");
        } else if (!hasScope(*lease, LeaseScopeV1::Observe)) {
          initial_error =
              error("AGENT_LEASE_SCOPE_MISSING",
                    "game observation requires observe scope", false,
                    std::nullopt, "observe");
        } else if ((!current_context.empty() &&
                    lease->context_version != current_context) ||
                   !lease->host_capability_revision ||
                   *lease->host_capability_revision !=
                       capabilities->revision ||
                   !lease->controlled_entity_id ||
                   *lease->controlled_entity_id !=
                       capabilities->controlled_entity_id) {
          initial_error =
              error("AGENT_CONTEXT_CHANGED",
                    "observation lease context or target changed");
        }
      }
    }
    if (!initial_error) {
      current_capabilities = *capabilities;
      started_generation = generation;
    }
  }
  if (initial_error) {
    fail<GameObservationV1>(callback, std::move(*initial_error));
    return;
  }

  auto cancellation = std::make_shared<CancellationSourceV1>();
  rememberCancellation(cancellation);
  const std::weak_ptr<Impl> weak = shared_from_this();
  try {
    adapter.observe(
        request, cancellation->token(),
        [weak, callback = std::move(callback), cancellation,
         current_capabilities, request,
         started_generation](AdapterResultV1<GameObservationV1> result) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (!result.ok()) {
            callback(std::move(result));
            return;
          }
          const auto validation = validateGameObservationV1(*result.value);
          if (!validation) {
            fail<GameObservationV1>(
                callback,
                error("AGENT_TOOL_OUTPUT_INVALID",
                      validation.issue()->message, false,
                      validation.issue()->field));
            return;
          }
          if (result.value->nearby_actors.size() >
                  request.max_nearby_actors ||
              result.value->nearby_voxels.size() >
                  request.max_nearby_voxels) {
            fail<GameObservationV1>(
                callback,
                error("AGENT_TOOL_OUTPUT_INVALID",
                      "observation exceeded requested deterministic bounds"));
            return;
          }
          const std::int64_t now = self->clock.epochMillis();
          const std::int64_t observed = core::parseIso8601Millis(
              result.value->observed_at.data(),
              result.value->observed_at.size());
          const std::int64_t expires = core::parseIso8601Millis(
              result.value->expires_at.data(), result.value->expires_at.size());
          if (observed > now + 1'000 || expires <= now ||
              expires < observed ||
              now - observed >
                  current_capabilities.observation.max_age_ms) {
            fail<GameObservationV1>(
                callback,
                error("AGENT_OBSERVATION_STALE",
                      "observation freshness bounds are invalid"));
            return;
          }
          if (result.value->capability_revision !=
                  current_capabilities.revision ||
              result.value->controlled_entity_id !=
                  current_capabilities.controlled_entity_id) {
            fail<GameObservationV1>(
                callback,
                error("AGENT_CONTROL_TARGET_CHANGED",
                      "observation target or capability revision changed"));
            return;
          }
          bool fenced = false;
          {
            std::lock_guard lock(self->mutex);
            fenced = self->shutting_down || cancellation->cancelled() ||
                     started_generation != self->generation ||
                     !self->capabilities ||
                     self->capabilities->revision !=
                         current_capabilities.revision ||
                     self->capabilities->controlled_entity_id !=
                         current_capabilities.controlled_entity_id;
            if (!fenced) {
              const std::string id = result.value->observation_id;
              if (self->observations.find(id) == self->observations.end()) {
                self->observation_order.push_back(id);
              }
              self->observations[id] = *result.value;
              while (self->observation_order.size() >
                     self->options.max_remembered_observations) {
                self->observations.erase(self->observation_order.front());
                self->observation_order.pop_front();
              }
            }
          }
          if (fenced) {
            fail<GameObservationV1>(
                callback,
                error("AGENT_CONTEXT_STALE",
                      "late observation was fenced by context preemption"));
            return;
          }
          callback(std::move(result));
        });
  } catch (...) {
    fail<GameObservationV1>(
        callback,
        error("AGENT_HOST_UNAVAILABLE", "native host observation call failed"));
  }
}

void AgentControlLeaseManager::Impl::completeCall(
    const std::string& tool_call_id,
    AdapterResultV1<GameCommandResultV1> result) {
  std::vector<CommandCallbackV1> callbacks;
  {
    std::lock_guard lock(mutex);
    const auto found = calls.find(tool_call_id);
    if (found == calls.end() || found->second->result) return;
    found->second->result = result;
    callbacks = std::move(found->second->callbacks);
  }
  for (auto& callback : callbacks) callback(result);
}

void AgentControlLeaseManager::Impl::dispatch(AgentControlDispatchV1 input,
                                               CommandCallbackV1 callback) {
  if (input.tool_call_id.empty() || input.tool_call_id.size() > 128 ||
      !decimalEpoch(input.client_epoch) ||
      (input.lease_id &&
       (input.lease_id->empty() || input.lease_id->size() > 128)) ||
      (input.approval_grant &&
       (input.approval_grant->empty() ||
        input.approval_grant->size() > 512))) {
    fail<GameCommandResultV1>(
        callback,
        error("AGENT_TOOL_INPUT_INVALID",
              "game dispatch envelope is outside protocol bounds"));
    return;
  }
  const auto command_validation = validateGameCommandV1(input.command);
  if (!command_validation) {
    fail<GameCommandResultV1>(
        callback,
        error("AGENT_TOOL_INPUT_INVALID", command_validation.issue()->message,
              false, command_validation.issue()->field));
    return;
  }
  const CommandKindV1 kind = commandKind(input.command);

  std::optional<AdapterResultV1<GameCommandResultV1>> replay;
  bool conflict = false;
  bool cache_full = false;
  {
    std::lock_guard lock(mutex);
    const auto found = calls.find(input.tool_call_id);
    if (found != calls.end()) {
      if (found->second->input != input) {
        conflict = true;
      } else if (found->second->result) {
        replay = found->second->result;
      } else {
        found->second->callbacks.push_back(std::move(callback));
        return;
      }
    } else if (calls.size() >= options.max_remembered_calls) {
      cache_full = true;
    } else {
      auto record = std::make_shared<CallRecord>();
      record->input = input;
      record->callbacks.push_back(std::move(callback));
      calls.emplace(input.tool_call_id, std::move(record));
    }
  }
  if (conflict) {
    fail<GameCommandResultV1>(
        callback,
        error("AGENT_IDEMPOTENCY_CONFLICT",
              "tool call changed after its first dispatch"));
    return;
  }
  if (replay) {
    callback(std::move(*replay));
    return;
  }
  if (cache_full) {
    if (kind == CommandKindV1::Stop) {
      preempt(PreemptionReasonV1::HUMAN_STOP);
      GameCommandResultV1 result;
      result.status = CommandResultStatusV1::Succeeded;
      result.command_kind = CommandKindV1::Stop;
      callback(AdapterResultV1<GameCommandResultV1>::success(
          std::move(result)));
      return;
    }
    fail<GameCommandResultV1>(
        callback,
        error("AGENT_RATE_LIMITED",
              "native execute-once result cache is full"));
    return;
  }

  const PlannedCommandV1* common = plannedCommand(input.command);
  if (kind == CommandKindV1::Stop) {
    preempt(PreemptionReasonV1::HUMAN_STOP);
    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = CommandKindV1::Stop;
    completeCall(input.tool_call_id,
                 AdapterResultV1<GameCommandResultV1>::success(
                     std::move(result)));
    return;
  }

  tick();
  const std::string current_context = contextVersion();
  std::optional<AgentErrorV1> validation_error;
  std::optional<PreemptionReasonV1> preemption;
  std::optional<ValidatedGateV1> gate;
  std::optional<GameObservationV1> observation;
  std::uint64_t started_generation = 0;
  {
    std::lock_guard lock(mutex);
    if (!connected || !client_epoch) {
      validation_error =
          error("AGENT_DISCONNECTED", "native player host is disconnected");
    } else if (input.client_epoch != *client_epoch) {
      validation_error =
          error("AGENT_CLIENT_EPOCH_STALE",
                "game command belongs to a stale client epoch");
    } else if (!capabilities) {
      validation_error =
          error("AGENT_HOST_UNAVAILABLE", "host capabilities are unavailable");
    } else {
      const auto advertised = std::find_if(
          capabilities->commands.begin(), capabilities->commands.end(),
          [&](const CommandCapabilityV1& value) { return value.kind == kind; });
      if (advertised == capabilities->commands.end()) {
        validation_error =
            error("AGENT_HOST_UNAVAILABLE",
                  "host does not advertise this command kind");
      } else if (!lease || lease->status != LeaseStatusV1::Active ||
                 !input.lease_id || *input.lease_id != lease->lease_id) {
        validation_error =
            error("AGENT_LEASE_REQUIRED",
                  "game command requires the current visible Play lease");
      } else if (lease->client_epoch != *client_epoch) {
        preemption = PreemptionReasonV1::CLIENT_REATTACHED;
        validation_error =
            error("AGENT_CLIENT_EPOCH_STALE",
                  "Play lease belongs to a stale client epoch");
      } else if (!lease->host_capability_revision ||
                 !lease->controlled_entity_id ||
                 *lease->host_capability_revision != capabilities->revision ||
                 *lease->controlled_entity_id !=
                     capabilities->controlled_entity_id) {
        preemption = PreemptionReasonV1::CONTROL_TARGET_CHANGED;
        validation_error =
            error("AGENT_CONTROL_TARGET_CHANGED",
                  "controlled entity or host capability changed");
      } else if ((!current_context.empty() &&
                  lease->context_version != current_context)) {
        preemption = PreemptionReasonV1::CONTEXT_CHANGED;
        validation_error =
            error("AGENT_CONTEXT_CHANGED",
                  "Play lease game context changed");
      } else if (advertised->required_scope &&
                 !hasScope(*lease, *advertised->required_scope)) {
        validation_error =
            error("AGENT_LEASE_SCOPE_MISSING",
                  "game command is missing its required Play scope", false,
                  std::nullopt,
                  std::string(toString(*advertised->required_scope)));
      } else {
        const auto found = observations.find(common->observation_id);
        if (found == observations.end()) {
          validation_error =
              error("AGENT_OBSERVATION_STALE",
                    "command observation is missing or invalidated");
        } else {
          observation = found->second;
          const std::int64_t now = clock.epochMillis();
          const std::int64_t observed = core::parseIso8601Millis(
              observation->observed_at.data(),
              observation->observed_at.size());
          const std::int64_t expires = core::parseIso8601Millis(
              observation->expires_at.data(), observation->expires_at.size());
          if (expires <= now || expires < observed ||
              observed > now + 1'000 ||
              now - observed > capabilities->observation.max_age_ms) {
            validation_error =
                error("AGENT_OBSERVATION_STALE",
                      "game observation is stale");
          } else if (observation->capability_revision !=
                         capabilities->revision ||
                     observation->controlled_entity_id !=
                         capabilities->controlled_entity_id ||
                     common->capability_revision !=
                         observation->capability_revision ||
                     common->controlled_entity_id !=
                         observation->controlled_entity_id) {
            validation_error =
                error("AGENT_CONTROL_TARGET_CHANGED",
                      "command target does not match its observation");
          } else if (!observation->player.alive) {
            preemption = PreemptionReasonV1::DEATH;
            validation_error =
                error("AGENT_PREEMPTED", "player is dead");
          } else if (observation->input_state.human_input_active) {
            preemption = PreemptionReasonV1::HUMAN_INPUT;
            validation_error =
                error("AGENT_PREEMPTED", "human input took control");
          } else if (observation->input_state.modal_open ||
                     observation->input_state.text_input_focused) {
            validation_error =
                error("AGENT_CONTEXT_CHANGED",
                      "game modal or text input blocks agent control");
          } else {
            bool target_stale = false;
            std::visit(
                [&](const auto& command) {
                  using T = std::decay_t<decltype(command)>;
                  if constexpr (std::is_same_v<
                                    T, InventoryTransferCommandV1>) {
                    target_stale =
                        !exactTarget(*observation, command.container_ref);
                  } else if constexpr (std::is_same_v<T,
                                                       InteractCommandV1>) {
                    target_stale =
                        !exactTarget(*observation, command.target_ref);
                  } else if constexpr (std::is_same_v<T, MountCommandV1>) {
                    if (command.action == MountActionV1::Mount) {
                      target_stale =
                          !command.mount_ref ||
                          !exactTarget(*observation, *command.mount_ref) ||
                          !actorTargetExists(*observation, *command.mount_ref);
                    }
                  } else if constexpr (std::is_same_v<
                                           T, CombatAttackCommandV1>) {
                    target_stale =
                        !exactTarget(*observation, command.target_ref) ||
                        !actorTargetExists(*observation, command.target_ref);
                  }
                },
                input.command);
            if (target_stale) {
              validation_error =
                  error("AGENT_OBSERVATION_STALE",
                        "command target is not the fresh observed target");
            } else {
              bool approval_required =
                  advertised->approval == ApprovalPolicyV1::Required;
              bool approval_validator_failed = false;
              if (advertised->approval == ApprovalPolicyV1::Conditional) {
                if (options.conditional_approval_required) {
                  try {
                    approval_required =
                        options.conditional_approval_required(input.command,
                                                              *observation);
                  } catch (...) {
                    approval_validator_failed = true;
                  }
                } else if (const auto* attack =
                               std::get_if<CombatAttackCommandV1>(
                                   &input.command)) {
                  const auto actor = std::find_if(
                      observation->nearby_actors.begin(),
                      observation->nearby_actors.end(),
                      [&](const ObservationActorV1& value) {
                        return value.actor_id == attack->target_ref;
                      });
                  approval_required =
                      actor != observation->nearby_actors.end() &&
                      actor->kind == ActorKindV1::Player;
                }
              }
              bool approval_matches = true;
              if (approval_required && input.approval_grant &&
                  options.validate_approval_grant) {
                try {
                  approval_matches = options.validate_approval_grant(input);
                } catch (...) {
                  approval_validator_failed = true;
                  approval_matches = false;
                }
              }
              if (approval_validator_failed) {
                validation_error =
                    error("AGENT_APPROVAL_REQUIRED",
                          "local approval validator failed closed");
              } else if (approval_required && !input.approval_grant) {
                validation_error =
                    error("AGENT_APPROVAL_REQUIRED",
                          "game command requires exact human approval");
              } else if (approval_required && !approval_matches) {
                validation_error =
                    error("AGENT_APPROVAL_MISMATCH",
                          "approval grant does not bind this exact command");
              } else {
                auto& window = rate_windows[kind];
                while (!window.empty() &&
                       now - window.front() >= 1'000) {
                  window.pop_front();
                }
                if (window.size() >=
                    static_cast<std::size_t>(
                        advertised->rate_limit_per_second)) {
                  validation_error =
                      error("AGENT_RATE_LIMITED",
                            "command exceeded the serialized host rate limit",
                            true);
                } else {
                  window.push_back(now);
                  ValidatedGateV1 next_gate;
                  next_gate.client_epoch = *client_epoch;
                  next_gate.lease_id = lease->lease_id;
                  next_gate.scopes = lease->scopes;
                  next_gate.context_version = lease->context_version;
                  next_gate.observation_id = observation->observation_id;
                  next_gate.validated_at = isoTime(now);
                  gate = std::move(next_gate);
                  started_generation = generation;
                }
              }
            }
          }
        }
      }
    }
  }

  if (preemption) preempt(*preemption);
  if (validation_error) {
    completeCall(
        input.tool_call_id,
        AdapterResultV1<GameCommandResultV1>::failure(
            std::move(*validation_error)));
    return;
  }

  auto cancellation = std::make_shared<CancellationSourceV1>();
  {
    std::lock_guard lock(mutex);
    const auto found = calls.find(input.tool_call_id);
    if (found == calls.end() || found->second->result) return;
    found->second->cancellation = cancellation;
    found->second->submitted = true;
  }
  rememberCancellation(cancellation);
  const std::weak_ptr<Impl> weak = shared_from_this();
  const std::string tool_call_id = input.tool_call_id;
  const std::optional<std::string> observation_id = common->observation_id;
  try {
    adapter.dispatch(
        input.command, *gate, cancellation->token(),
        [weak, cancellation, tool_call_id, kind, observation_id,
         started_generation](
            AdapterResultV1<GameCommandResultV1> result) mutable {
          const auto self = weak.lock();
          if (!self) return;
          bool fenced = false;
          {
            std::lock_guard lock(self->mutex);
            fenced = self->shutting_down || cancellation->cancelled() ||
                     self->generation != started_generation;
          }
          if (fenced) {
            auto unknown = commandFailure(
                kind,
                error("AGENT_TOOL_OUTCOME_UNKNOWN",
                      "late game result was fenced after preemption"),
                CommandResultStatusV1::OutcomeUnknown, observation_id);
            self->completeCall(
                tool_call_id,
                AdapterResultV1<GameCommandResultV1>::success(
                    std::move(unknown)));
            return;
          }
          if (!result.ok()) {
            if (result.outcome_unknown) {
              auto unknown = commandFailure(
                  kind,
                  result.error.value_or(error(
                      "AGENT_TOOL_OUTCOME_UNKNOWN",
                      "native host could not confirm the command outcome")),
                  CommandResultStatusV1::OutcomeUnknown, observation_id);
              self->completeCall(
                  tool_call_id,
                  AdapterResultV1<GameCommandResultV1>::success(
                      std::move(unknown)));
            } else {
              self->completeCall(tool_call_id, std::move(result));
            }
            return;
          }
          const auto validation = validateGameCommandResultV1(*result.value);
          if (!validation || result.value->command_kind != kind) {
            auto unknown = commandFailure(
                kind,
                error("AGENT_TOOL_OUTPUT_INVALID",
                      "host returned an invalid command result"),
                CommandResultStatusV1::OutcomeUnknown, observation_id);
            self->completeCall(
                tool_call_id,
                AdapterResultV1<GameCommandResultV1>::success(
                    std::move(unknown)));
            return;
          }
          self->completeCall(tool_call_id, std::move(result));
        });
  } catch (...) {
    auto unknown = commandFailure(
        kind,
        error("AGENT_TOOL_OUTCOME_UNKNOWN",
              "native host call failed after command submission"),
        CommandResultStatusV1::OutcomeUnknown, observation_id);
    completeCall(input.tool_call_id,
                 AdapterResultV1<GameCommandResultV1>::success(
                     std::move(unknown)));
  }
}

void AgentControlLeaseManager::Impl::clearClosedSession() {
  std::lock_guard lock(mutex);
  calls.clear();
}

AgentControlLeaseManager::AgentControlLeaseManager(
    PlayerHostAdapterV1& adapter, AgentControlLeaseManagerOptionsV1 options)
    : impl_(std::make_shared<Impl>(adapter, std::move(options))) {}

AgentControlLeaseManager::~AgentControlLeaseManager() {
  if (impl_) impl_->shutdown();
}

AgentControlLeaseManager::AgentControlLeaseManager(
    AgentControlLeaseManager&&) noexcept = default;

AgentControlLeaseManager& AgentControlLeaseManager::operator=(
    AgentControlLeaseManager&&) noexcept = default;

AgentControlLeaseSnapshotV1 AgentControlLeaseManager::snapshot() const {
  return impl_->snapshot();
}

std::optional<AgentErrorV1> AgentControlLeaseManager::attach(
    std::string client_epoch) {
  return impl_->attach(std::move(client_epoch));
}

void AgentControlLeaseManager::disconnect() { impl_->disconnect(); }

void AgentControlLeaseManager::refreshCapabilities(
    CapabilitiesCallbackV1 callback) {
  impl_->refreshCapabilities(std::move(callback));
}

std::optional<AgentErrorV1> AgentControlLeaseManager::grantLease(
    const AgentControlLeaseV1& lease) {
  return impl_->grantLease(lease);
}

std::optional<AgentErrorV1> AgentControlLeaseManager::heartbeat(
    const AgentControlHeartbeatV1& heartbeat) {
  return impl_->heartbeat(heartbeat);
}

void AgentControlLeaseManager::observe(
    const ObserveRequestV1& request,
    std::optional<AgentObservationDispatchV1> dispatch,
    ObservationCallbackV1 callback) {
  if (!impl_->snapshot().capabilities) {
    const std::weak_ptr<Impl> weak = impl_;
    impl_->refreshCapabilities(
        [weak, request, dispatch = std::move(dispatch),
         callback = std::move(callback)](
            AdapterResultV1<PlayerHostCapabilitiesV1> refreshed) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (!refreshed.ok()) {
            callback(AdapterResultV1<GameObservationV1>::failure(
                refreshed.error.value_or(
                    error("AGENT_HOST_UNAVAILABLE",
                          "could not refresh host capabilities"))));
            return;
          }
          self->observe(request, dispatch, std::move(callback));
        });
    return;
  }
  impl_->observe(request, dispatch, std::move(callback));
}

void AgentControlLeaseManager::dispatch(AgentControlDispatchV1 input,
                                        CommandCallbackV1 callback) {
  impl_->dispatch(std::move(input), std::move(callback));
}

void AgentControlLeaseManager::tick() { impl_->tick(); }

void AgentControlLeaseManager::preempt(PreemptionReasonV1 reason) {
  impl_->preempt(reason);
}

void AgentControlLeaseManager::clearClosedSession() {
  impl_->clearClosedSession();
}

}  // namespace crowdy::player_host
