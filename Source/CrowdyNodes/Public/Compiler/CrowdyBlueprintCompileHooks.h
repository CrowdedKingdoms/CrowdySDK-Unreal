#pragma once

#include "CoreMinimal.h"

class UClass;

/**
 * The parts of the Crowdy Blueprint compile pass that only an editor can do, supplied by the editor module
 * rather than reached directly.
 *
 * The compile pass itself lives here, in an UncookedOnly module, because it must also run in an uncooked
 * non-editor process: those regenerate every Blueprint's bytecode on load, so a pass that only ran in the
 * editor would leave them with bodies that never had the replication gate spliced in. Work that needs the
 * cooked-registry baker or the live PIE registry cannot follow it here, because those are defined in an
 * editor-only module this one must not depend on. The editor installs them at startup instead, and with
 * nothing installed each call is a no-op, which is the correct behavior in a game process where none of
 * them has anything to act on.
 */
namespace CrowdyBlueprintCompileHooks
{
	// Announces that this class finished compiling, so the editor can update the cooked registry bake and
	// refresh a live PIE registry for just this class. Installed by the editor module.
	CROWDYNODES_API void SetClassCompiledHook(TFunction<void(UClass*)> Hook);

	// Runs the installed post-compile hook, or does nothing when none is installed.
	CROWDYNODES_API void NotifyClassCompiled(UClass* Class);
}
