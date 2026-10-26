#include "Replication/Subsystems/CrowdyKitActorClasses.h"

#include "CrowdyReplicationLog.h"

namespace
{
	TMap<FName, TSoftClassPtr<AActor>>& CrowdyKitActorClassMap()
	{
		static TMap<FName, TSoftClassPtr<AActor>> Classes;
		return Classes;
	}

	TSet<FName>& CrowdyKitActorClassesReported()
	{
		static TSet<FName> Reported;
		return Reported;
	}
}

void CrowdyKitActorClasses::Set(const FName Kind, const TSoftClassPtr<AActor>& Class)
{
	check(IsInGameThread());
	const TSoftClassPtr<AActor>* Known = CrowdyKitActorClassMap().Find(Kind);
	UE_CLOG(Known && *Known != Class, LogCrowdyReplication, Warning,
		TEXT("[Crowdy SDK][ActorManager]: two starters in Game Kit Actor Starters draw a %s, as %s and as %s; the last one is used."),
		*Kind.ToString(), *Known->ToString(), *Class.ToString());
	CrowdyKitActorClassMap().Add(Kind, Class);
	CrowdyKitActorClassesReported().Remove(Kind);
}

void CrowdyKitActorClasses::Reset()
{
	check(IsInGameThread());
	CrowdyKitActorClassMap().Reset();
	CrowdyKitActorClassesReported().Reset();
}

UClass* CrowdyKitActorClasses::Resolve(const FName Kind)
{
	check(IsInGameThread());
	const TSoftClassPtr<AActor>* Found = CrowdyKitActorClassMap().Find(Kind);
	if (UClass* Loaded = Found ? Found->Get() : nullptr)
	{
		return Loaded;
	}
	// Said once and not loaded again, so a crowd of a kind whose class is missing costs one failed load.
	if (CrowdyKitActorClassesReported().Contains(Kind))
	{
		return nullptr;
	}
	if (UClass* Loaded = Found && !Found->IsNull() ? Found->LoadSynchronous() : nullptr)
	{
		return Loaded;
	}
	CrowdyKitActorClassesReported().Add(Kind);
	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[Crowdy SDK][ActorManager]: a Game Kit starter shows %s, but no definition in the Game Kit Actor Starters project setting gives that kind a class that loads (%s), so it is not drawn. Set its class in the starter's Kit Actors."),
		Kind.IsNone() ? TEXT("an actor of a kind no starter's Kit Actors names") : *(TEXT("a ") + Kind.ToString()), Found ? *Found->ToString() : TEXT("none"));
	return nullptr;
}
