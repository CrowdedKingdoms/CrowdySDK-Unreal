#include "CrowdyExecLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCrowdyExec);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyExecTrace, TEXT("crowdy.exec.trace"),
		TEXT("When non-zero, logs Server Object activity: status changes, applied reads and pushes with their epoch and seq, ")
		TEXT("re-reads, subscribes, calls and the connection. Never values or tokens. Off by default."));
}

bool CrowdyExecTrace::Exec() { return CVarCrowdyExecTrace.GetValueOnAnyThread() != 0; }

IMPLEMENT_MODULE(FDefaultModuleImpl, CrowdyExec);
