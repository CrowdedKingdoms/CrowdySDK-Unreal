#pragma once

#include <string>

#include "crowdy/agent/native_tool_dispatcher.hpp"
#include "crowdy/agent/types.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/studio/controller.hpp"

namespace crowdy::studio {

/// Canonical common-subset projection used by native runtime.status.get,
/// studio.context.get, and studio.state.get host outputs. Native-only content
/// hash/module/pairing fields remain in CrowdyStudioRuntimeSync but do not
/// change the CrowdyJS-compatible v1 tool shape.
agent::StudioRuntimeStatusV1 crowdyStudioAgentRuntimeV1(
    const CrowdyStudioState& state);

/// Build the exact typed studio.state.get payload, including deterministic
/// file content hashes. This is a data projection only and grants no tool or
/// network authority.
agent::StudioStateV1 crowdyStudioAgentStateV1(
    const CrowdyStudioState& state, const core::ICrypto& crypto);

/// Build the exact typed diagnostics.local.get payload.
agent::StudioDiagnosticsV1 crowdyStudioAgentLocalDiagnosticsV1(
    const CrowdyStudioState& state);

/// Reference conversion seam for durable agent checkpoint events. The caller
/// supplies its already authenticated Studio scope; the returned event can be
/// passed to CrowdyStudioController::ingestCheckpointEvent.
CrowdyStudioCheckpointEvent crowdyStudioCheckpointEventFromAgentV1(
    const agent::AgentCheckpoint& checkpoint,
    CrowdyStudioProjectScope scope, std::string projectId);

}  // namespace crowdy::studio
