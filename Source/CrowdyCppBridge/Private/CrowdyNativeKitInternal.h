#pragma once

#include "CrowdyNativeKit.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/client.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/json.hpp"
THIRD_PARTY_INCLUDES_END

#include <memory>
#include <string>

namespace CrowdyNativeKit
{
	// The kit over Connection, which it never closes. Null when CrowdyCPP refuses to build it. Game thread only.
	TSharedPtr<FCrowdyNativeKit> Make(crowdy::CrowdyClient& Client, std::string AppId,
		std::shared_ptr<crowdy::domains::ExecConnection> Connection, std::string PlayerId);

	// One MessagePack value as a JVal, keeping integers and floats apart. False for malformed or trailing bytes, binary,
	// extension types, a map key that is not a string, an integer past int64, or nesting deeper than 64.
	bool ArgsFromMsgpack(const TArray<uint8>& Bytes, crowdy::graphql::JVal& Out);
}
