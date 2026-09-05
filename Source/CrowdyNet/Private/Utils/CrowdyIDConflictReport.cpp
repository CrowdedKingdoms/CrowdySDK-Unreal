#include "Core/CrowdyCategory/FCrowdyIDConflict.h"

#include "CrowdyNetLog.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

namespace CrowdyIDConflicts
{
	int32 ReportAll()
	{
		TArray<FCrowdyIDConflict> All;

		if (UEventPayloadRegistry* Events = UEventPayloadRegistry::Get())
			All.Append(Events->GetConflicts());

		if (UActorUpdatePayloadRegistry* ActorUpdates = UActorUpdatePayloadRegistry::Get())
			All.Append(ActorUpdates->GetConflicts());

		if (UCrowdyClassRegistry* Classes = UCrowdyClassRegistry::Get())
			All.Append(Classes->GetConflicts());

		if (All.Num() == 0)
			return 0;

		UE_LOG(LogCrowdyNet, Error,
			TEXT("[CrowdyIDConflicts] %d type(s) could not be registered because another type already holds their wire ID. Each one is left without a wire ID until an explicit ID is assigned."),
			All.Num());

		for (const FCrowdyIDConflict& Conflict : All)
		{
			UE_LOG(LogCrowdyNet, Error,
				TEXT("[CrowdyIDConflicts] %s ID=%u is held by '%s', so '%s' has no wire ID. %s"),
				*Conflict.RegistryName, Conflict.ID, *Conflict.IncumbentPath, *Conflict.RejectedPath, *Conflict.Remedy);
		}

		return All.Num();
	}
}
