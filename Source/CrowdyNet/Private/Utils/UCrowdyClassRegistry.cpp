#include "Utils/UCrowdyClassRegistry.h"
#include "CrowdyNetLog.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"

std::atomic<UCrowdyClassRegistry*> UCrowdyClassRegistry::Instance(nullptr);

bool UCrowdyClassRegistry::RegisterClass(const FCrowdyClassID ClassID, const FSoftClassPath& ClassPath)
{
	ensure(IsInGameThread());

	// Post-seal calls come from the second GI's AutoRegistry init in multi-client
	// PIE — the singleton is already populated, so silently skip them.
	if (bSealed.load(std::memory_order_relaxed))
		return false;

	if (ClassID == CROWDY_INVALID_CLASS_ID || !ClassPath.IsValid())
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[CrowdyClassRegistry] Rejected registration: ID=%u Path='%s'"),
			ClassID, *ClassPath.ToString());
		return false;
	}

	if (const FSoftClassPath* Existing = IDToClassPath.Find(ClassID))
	{
		if (*Existing == ClassPath) return false; // benign duplicate

		UE_LOG(LogCrowdyNet, Error,
			TEXT("[CrowdyClassRegistry] ClassID collision: %u is claimed by both")
			TEXT(" '%s' and '%s'. Add an entry to ClassIDOverrides in")
			TEXT(" CrowdySDKDeveloperSettings to resolve."),
			ClassID, *Existing->ToString(), *ClassPath.ToString());
		return false;
	}

	IDToClassPath.Add(ClassID, ClassPath);
	PathToID.Add(FName(*ClassPath.ToString()), ClassID);
	return true;
}

FSoftClassPath UCrowdyClassRegistry::Resolve(const FCrowdyClassID ClassID) const
{
	const FSoftClassPath* Found = IDToClassPath.Find(ClassID);
	return Found ? *Found : FSoftClassPath();
}

FCrowdyClassID UCrowdyClassRegistry::GetID(const UClass* Class) const
{
	if (!Class) return CROWDY_INVALID_CLASS_ID;

	const FCrowdyClassID* Found = PathToID.Find(FName(*Class->GetPathName()));
	return Found ? *Found : FCrowdyTypeIDGenerator::GenerateFromClass(Class);
}

void UCrowdyClassRegistry::Seal()
{
	ensure(IsInGameThread());
	// seq_cst fence: guarantees every worker thread started after
	// this call sees the complete, populated maps.
	bSealed.store(true, std::memory_order_seq_cst);

	UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log,
		TEXT("[CrowdyClassRegistry] Sealed. %d entity classes registered."),
		IDToClassPath.Num());
}
