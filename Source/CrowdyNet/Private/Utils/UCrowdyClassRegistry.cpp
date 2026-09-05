#include "Utils/UCrowdyClassRegistry.h"
#include "CrowdyNetLog.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"

std::atomic<UCrowdyClassRegistry*> UCrowdyClassRegistry::Instance(nullptr);

bool UCrowdyClassRegistry::RegisterClass(const FCrowdyClassID ClassID, const FSoftClassPath& ClassPath)
{
	ensure(IsInGameThread());

	// Post-seal calls come from the second GI's AutoRegistry init in multi-client
	// PIE. The singleton is already populated, so silently skip them.
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

		FCrowdyIDConflict Conflict;
		Conflict.RegistryName = TEXT("CrowdyClassRegistry");
		Conflict.ID = ClassID;
		Conflict.IncumbentPath = Existing->ToString();
		Conflict.RejectedPath = ClassPath.ToString();
		Conflict.Remedy = TEXT("Give one of the two an explicit ClassID via ClassIDOverrides in CrowdySDKDeveloperSettings (Project Settings, Plugins, Crowdy SDK).");
		Conflicts.Add(Conflict);

		// Remembered so GetID refuses to hand out the incumbent's ID for this class. Without it the
		// path hash below would return exactly the ID the collision was about, and every receiver
		// would resolve this class's spawn events to the incumbent instead of falling back to the
		// class path the event also carries.
		RefusedPaths.Add(FName(*Conflict.RejectedPath));

		UE_LOG(LogCrowdyNet, Error,
			TEXT("[CrowdyClassRegistry] ClassID collision: %u is claimed by both '%s' and '%s'. %s"),
			ClassID, *Conflict.IncumbentPath, *Conflict.RejectedPath, *Conflict.Remedy);
		return false;
	}

	const FName PathKey(*ClassPath.ToString());

	IDToClassPath.Add(ClassID, ClassPath);
	PathToID.Add(PathKey, ClassID);
	RefusedPaths.Remove(PathKey);
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

	// The three maps below are frozen once the registry is sealed, so on the game thread the answer for a
	// class cannot change and is worth remembering: every call otherwise rebuilds the class path into an
	// FString and hashes it into an FName, which the state send loop pays per tracked entity per tick.
	const bool bMemoize = bSealed.load(std::memory_order_acquire) && IsInGameThread();
	if (bMemoize)
	{
		if (const FCrowdyClassID* Memoized = ClassIDMemo.Find(Class))
			return *Memoized;
	}

	const FName PathKey(*Class->GetPathName());

	// A refusal is memoized like any other answer: it is what this class resolves to for as long as the
	// clash stands, and ClearConflicts drops the memo along with the refusal itself.
	FCrowdyClassID Resolved = CROWDY_INVALID_CLASS_ID;
	if (const FCrowdyClassID* Found = PathToID.Find(PathKey))
	{
		Resolved = *Found;
	}
	else if (!RefusedPaths.Contains(PathKey))
	{
		Resolved = FCrowdyTypeIDGenerator::GenerateFromClass(Class);
	}

	if (bMemoize)
	{
		ClassIDMemo.Add(Class, Resolved);
	}

	return Resolved;
}

void UCrowdyClassRegistry::InvalidateIDMemo()
{
	ensure(IsInGameThread());
	ClassIDMemo.Reset();
}

void UCrowdyClassRegistry::ClearConflicts()
{
	Conflicts.Reset();
	RefusedPaths.Reset();

	// The refusals the memo may have recorded are exactly what is being forgotten here, so a stale memo
	// would keep handing out CROWDY_INVALID_CLASS_ID for a clash that is resolved.
	ClassIDMemo.Reset();
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
