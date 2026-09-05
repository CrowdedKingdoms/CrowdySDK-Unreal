#pragma once

#include "CoreMinimal.h"

/**
 * A registration that was refused because a different type already claimed the
 * same wire ID. The type that registered first keeps the ID; the refused type
 * is left with no ID at all, so sending it fails with an error instead of
 * arriving at the far end and being decoded as the incumbent.
 *
 * This is an authoring problem, not a runtime one: it is fixed by assigning one
 * of the two types an explicit ID in project settings.
 */
struct FCrowdyIDConflict
{
	// Which registry refused the registration, for the report.
	FString RegistryName;

	// The contested ID.
	uint32 ID = 0;

	// Path of the type that holds the ID.
	FString IncumbentPath;

	// Path of the type that was refused and now has no ID.
	FString RejectedPath;

	// How to resolve it, phrased for whoever has to act on it.
	FString Remedy;
};

namespace CrowdyIDConflicts
{
	/**
	 * Logs every conflict recorded by every Crowdy ID registry as one block, so
	 * the whole set is visible at once rather than one line per unlucky load
	 * order. Returns the number of conflicts reported.
	 */
	CROWDYNET_API int32 ReportAll();
}
