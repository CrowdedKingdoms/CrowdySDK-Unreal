// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyReplicationMode.h"

#include "Replication/State/CrowdyStateMetaKeys.h"

namespace CrowdyReplicationModeDecision
{
	TArray<const TCHAR*> OwnedKeys(ECrowdyReplicationMode Mode)
	{
		switch (Mode)
		{
		case ECrowdyReplicationMode::Replicated:
			// CrowdyOnRep is not listed: it is only scrubbed on leaving Crowdy entirely.
			return { CrowdyStateMetaKeys::Replicate, CrowdyStateMetaKeys::OwnerOnly,
					 CrowdyStateMetaKeys::ManualDirty, CrowdyStateMetaKeys::Heartbeat };

		case ECrowdyReplicationMode::None:
		default:
			return {};
		}
	}

	TArray<const TCHAR*> KeysToScrubOnSwitch(ECrowdyReplicationMode FromMode, ECrowdyReplicationMode ToMode)
	{
		const TArray<const TCHAR*> FromKeys = OwnedKeys(FromMode);
		const TArray<const TCHAR*> ToKeys = OwnedKeys(ToMode);

		TArray<const TCHAR*> Scrub;
		for (const TCHAR* Key : FromKeys)
		{
			// Compare by content, not pointer: a pointer compare could wrongly keep a re-listed key.
			const bool bToOwns = ToKeys.ContainsByPredicate(
				[Key](const TCHAR* Other) { return FCString::Strcmp(Other, Key) == 0; });
			if (!bToOwns)
			{
				Scrub.Add(Key);
			}
		}

		if (ToMode == ECrowdyReplicationMode::None && FromMode == ECrowdyReplicationMode::Replicated)
		{
			Scrub.Add(CrowdyStateMetaKeys::OnRep);
		}
		return Scrub;
	}

	bool OnRepFollowsGraphRename(const FString& CurrentOnRep, FName OldGraphName, FName NewGraphName)
	{
		// A rename that changes nothing must move nothing: the write that follows recompiles the skeleton, so
		// answering yes here would cost a compile for every graph the editor happens to rename. A nameless new graph
		// is never a target either, since repointing a binding at None would clear it rather than move it.
		if (NewGraphName.IsNone() || OldGraphName == NewGraphName)
		{
			return false;
		}
		// Most variables carry no notify at all, so the cheap string test comes before interning an FName. This is an
		// early-out rather than a gate: an empty binding interns to NAME_None and fails the comparison anyway, which
		// is also why a NAME_None OldGraphName needs no test of its own.
		if (CurrentOnRep.IsEmpty())
		{
			return false;
		}
		return FName(*CurrentOnRep) == OldGraphName;
	}
}
