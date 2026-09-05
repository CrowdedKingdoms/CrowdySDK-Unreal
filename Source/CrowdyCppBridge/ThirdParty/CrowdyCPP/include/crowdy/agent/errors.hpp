#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "crowdy/graphql/errors.hpp"

namespace crowdy::agent {

inline constexpr std::array<std::string_view, 51> kAgentErrorCodes = {
    "AGENT_DISABLED",
    "AGENT_UNAUTHENTICATED",
    "AGENT_PERMISSION_DENIED",
    "AGENT_SCOPE_DENIED",
    "AGENT_CONTEXT_CHANGED",
    "AGENT_CONTEXT_STALE",
    "AGENT_SESSION_NOT_FOUND",
    "AGENT_SESSION_CLOSED",
    "AGENT_RUN_ALREADY_ACTIVE",
    "AGENT_RUN_NOT_ACTIVE",
    "AGENT_CANCELLED",
    "AGENT_PREEMPTED",
    "AGENT_OPERATOR_KILLED",
    "AGENT_DISCONNECTED",
    "AGENT_CLIENT_REATTACHED",
    "AGENT_CLIENT_EPOCH_STALE",
    "AGENT_EVENT_CURSOR_INVALID",
    "AGENT_EVENT_GAP",
    "AGENT_MODEL_NOT_ALLOWED",
    "AGENT_PROVIDER_POLICY_UNSATISFIED",
    "AGENT_PROVIDER_UNAVAILABLE",
    "AGENT_PROVIDER_OUTPUT_INVALID",
    "AGENT_PROVIDER_USAGE_UNAVAILABLE",
    "AGENT_BUDGET_EXHAUSTED",
    "AGENT_QUOTA_EXHAUSTED",
    "AGENT_RATE_LIMITED",
    "AGENT_TOOL_UNKNOWN",
    "AGENT_TOOL_VERSION_UNSUPPORTED",
    "AGENT_TOOL_INPUT_INVALID",
    "AGENT_TOOL_OUTPUT_INVALID",
    "AGENT_TOOL_DESCRIPTOR_INVALID",
    "AGENT_TOOL_FAILED",
    "AGENT_TOOL_TIMEOUT",
    "AGENT_TOOL_OUTCOME_UNKNOWN",
    "AGENT_HOST_UNAVAILABLE",
    "AGENT_HOST_CAPABILITY_CHANGED",
    "AGENT_OBSERVATION_STALE",
    "AGENT_CONTROL_TARGET_CHANGED",
    "AGENT_PARALLEL_TOOL_CALLS_UNSUPPORTED",
    "AGENT_APPROVAL_REQUIRED",
    "AGENT_APPROVAL_MISMATCH",
    "AGENT_APPROVAL_EXPIRED",
    "AGENT_APPROVAL_DENIED",
    "AGENT_APPROVAL_REVOKED",
    "AGENT_LEASE_REQUIRED",
    "AGENT_LEASE_EXPIRED",
    "AGENT_LEASE_REVOKED",
    "AGENT_LEASE_SCOPE_MISSING",
    "AGENT_IDEMPOTENCY_CONFLICT",
    "AGENT_CHECKPOINT_NOT_FOUND",
    "CROWDY_STUDIO_REVISION_CONFLICT",
};

bool isAgentErrorCode(std::string_view code);
std::string sanitizeAgentText(std::string_view value);

/// Stable safe error envelope carried by every agent boundary.
struct AgentError {
  std::string code;
  std::string message;
  bool retryable = false;
  std::optional<std::string> remediation;
  std::optional<std::string> field;
  std::optional<std::string> requiredScope;
};

class CrowdyAgentError : public graphql::CrowdyError {
 public:
  CrowdyAgentError(std::string code, std::string message,
                   bool retryable = false,
                   std::optional<std::string> remediation = std::nullopt,
                   std::optional<std::string> field = std::nullopt,
                   std::optional<std::string> requiredScope = std::nullopt);

  bool retryable() const { return retryable_; }
  const std::optional<std::string>& remediation() const {
    return remediation_;
  }
  const std::optional<std::string>& field() const { return field_; }
  const std::optional<std::string>& requiredScope() const {
    return requiredScope_;
  }
  AgentError value() const;

 private:
  bool retryable_;
  std::optional<std::string> remediation_;
  std::optional<std::string> field_;
  std::optional<std::string> requiredScope_;
};

/// Marks an effect whose acknowledgement was lost. Dispatchers must return
/// OUTCOME_UNKNOWN and must never retry it.
class CrowdyAgentOutcomeUnknownError final : public CrowdyAgentError {
 public:
  explicit CrowdyAgentOutcomeUnknownError(
      std::string message =
          "The tool effect may have occurred; inspect current state");
};

AgentError toAgentError(
    const std::exception& error,
    std::string_view fallbackCode = "AGENT_TOOL_FAILED");
AgentError makeAgentError(
    std::string_view code, std::string_view message, bool retryable = false);

}  // namespace crowdy::agent
