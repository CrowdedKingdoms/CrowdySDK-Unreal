#include "CrowdyReplication.h"
#include "CrowdyReplicationLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, CrowdyReplication)

DEFINE_LOG_CATEGORY(LogCrowdyReplication);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyEntityTrace, TEXT("crowdy.entity.trace"),
		TEXT("When non-zero, logs CrowdyReplication entity activity: registry add/remove, event ")
		TEXT("routing and dispatch, entity components, and actor tracking/management. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyPoolTrace, TEXT("crowdy.pool.trace"),
		TEXT("When non-zero, logs CrowdyReplication actor-pool activity: spawn, release, reuse, and ")
		TEXT("rendering-backend churn. Off by default."));
}

// GetValueOnAnyThread: replication and pool code can log off the game thread.
bool CrowdyReplicationTrace::Entity() { return CVarCrowdyEntityTrace.GetValueOnAnyThread() != 0; }
bool CrowdyReplicationTrace::Pool()   { return CVarCrowdyPoolTrace.GetValueOnAnyThread() != 0; }
