#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/agent/transport.hpp"
#include "crowdy/core/clock.hpp"
#include "crowdy/graphql/dispatcher.hpp"

namespace crowdy::agent {

enum class AgentConnectionState {
  Disconnected,
  Attaching,
  Replaying,
  Connected,
  Reconnecting,
  Error
};

struct CrowdyStudioAgentState {
  AgentConnectionState connection = AgentConnectionState::Disconnected;
  std::optional<AgentSession> session;
  std::optional<std::string> clientEpoch;
  std::string lastContiguousSeq = "0";
  std::string lastAcknowledgedSeq = "0";
  std::vector<AgentEvent> events;
  std::vector<AgentMessage> messages;
  std::string streamingText;
  std::vector<AgentToolTimelineItem> tools;
  std::vector<AgentApproval> approvals;
  std::vector<AgentLease> leases;
  std::vector<AgentCheckpoint> checkpoints;
  std::optional<AgentBudget> budget;
  std::shared_ptr<const AgentToolRegistry> toolRegistry;
  std::optional<std::string> lastHeartbeatAt;
  std::optional<std::string> playLeaseFreshUntil;
  std::optional<AgentError> lastError;
  bool reconnectRequired = false;
};

class IAgentBrowserToolDispatcher {
 public:
  virtual ~IAgentBrowserToolDispatcher() = default;
  virtual void dispatch(AgentToolInvocation invocation,
                        AgentCallback<AgentToolResult> callback) = 0;
  virtual void cancelActive(AgentPreemptionReason reason) = 0;
  virtual void clearClosedSession() = 0;
  /// Game-thread lifecycle pump for native deadline enforcement.
  virtual void tick() {}
};

struct CrowdyStudioAgentControllerOptions {
  IAgentTransport* transport = nullptr;
  IAgentEventSubscriptionAdapter* subscriptionAdapter = nullptr;
  std::shared_ptr<graphql::Dispatcher> dispatcher;
  const core::IClock* clock = nullptr;

  std::optional<std::string> sessionId;
  std::optional<AgentCreateSessionInput> createSession;
  std::string clientInstanceId;
  IAgentBrowserToolDispatcher* browserDispatcher = nullptr;

  std::function<void(AgentPreemptionReason)> onPreempt;
  std::function<void(std::string_view)> onEpochAttached;
  std::function<void(const AgentLease&)> onLeaseChanged;
  std::function<void(const CrowdyStudioAgentState&)> onStateChange;
  std::function<std::optional<AgentError>(AgentMode)> beforeAgentWork;
  std::function<std::string(std::string_view)> createIdempotencyKey;

  int historyPageSize = 100;
  std::size_t maxRetainedEvents = 1'000;
  bool autoReconnect = false;
  int reconnectDelayMs = 500;
  int maxReconnectAttempts = 5;
  int heartbeatIntervalMs = 2'000;
  int heartbeatStaleMs = 5'000;
  int workspaceRenewIntervalMs = 10'000;
};

/// Native deterministic client for `crowdy.studio-agent/1`.
///
/// No internal thread invokes user code. Transport, subscription, and tool
/// callbacks are posted through Dispatcher and applied by poll().
class CrowdyStudioAgentController {
 public:
  explicit CrowdyStudioAgentController(
      CrowdyStudioAgentControllerOptions options);
  ~CrowdyStudioAgentController();

  CrowdyStudioAgentController(const CrowdyStudioAgentController&) = delete;
  CrowdyStudioAgentController& operator=(
      const CrowdyStudioAgentController&) = delete;

  const CrowdyStudioAgentState& state() const { return state_; }
  const CrowdyStudioAgentState& getState() const { return state_; }
  std::function<void()> subscribe(
      std::function<void(const CrowdyStudioAgentState&)> listener);
  std::size_t poll();

  void initialize(AgentVoidCallback callback = {});
  void reconnect(AgentVoidCallback callback = {});
  void sendMessage(std::string content, AgentCallback<AgentRun> callback = {});
  void setMode(AgentMode mode, AgentVoidCallback callback = {});
  void approveTool(std::string toolCallId,
                   std::optional<std::string> expectedArgumentHash = std::nullopt,
                   AgentVoidCallback callback = {});
  void rejectTool(std::string toolCallId,
                  std::optional<std::string> reason = std::nullopt,
                  AgentVoidCallback callback = {});
  void grantPlayLease(std::vector<std::string> scopes, int durationSeconds,
                      std::string controlledEntityId,
                      std::string hostCapabilityRevision,
                      AgentCallback<AgentLease> callback = {});
  void revokeLease(std::string leaseId,
                   AgentPreemptionReason reason =
                       AgentPreemptionReason::HumanStop,
                   AgentVoidCallback callback = {});
  void pause(AgentVoidCallback callback = {});
  void resume(AgentVoidCallback callback = {});
  void cancelRun(std::optional<std::string> runId = std::nullopt,
                 AgentVoidCallback callback = {});
  void stop(AgentVoidCallback callback = {});
  void restoreCheckpoint(std::string checkpointId,
                         AgentVoidCallback callback = {});
  void close(AgentVoidCallback callback = {});

  void setPageVisible(bool visible);
  void projectSelectionChanged(std::optional<std::string> projectId);
  void preemptForHumanEdit();
  void destroy();

 private:
  struct Lifetime {
    std::atomic<bool> alive{true};
    CrowdyStudioAgentController* owner = nullptr;
  };
  struct DecimalLess {
    bool operator()(const std::string& left,
                    const std::string& right) const;
  };

  template <typename Fn>
  auto deliver(Fn fn) {
    const std::weak_ptr<Lifetime> lifetime = lifetime_;
    const auto dispatcher = dispatcher_;
    return [lifetime, dispatcher, fn = std::move(fn)](auto outcome) mutable {
      dispatcher->post(
          [lifetime, fn = std::move(fn),
           outcome = std::move(outcome)]() mutable {
            const auto locked = lifetime.lock();
            if (!locked || !locked->alive.load() || !locked->owner) return;
            fn(*locked->owner, std::move(outcome));
          });
    };
  }

  void beginAttach(AgentConnectionState connection,
                   AgentVoidCallback callback);
  void loadRemoteContext(std::uint64_t generation,
                         AgentVoidCallback callback);
  void replayHistory(std::uint64_t generation,
                     AgentVoidCallback callback);
  void openSubscription(std::uint64_t generation,
                        AgentVoidCallback callback);
  void enqueueEvent(AgentEvent event, std::uint64_t generation);
  void bufferEvent(AgentEvent event);
  void drainBuffered(std::uint64_t generation);
  void recoverGap(std::uint64_t generation, bool requireAdvance = true);
  void applyEvent(AgentEvent event, std::uint64_t generation);
  void dispatchBrowserTool(AgentToolInvocation invocation,
                           std::uint64_t connectionGeneration,
                           std::uint64_t effectGeneration);
  void scheduleAcknowledge(std::uint64_t generation);
  void sendAcknowledgement(std::uint64_t generation);
  void handleDisconnect(AgentError error, std::uint64_t generation);
  void fail(AgentError error);
  void checkTimers();
  void sendHeartbeat(std::uint64_t generation, bool workspace);
  void refreshTimerDeadlines();
  void preemptLocal(AgentPreemptionReason reason);
  void update();

  AgentSession& requireSession();
  AgentMutationContext mutationContext(std::string_view operation);
  std::string nextKey(std::string_view operation);
  static AgentOutcome<AgentVoid> voidSuccess();
  static void finish(const AgentVoidCallback& callback,
                     AgentOutcome<AgentVoid> outcome);

  CrowdyStudioAgentControllerOptions options_;
  IAgentTransport& transport_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
  const core::IClock& clock_;
  std::unique_ptr<PollingAgentEventSubscriptionAdapter> pollingAdapter_;
  IAgentEventSubscriptionAdapter* subscriptionAdapter_ = nullptr;
  std::unique_ptr<IAgentEventSubscription> subscription_;
  CrowdyStudioAgentState state_;
  std::shared_ptr<Lifetime> lifetime_;

  std::map<std::string, AgentEvent, DecimalLess> buffered_;
  std::unordered_map<std::string, std::string> appliedEventIds_;
  std::uint64_t generation_ = 0;
  std::uint64_t effectGeneration_ = 0;
  std::uint64_t keySequence_ = 0;
  std::uint64_t listenerSequence_ = 0;
  std::unordered_map<
      std::uint64_t,
      std::function<void(const CrowdyStudioAgentState&)>>
      listeners_;
  bool destroyed_ = false;
  bool pageVisible_ = true;
  bool historyInFlight_ = false;
  bool gapRecoveryInFlight_ = false;
  std::optional<std::uint64_t> acknowledgeGeneration_;
  bool heartbeatInFlight_ = false;
  bool workspaceHeartbeatInFlight_ = false;
  std::string pendingAcknowledge_ = "0";
  std::int64_t heartbeatStartedAt_ = 0;
  std::int64_t workspaceHeartbeatStartedAt_ = 0;
  std::int64_t nextHeartbeatAt_ = 0;
  std::int64_t nextWorkspaceRenewAt_ = 0;
  std::int64_t reconnectAt_ = 0;
  int reconnectAttempts_ = 0;
};

using CrowdyStudioAgentClient = CrowdyStudioAgentController;

}  // namespace crowdy::agent
