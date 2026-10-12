#pragma once

#include "CrowdyNativeExec.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/domains/exec.hpp"
THIRD_PARTY_INCLUDES_END

#include <memory>

namespace CrowdyNativeExec
{
	/** DialRefusal is what the Game API said when it last refused to name a host, empty once one is named; game thread only. */
	TSharedRef<FCrowdyNativeExecConnection> Wrap(std::shared_ptr<crowdy::domains::ExecConnection> Connection, std::shared_ptr<FString> DialRefusal);
}
