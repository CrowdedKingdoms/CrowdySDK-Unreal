// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyReplicationMode.h"

#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/State/CrowdyStateMetaKeys.h"

namespace CrowdyReplicationModeDecision
{
	TArray<const TCHAR*> OwnedKeys(ECrowdyReplicationMode Mode)
	{
		switch (Mode)
		{
		case ECrowdyReplicationMode::Replicated:
			// The CrowdyState-only keys. CrowdyOnRep is shared, so it is not listed here.
			return { CrowdyStateMetaKeys::Replicate, CrowdyStateMetaKeys::OwnerOnly,
					 CrowdyStateMetaKeys::ManualDirty, CrowdyStateMetaKeys::Heartbeat };

		case ECrowdyReplicationMode::ServerOwned:
			// The Game Model marker + the server-key override + the read-Visibility. The native ClampMin/ClampMax
			// the Min/Max sub-option writes are deliberately NOT owned: they are plain Unreal clamp metadata a
			// variable can want in any mode, so leaving Server Owned preserves them (only Crowdy keys are scrubbed).
			// CrowdyOnRep is shared, so it is not listed here.
			return { CrowdyGameModelMetaKeys::Model, CrowdyGameModelMetaKeys::Key,
					 CrowdyGameModelMetaKeys::Visibility };

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
			// Compare by content, not pointer: the key constants come from two different namespaces, so a
			// pointer compare could wrongly keep a re-listed key.
			const bool bToOwns = ToKeys.ContainsByPredicate(
				[Key](const TCHAR* Other) { return FCString::Strcmp(Other, Key) == 0; });
			if (!bToOwns)
			{
				Scrub.Add(Key);
			}
		}

		// CrowdyOnRep is shared by both replicated planes (one GAS-style notify convention), so it survives a
		// plane-to-plane switch and is only scrubbed when the variable leaves Crowdy entirely.
		if (ToMode == ECrowdyReplicationMode::None
			&& (FromMode == ECrowdyReplicationMode::Replicated || FromMode == ECrowdyReplicationMode::ServerOwned))
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
