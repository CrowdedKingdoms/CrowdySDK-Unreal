// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyModelCrossLinks.h"

#include "GameModel/CrowdyGameModelDelete.h"  // the reference scans this composes
#include "GameModel/CrowdyModelSnapshot.h"    // CrowdyModelSnapshotKeys::KeysMatch
#include "GameModel/CrowdyModelVocabulary.h"  // NormalizedToken, TokenIs, AttributeDisplayName(FromKey)

namespace
{
	// Every helper here carries the CrowdyCrossLink prefix. Adaptive unity merges this module's .cpp files into
	// shared translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	// What the reader is told when a link cannot be followed. Each says which of the four silences it is, because
	// "cannot be opened" with no reason reads as a bug in the page rather than as a fact about the data.
	const TCHAR* const CrowdyCrossLinkUnnamedModelReason =
		TEXT("on a model nothing here names, so it cannot be opened from here");
	const TCHAR* const CrowdyCrossLinkUnreportedModelReason =
		TEXT("on a model this app did not report, so it cannot be opened from here");
	const TCHAR* const CrowdyCrossLinkNoModelReason =
		TEXT("which belongs to no model, so it cannot be opened from here");

	// An automation is listed under the model it targets, so naming that model needs the automation itself. When
	// the app's automations do not carry it there is nothing to look the model up in, which is a different silence
	// from an automation that genuinely targets none.
	const TCHAR* const CrowdyCrossLinkUnreportedAutomationReason =
		TEXT("which this app did not report, so it cannot be opened from here");

	// The section of the detail panel a target of this kind lives in.
	FString CrowdyCrossLinkSectionFor(ECrowdyModelRowKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyModelRowKind::Function:
			return TEXT("functions");
		case ECrowdyModelRowKind::Automation:
			return TEXT("automations");
		default:
			return TEXT("attributes");
		}
	}

	FString CrowdyCrossLinkNoun(ECrowdyModelRowKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyModelRowKind::Model:
			return TEXT("model");
		case ECrowdyModelRowKind::Function:
			return TEXT("function");
		case ECrowdyModelRowKind::Automation:
			return TEXT("automation");
		case ECrowdyModelRowKind::LiveInstance:
			return TEXT("live model");
		default:
			return TEXT("attribute");
		}
	}

	/**
	 * One link, phrased. A link is navigable only when a model can be named to open it on; an automation belongs to
	 * no model of its own, so it is navigable through the model it targets and unnavigable when it targets none.
	 *
	 * Reason is what the line says instead of the model when it cannot be followed, and it is required for exactly
	 * that case: an unnavigable line with no reason tells the reader nothing they can act on.
	 *
	 * TargetName stays the raw server key regardless of kind: it is what navigation opens the target row by. Only
	 * an Attribute link's Display substitutes the authored spelling, resolved from AttributeDisplayNames when this
	 * project names one, so a line about hp_max reads "HpMax" the same way the grid beside it does.
	 */
	FCrowdyModelLink CrowdyCrossLinkMake(
		ECrowdyModelRowKind Kind, const FString& TargetModel, const FString& TargetName, const FString& Relation,
		const TCHAR* Reason, const TMap<FString, FString>& AttributeDisplayNames = TMap<FString, FString>())
	{
		FCrowdyModelLink Link;
		Link.Kind = Kind;
		Link.TargetModel = TargetModel;
		Link.TargetSection = CrowdyCrossLinkSectionFor(Kind);
		Link.TargetName = TargetName;
		Link.Relation = Relation;
		Link.bNavigable = !TargetModel.IsEmpty();

		FString DisplayName = TargetName;
		if (Kind == ECrowdyModelRowKind::Attribute)
		{
			if (const FString* Authored = AttributeDisplayNames.Find(TargetName))
			{
				DisplayName = CrowdyModelVocabulary::AttributeDisplayName(TargetName, *Authored);
			}
			else
			{
				DisplayName = CrowdyModelVocabulary::AttributeDisplayNameFromKey(TargetName);
			}
		}

		Link.Display = FString::Printf(TEXT("the %s %s"), *CrowdyCrossLinkNoun(Kind), *DisplayName);
		if (Link.bNavigable)
		{
			// A model's line already names the model. An automation is listed under the model it targets, and
			// naming that model here would read as though the automation belonged to it.
			if (Kind != ECrowdyModelRowKind::Automation && Kind != ECrowdyModelRowKind::Model)
			{
				Link.Display += FString::Printf(TEXT(" on %s"), *TargetModel);
			}
		}
		else
		{
			Link.Display += FString::Printf(TEXT(", %s"), Reason);
		}
		return Link;
	}

	bool CrowdyCrossLinkAlreadyListed(
		const TArray<FCrowdyModelLink>& Links, ECrowdyModelRowKind Kind, const FString& TargetModel,
		const FString& TargetName)
	{
		for (const FCrowdyModelLink& Link : Links)
		{
			if (Link.Kind == Kind
				&& CrowdyModelSnapshotKeys::KeysMatch(Link.TargetModel, TargetModel)
				&& CrowdyModelSnapshotKeys::KeysMatch(Link.TargetName, TargetName))
			{
				return true;
			}
		}
		return false;
	}

	void CrowdyCrossLinkAddUnique(TArray<FCrowdyModelLink>& Links, FCrowdyModelLink&& Link)
	{
		if (!CrowdyCrossLinkAlreadyListed(Links, Link.Kind, Link.TargetModel, Link.TargetName))
		{
			Links.Add(MoveTemp(Link));
		}
	}

	// A mutation whose target is the function's own model. "self" and an empty target both mean that; anything else
	// is a reference to a live model, and nothing in the function says which model type that reference resolves to.
	bool CrowdyCrossLinkTargetsSelf(const FString& Target)
	{
		const FString Token = CrowdyModelVocabulary::NormalizedToken(Target);
		return Token.IsEmpty() || CrowdyModelVocabulary::TokenIs(Token, TEXT("self"));
	}

	// Whether one function writes this key, whatever model the write lands on. The model is deliberately not
	// narrowed: a write through a reference lands on a model the function does not name, and matching only writes
	// to the function's own model would miss exactly those.
	bool CrowdyCrossLinkFunctionWritesKey(const FStudioFunction& Function, const FString& Key)
	{
		for (const FStudioFunctionMutation& Mutation : Function.Mutations)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Mutation.Property, Key))
			{
				return true;
			}
		}
		return false;
	}

	// How many functions of this name the app carries with no model. That is the count behind the one scope
	// ScopesCarryingFunctionName phrases rather than names, and it is what tells a function with no model of its own
	// whether that line is about somebody else or about itself.
	int32 CrowdyCrossLinkCountUnscopedCopies(
		const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& Name)
	{
		int32 Count = 0;
		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (Function.IsValid()
				&& CrowdyModelSnapshotKeys::KeysMatch(Function->Name, Name)
				&& Function->ContainerTypeName.IsEmpty())
			{
				++Count;
			}
		}
		return Count;
	}

	// Whether a scope from ScopesCarryingFunctionName is a model this app really reported. That list ends with one
	// entry phrased rather than named, for a copy whose model was never determined; asking the functions themselves
	// tells the two apart without spelling that phrase a second time here.
	bool CrowdyCrossLinkScopeNamesAModel(
		const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& Name, const FString& Scope)
	{
		if (Scope.IsEmpty())
		{
			return false;
		}
		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (Function.IsValid()
				&& CrowdyModelSnapshotKeys::KeysMatch(Function->Name, Name)
				&& CrowdyModelSnapshotKeys::KeysMatch(Function->ContainerTypeName, Scope))
			{
				return true;
			}
		}
		return false;
	}

	// The clause naming a list the answer could not be computed from, or empty when it was read. Each state gets its
	// own words: a read that never happened, a read still running and a read that failed are three different things
	// to tell a reader, and only one of them is worth waiting for.
	FString CrowdyCrossLinkUnreadClause(const TCHAR* FamilyPlural, ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::NeverRequested:
			return FString::Printf(TEXT("this app's %s have not been read"), FamilyPlural);
		case ECrowdyModelLoadState::Loading:
			return FString::Printf(TEXT("this app's %s are still being read"), FamilyPlural);
		case ECrowdyModelLoadState::Failed:
			return FString::Printf(TEXT("this app's %s could not be read"), FamilyPlural);
		default:
			return FString();
		}
	}

	// The unread clauses as one phrase. Three of them need a real join: "A and B" for two, "A, B and C" for three,
	// because a chain of bare "and" reads as one clause about one list.
	FString CrowdyCrossLinkJoinClauses(const TArray<FString>& Clauses)
	{
		if (Clauses.Num() <= 1)
		{
			return Clauses.Num() == 1 ? Clauses[0] : FString();
		}

		FString Joined;
		for (int32 Index = 0; Index < Clauses.Num() - 1; ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT(", ");
			}
			Joined += Clauses[Index];
		}
		return Joined + TEXT(" and ") + Clauses.Last();
	}

	FString CrowdyCrossLinkCapitalized(const FString& Sentence)
	{
		if (Sentence.IsEmpty())
		{
			return Sentence;
		}
		FString Capitalized = Sentence;
		Capitalized[0] = FChar::ToUpper(Capitalized[0]);
		return Capitalized;
	}

	const FStudioFunction* CrowdyCrossLinkFindFunction(
		const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& OwningType, const FString& Name)
	{
		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (Function.IsValid()
				&& CrowdyModelSnapshotKeys::KeysMatch(Function->Name, Name)
				&& CrowdyModelSnapshotKeys::KeysMatch(Function->ContainerTypeName, OwningType))
			{
				return Function.Get();
			}
		}
		return nullptr;
	}

	const FStudioAutomation* CrowdyCrossLinkFindAutomation(
		const TArray<TSharedPtr<FStudioAutomation>>& Automations, const FString& Name)
	{
		for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
		{
			if (Automation.IsValid() && CrowdyModelSnapshotKeys::KeysMatch(Automation->Name, Name))
			{
				return Automation.Get();
			}
		}
		return nullptr;
	}
}

TArray<FCrowdyModelLink> CrowdyModelCrossLinks::ForAttribute(
	const FString& OwningType, const FString& Key,
	const TArray<TSharedPtr<FStudioFunction>>& Functions,
	const TArray<TSharedPtr<FStudioAutomation>>& Automations,
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers)
{
	TArray<FCrowdyModelLink> Links;
	if (Key.IsEmpty())
	{
		return Links;
	}

	// The writes first, and as their own pass, because "writes it" is the relationship a reader acts on and it must
	// not be reported as the weaker "reads it" that the whole-function scan below would give it.
	for (const TSharedPtr<FStudioFunction>& Function : Functions)
	{
		if (!Function.IsValid() || !CrowdyCrossLinkFunctionWritesKey(*Function, Key))
		{
			continue;
		}
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Function, Function->ContainerTypeName, Function->Name, TEXT("writes it"),
			CrowdyCrossLinkUnreportedModelReason));
	}

	// Everywhere else a function can name the key: its authority gate, the value it answers with, the arguments of
	// every notification it emits, and the delay, dedupe key and bound parameters of every timer it arms.
	for (const TSharedPtr<FStudioFunction>& Function : Functions)
	{
		if (!Function.IsValid()
			|| !CrowdyGameModelDelete::FunctionReferencesAttribute(*Function, OwningType, Key))
		{
			continue;
		}
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Function, Function->ContainerTypeName, Function->Name, TEXT("reads it"),
			CrowdyCrossLinkUnreportedModelReason));
	}

	// The triggers before the selectors: an automation that waits on a change to this key stops firing when the key
	// goes, which is a stronger thing to say about that automation than that its rule mentions the key.
	for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Triggers)
	{
		if (!Trigger.IsValid()
			|| !CrowdyGameModelDelete::TriggerReferencesAttribute(*Trigger, OwningType, Key))
		{
			continue;
		}

		// The model to open is the automation's own target, never the trigger's container filter. An automation is
		// listed under the model it targets, so a link built from the filter opens a model whose automations do not
		// hold it and highlights nothing. An automation the app did not report names no model at all, and the line
		// says that rather than pointing somewhere wrong or vanishing.
		const FStudioAutomation* Fired = CrowdyCrossLinkFindAutomation(Automations, Trigger->AutomationName);
		const FString FiredModel = Fired ? Fired->TargetTypeName : FString();
		const TCHAR* const FiredReason =
			Fired ? CrowdyCrossLinkNoModelReason : CrowdyCrossLinkUnreportedAutomationReason;
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Automation, FiredModel, Trigger->AutomationName,
			TEXT("waits on it"), FiredReason));
	}

	for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
	{
		if (!Automation.IsValid()
			|| !CrowdyGameModelDelete::AutomationReferencesAttribute(*Automation, Key))
		{
			continue;
		}
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Automation, Automation->TargetTypeName, Automation->Name, TEXT("names it"),
			CrowdyCrossLinkNoModelReason));
	}

	return Links;
}

TArray<FCrowdyModelLink> CrowdyModelCrossLinks::ForFunction(
	const FStudioFunction& Function,
	const TArray<TSharedPtr<FStudioFunction>>& Functions,
	const TArray<TSharedPtr<FStudioAutomation>>& Automations,
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
	const TMap<FString, FString>& AttributeDisplayNames)
{
	TArray<FCrowdyModelLink> Links;

	// What it writes. A write through a reference names a key and not the model it belongs to, so that line names
	// the key and says plainly that the model could not be worked out, rather than guessing at this function's own.
	for (const FStudioFunctionMutation& Mutation : Function.Mutations)
	{
		if (Mutation.Property.IsEmpty())
		{
			continue;
		}
		const bool bSelf = CrowdyCrossLinkTargetsSelf(Mutation.Target);
		const FString WrittenModel = bSelf ? Function.ContainerTypeName : FString();
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Attribute, WrittenModel, Mutation.Property, TEXT("written by this"),
			bSelf ? CrowdyCrossLinkUnreportedModelReason : CrowdyCrossLinkUnnamedModelReason,
			AttributeDisplayNames));
	}

	for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
	{
		if (!Automation.IsValid()
			|| !CrowdyGameModelDelete::AutomationReferencesFunction(*Automation, Function.Name))
		{
			continue;
		}
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Automation, Automation->TargetTypeName, Automation->Name, TEXT("runs it"),
			CrowdyCrossLinkNoModelReason));
	}

	for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Triggers)
	{
		if (!Trigger.IsValid()
			|| !CrowdyGameModelDelete::TriggerReferencesFunction(*Trigger, Function.Name))
		{
			continue;
		}

		// The automation's own target model, for the same reason as the attribute pass above: the trigger's
		// container is a filter on what fires it, and the automation is listed under the model it runs against.
		const FStudioAutomation* Fired = CrowdyCrossLinkFindAutomation(Automations, Trigger->AutomationName);
		const FString FiredModel = Fired ? Fired->TargetTypeName : FString();
		const TCHAR* const FiredReason =
			Fired ? CrowdyCrossLinkNoModelReason : CrowdyCrossLinkUnreportedAutomationReason;
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Automation, FiredModel, Trigger->AutomationName,
			TEXT("names it"), FiredReason));
	}

	// Every other model carrying this name. The server resolves some calls by bare name, so a reader standing on one
	// copy needs to know the others exist; this is the first surface that says so.
	const int32 UnscopedCopies = CrowdyCrossLinkCountUnscopedCopies(Functions, Function.Name);
	const int32 UnscopedCopiesElsewhere =
		Function.ContainerTypeName.IsEmpty() ? UnscopedCopies - 1 : UnscopedCopies;
	for (const FString& Scope : CrowdyGameModelDelete::ScopesCarryingFunctionName(Functions, Function.Name))
	{
		if (CrowdyCrossLinkScopeNamesAModel(Functions, Function.Name, Scope))
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Scope, Function.ContainerTypeName))
			{
				continue; // the copy the reader is standing on
			}
			CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
				ECrowdyModelRowKind::Function, Scope, Function.Name, TEXT("same name"),
				CrowdyCrossLinkUnreportedModelReason));
			continue;
		}

		if (UnscopedCopiesElsewhere <= 0)
		{
			continue; // the only copy with no model is this one
		}
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Function, FString(), Function.Name, TEXT("same name"),
			CrowdyCrossLinkUnreportedModelReason));
	}

	return Links;
}

TArray<FCrowdyModelLink> CrowdyModelCrossLinks::ForAutomation(
	const FStudioAutomation& Automation,
	const TArray<TSharedPtr<FStudioFunction>>& Functions,
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
	const TMap<FString, FString>& AttributeDisplayNames)
{
	TArray<FCrowdyModelLink> Links;

	// The function it runs, named without a model on the wire, so every copy of that name is a candidate and all of
	// them are listed rather than one of them picked.
	if (!Automation.FunctionName.IsEmpty())
	{
		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (!Function.IsValid() || !CrowdyModelSnapshotKeys::KeysMatch(Function->Name, Automation.FunctionName))
			{
				continue;
			}
			CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
				ECrowdyModelRowKind::Function, Function->ContainerTypeName, Function->Name, TEXT("run by this"),
				CrowdyCrossLinkUnreportedModelReason));
		}
	}

	// The model it runs against. Asked of the automation's own target rather than through a match against it: a
	// comparison of a value with itself answers nothing, and what is being decided here is only whether the
	// automation names a model at all. An automation that names none belongs to no model and cannot be opened.
	if (!Automation.TargetTypeName.IsEmpty())
	{
		CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
			ECrowdyModelRowKind::Model, Automation.TargetTypeName, Automation.TargetTypeName,
			TEXT("targeted by this"), CrowdyCrossLinkNoModelReason));
	}

	// What its event triggers wait on. A trigger is not a row of its own, so the line goes to the thing it watches:
	// the attribute whose change fires it, or the function whose invocation does.
	for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Triggers)
	{
		if (!Trigger.IsValid()
			|| !CrowdyModelSnapshotKeys::KeysMatch(Trigger->AutomationName, Automation.Name))
		{
			continue;
		}

		if (!Trigger->PropertyKey.IsEmpty())
		{
			CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
				ECrowdyModelRowKind::Attribute, Trigger->ContainerTypeName, Trigger->PropertyKey,
				TEXT("fires this"), CrowdyCrossLinkUnreportedModelReason, AttributeDisplayNames));
		}
		else if (!Trigger->FunctionName.IsEmpty())
		{
			CrowdyCrossLinkAddUnique(Links, CrowdyCrossLinkMake(
				ECrowdyModelRowKind::Function, Trigger->ContainerTypeName, Trigger->FunctionName,
				TEXT("fires this"), CrowdyCrossLinkUnreportedModelReason));
		}
	}

	return Links;
}

TArray<FCrowdyModelLink> CrowdyModelCrossLinks::ForRow(
	const FCrowdyModelRow& Row,
	const TArray<TSharedPtr<FStudioFunction>>& Functions,
	const TArray<TSharedPtr<FStudioAutomation>>& Automations,
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
	const TMap<FString, FString>& AttributeDisplayNames)
{
	switch (Row.Kind)
	{
	case ECrowdyModelRowKind::Attribute:
		// ForAttribute never draws an Attribute-kind link to itself, only Function and Automation ones, so it has
		// no use for the authored-name map.
		return ForAttribute(Row.OwningType, Row.Name, Functions, Automations, Triggers);

	case ECrowdyModelRowKind::Function:
		if (const FStudioFunction* Function = CrowdyCrossLinkFindFunction(Functions, Row.OwningType, Row.Name))
		{
			return ForFunction(*Function, Functions, Automations, Triggers, AttributeDisplayNames);
		}
		return TArray<FCrowdyModelLink>();

	case ECrowdyModelRowKind::Automation:
		if (const FStudioAutomation* Automation = CrowdyCrossLinkFindAutomation(Automations, Row.Name))
		{
			return ForAutomation(*Automation, Functions, Triggers, AttributeDisplayNames);
		}
		return TArray<FCrowdyModelLink>();

	default:
		// A model row and a live model have no cross-links on this page: the Models tab never reads live models, so
		// a link to one would need a read this page does not make.
		return TArray<FCrowdyModelLink>();
	}
}

FString CrowdyModelCrossLinks::SummaryLine(
	int32 LinkCount, ECrowdyModelLoadState FunctionsState, ECrowdyModelLoadState AutomationsState,
	ECrowdyModelLoadState AutomationTriggersState)
{
	// Every list the answer is computed from, named when it could not be read. The event triggers are one of them
	// on their own terms: they arrive in a second read chained behind the automations and can fail while the
	// automations themselves are present, and every link saying an automation waits on something comes from that
	// read alone. Leaving them out turns a zero from an unread list into a flat "nothing else names this".
	TArray<FString> Clauses;
	const auto AddClause = [&Clauses](const TCHAR* FamilyPlural, ECrowdyModelLoadState State)
	{
		const FString Clause = CrowdyCrossLinkUnreadClause(FamilyPlural, State);
		if (!Clause.IsEmpty())
		{
			Clauses.Add(Clause);
		}
	};

	AddClause(TEXT("functions"), FunctionsState);
	AddClause(TEXT("automations"), AutomationsState);
	AddClause(TEXT("automation triggers"), AutomationTriggersState);

	const FString Caveat = CrowdyCrossLinkJoinClauses(Clauses);

	const FString Count = LinkCount == 1
		? FString(TEXT("1 thing names this"))
		: FString::Printf(TEXT("%d things name this"), LinkCount);

	if (Caveat.IsEmpty())
	{
		return LinkCount <= 0 ? FString(TEXT("Nothing else names this.")) : Count + TEXT(".");
	}

	// A zero from a list nobody read is not a zero, and a count from one is a floor rather than a total. Both say
	// which list is missing, because the reader can act on that and cannot act on "unknown".
	if (LinkCount <= 0)
	{
		return CrowdyCrossLinkCapitalized(Caveat) + TEXT(", so nothing can be said about what names this.");
	}
	return Count + TEXT(", but ") + Caveat + TEXT(", so there may be more.");
}
