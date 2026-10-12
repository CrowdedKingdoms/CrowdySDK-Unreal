#pragma once

#include "CoreMinimal.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

struct FCrowdyMassActionFragment;
class UCrowdyMassAnimSet;

// Bounds every action field an inbound event can set. The values arrive off the network and are
// treated as forged: a play rate of zero would freeze an animation forever, a huge one would burn
// through a sequence in a frame, and a non-finite one poisons every comparison downstream. The
// variant is an authoring selector, so it is clamped to a range an anim set could plausibly cover
// rather than to whatever an integer can hold.
inline constexpr float CrowdyMassMinActionPlayRate = 0.1f;
inline constexpr float CrowdyMassMaxActionPlayRate = 4.0f;
inline constexpr int32 CrowdyMassMaxActionVariant = 63;

/**
 * The parameter names an action event is read by when nothing says otherwise.
 *
 * Named here rather than written twice, because the handler that reads them and the diagnostic that
 * reports an event carrying none of them have to agree: a diagnostic checking a different spelling from
 * the one the handler reads would pass an event that then lands nothing.
 */
namespace CrowdyMassActionParamNames
{
	inline const FName ActionId = TEXT("ActionId");
	inline const FName Variant  = TEXT("Variant");
	inline const FName PlayRate = TEXT("PlayRate");
}

/**
 * Brings an inbound play rate inside the bounds an action may be played at.
 *
 * Public because BOTH representations have to apply it. The value came off the network and is forgeable:
 * zero freezes an animation on its first frame, a huge one burns through the clip in a frame, and a
 * non-finite one poisons every comparison downstream. An actor clamping differently from the fragment
 * write, or not at all, is the same event playing at two speeds depending on how a client happens to be
 * drawing the entity.
 */
CROWDYMASS_API float CrowdyMassClampActionPlayRate(float PlayRate);

/**
 * Starts a one-shot action on one entity: an attack swing, a flinch, an emote.
 *
 * The single write behind every road a one-shot travels, so the clamps and the retrigger bookkeeping are
 * stated once. False when nothing was written, which is the case for an id of zero or less: zero is the
 * fragment's own "nothing is playing" value and a negative id is nonsense, so neither can name an action,
 * and honoring one would let an event cancel an action rather than start one.
 *
 * ActionId names the action in the vocabulary the sending game and the receiving anim set share. It is
 * resolved against the entity's anim set by the animation pass, and an id that does not resolve there
 * plays nothing.
 */
CROWDYMASS_API bool CrowdyMassWriteAction(FCrowdyMassActionFragment& Action, int32 ActionId, int32 Variant,
	float PlayRate);

/**
 * A ready-made handler for a game's own one-shot action event, for RegisterEventHandler.
 *
 * This is the whole of what a crowd entity does with an action event, so a game that declares
 * PlayOneShot on its own actor class registers this against it and writes no receive code at all.
 *
 * The parameters are read BY NAME off the call, defaulting to the names an action event usually gives
 * them; pass the names the event actually uses when they differ. A name the event does not declare simply
 * does not land: an absent variant is zero and an absent play rate is 1, which is what an event carrying
 * neither means. An absent ACTION ID writes nothing at all, because an action with no id is not an action.
 *
 * Everything it reads came off the network from the client being observed and it produces a visual and
 * nothing else, which is the only thing a forgeable value may decide.
 */
CROWDYMASS_API FCrowdyMassEventIngestor CrowdyMassMakeActionEventHandler(
	FName ActionIdParam = CrowdyMassActionParamNames::ActionId,
	FName VariantParam = CrowdyMassActionParamNames::Variant,
	FName PlayRateParam = CrowdyMassActionParamNames::PlayRate);

/**
 * True when Function could carry an action at all: it declares an int32 input parameter under the name an
 * action event is read by.
 *
 * Separate from whether the author DECLARED it an action (meta=(CrowdyAction), read through
 * FCrowdyFnInfo). The two are asked in different places and mean different things: the declaration is
 * what makes a crowd entity react to the event at all, and this is what says whether that reaction can
 * amount to anything. An event declared an action and carrying no id is an authoring mistake worth
 * reporting, not a reason to stop delivering it.
 */
CROWDYMASS_API bool CrowdyMassDeclaresActionId(const UFunction* Function);

/**
 * Why an event declared a one-shot action will start nothing on an entity held as a row, or an empty
 * string when it will.
 *
 * The same verdict CrowdyMassDeclaresActionId gives, worded for an author rather than for a branch, so an
 * editor that has no idea what an action needs can report it without learning. The sentence continues one
 * that has already named the event, so it starts mid-clause.
 *
 * Empty always means "nothing to report" and never "could not tell", because everything it looks at is on
 * the function in front of it.
 */
CROWDYMASS_API FString CrowdyMassDescribeActionEventProblem(const UFunction* Function);

/**
 * The one action event the SDK declares, UCrowdyMassEntity::PlayCrowdyAction, resolved by reflection.
 *
 * Named in one place so the send path, the tests and anything else asking for it cannot disagree about
 * the spelling; a receiver declared in C++ is always <Event>_Implementation, which is easy to get wrong
 * by hand. Null only if the class has been reshaped and this was not updated with it.
 */
CROWDYMASS_API UFunction* CrowdyMassFindPlayActionEvent();

/**
 * Why an action named by NAME cannot be sent to one entity, or that it can.
 *
 * Four refusals rather than one, because they call for different things and are otherwise all just "no
 * action started": a class this client has not learned yet resolves later on its own, a class that names
 * no animation set is a binding an author has to make, and a name no row carries is a typo or an
 * unauthored row. Folding them together is what makes a send that lands nothing indistinguishable from
 * one that never happened.
 */
enum class ECrowdyMassActionSendVerdict : uint8
{
	Send,           // OutActionId names a row in this class's set.
	ClassUnknown,   // Nothing has told this client what class the entity is, so there is no set to ask.
	NoAnimSet,      // The class is known and names no animation set, so it authors no actions at all.
	UnknownAction,  // The set is there and authors no row under this name (NAME_None included).
};

/**
 * Turns an action NAME into the id the wire carries, against the animation set of the class the TARGET
 * is recorded as.
 *
 * THE CLASS IS PART OF THE QUESTION, exactly as it is for a pose. An id is a row position in one class's
 * set, so resolving a name against another class's set hands the entity whatever that class authored
 * under it, which is a wrong action rather than no action and nothing anywhere would notice.
 *
 * OutActionId is 0 on every verdict but Send, and 0 is the fragment's own "nothing playing" value, so a
 * caller that sent it anyway would report success and start nothing.
 *
 * Free-standing and pure: this is the whole decision behind Send Crowdy Action, asserted directly rather
 * than through a world, a Mass entity manager and a spawned crowd.
 */
CROWDYMASS_API ECrowdyMassActionSendVerdict CrowdyMassDecideActionSend(const UClass* EntityClass,
	const UCrowdyMassAnimSet* AnimSet, FName ActionName, int32& OutActionId);

/**
 * Whether this refusal has yet to be said, recording it as said when it has not.
 *
 * A refusal never changes between shots, so a line per shot would bury the one sentence that says what to
 * author; but the three reasons are three separately-actionable sentences and each has to be said for
 * itself. THE VERDICT IS PART OF THE SUBJECT, which is the whole point of this signature: binding a class
 * to an animation set is exactly what the "names no animation set" line asks for, and it turns that
 * verdict into "the set authors no such row" without changing the class object. Remembering class and name
 * alone would then answer "already said" and swallow the sentence naming the remaining fix, so an author
 * who followed the printed advice would be rewarded with identical silence.
 *
 * A null class keys the "not learned yet" case, which is temporary and has no class of its own to key on.
 *
 * Game-thread only: the record is a plain static map.
 */
CROWDYMASS_API bool CrowdyMassShouldReportActionSendRefusal(const UClass* EntityClass, FName ActionName,
	ECrowdyMassActionSendVerdict Verdict);

/**
 * Forgets every refusal already reported, so the next one is reported again.
 *
 * The record is process-wide and never expires, so without this a test asserting that a refusal WAS
 * reported depends on whatever ran before it, and a test asserting that a second, different reason still
 * reports cannot tell suppressed from reported at all.
 */
CROWDYMASS_API void CrowdyMassResetActionSendRefusalLog();
