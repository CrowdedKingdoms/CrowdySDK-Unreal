#pragma once

#include "CoreMinimal.h"

CROWDYEXEC_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyExec, Log, All);

namespace CrowdyExecTrace
{
	/** crowdy.exec.trace: Server Object status, applied reads and pushes, re-reads, subscribes, calls and the connection. */
	CROWDYEXEC_API bool Exec();
}
