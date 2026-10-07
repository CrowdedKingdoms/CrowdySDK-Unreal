#pragma once

#include "CrowdyNativeExec.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/domains/exec.hpp"
THIRD_PARTY_INCLUDES_END

#include <memory>

namespace CrowdyNativeExec
{
	TSharedRef<FCrowdyNativeExecConnection> Wrap(std::shared_ptr<crowdy::domains::ExecConnection> Connection);
}
