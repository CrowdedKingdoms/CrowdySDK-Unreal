#pragma once

#include "CoreMinimal.h"

// Save format versions for CrowdySDK objects, recorded in every package that saves one.
struct CROWDYREPLICATION_API FCrowdySDKCustomVersion
{
	enum Type
	{
		BeforeCustomVersionWasAdded = 0,

		// Crowdy Entity components save Owned By and Replicate Movement.
		OwnedByAxis,

		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	static const FGuid GUID;

private:
	FCrowdySDKCustomVersion() = delete;
};
