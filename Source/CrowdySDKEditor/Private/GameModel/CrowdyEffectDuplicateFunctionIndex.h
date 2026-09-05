// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

// The editor-side implementation of two runtime CrowdyReplication no-cycle hooks (both declared in the runtime
// module's CrowdyEffect.h): CrowdyEffectDuplicateFunctionCheck's sweep hook, backed by a project-wide index of
// (container type, function name) -> every Crowdy Effect asset path authoring that pair, and
// CrowdyEffectFunctionCatalog's catalog hook, backed by a project-wide index of container type -> every declared
// function on it. Both indexes are built by the SAME asset-registry sweep and kept warm across calls instead of
// re-sweeping per asset or per hook. See CrowdyEffectDuplicateFunctionIndex.cpp for the caching/invalidation design.
namespace CrowdyEffectDuplicateFunctionIndex
{
	// Registers both hooks and starts listening for the asset-registry / property-edit events that invalidate the
	// cached indexes. Call once from FCrowdySDKEditorModule::StartupModule. Safe to call again while already
	// registered: it reinstalls the hooks (which is how a caller that swapped in its own hook puts the real one
	// back) without binding a second copy of the listeners.
	void Register();

	// Clears both hooks, stops listening, and drops the cached indexes. Call once from
	// FCrowdySDKEditorModule::ShutdownModule.
	void Unregister();

	// Drops the cached indexes so the next question rebuilds them from the current state of the project.
	//
	// Call this right after writing a Crowdy Effect's authoring state directly rather than through the property
	// system: assigning EffectScript on the asset, or changing the effect's node graph. Neither raises
	// OnObjectPropertyChanged for the effect, so nothing else tells the index that what it recorded about the
	// effect (its function name, whether it authors a return, whether it has mutations) is now out of date, and
	// callers keep getting pre-edit answers until the editor restarts.
	//
	// Cheap: it only clears the cached maps. The rebuild happens on the next lookup, not here.
	void NotifyEffectAuthoringChanged();
}
