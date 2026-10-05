#include "CrowdyServerComputeSettings.h"

bool UCrowdyServerComputeSettings::SetRevisionsToKeep(int32 Count)
{
	RevisionsToKeep = FMath::Clamp(Count, MinRevisionsToKeep, MaxRevisionsToKeep);
	return TryUpdateDefaultConfigFile();
}
