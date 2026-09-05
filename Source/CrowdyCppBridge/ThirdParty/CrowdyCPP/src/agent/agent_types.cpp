#include "crowdy/agent/types.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace crowdy::agent {

namespace {

using namespace std::string_view_literals;

template <typename Enum, std::size_t Size>
std::optional<Enum> parseEnum(
    std::string_view value,
    const std::array<std::pair<std::string_view, Enum>, Size>& values) {
  const auto found = std::find_if(
      values.begin(), values.end(),
      [&](const auto& entry) { return entry.first == value; });
  return found == values.end() ? std::nullopt
                               : std::optional<Enum>(found->second);
}

template <typename Enum, std::size_t Size>
std::string_view printEnum(
    Enum value,
    const std::array<std::pair<std::string_view, Enum>, Size>& values) {
  const auto found = std::find_if(
      values.begin(), values.end(),
      [&](const auto& entry) { return entry.second == value; });
  return found == values.end() ? std::string_view{} : found->first;
}

constexpr auto kModes =
    std::array{std::pair{"ASK"sv, AgentMode::Ask},
               std::pair{"BUILD"sv, AgentMode::Build},
               std::pair{"PLAY"sv, AgentMode::Play}};
constexpr auto kExecutors =
    std::array{std::pair{"SERVER"sv, AgentToolExecutor::Server},
               std::pair{"BROWSER"sv, AgentToolExecutor::Browser}};
constexpr auto kRisks =
    std::array{std::pair{"READ_ONLY"sv, AgentToolRisk::ReadOnly},
               std::pair{"ROUTINE_WRITE"sv, AgentToolRisk::RoutineWrite},
               std::pair{"WORLD_CONTROL"sv, AgentToolRisk::WorldControl},
               std::pair{"DESTRUCTIVE"sv, AgentToolRisk::Destructive},
               std::pair{"TRUST_CONSENT"sv, AgentToolRisk::TrustConsent},
               std::pair{"ECONOMIC"sv, AgentToolRisk::Economic},
               std::pair{"IRREVERSIBLE"sv, AgentToolRisk::Irreversible}};
constexpr auto kApprovalPolicies =
    std::array{std::pair{"NONE"sv, AgentApprovalPolicy::None},
               std::pair{"REQUIRED"sv, AgentApprovalPolicy::Required},
               std::pair{"CONDITIONAL"sv, AgentApprovalPolicy::Conditional}};
constexpr auto kIdempotency =
    std::array{std::pair{"PURE"sv, AgentIdempotencyClass::Pure},
               std::pair{"KEYED"sv, AgentIdempotencyClass::Keyed},
               std::pair{"TOOL_CALL_ONCE"sv,
                         AgentIdempotencyClass::ToolCallOnce},
               std::pair{"NON_RETRYABLE"sv,
                         AgentIdempotencyClass::NonRetryable}};
constexpr auto kResultStatuses =
    std::array{std::pair{"SUCCEEDED"sv, AgentToolResultStatus::Succeeded},
               std::pair{"FAILED"sv, AgentToolResultStatus::Failed},
               std::pair{"CANCELLED"sv, AgentToolResultStatus::Cancelled},
               std::pair{"TIMED_OUT"sv, AgentToolResultStatus::TimedOut},
               std::pair{"OUTCOME_UNKNOWN"sv,
                         AgentToolResultStatus::OutcomeUnknown}};
constexpr auto kSessionStatuses =
    std::array{std::pair{"ACTIVE"sv, AgentSessionStatus::Active},
               std::pair{"PAUSED"sv, AgentSessionStatus::Paused},
               std::pair{"CLOSED"sv, AgentSessionStatus::Closed},
               std::pair{"REVOKED"sv, AgentSessionStatus::Revoked}};
constexpr auto kRunStatuses =
    std::array{std::pair{"QUEUED"sv, AgentRunStatus::Queued},
               std::pair{"RUNNING"sv, AgentRunStatus::Running},
               std::pair{"WAITING_FOR_TOOL"sv,
                         AgentRunStatus::WaitingForTool},
               std::pair{"WAITING_FOR_APPROVAL"sv,
                         AgentRunStatus::WaitingForApproval},
               std::pair{"PAUSED"sv, AgentRunStatus::Paused},
               std::pair{"SUCCEEDED"sv, AgentRunStatus::Succeeded},
               std::pair{"FAILED"sv, AgentRunStatus::Failed},
               std::pair{"CANCELLED"sv, AgentRunStatus::Cancelled},
               std::pair{"PREEMPTED"sv, AgentRunStatus::Preempted}};
constexpr auto kToolStatuses =
    std::array{std::pair{"PROPOSED"sv, AgentToolCallStatus::Proposed},
               std::pair{"WAITING_FOR_APPROVAL"sv,
                         AgentToolCallStatus::WaitingForApproval},
               std::pair{"DISPATCHED"sv, AgentToolCallStatus::Dispatched},
               std::pair{"RUNNING"sv, AgentToolCallStatus::Running},
               std::pair{"SUCCEEDED"sv, AgentToolCallStatus::Succeeded},
               std::pair{"FAILED"sv, AgentToolCallStatus::Failed},
               std::pair{"DENIED"sv, AgentToolCallStatus::Denied},
               std::pair{"TIMED_OUT"sv, AgentToolCallStatus::TimedOut},
               std::pair{"CANCELLED"sv, AgentToolCallStatus::Cancelled},
               std::pair{"STALE"sv, AgentToolCallStatus::Stale},
               std::pair{"OUTCOME_UNKNOWN"sv,
                         AgentToolCallStatus::OutcomeUnknown}};
constexpr auto kLeaseStatuses =
    std::array{std::pair{"ACTIVE"sv, AgentLeaseStatus::Active},
               std::pair{"REVOKED"sv, AgentLeaseStatus::Revoked},
               std::pair{"EXPIRED"sv, AgentLeaseStatus::Expired}};
constexpr auto kApprovalStatuses =
    std::array{std::pair{"PENDING"sv, AgentApprovalStatus::Pending},
               std::pair{"GRANTED"sv, AgentApprovalStatus::Granted},
               std::pair{"DENIED"sv, AgentApprovalStatus::Denied},
               std::pair{"CONSUMED"sv, AgentApprovalStatus::Consumed},
               std::pair{"REVOKED"sv, AgentApprovalStatus::Revoked},
               std::pair{"EXPIRED"sv, AgentApprovalStatus::Expired}};
constexpr auto kLeaseKinds =
    std::array{std::pair{"WORKSPACE"sv, AgentLeaseKind::Workspace},
               std::pair{"PLAY"sv, AgentLeaseKind::Play}};
constexpr auto kPreemptions = std::array{
    std::pair{"HUMAN_INPUT"sv, AgentPreemptionReason::HumanInput},
    std::pair{"HUMAN_EDIT"sv, AgentPreemptionReason::HumanEdit},
    std::pair{"HUMAN_STOP"sv, AgentPreemptionReason::HumanStop},
    std::pair{"ESCAPE"sv, AgentPreemptionReason::Escape},
    std::pair{"DEATH"sv, AgentPreemptionReason::Death},
    std::pair{"CONTEXT_CHANGED"sv, AgentPreemptionReason::ContextChanged},
    std::pair{"PERMISSION_CHANGED"sv,
              AgentPreemptionReason::PermissionChanged},
    std::pair{"ADMISSION_CHANGED"sv, AgentPreemptionReason::AdmissionChanged},
    std::pair{"CONTROL_TARGET_CHANGED"sv,
              AgentPreemptionReason::ControlTargetChanged},
    std::pair{"DISCONNECTED"sv, AgentPreemptionReason::Disconnected},
    std::pair{"CLIENT_REATTACHED"sv,
              AgentPreemptionReason::ClientReattached},
    std::pair{"QUOTA_FAILURE"sv, AgentPreemptionReason::QuotaFailure},
    std::pair{"BUDGET_FAILURE"sv, AgentPreemptionReason::BudgetFailure},
    std::pair{"OPERATOR_KILL"sv, AgentPreemptionReason::OperatorKill},
    std::pair{"LEASE_EXPIRED"sv, AgentPreemptionReason::LeaseExpired},
    std::pair{"SESSION_CLOSED"sv, AgentPreemptionReason::SessionClosed},
};
constexpr auto kEventTypes = std::array{
    std::pair{"SESSION_CREATED"sv, AgentEventType::SessionCreated},
    std::pair{"SESSION_PAUSED"sv, AgentEventType::SessionPaused},
    std::pair{"SESSION_RESUMED"sv, AgentEventType::SessionResumed},
    std::pair{"SESSION_CLOSED"sv, AgentEventType::SessionClosed},
    std::pair{"CLIENT_ATTACHED"sv, AgentEventType::ClientAttached},
    std::pair{"CLIENT_DETACHED"sv, AgentEventType::ClientDetached},
    std::pair{"MODE_SELECTED"sv, AgentEventType::ModeSelected},
    std::pair{"USER_MESSAGE"sv, AgentEventType::UserMessage},
    std::pair{"RUN_STARTED"sv, AgentEventType::RunStarted},
    std::pair{"ASSISTANT_CHUNK"sv, AgentEventType::AssistantChunk},
    std::pair{"ASSISTANT_MESSAGE"sv, AgentEventType::AssistantMessage},
    std::pair{"TOOL_PROPOSED"sv, AgentEventType::ToolProposed},
    std::pair{"TOOL_DISPATCHED"sv, AgentEventType::ToolDispatched},
    std::pair{"TOOL_SUCCEEDED"sv, AgentEventType::ToolSucceeded},
    std::pair{"TOOL_FAILED"sv, AgentEventType::ToolFailed},
    std::pair{"TOOL_DENIED"sv, AgentEventType::ToolDenied},
    std::pair{"TOOL_TIMED_OUT"sv, AgentEventType::ToolTimedOut},
    std::pair{"TOOL_OUTCOME_UNKNOWN"sv,
              AgentEventType::ToolOutcomeUnknown},
    std::pair{"APPROVAL_REQUESTED"sv, AgentEventType::ApprovalRequested},
    std::pair{"APPROVAL_GRANTED"sv, AgentEventType::ApprovalGranted},
    std::pair{"APPROVAL_DENIED"sv, AgentEventType::ApprovalDenied},
    std::pair{"APPROVAL_CONSUMED"sv, AgentEventType::ApprovalConsumed},
    std::pair{"APPROVAL_EXPIRED"sv, AgentEventType::ApprovalExpired},
    std::pair{"CHECKPOINT_CREATED"sv, AgentEventType::CheckpointCreated},
    std::pair{"CHECKPOINT_RESTORED"sv, AgentEventType::CheckpointRestored},
    std::pair{"LEASE_GRANTED"sv, AgentEventType::LeaseGranted},
    std::pair{"LEASE_REVOKED"sv, AgentEventType::LeaseRevoked},
    std::pair{"LEASE_EXPIRED"sv, AgentEventType::LeaseExpired},
    std::pair{"CONTEXT_CHANGED"sv, AgentEventType::ContextChanged},
    std::pair{"BUDGET_UPDATED"sv, AgentEventType::BudgetUpdated},
    std::pair{"RUN_PAUSED"sv, AgentEventType::RunPaused},
    std::pair{"RUN_SUCCEEDED"sv, AgentEventType::RunSucceeded},
    std::pair{"RUN_FAILED"sv, AgentEventType::RunFailed},
    std::pair{"RUN_CANCELLED"sv, AgentEventType::RunCancelled},
    std::pair{"RUN_PREEMPTED"sv, AgentEventType::RunPreempted},
};

std::optional<std::string> optionalString(const graphql::Json& value,
                                          std::string_view field) {
  const auto child = value[field];
  if (!child.ok() || child.isNull()) return std::nullopt;
  if (!child.isString() && !child.isNumber()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           std::string(field) + " must be a string");
  }
  return child.isString() ? child.asString() : child.dump();
}

std::string requiredString(const graphql::Json& value,
                           std::string_view field,
                           std::size_t maximum = 65'536) {
  auto result = optionalString(value, field);
  if (!result || result->empty() || result->size() > maximum) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           std::string(field) + " is outside protocol bounds");
  }
  return *result;
}

template <typename Enum>
Enum requireEnum(std::optional<Enum> value, std::string_view field) {
  if (!value) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           std::string(field) + " contains an unknown value");
  }
  return *value;
}

std::vector<std::string> stringArray(const graphql::Json& value) {
  if (!value.ok() || value.isNull()) return {};
  if (!value.isArray()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Expected an array of strings");
  }
  std::vector<std::string> result;
  result.reserve(value.size());
  value.forEach([&](const graphql::Json& entry) {
    if (!entry.isString()) {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "Expected an array of strings");
    }
    result.push_back(entry.asString());
  });
  return result;
}

bool isRunEvent(AgentEventType type) {
  return type == AgentEventType::RunStarted ||
         type == AgentEventType::RunPaused ||
         type == AgentEventType::RunSucceeded ||
         type == AgentEventType::RunFailed ||
         type == AgentEventType::RunCancelled ||
         type == AgentEventType::RunPreempted;
}

bool isToolEvent(AgentEventType type) {
  return type == AgentEventType::ToolProposed ||
         type == AgentEventType::ToolDispatched ||
         type == AgentEventType::ToolSucceeded ||
         type == AgentEventType::ToolFailed ||
         type == AgentEventType::ToolDenied ||
         type == AgentEventType::ToolTimedOut ||
         type == AgentEventType::ToolOutcomeUnknown;
}

}  // namespace

#define CROWDY_ENUM_IMPL(Name, Values, ParseName)                       \
  std::string_view toString(Name value) { return printEnum(value, Values); } \
  std::optional<Name> ParseName(std::string_view value) {               \
    return parseEnum(value, Values);                                    \
  }

CROWDY_ENUM_IMPL(AgentMode, kModes, agentModeFromString)
CROWDY_ENUM_IMPL(AgentToolExecutor, kExecutors, agentToolExecutorFromString)
CROWDY_ENUM_IMPL(AgentToolRisk, kRisks, agentToolRiskFromString)
CROWDY_ENUM_IMPL(AgentApprovalPolicy, kApprovalPolicies,
                 agentApprovalPolicyFromString)
CROWDY_ENUM_IMPL(AgentIdempotencyClass, kIdempotency,
                 agentIdempotencyClassFromString)
CROWDY_ENUM_IMPL(AgentToolResultStatus, kResultStatuses,
                 agentToolResultStatusFromString)
CROWDY_ENUM_IMPL(AgentSessionStatus, kSessionStatuses,
                 agentSessionStatusFromString)
CROWDY_ENUM_IMPL(AgentRunStatus, kRunStatuses, agentRunStatusFromString)
CROWDY_ENUM_IMPL(AgentToolCallStatus, kToolStatuses,
                 agentToolCallStatusFromString)
CROWDY_ENUM_IMPL(AgentLeaseStatus, kLeaseStatuses,
                 agentLeaseStatusFromString)
CROWDY_ENUM_IMPL(AgentApprovalStatus, kApprovalStatuses,
                 agentApprovalStatusFromString)
CROWDY_ENUM_IMPL(AgentPreemptionReason, kPreemptions,
                 agentPreemptionReasonFromString)
CROWDY_ENUM_IMPL(AgentEventType, kEventTypes, agentEventTypeFromString)
CROWDY_ENUM_IMPL(AgentLeaseKind, kLeaseKinds, agentLeaseKindFromString)

#undef CROWDY_ENUM_IMPL

bool isNonNegativeSequence(std::string_view value) {
  if (value.empty() || value.size() > 40) return false;
  if (value.size() > 1 && value.front() == '0') return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isdigit(character) != 0;
  });
}

AgentError parseAgentError(const graphql::Json& value) {
  AgentError result;
  result.code = requiredString(value, "code", 128);
  result.message = sanitizeAgentText(requiredString(value, "message", 512));
  result.retryable = value["retryable"].asBool(false);
  result.remediation = optionalString(value, "remediation");
  result.field = optionalString(value, "field");
  result.requiredScope = optionalString(value, "requiredScope");
  return result;
}

AgentRun parseAgentRun(const graphql::Json& value) {
  AgentRun result;
  result.runId = requiredString(value, "runId", 128);
  result.status = requireEnum(
      agentRunStatusFromString(requiredString(value, "status", 32)),
      "run status");
  result.providerRounds =
      static_cast<int>(value["providerRounds"].asInt64(0));
  result.toolCalls = static_cast<int>(value["toolCalls"].asInt64(0));
  result.errorCode = optionalString(value, "errorCode");
  result.terminalReason = optionalString(value, "terminalReason");
  result.reason = optionalString(value, "reason");
  if (value["error"].ok() && !value["error"].isNull()) {
    result.error = parseAgentError(value["error"]);
  }
  result.startedAt = optionalString(value, "startedAt");
  result.finishedAt = optionalString(value, "finishedAt");
  result.createdAt = optionalString(value, "createdAt").value_or("");
  result.cancelled = value["cancelled"].asBool(false);
  return result;
}

AgentApproval parseAgentApproval(const graphql::Json& value,
                                 std::vector<std::string> reasons) {
  AgentApproval result;
  result.approvalId = requiredString(value, "approvalId", 128);
  result.toolCallId = requiredString(value, "toolCallId", 128);
  result.argumentHash = requiredString(value, "argumentHash", 71);
  result.status = requireEnum(
      agentApprovalStatusFromString(requiredString(value, "status", 32)),
      "approval status");
  result.safeSummary = requiredString(value, "safeSummary", 2'048);
  result.reasons = std::move(reasons);
  result.clientEpoch = optionalString(value, "clientEpoch");
  result.expiresAt = requiredString(value, "expiresAt", 40);
  result.approved = value["approved"].asBool(
      result.status == AgentApprovalStatus::Granted ||
      result.status == AgentApprovalStatus::Consumed);
  result.rejected = value["rejected"].asBool(
      result.status == AgentApprovalStatus::Denied);
  return result;
}

AgentLease parseAgentLease(const graphql::Json& value) {
  AgentLease result;
  result.leaseId = requiredString(value, "leaseId", 128);
  result.kind = requireEnum(
      agentLeaseKindFromString(requiredString(value, "kind", 16)),
      "lease kind");
  result.status = requireEnum(
      agentLeaseStatusFromString(requiredString(value, "status", 16)),
      "lease status");
  result.clientEpoch = requiredString(value, "clientEpoch", 40);
  result.scopes = stringArray(value["scopes"]);
  result.holder = requiredString(value, "holder", 256);
  result.controlledEntityId = optionalString(value, "controlledEntityId");
  result.hostCapabilityRevision =
      optionalString(value, "hostCapabilityRevision");
  result.expectedProjectRevision =
      optionalString(value, "expectedProjectRevision");
  result.contextVersion = requiredString(value, "contextVersion", 128);
  result.grantedAt = requiredString(value, "grantedAt", 40);
  result.expiresAt = requiredString(value, "expiresAt", 40);
  if (const auto reason = optionalString(value, "revokedReason")) {
    result.revokedReason = requireEnum(agentPreemptionReasonFromString(*reason),
                                       "lease revoked reason");
  }
  return result;
}

AgentBudget parseAgentBudget(const graphql::Json& value) {
  AgentBudget result;
  const auto dimensions = value["dimensions"];
  if (!dimensions.isArray()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "budget dimensions must be an array");
  }
  dimensions.forEach([&](const graphql::Json& dimension) {
    const auto name = requiredString(dimension, "name", 32);
    const auto scope = requiredString(dimension, "scope", 16);
    static constexpr std::array<std::string_view, 9> names = {
        "REQUESTS",        "INPUT_TOKENS", "OUTPUT_TOKENS",
        "REASONING_TOKENS", "PROVIDER_COST", "TOOL_ROUNDS",
        "WALL_CLOCK_MS",   "TOOL_CALLS",   "COMPILES"};
    static constexpr std::array<std::string_view, 3> scopes = {
        "TURN", "SESSION", "PLAYER_DAY"};
    if (std::find(names.begin(), names.end(), name) == names.end() ||
        std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "budget contains an unknown dimension");
    }
    result.dimensions.push_back(
        AgentBudgetDimension{name,
                             scope,
                             requiredString(dimension, "limit", 40),
                             requiredString(dimension, "reserved", 40),
                             requiredString(dimension, "consumed", 40),
                             requiredString(dimension, "remaining", 40),
                             requiredString(dimension, "unit", 40)});
  });
  result.resetAt = optionalString(value, "resetAt");
  result.platformFunded = value["platformFunded"].asBool(false);
  result.payer = requiredString(value, "payer", 16);
  if (result.payer != "PLATFORM" && result.payer != "APP" &&
      result.payer != "USER") {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "budget payer contains an unknown value");
  }
  return result;
}

AgentSession parseAgentSession(const graphql::Json& value) {
  AgentSession result;
  result.contractVersion = requiredString(value, "contractVersion", 64);
  if (result.contractVersion != "crowdy.studio-agent/1") {
    throw CrowdyAgentError("AGENT_CONTEXT_STALE",
                           "Unsupported agent session contract");
  }
  result.sessionId = requiredString(value, "sessionId", 128);
  result.appId = requiredString(value, "appId", 40);
  result.projectId = optionalString(value, "projectId");
  result.gridId = optionalString(value, "gridId");
  result.mode = requireEnum(
      agentModeFromString(requiredString(value, "mode", 16)), "session mode");
  result.status = requireEnum(
      agentSessionStatusFromString(requiredString(value, "status", 16)),
      "session status");
  result.requestedModel = requiredString(value, "requestedModel", 256);
  result.model = optionalString(value, "model");
  result.resolvedModel = optionalString(value, "resolvedModel");
  result.providerDataConsent = value["providerDataConsent"].asBool(false);
  result.registryDigest = requiredString(value, "registryDigest", 71);
  result.providerPolicyVersion =
      requiredString(value, "providerPolicyVersion", 128);
  result.appPolicyVersion =
      requiredString(value, "appPolicyVersion", 128);
  result.contextVersion = requiredString(value, "contextVersion", 128);
  result.hostCapabilityRevision =
      optionalString(value, "hostCapabilityRevision");
  result.currentClientEpoch =
      requiredString(value, "currentClientEpoch", 40);
  result.clientEpoch = optionalString(value, "clientEpoch");
  result.lastEventSeq = requiredString(value, "lastEventSeq", 40);
  if (value["currentRun"].ok() && !value["currentRun"].isNull()) {
    result.currentRun = parseAgentRun(value["currentRun"]);
  }
  if (!value["activeLeases"].isArray()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "activeLeases must be an array");
  }
  value["activeLeases"].forEach([&](const graphql::Json& lease) {
    result.activeLeases.push_back(parseAgentLease(lease));
  });
  if (value["pendingApproval"].ok() &&
      !value["pendingApproval"].isNull()) {
    result.pendingApproval = parseAgentApproval(value["pendingApproval"]);
  }
  result.createdAt = requiredString(value, "createdAt", 40);
  result.updatedAt = requiredString(value, "updatedAt", 40);
  result.closedAt = optionalString(value, "closedAt");
  if (!isNonNegativeSequence(result.currentClientEpoch) ||
      !isNonNegativeSequence(result.lastEventSeq) ||
      (result.clientEpoch && !isNonNegativeSequence(*result.clientEpoch))) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "session sequence fields are invalid");
  }
  return result;
}

AgentToolResult parseAgentToolResult(const graphql::Json& value) {
  AgentToolResult result;
  result.protocolVersion = requiredString(value, "protocolVersion", 64);
  result.toolCallId = requiredString(value, "toolCallId", 128);
  result.status = requireEnum(
      agentToolResultStatusFromString(requiredString(value, "status", 32)),
      "tool result status");
  result.outputJson = optionalString(value, "outputJson");
  if (value["error"].ok() && !value["error"].isNull()) {
    result.error = parseAgentError(value["error"]);
  }
  result.observedContextVersion =
      requiredString(value, "observedContextVersion", 128);
  result.startedAt = requiredString(value, "startedAt", 40);
  result.finishedAt = requiredString(value, "finishedAt", 40);
  return result;
}

AgentToolInvocation parseAgentToolInvocation(const graphql::Json& value) {
  AgentToolInvocation result;
  result.protocolVersion = requiredString(value, "protocolVersion", 64);
  result.sessionId = requiredString(value, "sessionId", 128);
  result.runId = requiredString(value, "runId", 128);
  result.toolCallId = requiredString(value, "toolCallId", 128);
  result.name = requiredString(value, "name", 160);
  result.version = requiredString(value, "version", 32);
  result.descriptorDigest =
      requiredString(value, "descriptorDigest", 71);
  result.argumentsJson =
      requiredString(value, "argumentsJson", 1'048'576);
  result.arguments = graphql::Json::parse(result.argumentsJson);
  if (!result.arguments.isObject()) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "argumentsJson must contain one object");
  }
  result.argumentHash = requiredString(value, "argumentHash", 71);
  result.contextVersion = requiredString(value, "contextVersion", 128);
  result.clientEpoch = optionalString(value, "clientEpoch");
  result.leaseId = optionalString(value, "leaseId");
  result.approvalGrant = optionalString(value, "approvalGrant");
  result.idempotencyKey = optionalString(value, "idempotencyKey");
  result.deadline = requiredString(value, "deadline", 40);
  return result;
}

AgentEvent parseAgentEvent(const graphql::Json& value) {
  AgentEvent event;
  event.protocolVersion = requiredString(value, "protocolVersion", 64);
  if (event.protocolVersion != "crowdy.agent-event/1") {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Unsupported agent event protocol");
  }
  event.eventId = requiredString(value, "eventId", 128);
  event.sessionId = requiredString(value, "sessionId", 128);
  event.seq = requiredString(value, "seq", 40);
  if (!isNonNegativeSequence(event.seq)) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "event seq must be non-negative decimal");
  }
  event.type = requireEnum(
      agentEventTypeFromString(requiredString(value, "type", 64)),
      "event type");
  event.runId = optionalString(value, "runId");
  event.version = requiredString(value, "version", 64);
  event.createdAt = requiredString(value, "createdAt", 40);

  const auto typenameValue = requiredString(value, "__typename", 64);
  if (typenameValue == "AgentLifecycleEvent") {
    AgentLifecycleEventPayload payload;
    if (const auto mode = optionalString(value, "lifecycleMode")) {
      payload.mode =
          requireEnum(agentModeFromString(*mode), "lifecycle mode");
    }
    payload.clientEpoch = optionalString(value, "lifecycleClientEpoch");
    payload.replayAfterSeq =
        optionalString(value, "lifecycleReplayAfterSeq");
    payload.reason = optionalString(value, "lifecycleReason");
    payload.contextVersion =
        optionalString(value, "lifecycleContextVersion");
    if (event.type == AgentEventType::SessionPaused) {
      payload.status = AgentSessionStatus::Paused;
    } else if (event.type == AgentEventType::SessionResumed ||
               event.type == AgentEventType::SessionCreated) {
      payload.status = AgentSessionStatus::Active;
    } else if (event.type == AgentEventType::SessionClosed) {
      payload.status = AgentSessionStatus::Closed;
    }
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentMessageEvent") {
    AgentMessageEventPayload payload;
    payload.message.messageId =
        requiredString(value, "messageEventId", 128);
    payload.message.role = requiredString(value, "messageRole", 16);
    if (payload.message.role != "USER" &&
        payload.message.role != "ASSISTANT") {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "message role contains an unknown value");
    }
    if (!value["messageContent"].isString()) {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "message content must be a string");
    }
    payload.message.content = value["messageContent"].asString();
    if (payload.message.content.size() > 65'536) {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "message content is outside bounds");
    }
    payload.message.runId = event.runId;
    payload.message.createdAt = event.createdAt;
    payload.chunk = event.type == AgentEventType::AssistantChunk;
    payload.chunkRunId = event.runId;
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentRunEvent" && isRunEvent(event.type)) {
    AgentRunEventPayload payload;
    payload.run.runId =
        event.runId.value_or(requiredString(value, "runId", 128));
    payload.run.status = requireEnum(
        agentRunStatusFromString(requiredString(value, "runStatus", 32)),
        "run status");
    payload.run.errorCode = optionalString(value, "runCode");
    payload.run.reason = optionalString(value, "runReason");
    if (value["runError"].ok() && !value["runError"].isNull()) {
      payload.run.error = parseAgentError(value["runError"]);
    }
    payload.run.createdAt = event.createdAt;
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentToolEvent" && isToolEvent(event.type)) {
    AgentToolEventPayload payload;
    payload.toolCallId =
        requiredString(value, "toolEventCallId", 128);
    payload.name = requiredString(value, "toolEventName", 160);
    payload.version = requiredString(value, "toolEventVersion", 32);
    payload.status = requireEnum(
        agentToolCallStatusFromString(requiredString(value, "toolStatus", 32)),
        "tool status");
    payload.safeSummary = optionalString(value, "toolSafeSummary");
    payload.argumentHash = optionalString(value, "toolArgumentHash");
    if (value["toolInvocation"].ok() &&
        !value["toolInvocation"].isNull()) {
      payload.invocation =
          parseAgentToolInvocation(value["toolInvocation"]);
    }
    if (value["toolResult"].ok() && !value["toolResult"].isNull()) {
      payload.result = parseAgentToolResult(value["toolResult"]);
    }
    if (value["toolError"].ok() && !value["toolError"].isNull()) {
      payload.error = parseAgentError(value["toolError"]);
    }
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentApprovalEvent") {
    AgentApprovalEventPayload payload;
    payload.approval.approvalId =
        requiredString(value, "approvalEventId", 128);
    payload.approval.toolCallId =
        requiredString(value, "approvalToolCallId", 128);
    payload.approval.argumentHash =
        requiredString(value, "approvalArgumentHash", 71);
    payload.approval.status = requireEnum(
        agentApprovalStatusFromString(
            requiredString(value, "approvalStatus", 32)),
        "approval status");
    payload.approval.safeSummary =
        requiredString(value, "approvalSafeSummary", 2'048);
    payload.approval.reasons = stringArray(value["approvalReasons"]);
    payload.approval.expiresAt =
        requiredString(value, "approvalExpiresAt", 40);
    payload.approval.approved =
        payload.approval.status == AgentApprovalStatus::Granted ||
        payload.approval.status == AgentApprovalStatus::Consumed;
    payload.approval.rejected =
        payload.approval.status == AgentApprovalStatus::Denied;
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentLeaseEvent") {
    AgentLeaseEventPayload payload;
    payload.lease.leaseId = requiredString(value, "leaseEventId", 128);
    payload.lease.kind = requireEnum(
        agentLeaseKindFromString(requiredString(value, "leaseKind", 16)),
        "lease kind");
    payload.lease.status = requireEnum(
        agentLeaseStatusFromString(requiredString(value, "leaseStatus", 16)),
        "lease status");
    payload.lease.clientEpoch =
        requiredString(value, "leaseClientEpoch", 40);
    payload.lease.scopes = stringArray(value["leaseScopes"]);
    payload.lease.holder = requiredString(value, "leaseHolder", 256);
    payload.lease.contextVersion =
        requiredString(value, "leaseContextVersion", 128);
    payload.lease.controlledEntityId =
        optionalString(value, "leaseControlledEntityId");
    payload.lease.hostCapabilityRevision =
        optionalString(value, "leaseHostCapabilityRevision");
    payload.lease.expectedProjectRevision =
        optionalString(value, "leaseExpectedProjectRevision");
    payload.lease.grantedAt =
        requiredString(value, "leaseGrantedAt", 40);
    payload.lease.expiresAt =
        requiredString(value, "leaseExpiresAt", 40);
    if (const auto reason = optionalString(value, "leaseReason")) {
      payload.lease.revokedReason =
          requireEnum(agentPreemptionReasonFromString(*reason),
                      "lease reason");
    }
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentCheckpointEvent") {
    AgentCheckpointEventPayload payload;
    payload.checkpoint.checkpointId =
        requiredString(value, "checkpointEventId", 128);
    payload.checkpoint.projectRevision =
        requiredString(value, "checkpointProjectRevision", 40);
    payload.checkpoint.contentHash =
        requiredString(value, "checkpointContentHash", 71);
    payload.checkpoint.reason =
        requiredString(value, "checkpointReason", 32);
    if (payload.checkpoint.reason != "AGENT_WRITE" &&
        payload.checkpoint.reason != "RESTORE_PREIMAGE" &&
        payload.checkpoint.reason != "MANUAL") {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "checkpoint reason contains an unknown value");
    }
    const auto files = value["checkpointFiles"];
    if (!files.isArray() || files.size() > 128) {
      throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                             "checkpoint files are outside bounds");
    }
    files.forEach([&](const graphql::Json& file) {
      const auto target = requiredString(file, "target", 16);
      if (target != "SERVER" && target != "CLIENT") {
        throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                               "checkpoint target contains an unknown value");
      }
      payload.checkpoint.files.push_back(
          AgentCheckpointFile{target,
                              requiredString(file, "path", 256),
                              requiredString(file, "contentHash", 71),
                              static_cast<int>(
                                  file["byteLength"].asInt64(0))});
    });
    payload.checkpoint.createdAt = event.createdAt;
    payload.checkpoint.restoredAt =
        optionalString(value, "checkpointRestoredAt");
    event.payload = std::move(payload);
  } else if (typenameValue == "AgentBudgetEvent") {
    AgentBudgetEventPayload payload;
    payload.budget = parseAgentBudget(value["budgetSnapshot"]);
    event.payload = std::move(payload);
  } else {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "Event typename does not match its type");
  }
  return event;
}

AgentConnection<AgentSession> parseAgentSessionConnection(
    const graphql::Json& value) {
  AgentConnection<AgentSession> result;
  const auto edges = value["edges"];
  if (!edges.isArray() || edges.size() > 50) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "session connection edges are outside bounds");
  }
  edges.forEach([&](const graphql::Json& edge) {
    result.edges.push_back(
        AgentEdge<AgentSession>{requiredString(edge, "cursor", 256),
                                parseAgentSession(edge["node"])});
  });
  const auto pageInfo = value["pageInfo"];
  result.pageInfo.hasNextPage = pageInfo["hasNextPage"].asBool(false);
  result.pageInfo.endCursor = optionalString(pageInfo, "endCursor");
  result.nodes.reserve(result.edges.size());
  for (const auto& edge : result.edges) result.nodes.push_back(edge.node);
  result.endCursor = optionalString(value, "endCursor");
  result.hasNextPage = value["hasNextPage"].asBool(
      result.pageInfo.hasNextPage);
  return result;
}

AgentHistoryPage parseAgentHistoryPage(const graphql::Json& value) {
  AgentHistoryPage result;
  const auto edges = value["edges"];
  if (!edges.isArray() || edges.size() > 200) {
    throw CrowdyAgentError("AGENT_EVENT_CURSOR_INVALID",
                           "history edges are outside bounds");
  }
  edges.forEach([&](const graphql::Json& edge) {
    result.edges.push_back(
        AgentEdge<AgentEvent>{requiredString(edge, "cursor", 40),
                              parseAgentEvent(edge["node"])});
  });
  const auto pageInfo = value["pageInfo"];
  result.pageInfo.hasNextPage = pageInfo["hasNextPage"].asBool(false);
  result.pageInfo.endCursor = optionalString(pageInfo, "endCursor");
  result.events.reserve(result.edges.size());
  for (const auto& edge : result.edges) result.events.push_back(edge.node);
  result.hasMore = value["hasMore"].asBool(result.pageInfo.hasNextPage);
  return result;
}

}  // namespace crowdy::agent
