// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * How a Blueprint variable participates in Crowdy networking. This is the single "Crowdy Replication"
 * mode a variable can be in; the details customization writes that mode's metadata for it.
 */
enum class ECrowdyReplicationMode : uint8
{
	// Not networked by Crowdy. Selecting this scrubs every Crowdy key off the variable.
	None,

	// CrowdyState: the fast, client-authoritative view plane. Writes the CrowdyState marker; reveals the
	// RepNotify field (CrowdyOnRep) and the Advanced sub-options (CrowdyOwnerOnly, CrowdyManualDirty, heartbeat).
	Replicated
};

/**
 * Pure, headless-testable decisions for the "Crowdy Replication" variable dropdown: which metadata keys a mode
 * OWNS, and therefore which keys a mode switch must scrub so no stale metadata lingers on the variable. Kept
 * out of the Slate customization so the key-ownership model is unit-tested rather than trapped in a callback.
 */
namespace CrowdyReplicationModeDecision
{
	// The metadata keys a mode OWNS: it writes its marker when selected, and the keys are scrubbed when another
	// mode is switched to. CrowdyOnRep is deliberately EXCLUDED, so it is only scrubbed when leaving Crowdy
	// entirely (-> None). Returns the CrowdyStateMetaKeys spellings only.
	CROWDYSDKEDITOR_API TArray<const TCHAR*> OwnedKeys(ECrowdyReplicationMode Mode);

	// The keys to remove when a variable leaves FromMode for ToMode: the keys FromMode owns that ToMode does
	// not, plus CrowdyOnRep when ToMode is None (leaving Crowdy). Re-picking the same mode scrubs nothing.
	CROWDYSDKEDITOR_API TArray<const TCHAR*> KeysToScrubOnSwitch(ECrowdyReplicationMode FromMode, ECrowdyReplicationMode ToMode);

	/**
	 * Whether a variable whose CrowdyOnRep currently reads CurrentOnRep is bound to a function graph that has just
	 * been renamed from OldGraphName to NewGraphName, and so has to follow it.
	 *
	 * CrowdyOnRep names its notify in metadata rather than through a reference the editor fixes up, so without this
	 * a rename leaves the binding naming a function that no longer exists: the variable silently stops notifying,
	 * and the only signal is a compile error. Unreal's own graph rename repoints every reference it owns (call
	 * nodes, local variable scopes, child overrides); this is the same fixup for the reference that is ours.
	 *
	 * The names are compared as FNames rather than as strings, because an FName comparison is what resolves the
	 * notify (UClass::FindFunctionByName): a binding differing only in case names the same function and follows too.
	 */
	CROWDYSDKEDITOR_API bool OnRepFollowsGraphRename(const FString& CurrentOnRep, FName OldGraphName, FName NewGraphName);
}
