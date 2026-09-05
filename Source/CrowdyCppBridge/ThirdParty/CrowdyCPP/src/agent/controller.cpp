#include "crowdy/agent/controller.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace crowdy::agent {

namespace {

bool decimalGreater(std::string_view left, std::string_view right) {
  if (!isNonNegativeSequence(left) || !isNonNegativeSequence(right)) {
    return false;
  }
  return left.size() != right.size() ? left.size() > right.size()
                                     : left > right;
}

std::string incrementDecimal(std::string value) {
  if (!isNonNegativeSequence(value)) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Cannot increment an invalid event sequence");
  }
  for (auto iterator = value.rbegin(); iterator != value.rend(); ++iterator) {
    if (*iterator != '9') {
      ++*iterator;
      return value;
    }
    *iterator = '0';
  }
  value.insert(value.begin(), '1');
  if (value.size() > 40) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Event sequence exceeded protocol bounds");
  }
  return value;
}

template <typename T, typename Key>
void upsert(std::vector<T>& values, T value, Key key) {
  const auto wanted = key(value);
  values.erase(std::remove_if(values.begin(), values.end(),
                              [&](const T& entry) {
                                return key(entry) == wanted;
                              }),
               values.end());
  values.push_back(std::move(value));
}

bool terminalPolicyError(std::string_view code) {
  return code == "AGENT_DISABLED" || code == "AGENT_OPERATOR_KILLED" ||
         code == "AGENT_SESSION_CLOSED" ||
         code == "AGENT_PERMISSION_DENIED";
}

IAgentTransport& requireTransport(IAgentTransport* transport) {
  if (!transport) {
    throw CrowdyAgentError("AGENT_HOST_UNAVAILABLE",
                           "Agent controller requires a typed transport");
  }
  return *transport;
}

}  // namespace

bool CrowdyStudioAgentController::DecimalLess::operator()(
    const std::string& left, const std::string& right) const {
  if (left.size() != right.size()) return left.size() < right.size();
  return left < right;
}

CrowdyStudioAgentController::CrowdyStudioAgentController(
    CrowdyStudioAgentControllerOptions options)
    : options_(std::move(options)),
      transport_(requireTransport(options_.transport)),
      dispatcher_(options_.dispatcher
                      ? options_.dispatcher
                      : std::make_shared<graphql::Dispatcher>()),
      clock_(options_.clock ? *options_.clock : core::systemClock()),
      lifetime_(std::make_shared<Lifetime>()) {
  if (!options_.sessionId && !options_.createSession) {
    throw CrowdyAgentError(
        "AGENT_SESSION_NOT_FOUND",
        "Provide sessionId or createSession to initialize the agent client");
  }
  options_.historyPageSize = std::clamp(options_.historyPageSize, 1, 200);
  options_.maxRetainedEvents =
      std::clamp<std::size_t>(options_.maxRetainedEvents, 1, 10'000);
  lifetime_->owner = this;
  if (options_.subscriptionAdapter &&
      options_.subscriptionAdapter->available()) {
    subscriptionAdapter_ = options_.subscriptionAdapter;
  } else if (auto* transportAdapter =
                 dynamic_cast<IAgentEventSubscriptionAdapter*>(
                     options_.transport);
             transportAdapter && transportAdapter->available()) {
    subscriptionAdapter_ = transportAdapter;
  } else {
    pollingAdapter_ = std::make_unique<PollingAgentEventSubscriptionAdapter>(
        transport_, options_.historyPageSize);
    subscriptionAdapter_ = pollingAdapter_.get();
  }
}

CrowdyStudioAgentController::~CrowdyStudioAgentController() { destroy(); }

std::function<void()> CrowdyStudioAgentController::subscribe(
    std::function<void(const CrowdyStudioAgentState&)> listener) {
  const auto id = ++listenerSequence_;
  listener(state_);
  listeners_.emplace(id, std::move(listener));
  const std::weak_ptr<Lifetime> lifetime = lifetime_;
  return [lifetime, id] {
    const auto locked = lifetime.lock();
    if (!locked || !locked->alive.load() || !locked->owner) return;
    locked->owner->listeners_.erase(id);
  };
}

std::size_t CrowdyStudioAgentController::poll() {
  if (destroyed_) return 0;
  subscriptionAdapter_->poll();
  if (options_.browserDispatcher) options_.browserDispatcher->tick();
  const auto first = dispatcher_->drain();
  checkTimers();
  const auto second = dispatcher_->drain();
  return first + second;
}

void CrowdyStudioAgentController::initialize(AgentVoidCallback callback) {
  if (destroyed_) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_SESSION_CLOSED",
                         "Agent controller is destroyed")));
    return;
  }
  if (options_.sessionId) {
    transport_.getSession(
        *options_.sessionId,
        deliver([callback = std::move(callback)](
                    CrowdyStudioAgentController& self,
                    AgentOutcome<AgentSession> outcome) mutable {
          if (!outcome.ok()) {
            self.fail(*outcome.error);
            finish(callback,
                   AgentOutcome<AgentVoid>::failure(*outcome.error));
            return;
          }
          self.state_.session = std::move(*outcome.value);
          self.update();
          self.beginAttach(AgentConnectionState::Attaching,
                           std::move(callback));
        }));
    return;
  }
  auto request = *options_.createSession;
  if (request.mode == AgentMode::Build && !request.projectId) {
    const auto error = makeAgentError(
        "AGENT_CONTEXT_CHANGED",
        "BUILD requires a saved Crowdy Studio project binding");
    fail(error);
    finish(callback, AgentOutcome<AgentVoid>::failure(error));
    return;
  }
  transport_.createSession(
      std::move(request),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        if (!outcome.ok()) {
          self.fail(*outcome.error);
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
          return;
        }
        self.state_.session = std::move(*outcome.value);
        self.update();
        self.beginAttach(AgentConnectionState::Attaching,
                         std::move(callback));
      }));
}

void CrowdyStudioAgentController::reconnect(AgentVoidCallback callback) {
  if (!state_.session) {
    initialize(std::move(callback));
    return;
  }
  beginAttach(AgentConnectionState::Reconnecting, std::move(callback));
}

void CrowdyStudioAgentController::beginAttach(
    AgentConnectionState connection, AgentVoidCallback callback) {
  auto& session = requireSession();
  const auto oldEpoch = state_.clientEpoch;
  const auto generation = ++generation_;
  ++effectGeneration_;
  if (subscription_) subscription_->close();
  subscription_.reset();
  heartbeatInFlight_ = false;
  workspaceHeartbeatInFlight_ = false;
  historyInFlight_ = false;
  gapRecoveryInFlight_ = false;
  acknowledgeGeneration_.reset();
  if (connection == AgentConnectionState::Reconnecting) {
    preemptLocal(AgentPreemptionReason::Disconnected);
  }
  state_.connection = connection;
  state_.lastError.reset();
  state_.reconnectRequired =
      connection == AgentConnectionState::Reconnecting;
  update();
  transport_.attachClient(
      session.sessionId,
      options_.clientInstanceId.empty()
          ? std::nullopt
          : std::optional<std::string>(options_.clientInstanceId),
      nextKey("attach-client"),
      deliver([generation, oldEpoch, callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentClientAttachment> outcome) mutable {
        if (generation != self.generation_) return;
        if (!outcome.ok()) {
          self.fail(*outcome.error);
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
          return;
        }
        auto attached = std::move(*outcome.value);
        if (attached.session.sessionId !=
                self.requireSession().sessionId ||
            (attached.session.clientEpoch &&
             *attached.session.clientEpoch != attached.clientEpoch) ||
            decimalGreater(attached.replayAfterSeq,
                           attached.session.lastEventSeq) ||
            (oldEpoch &&
             !decimalGreater(attached.clientEpoch, *oldEpoch))) {
          const auto error = makeAgentError(
              "AGENT_CLIENT_EPOCH_STALE",
              "Attach response session, epoch, or replay cursor is inconsistent");
          self.fail(error);
          finish(callback, AgentOutcome<AgentVoid>::failure(error));
          return;
        }
        if (self.options_.onEpochAttached) {
          self.options_.onEpochAttached(attached.clientEpoch);
        }
        self.buffered_.clear();
        const auto replayAfter =
            decimalGreater(attached.replayAfterSeq,
                           self.state_.lastContiguousSeq)
                ? attached.replayAfterSeq
                : self.state_.lastContiguousSeq;
        self.state_.connection = AgentConnectionState::Replaying;
        self.state_.session = std::move(attached.session);
        self.state_.clientEpoch = std::move(attached.clientEpoch);
        self.state_.lastContiguousSeq = replayAfter;
        if (decimalGreater(replayAfter,
                           self.state_.lastAcknowledgedSeq)) {
          self.state_.lastAcknowledgedSeq = replayAfter;
        }
        self.state_.leases = self.state_.session->activeLeases;
        self.state_.approvals.clear();
        if (self.state_.session->pendingApproval) {
          self.state_.approvals.push_back(
              *self.state_.session->pendingApproval);
        }
        for (const auto& lease : self.state_.leases) {
          if (self.options_.onLeaseChanged) {
            self.options_.onLeaseChanged(lease);
          }
        }
        self.update();
        self.loadRemoteContext(generation, std::move(callback));
      }));
}

void CrowdyStudioAgentController::loadRemoteContext(
    std::uint64_t generation, AgentVoidCallback callback) {
  const auto sessionId = requireSession().sessionId;
  transport_.toolDescriptors(
      sessionId,
      deliver([generation, callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentDescriptorSet> descriptors) mutable {
        if (generation != self.generation_) return;
        if (!descriptors.ok()) {
          self.fail(*descriptors.error);
          finish(callback,
                 AgentOutcome<AgentVoid>::failure(*descriptors.error));
          return;
        }
        if (descriptors.value->registryDigest !=
            self.requireSession().registryDigest) {
          const auto error = makeAgentError(
              "AGENT_CONTEXT_STALE",
              "Session registry digest does not match effective descriptors");
          self.fail(error);
          finish(callback, AgentOutcome<AgentVoid>::failure(error));
          return;
        }
        self.state_.toolRegistry = descriptors.value->registry;
        self.update();
        self.transport_.budget(
            self.requireSession().sessionId,
            self.deliver([generation, callback = std::move(callback)](
                             CrowdyStudioAgentController& nested,
                             AgentOutcome<AgentBudget> budget) mutable {
              if (generation != nested.generation_) return;
              if (!budget.ok()) {
                nested.fail(*budget.error);
                finish(callback,
                       AgentOutcome<AgentVoid>::failure(*budget.error));
                return;
              }
              nested.state_.budget = std::move(*budget.value);
              nested.update();
              nested.replayHistory(generation, std::move(callback));
            }));
      }));
}

void CrowdyStudioAgentController::replayHistory(
    std::uint64_t generation, AgentVoidCallback callback) {
  if (generation != generation_ || historyInFlight_) return;
  historyInFlight_ = true;
  transport_.history(
      requireSession().sessionId, state_.lastContiguousSeq,
      options_.historyPageSize,
      deliver([generation, callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentHistoryPage> page) mutable {
        if (generation != self.generation_) return;
        self.historyInFlight_ = false;
        if (!page.ok()) {
          self.fail(*page.error);
          finish(callback, AgentOutcome<AgentVoid>::failure(*page.error));
          return;
        }
        const auto before = self.state_.lastContiguousSeq;
        try {
          for (auto& event : page.value->events) {
            self.bufferEvent(std::move(event));
          }
          self.drainBuffered(generation);
        } catch (const CrowdyAgentError& error) {
          self.fail(error.value());
          finish(callback,
                 AgentOutcome<AgentVoid>::failure(error.value()));
          return;
        }
        if (page.value->hasMore) {
          if (before == self.state_.lastContiguousSeq &&
              page.value->events.empty()) {
            const auto error = makeAgentError(
                "AGENT_EVENT_CURSOR_INVALID",
                "History page claimed more events without advancing");
            self.fail(error);
            finish(callback, AgentOutcome<AgentVoid>::failure(error));
            return;
          }
          self.replayHistory(generation, std::move(callback));
          return;
        }
        self.openSubscription(generation, std::move(callback));
      }));
}

void CrowdyStudioAgentController::openSubscription(
    std::uint64_t generation, AgentVoidCallback callback) {
  const std::weak_ptr<Lifetime> lifetime = lifetime_;
  const auto dispatcher = dispatcher_;
  AgentEventSubscriptionCallbacks callbacks;
  callbacks.next =
      [lifetime, dispatcher, generation](AgentEvent event) mutable {
        dispatcher->post(
            [lifetime, generation, event = std::move(event)]() mutable {
              const auto locked = lifetime.lock();
              if (!locked || !locked->alive.load() || !locked->owner) return;
              locked->owner->enqueueEvent(std::move(event), generation);
            });
      };
  callbacks.error =
      [lifetime, dispatcher, generation](AgentError error) mutable {
        dispatcher->post(
            [lifetime, generation, error = std::move(error)]() mutable {
              const auto locked = lifetime.lock();
              if (!locked || !locked->alive.load() || !locked->owner) return;
              locked->owner->handleDisconnect(std::move(error), generation);
            });
      };
  callbacks.complete = [lifetime, dispatcher, generation] {
    dispatcher->post([lifetime, generation] {
      const auto locked = lifetime.lock();
      if (!locked || !locked->alive.load() || !locked->owner) return;
      locked->owner->handleDisconnect(
          makeAgentError("AGENT_DISCONNECTED",
                         "Agent event stream closed", true),
          generation);
    });
  };
  callbacks.reconnect = [lifetime, dispatcher, generation] {
    dispatcher->post([lifetime, generation] {
      const auto locked = lifetime.lock();
      if (!locked || !locked->alive.load() || !locked->owner) return;
      auto& owner = *locked->owner;
      if (generation != owner.generation_ || owner.destroyed_) return;
      owner.recoverGap(generation, false);
    });
  };
  try {
    subscription_ = subscriptionAdapter_->subscribe(
        AgentEventSubscriptionRequest{requireSession().sessionId,
                                      state_.lastContiguousSeq,
                                      *state_.clientEpoch},
        std::move(callbacks));
  } catch (const CrowdyAgentError& error) {
    fail(error.value());
    finish(callback, AgentOutcome<AgentVoid>::failure(error.value()));
    return;
  }
  if (generation != generation_) {
    subscription_->close();
    subscription_.reset();
    return;
  }
  reconnectAttempts_ = 0;
  state_.connection = AgentConnectionState::Connected;
  state_.reconnectRequired = false;
  refreshTimerDeadlines();
  update();
  finish(callback, voidSuccess());
}

void CrowdyStudioAgentController::enqueueEvent(
    AgentEvent event, std::uint64_t generation) {
  if (generation != generation_ || destroyed_) return;
  try {
    bufferEvent(std::move(event));
    drainBuffered(generation);
  } catch (const CrowdyAgentError& error) {
    fail(error.value());
  }
}

void CrowdyStudioAgentController::bufferEvent(AgentEvent event) {
  if (event.protocolVersion != "crowdy.agent-event/1" ||
      event.eventId.empty() || event.eventId.size() > 128 ||
      event.sessionId != requireSession().sessionId ||
      !isNonNegativeSequence(event.seq)) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Event envelope is invalid");
  }
  const auto applied = appliedEventIds_.find(event.seq);
  if (applied != appliedEventIds_.end()) {
    if (applied->second != event.eventId) {
      throw CrowdyAgentError(
          "AGENT_EVENT_CURSOR_INVALID",
          "An applied sequence was reused by another event");
    }
    return;
  }
  const auto existing = buffered_.find(event.seq);
  if (existing != buffered_.end() &&
      existing->second.eventId != event.eventId) {
    throw CrowdyAgentError(
        "AGENT_EVENT_CURSOR_INVALID",
        "A buffered sequence conflicts with another event");
  }
  if (!decimalGreater(event.seq, state_.lastContiguousSeq)) return;
  buffered_.insert_or_assign(event.seq, std::move(event));
}

void CrowdyStudioAgentController::drainBuffered(
    std::uint64_t generation) {
  if (generation != generation_ || destroyed_) return;
  while (true) {
    const auto next = incrementDecimal(state_.lastContiguousSeq);
    const auto found = buffered_.find(next);
    if (found == buffered_.end()) break;
    auto event = std::move(found->second);
    buffered_.erase(found);
    applyEvent(std::move(event), generation);
  }
  scheduleAcknowledge(generation);
  if (!buffered_.empty()) recoverGap(generation);
}

void CrowdyStudioAgentController::recoverGap(
    std::uint64_t generation, bool requireAdvance) {
  if (generation != generation_ || gapRecoveryInFlight_ || destroyed_) return;
  gapRecoveryInFlight_ = true;
  const auto before = state_.lastContiguousSeq;
  transport_.history(
      requireSession().sessionId, before, options_.historyPageSize,
      deliver([generation, before, requireAdvance](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentHistoryPage> page) mutable {
        if (generation != self.generation_) return;
        self.gapRecoveryInFlight_ = false;
        if (!page.ok()) {
          self.handleDisconnect(*page.error, generation);
          return;
        }
        try {
          for (auto& event : page.value->events) {
            self.bufferEvent(std::move(event));
          }
          const auto next = incrementDecimal(self.state_.lastContiguousSeq);
          if (requireAdvance &&
              self.buffered_.find(next) == self.buffered_.end() &&
              before == self.state_.lastContiguousSeq) {
            throw CrowdyAgentError(
                "AGENT_EVENT_GAP",
                "Durable history could not fill the missing event sequence",
                true);
          }
          self.drainBuffered(generation);
        } catch (const CrowdyAgentError& error) {
          self.fail(error.value());
        }
      }));
}

void CrowdyStudioAgentController::applyEvent(
    AgentEvent event, std::uint64_t generation) {
  const auto eventType = event.type;
  const auto eventCreatedAt = event.createdAt;
  const auto eventSeq = event.seq;
  const auto eventId = event.eventId;
  std::optional<AgentToolInvocation> browserInvocation;

  if (const auto* lifecyclePayload =
          std::get_if<AgentLifecycleEventPayload>(&event.payload)) {
    if (eventType == AgentEventType::ModeSelected && lifecyclePayload->mode) {
      requireSession().mode = *lifecyclePayload->mode;
    }
    if (lifecyclePayload->status) {
      requireSession().status = *lifecyclePayload->status;
    }
    if (eventType == AgentEventType::ContextChanged) {
      if (!lifecyclePayload->contextVersion) {
        throw CrowdyAgentError(
            "AGENT_EVENT_CURSOR_INVALID",
            "CONTEXT_CHANGED omitted its context version");
      }
      requireSession().contextVersion = *lifecyclePayload->contextVersion;
      state_.leases.clear();
      state_.approvals.clear();
      state_.reconnectRequired = true;
      preemptLocal(AgentPreemptionReason::ContextChanged);
    }
  } else if (const auto* messagePayload =
                 std::get_if<AgentMessageEventPayload>(&event.payload)) {
    if (messagePayload->chunk) {
      state_.streamingText += messagePayload->message.content;
      if (state_.streamingText.size() > 65'536) {
        state_.streamingText.erase(
            0, state_.streamingText.size() - 65'536);
      }
    } else {
      upsert(state_.messages, messagePayload->message,
             [](const AgentMessage& message) { return message.messageId; });
      std::sort(state_.messages.begin(), state_.messages.end(),
                [](const AgentMessage& left, const AgentMessage& right) {
                  return left.createdAt < right.createdAt;
                });
      if (eventType == AgentEventType::AssistantMessage) {
        state_.streamingText.clear();
      }
    }
  } else if (const auto* runPayload =
                 std::get_if<AgentRunEventPayload>(&event.payload)) {
    requireSession().currentRun = runPayload->run;
  } else if (const auto* toolPayload =
                 std::get_if<AgentToolEventPayload>(&event.payload)) {
    auto found = std::find_if(
        state_.tools.begin(), state_.tools.end(),
        [&](const AgentToolTimelineItem& item) {
          return item.toolCallId == toolPayload->toolCallId;
        });
    AgentToolTimelineItem item;
    if (found != state_.tools.end()) item = *found;
    item.toolCallId = toolPayload->toolCallId;
    item.name = toolPayload->name;
    item.version = toolPayload->version;
    item.status = toolPayload->status;
    item.safeSummary = toolPayload->safeSummary
                           ? toolPayload->safeSummary
                           : item.safeSummary;
    item.argumentHash =
        toolPayload->argumentHash ? toolPayload->argumentHash
                                  : item.argumentHash;
    item.result = toolPayload->result;
    item.error = toolPayload->error;
    item.updatedAt = eventCreatedAt;
    if (state_.toolRegistry) {
      if (const auto* descriptor =
              state_.toolRegistry->get(item.name, item.version)) {
        item.risk = descriptor->descriptor->risk;
      }
    }
    upsert(state_.tools, std::move(item),
           [](const AgentToolTimelineItem& entry) {
             return entry.toolCallId;
           });
    if (eventType == AgentEventType::ToolDispatched &&
        toolPayload->invocation) {
      browserInvocation = toolPayload->invocation;
    }
  } else if (const auto* approvalPayload =
                 std::get_if<AgentApprovalEventPayload>(&event.payload)) {
    upsert(state_.approvals, approvalPayload->approval,
           [](const AgentApproval& approval) {
             return approval.approvalId;
           });
  } else if (const auto* leasePayload =
                 std::get_if<AgentLeaseEventPayload>(&event.payload)) {
    upsert(state_.leases, leasePayload->lease,
           [](const AgentLease& lease) { return lease.leaseId; });
    if (options_.onLeaseChanged) {
      options_.onLeaseChanged(leasePayload->lease);
    }
    if (eventType == AgentEventType::LeaseExpired ||
        eventType == AgentEventType::LeaseRevoked) {
      preemptLocal(leasePayload->lease.revokedReason.value_or(
          eventType == AgentEventType::LeaseExpired
              ? AgentPreemptionReason::LeaseExpired
              : AgentPreemptionReason::ContextChanged));
    }
  } else if (const auto* checkpointPayload =
                 std::get_if<AgentCheckpointEventPayload>(&event.payload)) {
    upsert(state_.checkpoints, checkpointPayload->checkpoint,
           [](const AgentCheckpoint& checkpoint) {
             return checkpoint.checkpointId;
           });
  } else if (const auto* budgetPayload =
                 std::get_if<AgentBudgetEventPayload>(&event.payload)) {
    state_.budget = budgetPayload->budget;
  } else {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Event payload does not match a stable type");
  }

  state_.lastContiguousSeq = eventSeq;
  appliedEventIds_[eventSeq] = eventId;
  state_.events.push_back(std::move(event));
  if (state_.events.size() > options_.maxRetainedEvents) {
    state_.events.erase(
        state_.events.begin(),
        state_.events.begin() +
            static_cast<std::ptrdiff_t>(
                state_.events.size() - options_.maxRetainedEvents));
  }
  update();
  refreshTimerDeadlines();
  if (browserInvocation && options_.browserDispatcher) {
    dispatchBrowserTool(std::move(*browserInvocation), generation,
                        effectGeneration_);
  }
}

void CrowdyStudioAgentController::dispatchBrowserTool(
    AgentToolInvocation invocation, std::uint64_t connectionGeneration,
    std::uint64_t effectGeneration) {
  if (!state_.clientEpoch || !invocation.clientEpoch ||
      *invocation.clientEpoch != *state_.clientEpoch) {
    return;
  }
  const auto expectedEpoch = *state_.clientEpoch;
  const auto expectedContext = requireSession().contextVersion;
  const auto leaseId = invocation.leaseId;
  const auto toolCallId = invocation.toolCallId;
  options_.browserDispatcher->dispatch(
      std::move(invocation),
      deliver([connectionGeneration, effectGeneration, expectedEpoch,
               expectedContext, leaseId, toolCallId](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentToolResult> outcome) mutable {
        if (connectionGeneration != self.generation_ ||
            effectGeneration != self.effectGeneration_ ||
            !self.state_.clientEpoch ||
            *self.state_.clientEpoch != expectedEpoch ||
            self.requireSession().contextVersion != expectedContext ||
            !outcome.ok()) {
          return;
        }
        auto result = std::move(*outcome.value);
        if (result.toolCallId != toolCallId ||
            result.observedContextVersion != expectedContext ||
            (result.error &&
             result.error->code == "AGENT_CLIENT_EPOCH_STALE")) {
          return;
        }
        if (leaseId) {
          const bool active = std::any_of(
              self.state_.leases.begin(), self.state_.leases.end(),
              [&](const AgentLease& lease) {
                return lease.leaseId == *leaseId &&
                       lease.status == AgentLeaseStatus::Active;
              });
          if (!active) return;
        }
        const auto context =
            self.mutationContext("tool-result-" + toolCallId);
        self.transport_.browserToolResult(
            context, std::move(result),
            self.deliver([connectionGeneration, effectGeneration](
                             CrowdyStudioAgentController& nested,
                             AgentOutcome<AgentToolCallAck> accepted) {
              if (connectionGeneration != nested.generation_ ||
                  effectGeneration != nested.effectGeneration_) {
                return;
              }
              if (!accepted.ok()) {
                nested.handleDisconnect(*accepted.error,
                                        connectionGeneration);
              }
            }));
      }));
}

void CrowdyStudioAgentController::scheduleAcknowledge(
    std::uint64_t generation) {
  if (generation != generation_ ||
      !decimalGreater(state_.lastContiguousSeq,
                      state_.lastAcknowledgedSeq)) {
    return;
  }
  if (decimalGreater(state_.lastContiguousSeq, pendingAcknowledge_)) {
    pendingAcknowledge_ = state_.lastContiguousSeq;
  }
  if (!acknowledgeGeneration_) sendAcknowledgement(generation);
}

void CrowdyStudioAgentController::sendAcknowledgement(
    std::uint64_t generation) {
  if (generation != generation_ || acknowledgeGeneration_ ||
      !state_.clientEpoch ||
      !decimalGreater(pendingAcknowledge_,
                      state_.lastAcknowledgedSeq)) {
    return;
  }
  acknowledgeGeneration_ = generation;
  const auto through = pendingAcknowledge_;
  transport_.acknowledgeEvents(
      mutationContext("ack-" + through), through,
      deliver([generation, through](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<std::string> outcome) {
        const bool ownsCompletion =
            self.acknowledgeGeneration_ &&
            *self.acknowledgeGeneration_ == generation;
        if (ownsCompletion) self.acknowledgeGeneration_.reset();
        if (generation != self.generation_) return;
        if (!ownsCompletion) return;
        if (!outcome.ok()) {
          self.handleDisconnect(*outcome.error, generation);
          return;
        }
        if (decimalGreater(*outcome.value,
                           self.state_.lastAcknowledgedSeq) ||
            *outcome.value == self.state_.lastAcknowledgedSeq) {
          self.state_.lastAcknowledgedSeq = *outcome.value;
          self.update();
        }
        if (decimalGreater(self.pendingAcknowledge_,
                           self.state_.lastAcknowledgedSeq)) {
          self.sendAcknowledgement(generation);
        }
      }));
}

void CrowdyStudioAgentController::sendMessage(
    std::string content, AgentCallback<AgentRun> callback) {
  const auto first = content.find_first_not_of(" \t\r\n");
  const auto last = content.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) content.clear();
  else content = content.substr(first, last - first + 1);
  if (content.empty() || content.size() > 16'384) {
    if (callback) {
      callback(AgentOutcome<AgentRun>::failure(makeAgentError(
          "AGENT_TOOL_INPUT_INVALID",
          "Agent message must be 1 to 16384 UTF-8 bytes")));
    }
    return;
  }
  if (options_.beforeAgentWork) {
    if (const auto error = options_.beforeAgentWork(requireSession().mode)) {
      if (callback) callback(AgentOutcome<AgentRun>::failure(*error));
      return;
    }
  }
  transport_.sendMessage(
      mutationContext("send-message"), std::move(content),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentRun> outcome) mutable {
        if (!outcome.ok()) {
          if (terminalPolicyError(outcome.error->code)) {
            self.handleDisconnect(*outcome.error, self.generation_);
          }
        }
        if (callback) callback(std::move(outcome));
      }));
}

void CrowdyStudioAgentController::setMode(
    AgentMode mode, AgentVoidCallback callback) {
  if (mode == AgentMode::Build && !requireSession().projectId) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_CONTEXT_CHANGED",
                         "BUILD requires a project-bound session")));
    return;
  }
  preemptLocal(AgentPreemptionReason::ContextChanged);
  const auto generation = generation_;
  transport_.setMode(
      mutationContext("set-mode"), mode,
      deliver([generation, callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        if (generation != self.generation_) return;
        if (!outcome.ok()) {
          self.fail(*outcome.error);
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
          return;
        }
        self.state_.session = std::move(*outcome.value);
        self.state_.leases.clear();
        self.state_.approvals.clear();
        self.state_.reconnectRequired = false;
        self.update();
        const auto sessionId = self.requireSession().sessionId;
        self.transport_.toolDescriptors(
            sessionId,
            self.deliver([generation, callback = std::move(callback)](
                             CrowdyStudioAgentController& nested,
                             AgentOutcome<AgentDescriptorSet> descriptors) mutable {
              if (generation != nested.generation_) return;
              if (!descriptors.ok() ||
                  descriptors.value->registryDigest !=
                      nested.requireSession().registryDigest) {
                const auto error =
                    descriptors.error.value_or(makeAgentError(
                        "AGENT_CONTEXT_STALE",
                        "Mode repin descriptor digest mismatch"));
                nested.fail(error);
                finish(callback, AgentOutcome<AgentVoid>::failure(error));
                return;
              }
              nested.state_.toolRegistry = descriptors.value->registry;
              nested.transport_.budget(
                  nested.requireSession().sessionId,
                  nested.deliver(
                      [generation, callback = std::move(callback)](
                          CrowdyStudioAgentController& current,
                          AgentOutcome<AgentBudget> budget) mutable {
                        if (generation != current.generation_) return;
                        if (!budget.ok()) {
                          current.fail(*budget.error);
                          finish(callback,
                                 AgentOutcome<AgentVoid>::failure(
                                     *budget.error));
                          return;
                        }
                        current.state_.budget = std::move(*budget.value);
                        current.refreshTimerDeadlines();
                        current.update();
                        finish(callback, voidSuccess());
                      }));
            }));
      }));
}

void CrowdyStudioAgentController::approveTool(
    std::string toolCallId,
    std::optional<std::string> expectedArgumentHash,
    AgentVoidCallback callback) {
  const auto found = std::find_if(
      state_.approvals.begin(), state_.approvals.end(),
      [&](const AgentApproval& approval) {
        return approval.toolCallId == toolCallId &&
               approval.status == AgentApprovalStatus::Pending;
      });
  if (found == state_.approvals.end() ||
      (expectedArgumentHash &&
       *expectedArgumentHash != found->argumentHash)) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_APPROVAL_MISMATCH",
                         "No matching pending exact approval exists")));
    return;
  }
  const auto argumentHash = found->argumentHash;
  if (core::parseIso8601Millis(found->expiresAt.data(),
                               found->expiresAt.size()) <=
      clock_.epochMillis()) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_APPROVAL_EXPIRED",
                         "Exact tool approval expired")));
    return;
  }
  transport_.approveTool(
      mutationContext("approve-tool"), std::move(toolCallId), argumentHash,
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentApproval> outcome) mutable {
        if (outcome.ok()) {
          upsert(self.state_.approvals, std::move(*outcome.value),
                 [](const AgentApproval& approval) {
                   return approval.approvalId;
                 });
          self.update();
          finish(callback, voidSuccess());
        } else {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::rejectTool(
    std::string toolCallId, std::optional<std::string> reason,
    AgentVoidCallback callback) {
  const auto found = std::find_if(
      state_.approvals.begin(), state_.approvals.end(),
      [&](const AgentApproval& approval) {
        return approval.toolCallId == toolCallId &&
               approval.status == AgentApprovalStatus::Pending;
      });
  if (found == state_.approvals.end()) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_APPROVAL_MISMATCH",
                         "No matching pending exact approval exists")));
    return;
  }
  if (reason && reason->size() > 512) reason->resize(512);
  transport_.rejectTool(
      mutationContext("reject-tool"), std::move(toolCallId),
      found->argumentHash, std::move(reason),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentApproval> outcome) mutable {
        if (outcome.ok()) {
          upsert(self.state_.approvals, std::move(*outcome.value),
                 [](const AgentApproval& approval) {
                   return approval.approvalId;
                 });
          self.update();
          finish(callback, voidSuccess());
        } else {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::grantPlayLease(
    std::vector<std::string> scopes, int durationSeconds,
    std::string controlledEntityId, std::string hostCapabilityRevision,
    AgentCallback<AgentLease> callback) {
  static const std::set<std::string> allowed = {
      "observe", "locomotion", "interact", "craft", "combat",
      "communicate", "travel", "grid", "trust_consent", "commerce"};
  std::sort(scopes.begin(), scopes.end());
  scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
  const bool invalidScope =
      std::any_of(scopes.begin(), scopes.end(),
                  [&](const auto& scope) { return !allowed.contains(scope); });
  if (durationSeconds < 1 || durationSeconds > 600 || scopes.empty() ||
      scopes.size() > 10 || invalidScope) {
    if (callback) {
      callback(AgentOutcome<AgentLease>::failure(makeAgentError(
          "AGENT_LEASE_SCOPE_MISSING",
          "Play lease requires 1 to 10 known scopes and 1 to 600 seconds")));
    }
    return;
  }
  transport_.grantLease(
      mutationContext("grant-play-lease"), std::move(scopes),
      durationSeconds, std::move(controlledEntityId),
      std::move(hostCapabilityRevision),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentLease> outcome) mutable {
        if (outcome.ok()) {
          upsert(self.state_.leases, *outcome.value,
                 [](const AgentLease& lease) { return lease.leaseId; });
          if (self.options_.onLeaseChanged) {
            self.options_.onLeaseChanged(*outcome.value);
          }
          self.update();
        }
        if (callback) callback(std::move(outcome));
      }));
}

void CrowdyStudioAgentController::revokeLease(
    std::string leaseId, AgentPreemptionReason reason,
    AgentVoidCallback callback) {
  transport_.revokeLease(
      mutationContext("revoke-lease"), std::move(leaseId), reason,
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentLease> outcome) mutable {
        if (outcome.ok()) {
          upsert(self.state_.leases, std::move(*outcome.value),
                 [](const AgentLease& lease) { return lease.leaseId; });
          self.update();
          finish(callback, voidSuccess());
        } else {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::pause(AgentVoidCallback callback) {
  preemptLocal(AgentPreemptionReason::HumanStop);
  transport_.pause(
      mutationContext("pause"),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        if (outcome.ok()) {
          self.state_.session = std::move(*outcome.value);
          self.state_.playLeaseFreshUntil.reset();
          self.state_.leases.clear();
          self.update();
          finish(callback, voidSuccess());
        } else {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::resume(AgentVoidCallback callback) {
  const auto generation = generation_;
  transport_.resume(
      mutationContext("resume"),
      deliver([generation, callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        if (generation != self.generation_) return;
        if (!outcome.ok()) {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
          return;
        }
        self.state_.session = std::move(*outcome.value);
        if (self.requireSession().mode == AgentMode::Play) {
          self.state_.leases.erase(
              std::remove_if(
                  self.state_.leases.begin(), self.state_.leases.end(),
                  [](const AgentLease& lease) {
                    return lease.kind == AgentLeaseKind::Play;
                  }),
              self.state_.leases.end());
        }
        self.state_.reconnectRequired = false;
        self.update();
        self.transport_.toolDescriptors(
            self.requireSession().sessionId,
            self.deliver([generation, callback = std::move(callback)](
                             CrowdyStudioAgentController& nested,
                             AgentOutcome<AgentDescriptorSet> descriptors) mutable {
              if (generation != nested.generation_) return;
              if (!descriptors.ok() ||
                  descriptors.value->registryDigest !=
                      nested.requireSession().registryDigest) {
                const auto error =
                    descriptors.error.value_or(makeAgentError(
                        "AGENT_CONTEXT_STALE",
                        "Resume descriptor digest mismatch"));
                nested.fail(error);
                finish(callback, AgentOutcome<AgentVoid>::failure(error));
                return;
              }
              nested.state_.toolRegistry = descriptors.value->registry;
              nested.transport_.budget(
                  nested.requireSession().sessionId,
                  nested.deliver(
                      [generation, callback = std::move(callback)](
                          CrowdyStudioAgentController& current,
                          AgentOutcome<AgentBudget> budget) mutable {
                        if (generation != current.generation_) return;
                        if (!budget.ok()) {
                          current.fail(*budget.error);
                          finish(callback,
                                 AgentOutcome<AgentVoid>::failure(
                                     *budget.error));
                          return;
                        }
                        current.state_.budget = std::move(*budget.value);
                        current.refreshTimerDeadlines();
                        current.update();
                        finish(callback, voidSuccess());
                      }));
            }));
      }));
}

void CrowdyStudioAgentController::cancelRun(
    std::optional<std::string> runId, AgentVoidCallback callback) {
  if (!runId && state_.session && state_.session->currentRun) {
    runId = state_.session->currentRun->runId;
  }
  if (!runId) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_RUN_NOT_ACTIVE",
                         "No current run exists to cancel")));
    return;
  }
  transport_.cancelRun(
      mutationContext("cancel-run"), std::move(*runId),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentRun> outcome) mutable {
        if (outcome.ok()) {
          self.requireSession().currentRun = std::move(*outcome.value);
          self.update();
          finish(callback, voidSuccess());
        } else {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::stop(AgentVoidCallback callback) {
  const auto context = mutationContext("stop");
  const auto activeLeases = state_.leases;
  const auto currentRun =
      state_.session && state_.session->currentRun
          ? std::optional<std::string>(state_.session->currentRun->runId)
          : std::nullopt;
  preemptLocal(AgentPreemptionReason::HumanStop);
  for (const auto& lease : activeLeases) {
    if (lease.status != AgentLeaseStatus::Active) continue;
    auto revokeContext = context;
    revokeContext.idempotencyKey =
        nextKey("stop-revoke-" + lease.leaseId);
    transport_.revokeLease(
        std::move(revokeContext), lease.leaseId,
        AgentPreemptionReason::HumanStop,
        [](AgentOutcome<AgentLease>) {});
  }
  if (currentRun) {
    auto cancelContext = context;
    cancelContext.idempotencyKey = nextKey("stop-cancel");
    transport_.cancelRun(std::move(cancelContext), *currentRun,
                         [](AgentOutcome<AgentRun>) {});
  }
  auto pauseContext = context;
  pauseContext.idempotencyKey = nextKey("stop-pause");
  transport_.pause(
      std::move(pauseContext),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        self.state_.leases.clear();
        self.state_.approvals.clear();
        self.state_.playLeaseFreshUntil.reset();
        if (outcome.ok()) {
          self.state_.session = std::move(*outcome.value);
          self.update();
          finish(callback, voidSuccess());
        } else {
          self.update();
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      }));
}

void CrowdyStudioAgentController::restoreCheckpoint(
    std::string checkpointId, AgentVoidCallback callback) {
  const auto found = std::find_if(
      state_.checkpoints.begin(), state_.checkpoints.end(),
      [&](const AgentCheckpoint& checkpoint) {
        return checkpoint.checkpointId == checkpointId;
      });
  if (found == state_.checkpoints.end()) {
    finish(callback, AgentOutcome<AgentVoid>::failure(makeAgentError(
                         "AGENT_CHECKPOINT_NOT_FOUND",
                         "Checkpoint is not loaded")));
    return;
  }
  const auto message =
      "Request checkpoint restore for " + found->checkpointId +
      " at project revision " + found->projectRevision +
      " with content hash " + found->contentHash +
      ". Do not execute without exact human approval.";
  sendMessage(
      message,
      [callback = std::move(callback)](AgentOutcome<AgentRun> outcome) mutable {
        if (outcome.ok()) finish(callback, voidSuccess());
        else {
          finish(callback,
                 AgentOutcome<AgentVoid>::failure(*outcome.error));
        }
      });
}

void CrowdyStudioAgentController::close(AgentVoidCallback callback) {
  preemptLocal(AgentPreemptionReason::SessionClosed);
  transport_.closeSession(
      mutationContext("close-session"),
      deliver([callback = std::move(callback)](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentSession> outcome) mutable {
        if (!outcome.ok()) {
          finish(callback, AgentOutcome<AgentVoid>::failure(*outcome.error));
          return;
        }
        ++self.generation_;
        if (self.subscription_) self.subscription_->close();
        self.subscription_.reset();
        self.state_.connection = AgentConnectionState::Disconnected;
        self.state_.session = std::move(*outcome.value);
        self.state_.clientEpoch.reset();
        self.state_.leases.clear();
        self.state_.approvals.clear();
        self.acknowledgeGeneration_.reset();
        self.heartbeatInFlight_ = false;
        self.workspaceHeartbeatInFlight_ = false;
        if (self.options_.browserDispatcher) {
          self.options_.browserDispatcher->clearClosedSession();
        }
        self.update();
        finish(callback, voidSuccess());
      }));
}

void CrowdyStudioAgentController::setPageVisible(bool visible) {
  if (pageVisible_ == visible) return;
  pageVisible_ = visible;
  if (!visible && state_.connection == AgentConnectionState::Connected &&
      state_.session && state_.session->mode == AgentMode::Play) {
    preemptLocal(AgentPreemptionReason::Disconnected);
    state_.leases.clear();
    state_.playLeaseFreshUntil.reset();
  }
  refreshTimerDeadlines();
  update();
}

void CrowdyStudioAgentController::projectSelectionChanged(
    std::optional<std::string> projectId) {
  if (!state_.session || state_.session->projectId == projectId) return;
  if (state_.clientEpoch) {
    const auto context = mutationContext("project-switch");
    for (const auto& lease : state_.leases) {
      if (lease.status != AgentLeaseStatus::Active) continue;
      auto revokeContext = context;
      revokeContext.idempotencyKey =
          nextKey("project-switch-revoke-" + lease.leaseId);
      transport_.revokeLease(
          std::move(revokeContext), lease.leaseId,
          AgentPreemptionReason::ContextChanged,
          [](AgentOutcome<AgentLease>) {});
    }
    if (state_.session->currentRun) {
      auto cancelContext = context;
      cancelContext.idempotencyKey = nextKey("project-switch-cancel");
      transport_.cancelRun(std::move(cancelContext),
                           state_.session->currentRun->runId,
                           [](AgentOutcome<AgentRun>) {});
    }
    auto pauseContext = context;
    pauseContext.idempotencyKey = nextKey("project-switch-pause");
    transport_.pause(std::move(pauseContext),
                     [](AgentOutcome<AgentSession>) {});
  }
  preemptLocal(AgentPreemptionReason::ContextChanged);
  ++generation_;
  acknowledgeGeneration_.reset();
  if (subscription_) subscription_->close();
  subscription_.reset();
  state_.connection = AgentConnectionState::Error;
  state_.clientEpoch.reset();
  state_.leases.clear();
  state_.approvals.clear();
  state_.reconnectRequired = true;
  state_.lastError = makeAgentError(
      "AGENT_CONTEXT_CHANGED",
      "Selected project changed; create a new session for that project");
  update();
}

void CrowdyStudioAgentController::preemptForHumanEdit() {
  preemptLocal(AgentPreemptionReason::HumanEdit);
  state_.reconnectRequired = true;
  const auto found = std::find_if(
      state_.leases.begin(), state_.leases.end(), [](const AgentLease& lease) {
        return lease.kind == AgentLeaseKind::Workspace &&
               lease.status == AgentLeaseStatus::Active;
      });
  if (found != state_.leases.end() && state_.clientEpoch) {
    transport_.revokeLease(
        mutationContext("human-edit-revoke"), found->leaseId,
        AgentPreemptionReason::HumanEdit,
        [](AgentOutcome<AgentLease>) {});
  }
  if (state_.session && state_.session->currentRun &&
      state_.clientEpoch) {
    transport_.cancelRun(
        mutationContext("human-edit-cancel"),
        state_.session->currentRun->runId,
        [](AgentOutcome<AgentRun>) {});
  }
  update();
}

void CrowdyStudioAgentController::destroy() {
  if (destroyed_) return;
  destroyed_ = true;
  ++generation_;
  acknowledgeGeneration_.reset();
  ++effectGeneration_;
  if (subscription_) subscription_->close();
  subscription_.reset();
  if (options_.browserDispatcher) {
    options_.browserDispatcher->cancelActive(
        AgentPreemptionReason::Disconnected);
  }
  lifetime_->alive.store(false);
  lifetime_->owner = nullptr;
  listeners_.clear();
}

void CrowdyStudioAgentController::handleDisconnect(
    AgentError error, std::uint64_t generation) {
  if (generation != generation_ || destroyed_) return;
  ++generation_;
  ++effectGeneration_;
  if (subscription_) subscription_->close();
  subscription_.reset();
  heartbeatInFlight_ = false;
  workspaceHeartbeatInFlight_ = false;
  historyInFlight_ = false;
  gapRecoveryInFlight_ = false;
  acknowledgeGeneration_.reset();
  const auto reason =
      error.code == "AGENT_OPERATOR_KILLED"
          ? AgentPreemptionReason::OperatorKill
          : error.code == "AGENT_CLIENT_EPOCH_STALE"
                ? AgentPreemptionReason::ClientReattached
                : AgentPreemptionReason::Disconnected;
  preemptLocal(reason);
  state_.connection = AgentConnectionState::Disconnected;
  state_.clientEpoch.reset();
  state_.leases.clear();
  state_.approvals.clear();
  state_.playLeaseFreshUntil.reset();
  state_.reconnectRequired = true;
  state_.lastError = std::move(error);
  nextHeartbeatAt_ = 0;
  nextWorkspaceRenewAt_ = 0;
  update();
  if (options_.autoReconnect &&
      !terminalPolicyError(state_.lastError->code) &&
      reconnectAttempts_ < options_.maxReconnectAttempts) {
    const auto factor =
        std::min<std::int64_t>(8, std::int64_t{1} << reconnectAttempts_);
    reconnectAt_ = clock_.monotonicMillis() +
                   static_cast<std::int64_t>(options_.reconnectDelayMs) *
                       factor;
    ++reconnectAttempts_;
  }
}

void CrowdyStudioAgentController::fail(AgentError error) {
  state_.connection = AgentConnectionState::Error;
  state_.lastError = std::move(error);
  state_.reconnectRequired = true;
  nextHeartbeatAt_ = 0;
  nextWorkspaceRenewAt_ = 0;
  update();
}

void CrowdyStudioAgentController::checkTimers() {
  if (destroyed_) return;
  const auto now = clock_.monotonicMillis();
  if (reconnectAt_ > 0 && now >= reconnectAt_ &&
      state_.connection == AgentConnectionState::Disconnected) {
    reconnectAt_ = 0;
    reconnect();
    return;
  }
  if (heartbeatInFlight_ &&
      now - heartbeatStartedAt_ >= options_.heartbeatStaleMs) {
    handleDisconnect(
        makeAgentError("AGENT_DISCONNECTED",
                       "Agent heartbeat exceeded the freshness window", true),
        generation_);
    return;
  }
  if (workspaceHeartbeatInFlight_ &&
      now - workspaceHeartbeatStartedAt_ >= options_.heartbeatStaleMs) {
    workspaceHeartbeatInFlight_ = false;
    preemptLocal(AgentPreemptionReason::ContextChanged);
    for (auto& lease : state_.leases) {
      if (lease.kind == AgentLeaseKind::Workspace &&
          lease.status == AgentLeaseStatus::Active) {
        lease.status = AgentLeaseStatus::Revoked;
        lease.revokedReason = AgentPreemptionReason::ContextChanged;
        if (options_.onLeaseChanged) options_.onLeaseChanged(lease);
      }
    }
    state_.lastError = makeAgentError(
        "AGENT_LEASE_REVOKED",
        "Workspace renewal exceeded the freshness window");
    state_.reconnectRequired = true;
    nextWorkspaceRenewAt_ = 0;
    update();
  }
  refreshTimerDeadlines();
  if (nextHeartbeatAt_ > 0 && now >= nextHeartbeatAt_ &&
      !heartbeatInFlight_) {
    sendHeartbeat(generation_, false);
  }
  if (nextWorkspaceRenewAt_ > 0 && now >= nextWorkspaceRenewAt_ &&
      !workspaceHeartbeatInFlight_) {
    sendHeartbeat(generation_, true);
  }
}

void CrowdyStudioAgentController::sendHeartbeat(
    std::uint64_t generation, bool workspace) {
  if (generation != generation_ || !state_.clientEpoch) return;
  const auto now = clock_.monotonicMillis();
  if (workspace) {
    workspaceHeartbeatInFlight_ = true;
    workspaceHeartbeatStartedAt_ = now;
    nextWorkspaceRenewAt_ = 0;
  } else {
    heartbeatInFlight_ = true;
    heartbeatStartedAt_ = now;
    nextHeartbeatAt_ = 0;
  }
  std::string operation = "heartbeat";
  if (workspace) {
    const auto lease = std::find_if(
        state_.leases.begin(), state_.leases.end(),
        [](const AgentLease& entry) {
          return entry.kind == AgentLeaseKind::Workspace &&
                 entry.status == AgentLeaseStatus::Active;
        });
    operation =
        lease == state_.leases.end()
            ? "renew-workspace"
            : "renew-workspace-" + lease->leaseId;
  }
  transport_.heartbeat(
      mutationContext(operation),
      deliver([generation, workspace](
                  CrowdyStudioAgentController& self,
                  AgentOutcome<AgentHeartbeat> outcome) {
        if (generation != self.generation_) return;
        if (workspace) self.workspaceHeartbeatInFlight_ = false;
        else self.heartbeatInFlight_ = false;
        if (!outcome.ok()) {
          if (!workspace ||
              outcome.error->code == "AGENT_CLIENT_EPOCH_STALE" ||
              terminalPolicyError(outcome.error->code)) {
            self.handleDisconnect(*outcome.error, generation);
            return;
          }
          self.preemptLocal(AgentPreemptionReason::ContextChanged);
          self.state_.lastError = *outcome.error;
          self.state_.reconnectRequired = true;
          self.update();
          return;
        }
        const auto heartbeat = std::move(*outcome.value);
        self.state_.lastHeartbeatAt = heartbeat.serverTime;
        if (!workspace) {
          self.state_.playLeaseFreshUntil =
              heartbeat.playLeaseFreshUntil;
          self.nextHeartbeatAt_ =
              self.clock_.monotonicMillis() +
              self.options_.heartbeatIntervalMs;
        } else {
          if (!heartbeat.workspaceLeaseExpiresAt) {
            self.preemptLocal(AgentPreemptionReason::ContextChanged);
            self.state_.lastError = makeAgentError(
                "AGENT_LEASE_REVOKED",
                "Server did not renew the workspace lease");
            self.state_.reconnectRequired = true;
          } else {
            for (auto& lease : self.state_.leases) {
              if (lease.kind == AgentLeaseKind::Workspace &&
                  lease.status == AgentLeaseStatus::Active &&
                  lease.contextVersion ==
                      self.requireSession().contextVersion) {
                lease.expiresAt = *heartbeat.workspaceLeaseExpiresAt;
                if (self.options_.onLeaseChanged) {
                  self.options_.onLeaseChanged(lease);
                }
              }
            }
            self.nextWorkspaceRenewAt_ =
                self.clock_.monotonicMillis() +
                self.options_.workspaceRenewIntervalMs;
          }
        }
        self.update();
        self.refreshTimerDeadlines();
      }));
}

void CrowdyStudioAgentController::refreshTimerDeadlines() {
  const auto now = clock_.monotonicMillis();
  const bool connected =
      state_.connection == AgentConnectionState::Connected &&
      state_.session &&
      state_.session->status == AgentSessionStatus::Active &&
      state_.clientEpoch;
  const bool play =
      connected && pageVisible_ && state_.session->mode == AgentMode::Play;
  if (!play) {
    nextHeartbeatAt_ = 0;
    if (!heartbeatInFlight_) state_.playLeaseFreshUntil.reset();
  } else if (!heartbeatInFlight_ && nextHeartbeatAt_ == 0) {
    nextHeartbeatAt_ = now;
  }
  const bool hasWorkspace = std::any_of(
      state_.leases.begin(), state_.leases.end(),
      [](const AgentLease& lease) {
        return lease.kind == AgentLeaseKind::Workspace &&
               lease.status == AgentLeaseStatus::Active;
      });
  const bool build = connected &&
                     state_.session->mode == AgentMode::Build &&
                     hasWorkspace;
  if (!build) {
    nextWorkspaceRenewAt_ = 0;
  } else if (!workspaceHeartbeatInFlight_ &&
             nextWorkspaceRenewAt_ == 0) {
    nextWorkspaceRenewAt_ =
        now + options_.workspaceRenewIntervalMs;
  }
}

void CrowdyStudioAgentController::preemptLocal(
    AgentPreemptionReason reason) {
  ++effectGeneration_;
  nextWorkspaceRenewAt_ = 0;
  if (options_.browserDispatcher) {
    options_.browserDispatcher->cancelActive(reason);
  }
  if (options_.onPreempt) options_.onPreempt(reason);
}

void CrowdyStudioAgentController::update() {
  if (options_.onStateChange) options_.onStateChange(state_);
  for (const auto& [id, listener] : listeners_) {
    (void)id;
    listener(state_);
  }
}

AgentSession& CrowdyStudioAgentController::requireSession() {
  if (!state_.session) {
    throw CrowdyAgentError("AGENT_SESSION_NOT_FOUND",
                           "Agent session is not initialized");
  }
  return *state_.session;
}

AgentMutationContext CrowdyStudioAgentController::mutationContext(
    std::string_view operation) {
  if (!state_.clientEpoch) {
    throw CrowdyAgentError("AGENT_DISCONNECTED",
                           "An attached client epoch is required");
  }
  return AgentMutationContext{requireSession().sessionId,
                              *state_.clientEpoch, nextKey(operation)};
}

std::string CrowdyStudioAgentController::nextKey(
    std::string_view operation) {
  if (options_.createIdempotencyKey) {
    auto key = options_.createIdempotencyKey(operation);
    if (!key.empty() && key.size() <= 240) return key;
  }
  auto key = "crowdy-agent:" + std::string(operation) + ":" +
             std::to_string(clock_.epochMillis()) + ":" +
             std::to_string(++keySequence_);
  if (key.size() > 240) key.resize(240);
  return key;
}

AgentOutcome<AgentVoid> CrowdyStudioAgentController::voidSuccess() {
  return AgentOutcome<AgentVoid>::success(AgentVoid{});
}

void CrowdyStudioAgentController::finish(
    const AgentVoidCallback& callback, AgentOutcome<AgentVoid> outcome) {
  if (callback) callback(std::move(outcome));
}

}  // namespace crowdy::agent
