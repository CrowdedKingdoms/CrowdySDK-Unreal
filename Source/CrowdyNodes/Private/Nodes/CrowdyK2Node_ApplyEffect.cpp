// Fill out your copyright notice in the Description page of Project Settings.

#include "Nodes/CrowdyK2Node_ApplyEffect.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "TimerManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MakeMap.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
#include "Nodes/CrowdyApplyEffectNodePins.h"
#include "Nodes/CrowdyApplyEffectNodeShared.h"
#include "Replication/GameModel/CrowdyEffectActions.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ApplyEffect"

using namespace CrowdyApplyEffectNodeShared;

UCrowdyK2Node_ApplyEffect::UCrowdyK2Node_ApplyEffect(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ProxyActivateFunctionName = GET_FUNCTION_NAME_CHECKED(UBlueprintAsyncActionBase, Activate);
}

UCrowdyEffect* UCrowdyK2Node_ApplyEffect::GetLiteralEffect() const
{
	// Mid-reconstruction, the live Effect pin is a freshly allocated one with no value yet (ReconstructNode only
	// rewires the outgoing pin's value onto it after AllocateDefaultPins returns), so the pending value captured
	// in ReallocatePinsDuringReconstruction is the only correct source of truth at this point.
	UCrowdyEffect* Effect = PendingReconstructionEffect;
	if (!Effect)
	{
		const UEdGraphPin* Pin = FindPin(PN_Effect, EGPD_Input);
		if (!Pin || Pin->LinkedTo.Num() > 0)
		{
			return nullptr;
		}
		Effect = Cast<UCrowdyEffect>(Pin->DefaultObject);
	}

	// Everything this node shapes itself from lives on the asset, and the asset can still be an unserialized export
	// while this graph is compiled on load, so no caller may read it before it is in.
	PreloadEffectAsset(Effect);
	return Effect;
}

void UCrowdyK2Node_ApplyEffect::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	// AllocateDefaultPins (called by Super below) rebuilds a brand new Effect pin with an empty DefaultObject;
	// ReconstructNode only copies the outgoing pin's value onto it afterward (RewireOldPinsToNewPins, run by the
	// caller once this returns). Read that value now, off the still-intact OldPins, so SynchronizeEffectPins sees
	// the effect that is actually about to be restored instead of treating the node as freshly emptied out.
	PendingReconstructionEffect = nullptr;
	for (UEdGraphPin* OldPin : OldPins)
	{
		if (OldPin && OldPin->PinName == PN_Effect && OldPin->LinkedTo.Num() == 0)
		{
			PendingReconstructionEffect = Cast<UCrowdyEffect>(OldPin->DefaultObject);
			break;
		}
	}
	PendingCaseOnlyMagnitudeValues = CrowdyApplyEffectNodePins::CaptureCaseOnlyMagnitudeValues(OldPins);

	Super::ReallocatePinsDuringReconstruction(OldPins);

	PendingReconstructionEffect = nullptr;
}

void UCrowdyK2Node_ApplyEffect::PostReconstructNode()
{
	Super::PostReconstructNode();

	CrowdyApplyEffectNodePins::RestoreCaseOnlyMagnitudeValues(PendingCaseOnlyMagnitudeValues, Pins);
	PendingCaseOnlyMagnitudeValues.Reset();
}

void UCrowdyK2Node_ApplyEffect::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();
	SynchronizeEffectPins();
	RegisterEffectAssetListener();
}

void UCrowdyK2Node_ApplyEffect::SynchronizeEffectPins()
{
	UCrowdyEffect* Effect = GetLiteralEffect();
	if (!Effect)
	{
		// No literal effect: leave the plain async pin set (Overrides / Level / Source visible). ExpandNode and
		// ValidateNodeDuringCompilation detect this same state and degrade accordingly.
		return;
	}

	// The raw map pin is replaced by the typed magnitude pins, so hide it (ExpandNode still fills it).
	if (UEdGraphPin* OverridesPin = FindPin(PN_Overrides, EGPD_Input))
	{
		OverridesPin->bHidden = true;
	}

	const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);

	// Level only matters when a magnitude is curve-driven; Source only when the effect reads source.<attr>.
	if (!Plan.bIncludeLevel)
	{
		if (UEdGraphPin* LevelPin = FindPin(PN_Level, EGPD_Input))
		{
			RemovePin(LevelPin);
		}
	}
	if (!Plan.bIncludeSource)
	{
		if (UEdGraphPin* SourcePin = FindPin(PN_Source, EGPD_Input))
		{
			RemovePin(SourcePin);
		}
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	for (const FCrowdyApplyEffectPinEntry& Entry : Plan.Magnitudes)
	{
		UEdGraphPin* Pin = CreatePin(EGPD_Input, MagnitudePinType(Entry.ValueType), Entry.PinName);
		Pin->PinFriendlyName = FText::FromString(Entry.MagnitudeName);
		CrowdyApplyEffectNodePins::ApplyMagnitudeDefaultToPin(Pin, Entry, Schema);
	}

	// The typed form of the raw Return Value Json pin, which stays as it is. ExpandNode splices the decode behind
	// this pin and then removes it, so it never reaches the wrapped call.
	if (Plan.HasReturnPin())
	{
		UEdGraphPin* ReturnPin = CreatePin(EGPD_Output, ReturnPinType(Plan.ReturnType), PN_ReturnValue);
		ReturnPin->PinFriendlyName = LOCTEXT("ReturnValuePinFriendlyName", "Return Value");
		ReturnPin->PinToolTip = LOCTEXT("ReturnValuePinTooltip",
			"The value the effect answered with, decoded to the type it declares. An absent or unreadable result "
			"decodes to this type's default; Return Value Json is what distinguishes that from a real zero.")
			.ToString();
	}
}

void UCrowdyK2Node_ApplyEffect::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	Super::PinDefaultValueChanged(Pin);

	// A new effect asset reshapes the whole magnitude pin set. Reconstructing here would tear down this pin set
	// immediately, but the caller that set the value (the asset picker's TrySetDefaultValue path) keeps
	// dereferencing the changed pin after this returns - freeing it now is a use-after-free that crashes in
	// UEdGraphPin::DoesDefaultValueMatchAutogenerated. Defer the rebuild until the set has fully unwound.
	if (Pin && Pin->PinName == PN_Effect)
	{
		ScheduleReconstruct();
	}
}

void UCrowdyK2Node_ApplyEffect::ScheduleReconstruct()
{
	if (bReconstructPending || !GEditor)
	{
		return;
	}
	bReconstructPending = true;

	// SetTimerForNextTick runs once the current input event has fully unwound, so the pin the picker still holds is
	// no longer live when the reconstruct destroys it. Weak-bound: a node deleted before the tick fires does nothing.
	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		bReconstructPending = false;
		ReconstructNode();
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}));
}

void UCrowdyK2Node_ApplyEffect::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::PinConnectionListChanged(Pin);

	// Wiring or unwiring the Effect pin flips between the typed and the degraded (plain) pin sets.
	if (Pin && Pin->PinName == PN_Effect)
	{
		ReconstructNode();
	}
}

void UCrowdyK2Node_ApplyEffect::PostPlacedNewNode()
{
	Super::PostPlacedNewNode();
	RegisterEffectAssetListener();
}

void UCrowdyK2Node_ApplyEffect::PostLoad()
{
	Super::PostLoad();
	RegisterEffectAssetListener();
}

void UCrowdyK2Node_ApplyEffect::BeginDestroy()
{
	if (OnPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnPropertyChangedHandle);
		OnPropertyChangedHandle.Reset();
	}
	Super::BeginDestroy();
}

void UCrowdyK2Node_ApplyEffect::RegisterEffectAssetListener()
{
	// Only real graph nodes need to react to asset edits; template / action-database nodes have no graph and would
	// only add a needless global-delegate subscription.
	if (!GetGraph())
	{
		return;
	}
	if (!OnPropertyChangedHandle.IsValid())
	{
		OnPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
			this, &UCrowdyK2Node_ApplyEffect::HandleObjectPropertyChanged);
	}
}

void UCrowdyK2Node_ApplyEffect::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// React only to edits on the effect this node references; any other asset's change is irrelevant.
	if (!Object || Object != GetLiteralEffect())
	{
		return;
	}

	// A slider drag fires a stream of interactive changes; skip them and rebuild once when the final committed
	// (non-interactive) change lands, so mid-drag churn does not thrash the whole node.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	// Only an edit to a field the pin layout is built from needs a reconstruct.
	if (!ChangeAffectsPinPlan(PropertyChangedEvent))
	{
		return;
	}

	ReconstructNode();
}

FText UCrowdyK2Node_ApplyEffect::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (const UCrowdyEffect* Effect = GetLiteralEffect())
	{
		return FText::Format(LOCTEXT("NodeTitleWithEffect", "Apply Crowdy Effect: {0}"),
			FText::FromString(Effect->GetName()));
	}
	return LOCTEXT("NodeTitle", "Apply Crowdy Effect");
}

FText UCrowdyK2Node_ApplyEffect::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Apply an authored Game Model effect to a Target, exposing one typed pin per tuning magnitude.\n\n"
		"Latent. This node completes at a later time.");
}

FText UCrowdyK2Node_ApplyEffect::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Crowdy SDK|Game Model|Effects");
}

void UCrowdyK2Node_ApplyEffect::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (!ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		return;
	}

	UFunction* Factory = UCrowdyApplyEffectAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyApplyEffectAction, ApplyEffect));
	if (!Factory)
	{
		return;
	}

	UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Factory);
	check(Spawner);
	Spawner->NodeClass = GetClass();

	TWeakObjectPtr<UFunction> FactoryPtr = MakeWeakObjectPtr(Factory);
	Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda(
		[FactoryPtr](UEdGraphNode* NewNode, bool /*bIsTemplateNode*/)
		{
			UCrowdyK2Node_ApplyEffect* Node = CastChecked<UCrowdyK2Node_ApplyEffect>(NewNode);
			if (UFunction* Func = FactoryPtr.Get())
			{
				if (const FObjectProperty* ReturnProp = CastField<FObjectProperty>(Func->GetReturnProperty()))
				{
					Node->ProxyFactoryFunctionName = Func->GetFName();
					Node->ProxyFactoryClass = Func->GetOuterUClass();
					Node->ProxyClass = ReturnProp->PropertyClass;
				}
			}
		});

	ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
}

void UCrowdyK2Node_ApplyEffect::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);

	// Checked before the Effect diagnostics below, which return early: an unwired Target on a non-container
	// Blueprint is wrong regardless of what the Effect pin holds.
	CrowdyApplyEffectNodeShared::ValidateSelfTargetIsContainer(
		this, FindPin(CrowdyApplyEffectNodeShared::PN_Target, EGPD_Input), MessageLog);

	// Everything below compiles the effect, which resolves its container types and can load the asset that
	// declares one. That load is illegal while the loader is regenerating this Blueprint, because it re-enters
	// the compile already in flight; the checks run in full on the next compile the author asks for.
	if (CrowdyApplyEffectNodeShared::IsCompilingOnLoad(this))
	{
		return;
	}

	const UEdGraphPin* EffectPin = FindPin(PN_Effect, EGPD_Input);
	if (EffectPin && EffectPin->LinkedTo.Num() > 0)
	{
		MessageLog.Warning(*LOCTEXT("ConnectedEffect",
			"@@ has a connected Effect pin, so its magnitudes can't be read at compile time. It falls back to a raw "
			"Overrides map. Set the Effect as a literal to get typed magnitude pins.").ToString(), this);
		return;
	}

	UCrowdyEffect* Effect = GetLiteralEffect();
	if (!Effect)
	{
		MessageLog.Error(*LOCTEXT("MissingEffect", "@@ has no Effect assigned.").ToString(), this);
		return;
	}

	// A malformed effect (parse / lowering error) would silently apply nothing; surface it here so the graph fails to
	// compile rather than at runtime. Compiled without the fn-callee catalog: only Error diagnostics are surfaced
	// here, the catalog adds warnings alone, and a catalog cache miss would force-load every Crowdy Effect asset in
	// the middle of a Blueprint compile.
	const FCrowdyEffectLoweringResult Lowered = Effect->Compile(ECrowdyEffectFnCatalog::None);
	if (Lowered.HasErrors())
	{
		MessageLog.Error(*FText::Format(LOCTEXT("EffectCompileFailed",
			"@@ references effect '{0}', which does not compile ({1}). Fix it in the effect asset."),
			FText::FromString(Effect->GetName()),
			FText::FromString(FirstCompileError(Lowered.Diagnostics))).ToString(), this);
	}

	// The Return Value pin follows the declared type alone, because that is the only half of this question the node
	// can be notified about. Whether the body actually produces a value is asked here instead, off the lowering that
	// was just computed, so the answer is recomputed on every compile and can never go stale.
	if (CrowdyApplyEffectNodeShared::ReturnPinLacksBacking(Effect->ReturnType, Lowered.Function.ReturnExpression))
	{
		MessageLog.Warning(*FText::Format(LOCTEXT("ReturnTypeWithoutReturn",
			"@@ references effect '{0}', which declares a return type but authors no return expression, so its Return "
			"Value pin always reads the type's default. Add a return to the effect, or set its Return Type back to "
			"None."),
			FText::FromString(Effect->GetName())).ToString(), this);
	}

	const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);

	// A duplicated magnitude name collapses to one pin, so a second magnitude of that name would silently lose its
	// value. Fail compilation naming each duplicate rather than dropping it at runtime.
	for (const FString& Duplicate : Plan.DuplicateMagnitudeNames)
	{
		MessageLog.Error(*FText::Format(LOCTEXT("DuplicateMagnitudeName",
			"@@ references effect '{0}', which declares more than one magnitude named '{1}'. Give each magnitude a "
			"unique name."),
			FText::FromString(Effect->GetName()), FText::FromString(Duplicate)).ToString(), this);
	}

	// A required magnitude (no default, no curve) must be supplied: its pin must be wired or set to a value.
	for (const FCrowdyApplyEffectPinEntry& Entry : Plan.Magnitudes)
	{
		if (!Entry.bRequired)
		{
			continue;
		}
		const UEdGraphPin* Pin = FindPin(Entry.PinName, EGPD_Input);
		// A required pin is unsupplied when it is unwired and still sitting at its autogenerated default. This uses
		// the pin's own change-detection rather than an empty-string test: a numeric pin now carries a valid
		// non-empty default (see SynchronizeEffectPins), so "empty" is no longer the right signal for "untouched".
		const bool bUnset = Pin && Pin->LinkedTo.Num() == 0
			&& (Entry.IsContainerRef() ? (Pin->DefaultObject == nullptr) : Pin->DoesDefaultValueMatchAutogenerated());
		if (!Pin || bUnset)
		{
			MessageLog.Error(*FText::Format(LOCTEXT("RequiredMagnitudeEmpty",
				"@@ requires a value for magnitude '{0}' (the effect gives it no default)."),
				FText::FromString(Entry.MagnitudeName)).ToString(), this);
		}
	}
}

void UCrowdyK2Node_ApplyEffect::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	UCrowdyEffect* Effect = GetLiteralEffect();
	if (!Effect)
	{
		// Degraded (no literal effect): the plain async node expansion, with the raw Overrides map pin.
		Super::ExpandNode(CompilerContext, SourceGraph);
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	check(Schema);
	bool bIsErrorFree = true;

	UEdGraphPin* OverridesPin = FindPin(PN_Overrides, EGPD_Input);

	// Collect the magnitude pins that actually contribute an Override: wired, or changed from their prefill.
	struct FIncluded
	{
		UEdGraphPin* Pin = nullptr;
		ECrowdyEffectValueType ValueType = ECrowdyEffectValueType::Int;
	};
	TArray<FIncluded> Included;
	TArray<FString> IncludedKeys;

	const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);
	for (const FCrowdyApplyEffectPinEntry& Entry : Plan.Magnitudes)
	{
		UEdGraphPin* Pin = FindPin(Entry.PinName, EGPD_Input);
		if (!Pin)
		{
			continue;
		}
		const bool bConnected = Pin->LinkedTo.Num() > 0;
		if (CrowdyApplyEffectNodePins::ShouldIncludeOverride(bConnected, Entry.IsContainerRef(), Pin->DefaultObject,
			Pin->DefaultValue, Pin->AutogeneratedDefaultValue))
		{
			Included.Add({ Pin, Entry.ValueType });
			IncludedKeys.Add(Entry.MagnitudeName);
		}
	}

	if (Included.Num() > 0 && OverridesPin)
	{
		UK2Node_MakeMap* MakeMap = CompilerContext.SpawnIntermediateNode<UK2Node_MakeMap>(this, SourceGraph);
		MakeMap->NumInputs = Included.Num();
		MakeMap->AllocateDefaultPins();

		// Force the map type to TMap<FName, FString> to match the factory's Overrides param, rather than relying on
		// wildcard propagation during compilation.
		if (UEdGraphPin* MapOut = MakeMap->GetOutputPin())
		{
			MapOut->PinType = OverridesPin->PinType;
		}

		TArray<UEdGraphPin*> KeyPins;
		TArray<UEdGraphPin*> ValuePins;
		MakeMap->GetKeyAndValuePins(KeyPins, ValuePins);

		for (int32 Index = 0; Index < Included.Num(); ++Index)
		{
			UEdGraphPin* KeyPin = KeyPins.IsValidIndex(Index) ? KeyPins[Index] : nullptr;
			UEdGraphPin* ValuePin = ValuePins.IsValidIndex(Index) ? ValuePins[Index] : nullptr;
			if (!KeyPin || !ValuePin)
			{
				bIsErrorFree = false;
				continue;
			}

			KeyPin->PinType.ResetToDefaults();
			KeyPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			KeyPin->DefaultValue = IncludedKeys[Index];

			ValuePin->PinType.ResetToDefaults();
			ValuePin->PinType.PinCategory = UEdGraphSchema_K2::PC_String;

			FName InputParam;
			const FName EncoderName = EncoderFunctionName(Included[Index].ValueType, InputParam);

			UK2Node_CallFunction* Encoder = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
			Encoder->FunctionReference.SetExternalMember(EncoderName, UCrowdyEffects::StaticClass());
			Encoder->AllocateDefaultPins();

			UEdGraphPin* EncoderInput = Encoder->FindPin(InputParam, EGPD_Input);
			UEdGraphPin* EncoderReturn = Encoder->GetReturnValuePin();
			if (!EncoderInput || !EncoderReturn)
			{
				bIsErrorFree = false;
				continue;
			}

			// Carry the magnitude pin's literal default across, then move any external connection onto the encoder
			// input (a connected pin's value wins over the copied literal at runtime).
			EncoderInput->DefaultValue = Included[Index].Pin->DefaultValue;
			EncoderInput->DefaultObject = Included[Index].Pin->DefaultObject;
			EncoderInput->DefaultTextValue = Included[Index].Pin->DefaultTextValue;
			bIsErrorFree &= CompilerContext.MovePinLinksToIntermediate(*Included[Index].Pin, *EncoderInput).CanSafeConnect();

			bIsErrorFree &= Schema->TryCreateConnection(EncoderReturn, ValuePin);
		}

		// Feed the assembled map into this node's (hidden) Overrides pin. Super::ExpandNode then relocates that link
		// onto the factory call's Overrides argument.
		bIsErrorFree &= Schema->TryCreateConnection(MakeMap->GetOutputPin(), OverridesPin);
	}

	// The magnitude pins are consumed; remove them so the base expansion (which connects every input pin by name to
	// the factory function) does not see an unmatched pin.
	TArray<UEdGraphPin*> MagnitudePins;
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin && CrowdyApplyEffectNodePins::IsMagnitudePinName(Pin->PinName))
		{
			MagnitudePins.Add(Pin);
		}
	}
	for (UEdGraphPin* Pin : MagnitudePins)
	{
		RemovePin(Pin);
	}

	// The typed Return Value pin is the raw Return Value Json pin run through a pure decoder. Wiring the decoder's
	// INPUT to this node's own Return Value Json pin is what makes it work: the base expansion pairs every output
	// pin past the first delegate exec pin with a temporary variable, moves that pin's links onto the variable, and
	// then assigns the delegate's value into the variable on each branch. So the decoder ends up reading the same
	// variable both branches write, and being pure it is evaluated at each use, always downstream of that write.
	//
	// The Return Value pin itself must be gone before the base expansion runs. It matches no delegate parameter, so
	// it would be paired with a temporary variable of its own that nothing ever assigns to, and every consumer would
	// silently read that type's zero forever with nothing failing to compile.
	bool bReturnValueConnected = true;
	if (UEdGraphPin* ReturnPin = FindPin(PN_ReturnValue, EGPD_Output))
	{
		// An orphaned pin is one the effect no longer declares, kept only because the designer still has something
		// wired to it. Decoding it would mean asking for a decoder the effect no longer has, and FindPin returns it
		// even though the base expansion skips it, so it has to be excluded here by name. It is also left in place
		// rather than removed: the compiler reports an orphaned pin with a message that names it and tells the author
		// to refresh the node, which is the only actionable thing they are told, and removing it here would delete the
		// pin that message is about before it is ever emitted.
		const bool bDecodable = Plan.HasReturnPin() && !ReturnPin->bOrphanedPin;
		if (bDecodable && ReturnPin->LinkedTo.Num() > 0)
		{
			UEdGraphPin* JsonPin = FindPin(PN_ReturnValueJson, EGPD_Output);
			if (!JsonPin)
			{
				// The outcome delegate's parameter was renamed out from under this node. Nothing downstream catches
				// this, so say it here rather than let the typed pin quietly read its default.
				CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("MissingReturnValueJsonPin",
					"@@ has no '{0}' output pin, so its typed Return Value pin cannot be decoded. The apply outcome's "
					"parameter has been renamed."),
					FText::FromName(PN_ReturnValueJson)).ToString(), this);
			}
			else
			{
				FName InputParam;
				const FName DecoderName = DecoderFunctionName(Plan.ReturnType, InputParam);

				UK2Node_CallFunction* Decoder = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
				Decoder->FunctionReference.SetExternalMember(DecoderName, UCrowdyEffects::StaticClass());
				Decoder->AllocateDefaultPins();

				UEdGraphPin* DecoderInput = Decoder->FindPin(InputParam, EGPD_Input);
				UEdGraphPin* DecoderReturn = Decoder->GetReturnValuePin();
				if (!DecoderInput || !DecoderReturn)
				{
					CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("MissingReturnDecoder",
						"@@ could not resolve the decoder '{0}' for its typed Return Value pin."),
						FText::FromName(DecoderName)).ToString(), this);
				}
				else
				{
					bReturnValueConnected &= Schema->TryCreateConnection(JsonPin, DecoderInput);
					bReturnValueConnected &=
						CompilerContext.MovePinLinksToIntermediate(*ReturnPin, *DecoderReturn).CanSafeConnect();
				}
			}
		}

		if (!ReturnPin->bOrphanedPin)
		{
			RemovePin(ReturnPin);
		}
	}

	if (!bIsErrorFree)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("ExpandError", "@@: could not assemble the effect's magnitude overrides.").ToString(), this);
	}

	// Kept separate from the magnitude message above: a failure on the return path has nothing to do with the
	// overrides map, and naming the wrong one sends the author looking in the wrong place.
	if (!bReturnValueConnected)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("ReturnExpandError", "@@: could not connect the effect's typed Return Value pin.").ToString(), this);
	}

	Super::ExpandNode(CompilerContext, SourceGraph);
}

#undef LOCTEXT_NAMESPACE
