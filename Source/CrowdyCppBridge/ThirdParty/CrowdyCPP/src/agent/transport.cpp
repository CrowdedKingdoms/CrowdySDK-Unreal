#include "crowdy/agent/transport.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace crowdy::agent {

namespace {

AgentError graphFailure(const graphql::GraphQLOutcome& outcome) {
  if (!outcome.errors.empty()) {
    const auto& first = outcome.errors.front();
    const auto code =
        (first.code.starts_with("AGENT_") ||
         first.code == "CROWDY_STUDIO_REVISION_CONFLICT")
            ? first.code
            : "AGENT_DISCONNECTED";
    AgentError result = makeAgentError(
        code, first.message.empty() ? "Agent GraphQL operation failed"
                                    : first.message,
        code == "AGENT_DISCONNECTED");
    if (!first.remediation.empty()) result.remediation = first.remediation;
    return result;
  }
  return makeAgentError(
      "AGENT_DISCONNECTED",
      outcome.errorMessage.empty() ? "Agent GraphQL transport disconnected"
                                   : outcome.errorMessage,
      true);
}

AgentError subscriptionFailure(
    const graphql::GraphQLSubscriptionError& source) {
  std::string code = source.code;
  if (!code.starts_with("AGENT_") &&
      code != "CROWDY_STUDIO_REVISION_CONFLICT") {
    switch (source.kind) {
      case graphql::GraphQLSubscriptionErrorKind::Authentication:
        code = "AGENT_UNAUTHENTICATED";
        break;
      case graphql::GraphQLSubscriptionErrorKind::Authorization:
      case graphql::GraphQLSubscriptionErrorKind::AppScope:
        code = "AGENT_PERMISSION_DENIED";
        break;
      case graphql::GraphQLSubscriptionErrorKind::StaleClientEpoch:
        code = "AGENT_CLIENT_EPOCH_STALE";
        break;
      default:
        code = "AGENT_DISCONNECTED";
        break;
    }
  }
  AgentError result = makeAgentError(
      code,
      source.message.empty() ? "Agent GraphQL subscription failed"
                             : source.message,
      source.retryable);
  if (!source.errors.empty() &&
      !source.errors.front().remediation.empty()) {
    result.remediation = source.errors.front().remediation;
  }
  return result;
}

AgentError subscriptionFailure(
    const graphql::GraphQLSubscriptionOutcome& source) {
  if (!source.errors.empty()) {
    graphql::GraphQLSubscriptionError error;
    error.kind = graphql::GraphQLSubscriptionErrorKind::GraphQL;
    error.code = source.errors.front().code;
    error.message = source.errors.front().message;
    error.errors = source.errors;
    error.retryable = false;
    error.terminal = true;
    if (graphql::isTerminalSubscriptionErrorCode(error.code)) {
      if (error.code.find("EPOCH") != std::string::npos) {
        error.kind =
            graphql::GraphQLSubscriptionErrorKind::StaleClientEpoch;
      }
    }
    return subscriptionFailure(error);
  }
  return makeAgentError(
      "AGENT_DISCONNECTED",
      "Agent GraphQL subscription returned an unsuccessful payload", true);
}

class ClosedAgentEventSubscription final
    : public IAgentEventSubscription {
 public:
  void close() override {}
};

class GraphQLAgentEventSubscription final
    : public IAgentEventSubscription {
 public:
  explicit GraphQLAgentEventSubscription(
      graphql::SubscriptionHandle handle)
      : handle_(std::move(handle)) {}
  ~GraphQLAgentEventSubscription() override { close(); }

  void close() override { handle_.cancel(); }

 private:
  graphql::SubscriptionHandle handle_;
};

template <typename T, typename Parser>
void parseGraph(graphql::GraphQLOutcome outcome, AgentCallback<T> callback,
                Parser parser) {
  if (!outcome.ok()) {
    callback(AgentOutcome<T>::failure(graphFailure(outcome)));
    return;
  }
  try {
    callback(AgentOutcome<T>::success(parser(outcome.data)));
  } catch (const CrowdyAgentError& error) {
    callback(AgentOutcome<T>::failure(error.value()));
  } catch (const std::exception& error) {
    callback(AgentOutcome<T>::failure(toAgentError(
        error, "AGENT_EVENT_CURSOR_INVALID")));
  }
}

graphql::JVal mutationInput(const AgentMutationContext& context) {
  graphql::JVal input;
  input["sessionId"] = context.sessionId;
  input["clientEpoch"] = context.clientEpoch;
  input["idempotencyKey"] = context.idempotencyKey;
  return input;
}

AgentToolCallAck parseToolAck(const graphql::Json& value) {
  AgentToolCallAck result;
  result.toolCallId = value["toolCallId"].asString();
  result.toolName = value["toolName"].asString();
  const auto status =
      agentToolCallStatusFromString(value["status"].asStringView());
  if (!status || result.toolCallId.empty() || result.toolName.empty()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Tool-result acknowledgement is malformed");
  }
  result.status = *status;
  result.argumentHash = value["argumentHash"].asString();
  if (value["error"].ok() && !value["error"].isNull()) {
    result.error = parseAgentError(value["error"]);
  }
  result.accepted = value["accepted"].asBool(false);
  return result;
}

}  // namespace

std::unique_ptr<IAgentEventSubscription>
GraphQLAgentEventSubscriptionAdapter::subscribe(
    AgentEventSubscriptionRequest request,
    AgentEventSubscriptionCallbacks callbacks) {
  if (request.sessionId.empty() || request.sessionId.size() > 128 ||
      !isNonNegativeSequence(request.afterSeq) ||
      !isNonNegativeSequence(request.clientEpoch)) {
    if (callbacks.error) {
      callbacks.error(makeAgentError(
          "AGENT_EVENT_CURSOR_INVALID",
          "Agent subscription received an invalid session, cursor, or epoch"));
    }
    return std::make_unique<ClosedAgentEventSubscription>();
  }

  graphql::JVal variables;
  variables["sessionId"] = request.sessionId;
  variables["afterSeq"] = request.afterSeq;
  variables["clientEpoch"] = request.clientEpoch;
  auto sharedCallbacks =
      std::make_shared<AgentEventSubscriptionCallbacks>(
          std::move(callbacks));

  graphql::GraphQLSubscriptionCallbacks graphCallbacks;
  graphCallbacks.onNext =
      [sharedCallbacks](
          graphql::GraphQLSubscriptionOutcome outcome) mutable {
        if (!outcome.ok()) {
          if (sharedCallbacks->error) {
            sharedCallbacks->error(subscriptionFailure(outcome));
          }
          return;
        }
        const auto value = outcome.data["crowdyStudioAgentEvents"];
        if (!value.ok() || value.isNull()) {
          if (sharedCallbacks->error) {
            sharedCallbacks->error(makeAgentError(
                "AGENT_EVENT_CURSOR_INVALID",
                "Agent subscription payload omitted its durable event"));
          }
          return;
        }
        try {
          if (sharedCallbacks->next) {
            sharedCallbacks->next(parseAgentEvent(value));
          }
        } catch (const CrowdyAgentError& error) {
          if (sharedCallbacks->error) {
            sharedCallbacks->error(error.value());
          }
        } catch (const std::exception& error) {
          if (sharedCallbacks->error) {
            sharedCallbacks->error(toAgentError(
                error, "AGENT_EVENT_CURSOR_INVALID"));
          }
        }
      };
  graphCallbacks.onError =
      [sharedCallbacks](
          graphql::GraphQLSubscriptionError error) mutable {
        if (sharedCallbacks->error) {
          sharedCallbacks->error(subscriptionFailure(error));
        }
      };
  graphCallbacks.onComplete = [sharedCallbacks] {
    if (sharedCallbacks->complete) sharedCallbacks->complete();
  };
  graphCallbacks.onReconnect =
      [sharedCallbacks](graphql::GraphQLReconnectInfo) {
        if (sharedCallbacks->reconnect) sharedCallbacks->reconnect();
      };

  auto handle = subscriptions_.subscribe(
      gen::crowdyStudioAgent::documentFor("CrowdyStudioAgentEvents"), variables,
      "CrowdyStudioAgentEvents", std::move(graphCallbacks));
  return std::make_unique<GraphQLAgentEventSubscription>(
      std::move(handle));
}

void GraphQLAgentEventSubscriptionAdapter::poll() {
  subscriptions_.poll();
}

std::unique_ptr<IAgentEventSubscription>
CrowdyStudioAgentGraphQLTransport::subscribeEvents(
    AgentEventSubscriptionRequest request,
    AgentEventSubscriptionCallbacks callbacks) {
  if (!eventSubscriptions_) {
    if (callbacks.error) {
      callbacks.error(makeAgentError(
          "AGENT_HOST_UNAVAILABLE",
          "Agent event subscriptions require a GraphQLSubscriptionClient"));
    }
    return std::make_unique<ClosedAgentEventSubscription>();
  }
  return eventSubscriptions_->subscribe(std::move(request),
                                        std::move(callbacks));
}

void CrowdyStudioAgentGraphQLTransport::poll() {
  if (eventSubscriptions_) eventSubscriptions_->poll();
}

void CrowdyStudioAgentGraphQLTransport::getSession(
    std::string sessionId, AgentCallback<AgentSession> callback) {
  api_.sessionAsync(
      sessionId, [callback = std::move(callback)](
                     graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

void CrowdyStudioAgentGraphQLTransport::listSessions(
    std::string appId, std::optional<std::string> after, int first,
    AgentCallback<AgentConnection<AgentSession>> callback) {
  graphql::JVal page;
  if (after) page["after"] = *after;
  page["first"] = std::int64_t{first};
  api_.sessionsAsync(
      appId, page,
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentConnection<AgentSession>>(
            std::move(outcome), std::move(callback),
            parseAgentSessionConnection);
      });
}

void CrowdyStudioAgentGraphQLTransport::history(
    std::string sessionId, std::string afterSeq, int first,
    AgentCallback<AgentHistoryPage> callback) {
  api_.historyAsync(
      sessionId, afterSeq, first,
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentHistoryPage>(std::move(outcome),
                                     std::move(callback),
                                     parseAgentHistoryPage);
      });
}

void CrowdyStudioAgentGraphQLTransport::toolDescriptors(
    std::string sessionId, AgentCallback<AgentDescriptorSet> callback) {
  api_.toolDescriptorsAsync(
      sessionId, [callback = std::move(callback)](
                     graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentDescriptorSet>(
            std::move(outcome), std::move(callback),
            [](const graphql::Json& value) {
              auto registry = std::make_shared<const AgentToolRegistry>(
                  AgentToolRegistry::fromGraphQLDescriptorSet(value));
              return AgentDescriptorSet{registry->registryDigest(),
                                        std::move(registry)};
            });
      });
}

void CrowdyStudioAgentGraphQLTransport::budget(
    std::string sessionId, AgentCallback<AgentBudget> callback) {
  api_.budgetAsync(
      sessionId, [callback = std::move(callback)](
                     graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentBudget>(std::move(outcome), std::move(callback),
                                parseAgentBudget);
      });
}

void CrowdyStudioAgentGraphQLTransport::createSession(
    AgentCreateSessionInput request, AgentCallback<AgentSession> callback) {
  graphql::JVal input;
  input["appId"] = request.appId;
  if (request.projectId) input["projectId"] = *request.projectId;
  if (request.gridId) input["gridId"] = *request.gridId;
  input["mode"] = toString(request.mode);
  if (request.requestedModel) {
    input["requestedModel"] = *request.requestedModel;
  }
  input["providerDataConsent"] = request.providerDataConsent;
  input["idempotencyKey"] = request.idempotencyKey;
  api_.createSessionAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

void CrowdyStudioAgentGraphQLTransport::attachClient(
    std::string sessionId, std::optional<std::string> clientInstanceId,
    std::string idempotencyKey,
    AgentCallback<AgentClientAttachment> callback) {
  graphql::JVal input;
  input["sessionId"] = sessionId;
  if (clientInstanceId) input["clientInstanceId"] = *clientInstanceId;
  input["idempotencyKey"] = idempotencyKey;
  api_.attachClientAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentClientAttachment>(
            std::move(outcome), std::move(callback),
            [](const graphql::Json& value) {
              AgentClientAttachment result;
              result.session = parseAgentSession(value["session"]);
              result.clientEpoch = value["clientEpoch"].isString()
                                       ? value["clientEpoch"].asString()
                                       : value["clientEpoch"].dump();
              result.replayAfterSeq = value["replayAfterSeq"].isString()
                                          ? value["replayAfterSeq"].asString()
                                          : value["replayAfterSeq"].dump();
              if (!isNonNegativeSequence(result.clientEpoch) ||
                  !isNonNegativeSequence(result.replayAfterSeq)) {
                throw CrowdyAgentError(
                    "AGENT_EVENT_CURSOR_INVALID",
                    "Attach response has invalid epoch or replay cursor");
              }
              return result;
            });
      });
}

void CrowdyStudioAgentGraphQLTransport::setMode(
    AgentMutationContext context, AgentMode mode,
    AgentCallback<AgentSession> callback) {
  auto input = mutationInput(context);
  input["mode"] = toString(mode);
  api_.setModeAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

void CrowdyStudioAgentGraphQLTransport::acknowledgeEvents(
    AgentMutationContext context, std::string throughSeq,
    AgentCallback<std::string> callback) {
  auto input = mutationInput(context);
  input["throughSeq"] = throughSeq;
  api_.acknowledgeEventsAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<std::string>(
            std::move(outcome), std::move(callback),
            [](const graphql::Json& value) {
              const auto through = value["throughSeq"];
              const auto result = through.isString() ? through.asString()
                                                     : through.dump();
              if (!isNonNegativeSequence(result)) {
                throw CrowdyAgentError(
                    "AGENT_EVENT_CURSOR_INVALID",
                    "Acknowledged sequence is invalid");
              }
              return result;
            });
      });
}

void CrowdyStudioAgentGraphQLTransport::heartbeat(
    AgentMutationContext context, AgentCallback<AgentHeartbeat> callback) {
  api_.heartbeatAsync(
      mutationInput(context),
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentHeartbeat>(
            std::move(outcome), std::move(callback),
            [](const graphql::Json& value) {
              AgentHeartbeat result;
              result.serverTime = value["serverTime"].asString();
              if (value["playLeaseFreshUntil"].isString()) {
                result.playLeaseFreshUntil =
                    value["playLeaseFreshUntil"].asString();
              }
              if (value["workspaceLeaseExpiresAt"].isString()) {
                result.workspaceLeaseExpiresAt =
                    value["workspaceLeaseExpiresAt"].asString();
              }
              if (result.serverTime.empty()) {
                throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                                       "Heartbeat response is malformed");
              }
              return result;
            });
      });
}

void CrowdyStudioAgentGraphQLTransport::sendMessage(
    AgentMutationContext context, std::string message,
    AgentCallback<AgentRun> callback) {
  auto input = mutationInput(context);
  input["content"] = std::move(message);
  api_.sendMessageAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentRun>(std::move(outcome), std::move(callback),
                             parseAgentRun);
      });
}

void CrowdyStudioAgentGraphQLTransport::approveTool(
    AgentMutationContext context, std::string toolCallId,
    std::string argumentHash, AgentCallback<AgentApproval> callback) {
  auto input = mutationInput(context);
  input["toolCallId"] = std::move(toolCallId);
  input["argumentHash"] = std::move(argumentHash);
  api_.approveToolAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentApproval>(std::move(outcome), std::move(callback),
                                  [](const graphql::Json& value) {
                                    return parseAgentApproval(value);
                                  });
      });
}

void CrowdyStudioAgentGraphQLTransport::rejectTool(
    AgentMutationContext context, std::string toolCallId,
    std::string argumentHash, std::optional<std::string> reason,
    AgentCallback<AgentApproval> callback) {
  auto input = mutationInput(context);
  input["toolCallId"] = std::move(toolCallId);
  input["argumentHash"] = std::move(argumentHash);
  if (reason) input["reason"] = std::move(*reason);
  api_.rejectToolAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentApproval>(std::move(outcome), std::move(callback),
                                  [](const graphql::Json& value) {
                                    return parseAgentApproval(value);
                                  });
      });
}

void CrowdyStudioAgentGraphQLTransport::browserToolResult(
    AgentMutationContext context, AgentToolResult result,
    AgentCallback<AgentToolCallAck> callback) {
  auto input = mutationInput(context);
  graphql::JVal envelope;
  envelope["protocolVersion"] = result.protocolVersion;
  envelope["toolCallId"] = result.toolCallId;
  envelope["status"] = toString(result.status);
  if (result.outputJson) envelope["outputJson"] = *result.outputJson;
  if (result.error) {
    envelope["errorCode"] = result.error->code;
    envelope["errorMessage"] = result.error->message;
    envelope["errorRetryable"] = result.error->retryable;
  }
  envelope["observedContextVersion"] = result.observedContextVersion;
  envelope["startedAt"] = result.startedAt;
  envelope["finishedAt"] = result.finishedAt;
  input["result"] = std::move(envelope);
  api_.browserToolResultAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentToolCallAck>(std::move(outcome),
                                     std::move(callback), parseToolAck);
      });
}

void CrowdyStudioAgentGraphQLTransport::grantLease(
    AgentMutationContext context, std::vector<std::string> scopes,
    int durationSeconds, std::string controlledEntityId,
    std::string hostCapabilityRevision, AgentCallback<AgentLease> callback) {
  auto input = mutationInput(context);
  graphql::JArray scopeValues;
  scopeValues.reserve(scopes.size());
  for (auto& scope : scopes) scopeValues.emplace_back(std::move(scope));
  input["scopes"] = std::move(scopeValues);
  input["durationSeconds"] = std::int64_t{durationSeconds};
  input["controlledEntityId"] = std::move(controlledEntityId);
  input["hostCapabilityRevision"] = std::move(hostCapabilityRevision);
  api_.grantLeaseAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentLease>(std::move(outcome), std::move(callback),
                               parseAgentLease);
      });
}

void CrowdyStudioAgentGraphQLTransport::revokeLease(
    AgentMutationContext context, std::string leaseId,
    AgentPreemptionReason reason, AgentCallback<AgentLease> callback) {
  auto input = mutationInput(context);
  input["leaseId"] = std::move(leaseId);
  input["reason"] = toString(reason);
  api_.revokeLeaseAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentLease>(std::move(outcome), std::move(callback),
                               parseAgentLease);
      });
}

void CrowdyStudioAgentGraphQLTransport::pause(
    AgentMutationContext context, AgentCallback<AgentSession> callback) {
  api_.pauseAsync(
      mutationInput(context),
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

void CrowdyStudioAgentGraphQLTransport::resume(
    AgentMutationContext context, AgentCallback<AgentSession> callback) {
  api_.resumeAsync(
      mutationInput(context),
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

void CrowdyStudioAgentGraphQLTransport::cancelRun(
    AgentMutationContext context, std::string runId,
    AgentCallback<AgentRun> callback) {
  auto input = mutationInput(context);
  input["runId"] = std::move(runId);
  api_.cancelRunAsync(
      input, [callback = std::move(callback)](
                 graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentRun>(std::move(outcome), std::move(callback),
                             parseAgentRun);
      });
}

void CrowdyStudioAgentGraphQLTransport::closeSession(
    AgentMutationContext context, AgentCallback<AgentSession> callback) {
  api_.closeSessionAsync(
      mutationInput(context),
      [callback = std::move(callback)](
          graphql::GraphQLOutcome outcome) mutable {
        parseGraph<AgentSession>(std::move(outcome), std::move(callback),
                                 parseAgentSession);
      });
}

struct PollingAgentEventSubscriptionAdapter::Shared {
  struct Record {
    std::uint64_t id = 0;
    AgentEventSubscriptionRequest request;
    AgentEventSubscriptionCallbacks callbacks;
    bool inFlight = false;
    bool closed = false;
  };

  IAgentTransport* transport = nullptr;
  int pageSize = 100;
  std::uint64_t nextId = 1;
  bool alive = true;
  std::vector<std::shared_ptr<Record>> records;
};

namespace {

class PollingSubscription final : public IAgentEventSubscription {
 public:
  PollingSubscription(
      std::weak_ptr<PollingAgentEventSubscriptionAdapter::Shared> shared,
      std::weak_ptr<PollingAgentEventSubscriptionAdapter::Shared::Record>
          record)
      : shared_(std::move(shared)), record_(std::move(record)) {}
  ~PollingSubscription() override { close(); }

  void close() override {
    if (const auto record = record_.lock()) record->closed = true;
    if (const auto shared = shared_.lock()) {
      shared->records.erase(
          std::remove_if(
              shared->records.begin(), shared->records.end(),
              [](const auto& entry) { return entry->closed; }),
          shared->records.end());
    }
  }

 private:
  std::weak_ptr<PollingAgentEventSubscriptionAdapter::Shared> shared_;
  std::weak_ptr<PollingAgentEventSubscriptionAdapter::Shared::Record> record_;
};

bool sequenceGreater(std::string_view left, std::string_view right) {
  if (!isNonNegativeSequence(left) || !isNonNegativeSequence(right)) {
    return false;
  }
  return left.size() != right.size() ? left.size() > right.size()
                                     : left > right;
}

}  // namespace

PollingAgentEventSubscriptionAdapter::PollingAgentEventSubscriptionAdapter(
    IAgentTransport& transport, int pageSize)
    : shared_(std::make_shared<Shared>()) {
  shared_->transport = &transport;
  shared_->pageSize = std::clamp(pageSize, 1, 200);
}

PollingAgentEventSubscriptionAdapter::~PollingAgentEventSubscriptionAdapter() {
  shared_->alive = false;
  for (const auto& record : shared_->records) record->closed = true;
  shared_->records.clear();
}

std::unique_ptr<IAgentEventSubscription>
PollingAgentEventSubscriptionAdapter::subscribe(
    AgentEventSubscriptionRequest request,
    AgentEventSubscriptionCallbacks callbacks) {
  if (!isNonNegativeSequence(request.afterSeq) ||
      !isNonNegativeSequence(request.clientEpoch)) {
    callbacks.error(makeAgentError(
        "AGENT_EVENT_CURSOR_INVALID",
        "Polling subscription received an invalid cursor or epoch"));
    class ClosedSubscription final : public IAgentEventSubscription {
     public:
      void close() override {}
    };
    return std::make_unique<ClosedSubscription>();
  }
  auto record = std::make_shared<Shared::Record>();
  record->id = shared_->nextId++;
  record->request = std::move(request);
  record->callbacks = std::move(callbacks);
  shared_->records.push_back(record);
  return std::make_unique<PollingSubscription>(shared_, record);
}

void PollingAgentEventSubscriptionAdapter::poll() {
  if (!shared_->alive) return;
  const auto records = shared_->records;
  for (const auto& record : records) {
    if (record->closed || record->inFlight) continue;
    record->inFlight = true;
    std::weak_ptr<Shared> weakShared = shared_;
    std::weak_ptr<Shared::Record> weakRecord = record;
    shared_->transport->history(
        record->request.sessionId, record->request.afterSeq,
        shared_->pageSize,
        [weakShared, weakRecord](AgentOutcome<AgentHistoryPage> outcome) {
          const auto shared = weakShared.lock();
          const auto currentRecord = weakRecord.lock();
          if (!shared || !currentRecord || !shared->alive ||
              currentRecord->closed) {
            return;
          }
          currentRecord->inFlight = false;
          if (!outcome.ok()) {
            currentRecord->callbacks.error(
                outcome.error.value_or(makeAgentError(
                    "AGENT_DISCONNECTED", "Polling replay failed", true)));
            return;
          }
          for (auto& event : outcome.value->events) {
            if (!sequenceGreater(event.seq,
                                 currentRecord->request.afterSeq)) {
              continue;
            }
            currentRecord->request.afterSeq = event.seq;
            currentRecord->callbacks.next(std::move(event));
            if (currentRecord->closed) break;
          }
        });
  }
  shared_->records.erase(
      std::remove_if(shared_->records.begin(), shared_->records.end(),
                     [](const auto& entry) { return entry->closed; }),
      shared_->records.end());
}

}  // namespace crowdy::agent
