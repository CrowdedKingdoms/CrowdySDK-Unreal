// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * How a Blueprint variable participates in Crowdy networking. This is the single "Crowdy Replication"
 * mode a variable can be in; the details customization writes exactly one plane's metadata for it.
 *
 * None and Replicated ride the CrowdyState view plane surface; ServerOwned rides the Game Model truth plane.
 * A variable lives in exactly one plane, so the dropdown never leaves the other plane's metadata behind
 * (see CrowdyReplicationModeDecision::KeysToScrubOnSwitch).
 */
enum class ECrowdyReplicationMode : uint8
{
	// Not networked by Crowdy. Selecting this scrubs every Crowdy key off the variable.
	None,

	// CrowdyState: the fast, client-authoritative view plane. Writes the CrowdyState marker; reveals the
	// RepNotify field (CrowdyOnRep) and the Advanced sub-options (CrowdyOwnerOnly, CrowdyManualDirty, heartbeat).
	Replicated,

	// Game Models: server-authoritative truth over GraphQL. Writes the CrowdyModel marker; reveals the RepNotify
	// field (the SAME CrowdyOnRep key, GAS-style), the native Min/Max clamp, and the read-Visibility dropdown.
	ServerOwned
};

/**
 * Pure, headless-testable decisions for the unified "Crowdy Replication" variable dropdown: which metadata
 * keys each plane OWNS, and therefore which keys a mode switch must scrub so no stale plane metadata lingers
 * on the variable (a Server Owned -> Replicated switch must clear the Crowdy key override and Visibility). Kept
 * out of the Slate customization so the plane key-ownership model is unit-tested rather than trapped in a callback.
 */
namespace CrowdyReplicationModeDecision
{
	// The metadata keys a mode OWNS: it writes its marker when selected, and another mode must scrub them when
	// switched to. CrowdyOnRep is deliberately EXCLUDED because it is SHARED by both replicated planes (one
	// parameterless GAS-style notify convention), so it is only scrubbed when leaving Crowdy entirely (-> None).
	// The native ClampMin/ClampMax the Server Owned Min/Max sub-option writes are also EXCLUDED: they are plain
	// Unreal clamp metadata a variable can want in any mode, so no mode owns (or scrubs) them. Returns the
	// CrowdyStateMetaKeys / CrowdyGameModelMetaKeys spellings only.
	CROWDYSDKEDITOR_API TArray<const TCHAR*> OwnedKeys(ECrowdyReplicationMode Mode);

	// The keys to remove when a variable leaves FromMode for ToMode: the keys FromMode owns that ToMode does
	// not, plus the shared CrowdyOnRep when ToMode is None (leaving Crowdy). Re-picking the same mode scrubs
	// nothing. Compared by string content (the key constants are distinct pointers across the two namespaces).
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
