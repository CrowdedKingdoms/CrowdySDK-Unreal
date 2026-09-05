#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"

/**
 * A set of actor ids shared between the game thread and the actor update workers, with a hard ceiling on how
 * many it will hold.
 *
 * The ceiling bounds network input rather than sizing a table: the ids arrive over the wire, so without one a
 * sender could grow this set without limit. Reaching it is reported separately from finding a duplicate, because
 * the two mean opposite things to a caller deciding whether to spawn: one says the entity is already accounted
 * for, the other says nothing is being tracked for it and nothing will be.
 */
class FCrowdyGuidSet
{
public:

	enum class EAddResult : uint8
	{
		Added,
		AlreadyPresent,
		AtCapacity
	};

	explicit FCrowdyGuidSet(const int32 InCapacity)
		: Capacity(FMath::Max(1, InCapacity))
	{
		Guids.Reserve(Capacity);
	}

	EAddResult Add(const FGuid& Guid)
	{
		FWriteScopeLock Lock(SetLock);

		if (Guids.Contains(Guid))
			return EAddResult::AlreadyPresent;

		if (Guids.Num() >= Capacity)
			return EAddResult::AtCapacity;

		Guids.Add(Guid);
		return EAddResult::Added;
	}

	bool Contains(const FGuid& Guid) const
	{
		FReadScopeLock Lock(SetLock);
		return Guids.Contains(Guid);
	}

	bool Remove(const FGuid& Guid)
	{
		FWriteScopeLock Lock(SetLock);
		return Guids.Remove(Guid) > 0;
	}

	int32 Num() const
	{
		FReadScopeLock Lock(SetLock);
		return Guids.Num();
	}

private:

	const int32 Capacity;
	TSet<FGuid> Guids;
	mutable FRWLock SetLock;
};
