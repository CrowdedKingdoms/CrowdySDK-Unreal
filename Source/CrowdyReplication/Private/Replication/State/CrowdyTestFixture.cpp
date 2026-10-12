#include "Replication/State/CrowdyTestFixture.h"

#include "UObject/Class.h"

bool CrowdyTestFixture::IsTestFixture(const UClass* Class)
{
#if WITH_METADATA
	return Class && Class->HasMetaData(MetaKey);
#else
	// Metadata is stripped in a cooked build, so this answers false there, and every caller is an editor or startup-sweep path.
	return false;
#endif
}
