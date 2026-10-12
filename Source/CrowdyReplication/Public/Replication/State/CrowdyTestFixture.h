#pragma once

#include "CoreMinimal.h"

class UClass;

namespace CrowdyTestFixture
{
	// UCLASS key-only marker: an SDK test fixture class, filtered out of the heartbeat advisory and the registry baker's sweep.
	inline const TCHAR* MetaKey = TEXT("CrowdyTestFixture");

	// True when Class is an SDK test fixture (meta=(CrowdyTestFixture)); false in a cooked build, where metadata is stripped.
	CROWDYREPLICATION_API bool IsTestFixture(const UClass* Class);
}
