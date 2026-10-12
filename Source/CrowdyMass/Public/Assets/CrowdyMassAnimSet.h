#pragma once

#include "Animation/AnimSequenceTransformProviderData.h"
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/PropertyText.h"
#include "CrowdyMassAnimSet.generated.h"

class UAnimSequence;
class UEnum;

/**
 * How many gaits one anim set may author.
 *
 * A gait is selected by a one-byte row id whose 0 means "no gait resolved", so the last reachable row
 * is 255. Rows past this are never given an id, because id 256 truncates to 0 and 0 is the value that
 * means no gait at all.
 */
inline constexpr int32 CrowdyMassMaxGaits = 255;

/**
 * One locomotion gait, named by the author rather than chosen from a fixed list, so a game can have as
 * many as it likes and call them whatever it likes.
 *
 * ROW ORDER IS LOAD-BEARING: author the rows slowest first. Gait selection walks them by position to
 * decide whether an entity has sped up into a faster gait or slowed out of its current one, so a new
 * gait goes at its speed position in the array, not on the end. The first row is the floor every other
 * gait falls back to and is entered by having nowhere slower to go.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassGaitEntry
{
	GENERATED_BODY()

	// What this gait is called. Used for authoring, validation messages and diagnostics; selection
	// itself works off the row's position and never searches by name.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	FName GaitName;

	// Must already be present in the shared instanced-skinned-mesh animation bank this entity
	// renders through; resolution matches this asset by pointer, it does not add sequences to the
	// bank itself.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	TSoftObjectPtr<UAnimSequence> Sequence;

	// Speed (using the entity's smoothed speed, never raw velocity) above which gait selection may
	// enter this gait. Ignored for the first row, which is the floor every other gait falls back to.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim", meta=(ClampMin="0.0"))
	float EnterSpeed = 0.0f;

	// Speed below which gait selection leaves this gait for the one below it. Author it below
	// EnterSpeed: the gap between the two is the whole hysteresis band, and a gap of zero means an
	// entity hovering exactly at the threshold switches back and forth every frame.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim", meta=(ClampMin="0.0"))
	float ExitSpeed = 0.0f;

	// Playback rate for this gait's sequence, applied once at the moment the gait is entered. It
	// cannot be changed continuously to match speed: the renderer only reads playback settings when
	// the pushed sequence index changes, so a rate written without an index change is discarded.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim", meta=(ClampMin="0.0"))
	float PlayRate = 1.0f;
};

/**
 * A one-shot action authored as two explicit bank slot indices that both reference the identical
 * sequence asset. This is load-bearing, not redundant: the engine only pushes a new animation when
 * the pushed sequence index changes, so retriggering the same one-shot back to back needs the
 * index to alternate between two duplicate slots even though they play the same asset. Asset-
 * pointer resolution cannot produce this on its own, since a pointer search naturally collapses
 * duplicates to their first match; the pair has to be authored explicitly instead.
 *
 * A state an entity holds rather than plays once is a pose, not an action: see FCrowdyMassPoseEntry.
 * A pose needs no slot pair, because nothing ever retriggers it.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassActionEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	FName ActionName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	int32 PrimaryIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	int32 AlternateIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	float PlayRate = 1.0f;

	/**
	 * The same sequence both slots above reference, named as an asset so a receiver holding this entity
	 * as a real ACTOR can play the identical clip.
	 *
	 * This row is the one place an action's animation is decided, for the crowd and for an actor alike.
	 * Without it the two sides pick their clip from different places and drift the moment either is
	 * re-authored, which is the whole reason it is here rather than each side choosing its own.
	 *
	 * The crowd does not read this: it renders through the explicit slot pair above, which it needs
	 * because retriggering the same action requires the pushed index to alternate. `ValidateAgainstBank`
	 * reports a row whose asset and slots disagree. Leaving it unset is allowed and means "crowd only":
	 * an actor asking for this action then finds nothing and says so.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	TSoftObjectPtr<UAnimSequence> Sequence;
};

/**
 * Where a pose sits against the one-shot actions an entity can play. This is the whole of what a
 * pose's precedence is, because locomotion is always the floor: a pose that resolves outranks the
 * gait either way, and the only real question is what happens when an action fires while the pose is
 * held.
 *
 * Authored per pose rather than fixed in code, which is what lets a game decide that its own states
 * outrank actions without this plugin knowing those states exist.
 */
UENUM(BlueprintType)
enum class ECrowdyMassPoseRank : uint8
{
	// A one-shot action plays over this pose and the pose comes back when the action finishes.
	// Right for a stance the entity can still act from: sitting, mounted, carrying.
	BelowActions,

	// This pose overrides one-shot actions for as long as it is set. Right for a state the entity
	// cannot act its way out of: a corpse, a knockdown, a stun.
	AboveActions
};

/**
 * One continuous pose an entity can be put into, held until something sets a different one.
 *
 * This is the extension point for any state a game has that this plugin has never heard of. Author a
 * row naming one entry of the game's own state enum, give it a rank, and replicate that enum value;
 * there is no fragment, processor or C++ type to add for it. Death is exactly this and nothing more: a
 * row naming the game's dead state, ranked AboveActions, holding its last frame.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassPoseEntry
{
	GENERATED_BODY()

	// The entry of the owning set's state enum this row stands for, offered as a dropdown of that enum's
	// entries. An arriving state value is matched against these names once, so the game decides what a
	// value means and this row decides what it looks like. A row naming an entry the enum does not have
	// can only be reached by name, never by an arriving value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim", meta=(GetOptions="GetPoseStateOptions"))
	FName PoseName;

	// Must already be present in the shared instanced-skinned-mesh animation bank this entity
	// renders through; resolution matches this asset by pointer, it does not add sequences to the
	// bank itself.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	TSoftObjectPtr<UAnimSequence> Sequence;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	ECrowdyMassPoseRank Rank = ECrowdyMassPoseRank::BelowActions;

	// Stops on the animation's last frame instead of looping it. Check this for a pose that stands
	// for an end state rather than an activity, a corpse above all: a death clip that loops replays
	// the fall over and over.
	//
	// Give a held pose a bank slot no gait or other pose uses. The renderer only reacts to the pushed
	// index changing, so a pose sharing a slot with the animation an entity comes back to would leave
	// it frozen on screen after the pose was cleared.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim")
	bool bHoldLastFrame = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim", meta=(ClampMin="0.0"))
	float PlayRate = 1.0f;
};

/**
 * How many poses one anim set may author.
 *
 * A pose is selected by a one-byte id whose 0 means "no pose", so the last reachable row is 255. This
 * is a hard limit rather than a guideline: row 256 would take id 256, which truncates to 0, and 0 is
 * the value that CLEARS the pose. Rows past this are refused where a name is turned into an id, so an
 * over-long array costs the rows past the limit rather than silently clearing whoever asks for one.
 */
inline constexpr int32 CrowdyMassMaxPoses = 255;

/**
 * Pose row ids indexed by the state value that names them, one entry for every value a byte can hold.
 *
 * Dense rather than a map because this is read once per arriving state delta per entity, and the state
 * arrives as a single byte, so the lookup has to be a bounds check and an array index with no hashing
 * and no name search in it. 0 at a value means no authored row names it, which clears the pose and
 * lets the entity fall through to whatever is below. That is how a living player needs no authored row.
 *
 * Value 0 is an ordinary authorable value and is NOT the sentinel: a game enum's 0 is normally its
 * default (Normal, Alive), so a row has to be able to name it. The "no pose" sentinel lives in the id
 * space instead, where 0 means no pose.
 */
struct FCrowdyMassPoseStateTable
{
	// Every value a byte-wide state can carry. A state enum whose values fall outside this range
	// cannot reach a byte-wide replicated field at all and is refused when the asset is validated.
	static constexpr int32 ValueCount = 256;

	uint8 PoseIds[ValueCount] = {};

	// The pose id a state value selects, or 0 for a value no row authors and for any value outside
	// the byte range a replicated state can carry.
	uint8 Find(int64 StateValue) const
	{
		return (StateValue >= 0 && StateValue < ValueCount) ? PoseIds[static_cast<int32>(StateValue)] : 0;
	}

	// How many state values select a pose row. Zero on a set whose rows name entries of no enum, or of
	// the wrong one, which is the difference between "the value never arrived" and "the value arrived
	// and this set has nothing to show for it".
	int32 CountMappedValues() const
	{
		int32 Count = 0;
		for (const uint8 PoseId : PoseIds)
		{
			Count += PoseId != 0 ? 1 : 0;
		}

		return Count;
	}
};

// Result of resolving a gait or pose entry against a supplied bank. bResolved is false,
// BankIndex is INDEX_NONE, and DurationSeconds is 0 when the authored sequence asset could not be
// found in the bank; callers fall back to the first gait row.
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassResolvedSequence
{
	GENERATED_BODY()

	// INDEX_NONE (-1) when unresolved. Do not write this straight into an unsigned bitfield without
	// checking bResolved first: an unchecked -1 truncates to the field's max value (for example
	// 1048575 in a 20-bit field), which looks like a valid bank index instead of an obviously invalid
	// one. Use GetBankIndexOrFallback below when the destination cannot represent "unresolved".
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	int32 BankIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	float DurationSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	bool bResolved = false;

	// True when this result is not the entry that was actually asked for, but the first gait row
	// substituted for it because the requested entry did not resolve. bResolved is still true in this
	// case; check this flag to tell a genuine resolution apart from "missing, you are looking at the
	// floor gait".
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	bool bFellBackToFloorGait = false;

	// Returns BankIndex when resolution succeeded, otherwise FallbackIndex. Prefer this over reading
	// BankIndex directly when writing into a destination that cannot represent a negative index.
	int32 GetBankIndexOrFallback(int32 FallbackIndex) const
	{
		return bResolved ? BankIndex : FallbackIndex;
	}
};

// Outcome of checking one action's slot pair against a bank. The two failure cases need different
// diagnoses: SameIndex means the pair can never retrigger (the engine only reacts to an index change),
// while DifferentAsset means the two slots do not even play the same animation.
enum class ECrowdyMassActionPairStatus : uint8
{
	Valid,
	SameIndex,
	DifferentAsset
};

// One action's slot pair and duration, already resolved against one bank. Both indices are
// INDEX_NONE when the action's pair did not resolve (unauthored, same index twice, or the two
// slots reference different assets), which the compose rule treats as unselectable rather than
// guessing at one half of a broken pair.
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassResolvedAction
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	int32 PrimaryBankIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	int32 AlternateBankIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Anim")
	float DurationSeconds = 0.0f;
};

/**
 * One gait row already resolved against one bank: everything selecting it needs, with no lookup left
 * to do.
 *
 * BankIndex is INDEX_NONE when the row is not authored or its sequence is absent from the bank. Such a
 * row also carries speeds above every real speed, so it can never be entered and is left the moment it
 * somehow becomes current: a partly authored set degrades to the gaits that do resolve rather than
 * freezing on a missing one.
 */
struct FCrowdyMassResolvedGait
{
	int32 BankIndex = INDEX_NONE;
	float EnterSpeed = TNumericLimits<float>::Max();
	float ExitSpeed = TNumericLimits<float>::Max();
	float PlayRate = 1.0f;
};

// One pose already resolved against one bank: everything selecting it needs, with no lookup left to
// do. BankIndex is INDEX_NONE when the authored sequence is absent from the bank, which the compose
// rule treats as no pose at all rather than showing nothing.
struct FCrowdyMassResolvedPose
{
	int32 BankIndex = INDEX_NONE;
	float PlayRate = 1.0f;
	ECrowdyMassPoseRank Rank = ECrowdyMassPoseRank::BelowActions;
	EAnimSequenceTrackLoopMode LoopMode = EAnimSequenceTrackLoopMode::Loop;
};

/**
 * Maps an archetype's logical animation states (gait, action, pose) to sequence indices in the
 * shared instanced-skinned-mesh animation bank that archetype renders through. One asset is shared
 * by every entity of that archetype through a const shared fragment; nothing on this class is
 * per-entity data.
 *
 * Resolution takes the bank contents as a parameter rather than fetching them from a live
 * subsystem, so it can run against a plain instance of this asset in a headless test with no world
 * and no real skinned mesh.
 *
 * ResolveGait, ResolvePose, ValidateAction, and GetActionDurationSeconds keep no state of
 * their own and log nothing. They never load anything either: an authored sequence is resolved by
 * looking for the already-loaded object, and a sequence that is not in memory simply does not
 * resolve. That is not a race waiting to happen, because a bank is built from a provider asset that
 * holds its sequences by hard reference, so a loaded bank means loaded sequences. It does mean these
 * are lookups against one bank and not a way to make one exist.
 */
/**
 * Which of the two ways a set names its pose state enum is in use.
 *
 * It exists for the details panel as much as for the code. A UEnum reaches an asset by two routes that
 * cannot be merged: an object property can hold a Blueprint Enumeration but can never hold an enum
 * declared in C++, because a native enum is not an asset and the picker is filled from the asset registry.
 * Offering both fields at once made the panel show two settings for one decision, with the picker looking
 * canonical while the ordinary case (a native enum) could only be reached through the other one.
 *
 * So the source is chosen explicitly and the field that does not apply is HIDDEN rather than merely
 * ignored. An author sees one enum field, and which kind it is was their own choice rather than something
 * to infer from which of two boxes happened to take a value.
 */
UENUM()
enum class ECrowdyMassPoseStateEnumSource : uint8
{
	// The ordinary case, and the default. A state a game replicates is a UPROPERTY on its character, so
	// its type is normally a native `enum class E : uint8`.
	NativeByName,

	// A Blueprint Enumeration asset, which can be picked directly because it is an asset.
	BlueprintAsset
};

#if WITH_EDITOR
/**
 * What Sync Actions Into Bank would do, worked out without doing any of it.
 *
 * Split from the commit deliberately. Everything decided here is a property of the authored rows and the
 * bank contents alone, while committing writes two assets and asks the engine to rebuild derived data for
 * a bank that grew, which is far too heavy to run just to find out whether anything needed doing.
 */
struct FCrowdyMassActionBankSync
{
	// The bank as it would be afterwards. Never shorter than the one that went in and never reordered:
	// every index already authored has to keep meaning what it meant.
	TArray<FAnimSequenceTransformProviderSequence> Bank;

	// Parallel to the anim set's Actions, holding the slot pair each row would end up with. A row that
	// is skipped keeps the pair it already has.
	TArray<TPair<int32, int32>> Pairs;

	// Rows that name no Sequence. Their pairs stay hand-authored, which is a legitimate way to author a
	// crowd-only action, so they are listed rather than treated as faults.
	TArray<FString> CrowdOnlyActionNames;

	// Rows naming a Sequence that would not load. Unlike the above this IS a fault, and the two are kept
	// apart because only one of them is something to go and fix.
	TArray<FString> UnloadableActionNames;

	int32 AppendedSlots = 0;

	// Rows whose pair this would actually MOVE. Not the rows it was allowed to consider: a run that fixes
	// one row among nine correct ones changed one, and a report saying ten reads as if it rewrote them all.
	int32 ChangedPairCount = 0;

	bool bPairsChanged = false;

	// Deliberately does NOT count the skipped rows. A row naming a clip that will not load is a fault this
	// cannot fix, so it is not work to do; it is reported separately, and BEFORE anything consults this,
	// because the case where such rows are all that happened is the case this returns false for.
	bool HasWork() const { return AppendedSlots > 0 || bPairsChanged; }
};
#endif

UCLASS(BlueprintType)
class CROWDYMASS_API UCrowdyMassAnimSet : public UDataAsset
{
	GENERATED_BODY()

public:
	// Authored slowest first: the row order decides which gait an entity speeds up into and which it
	// slows back down to. See FCrowdyMassGaitEntry.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Gaits")
	TArray<FCrowdyMassGaitEntry> Gaits;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Actions")
	TArray<FCrowdyMassActionEntry> Actions;

	/**
	 * The animation bank these rows are authored against, and the bank a class that renders through this
	 * set animates with when the SDK derives its config from the class. A config you author keeps its own
	 * mesh description and its own bank at runtime.
	 *
	 * Asset validation checks the rows against it and Sync Actions Into Bank fills the slot pairs in from
	 * it. Leaving it unset is allowed for an authored config, but a derived class then renders in bind pose.
	 *
	 * **Naming the WRONG bank changes what a derived class renders.** Sync Actions Into Bank writes
	 * PrimaryIndex and AlternateIndex, which index whichever bank the mesh description carries, and validation
	 * confirms the pairs against this same bank, so a wrong one reads green. Point it at the provider whose
	 * skinned asset is the mesh the class renders.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim|Bank")
	TSoftObjectPtr<UAnimSequenceTransformProviderData> Bank;

	/**
	 * Whether the state enum below is declared in C++ or is a Blueprint Enumeration asset. Picking one
	 * shows the field for it and hides the other, because it is one decision and not two settings.
	 *
	 * The enum itself is what lets a game put an entity into one of its own states without this plugin
	 * knowing those states exist: the game replicates an ordinary property of that enum type, and the value
	 * that arrives is matched against the Poses rows. Two classes mapping the same value to different clips
	 * is not a collision, it is per-class animation, because the meaning of a value is fixed by the enum
	 * rather than by a row position.
	 *
	 * Either way it must be byte-valued (in C++, `enum class E : uint8`). A value outside 0..255 cannot
	 * reach the one-byte replicated field the state travels in, and a row naming one is refused when this
	 * asset is validated.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim|Pose State",
		meta=(DisplayName="Pose State Enum Is"))
	ECrowdyMassPoseStateEnumSource PoseStateEnumSource = ECrowdyMassPoseStateEnumSource::NativeByName;

	/**
	 * The state enum this set's Poses rows are entries of, when it is declared in C++.
	 *
	 * Offered as a dropdown of every byte-wide enum compiled into the running build, so it is picked
	 * rather than typed: a mistyped name resolves to nothing and no arriving state value can then select
	 * any row. A full object path is still accepted for the case where two modules declare one name.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim|Pose State",
		meta=(DisplayName="Pose State Enum", GetOptions="GetPoseStateEnumNameOptions",
			EditCondition="PoseStateEnumSource == ECrowdyMassPoseStateEnumSource::NativeByName",
			EditConditionHides))
	FString PoseStateEnumName;

	/**
	 * The state enum this set's Poses rows are entries of, when it is a Blueprint Enumeration asset.
	 *
	 * A Blueprint Enumeration is always byte-wide, so only the C++ route can get the width wrong.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim|Pose State",
		meta=(DisplayName="Pose State Enum",
			EditCondition="PoseStateEnumSource == ECrowdyMassPoseStateEnumSource::BlueprintAsset",
			EditConditionHides))
	TObjectPtr<UEnum> PoseStateEnum;

	// Each row names one entry of the state enum chosen above, which is why the two sit together: the enum
	// is what gives every row here a meaning. Editing this array, PoseStateEnumSource, PoseStateEnum or
	// PoseStateEnumName from C++ after something has already asked for a pose id must be followed by
	// InvalidatePoseStateTable(); loading the asset and editing it in the details panel do that for you.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Pose State")
	TArray<FCrowdyMassPoseEntry> Poses;

	/**
	 * How long the smoothed speed gait selection reads takes to follow a change in the entity's
	 * actual speed. Must stay comfortably above one position-update interval; the default suits the
	 * 10 Hz updates remote entities arrive on.
	 *
	 * Why any smoothing at all, and why an averaging one specifically. A remote entity's velocity is
	 * not sampled, it is produced by interpolating from the last position update to the newest one
	 * over a fixed duration. It therefore reads the segment's speed for that duration and then
	 * exactly zero until the next update lands, whether or not the entity is still moving. The
	 * useful property of that shape is that its average over an interval is the entity's true
	 * average speed, whatever the update rate and whatever the frame rate, so an average is what
	 * gait selection wants.
	 *
	 * Following the peaks instead of the average is the trap, and it is not a small error: an
	 * entity whose updates arrive at half the expected rate covers twice the distance per update
	 * and so reads twice its real speed at the peak. Reading peaks would put a walking player into
	 * a run and back on every update. Keep this a plain symmetric average; do not make it react
	 * faster in one direction than the other.
	 *
	 * The cost of averaging is ripple, since the input alternates between a speed and zero. Author
	 * each gait's enter and exit speeds with a gap wider than that ripple, which is what the
	 * hysteresis band is for.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Gaits", meta=(ClampMin="0.0"))
	float SpeedSmoothingTimeSeconds = 0.3f;

	// Longest frame the smoothing will treat as real. A frame that hitches past this would otherwise
	// close almost the whole gap to the input in one step, and since every in-flight interpolation
	// also completes on that frame, the input is zero: without the ceiling one long frame drops the
	// entire crowd to the slowest gait row and pops it back the moment the next update lands.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Gaits", meta=(ClampMin="0.0"))
	float MaxSmoothingStepSeconds = 0.1f;

	// Seconds to blend over when the pushed sequence changes for something the entity holds rather than
	// plays once, which is a gait or a pose. Zero snaps.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Anim|Gaits", meta=(ClampMin="0.0"))
	float BlendSeconds = 0.2f;

	// Resolves the sequence asset of gait row GaitId (id N is Gaits[N-1]) by pointer against
	// BankSequences (index N in BankSequences is the sequence index the engine expects in
	// AutoPlayData.SequenceIndex). Id 0 is the "no gait" sentinel and returns an unresolved,
	// non-fallback result immediately. An unresolved row other than the first falls back to resolving
	// the first instead, with bFellBackToFloorGait set on the returned result. Does not log; call
	// ValidateAgainstBank once beforehand to surface authoring problems.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	FCrowdyMassResolvedSequence ResolveGait(int32 GaitId, const TArray<UAnimSequence*>& BankSequences) const;

	// Resolves a pose's sequence asset the same way ResolveGait does. NAME_None is the documented
	// sentinel for "no pose authored" (also the default key of a blank details-panel row), so it is
	// not a lookup: it returns an unresolved, non-fallback result immediately. Any other unknown or
	// unresolved PoseName falls back to the first gait row, with bFellBackToFloorGait set on the
	// returned result. Does not log. For authoring and tooling only; the per-entity path is
	// FCrowdyMassResolvedAnimSet::FindResolvedPose(uint8), a bounds check and an array index with no
	// name search in it.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	FCrowdyMassResolvedSequence ResolvePose(FName PoseName, const TArray<UAnimSequence*>& BankSequences) const;

	// Converts an authored pose name to the id FCrowdyMassViewPoseFragment::PoseId expects (id N
	// selects Poses[N-1], the same convention ActionId uses for Actions; 0 means none, including when
	// PoseName is NAME_None or unknown). This is how a game names the pose it wants without ever
	// handling an index: call it once, wherever a pose is chosen by name, and pass the id from there
	// on. Never per entity per frame.
	//
	// A row past CrowdyMassMaxPoses reports 0 and warns, rather than an id its one-byte destination
	// cannot hold: truncating instead would hand back 0 for row 256, and 0 clears the pose.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	int32 FindPoseId(FName PoseName) const;

	/**
	 * Converts an arriving PoseStateEnum value to the id FCrowdyMassViewPoseFragment::PoseId expects.
	 *
	 * This is the form the receive path uses: a game replicates a property of its own enum type, the
	 * value arrives as one byte, and this turns it into the row that value names, once per arriving
	 * delta rather than per frame.
	 *
	 * Value 0 is an ordinary value a row may name, because a game enum's 0 is normally its default. A
	 * value no row authors reports 0, which CLEARS the pose and drops the entity through to whatever is
	 * below, so a living player needs no authored row at all. So does a value outside the 0..255 a
	 * one-byte state can carry.
	 *
	 * Answers off a table built from the authored rows on first use, so this is a bounds check and an
	 * array index. Build it on the game thread, and call InvalidatePoseStateTable after changing Poses or
	 * either of the state enum properties from C++.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	int32 FindPoseIdByStateValue(int64 StateValue) const;

	// Drops the table FindPoseIdByStateValue answers from, so the next call rebuilds it from the
	// authored rows. Loading the asset and editing it in the details panel do this already; C++ that
	// writes Poses, PoseStateEnum or PoseStateEnumName directly has to. Dropping the WHOLE table rather
	// than one entry is deliberate: a value that resolved to nothing is as much an answer as one that
	// resolved to a row, so a kept miss would mean adding the row a warning just asked for still selected
	// nothing.
	//
	// It also advances GetSourceRevision, so a table resolved against the previous rows is rebuilt rather
	// than kept.
	void InvalidatePoseStateTable();

	/**
	 * The state enum this set's rows are entries of, whichever way it was named.
	 *
	 * PoseStateEnumName wins over PoseStateEnum, mirroring the engine's own pairing of the two: the object
	 * property can only ever hold a Blueprint Enumeration, so a name is the only way to reach the native
	 * enum a replicated state normally travels as. Null when neither names anything that resolves, which is
	 * a set that can still be driven by name and that no arriving value can select a row of.
	 *
	 * Resolving a name is a slow search, so the answer is memoised and dropped by InvalidatePoseStateTable
	 * along with everything else derived from the two properties.
	 */
	const UEnum* GetPoseStateEnum() const;

	/**
	 * A counter that moves whenever the authored rows or the state enum can have changed.
	 *
	 * Anything holding a table resolved from this asset compares this against what it recorded at the time.
	 * Comparing the animation bank alone is not enough: adding a pose row, retyping a gait's speeds or
	 * repointing the state enum all leave the bank untouched, and every index into a table built before the
	 * change would then name a row that is no longer there.
	 */
	uint32 GetSourceRevision() const { return SourceRevision; }

	// Converts an authored action name to the id FCrowdyMassActionFragment::ActionId expects (id N
	// selects Actions[N-1]; 0 means none, including when ActionName is NAME_None or unknown). The
	// counterpart to FindPoseId, and used the same way: once, off the per-entity path.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	int32 FindActionId(FName ActionName) const;

	// True if the action's PrimaryIndex and AlternateIndex are two distinct indices that both
	// reference the same asset in BankSequences. Identical indices are rejected even when they would
	// resolve to the same asset, since retriggering the action depends on the pushed index changing.
	// Does not log; call ValidateAgainstBank once beforehand to surface authoring problems.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	bool ValidateAction(FName ActionName, const TArray<UAnimSequence*>& BankSequences) const;

	/**
	 * The sequence an ACTOR should play for ActionId, which is the same row the crowd renders.
	 *
	 * ActionId N is Actions[N-1], the convention the wire and FCrowdyMassResolvedAnimSet already use, so
	 * one id names one row on every representation. Null when the id names no row, or when that row
	 * authored no asset (a crowd-only action).
	 *
	 * The row's own PlayRate is deliberately NOT returned. For an action the crowd uses the play rate the
	 * EVENT carried, not the row's, so an actor reading the row's rate here would be the one
	 * representation playing at a different speed. Pass the event's rate through
	 * CrowdyMassClampActionPlayRate instead, exactly as the fragment write does.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	UAnimSequence* FindActionSequence(int32 ActionId) const;

	// Playback duration of an action's underlying asset, read through its PrimaryIndex slot in
	// BankSequences. Lets a later phase assert that a near-tier montage and this far-tier sequence
	// agree on duration. Returns 0 if the action is unknown or PrimaryIndex does not resolve.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	float GetActionDurationSeconds(FName ActionName, const TArray<UAnimSequence*>& BankSequences) const;

	// Walks the Gait, Action and Pose entries and logs a warning for each one that fails to resolve
	// against BankSequences (an action's slot pair counts as failing if the indices are identical, sit
	// outside the bank, or resolve to different assets). Call this once on the game thread whenever a
	// bank is assigned to an entity, not per entity per frame. Returns false if anything failed.
	//
	// Not linear: the pose collision pass below compares every pose against every gait, every later pose
	// and every action, and resolving a row scans the bank. Cheap at authored sizes and worth knowing
	// before anyone puts it somewhere hot.
	//
	// It also reports a pose that lands on a slot a gait, another pose, or an action already occupies.
	// That is not a resolution failure and each entry involved is individually correct, which is why it
	// needs the bank to see at all: the renderer only reacts to the pushed index CHANGING, so an entity
	// leaving a pose for something that shares its slot is left frozen on screen in the pose it just
	// left. Authoring cannot see this either, because a pose names an asset and an action names raw
	// slot indices.
	//
	// Called from the pass that selects each entity's animation, at the one moment a bank is first
	// paired with this asset. It cannot live in the pass that batches animation data instead: that
	// pass matches entities by the same requirements as the engine's transform batching, deliberately
	// and clause for clause, and requiring this asset there would narrow it and misalign the two
	// batches.
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	bool ValidateAgainstBank(const TArray<UAnimSequence*>& BankSequences) const;

	const FCrowdyMassActionEntry* FindAction(FName ActionName) const;

	/**
	 * The entries of PoseStateEnum, which is what the Poses rows above offer as a dropdown. Empty when no
	 * enum is named, so a set that has not chosen one shows an empty list rather than free text that could
	 * never match anything.
	 *
	 * Each option carries its stored value and its shown text separately, and the two genuinely differ. A
	 * Blueprint Enumeration keeps its entries as NewEnumerator0, NewEnumerator1 and so on, and the names the
	 * author typed live only as display text, so a list of entry names alone would offer a column of
	 * indistinguishable options and a wrong pick would be silent: it resolves to a real value, just not the
	 * state that was meant. Storing the entry name rather than the display text is equally deliberate, since
	 * renaming what an entry displays leaves the entry name alone and two entries may display the same text.
	 */
	UFUNCTION()
	TArray<FPropertyTextFName> GetPoseStateOptions() const;

	/**
	 * Every byte-wide enum compiled into the running build, for the Pose State Enum dropdown.
	 *
	 * A dropdown rather than a text field because a mistyped name is the worst failure this asset has: it
	 * resolves to nothing, no arriving state value can select any row, and the set still reads as
	 * configured. Picking from a list removes that failure mode rather than reporting it.
	 *
	 * Filtered to what can actually work, which is the same test the receive path applies: compiled into a
	 * module (a Blueprint Enumeration is chosen through the object property instead, and naming one here
	 * would leave nothing referencing it, so it would not be cooked) and byte-valued.
	 */
	UFUNCTION()
	TArray<FString> GetPoseStateEnumNameOptions() const;

	// Per-frame blend weight for an exponential smoothing step with the given time constant: the
	// fraction of the remaining distance to close this frame. DeltaSeconds is clamped to
	// MaxStepSeconds first. Depends only on the time constant and the frame length, so it is
	// computed once per chunk rather than once per entity.
	static float ComputeSmoothingAlpha(float TimeConstantSeconds, float DeltaSeconds, float MaxStepSeconds);

	// One averaging step towards RawSpeed, deliberately the same weight in both directions; see
	// UCrowdyMassAnimSet::SpeedSmoothingTimeSeconds for why following peaks instead is wrong. A
	// non-finite input is treated as a standstill: without that, one bad value would pin the entity
	// in its current gait for the rest of its life, since every comparison against a NaN is false.
	static float AdvanceSmoothedSpeed(float PreviousSmoothedSpeed, float RawSpeed, float Alpha);

	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;

	/**
	 * Gives every action row that names a Sequence the two bank slots it needs, appending the clip to
	 * Bank where it is short, and writes the resulting indices into PrimaryIndex and
	 * AlternateIndex. After this an action is authored as a name plus a clip.
	 *
	 * APPEND only, never reorder or remove. Every existing index keeps meaning what it meant, which is
	 * what makes this safe to run on a bank that gaits and poses already resolve against: reordering the
	 * bank would silently repoint every one of them.
	 *
	 * Idempotent. A row whose clip already has two slots is left with those two, so running it twice
	 * appends nothing. Rows with no Sequence are skipped and reported: they are crowd-only actions whose
	 * slots are hand-authored, and guessing a clip for them is not something this can do.
	 *
	 * Deliberately a button rather than something PostEditChangeProperty does. It writes a second asset,
	 * and an author who has just typed a clip name has not necessarily decided to grow the bank.
	 */
	UFUNCTION(CallInEditor, Category="Crowdy Mass|Anim|Bank", meta=(DisplayName="Sync Actions Into Bank"))
	void SyncActionsIntoBank();

	// What the sync above would do to CurrentBank, without writing anything. The rules live here and the
	// button is the commit, so what a caller can check ahead of time and what actually happens cannot
	// disagree.
	void ComputeActionBankSync(const TArray<FAnimSequenceTransformProviderSequence>& CurrentBank,
		FCrowdyMassActionBankSync& OutSync) const;
#endif

private:
	// Builds the resolved table straight out of the authored rows, which the by-name lookups above
	// cannot do for it: a name resolves to the FIRST row carrying it, so a duplicate name would give
	// row N the first row's bank index while it kept row N's own rank and play rate. Every id has to
	// describe the row it actually indexes.
	friend struct FCrowdyMassResolvedAnimSet;

	// Every way the authored rows can disagree with a bank, reported one message at a time. The single
	// implementation of those rules: ValidateAgainstBank logs what comes out of here and asset
	// validation raises the same strings as errors, so the two can never drift into disagreeing about
	// what is wrong with an asset. A second copy of the rules would answer confidently and wrongly the
	// first time one of them changed.
	//
	// bAlsoNamedByRowRules says whether IsDataValid's own per-row checks already name this same fault.
	// The two callers need opposite things from that: ValidateAgainstBank runs at runtime, where no
	// per-row pass has happened, so it reports everything; IsDataValid has already raised the row-level
	// error and would otherwise show two for one mistake. An unset sequence and an identical slot pair
	// are the overlaps, and they are the two an unsynced row hits first.
	void ForEachBankProblem(const TArray<UAnimSequence*>& BankSequences,
		TFunctionRef<void(const FString&, bool /*bAlsoNamedByRowRules*/)> Report) const;

#if WITH_EDITOR
	// The Bank's clips flattened into the array the rules above index, loading the bank if it
	// is not already in memory. False when no bank is named, when the named one will not load, or when it
	// holds nothing: those are three different faults and the caller separates them, so this deliberately
	// does not report any of them itself.
	bool GatherBankSequences(TArray<UAnimSequence*>& OutBankSequences) const;
#endif

	static int32 FindInBank(const UAnimSequence* Sequence, const TArray<UAnimSequence*>& BankSequences);
	static float GetSequenceDuration(const UAnimSequence* Sequence);
	static FCrowdyMassResolvedSequence ResolveSequenceAsset(const TSoftObjectPtr<UAnimSequence>& SequenceAsset, const TArray<UAnimSequence*>& BankSequences);
	static ECrowdyMassActionPairStatus ClassifyActionPair(const FCrowdyMassActionEntry& Entry, const TArray<UAnimSequence*>& BankSequences);

	// The state value one authored pose row names, or INDEX_NONE when the set names no enum, when the
	// row names an entry the enum does not have, or when that entry's value is outside the 0..255 a
	// one-byte replicated state can carry. The one place a row's name becomes a value.
	static int64 ResolvePoseStateValue(const UEnum* StateEnum, FName PoseName);

	// Walks the authored rows once and records, for every state value, the id of the FIRST row naming
	// it. Each row is resolved from the row being filled, never by looking its name up again: two rows
	// sharing a name would otherwise both answer with the first row's id.
	static void FillPoseStateTable(const UCrowdyMassAnimSet& AnimSet, FCrowdyMassPoseStateTable& OutTable);

	void EnsurePoseStateTable() const;

	// Answers FindPoseIdByStateValue without walking the rows or searching a name. Not a UPROPERTY and
	// not saved: it is derived entirely from Poses and the state enum and is rebuilt from them whenever
	// either can have changed.
	mutable FCrowdyMassPoseStateTable CachedPoseStateTable;
	mutable bool bPoseStateTableBuilt = false;

	// The enum PoseStateEnumName named, once the slow search has found it. Weak because a Blueprint
	// Enumeration reached this way is an ordinary asset and can be unloaded; a native enum, which is what
	// this field is for, lives for the life of the process.
	mutable TWeakObjectPtr<UEnum> CachedNamedPoseStateEnum;

	// Whether the slow search above has already run for the current name. Kept beside the weak pointer
	// rather than inferred from it, because a MISS leaves that pointer null and is exactly the answer worth
	// remembering: this is reachable from the details panel's GetOptions, so a mistyped name would
	// otherwise pay a full object-hash scan per row per refresh.
	mutable bool bNamedPoseStateEnumSearched = false;

	// Moved by InvalidatePoseStateTable, which every path that can change the rows or the enum already
	// goes through. Not saved: it is only ever compared against a value another object recorded during
	// this session.
	uint32 SourceRevision = 1;
};

/**
 * One anim set already resolved against one animation bank: every authored asset pointer and state
 * name turned into a bank index, laid out so that choosing an animation for an entity is array
 * indexing and float comparison and nothing else.
 *
 * This exists because the alternative is not affordable. Selecting per entity straight off the asset
 * would search the gait list, search the pose list, and resolve a soft object reference,
 * for every entity, every frame; at crowd sizes that is thousands of searches a frame on the game
 * thread, which is the scarce resource here. Resolution instead happens once per (anim set, bank)
 * pair and is reused until either the set's authored rows or the bank changes.
 *
 * Building it touches asset pointers, so build on the game thread. Reading it does not.
 */
struct CROWDYMASS_API FCrowdyMassResolvedAnimSet
{
	// Parallel to the owning anim set's Gaits array (index N here is gait id N+1, the same convention
	// ResolvedPoses uses for PoseId), in authored order, so selection walks it by position and never
	// searches a name. Built even for a row whose sequence did not resolve, so the positions keep
	// meaning "gait id N+1" for every row authored after a broken one.
	//
	// Never longer than CrowdyMassMaxGaits: a gait id has one byte to live in with 0 meaning none, so a
	// row past that could only be given an id that truncates to "no gait".
	TArray<FCrowdyMassResolvedGait> ResolvedGaits;

	// Parallel to the owning anim set's Poses array (index N here is PoseId N+1, the same convention
	// ResolvedActions uses for ActionId). Built even for a pose whose sequence did not resolve, so
	// FindResolvedPose can answer "authored but broken" (INDEX_NONE at that id) the same way
	// ResolvedActions answers it for an action. Rank, play rate and loop mode are carried here rather
	// than read back off the asset, so selecting a pose touches no UObject at all.
	TArray<FCrowdyMassResolvedPose> ResolvedPoses;

	// Parallel to the owning anim set's Actions array (index N here is ActionId N+1). Built even
	// for an action whose pair did not resolve, so FindResolvedAction can answer "authored but
	// broken" the same way ResolvedGaits answers it for a gait: present, both indices INDEX_NONE.
	TArray<FCrowdyMassResolvedAction> ResolvedActions;

	// Number of sequences in the bank this was resolved against.
	int32 BankSize = 0;

	// What UCrowdyMassAnimSet::GetSourceRevision read when this was built. The other half of the staleness
	// check, and the half the bank cannot answer: adding a pose row or repointing the state enum leaves the
	// bank identical while every index in this table means something else.
	uint32 SourceRevision = 0;

	// Hash of the bank's sequence assets, in order. This is the staleness check, and it has to be
	// the contents rather than the length: a bank can be re-authored in place, swapping one sequence
	// for another or reordering two, without its length changing and without the provider asset
	// being replaced. Every index in this table would then mean a different animation, and every
	// one of them would still be in range, so nothing downstream could notice.
	uint32 BankContentHash = 0;

	// Whether Build has run. Needed on its own, because a default-constructed table and a table built
	// against an empty bank are otherwise indistinguishable, and treating the first as the second
	// would leave every entity permanently unanimated with nothing to show for it.
	bool bBuilt = false;

	// Which pose row each state value selects, so the receive path turns an arriving byte into a row id
	// without touching this asset. Built from the authored rows here rather than copied off the anim
	// set's own table, because this one has to describe the rows as they were when the set was resolved.
	//
	// Declared after everything above on purpose. It is 256 bytes and nothing per frame reads it, while the
	// three array headers, the bank identity and the built flag are what the per-entity pass indexes
	// through every frame; putting it between them would push that group across a second cache line.
	FCrowdyMassPoseStateTable PoseStateTable;

	// A null AnimSet resolves to a table in which nothing is authored, so callers need no null check
	// of their own: no gait resolves and selection produces no animation.
	void Build(const UCrowdyMassAnimSet* AnimSet, const TArray<UAnimSequence*>& BankSequences);

	/**
	 * True when this table still describes both the set it was built from and the bank it was built
	 * against. Cheap enough to call whenever the table is handed out, which is what makes an in-place
	 * re-author of either one visible.
	 *
	 * Both halves are needed and neither implies the other. Re-authoring the bank leaves the rows alone
	 * while changing what every index in it means; adding a pose row, retyping a gait's speeds or
	 * repointing the state enum leaves the bank alone while changing which rows this table has at all.
	 */
	bool MatchesSource(const UCrowdyMassAnimSet* AnimSet, const TArray<UAnimSequence*>& BankSequences) const;

	static uint32 HashBankContents(const TArray<UAnimSequence*>& BankSequences);

	/**
	 * The gait row an entity at SmoothedSpeed should be in, given the row it is in now, as a one-based
	 * gait id. 0 in and 0 out both mean no gait; a set with no authored rows always answers 0.
	 *
	 * Speeding up takes the fastest row whose enter speed has been reached; slowing down drops one row
	 * at a time while the speed is under the current row's exit speed, and stops at the first row,
	 * which is the floor. The gap between a row's enter and exit speed is what stops an entity sitting
	 * exactly on a threshold from switching animation every frame.
	 *
	 * An entity that has never been given a gait starts on the floor row, which is where a standing
	 * entity belongs and what every faster row is entered from.
	 */
	uint8 SelectGait(uint8 CurrentGaitId, float SmoothedSpeed) const;

	// Nullptr for gait id 0 (no gait resolved) or an id outside the authored Gaits array. A non-null
	// result can still carry an INDEX_NONE bank index if that row's sequence did not resolve against
	// the bank. One bounds check and one array index, mirroring FindResolvedPose.
	const FCrowdyMassResolvedGait* FindResolvedGait(uint8 GaitId) const;

	// The per-entity lookup: id 0 (no pose set) or an id outside the authored Poses array returns
	// nullptr without touching the array. A non-null result can still carry an INDEX_NONE bank index
	// if that row's sequence did not resolve against the bank; callers fall through to the sources
	// below exactly as an unresolved action does. One bounds check and one array index, no name
	// search, mirroring FindResolvedAction.
	const FCrowdyMassResolvedPose* FindResolvedPose(uint8 PoseId) const;

	// Nullptr for ActionId 0 (never active) or an ActionId outside the authored Actions array.
	// A non-null result can still carry unresolved (INDEX_NONE) slot indices; check those before
	// using them.
	const FCrowdyMassResolvedAction* FindResolvedAction(int32 ActionId) const;
};

// The animation an entity should be showing, already resolved to a bank index. BankIndex is
// INDEX_NONE when nothing selected, in which case the entity keeps whatever it already had.
struct FCrowdyMassComposedAnimation
{
	int32 BankIndex = INDEX_NONE;
	float PlayRate = 1.0f;
	float BlendSeconds = 0.0f;
	EAnimSequenceTrackLoopMode LoopMode = EAnimSequenceTrackLoopMode::Loop;

	// True when BankIndex came from the action tier. The only consumer is the animation
	// processor's first-applied-frame Position correction: it must run exactly when an action's
	// index is the one actually reaching the renderer this frame, never for a gait or a pose that
	// happens to share a bank index with some action.
	bool bIsAction = false;

	// The active action's authored duration, valid only when bIsAction is true. Carried here so
	// the one place that needs it (the Position correction, at push time) does not have to resolve
	// the action a second time.
	float ActionDurationSeconds = 0.0f;

	// True when this frame's action index is a trigger the renderer has not been given yet, which
	// is what the Position correction has to key on. A per-trigger flag cannot answer this: it
	// records that SOME trigger was corrected, not which, so a trigger that arrived while an
	// earlier one was still showing reads as already handled. Valid only when bIsAction is true.
	bool bIsFirstShowOfTrigger = false;
};

/**
 * Picks one animation out of everything an entity could be showing: its pose, its one-shot action,
 * and the gait derived from how it is moving. The pose's authored rank is what orders it against the
 * action, and locomotion is always the floor.
 *
 * The ordering is what makes the three independent of each other. Whoever sets a pose does not have
 * to know that gait selection exists, and gait selection keeps running underneath it, so clearing
 * the pose returns the entity to the right locomotion immediately rather than to whatever it was
 * doing when the pose began. Nothing has to be cleared in any particular order, and nothing has to
 * be rewritten every frame to stay true.
 *
 * A pose that names nothing this bank can resolve is an authoring gap rather than a reason for a
 * corpse to keep jogging on the spot: it drops out of the ordering entirely and the sources below it
 * are evaluated as though it had never been set, exactly as an unresolved action does.
 *
 * An action resolves to whichever of its two authored slots ActionState.Serial has not most
 * recently shown, which is what lets retriggering the same action restart it: the engine only reacts
 * to the pushed index changing, so replaying the same slot twice in a row would be a silent no-op.
 *
 * Lives here rather than inside the processor so the precedence can be asserted directly. It is the
 * kind of rule that is only ever wrong in an order nobody wrote a test for.
 */
CROWDYMASS_API FCrowdyMassComposedAnimation ComposeCrowdyMassAnimation(
	const struct FCrowdyMassAnimStateFragment& AnimState, const struct FCrowdyMassActionFragment& ActionState,
	uint8 PoseId, const FCrowdyMassResolvedAnimSet& Resolved, float BlendSeconds);

/**
 * Whether the renderer needs to be handed DesiredBankIndex, given what was last pushed for this
 * entity.
 *
 * The renderer only reacts to a CHANGE of the pushed index: pushing the same one again is dropped,
 * and playback settings sent without an index change are ignored entirely. Skipping an unchanged
 * index is therefore not an optimisation, it is the reason a crowd standing still costs nothing.
 *
 * The one subtlety is the first push. An untouched entity already reads as index 0, which is a real
 * bank slot, so an entity whose selection genuinely is slot 0 would never be written and would keep
 * the default playback rate and blend rather than the authored ones. PushedBankIndex is the
 * entity's own record of what it has actually been given, and is INDEX_NONE until the first write.
 */
CROWDYMASS_API bool ShouldPushCrowdyMassAnimation(int32 DesiredBankIndex, int32 CurrentBankIndex, int32 PushedBankIndex);

/**
 * The Position to write on the frame an action's animation first reaches the renderer.
 *
 * The renderer's playback clock starts counting from the moment a pushed sequence index changes,
 * never from the moment the action was triggered, so an action that waited before being applied
 * (a deferred call, a late-arriving event) must jump its Position forward by however long it
 * already waited, or it visibly restarts from the beginning instead of playing from where it
 * should already be.
 *
 * Clamped to DurationSeconds: an action applied so late that it would already be over still starts
 * at its last frame (Clamp loop mode holds it there) rather than seeking Position past the end of
 * the clip, which the renderer does not define behaviour for.
 */
CROWDYMASS_API float GetCrowdyMassActionCorrectedPosition(float ElapsedSecondsSinceTrigger, float DurationSeconds);
