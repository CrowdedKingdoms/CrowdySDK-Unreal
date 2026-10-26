#include "Assets/CrowdyMassAnimSet.h"

#include "Animation/AnimSequence.h"
#include "CrowdyMassLog.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/DataValidation.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#endif

FCrowdyMassResolvedSequence UCrowdyMassAnimSet::ResolveGait(int32 GaitId, const TArray<UAnimSequence*>& BankSequences) const
{
	// Gait id N is Gaits[N-1], the same one-based convention pose and action ids use. Id 0 is the "no
	// gait resolved" sentinel, so it is nothing to look up rather than an unresolved lookup: falling
	// back to the floor row for it would tell an entity that has never been given a gait that it is
	// standing still, which is a different claim.
	if (GaitId <= 0)
	{
		return FCrowdyMassResolvedSequence();
	}

	const int32 RowIndex = GaitId - 1;

	FCrowdyMassResolvedSequence Result = Gaits.IsValidIndex(RowIndex)
		? ResolveSequenceAsset(Gaits[RowIndex].Sequence, BankSequences)
		: FCrowdyMassResolvedSequence();

	// The first row is the floor everything else falls back to, so it must never fall back to itself:
	// an unresolvable floor row would recurse without end.
	if (!Result.bResolved && RowIndex != 0)
	{
		FCrowdyMassResolvedSequence Fallback = ResolveGait(1, BankSequences);
		Fallback.bFellBackToFloorGait = true;
		return Fallback;
	}

	return Result;
}

FCrowdyMassResolvedSequence UCrowdyMassAnimSet::ResolvePose(FName PoseName, const TArray<UAnimSequence*>& BankSequences) const
{
	// NAME_None is the documented sentinel for "no pose authored", and also the default key of a
	// blank details-panel row. Treat it as nothing to look up rather than an unresolved lookup, so a
	// caller can tell "nothing was authored" apart from "something was authored but is broken".
	if (PoseName.IsNone())
	{
		return FCrowdyMassResolvedSequence();
	}

	const FCrowdyMassPoseEntry* Entry = Poses.FindByPredicate([PoseName](const FCrowdyMassPoseEntry& Candidate)
	{
		return Candidate.PoseName == PoseName;
	});

	FCrowdyMassResolvedSequence Result = Entry ? ResolveSequenceAsset(Entry->Sequence, BankSequences) : FCrowdyMassResolvedSequence();

	if (!Result.bResolved)
	{
		FCrowdyMassResolvedSequence Fallback = ResolveGait(1, BankSequences);
		Fallback.bFellBackToFloorGait = true;
		return Fallback;
	}

	return Result;
}

int32 UCrowdyMassAnimSet::FindPoseId(FName PoseName) const
{
	// NAME_None is the documented "no pose" sentinel, matching ResolvePose's own treatment of it:
	// nothing to look up, not an unresolved lookup.
	if (PoseName.IsNone())
	{
		return 0;
	}

	const int32 Index = Poses.IndexOfByPredicate([PoseName](const FCrowdyMassPoseEntry& Candidate)
	{
		return Candidate.PoseName == PoseName;
	});

	// 1-based so 0 stays free to mean "none", the same convention FCrowdyMassActionFragment::ActionId
	// uses. An unknown name reports 0 rather than a made-up id: fabricating one that happened to be in
	// range would resolve to whatever row occupies it.
	if (Index == INDEX_NONE)
	{
		return 0;
	}

	const int32 PoseId = Index + 1;

	// The id has one byte to live in, so an id past CrowdyMassMaxPoses cannot be carried at all: row
	// 256 takes id 256, which truncates to 0, and 0 is the value that clears the pose. Refused here
	// rather than truncated, so the row is unreachable instead of quietly meaning "no pose".
	if (PoseId > CrowdyMassMaxPoses)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("CrowdyMassAnimSet %s: pose '%s' is authored at row %d, past the %d a one-byte pose id can reach, so it can never be selected. Move it above row %d or remove rows before it."),
			*GetName(), *PoseName.ToString(), PoseId, CrowdyMassMaxPoses, CrowdyMassMaxPoses);
		return 0;
	}

	return PoseId;
}

int32 UCrowdyMassAnimSet::FindPoseIdByStateValue(int64 StateValue) const
{
	EnsurePoseStateTable();

	// A value no row names answers 0, which clears the pose rather than selecting a made-up row. That
	// is deliberate and is what lets a game author rows only for the states it wants to show: a living
	// player's state simply names no row and falls through to locomotion.
	return static_cast<int32>(CachedPoseStateTable.Find(StateValue));
}

void UCrowdyMassAnimSet::InvalidatePoseStateTable()
{
	bPoseStateTableBuilt = false;
	CachedNamedPoseStateEnum.Reset();
	bNamedPoseStateEnumSearched = false;

	// Wraps to 1 rather than to 0, so it can never land on the value a table that was never built carries.
	++SourceRevision;
	if (SourceRevision == 0)
	{
		SourceRevision = 1;
	}
}

TArray<FString> UCrowdyMassAnimSet::GetPoseStateEnumNameOptions() const
{
	TArray<FString> Options;

#if WITH_EDITOR
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		const UEnum* Candidate = *It;

		// Compiled into a module, which is what makes it nameable by string at all: a Blueprint
		// Enumeration named this way leaves nothing referencing it, so it is not cooked and the name
		// resolves to nothing in a packaged build while working perfectly in the editor.
		if (!Candidate || !Candidate->GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn))
		{
			continue;
		}

		// Byte-valued, the same bound the replicated field imposes: a wider enum cannot reach the one byte
		// the state travels in, so offering it would only be offering a mistake.
		if (Candidate->GetMaxEnumValue() >= FCrowdyMassPoseStateTable::ValueCount)
		{
			continue;
		}

		Options.Add(Candidate->GetName());
	}

	// Alphabetical, because the iteration order above is allocation order and would otherwise reshuffle
	// the list between sessions for no reason an author could follow.
	Options.Sort();
#endif

	return Options;
}

const UEnum* UCrowdyMassAnimSet::GetPoseStateEnum() const
{
	// The author's own choice decides which of the two fields is read, rather than one of them winning by
	// being filled in. Both used to be live at once, with the name quietly taking priority, which made the
	// details panel show two settings for one decision and the wrong one look canonical.
	if (PoseStateEnumSource == ECrowdyMassPoseStateEnumSource::BlueprintAsset)
	{
		return PoseStateEnum;
	}

	// The name wins, because the object property beside it can only ever hold a Blueprint Enumeration: the
	// details panel fills it from the asset registry, and an enum declared in C++ is not an asset. A set
	// whose rows are entries of a native enum has no other way to say so.
	if (PoseStateEnumName.IsEmpty())
	{
		return PoseStateEnum;
	}

	if (const UEnum* Cached = CachedNamedPoseStateEnum.Get())
	{
		return Cached;
	}

	// Memoised whether it hit or MISSED. A miss left unmemoised is not merely slow: this is reachable from
	// the GetOptions function the details panel calls, so a mistyped name would pay a full object-hash scan
	// per row per panel refresh.
	if (bNamedPoseStateEnumSearched)
	{
		return nullptr;
	}

	bNamedPoseStateEnumSearched = true;

	// Accepts a plain name, which is what an author normally types, and a full object path, which is how
	// two modules declaring the same name are told apart. Slow either way, so it happens once and is kept
	// until something the answer depends on can have changed.
	//
	// The Safe variant, because the plain one is FATAL while a package is saving or a collection is running
	// and this is reachable from IsDataValid, which runs during cook. Ambiguity is asked to be reported
	// rather than suppressed: two modules declaring the same enum name resolve to whichever the hash bucket
	// yields first, which is not stable between runs, and the default verbosity hides that completely.
	UEnum* Found = Cast<UEnum>(StaticFindFirstObjectSafe(UEnum::StaticClass(), *PoseStateEnumName,
		EFindFirstObjectOptions::NativeFirst, ELogVerbosity::Warning));

	CachedNamedPoseStateEnum = Found;
	return Found;
}

void UCrowdyMassAnimSet::EnsurePoseStateTable() const
{
	if (bPoseStateTableBuilt)
	{
		return;
	}

	// Built on the thread that first asks, which is the game thread on every path that reaches here:
	// state deltas are applied from the router's dispatch and resolution happens once per bank pairing.
	FillPoseStateTable(*this, CachedPoseStateTable);
	bPoseStateTableBuilt = true;
}

int64 UCrowdyMassAnimSet::ResolvePoseStateValue(const UEnum* StateEnum, FName PoseName)
{
	if (!StateEnum || PoseName.IsNone())
	{
		return INDEX_NONE;
	}

	// Accepts the short entry name an author picks from the dropdown as well as the fully qualified one,
	// follows any enum value redirect the game has declared so renaming an entry does not silently
	// unhook every row naming it, and accepts the authored name a Blueprint enum shows in place of its
	// generated one.
	const int32 EntryIndex = StateEnum->GetIndexByName(PoseName, EGetByNameFlags::CheckAuthoredName);
	if (EntryIndex == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	const int64 StateValue = StateEnum->GetValueByIndex(EntryIndex);

	// A value outside the range one byte can hold cannot reach a byte-wide replicated field, so it can
	// never arrive and must not take a slot. Reported where the asset is validated.
	if (StateValue < 0 || StateValue >= FCrowdyMassPoseStateTable::ValueCount)
	{
		return INDEX_NONE;
	}

	return StateValue;
}

void UCrowdyMassAnimSet::FillPoseStateTable(const UCrowdyMassAnimSet& AnimSet, FCrowdyMassPoseStateTable& OutTable)
{
	OutTable = FCrowdyMassPoseStateTable();

	// Resolved once for the whole walk rather than per row: it is the same enum for every row, and reaching
	// it can mean a search across every loaded type.
	const UEnum* StateEnum = AnimSet.GetPoseStateEnum();

	// Rows past the limit can never be given a one-byte id, so they are skipped here rather than
	// truncated: id 256 is 0, and 0 is the value that clears the pose.
	const int32 RowCount = FMath::Min(AnimSet.Poses.Num(), CrowdyMassMaxPoses);

	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		// Resolved from the row being filled, never by looking this row's name up in the array again.
		// The two differ exactly when two rows share a name, and there the name search is wrong: every
		// such row would answer with the first row's id and one of them could never be reached.
		const int64 StateValue = ResolvePoseStateValue(StateEnum, AnimSet.Poses[RowIndex].PoseName);
		if (StateValue == INDEX_NONE)
		{
			continue;
		}

		// In range by construction: ResolvePoseStateValue refuses anything a byte cannot carry.
		const int32 ValueIndex = static_cast<int32>(StateValue);

		// The first row naming a value keeps it, matching FindPoseId's first-match rule so a name and a
		// value never select different rows. Two rows on one value is reported where the asset is
		// validated.
		if (OutTable.PoseIds[ValueIndex] != 0)
		{
			continue;
		}

		OutTable.PoseIds[ValueIndex] = static_cast<uint8>(RowIndex + 1);
	}
}

TArray<FPropertyTextFName> UCrowdyMassAnimSet::GetPoseStateOptions() const
{
	TArray<FPropertyTextFName> Options;

	// Through the one accessor, never off the object property: a set whose enum was named rather than
	// assigned would otherwise offer an empty list while its rows resolved perfectly, so an author would be
	// left typing names blind against an enum the asset had already found.
	const UEnum* StateEnum = GetPoseStateEnum();
	if (!StateEnum)
	{
		return Options;
	}

	// The trailing _MAX the reflection system appends is a count, not a state anything can be in.
	const int32 EntryCount = StateEnum->NumEnums() - (StateEnum->ContainsExistingMax() ? 1 : 0);
	Options.Reserve(FMath::Max(EntryCount, 0));

	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
#if WITH_METADATA
		if (StateEnum->HasMetaData(TEXT("Hidden"), EntryIndex))
		{
			continue;
		}
#endif

		FPropertyTextFName& Option = Options.AddDefaulted_GetRef();

		// Stored as the enum's own entry name, which is the stable half: renaming what a Blueprint
		// Enumeration entry DISPLAYS leaves this alone, so a row authored from this list stays hooked to the
		// entry it named, and two entries showing the same text still store two different values.
		Option.ValueString = FName(*StateEnum->GetNameStringByIndex(EntryIndex));

		// Shown as the name the author typed. For a native enum this is the entry name in words and nothing
		// changes; for a Blueprint Enumeration the entry name is a generated NewEnumeratorN and the typed
		// name exists only here, so without it the dropdown offers a column of options nobody can tell apart.
		Option.DisplayName = StateEnum->GetDisplayNameTextByIndex(EntryIndex);
	}

	return Options;
}

void UCrowdyMassAnimSet::PostLoad()
{
	Super::PostLoad();

	// The rows this was derived from have just been replaced by whatever was saved.
	InvalidatePoseStateTable();

	// An asset saved before the source became explicit carries only the two fields, and which one was
	// live was inferred from the name being non-empty. Carry that inference forward ONCE, on load, so such
	// an asset keeps resolving the enum it always did instead of silently falling to the new default.
	// Reads the saved fields only; it must not resolve anything, because at this point in a load the named
	// enum's module may not have registered and a resolution here would memoise a miss for the session.
	if (PoseStateEnumName.IsEmpty() && PoseStateEnum)
	{
		PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	}
}

int32 UCrowdyMassAnimSet::FindActionId(FName ActionName) const
{
	if (ActionName.IsNone())
	{
		return 0;
	}

	const int32 Index = Actions.IndexOfByPredicate([ActionName](const FCrowdyMassActionEntry& Candidate)
	{
		return Candidate.ActionName == ActionName;
	});

	return Index == INDEX_NONE ? 0 : Index + 1;
}

UAnimSequence* UCrowdyMassAnimSet::FindActionSequence(const int32 ActionId) const
{
	// The same one-based convention FindResolvedAction uses, so an id names the same row whether it is
	// read here for an actor or there for a row. Id 0 is the fragment's "nothing playing" value and can
	// never name an action.
	if (!Actions.IsValidIndex(ActionId - 1))
	{
		return nullptr;
	}

	// Synchronous, and deliberately: this is reached from an event body that is about to play the clip,
	// so an asynchronous load would return after the moment it was wanted for. Authored content on the
	// entity's own class is loaded by the time that class is running anyway.
	return Actions[ActionId - 1].Sequence.LoadSynchronous();
}

bool UCrowdyMassAnimSet::ValidateAction(FName ActionName, const TArray<UAnimSequence*>& BankSequences) const
{
	const FCrowdyMassActionEntry* Entry = FindAction(ActionName);
	if (!Entry)
	{
		return false;
	}

	return ClassifyActionPair(*Entry, BankSequences) == ECrowdyMassActionPairStatus::Valid;
}

float UCrowdyMassAnimSet::GetActionDurationSeconds(FName ActionName, const TArray<UAnimSequence*>& BankSequences) const
{
	const FCrowdyMassActionEntry* Entry = FindAction(ActionName);
	if (!Entry || !BankSequences.IsValidIndex(Entry->PrimaryIndex))
	{
		return 0.0f;
	}

	return GetSequenceDuration(BankSequences[Entry->PrimaryIndex]);
}

bool UCrowdyMassAnimSet::ValidateAgainstBank(const TArray<UAnimSequence*>& BankSequences) const
{
	bool bAllValid = true;
	// The flag is ignored here on purpose: no per-row pass has run at runtime, so every problem reported
	// here is the only report that fault is going to get.
	ForEachBankProblem(BankSequences, [&bAllValid](const FString& Problem, bool)
	{
		bAllValid = false;
		UE_LOG(LogCrowdyMass, Warning, TEXT("%s"), *Problem);
	});

	return bAllValid;
}

void UCrowdyMassAnimSet::ForEachBankProblem(const TArray<UAnimSequence*>& BankSequences,
	TFunctionRef<void(const FString&, bool)> Report) const
{
	// Which slot each gait landed on, kept so the pose collision pass below can ask without resolving
	// anything a second time. INDEX_NONE for a gait that did not resolve, which cannot collide.
	TArray<int32, TInlineAllocator<4>> GaitBankSlots;
	GaitBankSlots.Reserve(Gaits.Num());

	for (const FCrowdyMassGaitEntry& Gait : Gaits)
	{
		const FCrowdyMassResolvedSequence GaitResolved = ResolveSequenceAsset(Gait.Sequence, BankSequences);
		GaitBankSlots.Add(GaitResolved.bResolved ? GaitResolved.BankIndex : INDEX_NONE);

		if (!GaitResolved.bResolved)
		{
			// An UNSET sequence is the same fault IsDataValid raises as "has no Sequence set"; a set one
			// that is simply absent from the bank is a fault only a bank can see.
			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: gait %s did not resolve against the supplied animation bank"), *GetName(), *Gait.GaitName.ToString()),
				Gait.Sequence.IsNull());
		}
	}

	// Kept so the pose collision pass below can ask without classifying every action again for every pose,
	// which is the same reason the gait and pose slots above are kept. A pair's status depends only on the
	// row and the bank, so recomputing it per pose answers the identical question P times.
	TArray<ECrowdyMassActionPairStatus, TInlineAllocator<8>> ActionPairStatuses;
	ActionPairStatuses.Reserve(Actions.Num());

	for (const FCrowdyMassActionEntry& Action : Actions)
	{
		const ECrowdyMassActionPairStatus Status = ClassifyActionPair(Action, BankSequences);
		ActionPairStatuses.Add(Status);

		if (Status == ECrowdyMassActionPairStatus::SameIndex)
		{
			// Always the same fault IsDataValid raises per row, and that includes every action row still
			// at INDEX_NONE for both slots, which is every row before a sync.
			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: action %s slot pair (%d, %d) uses the same index for both slots, retriggering it will not restart playback because the engine only reacts to an index change"), *GetName(), *Action.ActionName.ToString(), Action.PrimaryIndex, Action.AlternateIndex),
				true);
		}
		// The actor half of the same row. An action is authored once and shown by two representations, so
		// the asset an actor plays and the slots the crowd renders have to be the same clip; if they are
		// not, one player sees a swing and another sees something else for the same event, and every
		// check above passes because each side resolves perfectly well on its own.
		//
		// An unset asset is not an error. It means the action is crowd-only, and an actor asking for it is
		// told so where it asks rather than here, once per authored row, for every project that has not
		// filled these in yet.
		if (!Action.Sequence.IsNull() && BankSequences.IsValidIndex(Action.PrimaryIndex))
		{
			const UAnimSequence* SlotAsset = BankSequences[Action.PrimaryIndex];
			if (SlotAsset && Action.Sequence.ToSoftObjectPath() != FSoftObjectPath(SlotAsset))
			{
				Report(FString::Printf(
					TEXT("CrowdyMassAnimSet %s: action %s names sequence '%s' for an actor but its slot %d renders '%s' for the crowd, so the two representations of this action would play different clips"),
					*GetName(), *Action.ActionName.ToString(), *Action.Sequence.ToString(),
					Action.PrimaryIndex, *SlotAsset->GetPathName()), false);
			}
		}

		if (Status == ECrowdyMassActionPairStatus::DifferentAsset)
		{
			// Split from the message below because the two have different fixes and only one of them is
			// "point both slots at one clip". A pair off the end of the bank names slots that do not
			// exist, and telling that author their slots disagree sends them to compare two clips they
			// cannot open. Reported with the bank's length, which is the number they need.
			const bool bPrimaryInBank = BankSequences.IsValidIndex(Action.PrimaryIndex);
			const bool bAlternateInBank = BankSequences.IsValidIndex(Action.AlternateIndex);
			if (!bPrimaryInBank || !bAlternateInBank)
			{
				Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: action %s slot pair (%d, %d) is outside the animation bank, which has %d slot(s), so this action can never play. Use Sync Actions Into Bank, or author indices within 0..%d."),
					*GetName(), *Action.ActionName.ToString(), Action.PrimaryIndex, Action.AlternateIndex,
					BankSequences.Num(), BankSequences.Num() - 1), false);
			}
			else
			{
				Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: action %s slot pair (%d, %d) does not reference the same bank asset, retriggering it will not restart playback"), *GetName(), *Action.ActionName.ToString(), Action.PrimaryIndex, Action.AlternateIndex), false);
			}
		}
	}

	// Parallel to Poses, INDEX_NONE where the row is the NAME_None sentinel or did not resolve. Inline
	// storage for the same reason the gait array has it: an ordinary set authors a handful of rows, and
	// this walk runs whenever a bank is first paired with the asset.
	TArray<int32, TInlineAllocator<8>> PoseBankSlots;
	PoseBankSlots.Reserve(Poses.Num());

	for (const FCrowdyMassPoseEntry& Pose : Poses)
	{
		// NAME_None rows are the documented "no pose" sentinel, not a broken lookup: nothing to
		// resolve.
		if (Pose.PoseName.IsNone())
		{
			PoseBankSlots.Add(INDEX_NONE);
			continue;
		}

		const FCrowdyMassResolvedSequence PoseResolved = ResolveSequenceAsset(Pose.Sequence, BankSequences);
		PoseBankSlots.Add(PoseResolved.bResolved ? PoseResolved.BankIndex : INDEX_NONE);

		if (!PoseResolved.bResolved)
		{
			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: pose %s did not resolve against the supplied animation bank"), *GetName(), *Pose.PoseName.ToString()),
				Pose.Sequence.IsNull());
		}
	}

	// A pose has to own its bank slot outright. The renderer only reacts to the pushed index changing,
	// so an entity that leaves a pose for an animation sharing that slot is handed the index it already
	// has, the push is dropped as a no-op, and it stays frozen in the pose it just left. Every entry
	// involved resolves correctly on its own, which is why nothing above and no authoring check can see
	// this: it is a property of the three of them against one bank.
	for (int32 PoseIndex = 0; PoseIndex < PoseBankSlots.Num(); ++PoseIndex)
	{
		const int32 PoseSlot = PoseBankSlots[PoseIndex];
		if (PoseSlot == INDEX_NONE)
		{
			continue;
		}

		const FName PoseName = Poses[PoseIndex].PoseName;

		for (int32 GaitIndex = 0; GaitIndex < GaitBankSlots.Num(); ++GaitIndex)
		{
			if (GaitBankSlots[GaitIndex] != PoseSlot)
			{
				continue;
			}

			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: pose %s (pose id %d) shares bank slot %d with gait %s, so an entity leaving that pose is handed the index it already has and stays frozen in the pose. Give the pose a sequence of its own in the bank."),
				*GetName(), *PoseName.ToString(), PoseIndex + 1, PoseSlot, *Gaits[GaitIndex].GaitName.ToString()), false);
		}

		for (int32 OtherIndex = PoseIndex + 1; OtherIndex < PoseBankSlots.Num(); ++OtherIndex)
		{
			if (PoseBankSlots[OtherIndex] != PoseSlot)
			{
				continue;
			}

			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: poses %s (pose id %d) and %s (pose id %d) share bank slot %d, so switching directly between them changes nothing on screen. Give each pose a sequence of its own in the bank."),
				*GetName(), *PoseName.ToString(), PoseIndex + 1, *Poses[OtherIndex].PoseName.ToString(), OtherIndex + 1, PoseSlot), false);
		}

		for (int32 ActionIndex = 0; ActionIndex < Actions.Num(); ++ActionIndex)
		{
			const FCrowdyMassActionEntry& Action = Actions[ActionIndex];

			// An unresolvable pair is never selected, so it cannot be the thing an entity comes back to.
			if (ActionPairStatuses[ActionIndex] != ECrowdyMassActionPairStatus::Valid)
			{
				continue;
			}

			if (Action.PrimaryIndex != PoseSlot && Action.AlternateIndex != PoseSlot)
			{
				continue;
			}

			Report(FString::Printf(TEXT("CrowdyMassAnimSet %s: pose %s (pose id %d) shares bank slot %d with action %s, whose slot pair is (%d, %d). An action landing on that slot leaves the pose unable to show itself again. Give the pose a sequence of its own in the bank."),
				*GetName(), *PoseName.ToString(), PoseIndex + 1, PoseSlot, *Action.ActionName.ToString(), Action.PrimaryIndex, Action.AlternateIndex), false);
		}
	}
}

float UCrowdyMassAnimSet::ComputeSmoothingAlpha(float TimeConstantSeconds, float DeltaSeconds, float MaxStepSeconds)
{
	// A time constant at or below zero means no smoothing was asked for, so follow the input exactly.
	if (TimeConstantSeconds <= UE_KINDA_SMALL_NUMBER)
	{
		return 1.0f;
	}

	const float StepSeconds = FMath::Clamp(DeltaSeconds, 0.0f, FMath::Max(0.0f, MaxStepSeconds));

	// Framerate independent: the same wall-clock time closes the same fraction of the gap whatever
	// the frame length. A plain per-frame constant would make gaits change faster on a fast machine.
	return 1.0f - FMath::Exp(-StepSeconds / TimeConstantSeconds);
}

float UCrowdyMassAnimSet::AdvanceSmoothedSpeed(float PreviousSmoothedSpeed, float RawSpeed, float Alpha)
{
	// A NaN survives every arithmetic step and defeats every comparison, so an entity that took one
	// would hold its current gait forever: it would satisfy neither the test for speeding up nor the
	// test for slowing down. Position comes off the wire, so this is reachable from a forged packet.
	const float SafeRawSpeed = FMath::IsFinite(RawSpeed) ? RawSpeed : 0.0f;
	const float SafePreviousSpeed = FMath::IsFinite(PreviousSmoothedSpeed) ? PreviousSmoothedSpeed : 0.0f;

	return FMath::Lerp(SafePreviousSpeed, SafeRawSpeed, FMath::Clamp(Alpha, 0.0f, 1.0f));
}

const FCrowdyMassActionEntry* UCrowdyMassAnimSet::FindAction(FName ActionName) const
{
	return Actions.FindByPredicate([ActionName](const FCrowdyMassActionEntry& Candidate)
	{
		return Candidate.ActionName == ActionName;
	});
}

int32 UCrowdyMassAnimSet::FindInBank(const UAnimSequence* Sequence, const TArray<UAnimSequence*>& BankSequences)
{
	return BankSequences.IndexOfByKey(Sequence);
}

float UCrowdyMassAnimSet::GetSequenceDuration(const UAnimSequence* Sequence)
{
	return Sequence ? Sequence->GetPlayLength() : 0.0f;
}

FCrowdyMassResolvedSequence UCrowdyMassAnimSet::ResolveSequenceAsset(const TSoftObjectPtr<UAnimSequence>& SequenceAsset, const TArray<UAnimSequence*>& BankSequences)
{
	FCrowdyMassResolvedSequence Result;

	// A sequence that is not already loaded cannot be in BankSequences either (the bank is built from
	// loaded assets), so a synchronous load could never change the outcome here. This can run on a
	// Mass worker thread, where a blocking package load is not a supported operation.
	UAnimSequence* Sequence = SequenceAsset.Get();
	if (!Sequence)
	{
		return Result;
	}

	const int32 BankIndex = FindInBank(Sequence, BankSequences);
	if (BankIndex == INDEX_NONE)
	{
		return Result;
	}

	Result.BankIndex = BankIndex;
	Result.DurationSeconds = GetSequenceDuration(Sequence);
	Result.bResolved = true;
	return Result;
}

ECrowdyMassActionPairStatus UCrowdyMassAnimSet::ClassifyActionPair(const FCrowdyMassActionEntry& Entry, const TArray<UAnimSequence*>& BankSequences)
{
	// The engine only pushes a new animation when the bank index changes, so an identical pair can
	// never retrigger even though the indices would resolve to the same asset. This has to be checked
	// before the asset comparison below, which would otherwise call an identical pair valid.
	if (Entry.PrimaryIndex == Entry.AlternateIndex)
	{
		return ECrowdyMassActionPairStatus::SameIndex;
	}

	const UAnimSequence* Primary = BankSequences.IsValidIndex(Entry.PrimaryIndex) ? BankSequences[Entry.PrimaryIndex] : nullptr;
	const UAnimSequence* Alternate = BankSequences.IsValidIndex(Entry.AlternateIndex) ? BankSequences[Entry.AlternateIndex] : nullptr;

	if (Primary == nullptr || Primary != Alternate)
	{
		return ECrowdyMassActionPairStatus::DifferentAsset;
	}

	return ECrowdyMassActionPairStatus::Valid;
}

void FCrowdyMassResolvedAnimSet::Build(const UCrowdyMassAnimSet* AnimSet, const TArray<UAnimSequence*>& BankSequences)
{
	*this = FCrowdyMassResolvedAnimSet();
	BankSize = BankSequences.Num();
	BankContentHash = HashBankContents(BankSequences);
	bBuilt = true;

	if (!AnimSet)
	{
		return;
	}

	// Read before anything is resolved from the rows, so a revision recorded here can never be newer than
	// the rows this table was actually built from.
	SourceRevision = AnimSet->GetSourceRevision();

	// One entry per authored gait row, in order, so gait id N+1 is always ResolvedGaits[N]. A row is
	// never skipped, so the ids keep meaning the same rows whether or not a row above them resolved.
	//
	// Rows past the limit are dropped rather than kept: a gait id has one byte to live in with 0
	// meaning none, so a row past that could only be given an id that truncates to "no gait".
	const int32 GaitRowCount = FMath::Min(AnimSet->Gaits.Num(), CrowdyMassMaxGaits);
	ResolvedGaits.Reserve(GaitRowCount);

	for (int32 GaitIndex = 0; GaitIndex < GaitRowCount; ++GaitIndex)
	{
		const FCrowdyMassGaitEntry& Gait = AnimSet->Gaits[GaitIndex];
		FCrowdyMassResolvedGait& Entry = ResolvedGaits.AddDefaulted_GetRef();

		// Resolved from THIS row's own sequence rather than through ResolveGait, because the floor
		// fallback belongs to selection: a table that recorded the floor row's index under a faster row
		// could never report that the faster row is missing, and every entity would silently be told it
		// is running while playing an idle.
		const FCrowdyMassResolvedSequence Resolved = UCrowdyMassAnimSet::ResolveSequenceAsset(Gait.Sequence, BankSequences);

		// An unresolved row keeps its default speeds, which are above every real speed, so it can never
		// be entered and is left the moment it somehow becomes current.
		if (!Resolved.bResolved)
		{
			continue;
		}

		Entry.BankIndex = Resolved.BankIndex;
		Entry.EnterSpeed = Gait.EnterSpeed;
		Entry.ExitSpeed = Gait.ExitSpeed;
		Entry.PlayRate = Gait.PlayRate;
	}

	// Which pose row each arriving state value selects, walked off the same authored rows as the table
	// below and resolved from the row being filled.
	UCrowdyMassAnimSet::FillPoseStateTable(*AnimSet, PoseStateTable);

	// One entry per authored pose, in order, so PoseId N+1 is always ResolvedPoses[N], the same
	// convention ResolvedActions uses for ActionId. A row is never skipped, including a duplicate name
	// or the NAME_None sentinel (which resolves to nothing here, matching ResolvePose's own treatment
	// of it): skipping a row would shift every id authored after it. FindPoseId, not this table, is
	// what picks the first match when a name is duplicated.
	//
	// The authored rank, play rate and loop mode are copied in alongside the bank index, so that
	// selecting a pose per entity per frame reads this table and nothing else. Reading them back off
	// the asset instead would put a UObject dereference on the hot path for values that cannot change
	// without the asset being re-resolved anyway.
	//
	// Resolved from THIS row's own sequence rather than through ResolvePose, which searches by name.
	// The two differ exactly when a name is duplicated, and there the name search is wrong: it would
	// give row N the first matching row's bank index while row N kept its own rank, play rate and loop
	// mode, producing a pose that exists in no authored row at all.
	ResolvedPoses.Reserve(AnimSet->Poses.Num());
	for (const FCrowdyMassPoseEntry& Pose : AnimSet->Poses)
	{
		FCrowdyMassResolvedPose& Entry = ResolvedPoses.AddDefaulted_GetRef();
		Entry.Rank = Pose.Rank;
		Entry.PlayRate = Pose.PlayRate;
		Entry.LoopMode = Pose.bHoldLastFrame ? EAnimSequenceTrackLoopMode::Clamp : EAnimSequenceTrackLoopMode::Loop;

		if (Pose.PoseName.IsNone())
		{
			continue;
		}

		const FCrowdyMassResolvedSequence PoseResolved = UCrowdyMassAnimSet::ResolveSequenceAsset(Pose.Sequence, BankSequences);
		if (PoseResolved.bResolved)
		{
			Entry.BankIndex = PoseResolved.BankIndex;
		}
	}

	// One entry per authored action, in order, so ActionId N+1 is always ResolvedActions[N] with no
	// lookup: FCrowdyMassActionFragment::ActionId is defined as that 1-based index specifically so
	// this table can be plain array indexing on the hot path. An entry stays at its default
	// (both indices INDEX_NONE) when the pair does not resolve, rather than being skipped, so the
	// array positions keep meaning "ActionId N+1" for every action authored after a broken one.
	//
	// Validated from THIS row rather than through ValidateAction, for the same reason the poses above
	// are, and the consequence here is worse than a wrong animation: a name search validates the first
	// row carrying the name and the loop then copies this row's own indices in regardless, so a
	// duplicate action name could hand the renderer a slot index that is not in the bank at all.
	ResolvedActions.Reserve(AnimSet->Actions.Num());
	for (const FCrowdyMassActionEntry& Action : AnimSet->Actions)
	{
		FCrowdyMassResolvedAction& Entry = ResolvedActions.AddDefaulted_GetRef();
		if (UCrowdyMassAnimSet::ClassifyActionPair(Action, BankSequences) != ECrowdyMassActionPairStatus::Valid)
		{
			continue;
		}

		Entry.PrimaryBankIndex = Action.PrimaryIndex;
		Entry.AlternateBankIndex = Action.AlternateIndex;
		Entry.DurationSeconds = UCrowdyMassAnimSet::GetSequenceDuration(BankSequences[Action.PrimaryIndex]);
	}
}

uint32 FCrowdyMassResolvedAnimSet::HashBankContents(const TArray<UAnimSequence*>& BankSequences)
{
	// Order matters as much as membership: reordering two rows changes what every index between them
	// means, so the hash has to fold the position in as well as the asset.
	uint32 Hash = static_cast<uint32>(BankSequences.Num());
	for (const UAnimSequence* Sequence : BankSequences)
	{
		Hash = HashCombine(Hash, GetTypeHash(FObjectKey(Sequence)));
	}

	return Hash;
}

bool FCrowdyMassResolvedAnimSet::MatchesSource(const UCrowdyMassAnimSet* AnimSet, const TArray<UAnimSequence*>& BankSequences) const
{
	if (!bBuilt || BankSize != BankSequences.Num() || BankContentHash != HashBankContents(BankSequences))
	{
		return false;
	}

	// A table built from no set at all describes no rows, so there is no row revision for it to still
	// agree with; the bank comparison above is the whole of its identity.
	if (!AnimSet)
	{
		return true;
	}

	return SourceRevision == AnimSet->GetSourceRevision();
}

uint8 FCrowdyMassResolvedAnimSet::SelectGait(uint8 CurrentGaitId, float SmoothedSpeed) const
{
	// A set with no authored gait row has nothing to select, and answering row 1 anyway would hand the
	// compose rule an id it cannot resolve.
	if (ResolvedGaits.IsEmpty())
	{
		return 0;
	}

	const int32 LastRow = ResolvedGaits.Num() - 1;

	// An entity that has never been given a gait (id 0) starts from the floor row, which is where a
	// standing entity belongs and what every faster row is entered from.
	int32 RowIndex = FMath::Clamp(static_cast<int32>(CurrentGaitId) - 1, 0, LastRow);

	// Speeding up: take the fastest row whose enter speed has been reached. Searching downwards from
	// the fastest rather than stepping up one row at a time means an entity that accelerates hard
	// between two frames arrives at the right gait immediately instead of walking through every gait
	// below it.
	for (int32 Candidate = LastRow; Candidate > RowIndex; --Candidate)
	{
		if (SmoothedSpeed >= ResolvedGaits[Candidate].EnterSpeed)
		{
			return static_cast<uint8>(Candidate + 1);
		}
	}

	// Slowing down: drop while the speed is below the current row's exit speed, stopping at the floor
	// row, whose own exit speed is never read. Exit speeds sit below enter speeds, so an entity that
	// just sped up cannot immediately fall back out.
	while (RowIndex > 0 && SmoothedSpeed < ResolvedGaits[RowIndex].ExitSpeed)
	{
		--RowIndex;
	}

	return static_cast<uint8>(RowIndex + 1);
}

const FCrowdyMassResolvedGait* FCrowdyMassResolvedAnimSet::FindResolvedGait(uint8 GaitId) const
{
	// Gait id 0 means no gait has been resolved for the entity and is never looked up, matching
	// FindResolvedPose's treatment of pose id 0.
	if (GaitId == 0)
	{
		return nullptr;
	}

	const int32 Index = static_cast<int32>(GaitId) - 1;
	return ResolvedGaits.IsValidIndex(Index) ? &ResolvedGaits[Index] : nullptr;
}

const FCrowdyMassResolvedPose* FCrowdyMassResolvedAnimSet::FindResolvedPose(uint8 PoseId) const
{
	// 0 means no pose is set and is never looked up, matching FindResolvedAction's treatment of
	// ActionId 0.
	if (PoseId == 0)
	{
		return nullptr;
	}

	const int32 Index = static_cast<int32>(PoseId) - 1;
	return ResolvedPoses.IsValidIndex(Index) ? &ResolvedPoses[Index] : nullptr;
}

const FCrowdyMassResolvedAction* FCrowdyMassResolvedAnimSet::FindResolvedAction(int32 ActionId) const
{
	// ActionId 0 means inactive and is never looked up; anything else is the 1-based index the
	// table was built at, so ActionId 1 is index 0.
	return ResolvedActions.IsValidIndex(ActionId - 1) ? &ResolvedActions[ActionId - 1] : nullptr;
}

FCrowdyMassComposedAnimation ComposeCrowdyMassAnimation(const FCrowdyMassAnimStateFragment& AnimState, const FCrowdyMassActionFragment& ActionState, uint8 PoseId, const FCrowdyMassResolvedAnimSet& Resolved, float BlendSeconds)
{
	FCrowdyMassComposedAnimation Composed;
	Composed.BlendSeconds = BlendSeconds;

	// Resolved once, before the ordering below, because the pose is compared against two separate
	// sources and a pose naming something this bank cannot show has to lose to both rather than
	// winning against one and producing nothing. An unresolvable pose is demoted to no pose at all
	// here, which is what lets the rest of this read as a plain rank comparison and what makes an
	// authoring gap fall through to the sources below instead of freezing the entity.
	const FCrowdyMassResolvedPose* Pose = Resolved.FindResolvedPose(PoseId);
	const bool bPoseResolved = Pose && Pose->BankIndex != INDEX_NONE;

	if (bPoseResolved && Pose->Rank == ECrowdyMassPoseRank::AboveActions)
	{
		Composed.BankIndex = Pose->BankIndex;
		Composed.PlayRate = Pose->PlayRate;
		Composed.LoopMode = Pose->LoopMode;
		return Composed;
	}

	if (ActionState.ActionId != 0)
	{
		const FCrowdyMassResolvedAction* Action = Resolved.FindResolvedAction(ActionState.ActionId);
		if (Action && Action->PrimaryBankIndex != INDEX_NONE)
		{
			// Whether the renderer is already showing THIS trigger, which is the question the slot
			// choice turns on. Both halves matter: the Serial says the trigger has been handed over,
			// and the index check says it is still the thing on screen rather than something that
			// outranked it in the meantime and has since cleared.
			const bool bAlreadyShowingThisTrigger =
				ActionState.AppliedSerial == ActionState.Serial
				&& (AnimState.SequenceIndex == Action->PrimaryBankIndex
					|| AnimState.SequenceIndex == Action->AlternateBankIndex);

			// A trigger that has not been shown takes whichever slot is NOT the one on screen, so the
			// pushed index always changes and the renderer always restarts the clip. Choosing it from
			// the Serial's parity instead reads as equivalent and is not: an even number of triggers
			// between two animation passes returns the parity to where it started, the index does not
			// change, and the push is skipped as a no-op.
			Composed.BankIndex = bAlreadyShowingThisTrigger
				? AnimState.SequenceIndex
				: (AnimState.SequenceIndex == Action->PrimaryBankIndex
					? Action->AlternateBankIndex
					: Action->PrimaryBankIndex);

			Composed.bIsFirstShowOfTrigger = !bAlreadyShowingThisTrigger;
			Composed.PlayRate = ActionState.PlayRate;
			// A one-shot holds its last pose instead of restarting; every other tier here is a
			// continuous pose and loops.
			Composed.LoopMode = EAnimSequenceTrackLoopMode::Clamp;
			Composed.bIsAction = true;
			Composed.ActionDurationSeconds = Action->DurationSeconds;
			return Composed;
		}

		// ActionId is set but did not resolve (unauthored, or a broken slot pair): fall through to
		// the sources below exactly as an unresolved pose would, rather than showing nothing.
	}

	// Every pose that resolves outranks locomotion. A pose ranked below actions has simply had its
	// turn come round now that no action is playing, and one ranked above them was already taken
	// above; either way an entity holding a pose never falls back to jogging underneath it.
	if (bPoseResolved)
	{
		Composed.BankIndex = Pose->BankIndex;
		Composed.PlayRate = Pose->PlayRate;
		Composed.LoopMode = Pose->LoopMode;
		return Composed;
	}

	// One lookup for both halves of the gait, rather than one per field: the row carries its own bank
	// index and play rate together, and this runs for every entity every frame.
	if (const FCrowdyMassResolvedGait* Gait = Resolved.FindResolvedGait(AnimState.Gait))
	{
		Composed.BankIndex = Gait->BankIndex;
		Composed.PlayRate = Gait->PlayRate;
	}

	return Composed;
}

bool ShouldPushCrowdyMassAnimation(int32 DesiredBankIndex, int32 CurrentBankIndex, int32 PushedBankIndex)
{
	if (DesiredBankIndex == INDEX_NONE)
	{
		return false;
	}

	// Nothing has been pushed for this entity yet, so the index it currently reads is the default
	// rather than anything anyone chose. Push once even if the two happen to agree, or an entity
	// whose selection is bank slot 0 keeps the default playback rate and blend for its whole life.
	if (PushedBankIndex == INDEX_NONE)
	{
		return true;
	}

	return DesiredBankIndex != CurrentBankIndex;
}

float GetCrowdyMassActionCorrectedPosition(float ElapsedSecondsSinceTrigger, float DurationSeconds)
{
	// A non-finite input cannot come from anything authored here (both operands are locally
	// accumulated floats), but Position feeds a renderer that has no defined behaviour for NaN or
	// infinity, so treat one as "just started" rather than propagate it.
	const float SafeElapsedSeconds = FMath::IsFinite(ElapsedSecondsSinceTrigger) ? FMath::Max(ElapsedSecondsSinceTrigger, 0.0f) : 0.0f;

	if (!FMath::IsFinite(DurationSeconds) || DurationSeconds <= 0.0f)
	{
		return 0.0f;
	}

	return FMath::Min(SafeElapsedSeconds, DurationSeconds);
}

#if WITH_EDITOR
void UCrowdyMassAnimSet::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Dropped wholesale rather than per row, and for every property rather than only the two the table
	// is derived from: a value that resolves to nothing is as much an answer as one that resolves to a
	// row, so keeping any part of it would leave a row added on the strength of a validation message
	// still selecting nothing.
	InvalidatePoseStateTable();
}

void UCrowdyMassAnimSet::PostEditUndo()
{
	Super::PostEditUndo();

	InvalidatePoseStateTable();

	// An undo does NOT go through PostEditChangeProperty, so it broadcasts no property-changed event and
	// the subsystem's own per-class caches never hear about it. Dropping this set's table alone is not
	// enough since the enum this set names is what decides WHICH replicated property a class binds, so an
	// undo that changes it leaves classes bound against an enum the set no longer names, or leaves them
	// unbound after an undo that restores one. Told directly, because there is nothing else to tell it.
	UCrowdyMassEntitySubsystem::InvalidateAnimSetCachesInEveryWorld();

	// A derived config copies this set's Bank, and an undo that restores or clears it reports no field.
	UCrowdyMassEntitySubsystem::InvalidateDerivedConfigsInEveryWorld();
}

EDataValidationResult UCrowdyMassAnimSet::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	// A gait id is one byte with 0 reserved for "no gait", so rows past this can never be selected.
	// Reported against the array rather than per row, because the fault is the array's length and every
	// row past the limit is individually well authored.
	if (Gaits.Num() > CrowdyMassMaxGaits)
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Gaits has %d entries, and a gait id is one byte with 0 reserved for 'no gait', so only the first %d can ever be selected."),
			Gaits.Num(), CrowdyMassMaxGaits)));
	}

	TSet<FName> SeenGaitNames;
	TSet<FSoftObjectPath> SeenGaitSequences;
	for (int32 GaitIndex = 0; GaitIndex < Gaits.Num(); ++GaitIndex)
	{
		const FCrowdyMassGaitEntry& Gait = Gaits[GaitIndex];

		// Every message names the row's position as well as its name, because the row order is what
		// decides which gait an entity speeds up into and a name alone leaves the author counting rows.
		const FString GaitLabel = FString::Printf(TEXT("Gaits row %d ('%s')"), GaitIndex, *Gait.GaitName.ToString());

		// Two gaits sharing one sequence resolve to the same bank index, and the renderer only reacts
		// to an index changing, so switching between them does nothing at all: the second gait's own
		// playback rate never reaches the renderer and both gaits look identical. This is the tempting
		// shape when a bank is short of clips, which is exactly when it needs saying.
		if (!Gait.Sequence.IsNull())
		{
			if (SeenGaitSequences.Contains(Gait.Sequence.ToSoftObjectPath()))
			{
				Result = EDataValidationResult::Invalid;
				Context.AddError(FText::FromString(FString::Printf(
					TEXT("%s reuses a sequence another gait already uses ('%s'). Both resolve to the same bank slot, so switching between them changes nothing and the per-gait PlayRate is discarded. Give each gait its own sequence."),
					*GaitLabel, *Gait.Sequence.ToString())));
			}
			SeenGaitSequences.Add(Gait.Sequence.ToSoftObjectPath());
		}

		if (Gait.GaitName.IsNone())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Gaits row %d has no GaitName set. It still occupies its speed position, but nothing that reports a gait can name it."),
				GaitIndex)));
		}
		else if (SeenGaitNames.Contains(Gait.GaitName))
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Gaits has more than one row named '%s'. Selection works off row order so both are reachable, but every message about either one names the same gait."),
				*Gait.GaitName.ToString())));
		}
		SeenGaitNames.Add(Gait.GaitName);

		if (Gait.Sequence.IsNull())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(TEXT("%s has no Sequence set."), *GaitLabel)));
		}
		else if (!Gait.Sequence.LoadSynchronous())
		{
			// A soft pointer to a deleted asset is not null, so every other check here passes and the gait
			// silently falls back to the floor row at runtime.
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("%s names Sequence '%s', which does not load. The asset has been deleted or renamed, so this gait resolves against no bank slot and falls back to the floor gait. Point it at a sequence that exists."),
				*GaitLabel, *Gait.Sequence.ToString())));
		}

		// The first row is the floor gait selection falls back to, so it is entered by having nowhere
		// slower to go rather than by reaching a speed, and its own speeds are never read.
		if (GaitIndex == 0)
		{
			continue;
		}

		if (Gait.ExitSpeed > Gait.EnterSpeed)
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("%s has ExitSpeed %.1f above EnterSpeed %.1f; an entity would leave this gait on the same frame it entered it."),
				*GaitLabel, Gait.ExitSpeed, Gait.EnterSpeed)));
		}
		// Equal speeds leave no hysteresis band at all, so an entity holding a speed anywhere near the
		// threshold changes animation every frame. Only worth reporting once a speed has been
		// authored; a row still at its defaults is covered by the warning below instead.
		else if (Gait.EnterSpeed > 0.0f && Gait.ExitSpeed == Gait.EnterSpeed)
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("%s has ExitSpeed equal to EnterSpeed (%.1f), leaving no hysteresis band; an entity at that speed changes animation every frame. Author ExitSpeed below EnterSpeed."),
				*GaitLabel, Gait.EnterSpeed)));
		}

		// Not an error: a set can deliberately leave a gait's speeds at zero while its sequence is
		// still being authored. It is worth naming, because the effect is invisible in the details
		// panel and total.
		if (Gait.EnterSpeed <= 0.0f)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("%s has an EnterSpeed of zero, so it is entered at any speed and every slower row, including the floor row, can never be shown."),
				*GaitLabel)));
		}
	}

	// Gait selection walks the rows in authored order, so a row entered at a lower speed than the row
	// above it makes that one unreachable rather than producing an error anywhere. Row 0 is skipped as
	// the comparison's slower side because it is the floor and its EnterSpeed is never read.
	for (int32 GaitIndex = 2; GaitIndex < Gaits.Num(); ++GaitIndex)
	{
		const FCrowdyMassGaitEntry& Faster = Gaits[GaitIndex];
		const FCrowdyMassGaitEntry& Slower = Gaits[GaitIndex - 1];

		if (Faster.EnterSpeed < Slower.EnterSpeed)
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Gaits row %d ('%s') has EnterSpeed %.1f, below row %d ('%s')'s %.1f, so row %d can never be shown. Author the rows slowest first, with each EnterSpeed above the one before it."),
				GaitIndex, *Faster.GaitName.ToString(), Faster.EnterSpeed,
				GaitIndex - 1, *Slower.GaitName.ToString(), Slower.EnterSpeed, GaitIndex - 1)));
		}
	}

	TSet<FName> SeenActionNames;
	for (const FCrowdyMassActionEntry& Action : Actions)
	{
		if (Action.ActionName.IsNone())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(TEXT("Actions has an entry with no ActionName set.")));
		}
		else if (SeenActionNames.Contains(Action.ActionName))
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Actions has more than one entry named '%s'; only the first will ever be used."),
				*Action.ActionName.ToString())));
		}
		SeenActionNames.Add(Action.ActionName);

		if (Action.PrimaryIndex == Action.AlternateIndex)
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Action '%s' has the same index (%d) for both PrimaryIndex and AlternateIndex; retriggering it will not restart playback."),
				*Action.ActionName.ToString(), Action.PrimaryIndex)));
		}
	}

	// A pose id is one byte with 0 reserved for "no pose", so rows past this cannot be selected at all.
	// Reported against the array rather than per row, because the fault is the array's length and every
	// row past the limit is individually well authored.
	if (Poses.Num() > CrowdyMassMaxPoses)
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Poses has %d entries, and a pose id is one byte with 0 reserved for 'no pose', so only the first %d can ever be selected. Rows past that are refused rather than truncated, because truncating would turn pose id 256 into 0 and clear the pose instead of setting it."),
			Poses.Num(), CrowdyMassMaxPoses)));
	}

	const UEnum* StateEnum = GetPoseStateEnum();

	// A name that names nothing is a different fault from naming no enum at all, and it is the worse of the
	// two: it reads as configured. Reported whether or not any row is authored, because the name is what an
	// author would go and fix.
	const bool bNamesANativeEnum = PoseStateEnumSource == ECrowdyMassPoseStateEnumSource::NativeByName;

	if (bNamesANativeEnum && !PoseStateEnumName.IsEmpty() && !StateEnum)
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Pose State Enum is '%s', and no enum of that name could be found, so no replicated state value can select any pose row. Pick one from the dropdown, or give the enum's full path if two modules declare that name."),
			*PoseStateEnumName)));
	}

	// An ERROR rather than a warning, and the promotion is deliberate. A replicated state value is the only
	// thing that selects a pose row on a crowd entity; the by-name lookup that survives beside it is a
	// local tool path, not a transport. So a set with pose rows and no enum has rows nothing on the wire
	// can ever reach, which is the same as having no rows at all while reading as fully authored.
	//
	// It was a warning first, and that is exactly how this shipped dead: warnings fail no validation run
	// and block no cook, so the one asset in the project sat in precisely this state and every test stayed
	// green because each builds its own set in memory.
	if (!Poses.IsEmpty() && !StateEnum && (!bNamesANativeEnum || PoseStateEnumName.IsEmpty()))
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(
			TEXT("Poses has rows but no state enum is set, so no replicated state value can select any of them and this set cannot pose a crowd entity at all. Set Pose State Enum, and name each row after one of its entries.")));
	}

	// A name resolving to something that is not compiled into a module is the cooked-build trap: the string
	// creates no reference, so nothing makes the cooker keep a Blueprint Enumeration named only here. It
	// resolves in the editor, where the asset registry has it loaded, and resolves to nothing in a packaged
	// build, where the failure is silent because Shipping compiles the warning out. The dropdown does not
	// offer one, so reaching this means the name was typed or carried over from an older asset.
	if (bNamesANativeEnum && StateEnum && !StateEnum->GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn))
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Pose State Enum is named as '%s', which is a Blueprint Enumeration asset rather than an enum declared in C++. Naming an asset by string leaves nothing referencing it, so it is not cooked and this set resolves no enum in a packaged build. Switch Pose State Enum Is to Blueprint Asset and pick it there, which makes the reference a real one."),
			*PoseStateEnumName)));
	}

	TSet<FName> SeenPoseNames;

	// Which row first claimed each state value, so a second row on the same value can be named against
	// the row that actually wins it. Two rows sharing a NAME are reported above instead, so this only
	// fires for two different entries of the enum that happen to hold the same value.
	TMap<int64, int32> FirstPoseIdByStateValue;

	for (int32 PoseIndex = 0; PoseIndex < Poses.Num(); ++PoseIndex)
	{
		const FCrowdyMassPoseEntry& Pose = Poses[PoseIndex];

		// The id a pose is set by is its position in this array, so every message here names the id
		// alongside the name. Whoever is wiring a pose up in code or on the wire is working in ids, and
		// a validation message that only names the row leaves them counting rows by hand.
		const int32 PoseId = PoseIndex + 1;

		if (Pose.PoseName.IsNone())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Poses entry %d has no PoseName set. Name it and nothing can select it by name, though it still occupies pose id %d."),
				PoseIndex, PoseId)));
		}
		else if (SeenPoseNames.Contains(Pose.PoseName))
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Poses has more than one entry named '%s'; FindPoseId only ever returns the first, so pose id %d can never be reached by name."),
				*Pose.PoseName.ToString(), PoseId)));
		}
		const bool bNameAlreadySeen = SeenPoseNames.Contains(Pose.PoseName);
		SeenPoseNames.Add(Pose.PoseName);

		if (Pose.Sequence.IsNull())
		{
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Poses entry '%s' (pose id %d) has no Sequence set."), *Pose.PoseName.ToString(), PoseId)));
		}
		else if (!Pose.Sequence.LoadSynchronous())
		{
			// A soft pointer to a deleted asset is not null, so every other check here passes and the pose
			// silently falls back to the floor gait at runtime.
			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Poses entry '%s' (pose id %d) names Sequence '%s', which does not load. The asset has been deleted or renamed, so this pose resolves against no bank slot and falls back to the floor gait. Point it at a sequence that exists."),
				*Pose.PoseName.ToString(), PoseId, *Pose.Sequence.ToString())));
		}

		// Everything below is about the row's name as a state value, so it needs both an enum to look
		// the name up in and a name to look up.
		if (StateEnum && !Pose.PoseName.IsNone())
		{
			const int32 EntryIndex = StateEnum->GetIndexByName(Pose.PoseName, EGetByNameFlags::CheckAuthoredName);
			if (EntryIndex == INDEX_NONE)
			{
				Result = EDataValidationResult::Invalid;
				Context.AddError(FText::FromString(FString::Printf(
					TEXT("Poses entry '%s' (pose id %d) is not an entry of %s, so no replicated state value can ever select it. Pick one of that enum's entries, or name the enum this row belongs to instead."),
					*Pose.PoseName.ToString(), PoseId, *StateEnum->GetName())));
			}
			else
			{
				const int64 StateValue = StateEnum->GetValueByIndex(EntryIndex);
				if (StateValue < 0 || StateValue >= FCrowdyMassPoseStateTable::ValueCount)
				{
					Result = EDataValidationResult::Invalid;
					Context.AddError(FText::FromString(FString::Printf(
						TEXT("Poses entry '%s' (pose id %d) names %s entry '%s', whose value %lld is outside the 0 to %d a one-byte replicated state can carry, so that value can never arrive. Declare the enum as 'enum class : uint8' and keep its values inside that range."),
						*Pose.PoseName.ToString(), PoseId, *StateEnum->GetName(), *Pose.PoseName.ToString(),
						StateValue, FCrowdyMassPoseStateTable::ValueCount - 1)));
				}
				else if (const int32* FirstPoseId = FirstPoseIdByStateValue.Find(StateValue))
				{
					// Two rows naming the same NAME are already reported above; reporting them here as
					// well would name one authoring mistake twice.
					if (!bNameAlreadySeen)
					{
						Result = EDataValidationResult::Invalid;
						Context.AddError(FText::FromString(FString::Printf(
							TEXT("Poses entry '%s' (pose id %d) names %s value %lld, which pose id %d already claims, so a state carrying that value always selects the earlier row and this one can only be reached by name. Two enum entries sharing one value cannot select two different poses."),
							*Pose.PoseName.ToString(), PoseId, *StateEnum->GetName(), StateValue, *FirstPoseId)));
					}
				}
				else
				{
					FirstPoseIdByStateValue.Add(StateValue, PoseId);
				}

				// Every pose that resolves outranks locomotion, so a row on the state an entity spends most
				// of its life in stops the whole crowd from ever showing a gait: the pose is always
				// resolved, and gait selection keeps running underneath with its answer discarded. Value 0
				// is that state on almost every enum, because it is what an unset property holds. Not an
				// error, since a crowd meant to play one clip and never walk is a real thing to author.
				if (StateValue == 0)
				{
					Context.AddWarning(FText::FromString(FString::Printf(
						TEXT("Poses entry '%s' (pose id %d) names %s value 0, which is the state an entity holds until the game sets another one. A pose that resolves always outranks locomotion, so entities in that state will never walk or run. Leave the resting state unauthored unless this crowd is meant to hold one clip."),
						*Pose.PoseName.ToString(), PoseId, *StateEnum->GetName())));
				}
			}
		}

		// A pose that outranks actions and loops is almost always a death or knockdown clip with the
		// wrong loop setting: the entity holds the state, so the clip restarts from the top forever
		// instead of settling. Not fatal, since a game can legitimately want a looping override, but it
		// is invisible until someone watches a corpse for a few seconds.
		if (Pose.Rank == ECrowdyMassPoseRank::AboveActions && !Pose.bHoldLastFrame)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Pose '%s' (pose id %d) outranks actions but loops. A state the entity cannot act out of usually wants Hold Last Frame checked, or the clip replays from the start for as long as the pose is held."),
				*Pose.PoseName.ToString(), PoseId)));
		}
	}

	// Everything above is answerable from this asset alone. The rules below need the bank, which is why
	// they lived only in ValidateAgainstBank until now: an authored slot index means nothing without the
	// array it indexes, and until a crowd rendered, nothing had that array.
	TArray<UAnimSequence*> BankSequences;
	if (GatherBankSequences(BankSequences))
	{
		ForEachBankProblem(BankSequences, [&Result, &Context](const FString& Problem, const bool bAlsoNamedByRowRules)
		{
			// Skipped rather than reworded: the per-row pass above already raised this exact fault, and
			// two errors for one mistake is how an author learns to stop reading them. Every unsynced
			// action row hits this, since both its slots are INDEX_NONE.
			if (bAlsoNamedByRowRules)
			{
				return;
			}

			Result = EDataValidationResult::Invalid;
			Context.AddError(FText::FromString(Problem));
		});
	}
	else if (Bank.IsNull())
	{
		// Narrowed to action rows on purpose, and this is the bound rather than a claim of completeness.
		// Without a bank NO bank-dependent rule runs, pose and gait slot collisions included, but a slot
		// PAIR is the only authored data here that nothing else can check at all: a gait or pose names an
		// asset, which the checks above already read, while an action names two raw integers that mean
		// nothing without the array they index. Warning every set that never named a bank would fire on
		// assets whose rows are otherwise fully checked, and a message an author learns to dismiss stops
		// working for the case that needs it.
		if (!Actions.IsEmpty())
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Bank is not set, so the slot pair on each of the %d action row(s) here cannot be checked: those indices mean nothing without the bank they index, and a pair that is identical, out of range, or split across two different clips stays invisible until a crowd renders. Point it at the same transform provider the mesh description for this crowd uses, then use Sync Actions Into Bank to fill the pairs in."),
				Actions.Num())));
		}
	}
	else
	{
		// Named but unreachable is a different fault from never named, and it must not read as "checked
		// and fine": the rows below go unvalidated either way, and only one of the two is a broken
		// reference somebody has to repair.
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Bank names '%s', which did not load or carries no sequences, so nothing here could be checked against a bank."),
			*Bank.ToString())));
	}

	return Result;
}

bool UCrowdyMassAnimSet::GatherBankSequences(TArray<UAnimSequence*>& OutBankSequences) const
{
	OutBankSequences.Reset();
	if (Bank.IsNull())
	{
		return false;
	}

	UAnimSequenceTransformProviderData* Provider = Bank.LoadSynchronous();
	if (!Provider)
	{
		return false;
	}

	// The same array the sync writes, for the same reason it writes it: a provider naming a Sequence List
	// keeps its own Sequences as a copy the engine only refreshes on load and on a property edit, so
	// reading the copy here would let validation judge a pair against one array while the button computed
	// it against another.
	const UAnimSequenceTransformProviderSequenceList* SequenceList = Provider->GetSequenceList();
	const TArray<FAnimSequenceTransformProviderSequence>& BankEntries =
		SequenceList ? SequenceList->GetSequences() : Provider->GetSequences();

	// Nulls are KEPT. A slot index is a position, so dropping an empty row would shift every index after
	// it and quietly repoint every gait, pose and pair authored against this bank.
	OutBankSequences.Reserve(BankEntries.Num());
	bool bAnySequence = false;
	for (const FAnimSequenceTransformProviderSequence& BankEntry : BankEntries)
	{
		OutBankSequences.Add(BankEntry.Sequence);
		bAnySequence |= BankEntry.Sequence != nullptr;
	}

	// Length is not content. A provider holding rows that all name nothing would otherwise read as a
	// usable bank, and every gait and pose would be reported as failing to resolve against it, which
	// describes neither the fault nor its fix.
	return bAnySequence;
}

// The provider's Sequences array is private and has no setter: it is meant to be authored in a details
// panel, so the engine exposes no C++ way to write it. Reached through its own reflection data instead,
// with the element type checked, so an engine upgrade that renames or retypes the array fails here
// loudly rather than writing a differently shaped array into it.
static TArray<FAnimSequenceTransformProviderSequence>* CrowdyMassFindProviderSequences(UAnimSequenceTransformProviderData& Provider)
{
	FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(
		UAnimSequenceTransformProviderData::StaticClass(), TEXT("Sequences"));
	if (!ArrayProperty)
	{
		return nullptr;
	}

	const FStructProperty* Inner = CastField<FStructProperty>(ArrayProperty->Inner);
	if (!Inner || Inner->Struct != FAnimSequenceTransformProviderSequence::StaticStruct())
	{
		return nullptr;
	}

	return ArrayProperty->ContainerPtrToValuePtr<TArray<FAnimSequenceTransformProviderSequence>>(&Provider);
}

namespace
{
	bool IsGameOrPieWorldRunning()
	{
		if (!GEngine)
		{
			return false;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
			{
				return true;
			}
		}
		return false;
	}
}

void UCrowdyMassAnimSet::SyncActionsIntoBank()
{
	if (IsGameOrPieWorldRunning())
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank edits assets, so it does nothing while a game or Play In Editor session is running. Stop the session and run it again."), *GetName());
		return;
	}

	if (Bank.IsNull())
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank needs a Bank, and none is set. Point it at the same transform provider the mesh description for this crowd uses."), *GetName());
		return;
	}

	UAnimSequenceTransformProviderData* Provider = Bank.LoadSynchronous();
	if (!Provider)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank could not load the Bank '%s', so nothing was changed."), *GetName(), *Bank.ToString());
		return;
	}

	// Where the bank actually lives. A provider naming a Sequence List keeps its own Sequences array as a
	// COPY that the engine refreshes from that list on load and on every property edit, so writing the
	// provider there would be discarded the next time either happened, silently and much later.
	UAnimSequenceTransformProviderSequenceList* SequenceList = Provider->GetSequenceList();
	const TArray<FAnimSequenceTransformProviderSequence>& CurrentBank =
		SequenceList ? SequenceList->GetSequences() : Provider->GetSequences();

	// Worked out in full before anything is written, so a run that changes nothing dirties no asset and
	// leaves no undo step for a button press that did nothing.
	FCrowdyMassActionBankSync Sync;
	ComputeActionBankSync(CurrentBank, Sync);

	// Reported BEFORE the nothing-to-do return, not after it. A row whose clip will not load moves neither
	// counter, so the one case where the broken rows are the ONLY thing that happened is exactly the case
	// where returning first would throw the list away and print "nothing needed doing".
	UE_CLOG(!Sync.UnloadableActionNames.IsEmpty(), LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: %d action row(s) name a Sequence that would not load and were left untouched: %s."),
		*GetName(), Sync.UnloadableActionNames.Num(), *FString::Join(Sync.UnloadableActionNames, TEXT(", ")));

	UE_CLOG(!Sync.CrowdOnlyActionNames.IsEmpty(), LogCrowdyMass, Display, TEXT("CrowdyMassAnimSet %s: %d action row(s) name no Sequence and were left untouched, so their slot pairs stay hand-authored: %s. Give a row a Sequence if it should be synced."),
		*GetName(), Sync.CrowdOnlyActionNames.Num(), *FString::Join(Sync.CrowdOnlyActionNames, TEXT(", ")));

	const int32 AppendedCount = Sync.AppendedSlots;
	if (!Sync.HasWork())
	{
		// Says what it measured, which is that there was nothing it could do, rather than claiming every
		// row is correct: the rows named above are neither synced nor correct.
		UE_LOG(LogCrowdyMass, Display, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank had nothing to change. Every action row naming a loadable sequence already has two bank slots and the indices to match."), *GetName());
		return;
	}

	if (AppendedCount > 0)
	{
		if (SequenceList)
		{
			SequenceList->Modify();

			// EVERY provider referencing the list, not only the one this asset names. The list's handler
			// walks all of them and overwrites each one's copy of the array, so recording only ours would
			// leave a sibling mutated outside the transaction: an undo would restore the list and this
			// provider while that sibling kept the synced bank.
			TArray<UAnimSequenceTransformProviderData*> AffectedProviders;
			for (TObjectIterator<UAnimSequenceTransformProviderData> It; It; ++It)
			{
				if (It->GetSequenceList() == SequenceList)
				{
					It->Modify();
					AffectedProviders.Add(*It);
				}
			}

			SequenceList->SetSequences(Sync.Bank);

			// The list's own handler is what copies the array into every provider referencing it, so
			// skipping it would leave this provider holding the pre-sync bank until something else
			// happened to touch the list.
			FPropertyChangedEvent ListChanged(nullptr);
			SequenceList->PostEditChangeProperty(ListChanged);

			// Only a provider's OWN handler rebuilds the derived data the renderer animates from. The
			// list's handler copies the array and marks render state dirty, which is not the same thing:
			// a slot appended without this is in the array with no built animation behind it, so a crowd
			// renders the new action as nothing at all until the asset is reloaded.
			for (UAnimSequenceTransformProviderData* Affected : AffectedProviders)
			{
				FPropertyChangedEvent AffectedChanged(nullptr);
				Affected->PostEditChangeProperty(AffectedChanged);
			}
		}
		else
		{
			TArray<FAnimSequenceTransformProviderSequence>* ProviderSequences = CrowdyMassFindProviderSequences(*Provider);
			if (!ProviderSequences)
			{
				// Names the clips, not just how many. A count is not a followable remedy: an author told
				// to add four slots by hand has no way to know which four.
				TArray<FString> MissingClipNames;
				for (int32 SlotIndex = CurrentBank.Num(); SlotIndex < Sync.Bank.Num(); ++SlotIndex)
				{
					MissingClipNames.Add(GetNameSafe(Sync.Bank[SlotIndex].Sequence));
				}

				UE_LOG(LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank could not reach the Sequences array on '%s' through reflection, so nothing was changed. Add these %d slot(s) to that bank by hand, in this order: %s. If the array is there, report this: it means the engine's own property changed shape."),
					*GetName(), *Provider->GetName(), AppendedCount, *FString::Join(MissingClipNames, TEXT(", ")));
				return;
			}

			Provider->Modify();
			*ProviderSequences = Sync.Bank;

			FPropertyChangedEvent ProviderChanged(nullptr);
			Provider->PostEditChangeProperty(ProviderChanged);
		}
	}
	else if (SequenceList)
	{
		// Pairs moved without the bank growing, and the indices were computed against the LIST. The
		// provider's own copy is only refreshed on load and on a property edit, so without this the
		// pairs would point into an array the provider has not caught up with.
		FPropertyChangedEvent ProviderChanged(nullptr);
		Provider->PostEditChangeProperty(ProviderChanged);
	}

	// Last, so a failure to write the bank above leaves the indices alone rather than pointing them at
	// slots that were never added.
	// Bounded by the computed pairs rather than by Actions, because two PostEditChangeProperty broadcasts
	// have run arbitrary editor listeners since the pairs were worked out. A row added underneath us is a
	// row this run knows nothing about; skipping it is right, writing past the end of Pairs is not.
	Modify();
	const int32 PairCount = FMath::Min(Actions.Num(), Sync.Pairs.Num());
	for (int32 ActionIndex = 0; ActionIndex < PairCount; ++ActionIndex)
	{
		Actions[ActionIndex].PrimaryIndex = Sync.Pairs[ActionIndex].Key;
		Actions[ActionIndex].AlternateIndex = Sync.Pairs[ActionIndex].Value;
	}

	FPropertyChangedEvent SelfChanged(nullptr);
	PostEditChangeProperty(SelfChanged);

	// Display rather than gated behind the trace CVar: this is the only feedback a button press gives,
	// and a button that reports nothing is indistinguishable from one that did nothing. The count is the
	// rows this run actually MOVED, not the rows it was allowed to look at, so a press that fixed one row
	// among nine correct ones does not claim to have rewritten ten.
	UE_LOG(LogCrowdyMass, Display, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank appended %d slot(s) to '%s' and moved the slot pair on %d action row(s)."),
		*GetName(), AppendedCount, *Provider->GetName(), Sync.ChangedPairCount);
}

void UCrowdyMassAnimSet::ComputeActionBankSync(const TArray<FAnimSequenceTransformProviderSequence>& CurrentBank,
	FCrowdyMassActionBankSync& OutSync) const
{
	OutSync.Bank = CurrentBank;
	OutSync.Pairs.Reset();
	OutSync.Pairs.Reserve(Actions.Num());
	OutSync.CrowdOnlyActionNames.Reset();
	OutSync.UnloadableActionNames.Reset();
	OutSync.AppendedSlots = 0;
	OutSync.ChangedPairCount = 0;
	OutSync.bPairsChanged = false;

	// The clips gaits and poses render. An action landing on the slot one of these resolves to produces
	// exactly the collision the validation pass refuses: the gait or pose is handed the index it already
	// holds when the action ends, so it never changes on screen.
	//
	// Held as CLIPS rather than as slot indices because a gait whose clip is absent from the bank resolves
	// to whatever this run appends for it, which is a slot that does not exist yet.
	TSet<const UAnimSequence*> ClaimedClips;
	auto ClaimClip = [&ClaimedClips](const TSoftObjectPtr<UAnimSequence>& SequenceAsset)
	{
		if (const UAnimSequence* Clip = SequenceAsset.IsNull() ? nullptr : SequenceAsset.LoadSynchronous())
		{
			ClaimedClips.Add(Clip);
		}
	};

	for (const FCrowdyMassGaitEntry& Gait : Gaits)
	{
		ClaimClip(Gait.Sequence);
	}

	for (const FCrowdyMassPoseEntry& Pose : Poses)
	{
		// NAME_None rows are the "no pose" sentinel and resolve nothing, so they claim nothing.
		if (!Pose.PoseName.IsNone())
		{
			ClaimClip(Pose.Sequence);
		}
	}

	// Resolution answers with the FIRST slot holding a clip, so only that slot is spoken for. A later
	// duplicate of the same clip is free for an action to use.
	auto IsClaimedSlot = [&OutSync, &ClaimedClips](const int32 SlotIndex)
	{
		const UAnimSequence* Clip = OutSync.Bank[SlotIndex].Sequence;
		if (!Clip || !ClaimedClips.Contains(Clip))
		{
			return false;
		}

		for (int32 Earlier = 0; Earlier < SlotIndex; ++Earlier)
		{
			if (OutSync.Bank[Earlier].Sequence == Clip)
			{
				return false;
			}
		}

		return true;
	};

	for (const FCrowdyMassActionEntry& Action : Actions)
	{
		// Seeded with the row's existing pair, so a row this skips keeps what it has rather than being
		// cleared by a sync it was never part of.
		OutSync.Pairs.Emplace(Action.PrimaryIndex, Action.AlternateIndex);

		if (Action.Sequence.IsNull())
		{
			OutSync.CrowdOnlyActionNames.Add(Action.ActionName.ToString());
			continue;
		}

		UAnimSequence* Clip = Action.Sequence.LoadSynchronous();
		if (!Clip)
		{
			OutSync.UnloadableActionNames.Add(Action.ActionName.ToString());
			continue;
		}

		// The first TWO slots holding this clip. Two, because retriggering an action needs the pushed
		// index to change, so one slot cannot express it however many times the clip appears.
		int32 First = INDEX_NONE;
		int32 Second = INDEX_NONE;
		for (int32 SlotIndex = 0; SlotIndex < OutSync.Bank.Num(); ++SlotIndex)
		{
			if (OutSync.Bank[SlotIndex].Sequence != Clip || IsClaimedSlot(SlotIndex))
			{
				continue;
			}

			if (First == INDEX_NONE)
			{
				First = SlotIndex;
				continue;
			}

			Second = SlotIndex;
			break;
		}

		// APPEND, never insert or reorder. Every index already authored keeps meaning what it meant,
		// which is what makes this safe to run on a bank gaits and poses already resolve against.
		while (Second == INDEX_NONE)
		{
			FAnimSequenceTransformProviderSequence Appended;
			Appended.Sequence = Clip;
			const int32 AppendedIndex = OutSync.Bank.Add(Appended);
			++OutSync.AppendedSlots;

			// A gait or pose naming this clip resolves to the first slot holding it, even when this run is
			// what created that slot. Taking it here would hand the action the gait's own index.
			if (IsClaimedSlot(AppendedIndex))
			{
				continue;
			}

			if (First == INDEX_NONE)
			{
				First = AppendedIndex;
				continue;
			}

			Second = AppendedIndex;
		}

		// A row already naming these two slots the other way round is left exactly as it is. The pair is
		// a set of two slots to alternate between, so the order carries no meaning, and rewriting it
		// would dirty the asset and push an undo step for a press that changed nothing observable.
		const bool bAlreadySatisfied =
			(Action.PrimaryIndex == First && Action.AlternateIndex == Second) ||
			(Action.PrimaryIndex == Second && Action.AlternateIndex == First);
		if (bAlreadySatisfied)
		{
			continue;
		}

		OutSync.Pairs.Last() = TPair<int32, int32>(First, Second);
		OutSync.bPairsChanged = true;
		++OutSync.ChangedPairCount;
	}
}
#else
void UCrowdyMassAnimSet::SyncActionsIntoBank()
{
	UE_LOG(LogCrowdyMass, Warning, TEXT("CrowdyMassAnimSet %s: Sync Actions Into Bank only runs in the editor."), *GetName());
}
#endif
