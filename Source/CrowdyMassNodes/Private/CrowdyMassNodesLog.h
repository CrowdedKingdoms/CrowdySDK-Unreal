#pragma once

#include "CoreMinimal.h"

// Log category for the authoring-time nodes. Kept separate from the runtime LogCrowdyMass category
// because a log category object is not exported across module boundaries.
DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyMassNodes, Log, All);

namespace CrowdyMassNodesTrace
{
	// True while the crowdy.mass.nodes.trace console variable is set. Gates the informational lines
	// that explain where the event dropdown's entries came from; warnings and errors are always on.
	bool IsEnabled();
}
