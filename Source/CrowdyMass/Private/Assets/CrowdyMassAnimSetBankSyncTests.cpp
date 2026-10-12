#include "Assets/CrowdyMassAnimSet.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceTransformProviderData.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

// What Sync Actions Into Bank does to the two things it touches: the bank's contents and the slot pair on
// each action row. The cases here are the ones a bank can get wrong, which are all about POSITION rather
// than about content: a slot index means nothing except as a position in one particular array, so the
// interesting failures are appending where a row already had slots, disturbing indices other rows already
// resolve against, and running twice.
//
// Only the computation is exercised. Committing writes two assets and asks the engine to rebuild derived
// data for the bank, which is the part that needs an editor and a human, and is named as a gate rather
// than faked here.
static constexpr EAutomationTestFlags CrowdyMassAnimSetBankSyncTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassAnimSetBankSyncTestFixture
{
	static UAnimSequence* MakeSequence()
	{
		return NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	static UCrowdyMassAnimSet* MakeAnimSet()
	{
		return NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	static FCrowdyMassActionEntry MakeAction(const FName Name, UAnimSequence* Sequence,
		const int32 Primary = INDEX_NONE, const int32 Alternate = INDEX_NONE)
	{
		FCrowdyMassActionEntry Entry;
		Entry.ActionName = Name;
		Entry.Sequence = TSoftObjectPtr<UAnimSequence>(Sequence);
		Entry.PrimaryIndex = Primary;
		Entry.AlternateIndex = Alternate;
		return Entry;
	}

	static TArray<FAnimSequenceTransformProviderSequence> MakeBank(const TArray<UAnimSequence*>& Sequences)
	{
		TArray<FAnimSequenceTransformProviderSequence> Bank;
		for (UAnimSequence* Sequence : Sequences)
		{
			FAnimSequenceTransformProviderSequence Entry;
			Entry.Sequence = Sequence;
			Bank.Add(Entry);
		}

		return Bank;
	}

	/**
	 * A provider carrying these sequences.
	 *
	 * Written through reflection for the same reason the production sync is: the array is private with no
	 * setter because it is meant to be authored in a details panel. Deliberately the same route, so an
	 * engine change that moves it fails here as well rather than leaving the production path the only
	 * thing that breaks.
	 */
	static UAnimSequenceTransformProviderData* MakeProvider(const TArray<UAnimSequence*>& Sequences)
	{
		UAnimSequenceTransformProviderData* Provider =
			NewObject<UAnimSequenceTransformProviderData>(GetTransientPackage(), NAME_None, RF_Transient);

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(
			UAnimSequenceTransformProviderData::StaticClass(), TEXT("Sequences"));
		if (!ArrayProperty)
		{
			return Provider;
		}

		*ArrayProperty->ContainerPtrToValuePtr<TArray<FAnimSequenceTransformProviderSequence>>(Provider) =
			MakeBank(Sequences);
		return Provider;
	}
};

// The headline case: a row that names a clip and nothing else gets both slots it needs, and they are the
// two the row is then pointed at. An author types a name and a clip; the two integers stop existing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncAppendsPairTest,
	"CrowdySDK.Mass.AnimSetBankSyncAppendsAPairForAnUnbankedClip", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncAppendsPairTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(TArray<FAnimSequenceTransformProviderSequence>(), Sync);

	TestEqual(TEXT("a clip absent from the bank costs two slots, not one"), Sync.AppendedSlots, 2);

	// Bailed on rather than asserted past. Everything below indexes the bank, so a run that produced
	// fewer slots has to stop here and report it, not read off the end and take the process down.
	if (!TestEqual(TEXT("and the bank ends up holding exactly those two"), Sync.Bank.Num(), 2))
	{
		return false;
	}

	TestSamePtr(TEXT("the first appended slot holds the clip"),
		Sync.Bank[0].Sequence.Get(), Swing.Get());
	TestSamePtr(TEXT("and so does the second, which is what alternating needs"),
		Sync.Bank[1].Sequence.Get(), Swing.Get());

	TestEqual(TEXT("the row is pointed at the first slot"), Sync.Pairs[0].Key, 0);
	TestEqual(TEXT("and at the second, never at one index twice"), Sync.Pairs[0].Value, 1);
	TestTrue(TEXT("and the run reports that it changed the pair"), Sync.bPairsChanged);

	return true;
}

// A bank that already holds the clip once. Appending a second copy blindly would be wrong in the other
// direction too: it has to REUSE the existing slot, or every sync grows the bank by two forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncReusesExistingSlotTest,
	"CrowdySDK.Mass.AnimSetBankSyncReusesTheSlotsAClipAlreadyHas", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncReusesExistingSlotTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Idle(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));

	// Idle at slot 0 so the clip being synced is not the first thing in the bank, which is the case where
	// an off-by-one in the search reads as correct.
	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Fixture::MakeBank({Idle.Get(), Swing.Get()}), Sync);

	TestEqual(TEXT("a clip already in the bank once costs one more slot, not two"), Sync.AppendedSlots, 1);
	TestEqual(TEXT("the existing slot is reused rather than duplicated"), Sync.Pairs[0].Key, 1);
	TestEqual(TEXT("and the appended one becomes the alternate"), Sync.Pairs[0].Value, 2);
	TestSamePtr(TEXT("the slot another row resolves against is untouched"),
		Sync.Bank[0].Sequence.Get(), Idle.Get());

	return true;
}

// Running it twice must be a no-op, and this is the case that says so. A sync that appended every time
// would look correct on a first run and grow the bank without bound on an asset anyone edits twice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncIsIdempotentTest,
	"CrowdySDK.Mass.AnimSetBankSyncIsIdempotent", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncIsIdempotentTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));

	FCrowdyMassActionBankSync First;
	AnimSet->ComputeActionBankSync(TArray<FAnimSequenceTransformProviderSequence>(), First);

	// The commit, done by hand: this is exactly what the button writes back, so the second run below sees
	// the state a second button press would.
	AnimSet->Actions[0].PrimaryIndex = First.Pairs[0].Key;
	AnimSet->Actions[0].AlternateIndex = First.Pairs[0].Value;

	FCrowdyMassActionBankSync Second;
	AnimSet->ComputeActionBankSync(First.Bank, Second);

	TestEqual(TEXT("a second run appends nothing"), Second.AppendedSlots, 0);
	TestFalse(TEXT("and moves no pair"), Second.bPairsChanged);
	TestFalse(TEXT("so the button reports there was nothing to do"), Second.HasWork());
	TestEqual(TEXT("and the bank is the same length it was"), Second.Bank.Num(), First.Bank.Num());

	return true;
}

// The property that makes this safe to run at all. Gaits and poses resolve by asset, but they resolve to
// an INDEX, and everything downstream (the pose collision rules, every already-authored pair) is stated in
// those indices. A sync that inserted or reordered would silently repoint all of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncNeverMovesExistingSlotsTest,
	"CrowdySDK.Mass.AnimSetBankSyncOnlyEverAppends", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncNeverMovesExistingSlotsTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Idle(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Run(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Dead(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));

	const TArray<FAnimSequenceTransformProviderSequence> Original =
		Fixture::MakeBank({Idle.Get(), Run.Get(), Dead.Get()});

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Original, Sync);

	// Bailed on rather than asserted past, so a run that shortened the bank reports that instead of
	// reading off the end of it while checking that it did not.
	if (!TestEqual(TEXT("the bank grew by exactly the slots the new clip needed"),
		Sync.Bank.Num(), Original.Num() + 2))
	{
		return false;
	}

	for (int32 SlotIndex = 0; SlotIndex < Original.Num(); ++SlotIndex)
	{
		TestSamePtr(*FString::Printf(TEXT("slot %d still holds what it held before the sync"), SlotIndex),
			Sync.Bank[SlotIndex].Sequence.Get(), Original[SlotIndex].Sequence.Get());
	}

	TestTrue(TEXT("and the new pair sits entirely past the end of the original bank"),
		Sync.Pairs[0].Key >= Original.Num() && Sync.Pairs[0].Value >= Original.Num());

	return true;
}

// The two rows a sync must not touch, and it must not touch them for two different reasons. A row naming
// no clip is a legitimately authored crowd-only action; a row whose clip will not load is a fault. Both
// keep the pair they already have, and both are reported, separately.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncSkipsRowsWithNoClipTest,
	"CrowdySDK.Mass.AnimSetBankSyncLeavesCrowdOnlyRowsAlone", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncSkipsRowsWithNoClipTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

	// Hand-authored slots and no asset: the shape every action had before a Sequence column existed.
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("CrowdOnly"), nullptr, 7, 8));
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(TArray<FAnimSequenceTransformProviderSequence>(), Sync);

	TestEqual(TEXT("the crowd-only row keeps the primary slot it was authored with"), Sync.Pairs[0].Key, 7);
	TestEqual(TEXT("and its alternate"), Sync.Pairs[0].Value, 8);
	TestEqual(TEXT("it is named, so a remaining hand-typed index is not read as an oversight"),
		Sync.CrowdOnlyActionNames.Num(), 1);
	TestEqual(TEXT("and named as itself"), Sync.CrowdOnlyActionNames[0], FString(TEXT("CrowdOnly")));
	TestEqual(TEXT("a row naming no clip is not a load failure"), Sync.UnloadableActionNames.Num(), 0);

	// The row beside it still syncs, so one skipped row does not stop the pass.
	TestEqual(TEXT("the row that does name a clip is still given its slots"), Sync.AppendedSlots, 2);
	TestEqual(TEXT("starting at the front of an empty bank"), Sync.Pairs[1].Key, 0);

	return true;
}

// Two actions naming one clip share its pair rather than each getting their own. They ARE the same
// animation, so a second pair would spend two bank slots to show the identical thing, and retriggering
// still alternates within the shared pair. Pinned because it is a decision, not a consequence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncSharesOneClipsPairTest,
	"CrowdySDK.Mass.AnimSetBankSyncSharesOnePairBetweenActionsWithOneClip", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncSharesOneClipsPairTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Slash"), Swing.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(TArray<FAnimSequenceTransformProviderSequence>(), Sync);

	TestEqual(TEXT("one clip costs two slots however many rows name it"), Sync.AppendedSlots, 2);
	TestEqual(TEXT("both rows get the same primary"), Sync.Pairs[1].Key, Sync.Pairs[0].Key);
	TestEqual(TEXT("and the same alternate"), Sync.Pairs[1].Value, Sync.Pairs[0].Value);

	return true;
}

// The other half of the slice: with a bank named, asset validation raises what only a running crowd used
// to see. The pair here is out of range, which nothing without a bank can possibly know.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetValidatesAgainstBankTest,
	"CrowdySDK.Mass.AnimSetValidationChecksSlotPairsWhenABankIsNamed", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetValidatesAgainstBankTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());

	// Two slots holding one clip, which is what a valid pair looks like and therefore the only bank that
	// can tell a correct pair apart from a broken one here.
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> Provider(
		Fixture::MakeProvider({Swing.Get(), Swing.Get()}));

	{
		// No bank named: the pair cannot be checked, and that has to be said rather than passing quietly.
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
		AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get(), 40, 41));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("an unbanked set cannot call an out-of-range pair an error"),
			static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("but says exactly once that it could not check the pairs"),
			static_cast<int32>(Context.GetNumWarnings()), 1);
	}

	{
		// The same asset with the bank named. This is the whole point of the slice: the identical authored
		// data now fails validation instead of rendering wrongly later.
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
		AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get(), 40, 41));
		AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(Provider.Get());

		FDataValidationContext Context;
		TestTrue(TEXT("a pair pointing outside the bank now fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestTrue(TEXT("and is reported"), Context.GetNumErrors() > 0);
		TestEqual(TEXT("and the could-not-check warning is gone, because it could"),
			static_cast<int32>(Context.GetNumWarnings()), 0);
	}

	{
		// A set whose pair is correct against that same bank must stay silent, or the case above would
		// pass for an asset that simply always fails.
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
		AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get(), 0, 1));
		AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(Provider.Get());

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("a pair that resolves against the named bank raises nothing"),
			static_cast<int32>(Context.GetNumErrors()), 0);
	}

	return true;
}

// A row still at its authored defaults hits two rules that describe one mistake: the per-row identical
// pair check and the bank's own. An author who sees the same fault twice learns to skim the list, so the
// bank sink has to stay quiet about faults the row pass already named.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetValidationDoesNotDoubleReportTest,
	"CrowdySDK.Mass.AnimSetValidationReportsOneFaultOnce", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetValidationDoesNotDoubleReportTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> Provider(
		Fixture::MakeProvider({Swing.Get(), Swing.Get()}));

	// An unsynced row: both slots still INDEX_NONE, which is identical, which both rules catch.
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get()));
	AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(Provider.Get());

	FDataValidationContext Context;
	TestTrue(TEXT("an identical slot pair still fails validation"),
		AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
	TestEqual(TEXT("and is reported exactly once, not once per rule that can see it"),
		static_cast<int32>(Context.GetNumErrors()), 1);

	return true;
}

// A bank whose rows all name nothing is a fault of the BANK. Reporting it as every gait and pose failing
// to resolve sends the author to fix the anim set, which is the one asset here that is correct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetValidationNamesAnEmptyBankTest,
	"CrowdySDK.Mass.AnimSetValidationBlamesTheBankNotTheRows", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetValidationNamesAnEmptyBankTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	// Two slots that exist and hold nothing, which is why array length cannot answer this question.
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> Provider(
		Fixture::MakeProvider({nullptr, nullptr}));

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

	// Three rows that would each report a resolve failure if the empty bank were treated as usable, so
	// the count alone separates the two behaviours.
	FCrowdyMassGaitEntry Idle;
	Idle.GaitName = TEXT("Idle");
	Idle.Sequence = TSoftObjectPtr<UAnimSequence>(Fixture::MakeSequence());
	AnimSet->Gaits.Add(Idle);

	FCrowdyMassGaitEntry Run;
	Run.GaitName = TEXT("Run");
	Run.Sequence = TSoftObjectPtr<UAnimSequence>(Fixture::MakeSequence());
	Run.EnterSpeed = 300.0f;
	Run.ExitSpeed = 250.0f;
	AnimSet->Gaits.Add(Run);

	AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(Provider.Get());

	FDataValidationContext Context;
	TestTrue(TEXT("a bank holding no sequences fails validation"),
		AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
	TestEqual(TEXT("reported once against the bank, not once per row that could not resolve against it"),
		static_cast<int32>(Context.GetNumErrors()), 1);

	return true;
}

// The slot pair is a SET of two slots to alternate between, so the order carries no meaning. A row
// already naming both slots the other way round is finished, and rewriting it would dirty the asset and
// push an undo step for a press that changed nothing anyone can see.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncAcceptsASwappedPairTest,
	"CrowdySDK.Mass.AnimSetBankSyncLeavesASwappedPairAlone", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncAcceptsASwappedPairTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Swing.Get(), 1, 0));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Fixture::MakeBank({Swing.Get(), Swing.Get()}), Sync);

	TestEqual(TEXT("a swapped pair needs no new slots"), Sync.AppendedSlots, 0);
	TestFalse(TEXT("and is not treated as a pair to move"), Sync.bPairsChanged);
	TestFalse(TEXT("so the button reports nothing to do rather than dirtying the asset"), Sync.HasWork());
	TestEqual(TEXT("and the row keeps the order it was authored in"), Sync.Pairs[0].Key, 1);

	return true;
}

// The count in the success line is the rows the run MOVED. Counting the rows it was merely allowed to
// look at reads as though every correct row had been rewritten.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncCountsMovedRowsTest,
	"CrowdySDK.Mass.AnimSetBankSyncCountsOnlyTheRowsItMoved", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncCountsMovedRowsTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Swing(Fixture::MakeSequence());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

	// One row already correct against the bank below, one still unsynced.
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("AlreadyRight"), Swing.Get(), 0, 1));
	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Unsynced"), Swing.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Fixture::MakeBank({Swing.Get(), Swing.Get()}), Sync);

	TestEqual(TEXT("only the unsynced row counts as moved"), Sync.ChangedPairCount, 1);
	TestEqual(TEXT("and no slot was needed for either"), Sync.AppendedSlots, 0);
	TestEqual(TEXT("the correct row is left exactly as authored"), Sync.Pairs[0].Key, 0);
	TestEqual(TEXT("and the unsynced row lands on the same existing pair"), Sync.Pairs[1].Key, 0);

	return true;
}

// An action naming the clip a pose or a gait already renders is the one case where reusing an existing slot
// produces an asset that immediately fails validation: the pose is handed the index it already holds when
// the action ends, so it never changes on screen. The sync has to step over those slots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncAvoidsClaimedSlotsTest,
	"CrowdySDK.Mass.AnimSetBankSyncAvoidsSlotsAPoseOrGaitOwns", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncAvoidsClaimedSlotsTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Shared(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Idle(Fixture::MakeSequence());

	// Slot 0 is the gait's, slot 1 is the pose's, and slots 2 and 3 already hold the shared clip. Without
	// the claim the sync would hand the action slots 1 and 2, colliding with the pose.
	const TArray<FAnimSequenceTransformProviderSequence> Bank =
		Fixture::MakeBank({Idle.Get(), Shared.Get(), Shared.Get(), Shared.Get()});

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

	FCrowdyMassGaitEntry Gait;
	Gait.GaitName = TEXT("Idle");
	Gait.Sequence = Idle.Get();
	AnimSet->Gaits.Add(Gait);

	FCrowdyMassPoseEntry Pose;
	Pose.PoseName = TEXT("Dead");
	Pose.Sequence = Shared.Get();
	AnimSet->Poses.Add(Pose);

	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("Swing"), Shared.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Bank, Sync);

	if (!TestEqual(TEXT("the action still gets a pair"), Sync.Pairs.Num(), 1))
	{
		return false;
	}

	// The pose resolves to the FIRST slot holding its clip, which is slot 1.
	TestNotEqual(TEXT("the action's first slot is not the one the pose resolves to"), Sync.Pairs[0].Key, 1);
	TestNotEqual(TEXT("nor is its second"), Sync.Pairs[0].Value, 1);
	TestNotEqual(TEXT("and neither is the gait's slot"), Sync.Pairs[0].Key, 0);
	TestNotEqual(TEXT("on either half"), Sync.Pairs[0].Value, 0);

	// Slots 2 and 3 hold the clip and nothing claims them, so nothing needs appending: stepping over a
	// claimed slot must not become "append a new one every time".
	TestEqual(TEXT("and the two unclaimed slots holding that clip are reused rather than appended past"),
		Sync.AppendedSlots, 0);

	// Idempotence, which is the property a selection change is most likely to break.
	FCrowdyMassActionBankSync Second;
	AnimSet->Actions[0].PrimaryIndex = Sync.Pairs[0].Key;
	AnimSet->Actions[0].AlternateIndex = Sync.Pairs[0].Value;
	AnimSet->ComputeActionBankSync(Sync.Bank, Second);
	TestFalse(TEXT("running it again changes nothing"), Second.bPairsChanged);
	TestEqual(TEXT("and appends nothing"), Second.AppendedSlots, 0);

	return true;
}

// The harder half of the same rule. A gait whose clip is absent from the bank resolves to whatever this
// run appends for it, so claiming only the slots that exist when the run starts leaves the action taking
// the gait's slot: the collision is created by the sync itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankSyncAvoidsAppendedClaimTest,
	"CrowdySDK.Mass.AnimSetBankSyncAvoidsASlotItAppendsForAGait", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetBankSyncAvoidsAppendedClaimTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TStrongObjectPtr<UAnimSequence> Run(Fixture::MakeSequence());
	const TStrongObjectPtr<UAnimSequence> Shared(Fixture::MakeSequence());

	// The shared clip is in no slot yet, so every slot holding it after this run is one this run created.
	const TArray<FAnimSequenceTransformProviderSequence> Bank = Fixture::MakeBank({Run.Get()});

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

	FCrowdyMassGaitEntry Gait;
	Gait.GaitName = TEXT("Idle");
	Gait.Sequence = Shared.Get();
	AnimSet->Gaits.Add(Gait);

	AnimSet->Actions.Add(Fixture::MakeAction(TEXT("IdleEmote"), Shared.Get()));

	FCrowdyMassActionBankSync Sync;
	AnimSet->ComputeActionBankSync(Bank, Sync);

	if (!TestEqual(TEXT("the action gets a pair"), Sync.Pairs.Num(), 1))
	{
		return false;
	}

	// Where the gait will resolve once the bank is written back.
	int32 GaitSlot = INDEX_NONE;
	for (int32 SlotIndex = 0; SlotIndex < Sync.Bank.Num(); ++SlotIndex)
	{
		if (Sync.Bank[SlotIndex].Sequence == Shared.Get())
		{
			GaitSlot = SlotIndex;
			break;
		}
	}

	if (!TestNotEqual(TEXT("the gait's clip is in the bank afterwards"), GaitSlot, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	TestNotEqual(TEXT("and the action's first slot is not the one the gait resolves to"), Sync.Pairs[0].Key, GaitSlot);
	TestNotEqual(TEXT("nor its second"), Sync.Pairs[0].Value, GaitSlot);
	TestNotEqual(TEXT("and the two action slots differ from each other"), Sync.Pairs[0].Key, Sync.Pairs[0].Value);

	// Idempotence across the boundary this case is about: the second run sees the clip already banked.
	AnimSet->Actions[0].PrimaryIndex = Sync.Pairs[0].Key;
	AnimSet->Actions[0].AlternateIndex = Sync.Pairs[0].Value;

	FCrowdyMassActionBankSync Second;
	AnimSet->ComputeActionBankSync(Sync.Bank, Second);
	TestFalse(TEXT("running it again changes nothing"), Second.bPairsChanged);
	TestEqual(TEXT("and appends nothing"), Second.AppendedSlots, 0);

	return true;
}

// A gait or pose naming an asset that has been deleted is not the same as naming none, and every other
// check here reads IsNull, which a soft pointer to a missing asset is not. Left unchecked it validates
// green and silently falls back to the floor gait at runtime.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetValidationChecksSequencesLoadTest,
	"CrowdySDK.Mass.AnimSetValidationChecksAuthoredSequencesLoad", CrowdyMassAnimSetBankSyncTestFlags)
bool FCrowdyMassAnimSetValidationChecksSequencesLoadTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassAnimSetBankSyncTestFixture;

	const TSoftObjectPtr<UAnimSequence> Missing(
		FSoftObjectPath(TEXT("/Game/CrowdyMassTests/NoSuchSequence.NoSuchSequence")));
	TestFalse(TEXT("the fixture path is not null, which is the whole reason IsNull misses this"),
		Missing.IsNull());

	{
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

		FCrowdyMassGaitEntry Gait;
		Gait.GaitName = TEXT("Walk");
		Gait.Sequence = Missing;
		AnimSet->Gaits.Add(Gait);

		FDataValidationContext Context;
		TestTrue(TEXT("a gait naming a deleted sequence fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestTrue(TEXT("and is reported"), Context.GetNumErrors() > 0);
	}

	{
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

		FCrowdyMassPoseEntry Pose;
		Pose.PoseName = TEXT("Dead");
		Pose.Sequence = Missing;
		AnimSet->Poses.Add(Pose);

		FDataValidationContext Context;
		TestTrue(TEXT("and so does a pose naming one"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
	}

	{
		// The control. A gait naming a sequence that DOES load must not be caught by the same check, or the
		// two cases above would pass for an asset that always fails.
		const TStrongObjectPtr<UAnimSequence> Present(Fixture::MakeSequence());
		const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::MakeAnimSet());

		FCrowdyMassGaitEntry Gait;
		Gait.GaitName = TEXT("Walk");
		Gait.Sequence = Present.Get();
		AnimSet->Gaits.Add(Gait);

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("a gait naming a sequence that loads raises nothing"),
			static_cast<int32>(Context.GetNumErrors()), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
