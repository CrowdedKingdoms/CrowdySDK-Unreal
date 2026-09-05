#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "crowdy/player_host/lease_manager.hpp"

namespace crowdy::agent {

enum class NativeAgentModeV1 { Ask, Build, Play };
enum class NativeToolResultStatusV1 {
  Succeeded,
  Failed,
  Cancelled,
  TimedOut,
  OutcomeUnknown,
};

struct NoArgumentsV1 {
  bool operator==(const NoArgumentsV1&) const = default;
};

enum class StudioTargetV1 { Server, Client };
enum class StudioPairingPreferenceV1 { None, Optional, Required };
enum class StudioSaveStateV1 { Saving, Saved, Conflict, Offline };
enum class StudioProjectKindV1 { Server, Client, FullStack };
enum class StudioFileSourceV1 { Project, PersonalLibrary, Common };
enum class StudioRuntimeEnvironmentV1 { Draft, Live };
enum class StudioRuntimeParameterTypeV1 { String, Decimal, Boolean };

enum class StudioRuntimePhaseV1 {
  Idle,
  TestingDraft,
  DeployingLive,
  Compiling,
  Enabling,
  Running,
  CompileFailed,
  Stopping,
  Stopped,
  PartialFailure,
  Error,
};

enum class StudioRuntimeSyncV1 {
  NeverRun,
  RunningSaved,
  RunningStale,
  Stopped,
};

struct StudioRuntimeStatusV1 {
  StudioRuntimePhaseV1 phase = StudioRuntimePhaseV1::Idle;
  std::string saved_revision;
  std::optional<std::string> running_revision;
  StudioRuntimeSyncV1 sync = StudioRuntimeSyncV1::NeverRun;
  std::optional<StudioTargetV1> target;
  std::optional<bool> draft;
  std::optional<std::string> message;
  bool operator==(const StudioRuntimeStatusV1&) const = default;
};

struct StudioContextV1 {
  std::string app_ref;
  std::optional<std::string> project_ref;
  std::string grid_ref;
  std::string context_version;
  StudioSaveStateV1 save_state = StudioSaveStateV1::Saved;
  StudioRuntimeStatusV1 runtime;
  std::optional<std::string> client_epoch;
  std::vector<player_host::LeaseKindV1> lease_kinds;
  std::optional<std::string> host_capability_revision;
  bool operator==(const StudioContextV1&) const = default;
};

struct StudioFileSummaryV1 {
  StudioTargetV1 target = StudioTargetV1::Server;
  std::string path;
  std::string content_hash;
  std::uint32_t byte_length = 0;
  bool operator==(const StudioFileSummaryV1&) const = default;
};

struct StudioProjectProjectionV1 {
  std::string project_id;
  std::string name;
  std::optional<std::string> description;
  StudioProjectKindV1 kind = StudioProjectKindV1::Server;
  std::string revision;
  std::vector<StudioFileSummaryV1> files;
  std::optional<std::string> server_module_name;
  std::optional<std::string> client_module_name;
  StudioPairingPreferenceV1 pairing_preference =
      StudioPairingPreferenceV1::None;
  std::string updated_at;
  bool operator==(const StudioProjectProjectionV1&) const = default;
};

struct StudioOpenFileV1 {
  StudioFileSourceV1 source = StudioFileSourceV1::Project;
  StudioTargetV1 target = StudioTargetV1::Server;
  std::string path;
  bool operator==(const StudioOpenFileV1&) const = default;
};

struct StudioStateV1 {
  std::optional<StudioProjectProjectionV1> project;
  std::vector<StudioOpenFileV1> open_files;
  StudioSaveStateV1 save_state = StudioSaveStateV1::Saved;
  StudioRuntimeStatusV1 runtime;
  bool operator==(const StudioStateV1&) const = default;
};

struct StudioProjectSelectRequestV1 {
  std::string project_ref;
  bool operator==(const StudioProjectSelectRequestV1&) const = default;
};

struct StudioProjectSelectResultV1 {
  std::string selected_project_ref;
  std::string revision;
  bool operator==(const StudioProjectSelectResultV1&) const = default;
};

struct StudioFileTabRequestV1 {
  StudioFileSourceV1 source = StudioFileSourceV1::Project;
  StudioTargetV1 target = StudioTargetV1::Server;
  std::string path;
  std::optional<std::string> reference_ref;
  bool operator==(const StudioFileTabRequestV1&) const = default;
};

struct StudioOkV1 {
  bool ok = true;
  bool operator==(const StudioOkV1&) const = default;
};

enum class StudioDiagnosticSourceV1 { LocalAdvisory, Rustc, Runtime };
enum class StudioDiagnosticSeverityV1 { Error, Warning, Info, Hint };

struct StudioDiagnosticV1 {
  StudioDiagnosticSourceV1 source =
      StudioDiagnosticSourceV1::LocalAdvisory;
  StudioTargetV1 target = StudioTargetV1::Server;
  std::string path;
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  StudioDiagnosticSeverityV1 severity = StudioDiagnosticSeverityV1::Info;
  std::optional<std::string> code;
  std::string message;
  bool operator==(const StudioDiagnosticV1&) const = default;
};

struct StudioDiagnosticsV1 {
  std::vector<StudioDiagnosticV1> diagnostics;
  bool operator==(const StudioDiagnosticsV1&) const = default;
};

struct StudioRuntimeTestDraftRequestV1 {
  std::string expected_revision;
  std::vector<StudioTargetV1> targets;
  bool operator==(const StudioRuntimeTestDraftRequestV1&) const = default;
};

struct StudioRuntimeDeployLiveRequestV1 {
  std::string expected_revision;
  std::string project_content_hash;
  std::vector<StudioTargetV1> targets;
  StudioPairingPreferenceV1 pairing_preference =
      StudioPairingPreferenceV1::None;
  bool draft = false;
  bool operator==(const StudioRuntimeDeployLiveRequestV1&) const = default;
};

struct StudioRuntimeParameterV1 {
  std::string name;
  StudioRuntimeParameterTypeV1 type = StudioRuntimeParameterTypeV1::String;
  std::string value;
  bool operator==(const StudioRuntimeParameterV1&) const = default;
};

struct StudioRuntimeInvokeRequestV1 {
  std::string export_name;
  StudioRuntimeEnvironmentV1 environment =
      StudioRuntimeEnvironmentV1::Draft;
  std::vector<StudioRuntimeParameterV1> params;
  bool operator==(const StudioRuntimeInvokeRequestV1&) const = default;
};

struct StudioRuntimePlanResultV1 {
  StudioRuntimeStatusV1 runtime;
  std::vector<StudioTargetV1> targets;
  bool operator==(const StudioRuntimePlanResultV1&) const = default;
};

enum class StudioRuntimeResultTypeV1 { Empty, Text, Base64 };

struct StudioRuntimeInvokeResultV1 {
  StudioRuntimeResultTypeV1 result_type =
      StudioRuntimeResultTypeV1::Empty;
  std::string result;
  std::string fuel_used;
  std::uint32_t duration_us = 0;
  bool operator==(const StudioRuntimeInvokeResultV1&) const = default;
};

struct StudioRuntimeStopResultV1 {
  bool server_stopped = false;
  bool client_stopped = false;
  std::vector<std::string> failures;
  bool operator==(const StudioRuntimeStopResultV1&) const = default;
};

enum class StudioNativeToolKindV1 {
  ContextGet,
  StateGet,
  ProjectSelect,
  WorkspaceTabOpen,
  WorkspaceTabClose,
  DiagnosticsLocalGet,
  RuntimeStatusGet,
  RuntimeTestDraft,
  RuntimeDeployLive,
  RuntimeInvoke,
  RuntimeStop,
};

using StudioNativeToolRequestV1 =
    std::variant<NoArgumentsV1, StudioProjectSelectRequestV1,
                 StudioFileTabRequestV1, StudioRuntimeTestDraftRequestV1,
                 StudioRuntimeDeployLiveRequestV1,
                 StudioRuntimeInvokeRequestV1>;

using StudioNativeToolOutputV1 =
    std::variant<StudioContextV1, StudioStateV1,
                 StudioProjectSelectResultV1, StudioOkV1,
                 StudioDiagnosticsV1, StudioRuntimeStatusV1,
                 StudioRuntimePlanResultV1, StudioRuntimeInvokeResultV1,
                 StudioRuntimeStopResultV1>;

struct ValidatedStudioGateV1 {
  std::string session_id;
  std::string run_id;
  std::string tool_call_id;
  std::string client_epoch;
  std::string context_version;
  std::optional<std::string> lease_id;
  std::optional<std::string> approval_grant;
  std::string validated_at;
  bool operator==(const ValidatedStudioGateV1&) const = default;
};

using StudioToolCallbackV1 = std::function<void(
    player_host::AdapterResultV1<StudioNativeToolOutputV1>)>;

/**
 * Native/headless Studio boundary. Implementations call the same controller
 * methods used by the human Studio UI. Server/model/provider tools and generic
 * CrowdyClient, GraphQL, filesystem, shell, or network access are forbidden.
 */
class CrowdyStudioHostAdapter {
 public:
  virtual ~CrowdyStudioHostAdapter() = default;
  virtual void dispatch(StudioNativeToolKindV1 kind,
                        const StudioNativeToolRequestV1& request,
                        const ValidatedStudioGateV1& gate,
                        player_host::CancellationTokenV1 cancellation,
                        StudioToolCallbackV1 callback) = 0;
  virtual void clearAgentOperation(
      player_host::PreemptionReasonV1 reason) noexcept = 0;
};

using NativeToolArgumentsV1 =
    std::variant<NoArgumentsV1, player_host::ObserveRequestV1,
                 player_host::GameCommandV1, StudioProjectSelectRequestV1,
                 StudioFileTabRequestV1, StudioRuntimeTestDraftRequestV1,
                 StudioRuntimeDeployLiveRequestV1,
                 StudioRuntimeInvokeRequestV1>;

using NativeToolOutputV1 =
    std::variant<player_host::PlayerHostCapabilitiesV1,
                 player_host::GameObservationV1,
                 player_host::GameCommandResultV1, StudioContextV1,
                 StudioStateV1, StudioProjectSelectResultV1, StudioOkV1,
                 StudioDiagnosticsV1, StudioRuntimeStatusV1,
                 StudioRuntimePlanResultV1, StudioRuntimeInvokeResultV1,
                 StudioRuntimeStopResultV1>;

struct NativeToolInvocationV1 {
  std::string protocol_version{"crowdy.tool-call/1"};
  std::string session_id;
  std::string run_id;
  std::string tool_call_id;
  std::string name;
  std::string version{"1.0.0"};
  std::string descriptor_digest;
  NativeToolArgumentsV1 arguments;
  std::string argument_hash;
  std::string context_version;
  std::optional<std::string> client_epoch;
  std::optional<std::string> lease_id;
  std::optional<std::string> approval_grant;
  std::optional<std::string> idempotency_key;
  std::string deadline;
  bool operator==(const NativeToolInvocationV1&) const = default;
};

struct NativeToolResultV1 {
  std::string protocol_version{"crowdy.tool-result/1"};
  std::string tool_call_id;
  NativeToolResultStatusV1 status = NativeToolResultStatusV1::Failed;
  std::optional<NativeToolOutputV1> output;
  std::optional<player_host::AgentErrorV1> error;
  std::string observed_context_version;
  std::string started_at;
  std::string finished_at;
  bool operator==(const NativeToolResultV1&) const = default;
};

struct NativeLocalToolContractV1 {
  std::string_view name;
  std::string_view version;
  std::string_view descriptor_digest;
  std::uint32_t timeout_ms;
  bool effectful;
};

/** Canonical CrowdyJS v12 local executor subset: 14 game + 11 Studio tools. */
std::span<const NativeLocalToolContractV1> nativeLocalToolContractsV1() noexcept;

struct NativeToolDispatcherOptionsV1 {
  const core::IClock* clock = nullptr;
  std::function<std::optional<std::string>()> session_id;
  std::function<std::optional<std::string>()> client_epoch;
  std::function<std::string()> context_version;
  std::function<NativeAgentModeV1()> mode;
  std::function<bool(std::string_view, player_host::LeaseKindV1)>
      is_lease_active;
  /** Optional canonical-hash verifier supplied by the Agent Controller. */
  std::function<bool(const NativeToolInvocationV1&)> validate_argument_hash;
  std::function<bool(const NativeToolInvocationV1&)> validate_approval_grant;
  std::size_t max_remembered_calls = 2'048;
};

using NativeToolCompletionV1 = std::function<void(NativeToolResultV1)>;

/**
 * Exact native local-tool router. It has no fallback executor: model,
 * provider, server, unknown, and raw SDK calls fail closed.
 *
 * dispatch() and adapter callbacks may complete inline. Call tick() from the
 * game loop to enforce deadlines without spawning an SDK thread.
 */
class NativeToolDispatcherV1 {
 public:
  NativeToolDispatcherV1(
      player_host::AgentControlLeaseManager& lease_manager,
      CrowdyStudioHostAdapter* studio_adapter,
      NativeToolDispatcherOptionsV1 options = {});
  ~NativeToolDispatcherV1();

  NativeToolDispatcherV1(const NativeToolDispatcherV1&) = delete;
  NativeToolDispatcherV1& operator=(const NativeToolDispatcherV1&) = delete;
  NativeToolDispatcherV1(NativeToolDispatcherV1&&) noexcept;
  NativeToolDispatcherV1& operator=(NativeToolDispatcherV1&&) noexcept;

  void dispatch(NativeToolInvocationV1 invocation,
                NativeToolCompletionV1 completion);
  void tick();

  /** Clears local host intent/operations before completing cancellations. */
  void cancelActive(
      player_host::PreemptionReasonV1 reason =
          player_host::PreemptionReasonV1::HUMAN_STOP);
  void clearClosedSession();
  bool has(std::string_view tool_call_id) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::agent
