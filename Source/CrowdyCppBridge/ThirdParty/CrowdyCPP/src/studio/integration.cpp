#include "crowdy/studio/integration.hpp"

#include <stdexcept>
#include <utility>

namespace crowdy::studio {

std::unique_ptr<CrowdyStudioIntegration>
CrowdyStudioIntegration::create(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime) {
  return std::unique_ptr<CrowdyStudioIntegration>(
      new CrowdyStudioIntegration(
          std::move(options), std::move(projectProvider),
          std::move(runtime)));
}

CrowdyStudioIntegration::CrowdyStudioIntegration(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime)
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
      platformPoll_(std::move(options.platformPoll)) {
  if (!projectProvider_ || !runtime_ || !crypto_) {
    throw std::invalid_argument(
        "Crowdy Studio integration requires owned project/runtime/crypto "
        "services");
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

  if (editorAdapter_) {
    editorBridge_ = std::make_unique<CrowdyStudioEditorBridge>(
        *controller_, editorAdapter_);
  }

  if (options.autoInitializeStudio) {
    controller_->initialize();
    studioInitialized_ = true;
  }
}

CrowdyStudioIntegration::~CrowdyStudioIntegration() { dispose(); }

void CrowdyStudioIntegration::initialize() { initializeStudio(); }

void CrowdyStudioIntegration::initializeStudio() {
  if (disposed_) {
    throw std::runtime_error("CrowdyStudioIntegration is disposed");
  }
  if (!studioInitialized_) {
    controller_->initialize();
    studioInitialized_ = true;
  }
}

std::size_t CrowdyStudioIntegration::poll() {
  if (disposed_) return 0;
  return platformPoll_ ? platformPoll_() : 0;
}

std::size_t CrowdyStudioIntegration::tick() { return poll(); }

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

void CrowdyStudioIntegration::schedule(std::function<void()> task) {
  if (disposed_ || !task) return;
  std::lock_guard lock(maintenanceMutex_);
  maintenanceTasks_.push_back(std::move(task));
}

void CrowdyStudioIntegration::setPageVisible(bool visible) {
  if (disposed_) return;
  controller_->setPageVisible(visible);
}

void CrowdyStudioIntegration::relayout() {
  if (disposed_) return;
  if (editorBridge_) editorBridge_->relayout();
}

void CrowdyStudioIntegration::dispose() noexcept {
  if (disposed_) return;
  disposed_ = true;
  callbackAlive_->store(false, std::memory_order_release);
  {
    std::lock_guard lock(maintenanceMutex_);
    maintenanceTasks_.clear();
  }
  if (editorBridge_) {
    editorBridge_->dispose();
    editorBridge_.reset();
  }
  if (controller_) {
    controller_->destroy();
    controller_.reset();
  }
  layoutController_.reset();
}

}  // namespace crowdy::studio
