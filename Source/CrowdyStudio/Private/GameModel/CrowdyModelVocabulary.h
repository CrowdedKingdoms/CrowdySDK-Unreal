// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyGameModelDelete.h" // ECrowdyDeleteKind, ECrowdyDeleteSeverity, ECrowdyDeleteFindingKind
#include "GameModel/CrowdyModelLedger.h" // ECrowdyModelProvenance, ECrowdyModelDrift
#include "Model/CrowdyStudioTypes.h"

// The one place a raw server enum string becomes designer-facing text for the Models browser. Pure: no Slate,
// no HTTP, no UObject, no world, so it can be exercised without spinning up any of those.
//
// Every label falls through to the raw server string when it does not recognize a value. The server's
// vocabulary can grow without this header knowing, and a blank cell where a type or a scope should be is
// indistinguishable from a bug, so an unfamiliar value is shown verbatim rather than guessed at or dropped.
//
// Every function here is `inline` and this is their only home, because the unity build merges this module's
// .cpp files into fewer translation units: two identical free-function definitions in different .cpp files
// would redefine each other and fail to compile, nondeterministically depending on how the blob is cut. For
// the same reason there is no LOCTEXT_NAMESPACE here; use NSLOCTEXT with an explicit namespace if a caller
// needs FText.
namespace CrowdyModelVocabulary
{
	// Server enum values are lowercase tokens. Comparing them with FString::operator== would be
	// case-INSENSITIVE, which is the wrong default for a server key, so the input is normalized once here and
	// every comparison below is then explicitly case-sensitive against a lowercase literal. Callers keep the
	// original string for the fall-through, so normalizing never changes what an unrecognized value renders as.
	inline FString NormalizedToken(const FString& Raw)
	{
		return Raw.TrimStartAndEnd().ToLower();
	}

	inline bool TokenIs(const FString& Normalized, const TCHAR* Lowercase)
	{
		return Normalized.Equals(Lowercase, ESearchCase::CaseSensitive);
	}

	// The attribute's storage type in plain words. "container_ref" is the server's link-to-another-model type.
	inline FString ValueTypeLabel(const FString& ValueType)
	{
		const FString Token = NormalizedToken(ValueType);
		if (TokenIs(Token, TEXT("int")))           { return TEXT("Number"); }
		if (TokenIs(Token, TEXT("float")))         { return TEXT("Decimal"); }
		if (TokenIs(Token, TEXT("string")))        { return TEXT("Text"); }
		if (TokenIs(Token, TEXT("bool")))          { return TEXT("True or false"); }
		if (TokenIs(Token, TEXT("json")))          { return TEXT("Structured data"); }
		if (TokenIs(Token, TEXT("object")))        { return TEXT("Structured data"); }
		if (TokenIs(Token, TEXT("array")))         { return TEXT("List"); }
		if (TokenIs(Token, TEXT("container_ref"))) { return TEXT("Link to a model"); }
		return ValueType;
	}

	// An attribute's server key turned back into a readable name: the key is split on underscores and each part
	// gets a capital, so "crop_stage" reads CropStage rather than cropstage. A key that already carries capitals
	// keeps them, and underscores at either end or doubled up add nothing. An empty key yields an empty name.
	//
	// This is a reconstruction, not a recovery: an attribute is addressed by a lowercased key, so a name that ran
	// two words together without an underscore cannot be split back apart. It is what a surface shows for an
	// attribute nothing in the project declares, which is the only case where no authored spelling exists to use.
	inline FString AttributeDisplayNameFromKey(const FString& ServerKey)
	{
		const FString Trimmed = ServerKey.TrimStartAndEnd();

		FString Display;
		Display.Reserve(Trimmed.Len());

		bool bStartOfWord = true;
		for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
		{
			const TCHAR Character = Trimmed[Index];
			if (Character == TEXT('_'))
			{
				bStartOfWord = true;
				continue;
			}
			Display.AppendChar(bStartOfWord ? FChar::ToUpper(Character) : Character);
			bStartOfWord = false;
		}
		return Display;
	}

	// The name one attribute is SHOWN under. AuthoredName is the UPROPERTY spelling the project declares it with,
	// which is the answer whenever the project has one: deriving the server key lowercases the name, so a read of
	// the server alone can never tell "hpregen" was authored HPRegen. Falls back to reconstructing the name from
	// the key above, which is all there is for an attribute only the server has.
	//
	// A LABEL, never identity. The server key is what addresses the attribute, and this must not be compared,
	// stored or sent as one. FString comparison and TMap<FString> fold case in Unreal, so a display string that
	// reached an identity test would match in some places and not others rather than failing outright.
	inline FString AttributeDisplayName(const FString& ServerKey, const FString& AuthoredName)
	{
		const FString Authored = AuthoredName.TrimStartAndEnd();
		return Authored.IsEmpty() ? AttributeDisplayNameFromKey(ServerKey) : Authored;
	}

	// Who may READ the attribute.
	inline FString VisibilityLabel(const FString& Visibility)
	{
		const FString Token = NormalizedToken(Visibility);
		if (TokenIs(Token, TEXT("public")))  { return TEXT("Everyone"); }
		if (TokenIs(Token, TEXT("owner")))   { return TEXT("Owner only"); }
		if (TokenIs(Token, TEXT("private"))) { return TEXT("Owner only"); }
		if (TokenIs(Token, TEXT("hidden")))  { return TEXT("Server only"); }
		return Visibility;
	}

	// Who may WRITE the attribute directly, as opposed to through a function.
	inline FString WritableLabel(const FString& Writable)
	{
		const FString Token = NormalizedToken(Writable);
		if (TokenIs(Token, TEXT("function"))) { return TEXT("Only a function"); }
		if (TokenIs(Token, TEXT("owner")))    { return TEXT("The owner"); }
		if (TokenIs(Token, TEXT("admin")))    { return TEXT("Admins only"); }
		if (TokenIs(Token, TEXT("server")))   { return TEXT("The server only"); }
		if (TokenIs(Token, TEXT("anyone")))   { return TEXT("Anyone"); }
		return Writable;
	}

	// Who may call the function. "internal" reads as something a designer recognizes because it is what a
	// warning about an unreachable function has to say.
	inline FString InvokeScopeLabel(const FString& Scope)
	{
		const FString Token = NormalizedToken(Scope);
		if (TokenIs(Token, TEXT("player")))   { return TEXT("Players"); }
		if (TokenIs(Token, TEXT("server")))   { return TEXT("The server only"); }
		if (TokenIs(Token, TEXT("admin")))    { return TEXT("Admins only"); }
		if (TokenIs(Token, TEXT("internal"))) { return TEXT("Only other functions"); }
		return Scope;
	}

	// A repeat period in the largest whole unit it divides into, so 300000 reads "5 minutes" rather than
	// "300000 milliseconds". Empty for a non-positive period, which the caller renders as an unset schedule.
	inline FString IntervalPhrase(int32 IntervalMs)
	{
		if (IntervalMs <= 0)
		{
			return FString();
		}

		int32 Count = IntervalMs;
		const TCHAR* Unit = TEXT("millisecond");
		if (IntervalMs % 3600000 == 0)
		{
			Count = IntervalMs / 3600000;
			Unit = TEXT("hour");
		}
		else if (IntervalMs % 60000 == 0)
		{
			Count = IntervalMs / 60000;
			Unit = TEXT("minute");
		}
		else if (IntervalMs % 1000 == 0)
		{
			Count = IntervalMs / 1000;
			Unit = TEXT("second");
		}

		return Count == 1
			? FString(Unit)
			: FString::Printf(TEXT("%d %s"), Count, *(FString(Unit) + TEXT("s")));
	}

	// What an event trigger reacts to, in plain words. The server names the event with a raw token
	// ("property_changed", "function_invoked", "container_created"), which is jargon nobody outside the wire format
	// recognizes, so it never reaches the screen unmapped. Where the trigger narrows the event to one property key or
	// one function, that name is folded into the phrase, because "when Health changes" is the answer the reader
	// wanted. A trigger naming no event at all yields no phrase, so the caller keeps whatever it had.
	inline FString OnEventLabel(const FStudioAutomationTrigger& Trigger)
	{
		const FString Event = NormalizedToken(Trigger.OnEvent);
		if (Event.IsEmpty())
		{
			return FString();
		}

		if (TokenIs(Event, TEXT("property_changed")))
		{
			const FString Key = Trigger.PropertyKey.TrimStartAndEnd();
			return Key.IsEmpty()
				? FString(TEXT("Runs when a value changes"))
				: FString::Printf(TEXT("Runs when %s changes"), *Key);
		}
		if (TokenIs(Event, TEXT("function_invoked")))
		{
			const FString FunctionName = Trigger.FunctionName.TrimStartAndEnd();
			return FunctionName.IsEmpty()
				? FString(TEXT("Runs after a function runs"))
				: FString::Printf(TEXT("Runs after %s runs"), *FunctionName);
		}
		if (TokenIs(Event, TEXT("container_created")))
		{
			return TEXT("Runs when a live model is created");
		}
		if (TokenIs(Event, TEXT("player_left")))
		{
			return TEXT("Runs when a player leaves, the last one included");
		}
		if (TokenIs(Event, TEXT("player_count_changed")))
		{
			return TEXT("Runs when the player count changes");
		}

		return FString::Printf(TEXT("Runs when %s"), *Trigger.OnEvent.TrimStartAndEnd());
	}

	// One short phrase for when an automation runs, covering the interval, cron and event cases. This is the
	// automation row's secondary text, so it must say something for every automation the server can return.
	inline FString AutomationTriggerLabel(const FStudioAutomation& Automation)
	{
		const FString Trigger = NormalizedToken(Automation.TriggerType);

		if (TokenIs(Trigger, TEXT("event")))
		{
			return TEXT("Runs when something happens");
		}

		if (TokenIs(Trigger, TEXT("schedule")))
		{
			const FString Schedule = NormalizedToken(Automation.ScheduleKind);
			if (TokenIs(Schedule, TEXT("cron")))
			{
				const FString Expr = Automation.CronExpr.TrimStartAndEnd();
				return Expr.IsEmpty()
					? FString(TEXT("Runs on a schedule"))
					: FString::Printf(TEXT("Runs on a schedule (%s)"), *Expr);
			}

			const FString Every = IntervalPhrase(Automation.IntervalMs);
			return Every.IsEmpty()
				? FString(TEXT("Runs on a timer"))
				: FString::Printf(TEXT("Runs every %s"), *Every);
		}

		return Automation.TriggerType.IsEmpty() ? FString(TEXT("No schedule set")) : Automation.TriggerType;
	}

	// Where a row came from, for the Source column. Deliberately silent for Unknown: with no plan run, or for an
	// entity nothing could be checked against, there is no answer to give, and a healthy app must read as plain grey
	// rather than as a page full of badges. These five words are also what the Crowdy Effect asset's own toolbar
	// says, so a designer reads one vocabulary in both places.
	inline FString ProvenanceLabel(ECrowdyModelProvenance Provenance)
	{
		switch (Provenance)
		{
		case ECrowdyModelProvenance::CodeSynced:    return TEXT("code-synced");
		case ECrowdyModelProvenance::CodeNotPushed: return TEXT("code-not-pushed");
		case ECrowdyModelProvenance::CodeDrifted:   return TEXT("code-drifted");
		case ECrowdyModelProvenance::ServerOnly:    return TEXT("server-only");
		case ECrowdyModelProvenance::KitOwned:      return TEXT("kit-owned");
		default:                                    return FString();
		}
	}

	// How a row differs from the project, for the Status column. Empty for None, which is the healthy answer and the
	// most common one: a column that says something on every row says nothing on any of them.
	inline FString DriftLabel(ECrowdyModelDrift Drift)
	{
		switch (Drift)
		{
		case ECrowdyModelDrift::NotOnServerYet:  return TEXT("Not on server yet");
		case ECrowdyModelDrift::ChangedInCode:   return TEXT("Changed in code");
		case ECrowdyModelDrift::OnlyOnServer:    return TEXT("Only on server");
		case ECrowdyModelDrift::NeedsAFix:       return TEXT("Needs a fix");
		case ECrowdyModelDrift::CannotBeChecked: return TEXT("Cannot be checked");
		default:                                 return FString();
		}
	}

	// The noun for what one delete acts on. The delete layer composes whole sentences out of these, so this is the
	// only place any of these five words is spelled, and a page that renamed "model" would rename it once here.
	// The plural is a parameter rather than a second function because every count line needs both forms and a
	// caller that had to pick between two functions would eventually pick the wrong one for a count of 1.
	inline FString DeleteKindNoun(ECrowdyDeleteKind Kind, bool bPlural)
	{
		switch (Kind)
		{
		case ECrowdyDeleteKind::Model:        return bPlural ? TEXT("models") : TEXT("model");
		case ECrowdyDeleteKind::Attribute:    return bPlural ? TEXT("attributes") : TEXT("attribute");
		case ECrowdyDeleteKind::Function:     return bPlural ? TEXT("functions") : TEXT("function");
		case ECrowdyDeleteKind::Automation:   return bPlural ? TEXT("automations") : TEXT("automation");
		case ECrowdyDeleteKind::LiveInstance: return bPlural ? TEXT("live models") : TEXT("live model");
		default:                              return bPlural ? TEXT("entries") : TEXT("entry");
		}
	}

	// How bad a finding is, in one word a reader can scan a column of. "Blocked" rather than "Blocker" because it
	// describes the delete's state, which is what the reader is deciding about, not a category of finding.
	inline FString DeleteSeverityLabel(ECrowdyDeleteSeverity Severity)
	{
		switch (Severity)
		{
		case ECrowdyDeleteSeverity::Blocker: return TEXT("Blocked");
		case ECrowdyDeleteSeverity::Caution: return TEXT("Caution");
		default:                             return TEXT("What happens");
		}
	}

	// The short caption above one finding's sentence. It states the consequence rather than naming the rule that
	// produced it, so a reader who stops at the caption has still learned something true.
	inline FString DeleteFindingLabel(ECrowdyDeleteFindingKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyDeleteFindingKind::ModelHasLiveModels:            return TEXT("Live models still exist");
		case ECrowdyDeleteFindingKind::ModelLiveCountUnknown:         return TEXT("Live models were not counted");
		case ECrowdyDeleteFindingKind::ModelHasBoundFunctions:        return TEXT("Functions are still bound to this model");
		case ECrowdyDeleteFindingKind::AttributeOrphansStoredValues:  return TEXT("Stored values are left behind");
		case ECrowdyDeleteFindingKind::AttributeReadByFunctions:      return TEXT("Functions name this attribute");
		case ECrowdyDeleteFindingKind::AttributeUsedByAutomations:    return TEXT("Automations name this attribute");
		case ECrowdyDeleteFindingKind::ModelCascadeOrphansAttributeReaders:
			return TEXT("Things still name the attributes this removes");
		case ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp:    return TEXT("This name is on more than one model");
		case ECrowdyDeleteFindingKind::FunctionRunByAutomations:      return TEXT("Automations run this function");
		case ECrowdyDeleteFindingKind::ModelTargetedByAutomations:    return TEXT("Automations run against this model");
		case ECrowdyDeleteFindingKind::LiveModelIsRecreatedByItsBinding: return TEXT("The runtime creates this again");
		case ECrowdyDeleteFindingKind::RecreatedByTheNextSync:        return TEXT("The next sync puts this back");
		case ECrowdyDeleteFindingKind::ModelCascadesItsAttributes:    return TEXT("Its attributes go with it");
		case ECrowdyDeleteFindingKind::ModelCascadesItsPlumbing:      return TEXT("Its hidden wiring goes with it");
		case ECrowdyDeleteFindingKind::AutomationCascadesItsTriggers: return TEXT("Its triggers go with it");
		case ECrowdyDeleteFindingKind::LiveModelCascadesItsValuesAndEdges: return TEXT("Its values and links go with it");
		default:                                                     return FString();
		}
	}

	// Every provenance a row can carry, in the order a legend reads them: the three that mean the project declares
	// this entity, then the two that mean it does not, then the one that means nothing has been checked. A legend
	// built by walking this cannot fall out of step with the shapes the gutter paints, which is the whole reason it
	// exists rather than a list typed out beside the widget.
	inline const TArray<ECrowdyModelProvenance>& AllProvenances()
	{
		static const TArray<ECrowdyModelProvenance> Values = {
			ECrowdyModelProvenance::CodeSynced,
			ECrowdyModelProvenance::CodeNotPushed,
			ECrowdyModelProvenance::CodeDrifted,
			ECrowdyModelProvenance::ServerOnly,
			ECrowdyModelProvenance::KitOwned,
			ECrowdyModelProvenance::Unknown
		};
		return Values;
	}

	inline const TArray<ECrowdyModelDrift>& AllDrifts()
	{
		static const TArray<ECrowdyModelDrift> Values = {
			ECrowdyModelDrift::NotOnServerYet,
			ECrowdyModelDrift::ChangedInCode,
			ECrowdyModelDrift::OnlyOnServer,
			ECrowdyModelDrift::NeedsAFix,
			ECrowdyModelDrift::CannotBeChecked,
			ECrowdyModelDrift::None
		};
		return Values;
	}

	// What a Source mark actually tells the reader. The words alone are a vocabulary, not an explanation: someone
	// meeting "code-drifted" for the first time can read it and still not know which side is ahead, which is the one
	// thing they need in order to decide what to do about it. Each sentence therefore says where the entity exists
	// and how the two sides compare, in that order.
	inline FString ProvenanceMeaning(ECrowdyModelProvenance Provenance)
	{
		switch (Provenance)
		{
		case ECrowdyModelProvenance::CodeSynced:
			return TEXT("This project declares it and the server matches. Nothing to do.");
		case ECrowdyModelProvenance::CodeNotPushed:
			return TEXT("This project declares it and the server does not have it yet. Sync to Server adds it.");
		case ECrowdyModelProvenance::CodeDrifted:
			return TEXT("Both have it and they differ. Sync to Server makes the server match this project.");
		case ECrowdyModelProvenance::ServerOnly:
			return TEXT("Nothing in this project declares it. Only something outside this project put it there.");
		case ECrowdyModelProvenance::KitOwned:
			return TEXT("A game kit deployed it and owns it. Leave it to the kit.");
		default:
			// Unknown paints no mark at all, so the sentence has to name the absence rather than describe a shape.
			return TEXT("No mark: nothing has been checked yet, or this entity could not be checked. Press Preview changes.");
		}
	}

	// What a Status word means for the reader's next move. Only the minority of rows carry one; the sentence for the
	// healthy case exists so a legend can say why most rows say nothing, which is otherwise read as a failed load.
	inline FString DriftMeaning(ECrowdyModelDrift Drift)
	{
		switch (Drift)
		{
		case ECrowdyModelDrift::NotOnServerYet:
			return TEXT("The next Sync to Server will create it.");
		case ECrowdyModelDrift::ChangedInCode:
			return TEXT("The next Sync to Server will update it to match this project.");
		case ECrowdyModelDrift::OnlyOnServer:
			return TEXT("No Sync will ever touch it, because nothing here describes it.");
		case ECrowdyModelDrift::NeedsAFix:
			return TEXT("Two assets claim this name, so neither is synced. Rename one of them.");
		case ECrowdyModelDrift::CannotBeChecked:
			return TEXT("Its author could not be read, so nothing can be said about how it compares.");
		default:
			return TEXT("Blank: this row matches the project, which is the healthy and most common case.");
		}
	}

	// What the automation runs against, in plain words.
	inline FString AutomationTargetLabel(const FStudioAutomation& Automation)
	{
		const FString Mode = NormalizedToken(Automation.TargetMode);

		if (TokenIs(Mode, TEXT("type")))
		{
			const FString TypeName = Automation.TargetTypeName.TrimStartAndEnd();
			return TypeName.IsEmpty()
				? FString(TEXT("Every model of one kind"))
				: FString::Printf(TEXT("Every %s"), *TypeName);
		}
		if (TokenIs(Mode, TEXT("container")))
		{
			return TEXT("One chosen live model");
		}
		if (TokenIs(Mode, TEXT("global")))
		{
			return TEXT("The whole app");
		}

		return Automation.TargetMode.IsEmpty() ? FString(TEXT("No target set")) : Automation.TargetMode;
	}
}
