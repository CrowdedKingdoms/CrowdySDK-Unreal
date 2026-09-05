#pragma once

#include "CoreMinimal.h"

CROWDYCPPBRIDGE_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyCpp, Log, All);

/**
 * The single boundary between Unreal and the vendored CrowdyCPP library.
 *
 * Every call into CrowdyCPP is expected to funnel through this class so its
 * exception-based error model stays contained and never escapes into the
 * engine. This header deliberately exposes only Unreal types; the CrowdyCPP
 * headers stay private to the bridge's translation units, so dependent modules
 * neither need the CrowdyCPP include path nor exception support.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppBridge
{
public:
	/**
	 * Exercises a spread of CrowdyCPP entry points inside a guarded scope to
	 * prove the library links and that its exceptions are contained. Performs
	 * no network I/O and is safe to call at runtime. Returns true when every
	 * step completes without an exception escaping.
	 */
	static bool SelfTest();
};
