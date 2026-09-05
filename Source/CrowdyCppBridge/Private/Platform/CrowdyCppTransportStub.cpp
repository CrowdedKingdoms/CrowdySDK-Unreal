#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/graphql/http.hpp"
THIRD_PARTY_INCLUDES_END

// Satisfies the client's reference to the default libcurl transport, which is not compiled into the bridge.
//
// Returning null here is deliberate. The client keeps two transport slots: a synchronous one and an asynchronous
// one. This integration fills only the asynchronous slot, with an FHttpModule-backed transport, because no code
// here may block the game thread on an HTTP round trip. The synchronous slot is left empty on purpose.
//
// The client's constructor initialises its synchronous transport unconditionally, so this runs once per client
// even though nothing goes on to use it. That is why the message below is informational rather than a warning:
// it records that the slot is empty, it does not report a fault. A blocking GraphQL call would be the fault, and
// the client reports that separately by refusing the call when the slot is null.
namespace crowdy::graphql
{
	std::shared_ptr<IHttpTransport> makeCurlTransport()
	{
		UE_LOG(LogCrowdyCpp, Log,
			TEXT("No synchronous HTTP transport is vendored. This is expected: the integration is asynchronous only and supplies its transport through ClientConfig::asyncTransport. Blocking GraphQL calls are unsupported and will be refused."));
		return nullptr;
	}
}
