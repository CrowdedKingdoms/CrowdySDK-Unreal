#include "crowdy/studio/integration.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "crowdy/agent/errors.hpp"

namespace crowdy::studio {
namespace {

agent::NativeAgentModeV1 nativeMode(agent::AgentMode mode) {
  switch (mode) {
    case agent::AgentMode::Ask: return agent::NativeAgentModeV1::Ask;
    case agent::AgentMode::Build: return agent::NativeAgentModeV1::Build;
    case agent::AgentMode::Play: return agent::NativeAgentModeV1::Play;
  }
  return agent::NativeAgentModeV1::Ask;
}

bool active(const agent::AgentLease& lease) {
  return lease.status == agent::AgentLeaseStatus::Active;
}

std::optional<player_host::LeaseScopeV1> nativeLeaseScope(
    std::string_view scope) {
  for (const auto candidate : player_host::kLeaseScopesV1) {
    if (player_host::toString(candidate) == scope) return candidate;
  }
  return std::nullopt;
}

std::optional<player_host::AgentControlLeaseV1> nativePlayLease(
    const agent::AgentLease& source) {
  if (source.kind != agent::AgentLeaseKind::Play || !active(source)) {
    return std::nullopt;
  }
  player_host::AgentControlLeaseV1 lease;
  lease.lease_id = source.leaseId;
  lease.kind = player_host::LeaseKindV1::Play;
  lease.status = player_host::LeaseStatusV1::Active;
  lease.client_epoch = source.clientEpoch;
  lease.scopes.reserve(source.scopes.size());
  for (const auto& scope : source.scopes) {
    const auto converted = nativeLeaseScope(scope);
    if (!converted) return std::nullopt;
    lease.scopes.push_back(*converted);
  }
  lease.holder = source.holder;
  lease.controlled_entity_id = source.controlledEntityId;
  lease.host_capability_revision = source.hostCapabilityRevision;
  lease.context_version = source.contextVersion;
  lease.granted_at = source.grantedAt;
  lease.expires_at = source.expiresAt;
  return lease;
}

}  // namespace

std::unique_ptr<CrowdyStudioIntegration>
CrowdyStudioIntegration::create(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime,
    CrowdyStudioAgentRuntimeFactory agentRuntimeFactory) {
  return std::unique_ptr<CrowdyStudioIntegration>(
      new CrowdyStudioIntegration(
          std::move(options), std::move(projectProvider),
          std::move(runtime), std::move(agentRuntimeFactory)));
}

CrowdyStudioIntegration::CrowdyStudioIntegration(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime,
    CrowdyStudioAgentRuntimeFactory agentRuntimeFactory)
    : projectProvider_(std::move(projectProvider)),
      clientRuntimeOwner_(std::move(options.clientRuntime)),
      runtime_(std::move(runtime)),
      crypto_(std::move(options.crypto)),
      editorAdapter_(std::move(options.editor)),
      synchronization_(std::move(options.synchronization)),
      approval_(std::move(options.approval)),
      walletProvider_(std::move(options.walletProvider)),
      layoutStorageOwner_(std::move(options.layoutStorage)),
      playerHost_(options.playerHost),
      leaseManager_(options.leaseManager),
      platformPoll_(std::move(options.platformPoll)) {
  if (!projectProvider_ || !runtime_ || !crypto_ ||
      (!playerHost_ && !leaseManager_) ||
      (playerHost_ && leaseManager_)) {
    throw std::invalid_argument(
        "Crowdy Studio integration requires owned project/runtime/crypto "
        "services and exactly one player host or external lease manager");
  }

  controller_ = std::make_unique<CrowdyStudioController>(
      std::move(options.studio), *projectProvider_, *runtime_, *crypto_,
      options.clock ? *options.clock : core::systemClock(),
      synchronization_.get(), approval_.get(), walletProvider_.get());
  auto layoutOptions = std::move(options.layout);
  if (layoutStorageOwner_) {
    layoutOptions.storage = layoutStorageOwner_.get();
  }
  layoutController_ =
      std::make_unique<StudioLayoutController>(std::move(layoutOptions));

  if (playerHost_) {
    auto leaseOptions = std::move(options.controlLeases);
    if (!leaseOptions.clock) leaseOptions.clock = options.clock;
    const auto fallbackContext = std::move(leaseOptions.context_version);
    leaseOptions.context_version = [this, fallbackContext] {
      const std::string current = currentContextVersion();
      if (!current.empty()) return current;
      return fallbackContext ? fallbackContext() : std::string{};
    };
    ownedLeaseManager_ =
        std::make_unique<player_host::AgentControlLeaseManager>(
            *playerHost_, std::move(leaseOptions));
    leaseManager_ = ownedLeaseManager_.get();
  }
  if (editorAdapter_) {
    editorBridge_ = std::make_unique<CrowdyStudioEditorBridge>(
        *controller_, editorAdapter_);
  }

  auto hostOptions = std::move(options.studioHost);
  const auto fallbackHostSession = hostOptions.sessionId;
  const auto fallbackHostEpoch = hostOptions.clientEpoch;
  const auto fallbackHostContext = hostOptions.contextVersion;
  const auto fallbackHostLeases = hostOptions.leaseKinds;
  const auto fallbackHostCapability =
      hostOptions.hostCapabilityRevision;
  const auto fallbackHostLeaseActive = hostOptions.isLeaseActive;

  hostOptions.sessionId = [this, fallbackHostSession] {
    auto current = currentSessionId();
    if (agentRuntime_) return current;
    return current || !fallbackHostSession ? current
                                           : fallbackHostSession();
  };
  hostOptions.clientEpoch = [this, fallbackHostEpoch] {
    auto current = currentClientEpoch();
    if (agentRuntime_) return current;
    return current || !fallbackHostEpoch ? current
                                         : fallbackHostEpoch();
  };
  hostOptions.contextVersion = [this, fallbackHostContext] {
    if (agentRuntime_) return currentContextVersion();
    if (fallbackHostContext) {
      const std::string fallback = fallbackHostContext();
      if (!fallback.empty()) return fallback;
    }
    return currentContextVersion();
  };
  hostOptions.leaseKinds = [this, fallbackHostLeases] {
    auto values = activeLeaseKinds();
    if (fallbackHostLeases) {
      auto fallback = fallbackHostLeases();
      values.insert(values.end(), fallback.begin(), fallback.end());
      std::sort(values.begin(), values.end());
      values.erase(std::unique(values.begin(), values.end()),
                   values.end());
    }
    return values;
  };
  hostOptions.hostCapabilityRevision =
      [this, fallbackHostCapability] {
        auto revision = hostCapabilityRevision();
        return revision || !fallbackHostCapability
                   ? revision
                   : fallbackHostCapability();
      };
  hostOptions.isLeaseActive =
      [this, fallbackHostLeaseActive](
          std::string_view id, player_host::LeaseKindV1 kind) {
        if (agentRuntime_) {
          return isLeaseActive(id, kind) &&
                 (!fallbackHostLeaseActive ||
                  fallbackHostLeaseActive(id, kind));
        }
        return fallbackHostLeaseActive &&
               fallbackHostLeaseActive(id, kind);
      };
  if (!hostOptions.schedule) {
    hostOptions.schedule = [this](std::function<void()> task) {
      std::lock_guard lock(maintenanceMutex_);
      maintenanceTasks_.push_back(std::move(task));
    };
  }

  studioHost_ =
      std::make_unique<CrowdyStudioControllerHostAdapter>(
          *controller_, *crypto_, std::move(hostOptions));

  auto nativeOptions = std::move(options.nativeTools);
  const auto fallbackSession = nativeOptions.session_id;
  const auto fallbackEpoch = nativeOptions.client_epoch;
  const auto fallbackContext = nativeOptions.context_version;
  const auto fallbackMode = nativeOptions.mode;
  const auto fallbackLeaseActive = nativeOptions.is_lease_active;
  nativeOptions.session_id = [this, fallbackSession] {
    auto current = currentSessionId();
    if (agentRuntime_) return current;
    return current || !fallbackSession ? current : fallbackSession();
  };
  nativeOptions.client_epoch = [this, fallbackEpoch] {
    auto current = currentClientEpoch();
    if (agentRuntime_) return current;
    return current || !fallbackEpoch ? current : fallbackEpoch();
  };
  nativeOptions.context_version = [this, fallbackContext] {
    if (agentRuntime_) return currentContextVersion();
    if (fallbackContext) {
      const std::string fallback = fallbackContext();
      if (!fallback.empty()) return fallback;
    }
    return currentContextVersion();
  };
  nativeOptions.mode = [this, fallbackMode] {
    return agentRuntime_ ? currentMode()
                         : fallbackMode ? fallbackMode()
                                        : agent::NativeAgentModeV1::Ask;
  };
  nativeOptions.is_lease_active =
      [this, fallbackLeaseActive](
          std::string_view id, player_host::LeaseKindV1 kind) {
        if (agentRuntime_) {
          return isLeaseActive(id, kind) &&
                 (!fallbackLeaseActive ||
                  fallbackLeaseActive(id, kind));
        }
        return fallbackLeaseActive && fallbackLeaseActive(id, kind);
      };

  nativeDispatcher_ = std::make_unique<agent::NativeToolDispatcherV1>(
      *leaseManager_, studioHost_.get(), std::move(nativeOptions));
  browserDispatcher_ =
      std::make_unique<agent::NativeBrowserToolDispatcherAdapter>(
          *nativeDispatcher_);

  if (options.agent) {
    if (!agentRuntimeFactory) {
      throw std::invalid_argument(
          "Agent controller options require an agent runtime factory");
    }
    auto agentOptions = std::move(*options.agent);
    const auto beforeAgentWork = agentOptions.beforeAgentWork;
    const auto onEpochAttached = agentOptions.onEpochAttached;
    const auto onLeaseChanged = agentOptions.onLeaseChanged;
    const auto onPreempt = agentOptions.onPreempt;
    agentOptions.browserDispatcher = browserDispatcher_.get();
    agentOptions.beforeAgentWork =
        [this, beforeAgentWork](agent::AgentMode mode) {
          if (const auto failure = prepareForAgentWork(mode)) {
            return failure;
          }
          if (beforeAgentWork) {
            if (const auto failure = beforeAgentWork(mode)) {
              controller_->finishAgentWork(true);
              return failure;
            }
          }
          return std::optional<agent::AgentError>{};
        };
    agentOptions.onEpochAttached =
        [this, onEpochAttached](std::string_view epoch) {
          epochAttached(epoch);
          if (onEpochAttached) onEpochAttached(epoch);
        };
    agentOptions.onLeaseChanged =
        [this, onLeaseChanged](const agent::AgentLease& lease) {
          leaseChanged(lease);
          if (onLeaseChanged) onLeaseChanged(lease);
        };
    agentOptions.onPreempt =
        [this, onPreempt](agent::AgentPreemptionReason reason) {
          preempted(reason);
          if (onPreempt) onPreempt(reason);
        };
    agentRuntime_ = agentRuntimeFactory(std::move(agentOptions));
    if (!agentRuntime_) {
      throw std::runtime_error(
          "Crowdy Studio agent runtime factory returned null");
    }
  }

  auto controlOptions = std::move(options.controlGate);
  if (!controlOptions.clock) controlOptions.clock = options.clock;
  auto clearAgentIntent =
      [this](player_host::PreemptionReasonV1 reason) {
        if (leaseManager_) leaseManager_->preempt(reason);
      };
  controlGate_ =
      std::make_unique<player_host::NativePlayerControlGate>(
          std::move(clearAgentIntent), std::move(controlOptions));
  controlGateUnbind_ =
      agentRuntime_
          ? controlGate_->bind(
                *leaseManager_, agentRuntime_->controller())
          : controlGate_->bind(*leaseManager_);

  stateSubscription_ = controller_->subscribe(
      [this](const CrowdyStudioState& state) {
        if (!disposed_) stateChanged(state);
      });
  humanEditSubscription_ = controller_->onHumanEdit([this] {
    if (!disposed_ && agentRuntime_) {
      agentRuntime_->controller().preemptForHumanEdit();
    }
  });

  if (options.autoInitializeStudio || options.autoInitializeAgent) {
    controller_->initialize();
    studioInitialized_ = true;
  }
  if (options.autoInitializeAgent && agentRuntime_) {
    agentRuntime_->controller().initialize();
    agentInitialized_ = true;
  }
}

CrowdyStudioIntegration::~CrowdyStudioIntegration() { dispose(); }

agent::CrowdyStudioAgentController*
CrowdyStudioIntegration::agentController() {
  return agentRuntime_ ? &agentRuntime_->controller() : nullptr;
}

const agent::CrowdyStudioAgentController*
CrowdyStudioIntegration::agentController() const {
  return agentRuntime_ ? &agentRuntime_->controller() : nullptr;
}

void CrowdyStudioIntegration::initialize() {
  initializeStudio();
  initializeAgent();
}

void CrowdyStudioIntegration::initializeStudio() {
  if (disposed_) {
    throw std::runtime_error("CrowdyStudioIntegration is disposed");
  }
  if (!studioInitialized_) {
    controller_->initialize();
    studioInitialized_ = true;
  }
}

void CrowdyStudioIntegration::initializeAgent() {
  if (disposed_) {
    throw std::runtime_error("CrowdyStudioIntegration is disposed");
  }
  if (agentRuntime_ && !agentInitialized_) {
    agentRuntime_->controller().initialize();
    agentInitialized_ = true;
  }
}

std::size_t CrowdyStudioIntegration::poll() {
  if (disposed_) return 0;
  const auto leaseBeforePoll =
      leaseManager_ ? leaseManager_->snapshot().lease
                    : std::optional<
                          player_host::AgentControlLeaseV1>{};
  std::size_t callbacks = platformPoll_ ? platformPoll_() : 0;
  if (agentRuntime_) {
    callbacks += agentRuntime_->poll();
    synchronizePlayHeartbeat();
  } else {
    nativeDispatcher_->tick();
  }
  if (leaseManager_) {
    leaseManager_->tick();
    if (leaseBeforePoll && !leaseManager_->snapshot().lease &&
        controlGate_) {
      controlGate_->preempt(
          player_host::PreemptionReasonV1::LEASE_EXPIRED);
    }
  }
  return callbacks;
}

std::size_t CrowdyStudioIntegration::tick() {
  return poll();
}

std::size_t CrowdyStudioIntegration::runStudioMaintenance(
    std::size_t maxTasks) {
  if (disposed_) return 0;
  std::size_t completed = 0;
  while (completed < maxTasks) {
    std::function<void()> task;
    {
      std::lock_guard lock(maintenanceMutex_);
      if (maintenanceTasks_.empty()) break;
      task = std::move(maintenanceTasks_.front());
      maintenanceTasks_.pop_front();
    }
    if (task) task();
    ++completed;
  }
  controller_->tick();
  return completed;
}

std::size_t CrowdyStudioIntegration::pendingStudioMaintenance() const {
  std::lock_guard lock(maintenanceMutex_);
  return maintenanceTasks_.size();
}

void CrowdyStudioIntegration::setPageVisible(bool visible) {
  if (disposed_) return;
  controller_->setPageVisible(visible);
  if (agentRuntime_) agentRuntime_->controller().setPageVisible(visible);
}

void CrowdyStudioIntegration::relayout() {
  if (disposed_) return;
  if (editorBridge_) editorBridge_->relayout();
}

void CrowdyStudioIntegration::dispose() noexcept {
  if (disposed_) return;
  disposed_ = true;
  callbackAlive_->store(false, std::memory_order_release);
  if (stateSubscription_ != 0 && controller_) {
    controller_->unsubscribe(stateSubscription_);
    stateSubscription_ = 0;
  }
  if (humanEditSubscription_ != 0 && controller_) {
    controller_->unsubscribeHumanEdit(humanEditSubscription_);
    humanEditSubscription_ = 0;
  }
  if (controlGateUnbind_) {
    controlGateUnbind_();
    controlGateUnbind_ = {};
  }
  if (controlGate_) {
    controlGate_->destroy();
    controlGate_.reset();
  }
  if (agentRuntime_) {
    agentRuntime_->controller().destroy();
    agentRuntime_.reset();
  }
  browserDispatcher_.reset();
  if (nativeDispatcher_) {
    nativeDispatcher_->cancelActive(
        player_host::PreemptionReasonV1::SESSION_CLOSED);
    nativeDispatcher_.reset();
  }
  {
    std::lock_guard lock(maintenanceMutex_);
    maintenanceTasks_.clear();
  }
  if (studioHost_) {
    studioHost_->clearAgentOperation(
        player_host::PreemptionReasonV1::SESSION_CLOSED);
    studioHost_.reset();
  }
  if (editorBridge_) {
    editorBridge_->dispose();
    editorBridge_.reset();
  }
  if (ownedLeaseManager_) {
    ownedLeaseManager_->disconnect();
    ownedLeaseManager_.reset();
    leaseManager_ = nullptr;
  }
  if (controller_) {
    controller_->destroy();
    controller_.reset();
  }
  layoutController_.reset();
}

std::optional<std::string>
CrowdyStudioIntegration::currentSessionId() const {
  if (!agentRuntime_) return std::nullopt;
  const auto& session = agentRuntime_->controller().state().session;
  return session ? std::optional<std::string>(session->sessionId)
                 : std::nullopt;
}

std::optional<std::string>
CrowdyStudioIntegration::currentClientEpoch() const {
  return agentRuntime_ ? agentRuntime_->controller().state().clientEpoch
                       : std::nullopt;
}

std::string CrowdyStudioIntegration::currentContextVersion() const {
  if (agentRuntime_) {
    const auto& session = agentRuntime_->controller().state().session;
    if (session && !session->contextVersion.empty()) {
      return session->contextVersion;
    }
  }
  return controller_->getAgentContext().contextVersion;
}

agent::NativeAgentModeV1 CrowdyStudioIntegration::currentMode() const {
  if (!agentRuntime_) return agent::NativeAgentModeV1::Ask;
  const auto& session = agentRuntime_->controller().state().session;
  return session ? nativeMode(session->mode)
                 : agent::NativeAgentModeV1::Ask;
}

bool CrowdyStudioIntegration::isLeaseActive(
    std::string_view leaseId, player_host::LeaseKindV1 kind) const {
  if (!agentRuntime_) return false;
  const auto expected = kind == player_host::LeaseKindV1::Workspace
                            ? agent::AgentLeaseKind::Workspace
                            : agent::AgentLeaseKind::Play;
  const auto& leases = agentRuntime_->controller().state().leases;
  return std::any_of(
      leases.begin(), leases.end(), [&](const agent::AgentLease& lease) {
        return lease.leaseId == leaseId && lease.kind == expected &&
               active(lease);
      });
}

std::vector<player_host::LeaseKindV1>
CrowdyStudioIntegration::activeLeaseKinds() const {
  std::vector<player_host::LeaseKindV1> values;
  if (!agentRuntime_) return values;
  for (const auto& lease : agentRuntime_->controller().state().leases) {
    if (!active(lease)) continue;
    values.push_back(
        lease.kind == agent::AgentLeaseKind::Workspace
            ? player_host::LeaseKindV1::Workspace
            : player_host::LeaseKindV1::Play);
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::optional<std::string>
CrowdyStudioIntegration::hostCapabilityRevision() const {
  if (!leaseManager_) return std::nullopt;
  const auto snapshot = leaseManager_->snapshot();
  return snapshot.capabilities
             ? std::optional<std::string>(
                   snapshot.capabilities->revision)
             : std::nullopt;
}

std::optional<agent::AgentError>
CrowdyStudioIntegration::prepareForAgentWork(agent::AgentMode) {
  try {
    (void)controller_->prepareForAgentWork();
    return std::nullopt;
  } catch (const CrowdyStudioRevisionConflictError& failure) {
    return agent::makeAgentError(
        "AGENT_CONTEXT_STALE", failure.what());
  } catch (const std::exception& failure) {
    return agent::makeAgentError(
        "AGENT_TOOL_FAILED", failure.what());
  } catch (...) {
    return agent::makeAgentError(
        "AGENT_TOOL_FAILED",
        "Crowdy Studio could not prepare for agent work");
  }
}

void CrowdyStudioIntegration::epochAttached(std::string_view epoch) {
  const auto previous = leaseManager_->snapshot().client_epoch;
  if (previous && *previous != epoch && controlGate_) {
    controlGate_->onClientReattached();
  }
  if (const auto failure = leaseManager_->attach(std::string(epoch))) {
    if (controlGate_) {
      controlGate_->onClientReattached();
    } else {
      leaseManager_->preempt(
          player_host::PreemptionReasonV1::CLIENT_REATTACHED);
    }
  }
  if (controlGate_) controlGate_->refresh();
}

void CrowdyStudioIntegration::leaseChanged(
    const agent::AgentLease& lease) {
  const auto native = nativePlayLease(lease);
  if (lease.kind == agent::AgentLeaseKind::Play && active(lease) &&
      !native) {
    if (controlGate_) {
      controlGate_->preempt(
          player_host::PreemptionReasonV1::CONTEXT_CHANGED);
    }
    return;
  }
  if (native && leaseManager_) {
    const std::weak_ptr<std::atomic<bool>> alive = callbackAlive_;
    leaseManager_->refreshCapabilities(
        [this, alive, native = *native](auto result) {
          const auto lifetime = alive.lock();
          if (!lifetime ||
              !lifetime->load(std::memory_order_acquire)) {
            return;
          }
          if (disposed_ || !agentRuntime_ || !result.ok() ||
              !isLeaseActive(
                  native.lease_id, player_host::LeaseKindV1::Play)) {
            if (!disposed_ && !result.ok() && controlGate_) {
              controlGate_->preempt(
                  player_host::PreemptionReasonV1::CONTROL_TARGET_CHANGED);
            }
            return;
          }
          if (const auto failure = leaseManager_->grantLease(native)) {
            (void)failure;
            if (controlGate_) {
              controlGate_->preempt(
                  player_host::PreemptionReasonV1::CONTROL_TARGET_CHANGED);
            }
            return;
          }
          if (controlGate_) controlGate_->refresh();
        });
  }
  if (controlGate_) controlGate_->refresh();
}

void CrowdyStudioIntegration::synchronizePlayHeartbeat() {
  if (!agentRuntime_ || !leaseManager_) return;
  const auto& state = agentRuntime_->controller().state();
  if (!state.lastHeartbeatAt ||
      state.lastHeartbeatAt == lastAgentHeartbeatAt_) {
    return;
  }
  lastAgentHeartbeatAt_ = state.lastHeartbeatAt;
  const auto local = leaseManager_->snapshot().lease;
  if (!local || local->status != player_host::LeaseStatusV1::Active ||
      !local->controlled_entity_id ||
      !local->host_capability_revision) {
    return;
  }
  const auto failure = leaseManager_->heartbeat({
      .lease_id = local->lease_id,
      .client_epoch = local->client_epoch,
      .context_version = local->context_version,
      .controlled_entity_id = *local->controlled_entity_id,
      .host_capability_revision = *local->host_capability_revision,
  });
  if (failure && controlGate_) {
    const auto reason = leaseManager_->snapshot().last_preemption.value_or(
        player_host::PreemptionReasonV1::CONTEXT_CHANGED);
    controlGate_->preempt(reason);
  }
  if (controlGate_) controlGate_->refresh();
}

void CrowdyStudioIntegration::preempted(
    agent::AgentPreemptionReason reason) {
  const auto native =
      gen::crowdyStudioAgentPreemptionReasonFromString(
          agent::toString(reason));
  if (native && controlGate_) {
    controlGate_->preempt(*native, false);
  } else if (native) {
    leaseManager_->preempt(*native);
  }
}

void CrowdyStudioIntegration::stateChanged(
    const CrowdyStudioState& state) {
  const std::optional<std::string> next =
      state.project
          ? std::optional<std::string>(state.project->projectId)
          : std::nullopt;
  if (next == selectedProjectId_) return;
  selectedProjectId_ = next;
  if (agentRuntime_) {
    agentRuntime_->controller().projectSelectionChanged(next);
  }
}

}  // namespace crowdy::studio
