#pragma once

#include "CrowdyNativeExec.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/domains/exec.hpp"
THIRD_PARTY_INCLUDES_END

#include <memory>

namespace CrowdyNativeExec
{
	/**
	 * DialRefusal is what the Game API said when it last refused to name a host, empty once one is named; RefusedForGood
	 * is set while the last dial was refused for a reason no retry can fix. Game thread only.
	 */
	TSharedRef<FCrowdyNativeExecConnection> Wrap(std::shared_ptr<crowdy::domains::ExecConnection> Connection, std::shared_ptr<FString> DialRefusal,
		std::shared_ptr<bool> RefusedForGood);

	FCrowdyNativeExecReply ReplyFrom(const crowdy::domains::ExecReply& Reply);
}
