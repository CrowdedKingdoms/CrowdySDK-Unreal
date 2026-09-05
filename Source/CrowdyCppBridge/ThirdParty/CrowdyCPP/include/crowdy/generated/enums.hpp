// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDL at https://docs.crowdedkingdoms.com/schema/game-api.graphql).
// schema.gql sha256: c44743c28bfc216017905e65b49cdacabc3d352102aef76dc2d3e88358d38f86
// operations sha256: aace4a3e955d5e4a1f6af1852e917079f9c09b1ede47d90a495807e9f3850a71

#pragma once

#include <optional>
#include <string_view>

/// GraphQL enums from the published schema. Values keep their wire spelling;
/// toString/fromString convert between the enum and the GraphQL string.
namespace crowdy::gen {

enum class AppDeploymentTarget {
  NONE,
  SHARED,
  DEDICATED,
};

inline constexpr std::string_view toString(AppDeploymentTarget v) {
  switch (v) {
    case AppDeploymentTarget::NONE: return "NONE";
    case AppDeploymentTarget::SHARED: return "SHARED";
    case AppDeploymentTarget::DEDICATED: return "DEDICATED";
  }
  return "";
}

inline std::optional<AppDeploymentTarget> appDeploymentTargetFromString(std::string_view s) {
  if (s == "NONE") return AppDeploymentTarget::NONE;
  if (s == "SHARED") return AppDeploymentTarget::SHARED;
  if (s == "DEDICATED") return AppDeploymentTarget::DEDICATED;
  return std::nullopt;
}

enum class AppRuntimeStatus {
  ACTIVE,
  GRACE,
  DENIED,
  SUSPENDED,
};

inline constexpr std::string_view toString(AppRuntimeStatus v) {
  switch (v) {
    case AppRuntimeStatus::ACTIVE: return "ACTIVE";
    case AppRuntimeStatus::GRACE: return "GRACE";
    case AppRuntimeStatus::DENIED: return "DENIED";
    case AppRuntimeStatus::SUSPENDED: return "SUSPENDED";
  }
  return "";
}

inline std::optional<AppRuntimeStatus> appRuntimeStatusFromString(std::string_view s) {
  if (s == "ACTIVE") return AppRuntimeStatus::ACTIVE;
  if (s == "GRACE") return AppRuntimeStatus::GRACE;
  if (s == "DENIED") return AppRuntimeStatus::DENIED;
  if (s == "SUSPENDED") return AppRuntimeStatus::SUSPENDED;
  return std::nullopt;
}

enum class AppStatus {
  DRAFT,
  LIVE,
  ARCHIVED,
};

inline constexpr std::string_view toString(AppStatus v) {
  switch (v) {
    case AppStatus::DRAFT: return "DRAFT";
    case AppStatus::LIVE: return "LIVE";
    case AppStatus::ARCHIVED: return "ARCHIVED";
  }
  return "";
}

inline std::optional<AppStatus> appStatusFromString(std::string_view s) {
  if (s == "DRAFT") return AppStatus::DRAFT;
  if (s == "LIVE") return AppStatus::LIVE;
  if (s == "ARCHIVED") return AppStatus::ARCHIVED;
  return std::nullopt;
}

enum class AppVisibility {
  PUBLIC,
  UNLISTED,
  PRIVATE,
};

inline constexpr std::string_view toString(AppVisibility v) {
  switch (v) {
    case AppVisibility::PUBLIC: return "PUBLIC";
    case AppVisibility::UNLISTED: return "UNLISTED";
    case AppVisibility::PRIVATE: return "PRIVATE";
  }
  return "";
}

inline std::optional<AppVisibility> appVisibilityFromString(std::string_view s) {
  if (s == "PUBLIC") return AppVisibility::PUBLIC;
  if (s == "UNLISTED") return AppVisibility::UNLISTED;
  if (s == "PRIVATE") return AppVisibility::PRIVATE;
  return std::nullopt;
}

enum class CheckoutPurpose {
  ORG_WALLET_TOPUP,
  PLAYER_WALLET_TOPUP,
  APP_ACCESS_PURCHASE,
};

inline constexpr std::string_view toString(CheckoutPurpose v) {
  switch (v) {
    case CheckoutPurpose::ORG_WALLET_TOPUP: return "ORG_WALLET_TOPUP";
    case CheckoutPurpose::PLAYER_WALLET_TOPUP: return "PLAYER_WALLET_TOPUP";
    case CheckoutPurpose::APP_ACCESS_PURCHASE: return "APP_ACCESS_PURCHASE";
  }
  return "";
}

inline std::optional<CheckoutPurpose> checkoutPurposeFromString(std::string_view s) {
  if (s == "ORG_WALLET_TOPUP") return CheckoutPurpose::ORG_WALLET_TOPUP;
  if (s == "PLAYER_WALLET_TOPUP") return CheckoutPurpose::PLAYER_WALLET_TOPUP;
  if (s == "APP_ACCESS_PURCHASE") return CheckoutPurpose::APP_ACCESS_PURCHASE;
  return std::nullopt;
}

enum class CheckoutStatus {
  PENDING,
  COMPLETED,
  FAILED,
  EXPIRED,
  CANCELED,
};

inline constexpr std::string_view toString(CheckoutStatus v) {
  switch (v) {
    case CheckoutStatus::PENDING: return "PENDING";
    case CheckoutStatus::COMPLETED: return "COMPLETED";
    case CheckoutStatus::FAILED: return "FAILED";
    case CheckoutStatus::EXPIRED: return "EXPIRED";
    case CheckoutStatus::CANCELED: return "CANCELED";
  }
  return "";
}

inline std::optional<CheckoutStatus> checkoutStatusFromString(std::string_view s) {
  if (s == "PENDING") return CheckoutStatus::PENDING;
  if (s == "COMPLETED") return CheckoutStatus::COMPLETED;
  if (s == "FAILED") return CheckoutStatus::FAILED;
  if (s == "EXPIRED") return CheckoutStatus::EXPIRED;
  if (s == "CANCELED") return CheckoutStatus::CANCELED;
  return std::nullopt;
}

enum class CodeAdmissionMode {
  IMPLICIT_ALLOW,
  ALLOW_LIST,
};

inline constexpr std::string_view toString(CodeAdmissionMode v) {
  switch (v) {
    case CodeAdmissionMode::IMPLICIT_ALLOW: return "IMPLICIT_ALLOW";
    case CodeAdmissionMode::ALLOW_LIST: return "ALLOW_LIST";
  }
  return "";
}

inline std::optional<CodeAdmissionMode> codeAdmissionModeFromString(std::string_view s) {
  if (s == "IMPLICIT_ALLOW") return CodeAdmissionMode::IMPLICIT_ALLOW;
  if (s == "ALLOW_LIST") return CodeAdmissionMode::ALLOW_LIST;
  return std::nullopt;
}

enum class CodeAdmissionSubjectKind {
  CODE,
  AUTHOR,
  ORG,
};

inline constexpr std::string_view toString(CodeAdmissionSubjectKind v) {
  switch (v) {
    case CodeAdmissionSubjectKind::CODE: return "CODE";
    case CodeAdmissionSubjectKind::AUTHOR: return "AUTHOR";
    case CodeAdmissionSubjectKind::ORG: return "ORG";
  }
  return "";
}

inline std::optional<CodeAdmissionSubjectKind> codeAdmissionSubjectKindFromString(std::string_view s) {
  if (s == "CODE") return CodeAdmissionSubjectKind::CODE;
  if (s == "AUTHOR") return CodeAdmissionSubjectKind::AUTHOR;
  if (s == "ORG") return CodeAdmissionSubjectKind::ORG;
  return std::nullopt;
}

enum class CrowdyStudioAgentApprovalStatus {
  PENDING,
  GRANTED,
  DENIED,
  CONSUMED,
  REVOKED,
  EXPIRED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentApprovalStatus v) {
  switch (v) {
    case CrowdyStudioAgentApprovalStatus::PENDING: return "PENDING";
    case CrowdyStudioAgentApprovalStatus::GRANTED: return "GRANTED";
    case CrowdyStudioAgentApprovalStatus::DENIED: return "DENIED";
    case CrowdyStudioAgentApprovalStatus::CONSUMED: return "CONSUMED";
    case CrowdyStudioAgentApprovalStatus::REVOKED: return "REVOKED";
    case CrowdyStudioAgentApprovalStatus::EXPIRED: return "EXPIRED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentApprovalStatus> crowdyStudioAgentApprovalStatusFromString(std::string_view s) {
  if (s == "PENDING") return CrowdyStudioAgentApprovalStatus::PENDING;
  if (s == "GRANTED") return CrowdyStudioAgentApprovalStatus::GRANTED;
  if (s == "DENIED") return CrowdyStudioAgentApprovalStatus::DENIED;
  if (s == "CONSUMED") return CrowdyStudioAgentApprovalStatus::CONSUMED;
  if (s == "REVOKED") return CrowdyStudioAgentApprovalStatus::REVOKED;
  if (s == "EXPIRED") return CrowdyStudioAgentApprovalStatus::EXPIRED;
  return std::nullopt;
}

enum class CrowdyStudioAgentEventType {
  SESSION_CREATED,
  SESSION_PAUSED,
  SESSION_RESUMED,
  SESSION_CLOSED,
  CLIENT_ATTACHED,
  CLIENT_DETACHED,
  MODE_SELECTED,
  USER_MESSAGE,
  RUN_STARTED,
  ASSISTANT_CHUNK,
  ASSISTANT_MESSAGE,
  TOOL_PROPOSED,
  TOOL_DISPATCHED,
  TOOL_SUCCEEDED,
  TOOL_FAILED,
  TOOL_DENIED,
  TOOL_TIMED_OUT,
  TOOL_OUTCOME_UNKNOWN,
  APPROVAL_REQUESTED,
  APPROVAL_GRANTED,
  APPROVAL_DENIED,
  APPROVAL_CONSUMED,
  APPROVAL_EXPIRED,
  CHECKPOINT_CREATED,
  CHECKPOINT_RESTORED,
  LEASE_GRANTED,
  LEASE_REVOKED,
  LEASE_EXPIRED,
  CONTEXT_CHANGED,
  BUDGET_UPDATED,
  RUN_PAUSED,
  RUN_SUCCEEDED,
  RUN_FAILED,
  RUN_CANCELLED,
  RUN_PREEMPTED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentEventType v) {
  switch (v) {
    case CrowdyStudioAgentEventType::SESSION_CREATED: return "SESSION_CREATED";
    case CrowdyStudioAgentEventType::SESSION_PAUSED: return "SESSION_PAUSED";
    case CrowdyStudioAgentEventType::SESSION_RESUMED: return "SESSION_RESUMED";
    case CrowdyStudioAgentEventType::SESSION_CLOSED: return "SESSION_CLOSED";
    case CrowdyStudioAgentEventType::CLIENT_ATTACHED: return "CLIENT_ATTACHED";
    case CrowdyStudioAgentEventType::CLIENT_DETACHED: return "CLIENT_DETACHED";
    case CrowdyStudioAgentEventType::MODE_SELECTED: return "MODE_SELECTED";
    case CrowdyStudioAgentEventType::USER_MESSAGE: return "USER_MESSAGE";
    case CrowdyStudioAgentEventType::RUN_STARTED: return "RUN_STARTED";
    case CrowdyStudioAgentEventType::ASSISTANT_CHUNK: return "ASSISTANT_CHUNK";
    case CrowdyStudioAgentEventType::ASSISTANT_MESSAGE: return "ASSISTANT_MESSAGE";
    case CrowdyStudioAgentEventType::TOOL_PROPOSED: return "TOOL_PROPOSED";
    case CrowdyStudioAgentEventType::TOOL_DISPATCHED: return "TOOL_DISPATCHED";
    case CrowdyStudioAgentEventType::TOOL_SUCCEEDED: return "TOOL_SUCCEEDED";
    case CrowdyStudioAgentEventType::TOOL_FAILED: return "TOOL_FAILED";
    case CrowdyStudioAgentEventType::TOOL_DENIED: return "TOOL_DENIED";
    case CrowdyStudioAgentEventType::TOOL_TIMED_OUT: return "TOOL_TIMED_OUT";
    case CrowdyStudioAgentEventType::TOOL_OUTCOME_UNKNOWN: return "TOOL_OUTCOME_UNKNOWN";
    case CrowdyStudioAgentEventType::APPROVAL_REQUESTED: return "APPROVAL_REQUESTED";
    case CrowdyStudioAgentEventType::APPROVAL_GRANTED: return "APPROVAL_GRANTED";
    case CrowdyStudioAgentEventType::APPROVAL_DENIED: return "APPROVAL_DENIED";
    case CrowdyStudioAgentEventType::APPROVAL_CONSUMED: return "APPROVAL_CONSUMED";
    case CrowdyStudioAgentEventType::APPROVAL_EXPIRED: return "APPROVAL_EXPIRED";
    case CrowdyStudioAgentEventType::CHECKPOINT_CREATED: return "CHECKPOINT_CREATED";
    case CrowdyStudioAgentEventType::CHECKPOINT_RESTORED: return "CHECKPOINT_RESTORED";
    case CrowdyStudioAgentEventType::LEASE_GRANTED: return "LEASE_GRANTED";
    case CrowdyStudioAgentEventType::LEASE_REVOKED: return "LEASE_REVOKED";
    case CrowdyStudioAgentEventType::LEASE_EXPIRED: return "LEASE_EXPIRED";
    case CrowdyStudioAgentEventType::CONTEXT_CHANGED: return "CONTEXT_CHANGED";
    case CrowdyStudioAgentEventType::BUDGET_UPDATED: return "BUDGET_UPDATED";
    case CrowdyStudioAgentEventType::RUN_PAUSED: return "RUN_PAUSED";
    case CrowdyStudioAgentEventType::RUN_SUCCEEDED: return "RUN_SUCCEEDED";
    case CrowdyStudioAgentEventType::RUN_FAILED: return "RUN_FAILED";
    case CrowdyStudioAgentEventType::RUN_CANCELLED: return "RUN_CANCELLED";
    case CrowdyStudioAgentEventType::RUN_PREEMPTED: return "RUN_PREEMPTED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentEventType> crowdyStudioAgentEventTypeFromString(std::string_view s) {
  if (s == "SESSION_CREATED") return CrowdyStudioAgentEventType::SESSION_CREATED;
  if (s == "SESSION_PAUSED") return CrowdyStudioAgentEventType::SESSION_PAUSED;
  if (s == "SESSION_RESUMED") return CrowdyStudioAgentEventType::SESSION_RESUMED;
  if (s == "SESSION_CLOSED") return CrowdyStudioAgentEventType::SESSION_CLOSED;
  if (s == "CLIENT_ATTACHED") return CrowdyStudioAgentEventType::CLIENT_ATTACHED;
  if (s == "CLIENT_DETACHED") return CrowdyStudioAgentEventType::CLIENT_DETACHED;
  if (s == "MODE_SELECTED") return CrowdyStudioAgentEventType::MODE_SELECTED;
  if (s == "USER_MESSAGE") return CrowdyStudioAgentEventType::USER_MESSAGE;
  if (s == "RUN_STARTED") return CrowdyStudioAgentEventType::RUN_STARTED;
  if (s == "ASSISTANT_CHUNK") return CrowdyStudioAgentEventType::ASSISTANT_CHUNK;
  if (s == "ASSISTANT_MESSAGE") return CrowdyStudioAgentEventType::ASSISTANT_MESSAGE;
  if (s == "TOOL_PROPOSED") return CrowdyStudioAgentEventType::TOOL_PROPOSED;
  if (s == "TOOL_DISPATCHED") return CrowdyStudioAgentEventType::TOOL_DISPATCHED;
  if (s == "TOOL_SUCCEEDED") return CrowdyStudioAgentEventType::TOOL_SUCCEEDED;
  if (s == "TOOL_FAILED") return CrowdyStudioAgentEventType::TOOL_FAILED;
  if (s == "TOOL_DENIED") return CrowdyStudioAgentEventType::TOOL_DENIED;
  if (s == "TOOL_TIMED_OUT") return CrowdyStudioAgentEventType::TOOL_TIMED_OUT;
  if (s == "TOOL_OUTCOME_UNKNOWN") return CrowdyStudioAgentEventType::TOOL_OUTCOME_UNKNOWN;
  if (s == "APPROVAL_REQUESTED") return CrowdyStudioAgentEventType::APPROVAL_REQUESTED;
  if (s == "APPROVAL_GRANTED") return CrowdyStudioAgentEventType::APPROVAL_GRANTED;
  if (s == "APPROVAL_DENIED") return CrowdyStudioAgentEventType::APPROVAL_DENIED;
  if (s == "APPROVAL_CONSUMED") return CrowdyStudioAgentEventType::APPROVAL_CONSUMED;
  if (s == "APPROVAL_EXPIRED") return CrowdyStudioAgentEventType::APPROVAL_EXPIRED;
  if (s == "CHECKPOINT_CREATED") return CrowdyStudioAgentEventType::CHECKPOINT_CREATED;
  if (s == "CHECKPOINT_RESTORED") return CrowdyStudioAgentEventType::CHECKPOINT_RESTORED;
  if (s == "LEASE_GRANTED") return CrowdyStudioAgentEventType::LEASE_GRANTED;
  if (s == "LEASE_REVOKED") return CrowdyStudioAgentEventType::LEASE_REVOKED;
  if (s == "LEASE_EXPIRED") return CrowdyStudioAgentEventType::LEASE_EXPIRED;
  if (s == "CONTEXT_CHANGED") return CrowdyStudioAgentEventType::CONTEXT_CHANGED;
  if (s == "BUDGET_UPDATED") return CrowdyStudioAgentEventType::BUDGET_UPDATED;
  if (s == "RUN_PAUSED") return CrowdyStudioAgentEventType::RUN_PAUSED;
  if (s == "RUN_SUCCEEDED") return CrowdyStudioAgentEventType::RUN_SUCCEEDED;
  if (s == "RUN_FAILED") return CrowdyStudioAgentEventType::RUN_FAILED;
  if (s == "RUN_CANCELLED") return CrowdyStudioAgentEventType::RUN_CANCELLED;
  if (s == "RUN_PREEMPTED") return CrowdyStudioAgentEventType::RUN_PREEMPTED;
  return std::nullopt;
}

enum class CrowdyStudioAgentLeaseStatus {
  ACTIVE,
  REVOKED,
  EXPIRED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentLeaseStatus v) {
  switch (v) {
    case CrowdyStudioAgentLeaseStatus::ACTIVE: return "ACTIVE";
    case CrowdyStudioAgentLeaseStatus::REVOKED: return "REVOKED";
    case CrowdyStudioAgentLeaseStatus::EXPIRED: return "EXPIRED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentLeaseStatus> crowdyStudioAgentLeaseStatusFromString(std::string_view s) {
  if (s == "ACTIVE") return CrowdyStudioAgentLeaseStatus::ACTIVE;
  if (s == "REVOKED") return CrowdyStudioAgentLeaseStatus::REVOKED;
  if (s == "EXPIRED") return CrowdyStudioAgentLeaseStatus::EXPIRED;
  return std::nullopt;
}

enum class CrowdyStudioAgentLeaseType {
  WORKSPACE,
  PLAY,
};

inline constexpr std::string_view toString(CrowdyStudioAgentLeaseType v) {
  switch (v) {
    case CrowdyStudioAgentLeaseType::WORKSPACE: return "WORKSPACE";
    case CrowdyStudioAgentLeaseType::PLAY: return "PLAY";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentLeaseType> crowdyStudioAgentLeaseTypeFromString(std::string_view s) {
  if (s == "WORKSPACE") return CrowdyStudioAgentLeaseType::WORKSPACE;
  if (s == "PLAY") return CrowdyStudioAgentLeaseType::PLAY;
  return std::nullopt;
}

enum class CrowdyStudioAgentMode {
  ASK,
  BUILD,
  PLAY,
};

inline constexpr std::string_view toString(CrowdyStudioAgentMode v) {
  switch (v) {
    case CrowdyStudioAgentMode::ASK: return "ASK";
    case CrowdyStudioAgentMode::BUILD: return "BUILD";
    case CrowdyStudioAgentMode::PLAY: return "PLAY";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentMode> crowdyStudioAgentModeFromString(std::string_view s) {
  if (s == "ASK") return CrowdyStudioAgentMode::ASK;
  if (s == "BUILD") return CrowdyStudioAgentMode::BUILD;
  if (s == "PLAY") return CrowdyStudioAgentMode::PLAY;
  return std::nullopt;
}

enum class CrowdyStudioAgentPolicyKind {
  PLATFORM,
  APP,
  EFFECTIVE,
};

inline constexpr std::string_view toString(CrowdyStudioAgentPolicyKind v) {
  switch (v) {
    case CrowdyStudioAgentPolicyKind::PLATFORM: return "PLATFORM";
    case CrowdyStudioAgentPolicyKind::APP: return "APP";
    case CrowdyStudioAgentPolicyKind::EFFECTIVE: return "EFFECTIVE";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentPolicyKind> crowdyStudioAgentPolicyKindFromString(std::string_view s) {
  if (s == "PLATFORM") return CrowdyStudioAgentPolicyKind::PLATFORM;
  if (s == "APP") return CrowdyStudioAgentPolicyKind::APP;
  if (s == "EFFECTIVE") return CrowdyStudioAgentPolicyKind::EFFECTIVE;
  return std::nullopt;
}

enum class CrowdyStudioAgentPreemptionReason {
  HUMAN_INPUT,
  HUMAN_EDIT,
  HUMAN_STOP,
  ESCAPE,
  DEATH,
  CONTEXT_CHANGED,
  PERMISSION_CHANGED,
  ADMISSION_CHANGED,
  CONTROL_TARGET_CHANGED,
  DISCONNECTED,
  CLIENT_REATTACHED,
  QUOTA_FAILURE,
  BUDGET_FAILURE,
  OPERATOR_KILL,
  LEASE_EXPIRED,
  SESSION_CLOSED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentPreemptionReason v) {
  switch (v) {
    case CrowdyStudioAgentPreemptionReason::HUMAN_INPUT: return "HUMAN_INPUT";
    case CrowdyStudioAgentPreemptionReason::HUMAN_EDIT: return "HUMAN_EDIT";
    case CrowdyStudioAgentPreemptionReason::HUMAN_STOP: return "HUMAN_STOP";
    case CrowdyStudioAgentPreemptionReason::ESCAPE: return "ESCAPE";
    case CrowdyStudioAgentPreemptionReason::DEATH: return "DEATH";
    case CrowdyStudioAgentPreemptionReason::CONTEXT_CHANGED: return "CONTEXT_CHANGED";
    case CrowdyStudioAgentPreemptionReason::PERMISSION_CHANGED: return "PERMISSION_CHANGED";
    case CrowdyStudioAgentPreemptionReason::ADMISSION_CHANGED: return "ADMISSION_CHANGED";
    case CrowdyStudioAgentPreemptionReason::CONTROL_TARGET_CHANGED: return "CONTROL_TARGET_CHANGED";
    case CrowdyStudioAgentPreemptionReason::DISCONNECTED: return "DISCONNECTED";
    case CrowdyStudioAgentPreemptionReason::CLIENT_REATTACHED: return "CLIENT_REATTACHED";
    case CrowdyStudioAgentPreemptionReason::QUOTA_FAILURE: return "QUOTA_FAILURE";
    case CrowdyStudioAgentPreemptionReason::BUDGET_FAILURE: return "BUDGET_FAILURE";
    case CrowdyStudioAgentPreemptionReason::OPERATOR_KILL: return "OPERATOR_KILL";
    case CrowdyStudioAgentPreemptionReason::LEASE_EXPIRED: return "LEASE_EXPIRED";
    case CrowdyStudioAgentPreemptionReason::SESSION_CLOSED: return "SESSION_CLOSED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentPreemptionReason> crowdyStudioAgentPreemptionReasonFromString(std::string_view s) {
  if (s == "HUMAN_INPUT") return CrowdyStudioAgentPreemptionReason::HUMAN_INPUT;
  if (s == "HUMAN_EDIT") return CrowdyStudioAgentPreemptionReason::HUMAN_EDIT;
  if (s == "HUMAN_STOP") return CrowdyStudioAgentPreemptionReason::HUMAN_STOP;
  if (s == "ESCAPE") return CrowdyStudioAgentPreemptionReason::ESCAPE;
  if (s == "DEATH") return CrowdyStudioAgentPreemptionReason::DEATH;
  if (s == "CONTEXT_CHANGED") return CrowdyStudioAgentPreemptionReason::CONTEXT_CHANGED;
  if (s == "PERMISSION_CHANGED") return CrowdyStudioAgentPreemptionReason::PERMISSION_CHANGED;
  if (s == "ADMISSION_CHANGED") return CrowdyStudioAgentPreemptionReason::ADMISSION_CHANGED;
  if (s == "CONTROL_TARGET_CHANGED") return CrowdyStudioAgentPreemptionReason::CONTROL_TARGET_CHANGED;
  if (s == "DISCONNECTED") return CrowdyStudioAgentPreemptionReason::DISCONNECTED;
  if (s == "CLIENT_REATTACHED") return CrowdyStudioAgentPreemptionReason::CLIENT_REATTACHED;
  if (s == "QUOTA_FAILURE") return CrowdyStudioAgentPreemptionReason::QUOTA_FAILURE;
  if (s == "BUDGET_FAILURE") return CrowdyStudioAgentPreemptionReason::BUDGET_FAILURE;
  if (s == "OPERATOR_KILL") return CrowdyStudioAgentPreemptionReason::OPERATOR_KILL;
  if (s == "LEASE_EXPIRED") return CrowdyStudioAgentPreemptionReason::LEASE_EXPIRED;
  if (s == "SESSION_CLOSED") return CrowdyStudioAgentPreemptionReason::SESSION_CLOSED;
  return std::nullopt;
}

enum class CrowdyStudioAgentRiskClass {
  READ_ONLY,
  ROUTINE_WRITE,
  WORLD_CONTROL,
  DESTRUCTIVE,
  TRUST_CONSENT,
  ECONOMIC,
  IRREVERSIBLE,
};

inline constexpr std::string_view toString(CrowdyStudioAgentRiskClass v) {
  switch (v) {
    case CrowdyStudioAgentRiskClass::READ_ONLY: return "READ_ONLY";
    case CrowdyStudioAgentRiskClass::ROUTINE_WRITE: return "ROUTINE_WRITE";
    case CrowdyStudioAgentRiskClass::WORLD_CONTROL: return "WORLD_CONTROL";
    case CrowdyStudioAgentRiskClass::DESTRUCTIVE: return "DESTRUCTIVE";
    case CrowdyStudioAgentRiskClass::TRUST_CONSENT: return "TRUST_CONSENT";
    case CrowdyStudioAgentRiskClass::ECONOMIC: return "ECONOMIC";
    case CrowdyStudioAgentRiskClass::IRREVERSIBLE: return "IRREVERSIBLE";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentRiskClass> crowdyStudioAgentRiskClassFromString(std::string_view s) {
  if (s == "READ_ONLY") return CrowdyStudioAgentRiskClass::READ_ONLY;
  if (s == "ROUTINE_WRITE") return CrowdyStudioAgentRiskClass::ROUTINE_WRITE;
  if (s == "WORLD_CONTROL") return CrowdyStudioAgentRiskClass::WORLD_CONTROL;
  if (s == "DESTRUCTIVE") return CrowdyStudioAgentRiskClass::DESTRUCTIVE;
  if (s == "TRUST_CONSENT") return CrowdyStudioAgentRiskClass::TRUST_CONSENT;
  if (s == "ECONOMIC") return CrowdyStudioAgentRiskClass::ECONOMIC;
  if (s == "IRREVERSIBLE") return CrowdyStudioAgentRiskClass::IRREVERSIBLE;
  return std::nullopt;
}

enum class CrowdyStudioAgentRunStatus {
  QUEUED,
  RUNNING,
  WAITING_FOR_TOOL,
  WAITING_FOR_APPROVAL,
  PAUSED,
  SUCCEEDED,
  FAILED,
  CANCELLED,
  PREEMPTED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentRunStatus v) {
  switch (v) {
    case CrowdyStudioAgentRunStatus::QUEUED: return "QUEUED";
    case CrowdyStudioAgentRunStatus::RUNNING: return "RUNNING";
    case CrowdyStudioAgentRunStatus::WAITING_FOR_TOOL: return "WAITING_FOR_TOOL";
    case CrowdyStudioAgentRunStatus::WAITING_FOR_APPROVAL: return "WAITING_FOR_APPROVAL";
    case CrowdyStudioAgentRunStatus::PAUSED: return "PAUSED";
    case CrowdyStudioAgentRunStatus::SUCCEEDED: return "SUCCEEDED";
    case CrowdyStudioAgentRunStatus::FAILED: return "FAILED";
    case CrowdyStudioAgentRunStatus::CANCELLED: return "CANCELLED";
    case CrowdyStudioAgentRunStatus::PREEMPTED: return "PREEMPTED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentRunStatus> crowdyStudioAgentRunStatusFromString(std::string_view s) {
  if (s == "QUEUED") return CrowdyStudioAgentRunStatus::QUEUED;
  if (s == "RUNNING") return CrowdyStudioAgentRunStatus::RUNNING;
  if (s == "WAITING_FOR_TOOL") return CrowdyStudioAgentRunStatus::WAITING_FOR_TOOL;
  if (s == "WAITING_FOR_APPROVAL") return CrowdyStudioAgentRunStatus::WAITING_FOR_APPROVAL;
  if (s == "PAUSED") return CrowdyStudioAgentRunStatus::PAUSED;
  if (s == "SUCCEEDED") return CrowdyStudioAgentRunStatus::SUCCEEDED;
  if (s == "FAILED") return CrowdyStudioAgentRunStatus::FAILED;
  if (s == "CANCELLED") return CrowdyStudioAgentRunStatus::CANCELLED;
  if (s == "PREEMPTED") return CrowdyStudioAgentRunStatus::PREEMPTED;
  return std::nullopt;
}

enum class CrowdyStudioAgentSessionStatus {
  ACTIVE,
  PAUSED,
  CLOSED,
  REVOKED,
};

inline constexpr std::string_view toString(CrowdyStudioAgentSessionStatus v) {
  switch (v) {
    case CrowdyStudioAgentSessionStatus::ACTIVE: return "ACTIVE";
    case CrowdyStudioAgentSessionStatus::PAUSED: return "PAUSED";
    case CrowdyStudioAgentSessionStatus::CLOSED: return "CLOSED";
    case CrowdyStudioAgentSessionStatus::REVOKED: return "REVOKED";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentSessionStatus> crowdyStudioAgentSessionStatusFromString(std::string_view s) {
  if (s == "ACTIVE") return CrowdyStudioAgentSessionStatus::ACTIVE;
  if (s == "PAUSED") return CrowdyStudioAgentSessionStatus::PAUSED;
  if (s == "CLOSED") return CrowdyStudioAgentSessionStatus::CLOSED;
  if (s == "REVOKED") return CrowdyStudioAgentSessionStatus::REVOKED;
  return std::nullopt;
}

enum class CrowdyStudioAgentToolCallStatus {
  PROPOSED,
  WAITING_FOR_APPROVAL,
  DISPATCHED,
  RUNNING,
  SUCCEEDED,
  FAILED,
  DENIED,
  TIMED_OUT,
  CANCELLED,
  STALE,
  OUTCOME_UNKNOWN,
};

inline constexpr std::string_view toString(CrowdyStudioAgentToolCallStatus v) {
  switch (v) {
    case CrowdyStudioAgentToolCallStatus::PROPOSED: return "PROPOSED";
    case CrowdyStudioAgentToolCallStatus::WAITING_FOR_APPROVAL: return "WAITING_FOR_APPROVAL";
    case CrowdyStudioAgentToolCallStatus::DISPATCHED: return "DISPATCHED";
    case CrowdyStudioAgentToolCallStatus::RUNNING: return "RUNNING";
    case CrowdyStudioAgentToolCallStatus::SUCCEEDED: return "SUCCEEDED";
    case CrowdyStudioAgentToolCallStatus::FAILED: return "FAILED";
    case CrowdyStudioAgentToolCallStatus::DENIED: return "DENIED";
    case CrowdyStudioAgentToolCallStatus::TIMED_OUT: return "TIMED_OUT";
    case CrowdyStudioAgentToolCallStatus::CANCELLED: return "CANCELLED";
    case CrowdyStudioAgentToolCallStatus::STALE: return "STALE";
    case CrowdyStudioAgentToolCallStatus::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentToolCallStatus> crowdyStudioAgentToolCallStatusFromString(std::string_view s) {
  if (s == "PROPOSED") return CrowdyStudioAgentToolCallStatus::PROPOSED;
  if (s == "WAITING_FOR_APPROVAL") return CrowdyStudioAgentToolCallStatus::WAITING_FOR_APPROVAL;
  if (s == "DISPATCHED") return CrowdyStudioAgentToolCallStatus::DISPATCHED;
  if (s == "RUNNING") return CrowdyStudioAgentToolCallStatus::RUNNING;
  if (s == "SUCCEEDED") return CrowdyStudioAgentToolCallStatus::SUCCEEDED;
  if (s == "FAILED") return CrowdyStudioAgentToolCallStatus::FAILED;
  if (s == "DENIED") return CrowdyStudioAgentToolCallStatus::DENIED;
  if (s == "TIMED_OUT") return CrowdyStudioAgentToolCallStatus::TIMED_OUT;
  if (s == "CANCELLED") return CrowdyStudioAgentToolCallStatus::CANCELLED;
  if (s == "STALE") return CrowdyStudioAgentToolCallStatus::STALE;
  if (s == "OUTCOME_UNKNOWN") return CrowdyStudioAgentToolCallStatus::OUTCOME_UNKNOWN;
  return std::nullopt;
}

enum class CrowdyStudioAgentToolExecutor {
  SERVER,
  BROWSER,
};

inline constexpr std::string_view toString(CrowdyStudioAgentToolExecutor v) {
  switch (v) {
    case CrowdyStudioAgentToolExecutor::SERVER: return "SERVER";
    case CrowdyStudioAgentToolExecutor::BROWSER: return "BROWSER";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentToolExecutor> crowdyStudioAgentToolExecutorFromString(std::string_view s) {
  if (s == "SERVER") return CrowdyStudioAgentToolExecutor::SERVER;
  if (s == "BROWSER") return CrowdyStudioAgentToolExecutor::BROWSER;
  return std::nullopt;
}

enum class CrowdyStudioAgentToolResultStatus {
  SUCCEEDED,
  FAILED,
  CANCELLED,
  TIMED_OUT,
  OUTCOME_UNKNOWN,
};

inline constexpr std::string_view toString(CrowdyStudioAgentToolResultStatus v) {
  switch (v) {
    case CrowdyStudioAgentToolResultStatus::SUCCEEDED: return "SUCCEEDED";
    case CrowdyStudioAgentToolResultStatus::FAILED: return "FAILED";
    case CrowdyStudioAgentToolResultStatus::CANCELLED: return "CANCELLED";
    case CrowdyStudioAgentToolResultStatus::TIMED_OUT: return "TIMED_OUT";
    case CrowdyStudioAgentToolResultStatus::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentToolResultStatus> crowdyStudioAgentToolResultStatusFromString(std::string_view s) {
  if (s == "SUCCEEDED") return CrowdyStudioAgentToolResultStatus::SUCCEEDED;
  if (s == "FAILED") return CrowdyStudioAgentToolResultStatus::FAILED;
  if (s == "CANCELLED") return CrowdyStudioAgentToolResultStatus::CANCELLED;
  if (s == "TIMED_OUT") return CrowdyStudioAgentToolResultStatus::TIMED_OUT;
  if (s == "OUTCOME_UNKNOWN") return CrowdyStudioAgentToolResultStatus::OUTCOME_UNKNOWN;
  return std::nullopt;
}

enum class CrowdyStudioAgentToolRisk {
  READ_ONLY,
  ROUTINE_WRITE,
  WORLD_CONTROL,
  DESTRUCTIVE,
  TRUST_CONSENT,
  ECONOMIC,
  IRREVERSIBLE,
};

inline constexpr std::string_view toString(CrowdyStudioAgentToolRisk v) {
  switch (v) {
    case CrowdyStudioAgentToolRisk::READ_ONLY: return "READ_ONLY";
    case CrowdyStudioAgentToolRisk::ROUTINE_WRITE: return "ROUTINE_WRITE";
    case CrowdyStudioAgentToolRisk::WORLD_CONTROL: return "WORLD_CONTROL";
    case CrowdyStudioAgentToolRisk::DESTRUCTIVE: return "DESTRUCTIVE";
    case CrowdyStudioAgentToolRisk::TRUST_CONSENT: return "TRUST_CONSENT";
    case CrowdyStudioAgentToolRisk::ECONOMIC: return "ECONOMIC";
    case CrowdyStudioAgentToolRisk::IRREVERSIBLE: return "IRREVERSIBLE";
  }
  return "";
}

inline std::optional<CrowdyStudioAgentToolRisk> crowdyStudioAgentToolRiskFromString(std::string_view s) {
  if (s == "READ_ONLY") return CrowdyStudioAgentToolRisk::READ_ONLY;
  if (s == "ROUTINE_WRITE") return CrowdyStudioAgentToolRisk::ROUTINE_WRITE;
  if (s == "WORLD_CONTROL") return CrowdyStudioAgentToolRisk::WORLD_CONTROL;
  if (s == "DESTRUCTIVE") return CrowdyStudioAgentToolRisk::DESTRUCTIVE;
  if (s == "TRUST_CONSENT") return CrowdyStudioAgentToolRisk::TRUST_CONSENT;
  if (s == "ECONOMIC") return CrowdyStudioAgentToolRisk::ECONOMIC;
  if (s == "IRREVERSIBLE") return CrowdyStudioAgentToolRisk::IRREVERSIBLE;
  return std::nullopt;
}

enum class CrowdyStudioCommonStatus {
  DRAFT,
  PUBLISHED,
  ARCHIVED,
};

inline constexpr std::string_view toString(CrowdyStudioCommonStatus v) {
  switch (v) {
    case CrowdyStudioCommonStatus::DRAFT: return "DRAFT";
    case CrowdyStudioCommonStatus::PUBLISHED: return "PUBLISHED";
    case CrowdyStudioCommonStatus::ARCHIVED: return "ARCHIVED";
  }
  return "";
}

inline std::optional<CrowdyStudioCommonStatus> crowdyStudioCommonStatusFromString(std::string_view s) {
  if (s == "DRAFT") return CrowdyStudioCommonStatus::DRAFT;
  if (s == "PUBLISHED") return CrowdyStudioCommonStatus::PUBLISHED;
  if (s == "ARCHIVED") return CrowdyStudioCommonStatus::ARCHIVED;
  return std::nullopt;
}

enum class CrowdyStudioFileProvenance {
  AUTHORED,
  LIBRARY,
  COMMON,
};

inline constexpr std::string_view toString(CrowdyStudioFileProvenance v) {
  switch (v) {
    case CrowdyStudioFileProvenance::AUTHORED: return "AUTHORED";
    case CrowdyStudioFileProvenance::LIBRARY: return "LIBRARY";
    case CrowdyStudioFileProvenance::COMMON: return "COMMON";
  }
  return "";
}

inline std::optional<CrowdyStudioFileProvenance> crowdyStudioFileProvenanceFromString(std::string_view s) {
  if (s == "AUTHORED") return CrowdyStudioFileProvenance::AUTHORED;
  if (s == "LIBRARY") return CrowdyStudioFileProvenance::LIBRARY;
  if (s == "COMMON") return CrowdyStudioFileProvenance::COMMON;
  return std::nullopt;
}

enum class CrowdyStudioImportSource {
  LIBRARY,
  COMMON,
};

inline constexpr std::string_view toString(CrowdyStudioImportSource v) {
  switch (v) {
    case CrowdyStudioImportSource::LIBRARY: return "LIBRARY";
    case CrowdyStudioImportSource::COMMON: return "COMMON";
  }
  return "";
}

inline std::optional<CrowdyStudioImportSource> crowdyStudioImportSourceFromString(std::string_view s) {
  if (s == "LIBRARY") return CrowdyStudioImportSource::LIBRARY;
  if (s == "COMMON") return CrowdyStudioImportSource::COMMON;
  return std::nullopt;
}

enum class CrowdyStudioPairingPreference {
  PAIRED,
  INDEPENDENT,
  SERVER_ONLY,
  CLIENT_ONLY,
};

inline constexpr std::string_view toString(CrowdyStudioPairingPreference v) {
  switch (v) {
    case CrowdyStudioPairingPreference::PAIRED: return "PAIRED";
    case CrowdyStudioPairingPreference::INDEPENDENT: return "INDEPENDENT";
    case CrowdyStudioPairingPreference::SERVER_ONLY: return "SERVER_ONLY";
    case CrowdyStudioPairingPreference::CLIENT_ONLY: return "CLIENT_ONLY";
  }
  return "";
}

inline std::optional<CrowdyStudioPairingPreference> crowdyStudioPairingPreferenceFromString(std::string_view s) {
  if (s == "PAIRED") return CrowdyStudioPairingPreference::PAIRED;
  if (s == "INDEPENDENT") return CrowdyStudioPairingPreference::INDEPENDENT;
  if (s == "SERVER_ONLY") return CrowdyStudioPairingPreference::SERVER_ONLY;
  if (s == "CLIENT_ONLY") return CrowdyStudioPairingPreference::CLIENT_ONLY;
  return std::nullopt;
}

enum class CrowdyStudioTarget {
  SERVER,
  CLIENT,
};

inline constexpr std::string_view toString(CrowdyStudioTarget v) {
  switch (v) {
    case CrowdyStudioTarget::SERVER: return "SERVER";
    case CrowdyStudioTarget::CLIENT: return "CLIENT";
  }
  return "";
}

inline std::optional<CrowdyStudioTarget> crowdyStudioTargetFromString(std::string_view s) {
  if (s == "SERVER") return CrowdyStudioTarget::SERVER;
  if (s == "CLIENT") return CrowdyStudioTarget::CLIENT;
  return std::nullopt;
}

enum class DatacenterServingStatus {
  SERVING,
  NOT_SERVING,
  UNKNOWN,
};

inline constexpr std::string_view toString(DatacenterServingStatus v) {
  switch (v) {
    case DatacenterServingStatus::SERVING: return "SERVING";
    case DatacenterServingStatus::NOT_SERVING: return "NOT_SERVING";
    case DatacenterServingStatus::UNKNOWN: return "UNKNOWN";
  }
  return "";
}

inline std::optional<DatacenterServingStatus> datacenterServingStatusFromString(std::string_view s) {
  if (s == "SERVING") return DatacenterServingStatus::SERVING;
  if (s == "NOT_SERVING") return DatacenterServingStatus::NOT_SERVING;
  if (s == "UNKNOWN") return DatacenterServingStatus::UNKNOWN;
  return std::nullopt;
}

enum class GameModelPlayerCountStatus {
  FRESH,
  PARTIAL,
  UNAVAILABLE,
};

inline constexpr std::string_view toString(GameModelPlayerCountStatus v) {
  switch (v) {
    case GameModelPlayerCountStatus::FRESH: return "FRESH";
    case GameModelPlayerCountStatus::PARTIAL: return "PARTIAL";
    case GameModelPlayerCountStatus::UNAVAILABLE: return "UNAVAILABLE";
  }
  return "";
}

inline std::optional<GameModelPlayerCountStatus> gameModelPlayerCountStatusFromString(std::string_view s) {
  if (s == "FRESH") return GameModelPlayerCountStatus::FRESH;
  if (s == "PARTIAL") return GameModelPlayerCountStatus::PARTIAL;
  if (s == "UNAVAILABLE") return GameModelPlayerCountStatus::UNAVAILABLE;
  return std::nullopt;
}

enum class GmLintCode {
  CONTAINER_TYPE_UNDEFINED,
  APP_HAS_NO_CONTAINER_TYPES,
  FUNCTION_NOT_DEFINED,
  PROPERTY_NOT_DECLARED,
  PARAM_NOT_DECLARED,
  TIMER_TARGET_MISSING,
  TIMER_TARGET_NOT_AUTONOMOUS,
  PERMISSION_KEY_UNKNOWN,
  GRID_LITERAL_INVALID,
  AUTOMATION_TRIGGER_UNMATCHABLE,
  NOTIFICATION_CHANNEL_FOREIGN,
  NOTIFICATION_CHANNEL_UNKNOWN,
  FUNCTION_UNCOMPILABLE,
};

inline constexpr std::string_view toString(GmLintCode v) {
  switch (v) {
    case GmLintCode::CONTAINER_TYPE_UNDEFINED: return "CONTAINER_TYPE_UNDEFINED";
    case GmLintCode::APP_HAS_NO_CONTAINER_TYPES: return "APP_HAS_NO_CONTAINER_TYPES";
    case GmLintCode::FUNCTION_NOT_DEFINED: return "FUNCTION_NOT_DEFINED";
    case GmLintCode::PROPERTY_NOT_DECLARED: return "PROPERTY_NOT_DECLARED";
    case GmLintCode::PARAM_NOT_DECLARED: return "PARAM_NOT_DECLARED";
    case GmLintCode::TIMER_TARGET_MISSING: return "TIMER_TARGET_MISSING";
    case GmLintCode::TIMER_TARGET_NOT_AUTONOMOUS: return "TIMER_TARGET_NOT_AUTONOMOUS";
    case GmLintCode::PERMISSION_KEY_UNKNOWN: return "PERMISSION_KEY_UNKNOWN";
    case GmLintCode::GRID_LITERAL_INVALID: return "GRID_LITERAL_INVALID";
    case GmLintCode::AUTOMATION_TRIGGER_UNMATCHABLE: return "AUTOMATION_TRIGGER_UNMATCHABLE";
    case GmLintCode::NOTIFICATION_CHANNEL_FOREIGN: return "NOTIFICATION_CHANNEL_FOREIGN";
    case GmLintCode::NOTIFICATION_CHANNEL_UNKNOWN: return "NOTIFICATION_CHANNEL_UNKNOWN";
    case GmLintCode::FUNCTION_UNCOMPILABLE: return "FUNCTION_UNCOMPILABLE";
  }
  return "";
}

inline std::optional<GmLintCode> gmLintCodeFromString(std::string_view s) {
  if (s == "CONTAINER_TYPE_UNDEFINED") return GmLintCode::CONTAINER_TYPE_UNDEFINED;
  if (s == "APP_HAS_NO_CONTAINER_TYPES") return GmLintCode::APP_HAS_NO_CONTAINER_TYPES;
  if (s == "FUNCTION_NOT_DEFINED") return GmLintCode::FUNCTION_NOT_DEFINED;
  if (s == "PROPERTY_NOT_DECLARED") return GmLintCode::PROPERTY_NOT_DECLARED;
  if (s == "PARAM_NOT_DECLARED") return GmLintCode::PARAM_NOT_DECLARED;
  if (s == "TIMER_TARGET_MISSING") return GmLintCode::TIMER_TARGET_MISSING;
  if (s == "TIMER_TARGET_NOT_AUTONOMOUS") return GmLintCode::TIMER_TARGET_NOT_AUTONOMOUS;
  if (s == "PERMISSION_KEY_UNKNOWN") return GmLintCode::PERMISSION_KEY_UNKNOWN;
  if (s == "GRID_LITERAL_INVALID") return GmLintCode::GRID_LITERAL_INVALID;
  if (s == "AUTOMATION_TRIGGER_UNMATCHABLE") return GmLintCode::AUTOMATION_TRIGGER_UNMATCHABLE;
  if (s == "NOTIFICATION_CHANNEL_FOREIGN") return GmLintCode::NOTIFICATION_CHANNEL_FOREIGN;
  if (s == "NOTIFICATION_CHANNEL_UNKNOWN") return GmLintCode::NOTIFICATION_CHANNEL_UNKNOWN;
  if (s == "FUNCTION_UNCOMPILABLE") return GmLintCode::FUNCTION_UNCOMPILABLE;
  return std::nullopt;
}

enum class GmLintSeverity {
  ERROR,
  WARNING,
};

inline constexpr std::string_view toString(GmLintSeverity v) {
  switch (v) {
    case GmLintSeverity::ERROR: return "ERROR";
    case GmLintSeverity::WARNING: return "WARNING";
  }
  return "";
}

inline std::optional<GmLintSeverity> gmLintSeverityFromString(std::string_view s) {
  if (s == "ERROR") return GmLintSeverity::ERROR;
  if (s == "WARNING") return GmLintSeverity::WARNING;
  return std::nullopt;
}

enum class GmLintSubjectKind {
  APP,
  CONTAINER_TYPE,
  CONTAINER,
  FUNCTION,
  AUTOMATION,
};

inline constexpr std::string_view toString(GmLintSubjectKind v) {
  switch (v) {
    case GmLintSubjectKind::APP: return "APP";
    case GmLintSubjectKind::CONTAINER_TYPE: return "CONTAINER_TYPE";
    case GmLintSubjectKind::CONTAINER: return "CONTAINER";
    case GmLintSubjectKind::FUNCTION: return "FUNCTION";
    case GmLintSubjectKind::AUTOMATION: return "AUTOMATION";
  }
  return "";
}

inline std::optional<GmLintSubjectKind> gmLintSubjectKindFromString(std::string_view s) {
  if (s == "APP") return GmLintSubjectKind::APP;
  if (s == "CONTAINER_TYPE") return GmLintSubjectKind::CONTAINER_TYPE;
  if (s == "CONTAINER") return GmLintSubjectKind::CONTAINER;
  if (s == "FUNCTION") return GmLintSubjectKind::FUNCTION;
  if (s == "AUTOMATION") return GmLintSubjectKind::AUTOMATION;
  return std::nullopt;
}

enum class GridClaimPolicy {
  SELF_CLAIM,
  APPROVAL,
  INVITE,
  MARKETPLACE_ONLY,
};

inline constexpr std::string_view toString(GridClaimPolicy v) {
  switch (v) {
    case GridClaimPolicy::SELF_CLAIM: return "SELF_CLAIM";
    case GridClaimPolicy::APPROVAL: return "APPROVAL";
    case GridClaimPolicy::INVITE: return "INVITE";
    case GridClaimPolicy::MARKETPLACE_ONLY: return "MARKETPLACE_ONLY";
  }
  return "";
}

inline std::optional<GridClaimPolicy> gridClaimPolicyFromString(std::string_view s) {
  if (s == "SELF_CLAIM") return GridClaimPolicy::SELF_CLAIM;
  if (s == "APPROVAL") return GridClaimPolicy::APPROVAL;
  if (s == "INVITE") return GridClaimPolicy::INVITE;
  if (s == "MARKETPLACE_ONLY") return GridClaimPolicy::MARKETPLACE_ONLY;
  return std::nullopt;
}

enum class GridOwnerKind {
  USER,
  GROUP,
  ORG,
};

inline constexpr std::string_view toString(GridOwnerKind v) {
  switch (v) {
    case GridOwnerKind::USER: return "USER";
    case GridOwnerKind::GROUP: return "GROUP";
    case GridOwnerKind::ORG: return "ORG";
  }
  return "";
}

inline std::optional<GridOwnerKind> gridOwnerKindFromString(std::string_view s) {
  if (s == "USER") return GridOwnerKind::USER;
  if (s == "GROUP") return GridOwnerKind::GROUP;
  if (s == "ORG") return GridOwnerKind::ORG;
  return std::nullopt;
}

enum class GridTenure {
  OWNED,
  RENTED,
};

inline constexpr std::string_view toString(GridTenure v) {
  switch (v) {
    case GridTenure::OWNED: return "OWNED";
    case GridTenure::RENTED: return "RENTED";
  }
  return "";
}

inline std::optional<GridTenure> gridTenureFromString(std::string_view s) {
  if (s == "OWNED") return GridTenure::OWNED;
  if (s == "RENTED") return GridTenure::RENTED;
  return std::nullopt;
}

enum class MeteredComputeEngine {
  EXPRESSION,
  STUDIO_WASM,
  PLAYER_WASM,
};

inline constexpr std::string_view toString(MeteredComputeEngine v) {
  switch (v) {
    case MeteredComputeEngine::EXPRESSION: return "EXPRESSION";
    case MeteredComputeEngine::STUDIO_WASM: return "STUDIO_WASM";
    case MeteredComputeEngine::PLAYER_WASM: return "PLAYER_WASM";
  }
  return "";
}

inline std::optional<MeteredComputeEngine> meteredComputeEngineFromString(std::string_view s) {
  if (s == "EXPRESSION") return MeteredComputeEngine::EXPRESSION;
  if (s == "STUDIO_WASM") return MeteredComputeEngine::STUDIO_WASM;
  if (s == "PLAYER_WASM") return MeteredComputeEngine::PLAYER_WASM;
  return std::nullopt;
}

enum class PaymentProvider {
  STRIPE,
  PAYPAL,
};

inline constexpr std::string_view toString(PaymentProvider v) {
  switch (v) {
    case PaymentProvider::STRIPE: return "STRIPE";
    case PaymentProvider::PAYPAL: return "PAYPAL";
  }
  return "";
}

inline std::optional<PaymentProvider> paymentProviderFromString(std::string_view s) {
  if (s == "STRIPE") return PaymentProvider::STRIPE;
  if (s == "PAYPAL") return PaymentProvider::PAYPAL;
  return std::nullopt;
}

enum class PlayerCodeAcquisitionMode {
  FREE,
  BUY,
  RENT,
  TIME_LIMITED,
  COST_LIMITED,
};

inline constexpr std::string_view toString(PlayerCodeAcquisitionMode v) {
  switch (v) {
    case PlayerCodeAcquisitionMode::FREE: return "FREE";
    case PlayerCodeAcquisitionMode::BUY: return "BUY";
    case PlayerCodeAcquisitionMode::RENT: return "RENT";
    case PlayerCodeAcquisitionMode::TIME_LIMITED: return "TIME_LIMITED";
    case PlayerCodeAcquisitionMode::COST_LIMITED: return "COST_LIMITED";
  }
  return "";
}

inline std::optional<PlayerCodeAcquisitionMode> playerCodeAcquisitionModeFromString(std::string_view s) {
  if (s == "FREE") return PlayerCodeAcquisitionMode::FREE;
  if (s == "BUY") return PlayerCodeAcquisitionMode::BUY;
  if (s == "RENT") return PlayerCodeAcquisitionMode::RENT;
  if (s == "TIME_LIMITED") return PlayerCodeAcquisitionMode::TIME_LIMITED;
  if (s == "COST_LIMITED") return PlayerCodeAcquisitionMode::COST_LIMITED;
  return std::nullopt;
}

enum class PlayerCodeAdmissionState {
  ADMITTED,
  PENDING,
  REVOKED,
};

inline constexpr std::string_view toString(PlayerCodeAdmissionState v) {
  switch (v) {
    case PlayerCodeAdmissionState::ADMITTED: return "ADMITTED";
    case PlayerCodeAdmissionState::PENDING: return "PENDING";
    case PlayerCodeAdmissionState::REVOKED: return "REVOKED";
  }
  return "";
}

inline std::optional<PlayerCodeAdmissionState> playerCodeAdmissionStateFromString(std::string_view s) {
  if (s == "ADMITTED") return PlayerCodeAdmissionState::ADMITTED;
  if (s == "PENDING") return PlayerCodeAdmissionState::PENDING;
  if (s == "REVOKED") return PlayerCodeAdmissionState::REVOKED;
  return std::nullopt;
}

enum class PlayerCodeLicenseMode {
  CLOSED,
  OPEN_SOURCE,
};

inline constexpr std::string_view toString(PlayerCodeLicenseMode v) {
  switch (v) {
    case PlayerCodeLicenseMode::CLOSED: return "CLOSED";
    case PlayerCodeLicenseMode::OPEN_SOURCE: return "OPEN_SOURCE";
  }
  return "";
}

inline std::optional<PlayerCodeLicenseMode> playerCodeLicenseModeFromString(std::string_view s) {
  if (s == "CLOSED") return PlayerCodeLicenseMode::CLOSED;
  if (s == "OPEN_SOURCE") return PlayerCodeLicenseMode::OPEN_SOURCE;
  return std::nullopt;
}

enum class PlayerCodeListingStatus {
  ACTIVE,
  DELISTED,
  KILLED,
};

inline constexpr std::string_view toString(PlayerCodeListingStatus v) {
  switch (v) {
    case PlayerCodeListingStatus::ACTIVE: return "ACTIVE";
    case PlayerCodeListingStatus::DELISTED: return "DELISTED";
    case PlayerCodeListingStatus::KILLED: return "KILLED";
  }
  return "";
}

inline std::optional<PlayerCodeListingStatus> playerCodeListingStatusFromString(std::string_view s) {
  if (s == "ACTIVE") return PlayerCodeListingStatus::ACTIVE;
  if (s == "DELISTED") return PlayerCodeListingStatus::DELISTED;
  if (s == "KILLED") return PlayerCodeListingStatus::KILLED;
  return std::nullopt;
}

enum class PlayerCodeOwnerKind {
  USER,
  ORG,
};

inline constexpr std::string_view toString(PlayerCodeOwnerKind v) {
  switch (v) {
    case PlayerCodeOwnerKind::USER: return "USER";
    case PlayerCodeOwnerKind::ORG: return "ORG";
  }
  return "";
}

inline std::optional<PlayerCodeOwnerKind> playerCodeOwnerKindFromString(std::string_view s) {
  if (s == "USER") return PlayerCodeOwnerKind::USER;
  if (s == "ORG") return PlayerCodeOwnerKind::ORG;
  return std::nullopt;
}

enum class PlayerComputeTarget {
  SERVER,
  CLIENT,
};

inline constexpr std::string_view toString(PlayerComputeTarget v) {
  switch (v) {
    case PlayerComputeTarget::SERVER: return "SERVER";
    case PlayerComputeTarget::CLIENT: return "CLIENT";
  }
  return "";
}

inline std::optional<PlayerComputeTarget> playerComputeTargetFromString(std::string_view s) {
  if (s == "SERVER") return PlayerComputeTarget::SERVER;
  if (s == "CLIENT") return PlayerComputeTarget::CLIENT;
  return std::nullopt;
}

enum class PlayerFaultCode {
  USER_CODE_ERROR,
  USER_CODE_TOO_SLOW,
  USER_CODE_LIMIT_EXCEEDED,
  PLATFORM_BUSY,
  PLATFORM_ERROR,
  BUDGET_EXCEEDED,
  RATE_LIMITED,
  QUOTA_EXHAUSTED,
  TEMPORARILY_DISABLED,
  INVALID_REQUEST,
  NOT_ALLOWED,
  NOT_FOUND,
  UNAUTHENTICATED,
  WRONG_DATACENTER,
  APP_UNAVAILABLE,
};

inline constexpr std::string_view toString(PlayerFaultCode v) {
  switch (v) {
    case PlayerFaultCode::USER_CODE_ERROR: return "USER_CODE_ERROR";
    case PlayerFaultCode::USER_CODE_TOO_SLOW: return "USER_CODE_TOO_SLOW";
    case PlayerFaultCode::USER_CODE_LIMIT_EXCEEDED: return "USER_CODE_LIMIT_EXCEEDED";
    case PlayerFaultCode::PLATFORM_BUSY: return "PLATFORM_BUSY";
    case PlayerFaultCode::PLATFORM_ERROR: return "PLATFORM_ERROR";
    case PlayerFaultCode::BUDGET_EXCEEDED: return "BUDGET_EXCEEDED";
    case PlayerFaultCode::RATE_LIMITED: return "RATE_LIMITED";
    case PlayerFaultCode::QUOTA_EXHAUSTED: return "QUOTA_EXHAUSTED";
    case PlayerFaultCode::TEMPORARILY_DISABLED: return "TEMPORARILY_DISABLED";
    case PlayerFaultCode::INVALID_REQUEST: return "INVALID_REQUEST";
    case PlayerFaultCode::NOT_ALLOWED: return "NOT_ALLOWED";
    case PlayerFaultCode::NOT_FOUND: return "NOT_FOUND";
    case PlayerFaultCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
    case PlayerFaultCode::WRONG_DATACENTER: return "WRONG_DATACENTER";
    case PlayerFaultCode::APP_UNAVAILABLE: return "APP_UNAVAILABLE";
  }
  return "";
}

inline std::optional<PlayerFaultCode> playerFaultCodeFromString(std::string_view s) {
  if (s == "USER_CODE_ERROR") return PlayerFaultCode::USER_CODE_ERROR;
  if (s == "USER_CODE_TOO_SLOW") return PlayerFaultCode::USER_CODE_TOO_SLOW;
  if (s == "USER_CODE_LIMIT_EXCEEDED") return PlayerFaultCode::USER_CODE_LIMIT_EXCEEDED;
  if (s == "PLATFORM_BUSY") return PlayerFaultCode::PLATFORM_BUSY;
  if (s == "PLATFORM_ERROR") return PlayerFaultCode::PLATFORM_ERROR;
  if (s == "BUDGET_EXCEEDED") return PlayerFaultCode::BUDGET_EXCEEDED;
  if (s == "RATE_LIMITED") return PlayerFaultCode::RATE_LIMITED;
  if (s == "QUOTA_EXHAUSTED") return PlayerFaultCode::QUOTA_EXHAUSTED;
  if (s == "TEMPORARILY_DISABLED") return PlayerFaultCode::TEMPORARILY_DISABLED;
  if (s == "INVALID_REQUEST") return PlayerFaultCode::INVALID_REQUEST;
  if (s == "NOT_ALLOWED") return PlayerFaultCode::NOT_ALLOWED;
  if (s == "NOT_FOUND") return PlayerFaultCode::NOT_FOUND;
  if (s == "UNAUTHENTICATED") return PlayerFaultCode::UNAUTHENTICATED;
  if (s == "WRONG_DATACENTER") return PlayerFaultCode::WRONG_DATACENTER;
  if (s == "APP_UNAVAILABLE") return PlayerFaultCode::APP_UNAVAILABLE;
  return std::nullopt;
}

enum class RateScope {
  SHARED,
  PLAYER,
};

inline constexpr std::string_view toString(RateScope v) {
  switch (v) {
    case RateScope::SHARED: return "SHARED";
    case RateScope::PLAYER: return "PLAYER";
  }
  return "";
}

inline std::optional<RateScope> rateScopeFromString(std::string_view s) {
  if (s == "SHARED") return RateScope::SHARED;
  if (s == "PLAYER") return RateScope::PLAYER;
  return std::nullopt;
}

enum class SellerOnboardingStatus {
  NONE,
  PENDING,
  COMPLETE,
  BLOCKED,
};

inline constexpr std::string_view toString(SellerOnboardingStatus v) {
  switch (v) {
    case SellerOnboardingStatus::NONE: return "NONE";
    case SellerOnboardingStatus::PENDING: return "PENDING";
    case SellerOnboardingStatus::COMPLETE: return "COMPLETE";
    case SellerOnboardingStatus::BLOCKED: return "BLOCKED";
  }
  return "";
}

inline std::optional<SellerOnboardingStatus> sellerOnboardingStatusFromString(std::string_view s) {
  if (s == "NONE") return SellerOnboardingStatus::NONE;
  if (s == "PENDING") return SellerOnboardingStatus::PENDING;
  if (s == "COMPLETE") return SellerOnboardingStatus::COMPLETE;
  if (s == "BLOCKED") return SellerOnboardingStatus::BLOCKED;
  return std::nullopt;
}

enum class ServerState {
  Starting,
  ReadyForClients,
  Stopping,
  Offline,
  NearCapacity,
  Full,
};

inline constexpr std::string_view toString(ServerState v) {
  switch (v) {
    case ServerState::Starting: return "Starting";
    case ServerState::ReadyForClients: return "ReadyForClients";
    case ServerState::Stopping: return "Stopping";
    case ServerState::Offline: return "Offline";
    case ServerState::NearCapacity: return "NearCapacity";
    case ServerState::Full: return "Full";
  }
  return "";
}

inline std::optional<ServerState> serverStateFromString(std::string_view s) {
  if (s == "Starting") return ServerState::Starting;
  if (s == "ReadyForClients") return ServerState::ReadyForClients;
  if (s == "Stopping") return ServerState::Stopping;
  if (s == "Offline") return ServerState::Offline;
  if (s == "NearCapacity") return ServerState::NearCapacity;
  if (s == "Full") return ServerState::Full;
  return std::nullopt;
}

enum class UdpErrorCode {
  NO_ERROR,
  UNKNOWN_ERROR,
  EMAIL_NOT_FOUND,
  BAD_PASSWORD,
  EMAIL_ALREADY_EXISTS,
  INVALID_TOKEN,
  APP_NOT_FOUND,
  UNAUTHORIZED,
  APP_NOT_LOADED,
  EMAIL_TOO_SHORT,
  EMAIL_TOO_LONG,
  PASSWORD_TOO_SHORT,
  PASSWORD_TOO_LONG,
  GAME_TOKEN_WRONG_SIZE,
  NAME_TOO_LONG,
  INVALID_REQUEST,
  EMAIL_INVALID,
  INVALID_TOKEN_LENGTH,
  INVALID_APP_ID,
  CHUNK_NOT_FOUND,
  USER_NOT_AUTHENTICATED,
  INVALID_STATE_DATA,
  USER_NOT_APP_ADMIN,
  GRID_OUTSIDE_ASSIGNMENT,
  NO_MATCHING_GRID_ASSIGNMENT,
  INVALID_GRID_COORDINATES,
  GRID_ALREADY_EXISTS,
  GRID_OVERLAPS_EXISTING,
  GAMERTAG_ALREADY_EXISTS,
  GRID_NOT_FOUND,
  CANNOT_DELETE_DEFAULT_WORLD_GRID,
  GRID_HAS_NESTED_CHILDREN,
  TOKEN_EXPIRED,
};

inline constexpr std::string_view toString(UdpErrorCode v) {
  switch (v) {
    case UdpErrorCode::NO_ERROR: return "NO_ERROR";
    case UdpErrorCode::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
    case UdpErrorCode::EMAIL_NOT_FOUND: return "EMAIL_NOT_FOUND";
    case UdpErrorCode::BAD_PASSWORD: return "BAD_PASSWORD";
    case UdpErrorCode::EMAIL_ALREADY_EXISTS: return "EMAIL_ALREADY_EXISTS";
    case UdpErrorCode::INVALID_TOKEN: return "INVALID_TOKEN";
    case UdpErrorCode::APP_NOT_FOUND: return "APP_NOT_FOUND";
    case UdpErrorCode::UNAUTHORIZED: return "UNAUTHORIZED";
    case UdpErrorCode::APP_NOT_LOADED: return "APP_NOT_LOADED";
    case UdpErrorCode::EMAIL_TOO_SHORT: return "EMAIL_TOO_SHORT";
    case UdpErrorCode::EMAIL_TOO_LONG: return "EMAIL_TOO_LONG";
    case UdpErrorCode::PASSWORD_TOO_SHORT: return "PASSWORD_TOO_SHORT";
    case UdpErrorCode::PASSWORD_TOO_LONG: return "PASSWORD_TOO_LONG";
    case UdpErrorCode::GAME_TOKEN_WRONG_SIZE: return "GAME_TOKEN_WRONG_SIZE";
    case UdpErrorCode::NAME_TOO_LONG: return "NAME_TOO_LONG";
    case UdpErrorCode::INVALID_REQUEST: return "INVALID_REQUEST";
    case UdpErrorCode::EMAIL_INVALID: return "EMAIL_INVALID";
    case UdpErrorCode::INVALID_TOKEN_LENGTH: return "INVALID_TOKEN_LENGTH";
    case UdpErrorCode::INVALID_APP_ID: return "INVALID_APP_ID";
    case UdpErrorCode::CHUNK_NOT_FOUND: return "CHUNK_NOT_FOUND";
    case UdpErrorCode::USER_NOT_AUTHENTICATED: return "USER_NOT_AUTHENTICATED";
    case UdpErrorCode::INVALID_STATE_DATA: return "INVALID_STATE_DATA";
    case UdpErrorCode::USER_NOT_APP_ADMIN: return "USER_NOT_APP_ADMIN";
    case UdpErrorCode::GRID_OUTSIDE_ASSIGNMENT: return "GRID_OUTSIDE_ASSIGNMENT";
    case UdpErrorCode::NO_MATCHING_GRID_ASSIGNMENT: return "NO_MATCHING_GRID_ASSIGNMENT";
    case UdpErrorCode::INVALID_GRID_COORDINATES: return "INVALID_GRID_COORDINATES";
    case UdpErrorCode::GRID_ALREADY_EXISTS: return "GRID_ALREADY_EXISTS";
    case UdpErrorCode::GRID_OVERLAPS_EXISTING: return "GRID_OVERLAPS_EXISTING";
    case UdpErrorCode::GAMERTAG_ALREADY_EXISTS: return "GAMERTAG_ALREADY_EXISTS";
    case UdpErrorCode::GRID_NOT_FOUND: return "GRID_NOT_FOUND";
    case UdpErrorCode::CANNOT_DELETE_DEFAULT_WORLD_GRID: return "CANNOT_DELETE_DEFAULT_WORLD_GRID";
    case UdpErrorCode::GRID_HAS_NESTED_CHILDREN: return "GRID_HAS_NESTED_CHILDREN";
    case UdpErrorCode::TOKEN_EXPIRED: return "TOKEN_EXPIRED";
  }
  return "";
}

inline std::optional<UdpErrorCode> udpErrorCodeFromString(std::string_view s) {
  if (s == "NO_ERROR") return UdpErrorCode::NO_ERROR;
  if (s == "UNKNOWN_ERROR") return UdpErrorCode::UNKNOWN_ERROR;
  if (s == "EMAIL_NOT_FOUND") return UdpErrorCode::EMAIL_NOT_FOUND;
  if (s == "BAD_PASSWORD") return UdpErrorCode::BAD_PASSWORD;
  if (s == "EMAIL_ALREADY_EXISTS") return UdpErrorCode::EMAIL_ALREADY_EXISTS;
  if (s == "INVALID_TOKEN") return UdpErrorCode::INVALID_TOKEN;
  if (s == "APP_NOT_FOUND") return UdpErrorCode::APP_NOT_FOUND;
  if (s == "UNAUTHORIZED") return UdpErrorCode::UNAUTHORIZED;
  if (s == "APP_NOT_LOADED") return UdpErrorCode::APP_NOT_LOADED;
  if (s == "EMAIL_TOO_SHORT") return UdpErrorCode::EMAIL_TOO_SHORT;
  if (s == "EMAIL_TOO_LONG") return UdpErrorCode::EMAIL_TOO_LONG;
  if (s == "PASSWORD_TOO_SHORT") return UdpErrorCode::PASSWORD_TOO_SHORT;
  if (s == "PASSWORD_TOO_LONG") return UdpErrorCode::PASSWORD_TOO_LONG;
  if (s == "GAME_TOKEN_WRONG_SIZE") return UdpErrorCode::GAME_TOKEN_WRONG_SIZE;
  if (s == "NAME_TOO_LONG") return UdpErrorCode::NAME_TOO_LONG;
  if (s == "INVALID_REQUEST") return UdpErrorCode::INVALID_REQUEST;
  if (s == "EMAIL_INVALID") return UdpErrorCode::EMAIL_INVALID;
  if (s == "INVALID_TOKEN_LENGTH") return UdpErrorCode::INVALID_TOKEN_LENGTH;
  if (s == "INVALID_APP_ID") return UdpErrorCode::INVALID_APP_ID;
  if (s == "CHUNK_NOT_FOUND") return UdpErrorCode::CHUNK_NOT_FOUND;
  if (s == "USER_NOT_AUTHENTICATED") return UdpErrorCode::USER_NOT_AUTHENTICATED;
  if (s == "INVALID_STATE_DATA") return UdpErrorCode::INVALID_STATE_DATA;
  if (s == "USER_NOT_APP_ADMIN") return UdpErrorCode::USER_NOT_APP_ADMIN;
  if (s == "GRID_OUTSIDE_ASSIGNMENT") return UdpErrorCode::GRID_OUTSIDE_ASSIGNMENT;
  if (s == "NO_MATCHING_GRID_ASSIGNMENT") return UdpErrorCode::NO_MATCHING_GRID_ASSIGNMENT;
  if (s == "INVALID_GRID_COORDINATES") return UdpErrorCode::INVALID_GRID_COORDINATES;
  if (s == "GRID_ALREADY_EXISTS") return UdpErrorCode::GRID_ALREADY_EXISTS;
  if (s == "GRID_OVERLAPS_EXISTING") return UdpErrorCode::GRID_OVERLAPS_EXISTING;
  if (s == "GAMERTAG_ALREADY_EXISTS") return UdpErrorCode::GAMERTAG_ALREADY_EXISTS;
  if (s == "GRID_NOT_FOUND") return UdpErrorCode::GRID_NOT_FOUND;
  if (s == "CANNOT_DELETE_DEFAULT_WORLD_GRID") return UdpErrorCode::CANNOT_DELETE_DEFAULT_WORLD_GRID;
  if (s == "GRID_HAS_NESTED_CHILDREN") return UdpErrorCode::GRID_HAS_NESTED_CHILDREN;
  if (s == "TOKEN_EXPIRED") return UdpErrorCode::TOKEN_EXPIRED;
  return std::nullopt;
}

enum class UserCodeFaultBlame {
  PLATFORM,
  AUTHOR,
  BUDGET,
};

inline constexpr std::string_view toString(UserCodeFaultBlame v) {
  switch (v) {
    case UserCodeFaultBlame::PLATFORM: return "PLATFORM";
    case UserCodeFaultBlame::AUTHOR: return "AUTHOR";
    case UserCodeFaultBlame::BUDGET: return "BUDGET";
  }
  return "";
}

inline std::optional<UserCodeFaultBlame> userCodeFaultBlameFromString(std::string_view s) {
  if (s == "PLATFORM") return UserCodeFaultBlame::PLATFORM;
  if (s == "AUTHOR") return UserCodeFaultBlame::AUTHOR;
  if (s == "BUDGET") return UserCodeFaultBlame::BUDGET;
  return std::nullopt;
}

enum class UserCodeFaultEngine {
  EXPRESSION,
  STUDIO_WASM,
  PLAYER_WASM,
};

inline constexpr std::string_view toString(UserCodeFaultEngine v) {
  switch (v) {
    case UserCodeFaultEngine::EXPRESSION: return "EXPRESSION";
    case UserCodeFaultEngine::STUDIO_WASM: return "STUDIO_WASM";
    case UserCodeFaultEngine::PLAYER_WASM: return "PLAYER_WASM";
  }
  return "";
}

inline std::optional<UserCodeFaultEngine> userCodeFaultEngineFromString(std::string_view s) {
  if (s == "EXPRESSION") return UserCodeFaultEngine::EXPRESSION;
  if (s == "STUDIO_WASM") return UserCodeFaultEngine::STUDIO_WASM;
  if (s == "PLAYER_WASM") return UserCodeFaultEngine::PLAYER_WASM;
  return std::nullopt;
}

enum class UserCodeFaultKind {
  EXPRESSION_TIMEOUT,
  GAS_EXHAUSTED,
  CALL_LIMIT_EXCEEDED,
  EXPRESSION_ERROR,
  WASM_TRAP,
  FUEL_EXHAUSTED,
  MEMORY_EXCEEDED,
  WATCHDOG_TERMINATED,
  MODULE_LOAD_FAILED,
  BINDING_MISMATCH,
  RESPONSE_TOO_LARGE,
  SANDBOX_POISONED,
  WORKER_EXIT,
  HOST_CALL_FAILED,
  UNKNOWN_HOST_FUNCTION,
  INTERNAL_ERROR,
  DB_OPS_EXCEEDED,
  EGRESS_BUDGET_EXCEEDED,
  STATE_WRITE_BUDGET_EXCEEDED,
  RATE_LIMIT_EXCEEDED,
  QUOTA_EXHAUSTED,
  CONTRACT_VALIDATION_FAILED,
  PLATFORM_BUSY,
  CIRCUIT_OPEN,
  DISABLED_BY_POLICY,
  NOTIFICATION_UNDELIVERABLE,
};

inline constexpr std::string_view toString(UserCodeFaultKind v) {
  switch (v) {
    case UserCodeFaultKind::EXPRESSION_TIMEOUT: return "EXPRESSION_TIMEOUT";
    case UserCodeFaultKind::GAS_EXHAUSTED: return "GAS_EXHAUSTED";
    case UserCodeFaultKind::CALL_LIMIT_EXCEEDED: return "CALL_LIMIT_EXCEEDED";
    case UserCodeFaultKind::EXPRESSION_ERROR: return "EXPRESSION_ERROR";
    case UserCodeFaultKind::WASM_TRAP: return "WASM_TRAP";
    case UserCodeFaultKind::FUEL_EXHAUSTED: return "FUEL_EXHAUSTED";
    case UserCodeFaultKind::MEMORY_EXCEEDED: return "MEMORY_EXCEEDED";
    case UserCodeFaultKind::WATCHDOG_TERMINATED: return "WATCHDOG_TERMINATED";
    case UserCodeFaultKind::MODULE_LOAD_FAILED: return "MODULE_LOAD_FAILED";
    case UserCodeFaultKind::BINDING_MISMATCH: return "BINDING_MISMATCH";
    case UserCodeFaultKind::RESPONSE_TOO_LARGE: return "RESPONSE_TOO_LARGE";
    case UserCodeFaultKind::SANDBOX_POISONED: return "SANDBOX_POISONED";
    case UserCodeFaultKind::WORKER_EXIT: return "WORKER_EXIT";
    case UserCodeFaultKind::HOST_CALL_FAILED: return "HOST_CALL_FAILED";
    case UserCodeFaultKind::UNKNOWN_HOST_FUNCTION: return "UNKNOWN_HOST_FUNCTION";
    case UserCodeFaultKind::INTERNAL_ERROR: return "INTERNAL_ERROR";
    case UserCodeFaultKind::DB_OPS_EXCEEDED: return "DB_OPS_EXCEEDED";
    case UserCodeFaultKind::EGRESS_BUDGET_EXCEEDED: return "EGRESS_BUDGET_EXCEEDED";
    case UserCodeFaultKind::STATE_WRITE_BUDGET_EXCEEDED: return "STATE_WRITE_BUDGET_EXCEEDED";
    case UserCodeFaultKind::RATE_LIMIT_EXCEEDED: return "RATE_LIMIT_EXCEEDED";
    case UserCodeFaultKind::QUOTA_EXHAUSTED: return "QUOTA_EXHAUSTED";
    case UserCodeFaultKind::CONTRACT_VALIDATION_FAILED: return "CONTRACT_VALIDATION_FAILED";
    case UserCodeFaultKind::PLATFORM_BUSY: return "PLATFORM_BUSY";
    case UserCodeFaultKind::CIRCUIT_OPEN: return "CIRCUIT_OPEN";
    case UserCodeFaultKind::DISABLED_BY_POLICY: return "DISABLED_BY_POLICY";
    case UserCodeFaultKind::NOTIFICATION_UNDELIVERABLE: return "NOTIFICATION_UNDELIVERABLE";
  }
  return "";
}

inline std::optional<UserCodeFaultKind> userCodeFaultKindFromString(std::string_view s) {
  if (s == "EXPRESSION_TIMEOUT") return UserCodeFaultKind::EXPRESSION_TIMEOUT;
  if (s == "GAS_EXHAUSTED") return UserCodeFaultKind::GAS_EXHAUSTED;
  if (s == "CALL_LIMIT_EXCEEDED") return UserCodeFaultKind::CALL_LIMIT_EXCEEDED;
  if (s == "EXPRESSION_ERROR") return UserCodeFaultKind::EXPRESSION_ERROR;
  if (s == "WASM_TRAP") return UserCodeFaultKind::WASM_TRAP;
  if (s == "FUEL_EXHAUSTED") return UserCodeFaultKind::FUEL_EXHAUSTED;
  if (s == "MEMORY_EXCEEDED") return UserCodeFaultKind::MEMORY_EXCEEDED;
  if (s == "WATCHDOG_TERMINATED") return UserCodeFaultKind::WATCHDOG_TERMINATED;
  if (s == "MODULE_LOAD_FAILED") return UserCodeFaultKind::MODULE_LOAD_FAILED;
  if (s == "BINDING_MISMATCH") return UserCodeFaultKind::BINDING_MISMATCH;
  if (s == "RESPONSE_TOO_LARGE") return UserCodeFaultKind::RESPONSE_TOO_LARGE;
  if (s == "SANDBOX_POISONED") return UserCodeFaultKind::SANDBOX_POISONED;
  if (s == "WORKER_EXIT") return UserCodeFaultKind::WORKER_EXIT;
  if (s == "HOST_CALL_FAILED") return UserCodeFaultKind::HOST_CALL_FAILED;
  if (s == "UNKNOWN_HOST_FUNCTION") return UserCodeFaultKind::UNKNOWN_HOST_FUNCTION;
  if (s == "INTERNAL_ERROR") return UserCodeFaultKind::INTERNAL_ERROR;
  if (s == "DB_OPS_EXCEEDED") return UserCodeFaultKind::DB_OPS_EXCEEDED;
  if (s == "EGRESS_BUDGET_EXCEEDED") return UserCodeFaultKind::EGRESS_BUDGET_EXCEEDED;
  if (s == "STATE_WRITE_BUDGET_EXCEEDED") return UserCodeFaultKind::STATE_WRITE_BUDGET_EXCEEDED;
  if (s == "RATE_LIMIT_EXCEEDED") return UserCodeFaultKind::RATE_LIMIT_EXCEEDED;
  if (s == "QUOTA_EXHAUSTED") return UserCodeFaultKind::QUOTA_EXHAUSTED;
  if (s == "CONTRACT_VALIDATION_FAILED") return UserCodeFaultKind::CONTRACT_VALIDATION_FAILED;
  if (s == "PLATFORM_BUSY") return UserCodeFaultKind::PLATFORM_BUSY;
  if (s == "CIRCUIT_OPEN") return UserCodeFaultKind::CIRCUIT_OPEN;
  if (s == "DISABLED_BY_POLICY") return UserCodeFaultKind::DISABLED_BY_POLICY;
  if (s == "NOTIFICATION_UNDELIVERABLE") return UserCodeFaultKind::NOTIFICATION_UNDELIVERABLE;
  return std::nullopt;
}

}  // namespace crowdy::gen
