#include "CrowdyGameModelLog.h"
#include "CrowdyLog.h"

DEFINE_LOG_CATEGORY(LogCrowdyGameModel);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyGameModelTrace, TEXT("crowdy.gamemodel.trace"),
		TEXT("When non-zero, logs CrowdyReplication Game Model activity: identity resolution, invoke ")
		TEXT("client, container cache/diffing, and effect lowering. Off by default."));
}

bool CrowdyGameModelTrace::GameModel() { return CVarCrowdyGameModelTrace.GetValueOnAnyThread() != 0; }
