#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "crowdy/player_host/adapter.hpp"
#include "crowdy/studio/editor.hpp"
#include "crowdy/studio/layout.hpp"

namespace crowdy::studio {

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
   * The engine-owned player host, kept for observation only. The Studio agent
   * now runs in the player's browser (CrowdyJS `dsh`) and a native engine has
   * no agent pane, so nothing here dispatches commands to it; games that keep
   * an adapter for their own tooling may still hand it over, and it must
   * outlive the integration.
   */
  player_host::PlayerHostAdapterV1* playerHost = nullptr;

  std::function<std::size_t()> platformPoll;
  bool autoInitializeStudio = false;
};

/**
 * Destruction-safe native Studio assembly: the headless controller, the
 * layout controller and the optional editor bridge, torn down in dependency
 * order. Shared providers and runtimes are retained behind the controller.
 *
 * Since 0.34.0 this carries no agent: the Crowdy Agent orchestrator this SDK
 * drove over GraphQL was retired in favour of the in-browser DeepSeek Harness,
 * which has no native counterpart. Policy, usage and consent for that agent
 * remain reachable through `CrowdyClient::crowdyStudioAgent()`.
 */
class CrowdyStudioIntegration {
 public:
  static std::unique_ptr<CrowdyStudioIntegration> create(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime);

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
  StudioLayoutController& layout() { return *layoutController_; }
  const StudioLayoutController& layout() const {
    return *layoutController_;
  }
  StudioLayoutState layoutSnapshot() const {
    return layoutController_->getState();
  }
  player_host::PlayerHostAdapterV1* playerHost() { return playerHost_; }
  const player_host::PlayerHostAdapterV1* playerHost() const {
    return playerHost_;
  }
  ICrowdyStudioWalletProvider* walletProvider() {
    return walletProvider_.get();
  }
  const ICrowdyStudioWalletProvider* walletProvider() const {
    return walletProvider_.get();
  }

  void initializeStudio();
  void initialize();
  /** Nonblocking platform callback pump. */
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
  /** Queue work for the next runStudioMaintenance(); engine adapters use this. */
  void schedule(std::function<void()> task);
  void setPageVisible(bool visible);
  void relayout();
  void dispose() noexcept;
  bool disposed() const noexcept { return disposed_; }

 private:
  CrowdyStudioIntegration(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime);

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
  std::function<std::size_t()> platformPoll_;

  std::unique_ptr<CrowdyStudioController> controller_;
  std::unique_ptr<StudioLayoutController> layoutController_;
  std::unique_ptr<CrowdyStudioEditorBridge> editorBridge_;

  mutable std::mutex maintenanceMutex_;
  std::deque<std::function<void()>> maintenanceTasks_;

  bool studioInitialized_ = false;
  bool disposed_ = false;
};

}  // namespace crowdy::studio
