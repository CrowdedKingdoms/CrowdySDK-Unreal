#include "CrowdyNet.h"
#include "CrowdyNetLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"

#if !UE_BUILD_SHIPPING
#include "Debug/CrowdyNetStatsDisplay.h"
#include "Engine/Engine.h"
#include "Misc/CoreDelegates.h"
#endif

IMPLEMENT_MODULE(FCrowdyNetModule, CrowdyNet)

DEFINE_LOG_CATEGORY(LogCrowdyNet);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyNetTrace, TEXT("crowdy.net.trace"),
		TEXT("When non-zero, logs CrowdyNet UDP transport activity: socket open/close, message ")
		TEXT("sends and receives, worker-thread pool, and buffer pool churn. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyQueryTrace, TEXT("crowdy.query.trace"),
		TEXT("When non-zero, logs CrowdyNet GraphQL activity: query dispatch and responses, and ")
		TEXT("live subscription lifecycle (start/data/error/stop). Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdySerializeTrace, TEXT("crowdy.serialize.trace"),
		TEXT("When non-zero, logs CrowdyNet payload serialization: message encode/decode and ")
		TEXT("payload/class registry resolves. High frequency — off by default."));
}

// GetValueOnAnyThread: CrowdyNet logs from the UDP listener and worker-thread pool, not just the
// game thread, so the trace gates must be readable from any thread.
bool CrowdyNetTrace::Net()       { return CVarCrowdyNetTrace.GetValueOnAnyThread() != 0; }
bool CrowdyNetTrace::Query()     { return CVarCrowdyQueryTrace.GetValueOnAnyThread() != 0; }
bool CrowdyNetTrace::Serialize() { return CVarCrowdySerializeTrace.GetValueOnAnyThread() != 0; }

#if !UE_BUILD_SHIPPING
namespace
{
	// Handle for the deferred stat registration. CrowdyNet loads at the Default phase, before
	// GEngine exists, so registration waits for OnPostEngineInit.
	FDelegateHandle GCrowdyNetStatsInitHandle;
}
#endif

void FCrowdyNetModule::StartupModule()
{
#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		// Loaded after engine init register straight away.
		CrowdyNetStats::Register();
	}
	else
	{
		GCrowdyNetStatsInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&CrowdyNetStats::Register);
	}
#endif
}

void FCrowdyNetModule::ShutdownModule()
{
#if !UE_BUILD_SHIPPING
	if (GCrowdyNetStatsInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(GCrowdyNetStatsInitHandle);
		GCrowdyNetStatsInitHandle.Reset();
	}
	CrowdyNetStats::Unregister();
#endif
}
