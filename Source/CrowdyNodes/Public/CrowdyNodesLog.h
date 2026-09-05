#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// This module's compile pass runs in the editor, in the cook commandlet, and in an uncooked non-editor
// process, so its diagnostics need a category that exists in all three. The editor module's LogCrowdyEditor
// does not: it lives in a module that never loads in a game process.
CROWDYNODES_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyNodes, Log, All);
