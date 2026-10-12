#include "Mass/CrowdyMassActionEvents.h"

#include "Actors/CrowdyMassNearTierActor.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassLog.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassEntity.h"
#include "Replication/RPC/CrowdyRPC.h"

// Why the variant or the play rate will not be read off this event, or an empty string when both will.
// Defined below, beside the id check it is the tail of.
static FString CrowdyMassDescribeActionOptionalParamProblem(const UFunction* Function);

bool CrowdyMassDeclaresActionId(const UFunction* Function)
{
	if (!Function)
	{
		return false;
	}

	const FProperty* ActionIdParam = Function->FindPropertyByName(CrowdyMassActionParamNames::ActionId);

	// An output parameter is refused for the same reason a handler may not declare one: the call arrived
	// from another machine and there is nobody to return anything to, so a value written into it goes
	// nowhere.
	return ActionIdParam
		&& ActionIdParam->HasAnyPropertyFlags(CPF_Parm)
		&& !FCrowdyRPC::IsTrueOutputParam(ActionIdParam)
		&& ActionIdParam->IsA<FIntProperty>();
}

FString CrowdyMassDescribeActionEventProblem(const UFunction* Function)
{
	if (!Function)
	{
		return FString();
	}

	// An id that reads is the whole of whether an action can start, so the optional pair is only worth
	// looking at once that is true; otherwise the author is told about a variant on an event that plays
	// nothing at all.
	if (CrowdyMassDeclaresActionId(Function))
	{
		return CrowdyMassDescribeActionOptionalParamProblem(Function);
	}

	// Named separately from the plain "no such parameter" case, because a parameter that IS there under the
	// right name and is refused for its type or its direction reads as working to anyone comparing the
	// event against the documentation.
	const FProperty* Existing = Function->FindPropertyByName(CrowdyMassActionParamNames::ActionId);
	if (Existing && Existing->HasAnyPropertyFlags(CPF_Parm))
	{
		if (FCrowdyRPC::IsTrueOutputParam(Existing))
		{
			return FString::Printf(
				TEXT("its '%s' parameter is an output. The call arrives from another machine and there is nobody to return a value to, so an output carries nothing. Make it an input."),
				*CrowdyMassActionParamNames::ActionId.ToString());
		}

		return FString::Printf(
			TEXT("its '%s' parameter is declared as %s rather than an integer, so no action id can be read from it. Declare it as an int32."),
			*CrowdyMassActionParamNames::ActionId.ToString(), *Existing->GetCPPType());
	}

	return FString::Printf(
		TEXT("it declares no int32 '%s' input parameter, so it will start no action on any entity drawn as one of a crowd; its body still runs wherever the entity is a real actor. Add an int32 parameter named '%s'; '%s' and '%s' are optional. Action ids start at 1 and follow the animation set's Actions row order, and 0 means no action."),
		*CrowdyMassActionParamNames::ActionId.ToString(),
		*CrowdyMassActionParamNames::ActionId.ToString(),
		*CrowdyMassActionParamNames::Variant.ToString(),
		*CrowdyMassActionParamNames::PlayRate.ToString());
}

static FString CrowdyMassDescribeActionOptionalParamProblem(const UFunction* Function)
{
	if (!Function)
	{
		return FString();
	}

	// The optional pair is read by name and its result is discarded, on purpose: an event that declares
	// neither means variant 0 at the ordinary rate, which is a perfectly good action. What the reader cannot
	// tell apart from that is a parameter that IS declared and cannot be read, which reads back as the same
	// default forever while the pin sits there in the graph looking connected.
	const FProperty* Variant = Function->FindPropertyByName(CrowdyMassActionParamNames::Variant);
	if (Variant && Variant->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(Variant)
		&& !Variant->IsA<FIntProperty>())
	{
		return FString::Printf(
			TEXT("its '%s' parameter is declared as %s rather than an integer, so no variant is ever read from it and every play of this action uses variant 0. Declare it as an int32, or remove it."),
			*CrowdyMassActionParamNames::Variant.ToString(), *Variant->GetCPPType());
	}

	// Read through GetFloat, which accepts either width, so both are correct and nothing else is.
	const FProperty* PlayRate = Function->FindPropertyByName(CrowdyMassActionParamNames::PlayRate);
	if (PlayRate && PlayRate->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(PlayRate)
		&& !PlayRate->IsA<FFloatProperty>() && !PlayRate->IsA<FDoubleProperty>())
	{
		return FString::Printf(
			TEXT("its '%s' parameter is declared as %s rather than a float, so no rate is ever read from it and every play of this action uses rate 1. Declare it as a float, or remove it."),
			*CrowdyMassActionParamNames::PlayRate.ToString(), *PlayRate->GetCPPType());
	}

	return FString();
}

UFunction* CrowdyMassFindPlayActionEvent()
{
	// Resolved every call rather than cached in a static. It is one name lookup on a native class, and a
	// cached UFunction* would be one more thing to keep correct across a hot reload for no measurable
	// saving; this is never on a per-entity path.
	return UCrowdyMassEntity::StaticClass()->FindFunctionByName(TEXT("PlayCrowdyAction_Implementation"));
}

ECrowdyMassActionSendVerdict CrowdyMassDecideActionSend(const UClass* EntityClass, const UCrowdyMassAnimSet* AnimSet,
	const FName ActionName, int32& OutActionId)
{
	OutActionId = 0;

	if (!EntityClass)
	{
		// Asked before the set, because a null set means two different things and only the class can tell
		// them apart: nothing has said what this entity is yet, which resolves itself, or the class is
		// known and binds no set, which somebody has to fix.
		return ECrowdyMassActionSendVerdict::ClassUnknown;
	}

	if (!AnimSet)
	{
		return ECrowdyMassActionSendVerdict::NoAnimSet;
	}

	const int32 ActionId = AnimSet->FindActionId(ActionName);
	if (ActionId <= 0)
	{
		return ECrowdyMassActionSendVerdict::UnknownAction;
	}

	OutActionId = ActionId;
	return ECrowdyMassActionSendVerdict::Send;
}

namespace
{
	// Keyed on the class, the name AND the verdict. See CrowdyMassShouldReportActionSendRefusal for why
	// the verdict cannot be left out.
	TMap<FObjectKey, TSet<TPair<FName, uint8>>>& ActionSendRefusalsReported()
	{
		static TMap<FObjectKey, TSet<TPair<FName, uint8>>> Reported;
		return Reported;
	}
}

bool CrowdyMassShouldReportActionSendRefusal(const UClass* EntityClass, const FName ActionName,
	const ECrowdyMassActionSendVerdict Verdict)
{
	bool bAlreadyReported = false;
	ActionSendRefusalsReported().FindOrAdd(FObjectKey(EntityClass))
		.Add(TPair<FName, uint8>(ActionName, static_cast<uint8>(Verdict)), &bAlreadyReported);
	return !bAlreadyReported;
}

void CrowdyMassResetActionSendRefusalLog()
{
	ActionSendRefusalsReported().Reset();
}

float CrowdyMassClampActionPlayRate(const float PlayRate)
{
	// A non-finite rate is not clamped, it is replaced: NaN compares false against every bound, so
	// clamping it returns it unchanged and it reaches the renderer intact.
	return FMath::IsFinite(PlayRate)
		? FMath::Clamp(PlayRate, CrowdyMassMinActionPlayRate, CrowdyMassMaxActionPlayRate)
		: 1.0f;
}

bool CrowdyMassWriteAction(FCrowdyMassActionFragment& Action, const int32 ActionId, const int32 Variant,
	const float PlayRate)
{
	if (ActionId <= 0)
	{
		// Zero is the fragment's "nothing is playing" value and a negative id is nonsense, so neither can
		// name an action. Dropping them here keeps clearing the slot the animation pass's decision alone.
		return false;
	}

	Action.ActionId = ActionId;
	Action.Variant = FMath::Clamp(Variant, 0, CrowdyMassMaxActionVariant);
	Action.PlayRate = CrowdyMassClampActionPlayRate(PlayRate);

	// Wraps rather than running off the end of the type: only the fact that it changed carries meaning,
	// and signed overflow would be undefined.
	Action.Serial = Action.Serial < MAX_int32 ? Action.Serial + 1 : 0;

	// The clock starts now, at the moment the event was received, because the wire carries no send
	// stamp to start it any earlier. What this corrects for is the gap between arriving here and being
	// picked up by the animation pass, which is why the pass must apply it: without that correction an
	// action that waits even one frame plays from its first frame anyway and finishes late.
	Action.ElapsedSecondsSinceTrigger = 0.0f;

	// AppliedSerial is deliberately left alone. It records which trigger the renderer was actually
	// given, and the Serial just written differs from it by construction, so the new trigger already
	// reads as outstanding. Clearing it here would throw away the only evidence of what is on screen.
	return true;
}

FCrowdyMassEventIngestor CrowdyMassMakeActionEventHandler(const FName ActionIdParam, const FName VariantParam,
	const FName PlayRateParam)
{
	return [ActionIdParam, VariantParam, PlayRateParam](const FCrowdyEventParams& Params,
		const FCrowdyMassViewStateContext& Context)
	{
		int32 ActionId = 0;
		if (!Params.GetInt32(ActionIdParam, ActionId))
		{
			// The event declares no such parameter, so there is no action to start. Left silent: which
			// names an event uses is the registering game's business, and it is told at registration time
			// by nothing landing rather than by a line per inbound event.
			return;
		}

		int32 Variant = 0;
		Params.GetInt32(VariantParam, Variant);

		// An event that does not carry a rate means the ordinary one, not zero, which would freeze the
		// animation on its first frame.
		float PlayRate = 1.0f;
		Params.GetFloat(PlayRateParam, PlayRate);

		FCrowdyMassActionFragment* Action = Context.GetFragment<FCrowdyMassActionFragment>();
		if (!Action)
		{
			// An entity built from a config with no animation trait carries no action slot, so it has
			// nothing an action could be played on. Ordinary, but said once per entity class under the
			// trace: from the outside this is indistinguishable from an event that never arrived, and the
			// reader goes looking in the transport, which is the one place the fault cannot be.
			UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
				TEXT("[CrowdyMassAction]: action %d arrived for an entity whose archetype carries no action slot, so nothing was started. The Mass entity config it was built from has no Crowdy Mass Animation trait."),
				ActionId);
			return;
		}

		UE_CLOG(CrowdyMassNearTier::IsTraceEnabled(), LogCrowdyMass, Log,
			TEXT("[CrowdyMassAction]: wrote action %d (variant %d, rate %.2f) into the entity's action slot."),
			ActionId, Variant, PlayRate);

		CrowdyMassWriteAction(*Action, ActionId, Variant, PlayRate);
	};
}
