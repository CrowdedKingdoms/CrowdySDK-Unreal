#include "crowdy/studio/host_adapter.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "crowdy/core/clock.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/agent_projection.hpp"

namespace crowdy::studio {

struct CrowdyStudioControllerHostAdapter::Lifetime {
  std::recursive_mutex mutex;
  CrowdyStudioControllerHostAdapter* owner = nullptr;
};

namespace {

using agent::StudioFileSourceV1;
using agent::StudioNativeToolKindV1;
using agent::StudioNativeToolOutputV1;
using agent::StudioNativeToolRequestV1;
using agent::StudioPairingPreferenceV1;
using agent::StudioProjectKindV1;
using agent::StudioRuntimeEnvironmentV1;
using agent::StudioRuntimePhaseV1;
using agent::StudioRuntimeSyncV1;
using agent::StudioSaveStateV1;
using agent::StudioTargetV1;
using player_host::AgentErrorV1;
using player_host::LeaseKindV1;

AgentErrorV1 error(std::string code, std::string message,
                   bool retryable = false,
                   std::optional<std::string> field = std::nullopt) {
  AgentErrorV1 value;
  value.code = std::move(code);
  value.message = std::move(message);
  value.retryable = retryable;
  value.field = std::move(field);
  return value;
}

class AdapterFailure final : public std::exception {
 public:
  explicit AdapterFailure(AgentErrorV1 value, bool ambiguous = false)
      : value_(std::move(value)), ambiguous_(ambiguous) {}

  const char* what() const noexcept override {
    return value_.message.c_str();
  }
  const AgentErrorV1& value() const noexcept { return value_; }
  bool ambiguous() const noexcept { return ambiguous_; }

 private:
  AgentErrorV1 value_;
  bool ambiguous_ = false;
};

[[noreturn]] void fail(std::string code, std::string message,
                       bool ambiguous = false,
                       std::optional<std::string> field = std::nullopt) {
  throw AdapterFailure(
      error(std::move(code), std::move(message), false, std::move(field)),
      ambiguous);
}

StudioTargetV1 target(CrowdyStudioTarget value) {
  return value == CrowdyStudioTarget::Server ? StudioTargetV1::Server
                                             : StudioTargetV1::Client;
}

CrowdyStudioTarget target(StudioTargetV1 value) {
  return value == StudioTargetV1::Server ? CrowdyStudioTarget::Server
                                         : CrowdyStudioTarget::Client;
}

StudioPairingPreferenceV1 pairing(
    CrowdyStudioPairingPreference value) {
  switch (value) {
    case CrowdyStudioPairingPreference::None:
      return StudioPairingPreferenceV1::None;
    case CrowdyStudioPairingPreference::Optional:
      return StudioPairingPreferenceV1::Optional;
    case CrowdyStudioPairingPreference::Required:
      return StudioPairingPreferenceV1::Required;
  }
  return StudioPairingPreferenceV1::None;
}

CrowdyStudioPairingPreference pairing(
    StudioPairingPreferenceV1 value) {
  switch (value) {
    case StudioPairingPreferenceV1::None:
      return CrowdyStudioPairingPreference::None;
    case StudioPairingPreferenceV1::Optional:
      return CrowdyStudioPairingPreference::Optional;
    case StudioPairingPreferenceV1::Required:
      return CrowdyStudioPairingPreference::Required;
  }
  return CrowdyStudioPairingPreference::None;
}

StudioSaveStateV1 saveState(CrowdyStudioSaveState value) {
  switch (value) {
    case CrowdyStudioSaveState::Saving:
      return StudioSaveStateV1::Saving;
    case CrowdyStudioSaveState::Saved:
      return StudioSaveStateV1::Saved;
    case CrowdyStudioSaveState::Conflict:
      return StudioSaveStateV1::Conflict;
    case CrowdyStudioSaveState::Offline:
      return StudioSaveStateV1::Offline;
  }
  return StudioSaveStateV1::Offline;
}

StudioRuntimePhaseV1 phase(CrowdyStudioPhase value) {
  switch (value) {
    case CrowdyStudioPhase::Idle: return StudioRuntimePhaseV1::Idle;
    case CrowdyStudioPhase::TestingDraft:
      return StudioRuntimePhaseV1::TestingDraft;
    case CrowdyStudioPhase::DeployingLive:
      return StudioRuntimePhaseV1::DeployingLive;
    case CrowdyStudioPhase::Compiling:
      return StudioRuntimePhaseV1::Compiling;
    case CrowdyStudioPhase::Enabling:
      return StudioRuntimePhaseV1::Enabling;
    case CrowdyStudioPhase::Running:
      return StudioRuntimePhaseV1::Running;
    case CrowdyStudioPhase::CompileFailed:
      return StudioRuntimePhaseV1::CompileFailed;
    case CrowdyStudioPhase::Stopping:
      return StudioRuntimePhaseV1::Stopping;
    case CrowdyStudioPhase::Stopped:
      return StudioRuntimePhaseV1::Stopped;
    case CrowdyStudioPhase::PartialFailure:
      return StudioRuntimePhaseV1::PartialFailure;
    case CrowdyStudioPhase::Error: return StudioRuntimePhaseV1::Error;
  }
  return StudioRuntimePhaseV1::Error;
}

StudioRuntimeSyncV1 runtimeSync(CrowdyStudioRuntimeSyncState value) {
  switch (value) {
    case CrowdyStudioRuntimeSyncState::NeverRun:
      return StudioRuntimeSyncV1::NeverRun;
    case CrowdyStudioRuntimeSyncState::RunningSaved:
      return StudioRuntimeSyncV1::RunningSaved;
    case CrowdyStudioRuntimeSyncState::RunningStale:
      return StudioRuntimeSyncV1::RunningStale;
    case CrowdyStudioRuntimeSyncState::Stopped:
      return StudioRuntimeSyncV1::Stopped;
  }
  return StudioRuntimeSyncV1::NeverRun;
}

StudioProjectKindV1 projectKind(CrowdyStudioProjectKind value) {
  switch (value) {
    case CrowdyStudioProjectKind::Server:
      return StudioProjectKindV1::Server;
    case CrowdyStudioProjectKind::Client:
      return StudioProjectKindV1::Client;
    case CrowdyStudioProjectKind::FullStack:
      return StudioProjectKindV1::FullStack;
  }
  return StudioProjectKindV1::Server;
}

StudioFileSourceV1 source(CrowdyStudioFileRef::Source value) {
  switch (value) {
    case CrowdyStudioFileRef::Source::Project:
      return StudioFileSourceV1::Project;
    case CrowdyStudioFileRef::Source::PersonalLibrary:
      return StudioFileSourceV1::PersonalLibrary;
    case CrowdyStudioFileRef::Source::Common:
      return StudioFileSourceV1::Common;
  }
  return StudioFileSourceV1::Project;
}

CrowdyStudioFileRef::Source source(StudioFileSourceV1 value) {
  switch (value) {
    case StudioFileSourceV1::Project:
      return CrowdyStudioFileRef::Source::Project;
    case StudioFileSourceV1::PersonalLibrary:
      return CrowdyStudioFileRef::Source::PersonalLibrary;
    case StudioFileSourceV1::Common:
      return CrowdyStudioFileRef::Source::Common;
  }
  return CrowdyStudioFileRef::Source::Project;
}

std::vector<CrowdyStudioTarget> targets(
    const std::vector<StudioTargetV1>& values) {
  std::vector<CrowdyStudioTarget> mapped;
  mapped.reserve(values.size());
  for (const auto value : values) mapped.push_back(target(value));
  return mapped;
}

std::vector<StudioTargetV1> targets(
    const std::vector<CrowdyStudioTarget>& values) {
  std::vector<StudioTargetV1> mapped;
  mapped.reserve(values.size());
  for (const auto value : values) mapped.push_back(target(value));
  return mapped;
}

template <typename T>
bool sameSet(std::vector<T> left, std::vector<T> right) {
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

std::optional<LeaseKindV1> requiredLease(
    StudioNativeToolKindV1 kind,
    const StudioNativeToolRequestV1& request) {
  if (kind == StudioNativeToolKindV1::RuntimeTestDraft ||
      kind == StudioNativeToolKindV1::RuntimeDeployLive) {
    return LeaseKindV1::Workspace;
  }
  if (kind == StudioNativeToolKindV1::RuntimeInvoke) {
    const auto* invocation =
        std::get_if<agent::StudioRuntimeInvokeRequestV1>(&request);
    if (!invocation) return LeaseKindV1::Workspace;
    return invocation->environment == StudioRuntimeEnvironmentV1::Live
               ? LeaseKindV1::Play
               : LeaseKindV1::Workspace;
  }
  return std::nullopt;
}

bool approvalRequired(StudioNativeToolKindV1 kind,
                      const StudioNativeToolRequestV1& request) {
  if (kind == StudioNativeToolKindV1::RuntimeDeployLive) return true;
  if (kind != StudioNativeToolKindV1::RuntimeInvoke) return false;
  const auto* invocation =
      std::get_if<agent::StudioRuntimeInvokeRequestV1>(&request);
  return invocation &&
         invocation->environment == StudioRuntimeEnvironmentV1::Live;
}

bool potentiallyBlocking(StudioNativeToolKindV1 kind) {
  return kind == StudioNativeToolKindV1::ProjectSelect ||
         kind == StudioNativeToolKindV1::RuntimeTestDraft ||
         kind == StudioNativeToolKindV1::RuntimeDeployLive ||
         kind == StudioNativeToolKindV1::RuntimeInvoke ||
         kind == StudioNativeToolKindV1::RuntimeStop;
}

std::string preemptionMessage(player_host::PreemptionReasonV1 reason) {
  return "Agent operation preempted: " +
         std::string(player_host::preemptionReasonName(reason));
}

}  // namespace

CrowdyStudioControllerHostAdapter::CrowdyStudioControllerHostAdapter(
    CrowdyStudioController& controller, const core::ICrypto& crypto,
    CrowdyStudioControllerHostAdapterOptions options)
    : controller_(controller),
      crypto_(crypto),
      options_(std::move(options)),
      lifetime_(std::make_shared<Lifetime>()) {
  lifetime_->owner = this;
}

CrowdyStudioControllerHostAdapter::~CrowdyStudioControllerHostAdapter() {
  disposed_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(lifetime_->mutex);
    lifetime_->owner = nullptr;
  }
  clearAgentOperation(player_host::PreemptionReasonV1::SESSION_CLOSED);
}

void CrowdyStudioControllerHostAdapter::dispatch(
    StudioNativeToolKindV1 kind, const StudioNativeToolRequestV1& request,
    const agent::ValidatedStudioGateV1& gate,
    player_host::CancellationTokenV1 cancellation,
    agent::StudioToolCallbackV1 callback) {
  if (options_.schedule && potentiallyBlocking(kind)) {
    const std::weak_ptr<Lifetime> weak = lifetime_;
    const auto completed =
        std::make_shared<std::atomic<bool>>(false);
    agent::StudioToolCallbackV1 complete =
        [completed, callback = std::move(callback)](
            StudioResult result) mutable {
          if (!completed->exchange(true, std::memory_order_acq_rel)) {
            callback(std::move(result));
          }
        };
    try {
      options_.schedule(
          [weak, kind, request, gate, cancellation,
           complete]() mutable {
            const auto lifetime = weak.lock();
            if (!lifetime) {
              complete(StudioResult::failure(
                  error("AGENT_HOST_UNAVAILABLE",
                        "native Studio host is no longer available")));
              return;
            }
            std::lock_guard lock(lifetime->mutex);
            if (!lifetime->owner) {
              complete(StudioResult::failure(
                  error("AGENT_HOST_UNAVAILABLE",
                        "native Studio host is no longer available")));
              return;
            }
            lifetime->owner->dispatchNow(
                kind, std::move(request), std::move(gate), cancellation,
                std::move(complete));
          });
    } catch (const std::exception& failure) {
      complete(StudioResult::failure(
          error("AGENT_TOOL_FAILED", failure.what())));
    } catch (...) {
      complete(StudioResult::failure(error(
          "AGENT_TOOL_FAILED",
          "native Studio scheduling failed before execution")));
    }
    return;
  }
  dispatchNow(kind, request, gate, cancellation, std::move(callback));
}

void CrowdyStudioControllerHostAdapter::dispatchNow(
    StudioNativeToolKindV1 kind, StudioNativeToolRequestV1 request,
    agent::ValidatedStudioGateV1 gate,
    player_host::CancellationTokenV1 cancellation,
    agent::StudioToolCallbackV1 callback) {
  const auto initialGeneration =
      generation_.load(std::memory_order_acquire);
  bool effectStarted = false;
  try {
    if (const auto validation =
            validateGate(kind, request, gate, cancellation)) {
      callback(StudioResult::failure(*validation));
      return;
    }
    StudioNativeToolOutputV1 output =
        execute(kind, request, gate, cancellation, effectStarted);
    const bool cancelled = cancellation.cancelled();
    const bool preempted =
        initialGeneration != generation_.load(std::memory_order_acquire);
    const bool stale =
        kind != StudioNativeToolKindV1::ProjectSelect &&
        !gateStillCurrent(gate);
    if (cancelled || preempted || stale) {
      const bool ambiguous = effectStarted;
      callback(StudioResult::failure(
          error(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                          : cancelled ? "AGENT_CANCELLED"
                                      : "AGENT_CONTEXT_STALE",
                ambiguous
                    ? "native Studio result was fenced after an effect"
                    : cancelled
                          ? "native Studio operation was cancelled"
                          : "native Studio context changed before completion"),
          ambiguous));
      return;
    }
    callback(StudioResult::success(std::move(output)));
  } catch (const AdapterFailure& failure) {
    bool fenced =
        cancellation.cancelled() ||
        initialGeneration != generation_.load(std::memory_order_acquire);
    if (!fenced && kind != StudioNativeToolKindV1::ProjectSelect) {
      try {
        fenced = !gateStillCurrent(gate);
      } catch (...) {
        fenced = effectStarted;
      }
    }
    if (effectStarted && (fenced || failure.ambiguous())) {
      callback(StudioResult::failure(
          error("AGENT_TOOL_OUTCOME_UNKNOWN",
                failure.ambiguous()
                    ? failure.what()
                    : "native Studio result was fenced after an effect"),
          true));
    } else {
      callback(StudioResult::failure(
          failure.value(), failure.ambiguous()));
    }
  } catch (const CrowdyStudioRevisionConflictError& failure) {
    callback(StudioResult::failure(
        error(effectStarted ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                            : "AGENT_CONTEXT_STALE",
              failure.what()),
        effectStarted));
  } catch (const std::invalid_argument& failure) {
    callback(StudioResult::failure(
        error(effectStarted ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                            : "AGENT_TOOL_INPUT_INVALID",
              failure.what()),
        effectStarted));
  } catch (const std::exception& failure) {
    callback(StudioResult::failure(
        error(effectStarted ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                            : "AGENT_TOOL_FAILED",
              failure.what()),
        effectStarted));
  } catch (...) {
    callback(StudioResult::failure(
        error(effectStarted ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                            : "AGENT_TOOL_FAILED",
              "native Studio host operation failed"),
        effectStarted));
  }
}

void CrowdyStudioControllerHostAdapter::clearAgentOperation(
    player_host::PreemptionReasonV1 reason) noexcept {
  generation_.fetch_add(1, std::memory_order_acq_rel);
  try {
    controller_.cancelAgentOperation(preemptionMessage(reason));
  } catch (...) {
  }
}

std::optional<AgentErrorV1>
CrowdyStudioControllerHostAdapter::validateGate(
    StudioNativeToolKindV1 kind, const StudioNativeToolRequestV1& request,
    const agent::ValidatedStudioGateV1& gate,
    const player_host::CancellationTokenV1& cancellation) const {
  if (disposed_.load(std::memory_order_acquire)) {
    return error("AGENT_HOST_UNAVAILABLE",
                 "native Studio host is disposed");
  }
  if (cancellation.cancelled()) {
    return error("AGENT_CANCELLED",
                 "native Studio operation was cancelled");
  }
  if (gate.session_id.empty() || gate.run_id.empty() ||
      gate.tool_call_id.empty() || gate.client_epoch.empty() ||
      gate.context_version.empty() ||
      core::parseIso8601Millis(gate.validated_at.data(),
                               gate.validated_at.size()) <= 0) {
    return error("AGENT_TOOL_INPUT_INVALID",
                 "validated Studio gate is incomplete");
  }
  if (!options_.sessionId || !options_.clientEpoch) {
    return error(
        "AGENT_HOST_UNAVAILABLE",
        "Studio host requires current session and epoch providers");
  }
  const auto currentSession = options_.sessionId();
  if (!currentSession || *currentSession != gate.session_id) {
    return error("AGENT_SESSION_NOT_FOUND",
                 "Studio gate belongs to another session");
  }
  const auto currentEpoch = options_.clientEpoch();
  if (!currentEpoch || *currentEpoch != gate.client_epoch) {
    return error("AGENT_CLIENT_EPOCH_STALE",
                 "Studio gate belongs to a stale client epoch");
  }
  const std::string currentContext =
      options_.contextVersion ? options_.contextVersion()
                              : controller_.getAgentContext().contextVersion;
  if (currentContext.empty() ||
      currentContext != gate.context_version) {
    return error("AGENT_CONTEXT_STALE",
                 "Studio gate belongs to a stale context");
  }
  if (const auto lease = requiredLease(kind, request)) {
    if (!gate.lease_id) {
      return error("AGENT_LEASE_REQUIRED",
                   "runtime operation requires its current lease");
    }
    if (!options_.isLeaseActive ||
        !options_.isLeaseActive(*gate.lease_id, *lease)) {
      return error("AGENT_LEASE_REVOKED",
                   "runtime lease is no longer active");
    }
  }
  if (approvalRequired(kind, request)) {
    if (!gate.approval_grant || gate.approval_grant->empty()) {
      return error("AGENT_APPROVAL_REQUIRED",
                   "runtime operation requires exact human approval");
    }
    if (options_.validateApprovalGrant &&
        !options_.validateApprovalGrant(kind, request, gate)) {
      return error("AGENT_APPROVAL_MISMATCH",
                   "approval does not bind this exact runtime call");
    }
    if (kind == StudioNativeToolKindV1::RuntimeInvoke &&
        !options_.validateApprovalGrant) {
      return error("AGENT_APPROVAL_MISMATCH",
                   "LIVE invoke requires a final host approval validator");
    }
  }
  return std::nullopt;
}

StudioNativeToolOutputV1 CrowdyStudioControllerHostAdapter::execute(
    StudioNativeToolKindV1 kind, const StudioNativeToolRequestV1& request,
    const agent::ValidatedStudioGateV1& gate,
    const player_host::CancellationTokenV1& cancellation,
    bool& effectStarted) {
  const auto markEffectStarted = [&] {
    if (cancellation.cancelled()) {
      fail("AGENT_CANCELLED",
           "native Studio operation was cancelled before its effect");
    }
    effectStarted = true;
    cancellation.markEffectStarted();
  };
  switch (kind) {
    case StudioNativeToolKindV1::ContextGet:
      return contextProjection();
    case StudioNativeToolKindV1::StateGet:
      return stateProjection();
    case StudioNativeToolKindV1::ProjectSelect: {
      const auto* input =
          std::get_if<agent::StudioProjectSelectRequestV1>(&request);
      if (!input) {
        fail("AGENT_TOOL_INPUT_INVALID",
             "project.select requires a project reference");
      }
      controller_.switchProject(input->project_ref);
      const auto& project = controller_.getState().project;
      if (!project) {
        fail("AGENT_CONTEXT_CHANGED",
             "selected project is not available", true);
      }
      return agent::StudioProjectSelectResultV1{
          .selected_project_ref = project->projectId,
          .revision = project->revision.id,
      };
    }
    case StudioNativeToolKindV1::WorkspaceTabOpen:
    case StudioNativeToolKindV1::WorkspaceTabClose: {
      const auto* input =
          std::get_if<agent::StudioFileTabRequestV1>(&request);
      if (!input) {
        fail("AGENT_TOOL_INPUT_INVALID",
             "workspace tab operation requires a file reference");
      }
      CrowdyStudioFileRef reference{
          source(input->source), target(input->target), input->path,
          input->reference_ref};
      if (kind == StudioNativeToolKindV1::WorkspaceTabOpen) {
        controller_.openFile(reference);
      } else {
        controller_.closeFile(reference);
      }
      return agent::StudioOkV1{.ok = true};
    }
    case StudioNativeToolKindV1::DiagnosticsLocalGet:
      return diagnosticsProjection();
    case StudioNativeToolKindV1::RuntimeStatusGet:
      return runtimeProjection();
    case StudioNativeToolKindV1::RuntimeTestDraft: {
      const auto* input =
          std::get_if<agent::StudioRuntimeTestDraftRequestV1>(&request);
      if (!input) {
        fail("AGENT_TOOL_INPUT_INVALID",
             "runtime.test_draft requires an exact plan");
      }
      if (controller_.getState().saveState !=
          CrowdyStudioSaveState::Saved) {
        fail("AGENT_CONTEXT_STALE",
             "project must be saved before testing a draft");
      }
      const CrowdyStudioDeploymentPlan exact =
          controller_.makeDeploymentPlan();
      const auto requestedTargets = targets(input->targets);
      if (input->expected_revision != exact.expectedRevisionId ||
          !sameSet(requestedTargets, exact.targets)) {
        fail("AGENT_CONTEXT_STALE",
             "draft plan no longer matches the complete project");
      }
      const std::uint64_t operation = controller_.beginAgentOperation();
      if (cancellation.cancelled()) {
        controller_.cancelAgentOperation();
        fail("AGENT_CANCELLED", "draft test was cancelled");
      }
      const auto result = controller_.testDraftPlan(
          exact, operation, markEffectStarted);
      if (result.status != CrowdyStudioDeployResult::Status::Running) {
        const bool ambiguous =
            effectStarted &&
            result.status == CrowdyStudioDeployResult::Status::Failed;
        fail(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                       : "AGENT_TOOL_FAILED",
             result.message, ambiguous);
      }
      return agent::StudioRuntimePlanResultV1{
          .runtime = runtimeProjection(),
          .targets = targets(result.targets),
      };
    }
    case StudioNativeToolKindV1::RuntimeDeployLive: {
      const auto* input =
          std::get_if<agent::StudioRuntimeDeployLiveRequestV1>(&request);
      if (!input) {
        fail("AGENT_TOOL_INPUT_INVALID",
             "runtime.deploy_live requires an exact plan");
      }
      if (controller_.getState().saveState !=
          CrowdyStudioSaveState::Saved) {
        fail("AGENT_CONTEXT_STALE",
             "project must be saved before deploying live");
      }
      const CrowdyStudioDeploymentPlan exact =
          controller_.makeDeploymentPlan();
      const auto requestedTargets = targets(input->targets);
      if (input->draft ||
          input->expected_revision != exact.expectedRevisionId ||
          input->project_content_hash !=
              exact.projectContentHash.value_or("") ||
          pairing(input->pairing_preference) !=
              exact.pairingPreference.value_or(
                  CrowdyStudioPairingPreference::None) ||
          !sameSet(requestedTargets, exact.targets)) {
        fail("AGENT_CONTEXT_STALE",
             "live plan hash, revision, pairing, or targets changed");
      }
      const std::uint64_t operation = controller_.beginAgentOperation();
      if (cancellation.cancelled()) {
        controller_.cancelAgentOperation();
        fail("AGENT_CANCELLED", "live deployment was cancelled");
      }
      const auto result = controller_.deployLivePlan(
          exact, gate.approval_grant.value_or(""), operation,
          markEffectStarted);
      if (result.status != CrowdyStudioDeployResult::Status::Running) {
        const bool ambiguous =
            effectStarted &&
            result.status == CrowdyStudioDeployResult::Status::Failed;
        fail(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                       : "AGENT_TOOL_FAILED",
             result.message, ambiguous);
      }
      return agent::StudioRuntimePlanResultV1{
          .runtime = runtimeProjection(),
          .targets = targets(result.targets),
      };
    }
    case StudioNativeToolKindV1::RuntimeInvoke: {
      const auto* input =
          std::get_if<agent::StudioRuntimeInvokeRequestV1>(&request);
      if (!input) {
        fail("AGENT_TOOL_INPUT_INVALID",
             "runtime.invoke requires typed invocation arguments");
      }
      const CrowdyStudioDeployment deployment =
          input->environment == StudioRuntimeEnvironmentV1::Draft
              ? CrowdyStudioDeployment::Draft
              : CrowdyStudioDeployment::Live;
      const auto& sync = controller_.getState().runtimeSync;
      if (sync.state != CrowdyStudioRuntimeSyncState::RunningSaved ||
          !sync.deployment || *sync.deployment != deployment) {
        fail("AGENT_CONTEXT_STALE",
             "no exact saved runtime environment is currently running");
      }
      graphql::JVal params = graphql::JVal::object({});
      for (const auto& parameter : input->params) {
        if (parameter.type ==
            agent::StudioRuntimeParameterTypeV1::Boolean) {
          params[parameter.name] = parameter.value == "true";
        } else {
          params[parameter.name] = parameter.value;
        }
      }
      const std::uint64_t operation = controller_.beginAgentOperation();
      if (cancellation.cancelled()) {
        controller_.cancelAgentOperation();
        fail("AGENT_CANCELLED", "runtime invocation was cancelled");
      }
      const auto result = controller_.invoke(
          input->export_name, params.dump(), deployment, operation,
          markEffectStarted);
      if (result.durationUs < 0 ||
          static_cast<std::uint64_t>(result.durationUs) >
              std::numeric_limits<std::uint32_t>::max()) {
        fail("AGENT_TOOL_OUTPUT_INVALID",
             "runtime invocation duration is outside contract bounds", true);
      }
      agent::StudioRuntimeInvokeResultV1 mapped;
      if (result.resultJson && !result.resultJson->empty()) {
        mapped.result_type = agent::StudioRuntimeResultTypeV1::Text;
        mapped.result = *result.resultJson;
      } else if (result.resultBase64 && !result.resultBase64->empty()) {
        mapped.result_type = agent::StudioRuntimeResultTypeV1::Base64;
        mapped.result = *result.resultBase64;
      }
      mapped.fuel_used =
          result.fuelUsed.empty() ? "0" : result.fuelUsed;
      mapped.duration_us =
          static_cast<std::uint32_t>(result.durationUs);
      return mapped;
    }
    case StudioNativeToolKindV1::RuntimeStop: {
      if (cancellation.cancelled()) {
        fail("AGENT_CANCELLED", "runtime stop was cancelled");
      }
      markEffectStarted();
      const auto result = controller_.stopProject();
      return agent::StudioRuntimeStopResultV1{
          .server_stopped = result.serverStopped.value_or(true),
          .client_stopped = result.clientStopped.value_or(true),
          .failures = result.failures,
      };
    }
  }
  fail("AGENT_TOOL_UNKNOWN", "unknown native Studio tool");
}

agent::StudioContextV1
CrowdyStudioControllerHostAdapter::contextProjection() const {
  const auto controllerContext = controller_.getAgentContext();
  const auto& state = controller_.getState();
  agent::StudioContextV1 value;
  value.app_ref = controllerContext.appRef;
  value.project_ref = controllerContext.projectRef;
  value.grid_ref = controllerContext.gridRef;
  value.context_version =
      options_.contextVersion ? options_.contextVersion()
                              : controllerContext.contextVersion;
  value.save_state = saveState(state.saveState);
  value.runtime = runtimeProjection();
  if (options_.clientEpoch) value.client_epoch = options_.clientEpoch();
  if (options_.leaseKinds) value.lease_kinds = options_.leaseKinds();
  std::sort(value.lease_kinds.begin(), value.lease_kinds.end());
  value.lease_kinds.erase(
      std::unique(value.lease_kinds.begin(), value.lease_kinds.end()),
      value.lease_kinds.end());
  if (options_.hostCapabilityRevision) {
    value.host_capability_revision =
        options_.hostCapabilityRevision();
  }
  return value;
}

agent::StudioStateV1
CrowdyStudioControllerHostAdapter::stateProjection() const {
  const auto& state = controller_.getState();
  agent::StudioStateV1 value;
  if (state.project) {
    agent::StudioProjectProjectionV1 project;
    project.project_id = state.project->projectId;
    project.name = state.project->metadata.name;
    project.description = state.project->metadata.description;
    project.kind = projectKind(state.project->kind);
    project.revision = state.project->revision.id;
    project.server_module_name =
        state.project->metadata.serverModuleName;
    project.client_module_name =
        state.project->metadata.clientModuleName;
    project.pairing_preference =
        pairing(state.project->metadata.pairingPreference);
    project.updated_at = state.project->updatedAt;
    project.files.reserve(state.project->files.size());
    for (const auto& file : state.project->files) {
      if (file.content.size() >
          std::numeric_limits<std::uint32_t>::max()) {
        fail("AGENT_TOOL_OUTPUT_INVALID",
             "project file exceeds native projection bounds");
      }
      project.files.push_back({
          .target = target(file.target),
          .path = file.path,
          .content_hash = sha256(file.content),
          .byte_length =
              static_cast<std::uint32_t>(file.content.size()),
      });
    }
    value.project = std::move(project);
  }
  for (const auto& file : state.openFiles) {
    if (!file.target) continue;
    value.open_files.push_back({
        .source = source(file.source),
        .target = target(*file.target),
        .path = file.path,
    });
  }
  value.save_state = saveState(state.saveState);
  value.runtime = runtimeProjection();
  return value;
}

agent::StudioRuntimeStatusV1
CrowdyStudioControllerHostAdapter::runtimeProjection() const {
  const auto& state = controller_.getState();
  agent::StudioRuntimeStatusV1 value;
  value.phase = phase(state.runtime.phase);
  value.saved_revision =
      state.runtimeSync.savedRevisionId.value_or(
          state.project ? state.project->revision.id : "0");
  value.running_revision = state.runtimeSync.runningRevisionId;
  value.sync = runtimeSync(state.runtimeSync.state);
  if (state.runtime.target) value.target = target(*state.runtime.target);
  if (state.runtimeSync.deployment) {
    value.draft =
        *state.runtimeSync.deployment == CrowdyStudioDeployment::Draft;
  }
  if (!state.runtime.message.empty()) value.message = state.runtime.message;
  return value;
}

agent::StudioDiagnosticsV1
CrowdyStudioControllerHostAdapter::diagnosticsProjection() const {
  return crowdyStudioAgentLocalDiagnosticsV1(controller_.getState());
}

std::string CrowdyStudioControllerHostAdapter::sha256(
    std::string_view value) const {
  std::array<std::uint8_t, 32> digest{};
  if (!crypto_.sha256(asBytes(value), digest.data())) {
    fail("AGENT_HOST_UNAVAILABLE",
         "native Studio SHA-256 provider failed");
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const std::uint8_t byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

bool CrowdyStudioControllerHostAdapter::gateStillCurrent(
    const agent::ValidatedStudioGateV1& gate) const {
  if (options_.sessionId) {
    const auto current = options_.sessionId();
    if (!current || *current != gate.session_id) return false;
  }
  if (options_.clientEpoch) {
    const auto current = options_.clientEpoch();
    if (!current || *current != gate.client_epoch) return false;
  }
  const std::string context =
      options_.contextVersion ? options_.contextVersion()
                              : controller_.getAgentContext().contextVersion;
  return context == gate.context_version;
}

}  // namespace crowdy::studio
