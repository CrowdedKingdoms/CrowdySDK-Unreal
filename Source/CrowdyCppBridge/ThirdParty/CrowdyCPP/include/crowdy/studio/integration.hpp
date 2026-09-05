#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "crowdy/agent/client_runtime.hpp"
#include "crowdy/agent/native_browser_dispatcher.hpp"
#include "crowdy/player_host/control_gate.hpp"
#include "crowdy/player_host/lease_manager.hpp"
#include "crowdy/studio/editor.hpp"
#include "crowdy/studio/host_adapter.hpp"
#include "crowdy/studio/layout.hpp"

namespace crowdy::studio {

using CrowdyStudioAgentRuntimeFactory = std::function<
    std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime>(
        agent::CrowdyStudioAgentControllerOptions)>;

struct CrowdyStudioIntegrationOptions {
  CrowdyStudioControllerOptions studio;
  std::shared_ptr<const core::ICrypto> crypto;
  const core::IClock* clock = nullptr;
  std::shared_ptr<ICrowdyStudioEditorAdapter> editor;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntime;
  std::shared_ptr<ICrowdyStudioSynchronizationProvider> synchronization;
  std::shared_ptr<ICrowdyStudioApprovalGate> approval;
  std::shared_ptr<ICrowdyStudioWalletProvider> walletProvider;
  /** CrowdyClient factories install their read-only PlayerWallet adapter. */
  bool observePlayerWallet = true;

  StudioLayoutControllerOptions layout;
  /** Optional owner for layout.storage; overrides layout.storage when set. */
  std::shared_ptr<ICrowdyStudioLayoutStorage> layoutStorage;

  /**
   * Preferred complete-assembly path. The engine-owned player host must
   * outlive the integration, which owns the exact lease manager bound to both
   * native dispatch and the concrete human-control gate.
   */
  player_host::PlayerHostAdapterV1* playerHost = nullptr;
  player_host::AgentControlLeaseManagerOptionsV1 controlLeases;
  player_host::NativePlayerControlGateOptionsV1 controlGate;

  /** Compatibility path: borrow one exact externally owned lease manager. */
  player_host::AgentControlLeaseManager* leaseManager = nullptr;
  agent::NativeToolDispatcherOptionsV1 nativeTools;
  CrowdyStudioControllerHostAdapterOptions studioHost;
  std::optional<agent::CrowdyStudioAgentControllerOptions> agent;

  std::function<std::size_t()> platformPoll;
  bool autoInitializeStudio = false;
  bool autoInitializeAgent = false;
};

/**
 * Destruction-safe native Studio assembly. Shared providers and runtimes are
 * retained behind the controller; agent/browser/native adapters are destroyed
 * before the controller. The preferred playerHost path owns one exact lease
 * manager shared by native dispatch and the concrete human-control gate.
 */
class CrowdyStudioIntegration {
 public:
  static std::unique_ptr<CrowdyStudioIntegration> create(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime,
      CrowdyStudioAgentRuntimeFactory agentRuntimeFactory = {});

  ~CrowdyStudioIntegration();
  CrowdyStudioIntegration(const CrowdyStudioIntegration&) = delete;
  CrowdyStudioIntegration& operator=(const CrowdyStudioIntegration&) =
      delete;

  CrowdyStudioController& studio() { return *controller_; }
  const CrowdyStudioController& studio() const { return *controller_; }
  CrowdyStudioEditorBridge* editor() { return editorBridge_.get(); }
  const CrowdyStudioEditorBridge* editor() const {
    return editorBridge_.get();
  }
  agent::CrowdyStudioAgentController* agentController();
  const agent::CrowdyStudioAgentController* agentController() const;
  StudioLayoutController& layout() { return *layoutController_; }
  const StudioLayoutController& layout() const {
    return *layoutController_;
  }
  StudioLayoutState layoutSnapshot() const {
    return layoutController_->getState();
  }
  player_host::AgentControlLeaseManager& leaseManager() {
    return *leaseManager_;
  }
  const player_host::AgentControlLeaseManager& leaseManager() const {
    return *leaseManager_;
  }
  player_host::AgentControlLeaseSnapshotV1 leaseSnapshot() const {
    return leaseManager_->snapshot();
  }
  player_host::NativePlayerControlGate& controlGate() {
    return *controlGate_;
  }
  const player_host::NativePlayerControlGate& controlGate() const {
    return *controlGate_;
  }
  player_host::NativePlayerControlGateSnapshotV1 controlSnapshot() const {
    return controlGate_->snapshot();
  }
  ICrowdyStudioWalletProvider* walletProvider() {
    return walletProvider_.get();
  }
  const ICrowdyStudioWalletProvider* walletProvider() const {
    return walletProvider_.get();
  }
  agent::NativeToolDispatcherV1& nativeTools() {
    return *nativeDispatcher_;
  }

  void initializeStudio();
  void initializeAgent();
  void initialize();
  /** Nonblocking platform/Agent callback and deadline pump. */
  std::size_t poll();
  /** Compatibility spelling for poll(); it performs no Studio HTTP or save. */
  std::size_t tick();
  /**
   * Explicit potentially-blocking Studio lane. Run from the engine's chosen
   * serialized worker/maintenance phase, never concurrently with controller
   * access. Drains scheduled host work, then runs autosave/monitor maintenance.
   */
  std::size_t runStudioMaintenance(std::size_t maxTasks = SIZE_MAX);
  std::size_t pendingStudioMaintenance() const;
  void setPageVisible(bool visible);
  void relayout();
  void dispose() noexcept;
  bool disposed() const noexcept { return disposed_; }

 private:
  CrowdyStudioIntegration(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime,
      CrowdyStudioAgentRuntimeFactory agentRuntimeFactory);

  std::optional<std::string> currentSessionId() const;
  std::optional<std::string> currentClientEpoch() const;
  std::string currentContextVersion() const;
  agent::NativeAgentModeV1 currentMode() const;
  bool isLeaseActive(
      std::string_view leaseId,
      player_host::LeaseKindV1 kind) const;
  std::vector<player_host::LeaseKindV1> activeLeaseKinds() const;
  std::optional<std::string> hostCapabilityRevision() const;
  std::optional<agent::AgentError> prepareForAgentWork(
      agent::AgentMode mode);
  void epochAttached(std::string_view epoch);
  void leaseChanged(const agent::AgentLease& lease);
  void synchronizePlayHeartbeat();
  void preempted(agent::AgentPreemptionReason reason);
  void stateChanged(const CrowdyStudioState& state);

  // Owners first: reverse member destruction tears down the dependency graph.
  std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider_;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntimeOwner_;
  std::shared_ptr<ICrowdyStudioRuntime> runtime_;
  std::shared_ptr<const core::ICrypto> crypto_;
  std::shared_ptr<ICrowdyStudioEditorAdapter> editorAdapter_;
  std::shared_ptr<ICrowdyStudioSynchronizationProvider> synchronization_;
  std::shared_ptr<ICrowdyStudioApprovalGate> approval_;
  std::shared_ptr<ICrowdyStudioWalletProvider> walletProvider_;
  std::shared_ptr<ICrowdyStudioLayoutStorage> layoutStorageOwner_;
  std::shared_ptr<std::atomic<bool>> callbackAlive_ =
      std::make_shared<std::atomic<bool>>(true);
  player_host::PlayerHostAdapterV1* playerHost_ = nullptr;
  std::unique_ptr<player_host::AgentControlLeaseManager>
      ownedLeaseManager_;
  player_host::AgentControlLeaseManager* leaseManager_ = nullptr;
  std::function<std::size_t()> platformPoll_;

  std::unique_ptr<CrowdyStudioController> controller_;
  std::unique_ptr<StudioLayoutController> layoutController_;
  std::unique_ptr<CrowdyStudioEditorBridge> editorBridge_;
  std::unique_ptr<CrowdyStudioControllerHostAdapter> studioHost_;
  std::unique_ptr<agent::NativeToolDispatcherV1> nativeDispatcher_;
  std::unique_ptr<agent::NativeBrowserToolDispatcherAdapter>
      browserDispatcher_;
  std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime> agentRuntime_;
  std::unique_ptr<player_host::NativePlayerControlGate> controlGate_;
  player_host::NativePlayerControlGate::Unbind controlGateUnbind_;

  mutable std::mutex maintenanceMutex_;
  std::deque<std::function<void()>> maintenanceTasks_;

  CrowdyStudioController::ListenerId stateSubscription_ = 0;
  CrowdyStudioController::ListenerId humanEditSubscription_ = 0;
  std::optional<std::string> selectedProjectId_;
  std::optional<std::string> lastAgentHeartbeatAt_;
  bool studioInitialized_ = false;
  bool agentInitialized_ = false;
  bool disposed_ = false;
};

}  // namespace crowdy::studio
