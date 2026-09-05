#pragma once

#include "CoreMinimal.h"
#include "Blueprint/BlueprintExtension.h"
#include "Engine/Blueprint.h" // full type for Within=Blueprint (mirrors UWidgetBlueprintExtension's Within=WidgetBlueprint)

#include "CrowdyContainerBlueprintExtension.generated.h"

/**
 * Editor-only marker that flags a Blueprint as a Crowdy Game Model container: the persistent authoring record
 * that this class's Server Owned variables should be discovered and synced as a server-authoritative container
 * type. Stored in UBlueprint::Extensions (the sanctioned per-Blueprint editor-only storage), so it saves with
 * the asset and survives editor restarts, yet is stripped at cook (a packaged build reads the baked registry).
 *
 * The container tag itself (meta=(CrowdyContainer=<type>)) is NOT stored on this object. The Blueprint compiler
 * extension stamps it onto the generated class from this marker on every compile, and removes it when the marker
 * is gone, the same opt-in-drives-the-tag pattern the entity component uses for the CrowdyEntity tag.
 */
UCLASS(Within = Blueprint)
class UCrowdyContainerBlueprintExtension : public UBlueprintExtension
{
	GENERATED_BODY()

public:
	// The server container type name for this class. Empty means "use the Blueprint's asset name" (see
	// CrowdyContainerMarker::ResolveTypeName): a non-empty value pins a stable type name independent of a later
	// asset rename, while leaving it empty lets a duplicated container re-derive a clean name from the copy.
	UPROPERTY()
	FString ContainerTypeName;

	// Whether a container of this class fetches its server state once as soon as it binds. Leave it on for state
	// that must be correct from the first frame; turn it off when the container is pulled on demand instead. It
	// does not stop later updates: once bound, a model-changed notification still re-pulls. This is a property of
	// the container type, so it applies to every instance of the class.
	UPROPERTY()
	bool bPullModelOnStart = true;
};

/**
 * Reads and writes the container marker on a Blueprint. Free functions (not methods on the UCLASS) so the pure
 * type-name resolution is unit-testable without an editor, and so the compiler extension, the variable-dropdown
 * gate, and the toolbar all resolve container-hood identically from one place.
 */
// What a Blueprint's class should carry for the pull-on-start tag. None removes it, which is what a class that is
// not a marked container gets; a marked container always states its answer so it cannot inherit a parent's.
enum class ECrowdyPullOnStartStamp : uint8
{
	None,
	On,
	Off
};

namespace CrowdyContainerMarker
{
	// The container marker on this Blueprint, or null when it is not marked. Read-only, so it is safe to call
	// during compilation (unlike adding one).
	CROWDYSDKEDITOR_API UCrowdyContainerBlueprintExtension* FindExtension(const UBlueprint* Blueprint);

	// True when the Blueprint is marked as a Game Model container in the editor (the persisted marker is present).
	// This is the toggle state; it does NOT consider a tag inherited from a C++ container base.
	CROWDYSDKEDITOR_API bool IsMarkedContainer(const UBlueprint* Blueprint);

	// True when the Blueprint is a Game Model container at all: marked in the editor (the marker, which reads true
	// immediately, before the recompile that stamps the tag) OR its class already carries the CrowdyContainer tag
	// (a C++ container base, or the marker's tag from a prior compile). This is the gate for showing the Server
	// Owned surface and the "not a container yet" note; the two agreed on one implementation.
	CROWDYSDKEDITOR_API bool IsGameModelContainerClass(const UBlueprint* Blueprint);

	// The server container type name given an authored override and the Blueprint's asset name: the trimmed
	// override when non-empty, otherwise the asset name. Pure, so the default-to-asset-name rule is unit-tested.
	CROWDYSDKEDITOR_API FString ResolveTypeName(const FString& AuthoredTypeName, const FString& AssetName);

	// The container type name this Blueprint should stamp on its class, or empty when it is not a marked
	// container (the compiler extension then removes any stale tag). Resolves the marker's ContainerTypeName
	// against the Blueprint's asset name.
	CROWDYSDKEDITOR_API FString ResolveContainerTypeName(const UBlueprint* Blueprint);

	// Mark or un-mark the Blueprint as a Game Model container (adds/removes the marker), inside an undo
	// transaction, then marks it structurally modified + dirty so the next compile stamps or removes the class
	// tag. No-op while the Blueprint is compiling or when it is already in the requested state.
	CROWDYSDKEDITOR_API void SetMarked(UBlueprint* Blueprint, bool bMarked);

	// Set the authored container type name. Stores empty when the value is blank or equals the asset name (so a
	// defaulted container keeps following its asset name); otherwise stores the trimmed override. No-op when the
	// Blueprint is not marked or is compiling.
	CROWDYSDKEDITOR_API void SetAuthoredTypeName(UBlueprint* Blueprint, const FString& TypeName);

	// Whether a container of this Blueprint's class pulls its server state once as soon as it binds. An unmarked
	// Blueprint answers true, the default, so the authoring surface shows the same state a container starts in.
	CROWDYSDKEDITOR_API bool GetPullModelOnStart(const UBlueprint* Blueprint);

	// Set whether this container type pulls its server state on bind, inside an undo transaction, then marks the
	// Blueprint structurally modified + dirty so the next compile stamps or clears the class tag. No-op when the
	// Blueprint is not marked, is compiling, or is already in the requested state.
	CROWDYSDKEDITOR_API void SetPullModelOnStart(UBlueprint* Blueprint, bool bPullOnStart);

	// True when this Blueprint's class must carry the pull-on-start-off tag: it is a marked container AND the
	// author turned the pull off. Everything else (an unmarked class, a container left at the default) is false,
	// and the compiler extension then removes any stale tag. Pulling on bind is the default, so only the opt-out
	// is written, which keeps the tag off every container that never opts out.
	CROWDYSDKEDITOR_API bool ShouldStampPullOnStartOff(const UBlueprint* Blueprint);

	// What the compiler extension writes for the pull-on-start tag. A marked container states its answer either
	// way; only a class that is not a marked container leaves the tag off.
	CROWDYSDKEDITOR_API ECrowdyPullOnStartStamp ResolvePullOnStartStamp(const UBlueprint* Blueprint);
}
