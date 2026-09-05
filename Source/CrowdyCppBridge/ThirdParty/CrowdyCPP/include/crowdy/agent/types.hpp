#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "crowdy/agent/errors.hpp"
#include "crowdy/graphql/json.hpp"

namespace crowdy::agent {

enum class AgentMode { Ask, Build, Play };
enum class AgentToolExecutor { Server, Browser };
enum class AgentToolRisk {
  ReadOnly,
  RoutineWrite,
  WorldControl,
  Destructive,
  TrustConsent,
  Economic,
  Irreversible
};
enum class AgentApprovalPolicy { None, Required, Conditional };
enum class AgentIdempotencyClass {
  Pure,
  Keyed,
  ToolCallOnce,
  NonRetryable
};
enum class AgentToolResultStatus {
  Succeeded,
  Failed,
  Cancelled,
  TimedOut,
  OutcomeUnknown
};
enum class AgentSessionStatus { Active, Paused, Closed, Revoked };
enum class AgentRunStatus {
  Queued,
  Running,
  WaitingForTool,
  WaitingForApproval,
  Paused,
  Succeeded,
  Failed,
  Cancelled,
  Preempted
};
enum class AgentToolCallStatus {
  Proposed,
  WaitingForApproval,
  Dispatched,
  Running,
  Succeeded,
  Failed,
  Denied,
  TimedOut,
  Cancelled,
  Stale,
  OutcomeUnknown
};
enum class AgentLeaseStatus { Active, Revoked, Expired };
enum class AgentApprovalStatus {
  Pending,
  Granted,
  Denied,
  Consumed,
  Revoked,
  Expired
};
enum class AgentPreemptionReason {
  HumanInput,
  HumanEdit,
  HumanStop,
  Escape,
  Death,
  ContextChanged,
  PermissionChanged,
  AdmissionChanged,
  ControlTargetChanged,
  Disconnected,
  ClientReattached,
  QuotaFailure,
  BudgetFailure,
  OperatorKill,
  LeaseExpired,
  SessionClosed
};
enum class AgentEventType {
  SessionCreated,
  SessionPaused,
  SessionResumed,
  SessionClosed,
  ClientAttached,
  ClientDetached,
  ModeSelected,
  UserMessage,
  RunStarted,
  AssistantChunk,
  AssistantMessage,
  ToolProposed,
  ToolDispatched,
  ToolSucceeded,
  ToolFailed,
  ToolDenied,
  ToolTimedOut,
  ToolOutcomeUnknown,
  ApprovalRequested,
  ApprovalGranted,
  ApprovalDenied,
  ApprovalConsumed,
  ApprovalExpired,
  CheckpointCreated,
  CheckpointRestored,
  LeaseGranted,
  LeaseRevoked,
  LeaseExpired,
  ContextChanged,
  BudgetUpdated,
  RunPaused,
  RunSucceeded,
  RunFailed,
  RunCancelled,
  RunPreempted
};

inline constexpr std::array<std::string_view, 16>
    kAgentPreemptionReasons = {
        "HUMAN_INPUT",          "HUMAN_EDIT",
        "HUMAN_STOP",           "ESCAPE",
        "DEATH",                "CONTEXT_CHANGED",
        "PERMISSION_CHANGED",   "ADMISSION_CHANGED",
        "CONTROL_TARGET_CHANGED", "DISCONNECTED",
        "CLIENT_REATTACHED",    "QUOTA_FAILURE",
        "BUDGET_FAILURE",       "OPERATOR_KILL",
        "LEASE_EXPIRED",        "SESSION_CLOSED",
};

std::string_view toString(AgentMode value);
std::string_view toString(AgentToolExecutor value);
std::string_view toString(AgentToolRisk value);
std::string_view toString(AgentApprovalPolicy value);
std::string_view toString(AgentIdempotencyClass value);
std::string_view toString(AgentToolResultStatus value);
std::string_view toString(AgentSessionStatus value);
std::string_view toString(AgentRunStatus value);
std::string_view toString(AgentToolCallStatus value);
std::string_view toString(AgentLeaseStatus value);
std::string_view toString(AgentApprovalStatus value);
std::string_view toString(AgentPreemptionReason value);
std::string_view toString(AgentEventType value);

std::optional<AgentMode> agentModeFromString(std::string_view value);
std::optional<AgentToolExecutor> agentToolExecutorFromString(
    std::string_view value);
std::optional<AgentToolRisk> agentToolRiskFromString(std::string_view value);
std::optional<AgentApprovalPolicy> agentApprovalPolicyFromString(
    std::string_view value);
std::optional<AgentIdempotencyClass> agentIdempotencyClassFromString(
    std::string_view value);
std::optional<AgentToolResultStatus> agentToolResultStatusFromString(
    std::string_view value);
std::optional<AgentSessionStatus> agentSessionStatusFromString(
    std::string_view value);
std::optional<AgentRunStatus> agentRunStatusFromString(std::string_view value);
std::optional<AgentToolCallStatus> agentToolCallStatusFromString(
    std::string_view value);
std::optional<AgentLeaseStatus> agentLeaseStatusFromString(
    std::string_view value);
std::optional<AgentApprovalStatus> agentApprovalStatusFromString(
    std::string_view value);
std::optional<AgentPreemptionReason> agentPreemptionReasonFromString(
    std::string_view value);
std::optional<AgentEventType> agentEventTypeFromString(std::string_view value);

struct AgentToolInvocation {
  std::string protocolVersion = "crowdy.tool-call/1";
  std::string sessionId;
  std::string runId;
  std::string toolCallId;
  std::string name;
  std::string version;
  std::string descriptorDigest;
  graphql::Json arguments;
  std::string argumentsJson;
  std::string argumentHash;
  std::string contextVersion;
  std::optional<std::string> clientEpoch;
  std::optional<std::string> leaseId;
  std::optional<std::string> approvalGrant;
  std::optional<std::string> idempotencyKey;
  std::string deadline;
};

struct AgentToolResult {
  std::string protocolVersion = "crowdy.tool-result/1";
  std::string toolCallId;
  AgentToolResultStatus status = AgentToolResultStatus::Failed;
  std::optional<std::string> outputJson;
  std::optional<AgentError> error;
  std::string observedContextVersion;
  std::string startedAt;
  std::string finishedAt;
};

struct AgentRun {
  std::string runId;
  AgentRunStatus status = AgentRunStatus::Queued;
  int providerRounds = 0;
  int toolCalls = 0;
  std::optional<std::string> errorCode;
  std::optional<std::string> terminalReason;
  std::optional<std::string> reason;
  std::optional<AgentError> error;
  std::optional<std::string> startedAt;
  std::optional<std::string> finishedAt;
  std::string createdAt;
  bool cancelled = false;
};

struct AgentApproval {
  std::string approvalId;
  std::string toolCallId;
  std::string argumentHash;
  AgentApprovalStatus status = AgentApprovalStatus::Pending;
  std::string safeSummary;
  std::vector<std::string> reasons;
  std::optional<std::string> clientEpoch;
  std::string expiresAt;
  bool approved = false;
  bool rejected = false;
};

enum class AgentLeaseKind { Workspace, Play };
std::string_view toString(AgentLeaseKind value);
std::optional<AgentLeaseKind> agentLeaseKindFromString(std::string_view value);

struct AgentLease {
  std::string leaseId;
  AgentLeaseKind kind = AgentLeaseKind::Workspace;
  AgentLeaseStatus status = AgentLeaseStatus::Active;
  std::string clientEpoch;
  std::vector<std::string> scopes;
  std::string holder;
  std::optional<std::string> controlledEntityId;
  std::optional<std::string> hostCapabilityRevision;
  std::optional<std::string> expectedProjectRevision;
  std::string contextVersion;
  std::string grantedAt;
  std::string expiresAt;
  std::optional<AgentPreemptionReason> revokedReason;
};

struct AgentCheckpointFile {
  std::string target;
  std::string path;
  std::string contentHash;
  int byteLength = 0;
};

struct AgentCheckpoint {
  std::string checkpointId;
  std::string projectRevision;
  std::string contentHash;
  std::string reason;
  std::vector<AgentCheckpointFile> files;
  std::string createdAt;
  std::optional<std::string> restoredAt;
};

struct AgentBudgetDimension {
  std::string name;
  std::string scope;
  std::string limit;
  std::string reserved;
  std::string consumed;
  std::string remaining;
  std::string unit;
};

struct AgentBudget {
  std::vector<AgentBudgetDimension> dimensions;
  std::optional<std::string> resetAt;
  bool platformFunded = false;
  std::string payer;
};

struct AgentHeartbeat {
  std::string serverTime;
  std::optional<std::string> playLeaseFreshUntil;
  std::optional<std::string> workspaceLeaseExpiresAt;
};

struct AgentToolCallAck {
  std::string toolCallId;
  std::string toolName;
  AgentToolCallStatus status = AgentToolCallStatus::Failed;
  std::string argumentHash;
  std::optional<AgentError> error;
  bool accepted = false;
};

struct AgentMessage {
  std::string messageId;
  std::string role;
  std::string content;
  std::optional<std::string> runId;
  std::string createdAt;
};

struct AgentSession {
  std::string contractVersion = "crowdy.studio-agent/1";
  std::string sessionId;
  std::string appId;
  std::optional<std::string> projectId;
  std::optional<std::string> gridId;
  AgentMode mode = AgentMode::Ask;
  AgentSessionStatus status = AgentSessionStatus::Active;
  std::string requestedModel;
  std::optional<std::string> model;
  std::optional<std::string> resolvedModel;
  bool providerDataConsent = false;
  std::string registryDigest;
  std::string providerPolicyVersion;
  std::string appPolicyVersion;
  std::string contextVersion;
  std::optional<std::string> hostCapabilityRevision;
  std::string currentClientEpoch;
  std::optional<std::string> clientEpoch;
  std::string lastEventSeq;
  std::optional<AgentRun> currentRun;
  std::vector<AgentLease> activeLeases;
  std::optional<AgentApproval> pendingApproval;
  std::string createdAt;
  std::string updatedAt;
  std::optional<std::string> closedAt;
};

struct AgentLifecycleEventPayload {
  std::optional<AgentMode> mode;
  std::optional<std::string> clientEpoch;
  std::optional<std::string> replayAfterSeq;
  std::optional<std::string> reason;
  std::optional<std::string> contextVersion;
  std::optional<AgentSessionStatus> status;
};
struct AgentMessageEventPayload {
  AgentMessage message;
  std::optional<std::string> chunkRunId;
  bool chunk = false;
};
struct AgentRunEventPayload {
  AgentRun run;
};
struct AgentToolEventPayload {
  std::string toolCallId;
  std::string name;
  std::string version;
  AgentToolCallStatus status = AgentToolCallStatus::Proposed;
  std::optional<std::string> safeSummary;
  std::optional<std::string> argumentHash;
  std::optional<AgentToolInvocation> invocation;
  std::optional<AgentToolResult> result;
  std::optional<AgentError> error;
};
struct AgentApprovalEventPayload {
  AgentApproval approval;
};
struct AgentLeaseEventPayload {
  AgentLease lease;
};
struct AgentCheckpointEventPayload {
  AgentCheckpoint checkpoint;
};
struct AgentBudgetEventPayload {
  AgentBudget budget;
};

using AgentEventPayload =
    std::variant<AgentLifecycleEventPayload, AgentMessageEventPayload,
                 AgentRunEventPayload, AgentToolEventPayload,
                 AgentApprovalEventPayload, AgentLeaseEventPayload,
                 AgentCheckpointEventPayload, AgentBudgetEventPayload>;

struct AgentEvent {
  std::string protocolVersion = "crowdy.agent-event/1";
  std::string eventId;
  std::string sessionId;
  std::string seq;
  AgentEventType type = AgentEventType::SessionCreated;
  std::optional<std::string> runId;
  std::string version;
  std::string createdAt;
  AgentEventPayload payload = AgentLifecycleEventPayload{};
};

template <typename T>
struct AgentEdge {
  std::string cursor;
  T node;
};

struct AgentPageInfo {
  bool hasNextPage = false;
  std::optional<std::string> endCursor;
};

template <typename T>
struct AgentConnection {
  std::vector<AgentEdge<T>> edges;
  AgentPageInfo pageInfo;
  std::vector<T> nodes;
  std::optional<std::string> endCursor;
  bool hasNextPage = false;
};

struct AgentHistoryPage {
  std::vector<AgentEdge<AgentEvent>> edges;
  AgentPageInfo pageInfo;
  std::vector<AgentEvent> events;
  bool hasMore = false;
};

struct AgentToolTimelineItem {
  std::string toolCallId;
  std::string name;
  std::string version;
  AgentToolCallStatus status = AgentToolCallStatus::Proposed;
  std::optional<AgentToolRisk> risk;
  std::optional<std::string> safeSummary;
  std::optional<std::string> argumentHash;
  std::optional<AgentToolResult> result;
  std::optional<AgentError> error;
  std::string updatedAt;
};

struct AgentClientAttachment {
  AgentSession session;
  std::string clientEpoch;
  std::string replayAfterSeq;
};

AgentError parseAgentError(const graphql::Json& value);
AgentRun parseAgentRun(const graphql::Json& value);
AgentApproval parseAgentApproval(const graphql::Json& value,
                                 std::vector<std::string> reasons = {});
AgentLease parseAgentLease(const graphql::Json& value);
AgentBudget parseAgentBudget(const graphql::Json& value);
AgentSession parseAgentSession(const graphql::Json& value);
AgentToolResult parseAgentToolResult(const graphql::Json& value);
AgentToolInvocation parseAgentToolInvocation(const graphql::Json& value);
AgentEvent parseAgentEvent(const graphql::Json& value);
AgentConnection<AgentSession> parseAgentSessionConnection(
    const graphql::Json& value);
AgentHistoryPage parseAgentHistoryPage(const graphql::Json& value);

bool isNonNegativeSequence(std::string_view value);

}  // namespace crowdy::agent
