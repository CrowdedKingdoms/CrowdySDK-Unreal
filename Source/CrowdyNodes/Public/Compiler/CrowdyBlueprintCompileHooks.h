#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;

/**
 * The parts of the Crowdy Blueprint compile pass that only an editor can do, supplied by the editor module
 * rather than reached directly.
 *
 * The compile pass itself lives here, in an UncookedOnly module, because it must also run in an uncooked
 * non-editor process: those regenerate every Blueprint's bytecode on load, so a pass that only ran in the
 * editor would leave them with bodies that never had the replication gate spliced in. Work that needs the
 * persisted container marker, the cooked-registry baker or the live PIE registry cannot follow it here,
 * because those are defined in an editor-only module this one must not depend on. The editor installs them
 * at startup instead, and with nothing installed each call is a no-op, which is the correct behavior in a
 * game process where none of them has anything to act on.
 *
 * Same no-cycle hook pattern as CrowdyApplyEffectNodeShared::SetContainerBlueprintResolver.
 */
namespace CrowdyBlueprintCompileHooks
{
	// Stamps the Game Model container tags (the container type name and the pull-on-start answer) onto a
	// freshly compiled class, reading the Blueprint's persisted container marker. Installed by the editor
	// module; absent in a game process, where no marker can be loaded to read.
	CROWDYNODES_API void SetContainerStampHook(TFunction<void(const UBlueprint*, UClass*)> Hook);

	// Applies the installed container stamp, or does nothing when none is installed.
	CROWDYNODES_API void StampContainerMetadata(const UBlueprint* Blueprint, UClass* Class);

	// Announces that this class finished compiling, so the editor can update the cooked registry bake and
	// refresh a live PIE registry for just this class. Installed by the editor module.
	CROWDYNODES_API void SetClassCompiledHook(TFunction<void(UClass*)> Hook);

	// Runs the installed post-compile hook, or does nothing when none is installed.
	CROWDYNODES_API void NotifyClassCompiled(UClass* Class);
}
