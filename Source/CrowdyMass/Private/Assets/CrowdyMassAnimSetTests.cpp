#include "Assets/CrowdyMassAnimSet.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequence.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	constexpr EAutomationTestFlags CrowdyMassAnimSetTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A set with pose rows and no state enum is itself an IsDataValid error: a replicated state value is
	// the only thing that selects a pose row on a crowd entity, so those rows are unreachable. Cases below
	// that deliberately author no enum therefore carry this one error BEFORE the fault they are named for,
	// and they say so by adding this rather than by quietly moving their expected number, so that a case
	// about row counts stays a case about row counts.
	constexpr int32 ErrorsFromPoseRowsWithNoStateEnum = 1;

	// A plain transient sequence with no skeleton. Good enough for these tests: resolution only
	// compares asset pointers and reads playback duration, it never evaluates the animation itself.
	UAnimSequence* NewTestSequence()
	{
		return NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	// UAnimSequence only exposes duration through GetPlayLength(), which reads the deprecated
	// SequenceLength member; setting it through the editor-only AnimDataController would pull in a
	// module this test has no reason to depend on. Reflection sidesteps the deprecation and gives
	// each fixture an exact, known duration to assert against.
	void SetTestSequenceLength(UAnimSequence* Sequence, float LengthSeconds)
	{
		FProperty* Property = UAnimSequenceBase::StaticClass()->FindPropertyByName(TEXT("SequenceLength"));
		FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property);
		check(FloatProperty);
		FloatProperty->SetPropertyValue_InContainer(Sequence, LengthSeconds);
	}

	// The gait ids the shared fixture's rows take. A gait id is one-based over the authored rows, so
	// these are positions in the Gaits array plus one, not values of any enum: the fixture authors its
	// four rows slowest first, which is the order gait selection depends on.
	constexpr uint8 IdleGaitId = 1;
	constexpr uint8 WalkGaitId = 2;
	constexpr uint8 JogGaitId = 3;
	constexpr uint8 RunGaitId = 4;

	// A byte-valued enum built at runtime, so the tests can name their own states rather than borrowing
	// one of the plugin's. Entries are prefixed with the object's own unique name, which is what
	// UEnum's short-name lookup rebuilds when it resolves a row's PoseName.
	UEnum* MakeTestStateEnum(const TCHAR* BaseName, const TArray<TPair<FName, int64>>& Entries,
		UEnum::EUnderlyingType UnderlyingType = UEnum::EUnderlyingType::uint8)
	{
		const FName EnumName = MakeUniqueObjectName(GetTransientPackage(), UEnum::StaticClass(), FName(BaseName));

		TArray<TPair<FName, int64>> QualifiedNames;
		QualifiedNames.Reserve(Entries.Num());
		for (const TPair<FName, int64>& Entry : Entries)
		{
			QualifiedNames.Emplace(
				FName(*FString::Printf(TEXT("%s::%s"), *EnumName.ToString(), *Entry.Key.ToString())), Entry.Value);
		}

		UEnum* Enum = NewObject<UEnum>(GetTransientPackage(), EnumName, RF_Transient);
		Enum->SetEnums(QualifiedNames, UEnum::ECppForm::EnumClass, UnderlyingType, EEnumFlags::None,
			UEnum::EAddMaxKeyIfMissing::Yes);
		return Enum;
	}

	FCrowdyMassGaitEntry MakeGaitEntry(FName GaitName, UAnimSequence* Sequence)
	{
		FCrowdyMassGaitEntry Entry;
		Entry.GaitName = GaitName;
		Entry.Sequence = TSoftObjectPtr<UAnimSequence>(Sequence);
		return Entry;
	}

	FCrowdyMassGaitEntry MakeGaitEntry(FName GaitName, UAnimSequence* Sequence, float EnterSpeed, float ExitSpeed, float PlayRate = 1.0f)
	{
		FCrowdyMassGaitEntry Entry = MakeGaitEntry(GaitName, Sequence);
		Entry.EnterSpeed = EnterSpeed;
		Entry.ExitSpeed = ExitSpeed;
		Entry.PlayRate = PlayRate;
		return Entry;
	}

	// Idle / walk / jog / run in one bank, with the speed thresholds the gait tests share. The bands
	// deliberately overlap: walk is entered at 100 and left at 60, so 60 to 100 is a speed at which
	// the answer depends on which gait the entity is already in.
	struct FGaitFixture
	{
		UAnimSequence* Sequences[4];
		TArray<UAnimSequence*> Bank;
		UCrowdyMassAnimSet* AnimSet = nullptr;
		FCrowdyMassResolvedAnimSet Resolved;
	};

	FGaitFixture MakeGaitFixture()
	{
		FGaitFixture Fixture;
		for (int32 GaitIndex = 0; GaitIndex < UE_ARRAY_COUNT(Fixture.Sequences); ++GaitIndex)
		{
			Fixture.Sequences[GaitIndex] = NewTestSequence();
		}

		// Bank order deliberately does not match gait order, so a resolver that assumed the two lined
		// up would be caught here as well as in the resolution test.
		Fixture.Bank = {
			Fixture.Sequences[RunGaitId - 1],
			Fixture.Sequences[IdleGaitId - 1],
			Fixture.Sequences[JogGaitId - 1],
			Fixture.Sequences[WalkGaitId - 1]};

		Fixture.AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		Fixture.AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), Fixture.Sequences[0], 0.0f, 0.0f));
		Fixture.AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), Fixture.Sequences[1], 100.0f, 60.0f, 1.1f));
		Fixture.AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), Fixture.Sequences[2], 300.0f, 250.0f, 1.2f));
		Fixture.AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), Fixture.Sequences[3], 500.0f, 430.0f, 1.3f));

		Fixture.Resolved.Build(Fixture.AnimSet, Fixture.Bank);
		return Fixture;
	}

	// The bank index a gait id resolves to, or INDEX_NONE for an id that names no row or a row whose
	// sequence is absent from the bank. The table answers with a pointer and every assertion below is
	// about the index, so unwrapping it once here keeps the tests reading as index comparisons.
	int32 GaitBankIndex(const FCrowdyMassResolvedAnimSet& Resolved, uint8 GaitId)
	{
		const FCrowdyMassResolvedGait* Gait = Resolved.FindResolvedGait(GaitId);
		return Gait ? Gait->BankIndex : INDEX_NONE;
	}

	float GaitPlayRate(const FCrowdyMassResolvedAnimSet& Resolved, uint8 GaitId)
	{
		const FCrowdyMassResolvedGait* Gait = Resolved.FindResolvedGait(GaitId);
		return Gait ? Gait->PlayRate : 1.0f;
	}

	FCrowdyMassActionEntry MakeActionEntry(FName ActionName, int32 PrimaryIndex, int32 AlternateIndex)
	{
		FCrowdyMassActionEntry Entry;
		Entry.ActionName = ActionName;
		Entry.PrimaryIndex = PrimaryIndex;
		Entry.AlternateIndex = AlternateIndex;
		return Entry;
	}

	FCrowdyMassPoseEntry MakePoseEntry(FName PoseName, UAnimSequence* Sequence,
		ECrowdyMassPoseRank Rank = ECrowdyMassPoseRank::BelowActions, bool bHoldLastFrame = false)
	{
		FCrowdyMassPoseEntry Entry;
		Entry.PoseName = PoseName;
		Entry.Sequence = TSoftObjectPtr<UAnimSequence>(Sequence);
		Entry.Rank = Rank;
		Entry.bHoldLastFrame = bHoldLastFrame;
		return Entry;
	}

	// The bank index a pose id resolves to, or INDEX_NONE for an id that names no pose or a pose whose
	// sequence is absent from the bank. The table answers with a pointer, and every assertion below is
	// about the index, so unwrapping it once here keeps the tests reading as index comparisons.
	int32 PoseBankIndex(const FCrowdyMassResolvedAnimSet& Resolved, uint8 PoseId)
	{
		const FCrowdyMassResolvedPose* Pose = Resolved.FindResolvedPose(PoseId);
		return Pose ? Pose->BankIndex : INDEX_NONE;
	}
}

// Each gait resolves by asset pointer to its actual position in a bank whose order deliberately
// does not match authoring order, so a resolver that assumed index parity with the Gaits array
// (instead of searching the bank) would be caught.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetIndexResolutionTest,
	"CrowdySDK.Mass.AnimSetIndexResolution", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetIndexResolutionTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* WalkSequence = NewTestSequence();
	UAnimSequence* JogSequence = NewTestSequence();
	UAnimSequence* RunSequence = NewTestSequence();
	SetTestSequenceLength(IdleSequence, 1.0f);
	SetTestSequenceLength(WalkSequence, 1.2f);
	SetTestSequenceLength(JogSequence, 0.8f);
	SetTestSequenceLength(RunSequence, 0.6f);

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), WalkSequence));
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), JogSequence));
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), RunSequence));

	// Bank order intentionally scrambled relative to authoring order.
	const TArray<UAnimSequence*> Bank = {WalkSequence, RunSequence, IdleSequence, JogSequence};

	const FCrowdyMassResolvedSequence IdleResult = AnimSet->ResolveGait(IdleGaitId, Bank);
	TestTrue(TEXT("idle resolved"), IdleResult.bResolved);
	TestEqual(TEXT("idle bank index"), IdleResult.BankIndex, 2);
	TestEqual(TEXT("idle duration"), IdleResult.DurationSeconds, 1.0f);

	const FCrowdyMassResolvedSequence WalkResult = AnimSet->ResolveGait(WalkGaitId, Bank);
	TestTrue(TEXT("walk resolved"), WalkResult.bResolved);
	TestEqual(TEXT("walk bank index"), WalkResult.BankIndex, 0);
	TestEqual(TEXT("walk duration"), WalkResult.DurationSeconds, 1.2f);

	const FCrowdyMassResolvedSequence JogResult = AnimSet->ResolveGait(JogGaitId, Bank);
	TestTrue(TEXT("jog resolved"), JogResult.bResolved);
	TestEqual(TEXT("jog bank index"), JogResult.BankIndex, 3);
	TestEqual(TEXT("jog duration"), JogResult.DurationSeconds, 0.8f);

	const FCrowdyMassResolvedSequence RunResult = AnimSet->ResolveGait(RunGaitId, Bank);
	TestTrue(TEXT("run resolved"), RunResult.bResolved);
	TestEqual(TEXT("run bank index"), RunResult.BankIndex, 1);
	TestEqual(TEXT("run duration"), RunResult.DurationSeconds, 0.6f);

	// Gait id 0 names no row, matching the pose and action id conventions, and must not be read as the
	// first row: an entity that has never been given a gait would otherwise be told it is idling.
	const FCrowdyMassResolvedSequence NoGaitResult = AnimSet->ResolveGait(0, Bank);
	TestFalse(TEXT("gait id 0 names no row"), NoGaitResult.bResolved);
	TestFalse(TEXT("and is not reported as a fallback either"), NoGaitResult.bFellBackToFloorGait);

	return true;
}

// An action's PrimaryIndex and AlternateIndex are distinct bank slots that must reference the same
// underlying asset (the retrigger mechanism depends on the engine seeing the pushed index change
// even though it plays the identical sequence). A pair whose two indices resolve to different assets
// must be rejected, a pair whose indices are identical must be rejected even though it trivially
// resolves to "the same asset", and an unknown action name must fail closed. ValidateAction is a
// pure lookup and does not log; ValidateAgainstBank is the logging entry point, covered separately.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetSlotPairsTest,
	"CrowdySDK.Mass.AnimSetSlotPairs", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetSlotPairsTest::RunTest(const FString& Parameters)
{
	UAnimSequence* AttackSequence = NewTestSequence();
	UAnimSequence* OtherSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Attack"), 0, 1));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Broken"), 0, 2));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Same"), 1, 1));

	// Slots 0 and 1 both hold the same asset (a valid retrigger pair); slot 2 holds a different asset.
	const TArray<UAnimSequence*> Bank = {AttackSequence, AttackSequence, OtherSequence};

	TestTrue(TEXT("distinct indices onto the same asset validate"), AnimSet->ValidateAction(TEXT("Attack"), Bank));
	TestFalse(TEXT("indices onto different assets are rejected"), AnimSet->ValidateAction(TEXT("Broken"), Bank));
	TestFalse(TEXT("identical indices are rejected even though they resolve to the same asset"),
		AnimSet->ValidateAction(TEXT("Same"), Bank));

	// An action name that was never authored has no entry to validate and must not log.
	TestFalse(TEXT("unknown action name fails closed"), AnimSet->ValidateAction(TEXT("Unknown"), Bank));

	return true;
}

// ValidateAgainstBank is the one entry point in this asset that logs. It must give the same-index
// and different-asset action failures distinct diagnoses, must warn on an unresolved gait and an
// unresolved pose, must skip the NAME_None pose sentinel rather than warning on it, and must report
// false as soon as anything above fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetValidateAgainstBankTest,
	"CrowdySDK.Mass.AnimSetValidateAgainstBank", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetValidateAgainstBankTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* OrphanSequence = NewTestSequence();
	UAnimSequence* OtherSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	// Authored, but its asset is absent from the bank passed to ValidateAgainstBank below.
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), OrphanSequence));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Same"), 0, 0));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Broken"), 0, 1));
	// NAME_None is the documented sentinel for "no pose authored": ValidateAgainstBank must skip it,
	// not report it as an unresolved lookup.
	AnimSet->Poses.Add(MakePoseEntry(NAME_None, IdleSequence));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Missing"), OrphanSequence));

	// Bank[0] and Bank[1] are different assets, so the "Broken" pair fails on asset identity, not index.
	const TArray<UAnimSequence*> Bank = {IdleSequence, OtherSequence};

	// Message text carries parentheses ("(%d, %d)"), so match on the parenthesis-free tail of it.
	AddExpectedErrorPlain(TEXT("gait Walk did not resolve against the supplied animation bank"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(
		TEXT("uses the same index for both slots, retriggering it will not restart playback because the engine only reacts to an index change"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("does not reference the same bank asset, retriggering it will not restart playback"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("pose Missing did not resolve against the supplied animation bank"),
		EAutomationExpectedErrorFlags::Contains, 1);

	const bool bValid = AnimSet->ValidateAgainstBank(Bank);
	TestFalse(TEXT("a bank with unresolved and malformed entries reports invalid"), bValid);

	return true;
}

// A gait that resolves to nothing against the supplied bank falls back to the first row's resolved
// value, and the fallback is flagged so a caller can tell "this genuinely resolved" apart from "this is
// the floor row standing in for something missing". ResolveGait is a pure lookup: it does not log, and
// repeated calls against the same unresolved gait are deterministic (no dedup state to go stale).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetUnresolvedFallsBackToIdleTest,
	"CrowdySDK.Mass.AnimSetUnresolvedFallsBackToIdle", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetUnresolvedFallsBackToIdleTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* OrphanSequence = NewTestSequence();
	SetTestSequenceLength(IdleSequence, 1.5f);

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	// Authored, but its asset is not present in the bank the resolver is given.
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), OrphanSequence));

	const TArray<UAnimSequence*> Bank = {IdleSequence};

	const FCrowdyMassResolvedSequence IdleResult = AnimSet->ResolveGait(IdleGaitId, Bank);
	TestTrue(TEXT("idle resolves directly"), IdleResult.bResolved);
	TestFalse(TEXT("a direct resolve is not flagged as a fallback"), IdleResult.bFellBackToFloorGait);

	const FCrowdyMassResolvedSequence First = AnimSet->ResolveGait(WalkGaitId, Bank);
	TestTrue(TEXT("first call falls back to a resolved idle"), First.bResolved);
	TestTrue(TEXT("first call is flagged as a fallback, not a genuine resolve"), First.bFellBackToFloorGait);
	TestEqual(TEXT("first call bank index is idle's"), First.BankIndex, 0);
	TestEqual(TEXT("first call duration is idle's"), First.DurationSeconds, 1.5f);

	// Querying the same unresolved gait again must give the same answer: there is no dedup state left
	// to go stale between calls.
	const FCrowdyMassResolvedSequence Second = AnimSet->ResolveGait(WalkGaitId, Bank);
	TestTrue(TEXT("second call still falls back to a resolved idle"), Second.bResolved);
	TestTrue(TEXT("second call is still flagged as a fallback"), Second.bFellBackToFloorGait);
	TestEqual(TEXT("second call bank index is idle's"), Second.BankIndex, 0);
	TestEqual(TEXT("second call duration is idle's"), Second.DurationSeconds, 1.5f);

	// A row id past the authored array is an unknown row, not the sentinel, so it falls back the same
	// way an unresolved one does rather than answering nothing at all.
	const FCrowdyMassResolvedSequence PastTheEnd = AnimSet->ResolveGait(99, Bank);
	TestTrue(TEXT("an id past the authored rows falls back to the floor row"), PastTheEnd.bResolved);
	TestTrue(TEXT("and says so"), PastTheEnd.bFellBackToFloorGait);

	return true;
}

// ResolvePose: an authored pose resolves cleanly (not flagged as a fallback), an unknown pose falls
// back to Idle (flagged), and NAME_None - the documented "no pose authored" sentinel, and also the
// default key of a blank details-panel row - returns unresolved immediately rather than silently
// matching whichever row was left blank or falling back to Idle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetResolvePoseTest,
	"CrowdySDK.Mass.AnimSetResolvePose", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetResolvePoseTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* SitSequence = NewTestSequence();
	SetTestSequenceLength(IdleSequence, 1.0f);
	SetTestSequenceLength(SitSequence, 3.0f);

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));

	const TArray<UAnimSequence*> Bank = {IdleSequence, SitSequence};

	const FCrowdyMassResolvedSequence SitResult = AnimSet->ResolvePose(TEXT("Sit"), Bank);
	TestTrue(TEXT("an authored state resolves"), SitResult.bResolved);
	TestFalse(TEXT("a genuine resolve is not flagged as a fallback"), SitResult.bFellBackToFloorGait);
	TestEqual(TEXT("resolved bank index"), SitResult.BankIndex, 1);
	TestEqual(TEXT("resolved duration"), SitResult.DurationSeconds, 3.0f);

	const FCrowdyMassResolvedSequence UnknownResult = AnimSet->ResolvePose(TEXT("Channeling"), Bank);
	TestTrue(TEXT("an unknown state falls back to a resolved idle"), UnknownResult.bResolved);
	TestTrue(TEXT("unknown-state fallback is flagged"), UnknownResult.bFellBackToFloorGait);
	TestEqual(TEXT("unknown-state fallback bank index is idle's"), UnknownResult.BankIndex, 0);

	const FCrowdyMassResolvedSequence NoneResult = AnimSet->ResolvePose(NAME_None, Bank);
	TestFalse(TEXT("NAME_None is the no-pose sentinel, not a lookup, and must not resolve"),
		NoneResult.bResolved);
	TestFalse(TEXT("NAME_None must not be reported as a fallback either"), NoneResult.bFellBackToFloorGait);
	TestEqual(TEXT("NAME_None leaves the bank index unresolved"), NoneResult.BankIndex, INDEX_NONE);

	return true;
}

// The floor-row fallback in ResolveGait terminates on the first row itself instead of recursing forever
// when that row cannot resolve either. Covers an anim set with no Gaits authored at all, an anim set
// whose first row's sequence is absent from the bank, and an empty bank.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetIdleFallbackTerminatesTest,
	"CrowdySDK.Mass.AnimSetIdleFallbackTerminates", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetIdleFallbackTerminatesTest::RunTest(const FString& Parameters)
{
	{
		// Nothing authored at all: the floor row has no entry to resolve, so the fallback a faster row
		// takes must terminate on it rather than recurse into resolving its own fallback.
		UCrowdyMassAnimSet* EmptyAnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		const TArray<UAnimSequence*> Bank = {NewTestSequence()};

		const FCrowdyMassResolvedSequence Result = EmptyAnimSet->ResolveGait(RunGaitId, Bank);
		TestFalse(TEXT("run cannot resolve when nothing is authored and the floor row does not resolve either"),
			Result.bResolved);
		TestEqual(TEXT("unresolved result carries no bank index"), Result.BankIndex, INDEX_NONE);
	}

	{
		// The floor row is authored, but neither its sequence nor the faster row's is in an empty bank.
		UAnimSequence* OrphanIdleSequence = NewTestSequence();
		UAnimSequence* JogSequence = NewTestSequence();

		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), OrphanIdleSequence));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), JogSequence));

		const TArray<UAnimSequence*> EmptyBank = {};

		const FCrowdyMassResolvedSequence Result = AnimSet->ResolveGait(WalkGaitId, EmptyBank);
		TestFalse(TEXT("a faster row cannot resolve when the bank is empty and the floor row's own sequence is absent from it too"),
			Result.bResolved);
		TestEqual(TEXT("unresolved result carries no bank index"), Result.BankIndex, INDEX_NONE);
	}

	return true;
}

// GetActionDurationSeconds reports the authored duration for a valid action, zero for an action name
// that was never authored, and zero (not a crash, not a stale value) for an authored action whose
// PrimaryIndex does not resolve against the supplied bank.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetDurationValidationTest,
	"CrowdySDK.Mass.AnimSetDurationValidation", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetDurationValidationTest::RunTest(const FString& Parameters)
{
	UAnimSequence* HeavySequence = NewTestSequence();
	UAnimSequence* LightSequence = NewTestSequence();
	SetTestSequenceLength(HeavySequence, 2.5f);
	SetTestSequenceLength(LightSequence, 0.75f);

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Heavy"), 0, 1));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Light"), 2, 3));
	// Authored, but PrimaryIndex is out of range for the bank below.
	AnimSet->Actions.Add(MakeActionEntry(TEXT("OutOfRange"), 99, 99));

	const TArray<UAnimSequence*> Bank = {HeavySequence, HeavySequence, LightSequence, LightSequence};

	TestEqual(TEXT("heavy duration matches its authored sequence"),
		AnimSet->GetActionDurationSeconds(TEXT("Heavy"), Bank), 2.5f);
	TestEqual(TEXT("light duration matches its authored sequence"),
		AnimSet->GetActionDurationSeconds(TEXT("Light"), Bank), 0.75f);
	TestEqual(TEXT("unauthored action reports zero duration"),
		AnimSet->GetActionDurationSeconds(TEXT("Unknown"), Bank), 0.0f);
	TestEqual(TEXT("out-of-range primary index reports zero, not a stale value"),
		AnimSet->GetActionDurationSeconds(TEXT("OutOfRange"), Bank), 0.0f);

	return true;
}

// Resolution against a bank happens once and is then read per entity per frame, so what the table
// holds is what selection can ever see. A gait whose sequence is missing from the bank must come out
// as unresolved rather than as Idle's index: recording the fallback here would leave every entity
// silently told it is walking while playing an idle, with nothing left able to notice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassResolvedAnimSetBuildTest,
	"CrowdySDK.Mass.ResolvedAnimSetBuild", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassResolvedAnimSetBuildTest::RunTest(const FString& Parameters)
{
	const FGaitFixture Fixture = MakeGaitFixture();

	TestTrue(TEXT("a built table reports itself built"), Fixture.Resolved.bBuilt);
	TestEqual(TEXT("bank size recorded"), Fixture.Resolved.BankSize, 4);
	TestEqual(TEXT("one resolved row per authored gait"), Fixture.Resolved.ResolvedGaits.Num(), 4);
	TestEqual(TEXT("idle index"), GaitBankIndex(Fixture.Resolved, IdleGaitId), 1);
	TestEqual(TEXT("walk index"), GaitBankIndex(Fixture.Resolved, WalkGaitId), 3);
	TestEqual(TEXT("jog index"), GaitBankIndex(Fixture.Resolved, JogGaitId), 2);
	TestEqual(TEXT("run index"), GaitBankIndex(Fixture.Resolved, RunGaitId), 0);
	TestEqual(TEXT("walk play rate"), GaitPlayRate(Fixture.Resolved, WalkGaitId), 1.1f);
	TestEqual(TEXT("run play rate"), GaitPlayRate(Fixture.Resolved, RunGaitId), 1.3f);
	TestNull(TEXT("gait id 0 names no row"), Fixture.Resolved.FindResolvedGait(0));
	TestNull(TEXT("nor does an id past the authored rows"), Fixture.Resolved.FindResolvedGait(5));

	{
		UAnimSequence* IdleSequence = NewTestSequence();
		UAnimSequence* OrphanSequence = NewTestSequence();
		UAnimSequence* SitSequence = NewTestSequence();

		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence, 0.0f, 0.0f));
		// Authored, but its sequence is absent from the bank below.
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), OrphanSequence, 100.0f, 60.0f));
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Missing"), OrphanSequence));

		const TArray<UAnimSequence*> Bank = {IdleSequence, SitSequence};

		// Build is a pure lookup and does not log; ValidateAgainstBank is the entry point that reports
		// these same failures, and it is covered separately.
		FCrowdyMassResolvedAnimSet Resolved;
		Resolved.Build(AnimSet, Bank);

		// The row is still present at its own id, so every row authored after a broken one keeps its id.
		TestEqual(TEXT("a broken row is still an addressable id"), Resolved.ResolvedGaits.Num(), 2);
		TestEqual(TEXT("a gait whose sequence is absent resolves to nothing, not to idle's index"),
			GaitBankIndex(Resolved, WalkGaitId), INDEX_NONE);
		TestEqual(TEXT("idle still resolves"), GaitBankIndex(Resolved, IdleGaitId), 0);
		TestEqual(TEXT("an authored pose resolves"),
			PoseBankIndex(Resolved, static_cast<uint8>(AnimSet->FindPoseId(TEXT("Sit")))), 1);
		TestEqual(TEXT("a pose whose sequence is absent resolves to nothing, not to idle's index"),
			PoseBankIndex(Resolved, static_cast<uint8>(AnimSet->FindPoseId(TEXT("Missing")))), INDEX_NONE);
		TestEqual(TEXT("an unauthored pose name resolves to nothing"),
			PoseBankIndex(Resolved, static_cast<uint8>(AnimSet->FindPoseId(TEXT("Channeling")))), INDEX_NONE);
		TestEqual(TEXT("the no-pose sentinel is not a lookup"),
			PoseBankIndex(Resolved, static_cast<uint8>(AnimSet->FindPoseId(NAME_None))), INDEX_NONE);

		// A gait with no resolved sequence must be unreachable rather than free to enter, and must be
		// left the moment it somehow becomes current: an entity stuck in a gait it has no animation for
		// would hold whatever pose it happened to be in.
		TestEqual(TEXT("an unresolved gait cannot be entered at any speed"),
			static_cast<int32>(Resolved.SelectGait(IdleGaitId, 100000.0f)), static_cast<int32>(IdleGaitId));
		TestEqual(TEXT("an unresolved gait is left immediately if it is somehow current"),
			static_cast<int32>(Resolved.SelectGait(WalkGaitId, 100000.0f)), static_cast<int32>(IdleGaitId));
	}

	{
		// A trait with no anim set assigned must produce a table that selects nothing, rather than one
		// that needs a null check at every call site.
		FCrowdyMassResolvedAnimSet Resolved;
		Resolved.Build(nullptr, TArray<UAnimSequence*>());

		TestTrue(TEXT("a table built from no anim set reports itself built"), Resolved.bBuilt);
		TestEqual(TEXT("it authors no gait row"), Resolved.ResolvedGaits.Num(), 0);
		TestEqual(TEXT("no gait resolves"), GaitBankIndex(Resolved, RunGaitId), INDEX_NONE);
		// 0 rather than the floor row: answering an id no row backs would hand the compose rule an id it
		// cannot resolve and leave the entity looking like it is idling.
		TestEqual(TEXT("selection reports no gait at all rather than an id nothing backs"),
			static_cast<int32>(Resolved.SelectGait(IdleGaitId, 9999.0f)), 0);
		TestEqual(TEXT("and says the same for an entity that has never had one"),
			static_cast<int32>(Resolved.SelectGait(0, 0.0f)), 0);
	}

	{
		// An entity that has never been given a gait has to reach one on its first pass, or a crowd that
		// spawns standing still never animates at all.
		const FGaitFixture Fresh = MakeGaitFixture();

		TestEqual(TEXT("an entity with no gait yet starts on the floor row"),
			static_cast<int32>(Fresh.Resolved.SelectGait(0, 0.0f)), static_cast<int32>(IdleGaitId));
		TestEqual(TEXT("and one that spawns already moving reaches the row its speed calls for"),
			static_cast<int32>(Fresh.Resolved.SelectGait(0, 520.0f)), static_cast<int32>(RunGaitId));
	}

	return true;
}

// The hysteresis band is the gap between a gait's enter and exit speed. Inside it the answer depends
// on which gait the entity is already in, which is the whole point: an entity holding a speed on a
// threshold must keep the gait it has rather than swapping animation every frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGaitSelectionHysteresisTest,
	"CrowdySDK.Mass.GaitSelectionHysteresis", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassGaitSelectionHysteresisTest::RunTest(const FString& Parameters)
{
	const FGaitFixture Fixture = MakeGaitFixture();
	const FCrowdyMassResolvedAnimSet& Resolved = Fixture.Resolved;

	TestEqual(TEXT("below walk's enter speed an idle entity stays idle"),
		static_cast<int32>(Resolved.SelectGait(IdleGaitId, 99.0f)), static_cast<int32>(IdleGaitId));
	TestEqual(TEXT("at walk's enter speed an idle entity walks"),
		static_cast<int32>(Resolved.SelectGait(IdleGaitId, 100.0f)), static_cast<int32>(WalkGaitId));

	// 70 is inside walk's band: above its exit speed, below its enter speed. The same speed gives two
	// different answers depending on the gait held, and that is correct.
	TestEqual(TEXT("inside the band a walking entity keeps walking"),
		static_cast<int32>(Resolved.SelectGait(WalkGaitId, 70.0f)), static_cast<int32>(WalkGaitId));
	TestEqual(TEXT("inside the band an idle entity stays idle"),
		static_cast<int32>(Resolved.SelectGait(IdleGaitId, 70.0f)), static_cast<int32>(IdleGaitId));
	TestEqual(TEXT("below walk's exit speed a walking entity stops"),
		static_cast<int32>(Resolved.SelectGait(WalkGaitId, 59.0f)), static_cast<int32>(IdleGaitId));

	// An entity that accelerates hard between two frames arrives at the gait its speed calls for,
	// rather than climbing one gait per frame and showing three animations on the way.
	TestEqual(TEXT("a large jump reaches the right gait in one step"),
		static_cast<int32>(Resolved.SelectGait(IdleGaitId, 520.0f)), static_cast<int32>(RunGaitId));
	// Slowing down is deliberately one gait at a time, so a sprint that ends abruptly still passes
	// through the gaits between rather than cutting from a run pose to an idle pose.
	TestEqual(TEXT("a large drop steps down one gait"),
		static_cast<int32>(Resolved.SelectGait(RunGaitId, 420.0f)), static_cast<int32>(JogGaitId));
	TestEqual(TEXT("a stop from a run reaches idle by stepping through every gait below its exit speed"),
		static_cast<int32>(Resolved.SelectGait(RunGaitId, 0.0f)), static_cast<int32>(IdleGaitId));
	TestEqual(TEXT("inside run's band a running entity keeps running"),
		static_cast<int32>(Resolved.SelectGait(RunGaitId, 440.0f)), static_cast<int32>(RunGaitId));

	// Shimmer: a speed oscillating across walk's enter speed must produce exactly one change of gait,
	// not one per frame. A selector with no hysteresis would report one change per sample here.
	const float ShimmerSpeeds[] = {95.0f, 105.0f, 95.0f, 105.0f, 95.0f, 105.0f, 95.0f, 98.0f};
	uint8 Gait = IdleGaitId;
	int32 GaitChanges = 0;
	for (const float Speed : ShimmerSpeeds)
	{
		const uint8 NextGait = Resolved.SelectGait(Gait, Speed);
		GaitChanges += NextGait != Gait ? 1 : 0;
		Gait = NextGait;
	}

	TestEqual(TEXT("a speed oscillating across a threshold changes gait once, not once per frame"), GaitChanges, 1);
	TestEqual(TEXT("and settles on the gait it entered"), static_cast<int32>(Gait), static_cast<int32>(WalkGaitId));

	// A fifth row is expressible now, and a set is not limited to the four this plugin used to name. The
	// same hysteresis has to hold across it, which is what says selection walks the authored rows rather
	// than a fixed count.
	{
		FGaitFixture Extended = MakeGaitFixture();
		UAnimSequence* SprintSequence = NewTestSequence();
		Extended.Bank.Add(SprintSequence);
		Extended.AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Sprint"), SprintSequence, 800.0f, 700.0f, 1.5f));
		Extended.Resolved.Build(Extended.AnimSet, Extended.Bank);

		constexpr uint8 SprintGaitId = 5;

		TestEqual(TEXT("a fifth authored gait is reachable"),
			static_cast<int32>(Extended.Resolved.SelectGait(RunGaitId, 850.0f)), static_cast<int32>(SprintGaitId));
		TestEqual(TEXT("and holds inside its own band"),
			static_cast<int32>(Extended.Resolved.SelectGait(SprintGaitId, 750.0f)), static_cast<int32>(SprintGaitId));
		TestEqual(TEXT("and drops one row when it is left"),
			static_cast<int32>(Extended.Resolved.SelectGait(SprintGaitId, 690.0f)), static_cast<int32>(RunGaitId));
		TestEqual(TEXT("and carries its own play rate"), GaitPlayRate(Extended.Resolved, SprintGaitId), 1.5f);
	}

	return true;
}

// What gait selection is fed is not a sampled speed. A remote entity is moved by interpolating from
// its last position update to the newest one over a FIXED duration, so its velocity reads the
// segment's speed for that duration and then exactly zero until the next update lands, whether or
// not the entity is still moving. This simulates that shape directly.
//
// Two things follow, and both are asserted below. The average of that shape over an update interval
// is the entity's true average speed, whatever the update rate and whatever the frame rate, so the
// smoothing must average. And when updates arrive at half the expected rate, each one carries twice
// the distance and so the PEAK reads twice the entity's real speed: a filter that follows peaks
// rather than the average would put a walking player into a run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGaitSmoothedSpeedTracksTrueSpeedTest,
	"CrowdySDK.Mass.GaitSmoothedSpeedTracksTrueSpeed", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassGaitSmoothedSpeedTracksTrueSpeedTest::RunTest(const FString& Parameters)
{
	const FGaitFixture Fixture = MakeGaitFixture();
	const FCrowdyMassResolvedAnimSet& Resolved = Fixture.Resolved;

	// The interpolation duration is a project setting and does not follow the update rate, which is
	// the whole reason the input looks the way it does.
	constexpr float InterpolationSeconds = 0.1f;

	// Runs the velocity an interpolated entity really produces: for each update, the segment's speed
	// for the interpolation duration and then zero until the next update.
	auto RunTraffic = [&Fixture](float TrueSpeed, float UpdateIntervalSeconds, float FrameSeconds, int32 Seconds,
		float& OutSmoothedSpeed, float& OutLowest, float& OutHighest)
	{
		const float SegmentSpeed = TrueSpeed * UpdateIntervalSeconds / InterpolationSeconds;
		const float Alpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(
			Fixture.AnimSet->SpeedSmoothingTimeSeconds, FrameSeconds, Fixture.AnimSet->MaxSmoothingStepSeconds);

		const int32 FrameCount = FMath::RoundToInt(Seconds / FrameSeconds);
		OutLowest = TNumericLimits<float>::Max();
		OutHighest = 0.0f;

		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			const float SecondsIntoInterval = FMath::Fmod(Frame * FrameSeconds, UpdateIntervalSeconds);
			const float RawSpeed = SecondsIntoInterval < InterpolationSeconds ? SegmentSpeed : 0.0f;
			OutSmoothedSpeed = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(OutSmoothedSpeed, RawSpeed, Alpha);

			// Ignore the first second, which is the filter converging from a standing start.
			if (Frame * FrameSeconds < 1.0f)
			{
				continue;
			}

			OutLowest = FMath::Min(OutLowest, OutSmoothedSpeed);
			OutHighest = FMath::Max(OutHighest, OutSmoothedSpeed);
		}
	};

	// The reading must be the entity's real speed at every combination of update rate and frame rate,
	// because the gait thresholds are authored once and cannot be tuned per machine or per connection
	// quality. 5 Hz is the dropped-packet case, and it is the one that reads double at the peak.
	const float UpdateIntervals[] = {0.1f, 0.2f};
	const float FrameLengths[] = {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f};
	constexpr float WalkingSpeed = 200.0f;

	for (const float UpdateInterval : UpdateIntervals)
	{
		for (const float FrameSeconds : FrameLengths)
		{
			float SmoothedSpeed = 0.0f;
			float Lowest = 0.0f;
			float Highest = 0.0f;
			RunTraffic(WalkingSpeed, UpdateInterval, FrameSeconds, 5, SmoothedSpeed, Lowest, Highest);

			const FString Case = FString::Printf(TEXT("%.0f Hz updates at %.0f fps"), 1.0f / UpdateInterval, 1.0f / FrameSeconds);
			TestTrue(*FString::Printf(TEXT("%s: the reading averages the entity's real speed"), *Case),
				FMath::IsNearlyEqual((Lowest + Highest) * 0.5f, WalkingSpeed, WalkingSpeed * 0.1f));
			// The ripple is the cost of averaging an input that alternates between a speed and zero.
			// It has to stay inside the hysteresis band or it becomes a gait change on its own.
			TestTrue(*FString::Printf(TEXT("%s: the ripple never reaches the next gait up"), *Case),
				Highest < Resolved.ResolvedGaits[JogGaitId - 1].EnterSpeed);
			TestTrue(*FString::Printf(TEXT("%s: the ripple never falls out of walk"), *Case),
				Lowest > Resolved.ResolvedGaits[WalkGaitId - 1].ExitSpeed);
		}
	}

	// The property that matters, stated directly, against the control that would break it. A walking
	// entity whose updates arrive at half rate must simply walk. Counting gait changes rather than
	// looking at the gait at the end is deliberate: the pathology is flicker, and any single sample
	// of a flickering value is just whichever phase the last frame happened to land on.
	{
		const float SegmentSpeed = WalkingSpeed * 0.2f / InterpolationSeconds;

		auto CountGaitChanges = [&Resolved, SegmentSpeed](float RiseAlpha, float FallAlpha)
		{
			float SmoothedSpeed = 0.0f;
			uint8 Gait = IdleGaitId;
			int32 Changes = 0;

			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				const float SecondsIntoInterval = FMath::Fmod(Frame / 60.0f, 0.2f);
				const float RawSpeed = SecondsIntoInterval < InterpolationSeconds ? SegmentSpeed : 0.0f;
				const float Alpha = RawSpeed > SmoothedSpeed ? RiseAlpha : FallAlpha;
				SmoothedSpeed = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(SmoothedSpeed, RawSpeed, Alpha);

				const uint8 NextGait = Resolved.SelectGait(Gait, SmoothedSpeed);
				// Ignore the first second, which is the entity getting up to speed from a standstill.
				Changes += Frame > 60 && NextGait != Gait ? 1 : 0;
				Gait = NextGait;
			}

			return MakeTuple(Gait, Changes);
		};

		const float AveragingAlpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(
			Fixture.AnimSet->SpeedSmoothingTimeSeconds, 1.0f / 60.0f, Fixture.AnimSet->MaxSmoothingStepSeconds);
		const TTuple<uint8, int32> Averaged = CountGaitChanges(AveragingAlpha, AveragingAlpha);

		TestEqual(TEXT("a walking entity on half-rate updates settles on walk"),
			static_cast<int32>(Averaged.Get<0>()), static_cast<int32>(WalkGaitId));
		TestEqual(TEXT("and stays there, changing gait not once"), Averaged.Get<1>(), 0);

		// Control: rises fast, falls slowly, so it follows the peak. At half rate each update carries
		// twice the distance, so the peak reads twice the entity's real speed.
		const TTuple<uint8, int32> PeakFollowed = CountGaitChanges(
			UCrowdyMassAnimSet::ComputeSmoothingAlpha(0.05f, 1.0f / 60.0f, 0.1f),
			UCrowdyMassAnimSet::ComputeSmoothingAlpha(0.2f, 1.0f / 60.0f, 0.1f));

		TestTrue(TEXT("following peaks instead flickers the same entity between gaits, once per update"),
			PeakFollowed.Get<1>() > 10);
	}

	// A real stop is the input going to zero and staying there, and must reach idle promptly.
	{
		const float Alpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(
			Fixture.AnimSet->SpeedSmoothingTimeSeconds, 1.0f / 60.0f, Fixture.AnimSet->MaxSmoothingStepSeconds);

		float SmoothedSpeed = 600.0f;
		uint8 Gait = RunGaitId;
		for (int32 Frame = 0; Frame < 60; ++Frame)
		{
			SmoothedSpeed = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(SmoothedSpeed, 0.0f, Alpha);
			Gait = Resolved.SelectGait(Gait, SmoothedSpeed);
		}

		TestEqual(TEXT("one second of standstill reaches idle"),
			static_cast<int32>(Gait), static_cast<int32>(IdleGaitId));
	}

	return true;
}

// Smoothing that is not framerate independent changes gaits at different speeds on different
// machines, and a frame long enough to matter must not be allowed to close the whole gap in one
// step: every in-flight interpolation also completes on a long frame, so the input on that frame is
// zero and an unclamped step would drop the entire crowd to idle and pop it back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSmoothingStepBoundsTest,
	"CrowdySDK.Mass.SmoothingStepBounds", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassSmoothingStepBoundsTest::RunTest(const FString& Parameters)
{
	constexpr float TimeConstant = 0.3f;
	constexpr float MaxStep = 0.1f;
	constexpr float Target = 600.0f;

	// Half a second of smoothing has to land in the same place however it is divided into frames.
	float Coarse = 0.0f;
	for (int32 Frame = 0; Frame < 15; ++Frame)
	{
		Coarse = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(Coarse, Target,
			UCrowdyMassAnimSet::ComputeSmoothingAlpha(TimeConstant, 1.0f / 30.0f, MaxStep));
	}

	float Fine = 0.0f;
	for (int32 Frame = 0; Frame < 72; ++Frame)
	{
		Fine = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(Fine, Target,
			UCrowdyMassAnimSet::ComputeSmoothingAlpha(TimeConstant, 1.0f / 144.0f, MaxStep));
	}

	TestTrue(TEXT("half a second of smoothing lands in the same place at 30 and 144 frames a second"),
		FMath::IsNearlyEqual(Coarse, Fine, 1.0f));

	// A hitch: one half-second frame, on which every interpolation completes and so the input is zero.
	const float HitchAlpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(TimeConstant, 0.5f, MaxStep);
	const float AfterHitch = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(Target, 0.0f, HitchAlpha);
	const float UnclampedAlpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(TimeConstant, 0.5f, 0.5f);
	const float AfterUnclampedHitch = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(Target, 0.0f, UnclampedAlpha);

	TestTrue(TEXT("a hitch frame is treated as no longer than the ceiling allows"), AfterHitch > Target * 0.5f);
	TestTrue(TEXT("and without the ceiling the same frame would all but zero the reading"),
		AfterUnclampedHitch < Target * 0.25f);

	// A non-finite reading must not be able to pin an entity: every comparison against a NaN is
	// false, so neither speeding up nor slowing down would ever fire again.
	const float Alpha = UCrowdyMassAnimSet::ComputeSmoothingAlpha(TimeConstant, 1.0f / 60.0f, MaxStep);
	const float AfterNaNInput = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(Target, FMath::Sqrt(-1.0f), Alpha);
	TestTrue(TEXT("a non-finite speed is treated as a standstill, not carried"), FMath::IsFinite(AfterNaNInput));
	TestTrue(TEXT("and a reading that is already non-finite recovers"),
		FMath::IsFinite(UCrowdyMassAnimSet::AdvanceSmoothedSpeed(FMath::Sqrt(-1.0f), 100.0f, Alpha)));

	return true;
}

// The order the animation tiers outrank each other in is the kind of rule that is only ever wrong in
// a direction nobody wrote a test for, so it is asserted directly rather than left implied by the
// shape of the code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassComposePrecedenceTest,
	"CrowdySDK.Mass.ComposePrecedence", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassComposePrecedenceTest::RunTest(const FString& Parameters)
{
	FGaitFixture Fixture = MakeGaitFixture();
	UAnimSequence* SitSequence = NewTestSequence();
	Fixture.Bank.Add(SitSequence);
	Fixture.AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));
	Fixture.Resolved.Build(Fixture.AnimSet, Fixture.Bank);

	const uint8 SitPoseId = static_cast<uint8>(Fixture.AnimSet->FindPoseId(TEXT("Sit")));
	const int32 SitIndex = PoseBankIndex(Fixture.Resolved, SitPoseId);
	const int32 RunIndex = GaitBankIndex(Fixture.Resolved, RunGaitId);
	TestNotEqual(TEXT("the fixture's sources resolve to distinct indices"), SitIndex, RunIndex);

	UAnimSequence* AttackPrimary = NewTestSequence();
	Fixture.Bank.Add(AttackPrimary);
	Fixture.Bank.Add(AttackPrimary);
	const int32 AttackPrimaryIndex = Fixture.Bank.Num() - 2;
	const int32 AttackAlternateIndex = Fixture.Bank.Num() - 1;
	Fixture.AnimSet->Actions.Add(MakeActionEntry(TEXT("Attack"), AttackPrimaryIndex, AttackAlternateIndex));
	Fixture.Resolved.Build(Fixture.AnimSet, Fixture.Bank);

	FCrowdyMassAnimStateFragment AnimState;
	AnimState.Gait = RunGaitId;
	FCrowdyMassActionFragment ActionState;

	TestEqual(TEXT("with nothing else set, the gait is what plays"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, RunIndex);
	TestEqual(TEXT("and it carries that gait's authored play rate"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).PlayRate, 1.3f);

	// An entity whose gait has not been resolved yet composes nothing from the gait tier, rather than
	// the floor row: the id is the whole of what the tier reads, and reading 0 as a row would show a
	// crowd idling before its first animation pass.
	{
		FCrowdyMassAnimStateFragment NoGaitState;
		NoGaitState.Gait = 0;
		TestEqual(TEXT("gait id 0 selects nothing at all"),
			ComposeCrowdyMassAnimation(NoGaitState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex,
			static_cast<int32>(INDEX_NONE));
	}

	// ActionId 1 is Attack, the first (and only) authored action. Nothing of it has been shown yet
	// (Serial 1 against AppliedSerial 0), and the entity is showing a gait, so the first trigger takes
	// the primary slot.
	ActionState.ActionId = 1;
	ActionState.Serial = 1;
	const FCrowdyMassComposedAnimation ActionComposed = ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f);
	TestEqual(TEXT("an action outranks the gait"), ActionComposed.BankIndex, AttackPrimaryIndex);
	TestTrue(TEXT("and is flagged as the action tier for the Position correction"), ActionComposed.bIsAction);
	TestTrue(TEXT("and is flagged as a trigger the renderer has not been given yet"), ActionComposed.bIsFirstShowOfTrigger);
	TestTrue(TEXT("and holds its last pose instead of looping"), ActionComposed.LoopMode == EAnimSequenceTrackLoopMode::Clamp);

	// The animation processor's push does both of these together.
	AnimState.SequenceIndex = AttackPrimaryIndex;
	ActionState.AppliedSerial = ActionState.Serial;

	const FCrowdyMassComposedAnimation Settled = ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f);
	TestEqual(TEXT("with the trigger shown, the same slot is composed so the push decision is a no-op"),
		Settled.BankIndex, AttackPrimaryIndex);
	TestFalse(TEXT("and it is no longer a first show, so the Position correction cannot run twice"),
		Settled.bIsFirstShowOfTrigger);

	ActionState.Serial = 2;
	TestEqual(TEXT("retriggering alternates to the second slot, since the engine only reacts to an index change"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, AttackAlternateIndex);

	// The case the old parity rule got wrong, and the reason the slot is chosen against what was
	// actually pushed rather than against the Serial's low bit. Two triggers landing between two
	// animation passes leave the parity where it started; the index would then not change, the push
	// would be skipped as a no-op, and neither swing would ever be shown.
	ActionState.Serial = 3;
	TestEqual(TEXT("two triggers between two animation passes still change the pushed index"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, AttackAlternateIndex);
	TestTrue(TEXT("and are still flagged as an unshown trigger"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).bIsFirstShowOfTrigger);

	// An action interrupted by something above it and then returning must replay rather than sit
	// silent, which a Serial comparison alone could not express: the Serial has not moved, but what is
	// on screen is no longer this action.
	ActionState.Serial = 3;
	ActionState.AppliedSerial = 3;
	AnimState.SequenceIndex = SitIndex;
	TestEqual(TEXT("an action returning after something outranked it takes a slot again rather than staying silent"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, AttackPrimaryIndex);

	AnimState.SequenceIndex = CrowdyMassAnimIndexNone;
	ActionState.AppliedSerial = 0;
	ActionState.Serial = 0;
	ActionState.ActionId = 0;

	TestEqual(TEXT("a pose outranks the gait"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, SitPoseId, Fixture.Resolved, 0.2f).BankIndex, SitIndex);

	// The authored rank is the whole of a pose's precedence, and both directions are asserted here
	// rather than only the one the shipping content happens to use. A rank that was read but never
	// compared, or compared the wrong way round, passes half of this.
	ActionState.ActionId = 1;
	TestEqual(TEXT("a pose ranked below actions gives way to one"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, SitPoseId, Fixture.Resolved, 0.2f).BankIndex, AttackPrimaryIndex);

	Fixture.AnimSet->Poses[SitPoseId - 1].Rank = ECrowdyMassPoseRank::AboveActions;
	Fixture.Resolved.Build(Fixture.AnimSet, Fixture.Bank);
	TestEqual(TEXT("the same pose ranked above actions overrides one"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, SitPoseId, Fixture.Resolved, 0.2f).BankIndex, SitIndex);
	ActionState.ActionId = 0;

	// Gait selection keeps running underneath a pose, so clearing the pose has to return the entity to
	// its current locomotion rather than to whatever it was doing when the pose began.
	TestEqual(TEXT("clearing the pose returns the entity to its gait"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, RunIndex);

	// A pose id nobody authored must not swallow the sources below it.
	TestEqual(TEXT("an unauthored pose does not outrank anything"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 99, Fixture.Resolved, 0.2f).BankIndex, RunIndex);

	// An ActionId nothing authored (or whose pair did not resolve) must not swallow the sources below
	// it either.
	ActionState.ActionId = 99;
	TestEqual(TEXT("an unresolved action does not outrank anything"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, RunIndex);

	return true;
}

// A held pose is what a game builds a death out of, so this walks that case end to end: the pose
// overrides an action while it is set, carries its own authored playback, and clearing it returns the
// entity to what it was doing with nothing else cleared. It also checks the thing
// ComposeCrowdyMassAnimation cannot see on its own: the renderer only reacts to the pushed sequence
// index actually CHANGING, so the pose and what follows it have to compose to two distinct indices,
// not just two distinct calls, or the push after clearing would be silently dropped as a no-op.
//
// Deliberately no death-specific machinery is involved. That is the property under test as much as
// any assertion here: if showing a corpse ever needs something this file cannot express with an
// authored row, the extension point has stopped being one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassComposeHeldPoseTest,
	"CrowdySDK.Mass.ComposeHeldPose", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassComposeHeldPoseTest::RunTest(const FString& Parameters)
{
	FGaitFixture Fixture = MakeGaitFixture();

	// The death clip gets a bank slot of its own, which is the authoring rule the D1 gate enforces at
	// runtime: a pose sharing a slot with what the entity returns to would leave it frozen there, since
	// the renderer only reacts to the pushed index changing.
	UAnimSequence* DeadSequence = NewTestSequence();
	Fixture.Bank.Add(DeadSequence);
	Fixture.AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), DeadSequence, ECrowdyMassPoseRank::AboveActions, true));
	Fixture.AnimSet->Poses[0].PlayRate = 0.5f;

	UAnimSequence* AttackSequence = NewTestSequence();
	Fixture.Bank.Add(AttackSequence);
	Fixture.Bank.Add(AttackSequence);
	const int32 AttackPrimaryIndex = Fixture.Bank.Num() - 2;
	const int32 AttackAlternateIndex = Fixture.Bank.Num() - 1;
	Fixture.AnimSet->Actions.Add(MakeActionEntry(TEXT("Attack"), AttackPrimaryIndex, AttackAlternateIndex));

	Fixture.Resolved.Build(Fixture.AnimSet, Fixture.Bank);

	const int32 RunIndex = GaitBankIndex(Fixture.Resolved, RunGaitId);
	const uint8 DeadPoseId = static_cast<uint8>(Fixture.AnimSet->FindPoseId(TEXT("Dead")));
	const int32 DeadIndex = PoseBankIndex(Fixture.Resolved, DeadPoseId);

	TestNotEqual(TEXT("the pose resolved against the bank"), DeadIndex, static_cast<int32>(INDEX_NONE));
	TestNotEqual(TEXT("the fixture's pose and run indices are distinct"), DeadIndex, RunIndex);

	FCrowdyMassAnimStateFragment AnimState;
	AnimState.Gait = RunGaitId;
	FCrowdyMassActionFragment ActionState;

	TestEqual(TEXT("with no pose set, the gait is what plays"),
		ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f).BankIndex, RunIndex);

	// An action running at the moment the pose is set, which is the case that matters: a player dying
	// mid-swing must show the corpse, not finish the swing.
	ActionState.ActionId = 1;
	ActionState.Serial = 1;

	const FCrowdyMassComposedAnimation Held = ComposeCrowdyMassAnimation(AnimState, ActionState, DeadPoseId, Fixture.Resolved, 0.2f);
	TestEqual(TEXT("a pose ranked above actions overrides an action already playing"), Held.BankIndex, DeadIndex);
	TestEqual(TEXT("and carries the pose's authored play rate"), Held.PlayRate, 0.5f);
	TestTrue(TEXT("and holds its last frame instead of looping"), Held.LoopMode == EAnimSequenceTrackLoopMode::Clamp);
	TestFalse(TEXT("and is not the action tier, so the Position correction cannot fire for it"), Held.bIsAction);

	// The animation processor's push, simulated: the pose actually reaches the renderer.
	TestTrue(TEXT("the pose is a real push from the run index"),
		ShouldPushCrowdyMassAnimation(Held.BankIndex, RunIndex, RunIndex));
	AnimState.SequenceIndex = Held.BankIndex;

	// Clearing the pose and nothing else. The action left set above is exactly what production code
	// leaves in place, since whoever triggered it owns clearing it, not the pose.
	const FCrowdyMassComposedAnimation Cleared = ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f);
	TestEqual(TEXT("clearing the pose returns to whichever source is highest, not straight to gait"),
		Cleared.BankIndex, AttackPrimaryIndex);
	TestTrue(TEXT("and that is a real push, a different index than the pose was showing"),
		ShouldPushCrowdyMassAnimation(Cleared.BankIndex, Held.BankIndex, AnimState.SequenceIndex));

	ActionState.ActionId = 0;
	const FCrowdyMassComposedAnimation BackToLocomotion = ComposeCrowdyMassAnimation(AnimState, ActionState, 0, Fixture.Resolved, 0.2f);
	TestEqual(TEXT("with nothing else active, clearing the pose returns to the entity's gait on its own"),
		BackToLocomotion.BankIndex, RunIndex);
	TestTrue(TEXT("and that is a real push too: the pose and locomotion are two distinct indices"),
		ShouldPushCrowdyMassAnimation(BackToLocomotion.BankIndex, Held.BankIndex, AnimState.SequenceIndex));

	// A pose left looping is a live authoring case rather than a mistake, so the flag has to be read
	// per row rather than assumed for anything ranked above actions.
	{
		FGaitFixture LoopingFixture = MakeGaitFixture();
		UAnimSequence* ChannelSequence = NewTestSequence();
		LoopingFixture.Bank.Add(ChannelSequence);
		LoopingFixture.AnimSet->Poses.Add(MakePoseEntry(TEXT("Channel"), ChannelSequence, ECrowdyMassPoseRank::AboveActions, false));
		LoopingFixture.Resolved.Build(LoopingFixture.AnimSet, LoopingFixture.Bank);

		FCrowdyMassAnimStateFragment LoopingAnimState;
		LoopingAnimState.Gait = RunGaitId;
		FCrowdyMassActionFragment LoopingActionState;

		const uint8 ChannelPoseId = static_cast<uint8>(LoopingFixture.AnimSet->FindPoseId(TEXT("Channel")));
		TestTrue(TEXT("a pose without Hold Last Frame loops"),
			ComposeCrowdyMassAnimation(LoopingAnimState, LoopingActionState, ChannelPoseId, LoopingFixture.Resolved, 0.2f).LoopMode
				== EAnimSequenceTrackLoopMode::Loop);
	}

	// A pose whose sequence is absent from the bank must not freeze the entity: it falls through to
	// whatever the sources below it call for, exactly as an unresolved action does. This is the shape
	// an authoring gap takes, and it is the reason a missing death clip is a visual bug and never a
	// stuck entity.
	{
		FGaitFixture OrphanFixture = MakeGaitFixture();
		UAnimSequence* OrphanSequence = NewTestSequence();
		OrphanFixture.AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), OrphanSequence, ECrowdyMassPoseRank::AboveActions, true));
		OrphanFixture.Resolved.Build(OrphanFixture.AnimSet, OrphanFixture.Bank);

		const uint8 OrphanPoseId = static_cast<uint8>(OrphanFixture.AnimSet->FindPoseId(TEXT("Dead")));
		TestNotEqual(TEXT("the pose is authored, so it holds an id"), static_cast<int32>(OrphanPoseId), 0);
		TestEqual(TEXT("but resolves to no bank index"), PoseBankIndex(OrphanFixture.Resolved, OrphanPoseId), INDEX_NONE);

		FCrowdyMassAnimStateFragment OrphanAnimState;
		OrphanAnimState.Gait = RunGaitId;
		FCrowdyMassActionFragment OrphanActionState;

		TestEqual(TEXT("an entity in a pose that does not resolve falls through to its gait"),
			ComposeCrowdyMassAnimation(OrphanAnimState, OrphanActionState, OrphanPoseId, OrphanFixture.Resolved, 0.2f).BankIndex,
			GaitBankIndex(OrphanFixture.Resolved, RunGaitId));
	}

	return true;
}

// Skipping an index the renderer already has is not an optimisation: the renderer ignores everything
// sent without an index change, so it is the reason a crowd standing still costs nothing. The
// exception is the first push, because an untouched entity already reads as bank slot 0 and an
// entity whose selection genuinely is slot 0 would otherwise never be given its authored play rate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPushDecisionTest,
	"CrowdySDK.Mass.PushDecision", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPushDecisionTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("an index the entity already has is not pushed again"),
		ShouldPushCrowdyMassAnimation(3, 3, 3));
	TestTrue(TEXT("a changed index is pushed"),
		ShouldPushCrowdyMassAnimation(4, 3, 3));
	TestFalse(TEXT("nothing selected means nothing is pushed"),
		ShouldPushCrowdyMassAnimation(INDEX_NONE, 3, 3));

	// The first push for an entity, where the index it reads is the default rather than a choice.
	TestTrue(TEXT("slot 0 is pushed once even though an untouched entity already reads slot 0"),
		ShouldPushCrowdyMassAnimation(0, 0, INDEX_NONE));
	TestFalse(TEXT("and is not pushed again once it has been"),
		ShouldPushCrowdyMassAnimation(0, 0, 0));

	// Reading back what was actually pushed, rather than trusting the entity's own record, is what
	// restores an index some other writer overwrote.
	TestTrue(TEXT("an index another writer overwrote is pushed again"),
		ShouldPushCrowdyMassAnimation(5, 1, 5));

	return true;
}

// FindResolvedPose(uint8) is the per-entity lookup a forged wire byte reaches directly, with no name
// search and no bounds check upstream of it, so its own bounds check is the only thing standing
// between a forged id and an out-of-range array read. Covers the sentinel, a valid id, an id far past
// the authored array's end, and the distinction between "never authored" (id 0, the sentinel path,
// never touches the array) and "authored but its sequence did not resolve against the bank" (a real
// row, INDEX_NONE for a different reason).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseIdBoundsTest,
	"CrowdySDK.Mass.PoseIdBounds", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseIdBoundsTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* SitSequence = NewTestSequence();
	UAnimSequence* OrphanSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	// Two authored rows only, so an id past Poses.Num() is easy to reach without going anywhere
	// near uint8's own range limit.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));
	// Authored, but its sequence is absent from the bank below: a real row that still answers INDEX_NONE,
	// for a different reason than an id nothing authored at all.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Broken"), OrphanSequence));

	const TArray<UAnimSequence*> Bank = {IdleSequence, SitSequence};

	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, Bank);

	TestEqual(TEXT("id 0 is the sentinel and is never looked up"),
		PoseBankIndex(Resolved,0), INDEX_NONE);
	TestEqual(TEXT("a valid id resolves to its authored bank index"),
		PoseBankIndex(Resolved,1), 1);
	TestEqual(TEXT("an authored row whose sequence did not resolve is INDEX_NONE, not a crash"),
		PoseBankIndex(Resolved,2), INDEX_NONE);

	// Only two rows are authored (ids 1 and 2), so every id from 3 up to 255 is past the end of
	// ResolvedPoses. A bounds check missing here would read past the array instead of
	// answering INDEX_NONE.
	TestEqual(TEXT("an id one past the authored array selects nothing, not an out-of-bounds read"),
		PoseBankIndex(Resolved,3), INDEX_NONE);
	TestEqual(TEXT("the largest possible uint8 id selects nothing either"),
		PoseBankIndex(Resolved,255), INDEX_NONE);

	return true;
}

// Every resolved row must describe the row it indexes. Resolution used to go back through the by-name
// lookups, which return the FIRST row carrying a name, so a duplicate name gave row N the first row's
// bank index while row N kept its own rank and play rate: a pose that exists in no authored row. On
// the action path the same shape is worse, because the name search validates one row's slot pair and
// the copy then takes another row's raw indices, which need not be in the bank at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDuplicateRowsResolveIndependentlyTest,
	"CrowdySDK.Mass.DuplicateRowsResolveIndependently", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassDuplicateRowsResolveIndependentlyTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* FirstHoldSequence = NewTestSequence();
	UAnimSequence* SecondHoldSequence = NewTestSequence();
	UAnimSequence* SwingSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));

	// Two rows under one name, deliberately differing in every field a resolved pose carries.
	FCrowdyMassPoseEntry FirstHold = MakePoseEntry(TEXT("Hold"), FirstHoldSequence, ECrowdyMassPoseRank::BelowActions, false);
	FirstHold.PlayRate = 1.0f;
	FCrowdyMassPoseEntry SecondHold = MakePoseEntry(TEXT("Hold"), SecondHoldSequence, ECrowdyMassPoseRank::AboveActions, true);
	SecondHold.PlayRate = 2.0f;
	AnimSet->Poses.Add(FirstHold);
	AnimSet->Poses.Add(SecondHold);

	// Slots 1 and 2 hold the same asset, which is what an action's slot pair needs.
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Swing"), 1, 2));
	// The same name again, with indices past the end of the bank. Validating by name would validate the
	// row above and then copy these in regardless.
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Swing"), 7, 8));

	const TArray<UAnimSequence*> Bank = {IdleSequence, SwingSequence, SwingSequence, FirstHoldSequence, SecondHoldSequence};

	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, Bank);

	// Pose id 2 is the second row, whichever row FindPoseId hands back for the shared name.
	const FCrowdyMassResolvedPose* SecondPose = Resolved.FindResolvedPose(2);
	TestNotNull(TEXT("the second row of a duplicated pose name is still an addressable id"), SecondPose);
	if (SecondPose)
	{
		TestEqual(TEXT("it carries its OWN sequence's bank slot, not the first row's"), SecondPose->BankIndex, 4);
		TestTrue(TEXT("and its own rank"), SecondPose->Rank == ECrowdyMassPoseRank::AboveActions);
		TestEqual(TEXT("and its own play rate"), SecondPose->PlayRate, 2.0f);
		TestTrue(TEXT("and its own loop mode"), SecondPose->LoopMode == EAnimSequenceTrackLoopMode::Clamp);
	}

	const FCrowdyMassResolvedPose* FirstPose = Resolved.FindResolvedPose(1);
	TestNotNull(TEXT("the first row is unaffected"), FirstPose);
	if (FirstPose)
	{
		TestEqual(TEXT("the first row keeps its own bank slot"), FirstPose->BankIndex, 3);
	}

	// Action id 2 is the second row, whose pair is out of the bank entirely. Refusing it is the whole
	// point: an unchecked copy would hand the renderer slot 7 of a five-slot bank.
	const FCrowdyMassResolvedAction* SecondAction = Resolved.FindResolvedAction(2);
	TestNotNull(TEXT("the second row of a duplicated action name is still an addressable id"), SecondAction);
	if (SecondAction)
	{
		TestEqual(TEXT("an out-of-bank slot pair resolves to nothing rather than being copied in"),
			SecondAction->PrimaryBankIndex, INDEX_NONE);
		TestEqual(TEXT("both halves of the pair, not just the first"),
			SecondAction->AlternateBankIndex, INDEX_NONE);
	}

	const FCrowdyMassResolvedAction* FirstAction = Resolved.FindResolvedAction(1);
	TestNotNull(TEXT("the first action row is unaffected"), FirstAction);
	if (FirstAction)
	{
		TestEqual(TEXT("the valid pair still resolves"), FirstAction->PrimaryBankIndex, 1);
		TestEqual(TEXT("both slots of it"), FirstAction->AlternateBankIndex, 2);
	}

	return true;
}

// A pose id has one byte to live in and 0 is the value that CLEARS the pose, so row 256 would take an
// id that truncates to "no pose". Naming it must refuse and say so, never hand back an id that means
// the opposite of what was asked for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseIdOverflowRefusedTest,
	"CrowdySDK.Mass.PoseIdOverflowRefused", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseIdOverflowRefusedTest::RunTest(const FString& Parameters)
{
	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

	// One row past the limit. The sequences are irrelevant here: naming an id is a search of the Poses
	// array and never touches the bank.
	for (int32 RowIndex = 0; RowIndex < CrowdyMassMaxPoses + 1; ++RowIndex)
	{
		AnimSet->Poses.Add(MakePoseEntry(FName(*FString::Printf(TEXT("Pose%d"), RowIndex)), nullptr));
	}

	TestEqual(TEXT("the first row is id 1"), AnimSet->FindPoseId(TEXT("Pose0")), 1);
	TestEqual(TEXT("the last reachable row is the id the limit names"),
		AnimSet->FindPoseId(FName(*FString::Printf(TEXT("Pose%d"), CrowdyMassMaxPoses - 1))), CrowdyMassMaxPoses);

	AddExpectedErrorPlain(TEXT("past the 255 a one-byte pose id can reach, so it can never be selected"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// 0 rather than 256, and 0 is not merely "unknown" here: it is the value that clears a pose, so
	// truncating would have SetEntityPose report success while removing the pose it was asked to set.
	TestEqual(TEXT("the row past the limit is refused rather than truncated"),
		AnimSet->FindPoseId(FName(*FString::Printf(TEXT("Pose%d"), CrowdyMassMaxPoses))), 0);

	return true;
}

// A pose has to own its bank slot outright, because the renderer only reacts to the pushed index
// CHANGING: an entity leaving a pose for something that shares its slot is handed the index it already
// has and stays frozen in the pose it just left. Every entry involved resolves correctly on its own,
// so this is only visible against a bank.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseSlotCollisionsTest,
	"CrowdySDK.Mass.PoseSlotCollisions", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseSlotCollisionsTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* DeadSequence = NewTestSequence();
	UAnimSequence* SwingSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Swing"), 2, 3));

	// Slot 1: two poses on it. A corpse that stands back up into the other pose shows nothing at all.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), DeadSequence, ECrowdyMassPoseRank::AboveActions, true));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), DeadSequence));
	// Slot 0: the gait the entity returns to when the pose clears.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Frozen"), IdleSequence));
	// Slot 2: the action's primary slot.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Swinging"), SwingSequence));

	const TArray<UAnimSequence*> Bank = {IdleSequence, DeadSequence, SwingSequence, SwingSequence};

	AddExpectedErrorPlain(TEXT("share bank slot 1, so switching directly between them changes nothing on screen"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("shares bank slot 0 with gait Idle"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("shares bank slot 2 with action Swing"),
		EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("a set whose poses share slots reports invalid"), AnimSet->ValidateAgainstBank(Bank));

	// The same set with each pose on a slot of its own has to stay silent, or the check would be a
	// standing warning nobody can act on.
	UAnimSequence* SitSequence = NewTestSequence();
	UCrowdyMassAnimSet* CleanSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	CleanSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	CleanSet->Poses.Add(MakePoseEntry(TEXT("Dead"), DeadSequence, ECrowdyMassPoseRank::AboveActions, true));
	CleanSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));

	const TArray<UAnimSequence*> CleanBank = {IdleSequence, DeadSequence, SitSequence};
	TestTrue(TEXT("poses on slots of their own report valid"), CleanSet->ValidateAgainstBank(CleanBank));

	return true;
}

// The renderer's playback clock starts when the pushed index changes, not when the action was
// triggered, so an action applied late has to start from where it should already be. Bounded at both
// ends: never before the start, never past the clip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionCorrectedPositionTest,
	"CrowdySDK.Mass.ActionCorrectedPosition", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassActionCorrectedPositionTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an action applied on the frame it triggered starts at the beginning"),
		GetCrowdyMassActionCorrectedPosition(0.0f, 2.0f), 0.0f);
	TestEqual(TEXT("an action that waited starts from however long it waited"),
		GetCrowdyMassActionCorrectedPosition(0.5f, 2.0f), 0.5f);

	// Clamped rather than seeking past the end, which the renderer defines no behaviour for. Clamp loop
	// mode then holds the last frame, so an action applied after it should have finished still resolves
	// to a pose rather than to nothing.
	TestEqual(TEXT("an action applied after it should have ended stops at its last frame"),
		GetCrowdyMassActionCorrectedPosition(5.0f, 2.0f), 2.0f);
	TestEqual(TEXT("a negative elapsed time is treated as just started"),
		GetCrowdyMassActionCorrectedPosition(-1.0f, 2.0f), 0.0f);

	// An action with no authored duration cannot be positioned into, so the only safe answer is the
	// start. Returning the elapsed time instead would seek past the end of whatever clip is playing.
	TestEqual(TEXT("a zero duration positions at the start"),
		GetCrowdyMassActionCorrectedPosition(1.0f, 0.0f), 0.0f);
	TestEqual(TEXT("a negative duration positions at the start"),
		GetCrowdyMassActionCorrectedPosition(1.0f, -2.0f), 0.0f);

	// Neither operand comes off the wire today, but Position feeds a renderer with no defined behaviour
	// for a non-finite value, so neither may propagate one.
	const float NotANumber = FMath::Sqrt(-1.0f);
	TestEqual(TEXT("a non-finite elapsed time is treated as just started"),
		GetCrowdyMassActionCorrectedPosition(NotANumber, 2.0f), 0.0f);
	TestEqual(TEXT("a non-finite duration positions at the start"),
		GetCrowdyMassActionCorrectedPosition(1.0f, NotANumber), 0.0f);

	return true;
}

#if WITH_EDITOR
// IsDataValid must reject every shadowed-key and unset-sequence shape the runtime lookups would
// otherwise silently paper over with FindByPredicate's first-match behavior: a duplicate GaitName, a
// duplicate ActionName, a duplicate PoseName, a NAME_None key on an action and on a pose, an unset
// gait Sequence, an unset pose Sequence, and an identical-index action pair. Each authored violation
// must produce exactly one error, so a mutation that drops any single check can be seen by an exact
// error count rather than only a pass/fail bit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetDuplicateKeysRejectedTest,
	"CrowdySDK.Mass.AnimSetDuplicateKeysRejected", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetDuplicateKeysRejectedTest::RunTest(const FString& Parameters)
{
	UAnimSequence* IdleSequence = NewTestSequence();
	UAnimSequence* SitSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

	// Gaits: a duplicate Idle name (1 error), the second Idle row also reusing the first's sequence
	// (1 error), and a Walk row with no Sequence authored (1 error).
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), IdleSequence));
	AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), nullptr));

	// Actions: an unnamed row (1 error), a duplicate "Attack" name (1 error), and an identical-index
	// pair (1 error).
	AnimSet->Actions.Add(MakeActionEntry(NAME_None, 0, 1));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Attack"), 0, 1));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Attack"), 2, 3));
	AnimSet->Actions.Add(MakeActionEntry(TEXT("Same"), 0, 0));

	// Poses: an unnamed row (1 error), a duplicate "Sit" name (1 error), and a "Rest" row with
	// no Sequence authored (1 error).
	AnimSet->Poses.Add(MakePoseEntry(NAME_None, IdleSequence));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Sit"), SitSequence));
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Rest"), nullptr));

	FDataValidationContext Context;
	const EDataValidationResult Result = AnimSet->IsDataValid(Context);

	TestTrue(TEXT("an asset with duplicate keys and unset sequences fails validation"),
		Result == EDataValidationResult::Invalid);
	TestEqual(TEXT("exactly one error per authored violation"),
		static_cast<int32>(Context.GetNumErrors()), 9 + ErrorsFromPoseRowsWithNoStateEnum);

	return true;
}

// A Poses array longer than a one-byte id can address is a fault of the array rather than of any row
// in it, so it is reported once against the array. Every row past the limit is individually well
// authored, which is exactly why nothing else can see it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetPoseCountValidatedTest,
	"CrowdySDK.Mass.AnimSetPoseCountValidated", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetPoseCountValidatedTest::RunTest(const FString& Parameters)
{
	UAnimSequence* PoseSequence = NewTestSequence();

	UCrowdyMassAnimSet* AtLimit = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	for (int32 RowIndex = 0; RowIndex < CrowdyMassMaxPoses; ++RowIndex)
	{
		AtLimit->Poses.Add(MakePoseEntry(FName(*FString::Printf(TEXT("Pose%d"), RowIndex)), PoseSequence));
	}

	{
		// Every row shares one sequence here, which is a separate fault the pose-vs-pose slot check
		// reports against a BANK. IsDataValid has no bank, so the only error it can raise on this asset
		// is the row count, and there is none yet.
		FDataValidationContext Context;
		AtLimit->IsDataValid(Context);
		TestEqual(TEXT("an array exactly at the limit raises no count error"),
			static_cast<int32>(Context.GetNumErrors()), ErrorsFromPoseRowsWithNoStateEnum);
	}

	AtLimit->Poses.Add(MakePoseEntry(TEXT("OnePastTheEnd"), PoseSequence));

	{
		FDataValidationContext Context;
		TestTrue(TEXT("an array past the limit fails validation"),
			AtLimit->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once against the array, not once per unreachable row"),
			static_cast<int32>(Context.GetNumErrors()), ErrorsFromPoseRowsWithNoStateEnum + 1);
	}

	return true;
}

// The two speed relationships gait selection depends on, neither of which is visible in the details
// panel. A gait entered at a lower speed than a slower gait makes the slower one unreachable, and a
// gait with no gap between its enter and exit speeds changes animation every frame at that speed.
// Both are authoring mistakes that produce no error anywhere at runtime.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetGaitSpeedsValidatedTest,
	"CrowdySDK.Mass.AnimSetGaitSpeedsValidated", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetGaitSpeedsValidatedTest::RunTest(const FString& Parameters)
{
	{
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 0.0f, 0.0f));
		// Exit above enter: the entity would leave on the frame it arrived.
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), NewTestSequence(), 100.0f, 150.0f));
		// No band at all.
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), NewTestSequence(), 300.0f, 300.0f));
		// Entered below the row beneath it, which makes that one unreachable.
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), NewTestSequence(), 200.0f, 180.0f));

		FDataValidationContext Context;
		TestTrue(TEXT("speed relationships that break gait selection fail validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("one error each for exit above enter, no band, and a non-ascending enter speed"),
			static_cast<int32>(Context.GetNumErrors()), 3);
	}

	{
		// A well authored set must pass cleanly, or the checks above are just noise.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 0.0f, 0.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), NewTestSequence(), 100.0f, 60.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), NewTestSequence(), 300.0f, 250.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), NewTestSequence(), 500.0f, 430.0f));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("a well authored gait table reports no errors"),
			static_cast<int32>(Context.GetNumErrors()), 0);
	}

	{
		// The row order is what selection walks, so a set is not limited to four gaits and a fifth
		// authored row has to validate as cleanly as the four above it.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 0.0f, 0.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Crawl"), NewTestSequence(), 40.0f, 20.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Walk"), NewTestSequence(), 100.0f, 60.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), NewTestSequence(), 300.0f, 250.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), NewTestSequence(), 500.0f, 430.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Sprint"), NewTestSequence(), 800.0f, 700.0f));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("six authored gait rows report no errors"),
			static_cast<int32>(Context.GetNumErrors()), 0);
	}

	{
		// An unnamed row and a duplicated name are both copy-paste mistakes that leave every message
		// about the table pointing at the wrong row, and neither is visible in the details panel.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 0.0f, 0.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(NAME_None, NewTestSequence(), 100.0f, 60.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 300.0f, 250.0f));

		FDataValidationContext Context;
		TestTrue(TEXT("an unnamed or duplicated gait row fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("one error for the unnamed row and one for the duplicated name"),
			static_cast<int32>(Context.GetNumErrors()), 2);
	}

	{
		// Two gaits on one sequence resolve to one bank slot, so the switch between them is invisible
		// to the renderer and the second gait's play rate is discarded. This is the tempting shape when
		// a bank is short of clips.
		UAnimSequence* Shared = NewTestSequence();
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Idle"), NewTestSequence(), 0.0f, 0.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Jog"), Shared, 300.0f, 250.0f, 1.0f));
		AnimSet->Gaits.Add(MakeGaitEntry(TEXT("Run"), Shared, 500.0f, 430.0f, 1.6f));

		FDataValidationContext Context;
		TestTrue(TEXT("two gaits sharing one sequence fail validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once, against the second gait to use it"),
			static_cast<int32>(Context.GetNumErrors()), 1);
	}

	return true;
}
#endif // WITH_EDITOR

// A bank can be re-authored in place, swapping one sequence for another or reordering two, without
// its length changing and without the provider asset being replaced. Every index in a table resolved
// against the old contents would then mean a different animation, and every one of them would still
// be in range, so nothing downstream could notice. The staleness check therefore has to be over the
// contents, not the length.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassResolvedAnimSetBankStalenessTest,
	"CrowdySDK.Mass.ResolvedAnimSetBankStaleness", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassResolvedAnimSetBankStalenessTest::RunTest(const FString& Parameters)
{
	const FGaitFixture Fixture = MakeGaitFixture();

	TestTrue(TEXT("a table matches the bank it was built against"),
		Fixture.Resolved.MatchesSource(Fixture.AnimSet, Fixture.Bank));

	TArray<UAnimSequence*> Reordered = Fixture.Bank;
	Swap(Reordered[0], Reordered[1]);
	TestFalse(TEXT("reordering two entries is caught, even though the length is unchanged"),
		Fixture.Resolved.MatchesSource(Fixture.AnimSet, Reordered));

	TArray<UAnimSequence*> Substituted = Fixture.Bank;
	Substituted[2] = NewTestSequence();
	TestFalse(TEXT("swapping one entry for a different asset is caught"),
		Fixture.Resolved.MatchesSource(Fixture.AnimSet, Substituted));

	TArray<UAnimSequence*> Shortened = Fixture.Bank;
	Shortened.Pop();
	TestFalse(TEXT("a shortened bank is caught"), Fixture.Resolved.MatchesSource(Fixture.AnimSet, Shortened));

	TestFalse(TEXT("a table that was never built matches nothing"),
		FCrowdyMassResolvedAnimSet().MatchesSource(Fixture.AnimSet, Fixture.Bank));

	// A table built against an empty bank is a real, if useless, result and must not read as unbuilt:
	// otherwise it would be rebuilt on every frame it is asked for.
	FCrowdyMassResolvedAnimSet EmptyBankTable;
	EmptyBankTable.Build(Fixture.AnimSet, TArray<UAnimSequence*>());
	TestTrue(TEXT("a table built against an empty bank still matches an empty bank"),
		EmptyBankTable.MatchesSource(Fixture.AnimSet, TArray<UAnimSequence*>()));

	return true;
}

/**
 * A table is also stale when the SET is re-authored, which the bank cannot report.
 *
 * This is the half a bank comparison structurally cannot see: adding a pose row, retyping a gait's speeds
 * or repointing the state enum leaves the bank identical in length, in contents and in identity, while
 * every row the table describes has moved or gained a neighbour. Without it, a resolved table answers from
 * the rows as they were when the level started for the rest of the session, so acting on the very
 * validation message that asked for a row changes nothing on screen.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassResolvedAnimSetSourceStalenessTest,
	"CrowdySDK.Mass.ResolvedAnimSetSourceStaleness", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassResolvedAnimSetSourceStalenessTest::RunTest(const FString& Parameters)
{
	UAnimSequence* DeadSequence = NewTestSequence();
	UAnimSequence* MountedSequence = NewTestSequence();

	UEnum* StateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceResolvedStale"),
		{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}, {TEXT("Mounted"), 2}});

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = StateEnum;
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), DeadSequence));

	// The bank stays this exact array throughout, so nothing below can be caught by the bank comparison.
	const TArray<UAnimSequence*> Bank = {DeadSequence, MountedSequence};

	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, Bank);

	TestTrue(TEXT("a freshly built table matches the set it came from"),
		Resolved.MatchesSource(AnimSet, Bank));
	TestEqual(TEXT("and describes the one authored row"), Resolved.ResolvedPoses.Num(), 1);

	// The row a validation message would have asked for, added exactly as the details panel adds one.
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Mounted"), MountedSequence));
	AnimSet->InvalidatePoseStateTable();

	TestFalse(TEXT("adding a pose row makes the table stale, with the bank untouched"),
		Resolved.MatchesSource(AnimSet, Bank));

	Resolved.Build(AnimSet, Bank);
	TestTrue(TEXT("rebuilding against the same bank matches again"), Resolved.MatchesSource(AnimSet, Bank));
	TestEqual(TEXT("and the new row is now describable"), Resolved.ResolvedPoses.Num(), 2);
	TestEqual(TEXT("and the value it names selects it through the resolved table"),
		static_cast<int32>(Resolved.PoseStateTable.Find(2)), 2);

	// The other property the table is derived from. Repointing it re-keys every row, and the bank is again
	// no help at all.
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceResolvedSwapped"),
		{{TEXT("Dead"), 5}, {TEXT("Mounted"), 6}});
	AnimSet->InvalidatePoseStateTable();

	TestFalse(TEXT("repointing the state enum makes the table stale too"),
		Resolved.MatchesSource(AnimSet, Bank));

	// A table built from no set at all describes no rows, so there is no revision for it to disagree with
	// and the bank is the whole of its identity.
	FCrowdyMassResolvedAnimSet NoSetTable;
	NoSetTable.Build(nullptr, Bank);
	TestTrue(TEXT("a table built from no set is judged by its bank alone"),
		NoSetTable.MatchesSource(nullptr, Bank));

	return true;
}

/**
 * One action id names one authored row whether an actor asks or the crowd renders it.
 *
 * This is what makes a single authored event body correct on every representation: the actor plays
 * FindActionSequence's clip and the crowd renders the slot pair on the same row, so neither side chooses
 * its own animation. The id convention is the load-bearing half, and it is the same one
 * FindResolvedAction uses: id N is Actions[N-1].
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetActorResolvesSameRowTest,
	"CrowdySDK.Mass.AnimSetActorResolvesTheSameActionRow", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetActorResolvesSameRowTest::RunTest(const FString& Parameters)
{
	UAnimSequence* AttackSequence = NewTestSequence();
	UAnimSequence* EmoteSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

	FCrowdyMassActionEntry Attack = MakeActionEntry(TEXT("Attack"), 0, 1);
	Attack.Sequence = TSoftObjectPtr<UAnimSequence>(AttackSequence);
	AnimSet->Actions.Add(Attack);

	FCrowdyMassActionEntry Emote = MakeActionEntry(TEXT("Emote"), 2, 3);
	Emote.Sequence = TSoftObjectPtr<UAnimSequence>(EmoteSequence);
	AnimSet->Actions.Add(Emote);

	// A crowd-only row: rendered from its slots, with nothing for an actor to play.
	AnimSet->Actions.Add(MakeActionEntry(TEXT("CrowdOnly"), 4, 5));

	// The id an action is SENT as, taken from the set rather than hard-coded, so this asserts the two
	// halves against each other rather than against a constant that could drift from either.
	const int32 AttackId = AnimSet->FindActionId(TEXT("Attack"));
	const int32 EmoteId = AnimSet->FindActionId(TEXT("Emote"));
	const int32 CrowdOnlyId = AnimSet->FindActionId(TEXT("CrowdOnly"));

	TestEqual(TEXT("Attack is action id 1"), AttackId, 1);
	TestEqual(TEXT("Emote is action id 2"), EmoteId, 2);

	TestTrue(TEXT("an actor resolves the first row's own clip"),
		AnimSet->FindActionSequence(AttackId) == AttackSequence);
	TestTrue(TEXT("and the second row's, rather than the first's"),
		AnimSet->FindActionSequence(EmoteId) == EmoteSequence);

	// The whole reason the ids are read off the set above: the crowd's renderer resolves the same ids
	// through its own table, and both must land on the same row.
	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, TArray<UAnimSequence*>{AttackSequence, AttackSequence, EmoteSequence, EmoteSequence});

	const FCrowdyMassResolvedAction* CrowdAttack = Resolved.FindResolvedAction(AttackId);
	if (!TestNotNull(TEXT("the crowd resolves the same attack id"), CrowdAttack))
	{
		return false;
	}
	TestEqual(TEXT("and lands on the slot holding the very clip the actor plays"),
		CrowdAttack->PrimaryBankIndex, 0);

	TestNull(TEXT("a crowd-only row gives an actor nothing to play"),
		AnimSet->FindActionSequence(CrowdOnlyId));
	TestNull(TEXT("id 0 names no action, since it is the fragment's nothing-playing value"),
		AnimSet->FindActionSequence(0));
	TestNull(TEXT("nor does an id past the authored rows"),
		AnimSet->FindActionSequence(AnimSet->Actions.Num() + 1));
	TestNull(TEXT("nor a negative one"), AnimSet->FindActionSequence(-1));

	return true;
}

/**
 * A row whose actor clip and crowd slots are different animations is reported.
 *
 * The one failure this design can produce that nothing else would catch: both halves resolve perfectly
 * well on their own, and the only thing wrong is that they disagree. Left unreported it shows up as one
 * player seeing a swing while another sees an emote for the same event.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetReportsActorCrowdClipDriftTest,
	"CrowdySDK.Mass.AnimSetReportsActorCrowdClipDrift", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassAnimSetReportsActorCrowdClipDriftTest::RunTest(const FString& Parameters)
{
	UAnimSequence* SlotSequence = NewTestSequence();
	UAnimSequence* DifferentSequence = NewTestSequence();

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

	FCrowdyMassActionEntry Agreeing = MakeActionEntry(TEXT("Agreeing"), 0, 1);
	Agreeing.Sequence = TSoftObjectPtr<UAnimSequence>(SlotSequence);
	AnimSet->Actions.Add(Agreeing);

	const TArray<UAnimSequence*> Bank = {SlotSequence, SlotSequence};

	// Asserted first, and this is what keeps the case honest: the warning below has to be caused by the
	// disagreement and not by anything else about the row, so the identical row with a matching clip must
	// validate clean.
	TestTrue(TEXT("a row whose clip matches its slots validates clean"), AnimSet->ValidateAgainstBank(Bank));

	AnimSet->Actions[0].Sequence = TSoftObjectPtr<UAnimSequence>(DifferentSequence);

	AddExpectedErrorPlain(TEXT("names sequence"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("the same row with a different clip from its slots does not"),
		AnimSet->ValidateAgainstBank(Bank));

	// An unset clip is a crowd-only action, not a mistake: every project authored before the field
	// existed has these, and warning for each would bury the disagreement above in noise.
	AnimSet->Actions[0].Sequence = nullptr;
	TestTrue(TEXT("and a row that authored no actor clip at all is silent"),
		AnimSet->ValidateAgainstBank(Bank));

	return true;
}

/**
 * Both representations clamp an action's play rate through one function.
 *
 * The rate arrives off the network and is forgeable. An actor clamping differently from the fragment
 * write, or not at all, is the same event playing at two speeds depending only on how a client happens to
 * be drawing the entity.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionPlayRateClampIsSharedTest,
	"CrowdySDK.Mass.ActionPlayRateClampIsShared", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassActionPlayRateClampIsSharedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an ordinary rate passes through"), CrowdyMassClampActionPlayRate(1.5f), 1.5f);
	TestEqual(TEXT("zero is lifted to the floor, not left to freeze the clip"),
		CrowdyMassClampActionPlayRate(0.0f), CrowdyMassMinActionPlayRate);
	TestEqual(TEXT("a huge rate is brought to the ceiling"),
		CrowdyMassClampActionPlayRate(1000.0f), CrowdyMassMaxActionPlayRate);
	TestEqual(TEXT("a negative rate is lifted to the floor"),
		CrowdyMassClampActionPlayRate(-4.0f), CrowdyMassMinActionPlayRate);

	// Replaced rather than clamped: NaN compares false against both bounds, so a clamp returns it
	// unchanged and it would reach the renderer intact.
	TestEqual(TEXT("a non-finite rate is replaced with the ordinary one"),
		CrowdyMassClampActionPlayRate(std::numeric_limits<float>::quiet_NaN()), 1.0f);
	TestEqual(TEXT("and so is an infinite one"),
		CrowdyMassClampActionPlayRate(std::numeric_limits<float>::infinity()), 1.0f);

	// The fragment write must agree with it, since that is the representation the actor is matching.
	FCrowdyMassActionFragment Action;
	CrowdyMassWriteAction(Action, 1, 0, 1000.0f);
	TestEqual(TEXT("the fragment write applies the very same clamp"),
		Action.PlayRate, CrowdyMassClampActionPlayRate(1000.0f));

	return true;
}

/**
 * A pose is selected by the value of the game's own state enum, not by the row's position.
 *
 * The rows are deliberately authored in an order that does not match their values, and every row
 * differs in every field a resolved pose carries, so a table built by treating a value as an index, or
 * by looking each row's name up in the array instead of reading the row being filled, would hand back a
 * pose that belongs to a different row and be caught here rather than on screen.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseIdFromStateValueTest,
	"CrowdySDK.Mass.PoseIdFromStateValue", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseIdFromStateValueTest::RunTest(const FString& Parameters)
{
	UAnimSequence* SitSequence = NewTestSequence();
	UAnimSequence* DeadSequence = NewTestSequence();
	UAnimSequence* NormalSequence = NewTestSequence();

	UEnum* StateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStance"),
		{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}, {TEXT("Sitting"), 2}, {TEXT("Downed"), 9}});

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = StateEnum;

	// Authored value 2 first, then 1, then 0, so row position and state value disagree everywhere.
	FCrowdyMassPoseEntry Sit = MakePoseEntry(TEXT("Sitting"), SitSequence, ECrowdyMassPoseRank::BelowActions, false);
	Sit.PlayRate = 1.0f;
	FCrowdyMassPoseEntry Dead = MakePoseEntry(TEXT("Dead"), DeadSequence, ECrowdyMassPoseRank::AboveActions, true);
	Dead.PlayRate = 0.5f;
	FCrowdyMassPoseEntry Normal = MakePoseEntry(TEXT("Normal"), NormalSequence, ECrowdyMassPoseRank::BelowActions, false);
	Normal.PlayRate = 2.0f;
	AnimSet->Poses.Add(Sit);
	AnimSet->Poses.Add(Dead);
	AnimSet->Poses.Add(Normal);

	const TArray<UAnimSequence*> Bank = {NormalSequence, DeadSequence, SitSequence};

	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, Bank);

	// Each value selects the row that names it, and that row carries its OWN clip, rank and play rate.
	{
		const int32 SitId = AnimSet->FindPoseIdByStateValue(2);
		TestEqual(TEXT("state value 2 selects the row naming it"), SitId, 1);

		const FCrowdyMassResolvedPose* Pose = Resolved.FindResolvedPose(static_cast<uint8>(SitId));
		if (TestNotNull(TEXT("and that id resolves"), Pose))
		{
			TestEqual(TEXT("to its own clip"), Pose->BankIndex, 2);
			TestEqual(TEXT("its own play rate"), Pose->PlayRate, 1.0f);
			TestTrue(TEXT("and its own rank"), Pose->Rank == ECrowdyMassPoseRank::BelowActions);
		}
	}

	{
		const int32 DeadId = AnimSet->FindPoseIdByStateValue(1);
		TestEqual(TEXT("state value 1 selects the second row"), DeadId, 2);

		const FCrowdyMassResolvedPose* Pose = Resolved.FindResolvedPose(static_cast<uint8>(DeadId));
		if (TestNotNull(TEXT("and that id resolves"), Pose))
		{
			TestEqual(TEXT("to its own clip"), Pose->BankIndex, 1);
			TestEqual(TEXT("its own play rate"), Pose->PlayRate, 0.5f);
			TestTrue(TEXT("its own rank"), Pose->Rank == ECrowdyMassPoseRank::AboveActions);
			TestTrue(TEXT("and its own loop mode"), Pose->LoopMode == EAnimSequenceTrackLoopMode::Clamp);
		}
	}

	// Value 0 is an ordinary authorable value and is not the sentinel: a game enum's 0 is normally its
	// default, so a row has to be able to name it. The "no pose" sentinel lives in the id space.
	{
		const int32 NormalId = AnimSet->FindPoseIdByStateValue(0);
		TestEqual(TEXT("state value 0 is authorable and selects the row naming it"), NormalId, 3);

		const FCrowdyMassResolvedPose* Pose = Resolved.FindResolvedPose(static_cast<uint8>(NormalId));
		if (TestNotNull(TEXT("and that id resolves"), Pose))
		{
			TestEqual(TEXT("to its own clip"), Pose->BankIndex, 0);
			TestEqual(TEXT("and its own play rate"), Pose->PlayRate, 2.0f);
		}
	}

	// An arriving value no row authors clears the pose rather than selecting anything, which is how a
	// living player needs no authored row at all.
	TestEqual(TEXT("an enum value no row authors clears the pose"), AnimSet->FindPoseIdByStateValue(9), 0);
	TestEqual(TEXT("a value that is not an entry of the enum clears it too"),
		AnimSet->FindPoseIdByStateValue(7), 0);
	TestEqual(TEXT("so does the largest value a byte can carry"), AnimSet->FindPoseIdByStateValue(255), 0);

	// Values a one-byte replicated state cannot carry can never arrive, and must answer "no pose"
	// rather than reading outside the table.
	TestEqual(TEXT("a value past the byte range clears rather than reading out of bounds"),
		AnimSet->FindPoseIdByStateValue(256), 0);
	TestEqual(TEXT("and so does a negative one"), AnimSet->FindPoseIdByStateValue(-1), 0);
	TestEqual(TEXT("and one far outside it"), AnimSet->FindPoseIdByStateValue(1000000), 0);

	// The table the resolved set carries is what a Mass-side reader uses, and it must agree with the
	// asset's own answer for every value or the two representations disagree about the same state.
	for (int64 StateValue = 0; StateValue < FCrowdyMassPoseStateTable::ValueCount; ++StateValue)
	{
		if (Resolved.PoseStateTable.Find(StateValue) == static_cast<uint8>(AnimSet->FindPoseIdByStateValue(StateValue)))
		{
			continue;
		}

		AddError(FString::Printf(TEXT("state value %lld resolves to id %d through the resolved table and %d through the asset"),
			StateValue, static_cast<int32>(Resolved.PoseStateTable.Find(StateValue)),
			AnimSet->FindPoseIdByStateValue(StateValue)));
		break;
	}

	TestEqual(TEXT("the resolved table maps exactly the three authored values"),
		Resolved.PoseStateTable.CountMappedValues(), 3);

	// A set that names no enum can still be driven by name, and must simply answer "no pose" to every
	// arriving value rather than guessing.
	{
		UCrowdyMassAnimSet* NoEnumSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		NoEnumSet->Poses.Add(MakePoseEntry(TEXT("Dead"), DeadSequence));

		TestEqual(TEXT("a set with no state enum still finds the row by name"),
			NoEnumSet->FindPoseId(TEXT("Dead")), 1);
		TestEqual(TEXT("but no arriving value selects it"), NoEnumSet->FindPoseIdByStateValue(0), 0);
		TestEqual(TEXT("at any value"), NoEnumSet->FindPoseIdByStateValue(1), 0);
	}

	return true;
}

/**
 * Two rows naming the same enum entry each keep their own data.
 *
 * The value selects the FIRST row naming it, matching what FindPoseId does with a duplicated name so
 * one state never means two different poses. What must not happen is the second row inheriting the
 * first's clip and playback: a table built by looking each row's name up in the array again would give
 * every duplicate the first match's data while it kept its own rank, producing a pose that exists in no
 * authored row.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDuplicateStateNamesResolveFromOwnRowTest,
	"CrowdySDK.Mass.DuplicateStateNamesResolveFromOwnRow", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassDuplicateStateNamesResolveFromOwnRowTest::RunTest(const FString& Parameters)
{
	UAnimSequence* AliveSequence = NewTestSequence();
	UAnimSequence* FirstDeadSequence = NewTestSequence();
	UAnimSequence* SecondDeadSequence = NewTestSequence();

	UEnum* StateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestLife"),
		{{TEXT("Alive"), 0}, {TEXT("Dead"), 1}});

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = StateEnum;
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Alive"), AliveSequence));

	FCrowdyMassPoseEntry FirstDead = MakePoseEntry(TEXT("Dead"), FirstDeadSequence, ECrowdyMassPoseRank::AboveActions, true);
	FirstDead.PlayRate = 0.25f;
	FCrowdyMassPoseEntry SecondDead = MakePoseEntry(TEXT("Dead"), SecondDeadSequence, ECrowdyMassPoseRank::BelowActions, false);
	SecondDead.PlayRate = 3.0f;
	AnimSet->Poses.Add(FirstDead);
	AnimSet->Poses.Add(SecondDead);

	const TArray<UAnimSequence*> Bank = {AliveSequence, FirstDeadSequence, SecondDeadSequence};

	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, Bank);

	TestEqual(TEXT("the state value selects the first row naming it"), AnimSet->FindPoseIdByStateValue(1), 2);
	TestEqual(TEXT("which is the row a by-name lookup also picks"), AnimSet->FindPoseId(TEXT("Dead")), 2);

	const FCrowdyMassResolvedPose* Selected = Resolved.FindResolvedPose(2);
	if (TestNotNull(TEXT("the selected row resolves"), Selected))
	{
		TestEqual(TEXT("and carries its own clip"), Selected->BankIndex, 1);
		TestEqual(TEXT("and its own play rate"), Selected->PlayRate, 0.25f);
	}

	// The row the value did not pick is still an addressable id, and still describes itself rather than
	// the row that won the value.
	const FCrowdyMassResolvedPose* Shadowed = Resolved.FindResolvedPose(3);
	if (TestNotNull(TEXT("the shadowed row is still an addressable id"), Shadowed))
	{
		TestEqual(TEXT("carrying its OWN clip, not the first row's"), Shadowed->BankIndex, 2);
		TestEqual(TEXT("its own play rate"), Shadowed->PlayRate, 3.0f);
		TestTrue(TEXT("and its own rank"), Shadowed->Rank == ECrowdyMassPoseRank::BelowActions);
	}

	// Only one row can own a value, so the duplicate does not add a second mapped value.
	TestEqual(TEXT("two rows on one value map one value"), Resolved.PoseStateTable.CountMappedValues(), 2);

	return true;
}

/**
 * The value-to-row answer is dropped wherever the rows can change, MISSES INCLUDED.
 *
 * A kept miss is the worse half and the reason this is asserted in that order: a value asked for before
 * its row existed would otherwise keep answering "no pose" for the life of the world, so adding the row
 * a validation message just asked for would change nothing at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseStateTableInvalidationTest,
	"CrowdySDK.Mass.PoseStateTableInvalidation", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseStateTableInvalidationTest::RunTest(const FString& Parameters)
{
	UEnum* StateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceCache"),
		{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}, {TEXT("Mounted"), 2}});

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = StateEnum;
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence()));

	TestEqual(TEXT("the authored row answers"), AnimSet->FindPoseIdByStateValue(1), 1);

	// The miss that has to be dropped: asked for before any row named it.
	TestEqual(TEXT("a value no row names answers nothing"), AnimSet->FindPoseIdByStateValue(2), 0);

	AnimSet->Poses.Add(MakePoseEntry(TEXT("Mounted"), NewTestSequence()));
	AnimSet->InvalidatePoseStateTable();

	TestEqual(TEXT("and once its row is added and the table dropped, the same value answers that row"),
		AnimSet->FindPoseIdByStateValue(2), 2);
	TestEqual(TEXT("without disturbing the row that already answered"), AnimSet->FindPoseIdByStateValue(1), 1);

	// Repointing the set at a different enum re-keys every row, so the old values must stop answering.
	// The table is derived from the enum as much as from the rows, and dropping it for one but not the
	// other would leave a value meaning whatever it meant under the enum that is no longer named.
	UEnum* OtherEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceSwapped"),
		{{TEXT("Mounted"), 4}, {TEXT("Dead"), 5}});
	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = OtherEnum;
	AnimSet->InvalidatePoseStateTable();

	TestEqual(TEXT("a value that meant something under the old enum stops answering"),
		AnimSet->FindPoseIdByStateValue(1), 0);
	TestEqual(TEXT("and the new enum's value for the same row answers instead"),
		AnimSet->FindPoseIdByStateValue(5), 1);
	TestEqual(TEXT("as does the new enum's value for the row added above"),
		AnimSet->FindPoseIdByStateValue(4), 2);

	return true;
}

/**
 * The dropdown a pose row offers lists the state enum's own entries.
 *
 * Without it a row is free text that has to match an enum entry exactly, and a typo is invisible until
 * a state arrives in a live session and nothing happens.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseStateOptionsTest,
	"CrowdySDK.Mass.PoseStateOptions", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseStateOptionsTest::RunTest(const FString& Parameters)
{
	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

	TestEqual(TEXT("a set naming no enum offers nothing rather than a stale list"),
		AnimSet->GetPoseStateOptions().Num(), 0);

	// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
	// not inferred from which one happens to be filled in.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceOptions"),
		{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}, {TEXT("Mounted"), 2}});

	const TArray<FPropertyTextFName> Options = AnimSet->GetPoseStateOptions();

	// The generated _MAX entry is a count rather than a state, so offering it would let an author pick a
	// value nothing can ever be in.
	TestEqual(TEXT("one option per authored entry, with the generated maximum left out"), Options.Num(), 3);

	TArray<FName> StoredNames;
	for (const FPropertyTextFName& Option : Options)
	{
		StoredNames.Add(Option.ValueString);

		// Every option has to show something, or the dropdown is a column of blanks. For a native enum
		// this is the entry name in words; for a Blueprint Enumeration it is the name the author typed,
		// which is the whole reason the value and the text are carried separately.
		TestFalse(*FString::Printf(TEXT("the option stored as '%s' shows a name"), *Option.ValueString.ToString()),
			Option.DisplayName.IsEmpty());
	}

	TestTrue(TEXT("stored as short entry names, which is what a row stores"), StoredNames.Contains(FName(TEXT("Normal"))));
	TestTrue(TEXT("every entry is offered"),
		StoredNames.Contains(FName(TEXT("Dead"))) && StoredNames.Contains(FName(TEXT("Mounted"))));

	// The names the dropdown offers have to be exactly the names resolution accepts, or an author who
	// picks from the list still authors a row nothing can select. Asserted by authoring one row per
	// offered name and asking for each by the value that name stands for.
	for (const FName& StoredName : StoredNames)
	{
		AnimSet->Poses.Add(MakePoseEntry(StoredName, NewTestSequence()));
	}
	AnimSet->InvalidatePoseStateTable();

	for (int32 OptionIndex = 0; OptionIndex < StoredNames.Num(); ++OptionIndex)
	{
		const int64 StateValue = AnimSet->PoseStateEnum->GetValueByNameString(StoredNames[OptionIndex].ToString());
		TestEqual(*FString::Printf(TEXT("a row authored from the offered name '%s' is selected by its value"), *StoredNames[OptionIndex].ToString()),
			AnimSet->FindPoseIdByStateValue(StateValue), OptionIndex + 1);
	}

	return true;
}

/**
 * A C++ state enum is reachable, which the object property beside it cannot make it.
 *
 * The state a game replicates is an ordinary UPROPERTY on its character, so its type is a native
 * `enum class E : uint8`. A native enum is not an asset, and the details panel fills an object property
 * from the asset registry, so it can never be picked there: without the name field the rows of a set
 * meant for the wire could not name their enum at all, every row would resolve to nothing, and the asset
 * would validate with a remedy the editor cannot perform.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseStateEnumByNameTest,
	"CrowdySDK.Mass.PoseStateEnumNamedInCode", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseStateEnumByNameTest::RunTest(const FString& Parameters)
{
	// Named rather than assigned, exactly as an author would type it, and looked up the same way the asset
	// does. A runtime-built enum stands in for a native one here: both are objects with a reflected name
	// and neither is an asset, which is the property that matters.
	UEnum* NamedEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceByName"),
		{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}});

	UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
	AnimSet->PoseStateEnumName = NamedEnum->GetName();
	AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence()));

	TestSamePtr(TEXT("the named enum is the set's state enum"), AnimSet->GetPoseStateEnum(),
		static_cast<const UEnum*>(NamedEnum));
	TestEqual(TEXT("and an arriving value selects the row naming it"), AnimSet->FindPoseIdByStateValue(1), 1);
	TestEqual(TEXT("while a value no row names still clears"), AnimSet->FindPoseIdByStateValue(0), 0);

	// The dropdown has to come off the same enum, or an author picks from one list and the wire resolves
	// against another.
	TestEqual(TEXT("the dropdown offers that enum's entries"), AnimSet->GetPoseStateOptions().Num(), 2);

	// The SOURCE decides which field is read, and nothing else does. It used to be the name winning
	// whenever it was non-empty, which is what made the details panel show two settings for one decision
	// with the wrong one looking canonical. Both fields stay filled in throughout below, so every
	// assertion is about the source and never about one field having been emptied.
	UEnum* AssetEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceAsAsset"), {{TEXT("Dead"), 7}});
	AnimSet->PoseStateEnum = AssetEnum;
	AnimSet->InvalidatePoseStateTable();

	TestSamePtr(TEXT("assigning the asset changes nothing while the source still says by-name"),
		AnimSet->GetPoseStateEnum(), static_cast<const UEnum*>(NamedEnum));

	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
	AnimSet->InvalidatePoseStateTable();

	TestSamePtr(TEXT("switching the source to the asset makes the asset live"),
		AnimSet->GetPoseStateEnum(), static_cast<const UEnum*>(AssetEnum));
	TestEqual(TEXT("so the asset's value now selects the row"), AnimSet->FindPoseIdByStateValue(7), 1);
	TestEqual(TEXT("and the named enum's value no longer does"), AnimSet->FindPoseIdByStateValue(1), 0);

	// Back again, with the name never having been touched: switching source is reversible, which is why
	// the hidden field keeps its value rather than being cleared.
	AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::NativeByName;
	AnimSet->InvalidatePoseStateTable();

	TestSamePtr(TEXT("switching back restores the named enum, which was never cleared"),
		AnimSet->GetPoseStateEnum(), static_cast<const UEnum*>(NamedEnum));
	TestEqual(TEXT("and its value selects the row again"), AnimSet->FindPoseIdByStateValue(1), 1);

	// A name that resolves to nothing is the fault worth telling apart from naming no enum, because it
	// reads as configured. Asserted with the asset STILL assigned, so this cannot pass by falling back to
	// it: under the by-name source there is no fallback.
	AnimSet->PoseStateEnumName = TEXT("ECrowdyMassTestStanceThatDoesNotExistAnywhere");
	AnimSet->InvalidatePoseStateTable();

	TestNull(TEXT("a name that resolves to nothing is not an enum"), AnimSet->GetPoseStateEnum());
	TestEqual(TEXT("and no arriving value selects a row"), AnimSet->FindPoseIdByStateValue(1), 0);

	return true;
}

#if WITH_EDITOR
/**
 * A pose row that no arriving state can ever select is refused, with the remedy named.
 *
 * Three separate faults, each individually invisible: a row naming an entry the set's enum does not
 * have, a row naming an entry whose value is wider than the one byte a replicated state travels in, and
 * two entries that share a value so only the first row can be selected.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseStateValidationTest,
	"CrowdySDK.Mass.PoseStateValidation", CrowdyMassAnimSetTestFlags)
bool FCrowdyMassPoseStateValidationTest::RunTest(const FString& Parameters)
{
	{
		// A well authored set has to pass cleanly, or the checks below are just noise.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceClean"),
			{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}});
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence(), ECrowdyMassPoseRank::AboveActions, true));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("a row naming an entry of the set's own enum reports no errors"),
			static_cast<int32>(Context.GetNumErrors()), 0);
	}

	{
		// A name that is not an entry of the enum can only be reached from local code, never from the
		// wire, and nothing else in the project says so.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceUnknown"),
			{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}});
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Knocked"), NewTestSequence()));

		FDataValidationContext Context;
		TestTrue(TEXT("a row naming no entry of the enum fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once against that row"), static_cast<int32>(Context.GetNumErrors()), 1);
	}

	{
		// An enum value outside the byte range cannot reach a byte-wide replicated field at all, so the
		// row is dead however well authored the rest of it is.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceWide"),
			{{TEXT("Normal"), 0}, {TEXT("Distant"), 300}}, UEnum::EUnderlyingType::int32);
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Distant"), NewTestSequence()));

		FDataValidationContext Context;
		TestTrue(TEXT("a row naming a value wider than a byte fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once against that row"), static_cast<int32>(Context.GetNumErrors()), 1);

		// And the row genuinely cannot be selected, so the message is describing the runtime rather than
		// warning about something that still works.
		TestEqual(TEXT("and the value it names selects nothing"), AnimSet->FindPoseIdByStateValue(300), 0);
	}

	{
		// Two entries sharing one value is legal in C++ and means the second row can never be selected
		// by a state, which is invisible in the details panel because both rows are well formed.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceAliased"),
			{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}, {TEXT("Destroyed"), 1}});
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence()));
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Destroyed"), NewTestSequence()));

		FDataValidationContext Context;
		TestTrue(TEXT("two rows on one enum value fail validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once, against the row that cannot be reached"),
			static_cast<int32>(Context.GetNumErrors()), 1);
	}

	{
		// Pose rows with no enum to key them is an ERROR, and the promotion from a warning is the point of
		// this case. A replicated state value is the only thing that selects a pose row on a crowd entity,
		// so rows with no enum are rows nothing on the wire can reach: the set reads as fully authored and
		// poses nothing. As a warning it failed no validation run and blocked no cook, which is exactly how
		// the one anim set in this project sat in this state while every test stayed green.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence()));

		FDataValidationContext Context;
		TestTrue(TEXT("a set with rows and no state enum fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("and says so exactly once"), static_cast<int32>(Context.GetNumErrors()), 1);
	}

	{
		// A named enum that resolves to nothing reads as configured while behaving exactly like naming no
		// enum at all, which is why it is an error rather than the warning above: the name is the thing to
		// go and fix, and nothing else in the project points at it.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->PoseStateEnumName = TEXT("ECrowdyMassTestStanceNobodyDeclared");
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence()));

		FDataValidationContext Context;
		TestTrue(TEXT("a state enum name that resolves to nothing fails validation"),
			AnimSet->IsDataValid(Context) == EDataValidationResult::Invalid);
		TestEqual(TEXT("reported once, against the name"), static_cast<int32>(Context.GetNumErrors()), 1);
		TestEqual(TEXT("and not also as the no-enum-named warning, which would be the wrong remedy"),
			static_cast<int32>(Context.GetNumWarnings()), 0);
	}

	{
		// A row on the state an entity rests in stops the whole crowd from ever showing a gait: a resolved
		// pose always outranks locomotion, so gait selection keeps running with its answer discarded. Every
		// other check passes, the row resolves, the id is right and the bank slots do not collide, so this
		// is invisible until someone watches a crowd fail to walk.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceResting"),
			{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}});
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Normal"), NewTestSequence()));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("a row on the resting state is not an error, since a one-clip crowd is authorable"),
			static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("but it is warned about once"), static_cast<int32>(Context.GetNumWarnings()), 1);

		// And the row it warns about genuinely does resolve, so the warning is describing what will happen
		// rather than guarding against something that already fails.
		TestEqual(TEXT("and the resting value really does select that row"),
			AnimSet->FindPoseIdByStateValue(0), 1);
	}

	{
		// The other side of that pair: a set that leaves the resting state unauthored, which is what the
		// contract expects and what a living player relies on, must stay silent. Without this the warning
		// above could be firing on every set and nobody would notice.
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		// The asset route, said explicitly: which of the two fields is live is the author's own choice now,
		// not inferred from which one happens to be filled in.
		AnimSet->PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::BlueprintAsset;
		AnimSet->PoseStateEnum = MakeTestStateEnum(TEXT("ECrowdyMassTestStanceRestingUnauthored"),
			{{TEXT("Normal"), 0}, {TEXT("Dead"), 1}});
		AnimSet->Poses.Add(MakePoseEntry(TEXT("Dead"), NewTestSequence(), ECrowdyMassPoseRank::AboveActions, true));

		FDataValidationContext Context;
		AnimSet->IsDataValid(Context);
		TestEqual(TEXT("leaving the resting state unauthored raises no error"),
			static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("and no warning either"), static_cast<int32>(Context.GetNumWarnings()), 0);
	}

	return true;
}
#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
