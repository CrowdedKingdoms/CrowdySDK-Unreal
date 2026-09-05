#pragma once

#include "CoreMinimal.h"

namespace crowdy::core
{
	class ICrypto;
}

/**
 * The vendored CrowdyCPP library takes its crypto provider as a parameter to
 * every wire-codec call rather than linking one in, and the provider that
 * binds those calls to the engine's OpenSSL lives inside this module and is
 * not otherwise reachable. This accessor is what lets code in other modules
 * call the library's header-only wire codec (encodeLongSpatial,
 * verifyLongSpatial, and the rest) with a real provider.
 */
CROWDYCPPBRIDGE_API const crowdy::core::ICrypto& GetCrowdyCppCrypto();
