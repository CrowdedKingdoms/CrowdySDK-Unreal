#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/agent/registry.hpp"
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/graphql/subscription_client.hpp"

namespace crowdy::agent {

template <typename T>
struct AgentOutcome {
  std::optional<T> value;
  std::optional<AgentError> error;
  bool ok() const { return value.has_value() && !error.has_value(); }

  static AgentOutcome success(T result) {
    AgentOutcome outcome;
    outcome.value = std::move(result);
    return outcome;
  }
  static AgentOutcome failure(AgentError failure) {
    AgentOutcome outcome;
    outcome.error = std::move(failure);
    return outcome;
  }
};

using AgentVoid = std::monostate;
template <typename T>
using AgentCallback = std::function<void(AgentOutcome<T>)>;
using AgentVoidCallback = AgentCallback<AgentVoid>;

struct AgentCreateSessionInput {
  std::string appId;
  std::optional<std::string> projectId;
  std::optional<std::string> gridId;
  AgentMode mode = AgentMode::Ask;
  std::optional<std::string> requestedModel;
  bool providerDataConsent = false;
  std::string idempotencyKey;
};

struct AgentMutationContext {
  std::string sessionId;
  std::string clientEpoch;
  std::string idempotencyKey;
};

struct AgentDescriptorSet {
  std::string registryDigest;
  std::shared_ptr<const AgentToolRegistry> registry;
};

class IAgentTransport {
 public:
  virtual ~IAgentTransport() = default;

  virtual void getSession(std::string sessionId,
                          AgentCallback<AgentSession> callback) = 0;
  virtual void listSessions(std::string appId, std::optional<std::string> after,
                            int first,
                            AgentCallback<AgentConnection<AgentSession>>
                                callback) = 0;
  virtual void history(std::string sessionId, std::string afterSeq, int first,
                       AgentCallback<AgentHistoryPage> callback) = 0;
  virtual void toolDescriptors(std::string sessionId,
                               AgentCallback<AgentDescriptorSet> callback) = 0;
  virtual void budget(std::string sessionId,
                      AgentCallback<AgentBudget> callback) = 0;

  virtual void createSession(AgentCreateSessionInput input,
                             AgentCallback<AgentSession> callback) = 0;
  virtual void attachClient(std::string sessionId,
                            std::optional<std::string> clientInstanceId,
                            std::string idempotencyKey,
                            AgentCallback<AgentClientAttachment> callback) = 0;
  virtual void setMode(AgentMutationContext context, AgentMode mode,
                       AgentCallback<AgentSession> callback) = 0;
  virtual void acknowledgeEvents(AgentMutationContext context,
                                 std::string throughSeq,
                                 AgentCallback<std::string> callback) = 0;
  virtual void heartbeat(AgentMutationContext context,
                         AgentCallback<AgentHeartbeat> callback) = 0;
  virtual void sendMessage(AgentMutationContext context, std::string message,
                           AgentCallback<AgentRun> callback) = 0;
  virtual void approveTool(AgentMutationContext context,
                           std::string toolCallId, std::string argumentHash,
                           AgentCallback<AgentApproval> callback) = 0;
  virtual void rejectTool(AgentMutationContext context,
                          std::string toolCallId, std::string argumentHash,
                          std::optional<std::string> reason,
                          AgentCallback<AgentApproval> callback) = 0;
  virtual void browserToolResult(AgentMutationContext context,
                                 AgentToolResult result,
                                 AgentCallback<AgentToolCallAck> callback) = 0;
  virtual void grantLease(AgentMutationContext context,
                          std::vector<std::string> scopes,
                          int durationSeconds,
                          std::string controlledEntityId,
                          std::string hostCapabilityRevision,
                          AgentCallback<AgentLease> callback) = 0;
  virtual void revokeLease(AgentMutationContext context, std::string leaseId,
                           AgentPreemptionReason reason,
                           AgentCallback<AgentLease> callback) = 0;
  virtual void pause(AgentMutationContext context,
                     AgentCallback<AgentSession> callback) = 0;
  virtual void resume(AgentMutationContext context,
                      AgentCallback<AgentSession> callback) = 0;
  virtual void cancelRun(AgentMutationContext context, std::string runId,
                         AgentCallback<AgentRun> callback) = 0;
  virtual void closeSession(AgentMutationContext context,
                            AgentCallback<AgentSession> callback) = 0;
};

struct AgentEventSubscriptionRequest {
  std::string sessionId;
  std::string afterSeq;
  std::string clientEpoch;
};

struct AgentEventSubscriptionCallbacks {
  std::function<void(AgentEvent)> next;
  std::function<void(AgentError)> error;
  std::function<void()> complete;
  /// A graphql-transport-ws connection was replaced and the durable stream is
  /// about to be replayed. Controllers use this to start an explicit history
  /// gap-fill before replayed push events arrive.
  std::function<void()> reconnect;
};

class IAgentEventSubscription {
 public:
  virtual ~IAgentEventSubscription() = default;
  virtual void close() = 0;
};

/// GraphQL-WS and other realtime branches implement this exact adapter. It
/// carries only typed durable agent events; it cannot execute arbitrary
/// GraphQL, UDP, provider, or tool requests.
class IAgentEventSubscriptionAdapter {
 public:
  virtual ~IAgentEventSubscriptionAdapter() = default;
  virtual std::unique_ptr<IAgentEventSubscription> subscribe(
      AgentEventSubscriptionRequest request,
      AgentEventSubscriptionCallbacks callbacks) = 0;
  virtual void poll() {}
  virtual bool available() const { return true; }
};

/// Typed `crowdyStudioAgentEvents` adapter over the portable
/// GraphQLSubscriptionClient. The underlying subscription handle provides
/// execute-once RAII cancellation and Dispatcher-thread delivery.
class GraphQLAgentEventSubscriptionAdapter final
    : public IAgentEventSubscriptionAdapter {
 public:
  explicit GraphQLAgentEventSubscriptionAdapter(
      graphql::GraphQLSubscriptionClient& subscriptions)
      : subscriptions_(subscriptions) {}

  std::unique_ptr<IAgentEventSubscription> subscribe(
      AgentEventSubscriptionRequest request,
      AgentEventSubscriptionCallbacks callbacks) override;
  void poll() override;

 private:
  graphql::GraphQLSubscriptionClient& subscriptions_;
};

/// Deterministic no-thread replay fallback. poll() requests durable history
/// after the last delivered sequence and emits it in order.
class PollingAgentEventSubscriptionAdapter final
    : public IAgentEventSubscriptionAdapter {
 public:
  struct Shared;

  explicit PollingAgentEventSubscriptionAdapter(IAgentTransport& transport,
                                                int pageSize = 100);
  ~PollingAgentEventSubscriptionAdapter() override;

  std::unique_ptr<IAgentEventSubscription> subscribe(
      AgentEventSubscriptionRequest request,
      AgentEventSubscriptionCallbacks callbacks) override;
  void poll() override;

 private:
  std::shared_ptr<Shared> shared_;
};

/// Typed generated-document adapter from CrowdyStudioAgentAPI to the
/// controller transport. All async completions preserve the API's
/// Dispatcher/poll delivery.
class CrowdyStudioAgentGraphQLTransport final
    : public IAgentTransport,
      public IAgentEventSubscriptionAdapter {
 public:
  explicit CrowdyStudioAgentGraphQLTransport(
      domains::CrowdyStudioAgentAPI& api)
      : api_(api) {}
  CrowdyStudioAgentGraphQLTransport(
      domains::CrowdyStudioAgentAPI& api,
      graphql::GraphQLSubscriptionClient& subscriptions)
      : api_(api),
        eventSubscriptions_(
            std::make_unique<GraphQLAgentEventSubscriptionAdapter>(
                subscriptions)) {}

  /// Compatibility lifecycle hook. HTTP calls are request-scoped and each
  /// realtime subscription is owned by its returned RAII handle.
  void close() {}

  /// Exact CrowdyJS transport spelling. This uses only the committed
  /// CrowdyStudioAgentEvents document and cannot execute arbitrary GraphQL.
  std::unique_ptr<IAgentEventSubscription> subscribeEvents(
      AgentEventSubscriptionRequest request,
      AgentEventSubscriptionCallbacks callbacks);
  std::unique_ptr<IAgentEventSubscription> subscribe(
      AgentEventSubscriptionRequest request,
      AgentEventSubscriptionCallbacks callbacks) override {
    return subscribeEvents(std::move(request), std::move(callbacks));
  }
  void poll() override;
  bool available() const override {
    return static_cast<bool>(eventSubscriptions_);
  }

  void getSession(std::string sessionId,
                  AgentCallback<AgentSession> callback) override;
  void listSessions(
      std::string appId, std::optional<std::string> after, int first,
      AgentCallback<AgentConnection<AgentSession>> callback) override;
  void history(std::string sessionId, std::string afterSeq, int first,
               AgentCallback<AgentHistoryPage> callback) override;
  void toolDescriptors(std::string sessionId,
                       AgentCallback<AgentDescriptorSet> callback) override;
  void budget(std::string sessionId,
              AgentCallback<AgentBudget> callback) override;
  void createSession(AgentCreateSessionInput input,
                     AgentCallback<AgentSession> callback) override;
  void attachClient(std::string sessionId,
                    std::optional<std::string> clientInstanceId,
                    std::string idempotencyKey,
                    AgentCallback<AgentClientAttachment> callback) override;
  void setMode(AgentMutationContext context, AgentMode mode,
               AgentCallback<AgentSession> callback) override;
  void acknowledgeEvents(AgentMutationContext context, std::string throughSeq,
                         AgentCallback<std::string> callback) override;
  void heartbeat(AgentMutationContext context,
                 AgentCallback<AgentHeartbeat> callback) override;
  void sendMessage(AgentMutationContext context, std::string message,
                   AgentCallback<AgentRun> callback) override;
  void approveTool(AgentMutationContext context, std::string toolCallId,
                   std::string argumentHash,
                   AgentCallback<AgentApproval> callback) override;
  void rejectTool(AgentMutationContext context, std::string toolCallId,
                  std::string argumentHash,
                  std::optional<std::string> reason,
                  AgentCallback<AgentApproval> callback) override;
  void browserToolResult(AgentMutationContext context, AgentToolResult result,
                         AgentCallback<AgentToolCallAck> callback) override;
  void grantLease(AgentMutationContext context,
                  std::vector<std::string> scopes, int durationSeconds,
                  std::string controlledEntityId,
                  std::string hostCapabilityRevision,
                  AgentCallback<AgentLease> callback) override;
  void revokeLease(AgentMutationContext context, std::string leaseId,
                   AgentPreemptionReason reason,
                   AgentCallback<AgentLease> callback) override;
  void pause(AgentMutationContext context,
             AgentCallback<AgentSession> callback) override;
  void resume(AgentMutationContext context,
              AgentCallback<AgentSession> callback) override;
  void cancelRun(AgentMutationContext context, std::string runId,
                 AgentCallback<AgentRun> callback) override;
  void closeSession(AgentMutationContext context,
                    AgentCallback<AgentSession> callback) override;

 private:
  domains::CrowdyStudioAgentAPI& api_;
  std::unique_ptr<GraphQLAgentEventSubscriptionAdapter> eventSubscriptions_;
};

using CrowdyAgentGraphQLTransport =
    CrowdyStudioAgentGraphQLTransport;

}  // namespace crowdy::agent
