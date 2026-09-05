#include "crowdy/studio/agent_projection.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace crowdy::studio {
namespace {

agent::StudioRuntimePhaseV1 phase(CrowdyStudioPhase value) {
  switch (value) {
    case CrowdyStudioPhase::Idle:
      return agent::StudioRuntimePhaseV1::Idle;
    case CrowdyStudioPhase::TestingDraft:
      return agent::StudioRuntimePhaseV1::TestingDraft;
    case CrowdyStudioPhase::DeployingLive:
      return agent::StudioRuntimePhaseV1::DeployingLive;
    case CrowdyStudioPhase::Compiling:
      return agent::StudioRuntimePhaseV1::Compiling;
    case CrowdyStudioPhase::Enabling:
      return agent::StudioRuntimePhaseV1::Enabling;
    case CrowdyStudioPhase::Running:
      return agent::StudioRuntimePhaseV1::Running;
    case CrowdyStudioPhase::CompileFailed:
      return agent::StudioRuntimePhaseV1::CompileFailed;
    case CrowdyStudioPhase::Stopping:
      return agent::StudioRuntimePhaseV1::Stopping;
    case CrowdyStudioPhase::Stopped:
      return agent::StudioRuntimePhaseV1::Stopped;
    case CrowdyStudioPhase::PartialFailure:
      return agent::StudioRuntimePhaseV1::PartialFailure;
    case CrowdyStudioPhase::Error:
      return agent::StudioRuntimePhaseV1::Error;
  }
  return agent::StudioRuntimePhaseV1::Error;
}

agent::StudioRuntimeSyncV1 sync(CrowdyStudioRuntimeSyncState value) {
  switch (value) {
    case CrowdyStudioRuntimeSyncState::NeverRun:
      return agent::StudioRuntimeSyncV1::NeverRun;
    case CrowdyStudioRuntimeSyncState::RunningSaved:
      return agent::StudioRuntimeSyncV1::RunningSaved;
    case CrowdyStudioRuntimeSyncState::RunningStale:
      return agent::StudioRuntimeSyncV1::RunningStale;
    case CrowdyStudioRuntimeSyncState::Stopped:
      return agent::StudioRuntimeSyncV1::Stopped;
  }
  return agent::StudioRuntimeSyncV1::NeverRun;
}

agent::StudioTargetV1 target(CrowdyStudioTarget value) {
  return value == CrowdyStudioTarget::Server
             ? agent::StudioTargetV1::Server
             : agent::StudioTargetV1::Client;
}

agent::StudioSaveStateV1 saveState(CrowdyStudioSaveState value) {
  switch (value) {
    case CrowdyStudioSaveState::Saving:
      return agent::StudioSaveStateV1::Saving;
    case CrowdyStudioSaveState::Saved:
      return agent::StudioSaveStateV1::Saved;
    case CrowdyStudioSaveState::Conflict:
      return agent::StudioSaveStateV1::Conflict;
    case CrowdyStudioSaveState::Offline:
      return agent::StudioSaveStateV1::Offline;
  }
  return agent::StudioSaveStateV1::Offline;
}

agent::StudioProjectKindV1 projectKind(CrowdyStudioProjectKind value) {
  switch (value) {
    case CrowdyStudioProjectKind::Server:
      return agent::StudioProjectKindV1::Server;
    case CrowdyStudioProjectKind::Client:
      return agent::StudioProjectKindV1::Client;
    case CrowdyStudioProjectKind::FullStack:
      return agent::StudioProjectKindV1::FullStack;
  }
  return agent::StudioProjectKindV1::Server;
}

agent::StudioPairingPreferenceV1 pairing(
    CrowdyStudioPairingPreference value) {
  switch (value) {
    case CrowdyStudioPairingPreference::None:
      return agent::StudioPairingPreferenceV1::None;
    case CrowdyStudioPairingPreference::Optional:
      return agent::StudioPairingPreferenceV1::Optional;
    case CrowdyStudioPairingPreference::Required:
      return agent::StudioPairingPreferenceV1::Required;
  }
  return agent::StudioPairingPreferenceV1::None;
}

agent::StudioFileSourceV1 source(CrowdyStudioFileRef::Source value) {
  switch (value) {
    case CrowdyStudioFileRef::Source::Project:
      return agent::StudioFileSourceV1::Project;
    case CrowdyStudioFileRef::Source::PersonalLibrary:
      return agent::StudioFileSourceV1::PersonalLibrary;
    case CrowdyStudioFileRef::Source::Common:
      return agent::StudioFileSourceV1::Common;
  }
  return agent::StudioFileSourceV1::Project;
}

agent::StudioDiagnosticSeverityV1 diagnosticSeverity(
    CrowdyStudioDiagnosticSeverity value) {
  switch (value) {
    case CrowdyStudioDiagnosticSeverity::Error:
      return agent::StudioDiagnosticSeverityV1::Error;
    case CrowdyStudioDiagnosticSeverity::Warning:
      return agent::StudioDiagnosticSeverityV1::Warning;
    case CrowdyStudioDiagnosticSeverity::Info:
      return agent::StudioDiagnosticSeverityV1::Info;
    case CrowdyStudioDiagnosticSeverity::Hint:
      return agent::StudioDiagnosticSeverityV1::Hint;
  }
  return agent::StudioDiagnosticSeverityV1::Info;
}

agent::StudioDiagnosticSourceV1 diagnosticSource(
    CrowdyStudioDiagnosticSource value) {
  switch (value) {
    case CrowdyStudioDiagnosticSource::Rustc:
      return agent::StudioDiagnosticSourceV1::Rustc;
    case CrowdyStudioDiagnosticSource::LocalAdvisory:
      return agent::StudioDiagnosticSourceV1::LocalAdvisory;
    case CrowdyStudioDiagnosticSource::Runtime:
      return agent::StudioDiagnosticSourceV1::Runtime;
    // ModelLint projects onto Runtime rather than widening the V1 wire enum.
    // That enum is versioned and shared with peers built against v1: adding a
    // value would produce a source those peers cannot decode, to say something
    // they have no separate handling for anyway. Runtime is also the honest
    // mapping -- the finding comes from the running server, not from the
    // compiler and not from a local check. A v2 can separate them if a consumer
    // ever needs to tell them apart.
    case CrowdyStudioDiagnosticSource::ModelLint:
      return agent::StudioDiagnosticSourceV1::Runtime;
  }
  return agent::StudioDiagnosticSourceV1::LocalAdvisory;
}

std::string sha256(std::string_view value, const core::ICrypto& crypto) {
  std::array<std::uint8_t, 32> digest{};
  if (!crypto.sha256(asBytes(value), digest.data())) {
    throw std::runtime_error(
        "Crowdy Studio agent projection SHA-256 failed");
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const std::uint8_t byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

agent::StudioProjectProjectionV1 projectProjection(
    const CrowdyStudioProject& project, const core::ICrypto& crypto) {
  if (project.files.size() > 128) {
    throw std::invalid_argument(
        "Crowdy Studio agent project exceeds the v1 file bound");
  }
  agent::StudioProjectProjectionV1 result;
  result.project_id = project.projectId;
  result.name = project.metadata.name;
  result.description = project.metadata.description;
  result.kind = projectKind(project.kind);
  result.revision = project.revision.id;
  result.server_module_name = project.metadata.serverModuleName;
  result.client_module_name = project.metadata.clientModuleName;
  result.pairing_preference =
      pairing(project.metadata.pairingPreference);
  result.updated_at = project.updatedAt;
  result.files.reserve(project.files.size());
  for (const auto& file : project.files) {
    if (file.content.size() > 1'048'576 ||
        file.content.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
      throw std::invalid_argument(
          "Crowdy Studio agent file exceeds the v1 byte bound");
    }
    result.files.push_back(
        {target(file.target), normalizeCrowdyStudioPath(file.path),
         sha256(file.content, crypto),
         static_cast<std::uint32_t>(file.content.size())});
  }
  return result;
}

}  // namespace

agent::StudioRuntimeStatusV1 crowdyStudioAgentRuntimeV1(
    const CrowdyStudioState& state) {
  agent::StudioRuntimeStatusV1 result;
  result.phase = phase(state.runtime.phase);
  result.saved_revision =
      state.runtimeSync.savedRevisionId
          ? *state.runtimeSync.savedRevisionId
          : state.project ? state.project->revision.id : "0";
  result.running_revision = state.runtimeSync.runningRevisionId;
  result.sync = sync(state.runtimeSync.state);
  if (state.runtime.target) result.target = target(*state.runtime.target);
  if (state.runtimeSync.deployment) {
    result.draft =
        *state.runtimeSync.deployment == CrowdyStudioDeployment::Draft;
  }
  if (!state.runtime.message.empty()) {
    result.message = state.runtime.message;
  }
  return result;
}

agent::StudioStateV1 crowdyStudioAgentStateV1(
    const CrowdyStudioState& state, const core::ICrypto& crypto) {
  if (state.openFiles.size() > 32) {
    throw std::invalid_argument(
        "Crowdy Studio agent state exceeds the v1 open-file bound");
  }
  agent::StudioStateV1 result;
  if (state.project) {
    result.project = projectProjection(*state.project, crypto);
  }
  for (const auto& file : state.openFiles) {
    if (!file.target) continue;
    result.open_files.push_back(
        {source(file.source), target(*file.target),
         normalizeCrowdyStudioPath(file.path)});
  }
  result.save_state = saveState(state.saveState);
  result.runtime = crowdyStudioAgentRuntimeV1(state);
  return result;
}

agent::StudioDiagnosticsV1 crowdyStudioAgentLocalDiagnosticsV1(
    const CrowdyStudioState& state) {
  if (state.localDiagnostics.size() >
      kCrowdyStudioDiagnosticMaxCount) {
    throw std::invalid_argument(
        "Crowdy Studio local diagnostics exceed the v1 output bound");
  }
  agent::StudioDiagnosticsV1 result;
  result.diagnostics.reserve(state.localDiagnostics.size());
  for (const auto& diagnostic : state.localDiagnostics) {
    agent::StudioDiagnosticV1 mapped;
    mapped.source = diagnosticSource(diagnostic.source);
    mapped.target = target(diagnostic.target);
    mapped.path = normalizeCrowdyStudioPath(diagnostic.path);
    mapped.line = diagnostic.line;
    mapped.column = diagnostic.column;
    mapped.severity = diagnosticSeverity(diagnostic.severity);
    mapped.code = diagnostic.code;
    mapped.message = diagnostic.message;
    result.diagnostics.push_back(std::move(mapped));
  }
  return result;
}

CrowdyStudioCheckpointEvent crowdyStudioCheckpointEventFromAgentV1(
    const agent::AgentCheckpoint& checkpoint,
    CrowdyStudioProjectScope scope, std::string projectId) {
  if (scope.appId.empty() || scope.gridId.empty() || projectId.empty()) {
    throw std::invalid_argument(
        "Agent checkpoint bridge requires an exact Studio scope");
  }
  CrowdyStudioCheckpointMetadata mapped;
  mapped.checkpointId = checkpoint.checkpointId;
  mapped.projectRevisionId = checkpoint.projectRevision;
  mapped.contentHash = checkpoint.contentHash;
  if (checkpoint.reason == "AGENT_WRITE") {
    mapped.reason = CrowdyStudioCheckpointMetadata::Reason::AgentWrite;
  } else if (checkpoint.reason == "RESTORE_PREIMAGE") {
    mapped.reason =
        CrowdyStudioCheckpointMetadata::Reason::RestorePreimage;
  } else if (checkpoint.reason == "MANUAL") {
    mapped.reason = CrowdyStudioCheckpointMetadata::Reason::Manual;
  } else {
    throw std::invalid_argument(
        "Agent checkpoint event has an unknown reason");
  }
  if (checkpoint.files.size() > 128) {
    throw std::invalid_argument(
        "Agent checkpoint event exceeds the Studio file bound");
  }
  mapped.files.reserve(checkpoint.files.size());
  for (const auto& file : checkpoint.files) {
    const auto mappedTarget = targetFromString(file.target);
    if (!mappedTarget || file.byteLength < 0 ||
        file.byteLength > 1'048'576) {
      throw std::invalid_argument(
          "Agent checkpoint event contains invalid file metadata");
    }
    mapped.files.push_back(
        {*mappedTarget, normalizeCrowdyStudioPath(file.path),
         file.contentHash, static_cast<std::size_t>(file.byteLength)});
  }
  mapped.createdAt = checkpoint.createdAt;
  mapped.restoredAt = checkpoint.restoredAt;
  return {std::move(scope), std::move(projectId), std::move(mapped)};
}

}  // namespace crowdy::studio
