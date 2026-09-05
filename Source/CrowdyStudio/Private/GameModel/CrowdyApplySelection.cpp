// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyApplySelection.h"

#include "Containers/StringConv.h"
#include "GameModel/CrowdyModelLedger.h"     // IsReservedAttribute / IsReservedFunction
#include "GameModel/CrowdyModelSnapshot.h"   // CrowdyModelSnapshotKeys::KeysMatch
#include "GameModel/CrowdyModelVocabulary.h" // DeleteKindNoun, OnEventLabel, NormalizedToken, AttributeDisplayName
#include "Misc/SecureHash.h"
#include "Model/CrowdyStudioTypes.h"         // FStudioAutomationTrigger
#include "Templates/Function.h"

namespace
{
	// Every helper here is prefixed, because the unity build merges this module's .cpp files into one translation
	// unit and an unprefixed name would collide with one of the others.

	bool CrowdyApplyIsIdentifierChar(TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TEXT('_');
	}

	// A fixed-width digest of a canonical text, over UTF-8 bytes so it does not depend on the character width the
	// editor happens to be built with. Nothing reads this back; it is only ever compared against another one.
	FString CrowdyApplyDigest(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, sizeof(Digest));
	}

	// The token that leads an identity key. A word rather than an enum's integer, so a key that travels through a
	// stored selection and comes back stays readable and does not silently change meaning when a value is inserted
	// into the enum.
	const TCHAR* CrowdyApplyKindToken(ECrowdyApplyKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyApplyKind::Type:       return TEXT("type");
		case ECrowdyApplyKind::Attribute:  return TEXT("attribute");
		case ECrowdyApplyKind::Function:   return TEXT("function");
		case ECrowdyApplyKind::Automation: return TEXT("automation");
		case ECrowdyApplyKind::Trigger:    return TEXT("trigger");
		default:                           return TEXT("unknown");
		}
	}

	// The noun for what one apply acts on. Four of the five are the bound nouns the delete layer already spells, so
	// they come from the one place this page spells them; only the trigger, which has no delete of its own, is
	// named here.
	FString CrowdyApplyKindNoun(ECrowdyApplyKind Kind, bool bPlural)
	{
		switch (Kind)
		{
		case ECrowdyApplyKind::Type:       return CrowdyModelVocabulary::DeleteKindNoun(ECrowdyDeleteKind::Model, bPlural);
		case ECrowdyApplyKind::Attribute:  return CrowdyModelVocabulary::DeleteKindNoun(ECrowdyDeleteKind::Attribute, bPlural);
		case ECrowdyApplyKind::Function:   return CrowdyModelVocabulary::DeleteKindNoun(ECrowdyDeleteKind::Function, bPlural);
		case ECrowdyApplyKind::Automation: return CrowdyModelVocabulary::DeleteKindNoun(ECrowdyDeleteKind::Automation, bPlural);
		case ECrowdyApplyKind::Trigger:    return bPlural ? TEXT("triggers") : TEXT("trigger");
		default:                           return bPlural ? TEXT("entries") : TEXT("entry");
		}
	}

	FString CrowdyApplyCountPhrase(int32 Count, ECrowdyApplyKind Kind)
	{
		return FString::Printf(TEXT("%d %s"), Count, *CrowdyApplyKindNoun(Kind, Count != 1));
	}

	FString CrowdyApplyChangePhrase(int32 Count)
	{
		return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? TEXT("change") : TEXT("changes"));
	}

	FString CrowdyApplyJoinPhrases(const TArray<FString>& Parts)
	{
		if (Parts.Num() == 0)
		{
			return FString();
		}
		if (Parts.Num() == 1)
		{
			return Parts[0];
		}

		FString Joined;
		for (int32 Index = 0; Index < Parts.Num() - 1; ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT(", ");
			}
			Joined += Parts[Index];
		}
		return Joined + TEXT(" and ") + Parts.Last();
	}

	// A unit named the way the reason sentences name it, so both ends of an addition read as one sentence.
	FString CrowdyApplyDescribeUnit(const FCrowdyApplyUnit& Unit)
	{
		switch (Unit.Kind)
		{
		case ECrowdyApplyKind::Type:
			return FString::Printf(TEXT("the model %s"), *Unit.Name);

		case ECrowdyApplyKind::Attribute:
			return Unit.OwningType.IsEmpty()
				? FString::Printf(TEXT("the attribute %s"), *Unit.Name)
				: FString::Printf(TEXT("the attribute %s on %s"), *Unit.Name, *Unit.OwningType);

		case ECrowdyApplyKind::Function:
			return Unit.OwningType.IsEmpty()
				? FString::Printf(TEXT("the function %s"), *Unit.Name)
				: FString::Printf(TEXT("the function %s on %s"), *Unit.Name, *Unit.OwningType);

		case ECrowdyApplyKind::Automation:
			return FString::Printf(TEXT("the automation %s"), *Unit.Name);

		case ECrowdyApplyKind::Trigger:
			return FString::Printf(TEXT("the trigger on %s"), *Unit.Name);

		default:
			return Unit.Name;
		}
	}

	// A function named the short way the write and call sentences use: "take_damage on Knight", or the bare name
	// when nothing determined its model.
	FString CrowdyApplyShortFunctionPhrase(const FCrowdyApplyUnit& Unit)
	{
		return Unit.OwningType.IsEmpty()
			? Unit.Name
			: FString::Printf(TEXT("%s on %s"), *Unit.Name, *Unit.OwningType);
	}

	// The plan's arrays read through a null-safe accessor, because the controller fills the input by pointer and a
	// caller that has only some of the five is still owed an answer.
	template <typename UpsertType>
	const UpsertType* CrowdyApplyEntry(const TArray<UpsertType>* Array, int32 Index)
	{
		return Array && Array->IsValidIndex(Index) ? &(*Array)[Index] : nullptr;
	}

	template <typename UpsertType>
	int32 CrowdyApplyNum(const TArray<UpsertType>* Array)
	{
		return Array ? Array->Num() : 0;
	}

	// A trigger's identity beyond its automation name, and it must stay the same field list the schema diff keys a
	// trigger on: the event plus every filter, with the write source deliberately left out because the diff treats
	// that as a tunable it updates a trigger with rather than as a different trigger. A key finer than the diff's
	// silently drops a selection whenever one of the extra fields changes across a re-plan, and a coarser one folds
	// two distinct triggers into one, so matching it exactly is the only reading with neither failure.
	//
	// The container filter is repeated here even though a trigger unit also carries it as its owning type, so that
	// this field alone, alongside the automation name, is the whole key.
	FString CrowdyApplyTriggerDiscriminator(const FCrowdyGameModelAutomationTriggerInput& Trigger)
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"),
			*Trigger.OnEvent, *Trigger.FunctionName, *Trigger.ContainerTypeName, *Trigger.PropertyKey);
	}

	// A mutation whose target is the function's own model. "self" and an empty target both mean that; anything else
	// is a reference this struct does not resolve to a model, which the closure handles by over-including.
	bool CrowdyApplyTargetsSelf(const FString& Target)
	{
		const FString Token = CrowdyModelVocabulary::NormalizedToken(Target);
		return Token.IsEmpty() || CrowdyModelVocabulary::TokenIs(Token, TEXT("self"));
	}

	void CrowdyApplyAddUniqueName(TArray<FString>& Names, const FString& Name)
	{
		if (!Name.IsEmpty() && !CrowdyModelSnapshotKeys::Contains(Names, Name))
		{
			Names.Add(Name);
		}
	}

	// Every fn:<name> callee in one expression, appended in first-seen order. The name runs to the end of its
	// identifier, so fn:regen_all names regen_all and never regen; the "fn:" itself must start a word, so a longer
	// token that happens to end in fn: is not read as a call.
	void CrowdyApplyCollectCallees(const FString& Text, TArray<FString>& InOutCallees)
	{
		static const FString Marker = TEXT("fn:");
		if (Text.IsEmpty())
		{
			return;
		}

		int32 SearchFrom = 0;
		while (SearchFrom <= Text.Len() - Marker.Len())
		{
			const int32 Found = Text.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Found == INDEX_NONE)
			{
				return;
			}

			SearchFrom = Found + Marker.Len();

			const bool bStartsWord = Found == 0 || !CrowdyApplyIsIdentifierChar(Text[Found - 1]);
			if (bStartsWord)
			{
				int32 End = SearchFrom;
				while (End < Text.Len() && CrowdyApplyIsIdentifierChar(Text[End]))
				{
					++End;
				}
				if (End > SearchFrom)
				{
					CrowdyApplyAddUniqueName(InOutCallees, Text.Mid(SearchFrom, End - SearchFrom));
				}
			}
		}
	}

	// What one closure step needs, resolved into the lookup the worklist runs. Kept as data so the sentence and the
	// lookup are written once each rather than once per edge.
	enum class ECrowdyApplyEdge : uint8
	{
		AttributeNeedsType,
		FunctionNeedsType,
		FunctionWritesOwnAttribute,
		FunctionWritesUnscopedAttribute,
		FunctionSchedulesFunction,
		FunctionCallsFunction,
		AutomationRunsFunction,
		AutomationTargetsType,
		TriggerFiresAutomation
	};

	struct FCrowdyApplyRequirement
	{
		ECrowdyApplyEdge Edge = ECrowdyApplyEdge::AttributeNeedsType;

		// What kind of unit satisfies it.
		ECrowdyApplyKind Kind = ECrowdyApplyKind::Type;

		// The scope the candidate must carry, or empty when the reference names no scope. An empty scope here means
		// "any model", which is the over-inclusion rule: the reference names a key or a name and nothing names the
		// model it belongs to, so every candidate of that name in the plan is required.
		FString OwningType;
		bool bScoped = false;

		FString Name;

		// The key an unscoped write names, carried so the sentence can say which one could not be narrowed down.
		FString WrittenKey;
	};

	FString CrowdyApplyBecause(
		const FCrowdyApplyUnit& From, const FCrowdyApplyRequirement& Requirement)
	{
		switch (Requirement.Edge)
		{
		case ECrowdyApplyEdge::AttributeNeedsType:
			return FString::Printf(TEXT("Added because %s needs it."), *CrowdyApplyDescribeUnit(From));

		case ECrowdyApplyEdge::FunctionNeedsType:
			return FString::Printf(TEXT("Added because the function %s needs the model %s."),
				*From.Name, *Requirement.Name);

		case ECrowdyApplyEdge::FunctionWritesOwnAttribute:
			return FString::Printf(TEXT("Added because %s writes it."), *CrowdyApplyShortFunctionPhrase(From));

		case ECrowdyApplyEdge::FunctionWritesUnscopedAttribute:
			return FString::Printf(
				TEXT("Added because %s writes %s on another model this plan could not narrow down."),
				*CrowdyApplyShortFunctionPhrase(From), *Requirement.WrittenKey);

		case ECrowdyApplyEdge::FunctionSchedulesFunction:
			return FString::Printf(TEXT("Added because %s schedules it."), *CrowdyApplyShortFunctionPhrase(From));

		case ECrowdyApplyEdge::FunctionCallsFunction:
			return FString::Printf(TEXT("Added because %s calls it."), *CrowdyApplyShortFunctionPhrase(From));

		case ECrowdyApplyEdge::AutomationRunsFunction:
			return FString::Printf(TEXT("Added because the automation %s runs it."), *From.Name);

		case ECrowdyApplyEdge::AutomationTargetsType:
			return FString::Printf(TEXT("Added because the automation %s runs against it."), *From.Name);

		case ECrowdyApplyEdge::TriggerFiresAutomation:
			return FString::Printf(TEXT("Added because the trigger on %s fires it."), *From.Name);

		default:
			return FString::Printf(TEXT("Added because %s needs it."), *CrowdyApplyDescribeUnit(From));
		}
	}

	// Everything one unit requires, in the order the edges are numbered. The trigger's own FunctionName,
	// ContainerTypeName and PropertyKey are deliberately absent: they are FILTERS, not references. A trigger whose
	// filter names something the server does not have simply never matches, so nothing is broken; treating a filter
	// as a prerequisite would drag half a plan in behind one trigger.
	TArray<FCrowdyApplyRequirement> CrowdyApplyRequirementsOf(
		const FCrowdyApplyUnit& Unit, const FCrowdyApplyPlanInput& Plan)
	{
		TArray<FCrowdyApplyRequirement> Requirements;

		switch (Unit.Kind)
		{
		case ECrowdyApplyKind::Attribute:
		{
			if (!Unit.OwningType.IsEmpty())
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::AttributeNeedsType;
				Requirement.Kind = ECrowdyApplyKind::Type;
				Requirement.Name = Unit.OwningType;
				Requirements.Add(MoveTemp(Requirement));
			}
			break;
		}

		case ECrowdyApplyKind::Function:
		{
			const FCrowdySchemaFunctionUpsert* Upsert = CrowdyApplyEntry(Plan.Functions, Unit.PlanIndex);
			if (!Upsert)
			{
				break;
			}
			const FCrowdyGameModelFunctionInput& Function = Upsert->Function;

			if (!Function.ContainerTypeName.IsEmpty())
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::FunctionNeedsType;
				Requirement.Kind = ECrowdyApplyKind::Type;
				Requirement.Name = Function.ContainerTypeName;
				Requirements.Add(MoveTemp(Requirement));
			}

			for (const TPair<FString, FString>& Write : CrowdyApplySelection::FunctionWrites(Function))
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Kind = ECrowdyApplyKind::Attribute;
				Requirement.Name = Write.Value;
				Requirement.WrittenKey = Write.Value;
				if (Write.Key.IsEmpty())
				{
					// Nothing in the compiled function names the model this key belongs to, so every new attribute
					// of that key in the plan is required. Over-inclusion is the only safe direction: one extra
					// idempotent upsert costs a round trip, while omitting the right one writes a function whose
					// mutation names a key the server does not have.
					Requirement.Edge = ECrowdyApplyEdge::FunctionWritesUnscopedAttribute;
					Requirement.bScoped = false;
				}
				else
				{
					Requirement.Edge = ECrowdyApplyEdge::FunctionWritesOwnAttribute;
					Requirement.bScoped = true;
					Requirement.OwningType = Write.Key;
				}
				Requirements.Add(MoveTemp(Requirement));
			}

			for (const FCrowdyGameModelTimer& Timer : Function.Timers)
			{
				if (Timer.FunctionName.IsEmpty())
				{
					continue;
				}
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::FunctionSchedulesFunction;
				Requirement.Kind = ECrowdyApplyKind::Function;
				Requirement.Name = Timer.FunctionName;
				Requirements.Add(MoveTemp(Requirement));
			}

			for (const FString& Callee : CrowdyApplySelection::FunctionCallees(Function))
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::FunctionCallsFunction;
				Requirement.Kind = ECrowdyApplyKind::Function;
				Requirement.Name = Callee;
				Requirements.Add(MoveTemp(Requirement));
			}
			break;
		}

		case ECrowdyApplyKind::Automation:
		{
			const FCrowdySchemaAutomationUpsert* Upsert = CrowdyApplyEntry(Plan.Automations, Unit.PlanIndex);
			if (!Upsert)
			{
				break;
			}
			const FCrowdyGameModelAutomationInput& Automation = Upsert->Automation;

			if (!Automation.FunctionName.IsEmpty())
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::AutomationRunsFunction;
				Requirement.Kind = ECrowdyApplyKind::Function;
				Requirement.Name = Automation.FunctionName;
				Requirements.Add(MoveTemp(Requirement));
			}

			// The target type is a reference only when the automation actually fans out over a type. A container or
			// global automation carries whatever the field last held, and reading that as a reference would pull in
			// a model nothing runs against.
			const FString TargetMode = CrowdyModelVocabulary::NormalizedToken(Automation.TargetMode);
			if (CrowdyModelVocabulary::TokenIs(TargetMode, TEXT("type")) && !Automation.TargetTypeName.IsEmpty())
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::AutomationTargetsType;
				Requirement.Kind = ECrowdyApplyKind::Type;
				Requirement.Name = Automation.TargetTypeName;
				Requirements.Add(MoveTemp(Requirement));
			}
			break;
		}

		case ECrowdyApplyKind::Trigger:
		{
			if (!Unit.Name.IsEmpty())
			{
				FCrowdyApplyRequirement Requirement;
				Requirement.Edge = ECrowdyApplyEdge::TriggerFiresAutomation;
				Requirement.Kind = ECrowdyApplyKind::Automation;
				Requirement.Name = Unit.Name;
				Requirements.Add(MoveTemp(Requirement));
			}
			break;
		}

		default:
			break;
		}

		return Requirements;
	}

	// The units that could satisfy one requirement. A scoped requirement matches on (model, name); an unscoped one
	// matches on the name alone, on any model.
	TArray<int32> CrowdyApplyCandidates(
		const TArray<FCrowdyApplyUnit>& AllUnits, const FCrowdyApplyRequirement& Requirement)
	{
		TArray<int32> Candidates;
		if (Requirement.Name.IsEmpty())
		{
			return Candidates;
		}

		for (int32 Index = 0; Index < AllUnits.Num(); ++Index)
		{
			const FCrowdyApplyUnit& Unit = AllUnits[Index];
			if (Unit.Kind != Requirement.Kind || !CrowdyModelSnapshotKeys::KeysMatch(Unit.Name, Requirement.Name))
			{
				continue;
			}
			if (Requirement.bScoped && !CrowdyModelSnapshotKeys::KeysMatch(Unit.OwningType, Requirement.OwningType))
			{
				continue;
			}
			Candidates.Add(Index);
		}
		return Candidates;
	}

	int32 CrowdyApplyIndexOfKey(const TArray<FCrowdyApplyUnit>& Units, const FString& Key)
	{
		for (int32 Index = 0; Index < Units.Num(); ++Index)
		{
			if (Units[Index].IdentityKey().Equals(Key, ESearchCase::CaseSensitive))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

ECrowdyApplyScope FCrowdyApplyUnit::Scope() const
{
	switch (Kind)
	{
	case ECrowdyApplyKind::Attribute:
	case ECrowdyApplyKind::Function:
		// The one kind where this is reachable in practice is a function: an effect whose container class does not
		// resolve records an empty container type, and reading that as "app-wide" is how a wrong upsert gets sent.
		return OwningType.IsEmpty() ? ECrowdyApplyScope::Undetermined : ECrowdyApplyScope::Determined;

	case ECrowdyApplyKind::Trigger:
		// A trigger's container type is a FILTER, not a scope, and an empty one legitimately means "any model". A
		// trigger is therefore never Undetermined on that field. Do not "fix" this to match the two kinds above.
		return ECrowdyApplyScope::NotScoped;

	default:
		// A model's own name is its identity and an automation's name is unique app-wide, so an empty scope on
		// either is a determined answer.
		return ECrowdyApplyScope::NotScoped;
	}
}

bool FCrowdyApplyUnit::HasDeterminedScope() const
{
	return Scope() != ECrowdyApplyScope::Undetermined;
}

FString FCrowdyApplyUnit::OwningModelName() const
{
	return Kind == ECrowdyApplyKind::Type ? Name : OwningType;
}

FString FCrowdyApplyUnit::IdentityKey() const
{
	// The kind leads, because an attribute and a function on one model can share a name and are two entities. The
	// scope is in it because two models carrying a function called regen are two functions. The discriminator is in
	// it because two triggers on one automation differing only in a filter are two triggers. The separator is a
	// pipe, which no server key contains, and the discriminator is last so its own pipes cannot make two different
	// keys collide.
	return FString::Printf(TEXT("%s|%s|%s|%s"),
		CrowdyApplyKindToken(Kind), *OwningType, *Name, *Discriminator);
}

bool FCrowdyApplyUnit::operator==(const FCrowdyApplyUnit& Other) const
{
	// Display is not identity: two entities that happen to render the same label are still two entities.
	return Kind == Other.Kind
		&& CrowdyModelSnapshotKeys::KeysMatch(OwningType, Other.OwningType)
		&& CrowdyModelSnapshotKeys::KeysMatch(Name, Other.Name)
		&& CrowdyModelSnapshotKeys::KeysMatch(Discriminator, Other.Discriminator);
}

int32 FCrowdyApplyPlan::CountUnits(ECrowdyApplyKind Kind) const
{
	int32 Count = 0;
	for (const FCrowdyApplyUnit& Unit : ClosedSet)
	{
		if (Unit.Kind == Kind)
		{
			++Count;
		}
	}
	return Count;
}

int32 FCrowdyApplyPlan::CountAdditions() const
{
	return Additions.Num();
}

int32 FCrowdyApplyPlan::CountReserved() const
{
	int32 Count = 0;
	for (const FCrowdyApplyUnit& Unit : ClosedSet)
	{
		if (Unit.bReserved)
		{
			++Count;
		}
	}
	return Count;
}

const TArray<ECrowdyApplyKind>& CrowdyApplySelection::ApplyOrder()
{
	// The existing apply's five loops, as a fixed list. Types first because a new attribute needs its type;
	// attributes next because a function names the keys it writes; functions next because an automation names the
	// function it runs; triggers last because a trigger names its automation. Read off the walk rather than
	// re-derived, so a test can compare against it byte for byte.
	static const TArray<ECrowdyApplyKind> Order = {
		ECrowdyApplyKind::Type,
		ECrowdyApplyKind::Attribute,
		ECrowdyApplyKind::Function,
		ECrowdyApplyKind::Automation,
		ECrowdyApplyKind::Trigger
	};
	return Order;
}

TArray<FCrowdyApplyUnit> CrowdyApplySelection::BuildUnits(const FCrowdyApplyPlanInput& Plan)
{
	TArray<FCrowdyApplyUnit> Units;
	Units.Reserve(
		CrowdyApplyNum(Plan.Types) + CrowdyApplyNum(Plan.Props) + CrowdyApplyNum(Plan.Functions)
		+ CrowdyApplyNum(Plan.Automations) + CrowdyApplyNum(Plan.Triggers));

	for (const ECrowdyApplyKind Kind : ApplyOrder())
	{
		switch (Kind)
		{
		case ECrowdyApplyKind::Type:
			for (int32 Index = 0; Index < CrowdyApplyNum(Plan.Types); ++Index)
			{
				const FCrowdySchemaTypeUpsert& Upsert = (*Plan.Types)[Index];
				FCrowdyApplyUnit Unit;
				Unit.Kind = ECrowdyApplyKind::Type;
				Unit.Name = Upsert.Type.TypeName;
				Unit.Display = Upsert.Type.DisplayName.IsEmpty()
					|| CrowdyModelSnapshotKeys::KeysMatch(Upsert.Type.DisplayName, Upsert.Type.TypeName)
					? Upsert.Type.TypeName
					: FString::Printf(TEXT("%s (%s)"), *Upsert.Type.DisplayName, *Upsert.Type.TypeName);
				Unit.bIsNew = Upsert.bIsNew;
				Unit.PlanIndex = Index;
				Units.Add(MoveTemp(Unit));
			}
			break;

		case ECrowdyApplyKind::Attribute:
			for (int32 Index = 0; Index < CrowdyApplyNum(Plan.Props); ++Index)
			{
				const FCrowdySchemaPropUpsert& Upsert = (*Plan.Props)[Index];
				FCrowdyApplyUnit Unit;
				Unit.Kind = ECrowdyApplyKind::Attribute;
				Unit.OwningType = Upsert.ContainerTypeName;
				Unit.Name = Upsert.Prop.Key;
				// The upsert sends the key; the line about it names the attribute the way it was authored, since a
				// reader recognizes CropStage and has to work out what crop_stage was.
				const FString AttributeName =
					CrowdyModelVocabulary::AttributeDisplayName(Upsert.Prop.Key, Upsert.Prop.AuthoredName());
				Unit.Display = Upsert.ContainerTypeName.IsEmpty()
					? AttributeName
					: FString::Printf(TEXT("%s on %s"), *AttributeName, *Upsert.ContainerTypeName);
				Unit.bIsNew = Upsert.bIsNew;
				Unit.bReserved = CrowdyModelLedger::IsReservedAttribute(Upsert.Prop.Key);
				Unit.PlanIndex = Index;
				Units.Add(MoveTemp(Unit));
			}
			break;

		case ECrowdyApplyKind::Function:
			for (int32 Index = 0; Index < CrowdyApplyNum(Plan.Functions); ++Index)
			{
				const FCrowdySchemaFunctionUpsert& Upsert = (*Plan.Functions)[Index];
				FCrowdyApplyUnit Unit;
				Unit.Kind = ECrowdyApplyKind::Function;
				Unit.OwningType = Upsert.Function.ContainerTypeName;
				Unit.Name = Upsert.Function.Name;
				Unit.Display = Upsert.Function.ContainerTypeName.IsEmpty()
					? Upsert.Function.Name
					: FString::Printf(TEXT("%s on %s"), *Upsert.Function.Name, *Upsert.Function.ContainerTypeName);
				Unit.bIsNew = Upsert.bIsNew;
				Unit.bReserved = CrowdyModelLedger::IsReservedFunction(Upsert.Function.Name);
				Unit.PlanIndex = Index;
				Units.Add(MoveTemp(Unit));
			}
			break;

		case ECrowdyApplyKind::Automation:
			for (int32 Index = 0; Index < CrowdyApplyNum(Plan.Automations); ++Index)
			{
				const FCrowdySchemaAutomationUpsert& Upsert = (*Plan.Automations)[Index];
				FCrowdyApplyUnit Unit;
				Unit.Kind = ECrowdyApplyKind::Automation;
				Unit.Name = Upsert.Automation.Name;
				Unit.Display = Upsert.Automation.Name;
				Unit.bIsNew = Upsert.bIsNew;
				Unit.PlanIndex = Index;
				Units.Add(MoveTemp(Unit));
			}
			break;

		case ECrowdyApplyKind::Trigger:
			for (int32 Index = 0; Index < CrowdyApplyNum(Plan.Triggers); ++Index)
			{
				const FCrowdySchemaTriggerUpsert& Upsert = (*Plan.Triggers)[Index];

				// The event token is jargon on the wire, so it becomes a phrase through the one place this page
				// turns a server value into text.
				FStudioAutomationTrigger ForLabel;
				ForLabel.AutomationName = Upsert.Trigger.AutomationName;
				ForLabel.OnEvent = Upsert.Trigger.OnEvent;
				ForLabel.FunctionName = Upsert.Trigger.FunctionName;
				ForLabel.ContainerTypeName = Upsert.Trigger.ContainerTypeName;
				ForLabel.PropertyKey = Upsert.Trigger.PropertyKey;
				const FString EventPhrase = CrowdyModelVocabulary::OnEventLabel(ForLabel);

				FCrowdyApplyUnit Unit;
				Unit.Kind = ECrowdyApplyKind::Trigger;
				Unit.OwningType = Upsert.Trigger.ContainerTypeName;
				Unit.Name = Upsert.Trigger.AutomationName;
				Unit.Discriminator = CrowdyApplyTriggerDiscriminator(Upsert.Trigger);
				Unit.Display = EventPhrase.IsEmpty()
					? Upsert.Trigger.AutomationName
					: FString::Printf(TEXT("%s: %s"), *Upsert.Trigger.AutomationName, *EventPhrase);
				Unit.bIsNew = Upsert.bIsNew;
				Unit.PlanIndex = Index;
				Units.Add(MoveTemp(Unit));
			}
			break;

		default:
			break;
		}
	}

	return Units;
}

TArray<FCrowdyApplyUnit> CrowdyApplySelection::SelectableUnits(const TArray<FCrowdyApplyUnit>& Units)
{
	TArray<FCrowdyApplyUnit> Selectable;
	Selectable.Reserve(Units.Num());
	for (const FCrowdyApplyUnit& Unit : Units)
	{
		if (!Unit.bReserved)
		{
			Selectable.Add(Unit);
		}
	}
	return Selectable;
}

bool CrowdyApplySelection::CanSelect(const FCrowdyApplyUnit& Unit, FString& OutReason)
{
	if (Unit.Scope() == ECrowdyApplyScope::Undetermined)
	{
		OutReason = FString::Printf(
			TEXT("%s belongs to no model this plan could name, so nothing can say what it needs. Fix the effect ")
			TEXT("whose target model does not resolve and press Preview changes."),
			*CrowdyApplyDescribeUnit(Unit));
		return false;
	}

	OutReason.Empty();
	return true;
}

TArray<FString> CrowdyApplySelection::FunctionCallees(const FCrowdyGameModelFunctionInput& Function)
{
	TArray<FString> Callees;

	// Everywhere a function can name something. The same enumeration the delete layer's attribute scan uses, so a
	// call this misses is a call that layer misses too rather than a second list that quietly drifts.
	for (const FCrowdyGameModelMutation& Mutation : Function.Mutations)
	{
		CrowdyApplyCollectCallees(Mutation.Target, Callees);
		CrowdyApplyCollectCallees(Mutation.Expression, Callees);
	}

	CrowdyApplyCollectCallees(Function.ReturnExpression, Callees);
	CrowdyApplyCollectCallees(Function.InvokePolicyJson, Callees);

	for (const FCrowdyGameModelNotification& Notification : Function.Notifications)
	{
		for (const FCrowdyGameModelNotificationArg& Arg : Notification.Args)
		{
			CrowdyApplyCollectCallees(Arg.Expression, Callees);
		}
	}

	for (const FCrowdyGameModelTimer& Timer : Function.Timers)
	{
		CrowdyApplyCollectCallees(Timer.DelayMsExpression, Callees);
		CrowdyApplyCollectCallees(Timer.DedupeKeyExpression, Callees);
		CrowdyApplyCollectCallees(Timer.Target, Callees);
		for (const FCrowdyGameModelTimerParam& Param : Timer.Params)
		{
			CrowdyApplyCollectCallees(Param.Expression, Callees);
		}
	}

	return Callees;
}

TArray<TPair<FString, FString>> CrowdyApplySelection::FunctionWrites(const FCrowdyGameModelFunctionInput& Function)
{
	TArray<TPair<FString, FString>> Writes;
	for (const FCrowdyGameModelMutation& Mutation : Function.Mutations)
	{
		if (Mutation.Property.IsEmpty())
		{
			continue;
		}

		// A ref() target names a live model, and nothing in the compiled function says which model type that ref
		// resolves to, so the owning model comes back empty rather than guessed at.
		const FString OwningType = CrowdyApplyTargetsSelf(Mutation.Target) ? Function.ContainerTypeName : FString();
		Writes.Add(TPair<FString, FString>(OwningType, Mutation.Property));
	}
	return Writes;
}

void CrowdyApplySelection::ClosePrerequisites(
	const TArray<FCrowdyApplyUnit>& AllUnits,
	const FCrowdyApplyPlanInput& Plan,
	const TArray<FString>& SelectedKeys,
	TArray<FCrowdyApplyUnit>& OutClosedSet,
	TArray<FCrowdyApplyAddition>& OutAdditions,
	TArray<FCrowdyApplyUnit>& OutUnresolvable)
{
	OutClosedSet.Reset();
	OutAdditions.Reset();
	OutUnresolvable.Reset();

	TArray<bool> bInClosed;
	bInClosed.Init(false, AllUnits.Num());

	// The SELECTED unit at the root of each chain, not the immediate parent: a reader can act on "you ticked this",
	// and cannot act on "the thing added by the thing you ticked".
	TArray<FString> RootKeys;
	RootKeys.SetNum(AllUnits.Num());

	TArray<int32> Worklist;

	auto MarkUnresolvable = [&OutUnresolvable](const FCrowdyApplyUnit& Unit)
	{
		for (const FCrowdyApplyUnit& Existing : OutUnresolvable)
		{
			if (Existing == Unit)
			{
				return;
			}
		}
		OutUnresolvable.Add(Unit);
	};

	// Add one unit to the closed set. The membership check below is the ONLY place the walk decides a unit is
	// already handled, and it is what makes the walk terminate: two functions can arm or call each other and a
	// function can name itself, so the graph is cyclic. A unit already in the set is a prerequisite already
	// satisfied, never a missing one and never a second entry.
	TFunction<void(int32, const FString&, const FString&)> AddUnit;
	AddUnit = [&](int32 Index, const FString& RootKey, const FString& Because)
	{
		if (!AllUnits.IsValidIndex(Index) || bInClosed[Index])
		{
			return;
		}

		bInClosed[Index] = true;
		RootKeys[Index] = RootKey;
		Worklist.Add(Index);

		const FCrowdyApplyUnit& Unit = AllUnits[Index];

		// A reserved unit is never listed, so a line about one names something the reader was never shown and
		// cannot act on. The sheet folds them into a single line instead.
		if (!Because.IsEmpty() && !Unit.bReserved)
		{
			FCrowdyApplyAddition Addition;
			Addition.Unit = Unit;
			Addition.ForcedByKey = RootKey;
			Addition.Because = Because;
			OutAdditions.Add(MoveTemp(Addition));
		}

		// The SDK's revision attribute and touch function are what make a model-collection change observable, and
		// applying one half of the pair breaks it silently. Whenever anything on a model goes, both go with it.
		const FString ModelName = Unit.OwningModelName();
		if (!ModelName.IsEmpty())
		{
			for (int32 Other = 0; Other < AllUnits.Num(); ++Other)
			{
				if (AllUnits[Other].bReserved
					&& !bInClosed[Other]
					&& CrowdyModelSnapshotKeys::KeysMatch(AllUnits[Other].OwningModelName(), ModelName))
				{
					AddUnit(Other, RootKey, FString());
				}
			}
		}
	};

	for (const FString& Key : SelectedKeys)
	{
		const int32 Index = CrowdyApplyIndexOfKey(AllUnits, Key);
		if (Index == INDEX_NONE)
		{
			continue;
		}

		// A ticked unit whose model nothing determined is the same silence a prerequisite's would be: nothing can
		// say what it needs or whether the server already has it, so it is refused rather than sent.
		if (AllUnits[Index].Scope() == ECrowdyApplyScope::Undetermined)
		{
			MarkUnresolvable(AllUnits[Index]);
			continue;
		}

		AddUnit(Index, Key, FString());
	}

	for (int32 Cursor = 0; Cursor < Worklist.Num(); ++Cursor)
	{
		const int32 Index = Worklist[Cursor];
		const FCrowdyApplyUnit& Unit = AllUnits[Index];
		const FString RootKey = RootKeys[Index];

		for (const FCrowdyApplyRequirement& Requirement : CrowdyApplyRequirementsOf(Unit, Plan))
		{
			for (const int32 Candidate : CrowdyApplyCandidates(AllUnits, Requirement))
			{
				// Case 3. Nothing can tell a create from an update for a unit whose model was never determined, and
				// adding it and omitting it are opposite wrong answers to the same silence.
				if (AllUnits[Candidate].Scope() == ECrowdyApplyScope::Undetermined)
				{
					MarkUnresolvable(AllUnits[Candidate]);
					continue;
				}

				// Case 2. The plan holds it only as an update, so the server already has the entity and it is not a
				// prerequisite. Safe only because these arrays are a diff.
				if (!AllUnits[Candidate].bIsNew)
				{
					continue;
				}

				// Case 1. A candidate already in the set falls out here: AddUnit sees it and does nothing, which is
				// also the answer for a function that arms or calls itself.
				AddUnit(Candidate, RootKey, CrowdyApplyBecause(Unit, Requirement));
			}
		}
	}

	// Emitted by walking the units in apply order rather than in discovery order, so the same selection always
	// produces the same list and the walk runs in the order the server needs.
	for (int32 Index = 0; Index < AllUnits.Num(); ++Index)
	{
		if (bInClosed[Index])
		{
			OutClosedSet.Add(AllUnits[Index]);
		}
	}
}

TArray<FString> CrowdyApplySelection::ReconcileSelection(
	const TArray<FString>& SelectedKeys, const TArray<FCrowdyApplyUnit>& NewUnits, TArray<FString>& OutDropped)
{
	OutDropped.Reset();

	TArray<FString> Kept;
	Kept.Reserve(SelectedKeys.Num());
	for (const FString& Key : SelectedKeys)
	{
		// A key the new plan does not hold is dropped, never mapped to a neighbour: the entity it named is gone and
		// the nearest one is a different entity.
		if (CrowdyApplyIndexOfKey(NewUnits, Key) != INDEX_NONE)
		{
			Kept.Add(Key);
		}
		else
		{
			OutDropped.Add(Key);
		}
	}
	return Kept;
}

FString CrowdyApplySelection::MakeConsentToken(
	int64 AppId, const TArray<FCrowdyApplyUnit>& ClosedSet, ECrowdyApplyRefusal Refusal)
{
	// Every unit's identity and its position in the plan's own arrays, in order, so one entry changing while the
	// count stays the same still changes the token. The refusal is in it because consent given to a sheet that
	// could be sent is not consent to one that has since become unsendable, or the reverse.
	FString Canonical = FString::Printf(
		TEXT("app=%lld\nrefusal=%d\ncount=%d\n"), AppId, static_cast<int32>(Refusal), ClosedSet.Num());

	for (const FCrowdyApplyUnit& Unit : ClosedSet)
	{
		Canonical += FString::Printf(TEXT("%s@%d|%d|%d\n"),
			*Unit.IdentityKey(), Unit.PlanIndex, Unit.bIsNew ? 1 : 0, Unit.bReserved ? 1 : 0);
	}

	return CrowdyApplyDigest(Canonical);
}

FCrowdyApplySheet CrowdyApplySelection::BuildSheet(
	int64 AppId, const TArray<FCrowdyApplyUnit>& ClosedSet, const TArray<FCrowdyApplyAddition>& Additions,
	const TArray<FCrowdyApplyUnit>& Remainder, ECrowdyApplyRefusal Refusal,
	const TArray<FCrowdyApplyUnit>& Unresolvable)
{
	FCrowdyApplySheet Sheet;

	Sheet.AppLine = AppId == 0
		? FString(TEXT("No app is selected."))
		: FString::Printf(TEXT("Writes to app %lld."), AppId);

	// Every count on the sheet is a count of rows the reader was shown. The SDK's own wiring is disclosed on its
	// own line rather than folded into these, so a number here always matches something that was on screen.
	auto CountVisible = [&ClosedSet](ECrowdyApplyKind Kind)
	{
		int32 Count = 0;
		for (const FCrowdyApplyUnit& Unit : ClosedSet)
		{
			if (Unit.Kind == Kind && !Unit.bReserved)
			{
				++Count;
			}
		}
		return Count;
	};

	int32 VisibleTotal = 0;
	int32 ReservedTotal = 0;
	TArray<FString> ReservedModels;
	for (const FCrowdyApplyUnit& Unit : ClosedSet)
	{
		if (Unit.bReserved)
		{
			++ReservedTotal;
			CrowdyApplyAddUniqueName(ReservedModels, Unit.OwningModelName());
		}
		else
		{
			++VisibleTotal;
		}
	}

	TArray<FString> Parts;
	for (const ECrowdyApplyKind Kind : ApplyOrder())
	{
		const int32 Count = CountVisible(Kind);
		if (Count > 0)
		{
			Parts.Add(CrowdyApplyCountPhrase(Count, Kind));
			Sheet.CountLines.Add(CrowdyApplyCountPhrase(Count, Kind));
		}
	}

	if (ClosedSet.Num() == 0)
	{
		Sheet.Headline = TEXT("Nothing to send.");
	}
	else if (Parts.Num() == 0)
	{
		// Everything in the set is wiring the reader was never shown, so the count is all there is to say and
		// saying it is better than a sentence with a hole in it.
		Sheet.Headline = FString::Printf(TEXT("Send %s?"), *CrowdyApplyChangePhrase(ClosedSet.Num()));
	}
	else
	{
		Sheet.Headline = FString::Printf(TEXT("Send %s?"), *CrowdyApplyJoinPhrases(Parts));
	}

	if (Additions.Num() > 0)
	{
		Sheet.AddedLine = FString::Printf(
			TEXT("%d more %s added because something you picked needs them."),
			Additions.Num(), Additions.Num() == 1 ? TEXT("was") : TEXT("were"));
	}

	if (ReservedTotal > 0)
	{
		Sheet.ReservedLine = FString::Printf(
			TEXT("plus the wiring the SDK keeps on %d %s."),
			ReservedModels.Num(),
			*CrowdyApplyKindNoun(ECrowdyApplyKind::Type, ReservedModels.Num() != 1));
	}

	// The remainder line counts the rows the reader picked from, on both sides of the "of", so the two numbers can
	// be read against each other. The reserved units a partial send strands are counted on the line above.
	int32 VisibleRemainder = 0;
	for (const FCrowdyApplyUnit& Unit : Remainder)
	{
		if (!Unit.bReserved)
		{
			++VisibleRemainder;
		}
	}
	if (VisibleRemainder > 0)
	{
		Sheet.RemainderLine = FString::Printf(TEXT("%d of %s stay unsent."),
			VisibleRemainder, *CrowdyApplyChangePhrase(VisibleRemainder + VisibleTotal));
	}

	// The button always names its count, so the action and the number it acts on cannot be read apart.
	Sheet.ActionLabel = VisibleTotal > 0
		? FString::Printf(TEXT("Send %s"), *CrowdyApplyChangePhrase(VisibleTotal))
		: (ClosedSet.Num() > 0
			? FString::Printf(TEXT("Send %s"), *CrowdyApplyChangePhrase(ClosedSet.Num()))
			: FString(TEXT("Send")));

	switch (Refusal)
	{
	case ECrowdyApplyRefusal::NothingSelected:
		Sheet.BlockedReason = TEXT("Nothing is selected. Tick at least one change to send.");
		break;

	case ECrowdyApplyRefusal::UnresolvablePrerequisite:
		// Only a function can reach this: an attribute's model comes from the desired schema and is always there.
		Sheet.BlockedReason = FString::Printf(
			TEXT("One thing this needs could not be identified: %d function(s) in this plan have no model. Fix the ")
			TEXT("effect whose target model does not resolve, press Preview changes, and try again."),
			Unresolvable.Num());
		break;

	case ECrowdyApplyRefusal::SelectionIsStale:
		Sheet.BlockedReason = TEXT("This plan was recomputed and none of what you picked is in it. Pick again.");
		break;

	case ECrowdyApplyRefusal::NoPlan:
		Sheet.BlockedReason = TEXT("Nothing to send. Press Preview changes first.");
		break;

	default:
		break;
	}

	return Sheet;
}

FCrowdyApplyPlan CrowdyApplySelection::BuildPlan(
	const FCrowdyApplyPlanInput& Plan, const TArray<FString>& SelectedKeys)
{
	FCrowdyApplyPlan Result;
	Result.AppId = Plan.AppId;
	Result.AllUnits = BuildUnits(Plan);

	if (Result.AllUnits.Num() == 0)
	{
		Result.Refusal = ECrowdyApplyRefusal::NoPlan;
		Result.Sheet = BuildSheet(
			Result.AppId, Result.ClosedSet, Result.Additions, Result.Remainder, Result.Refusal, Result.Unresolvable);
		Result.ConsentToken = MakeConsentToken(Result.AppId, Result.ClosedSet, Result.Refusal);
		return Result;
	}

	Result.SelectedKeys = ReconcileSelection(SelectedKeys, Result.AllUnits, Result.DroppedKeys);

	ClosePrerequisites(
		Result.AllUnits, Plan, Result.SelectedKeys, Result.ClosedSet, Result.Additions, Result.Unresolvable);

	// The closed set is a subsequence of the units in the same order, so one walk with a cursor separates it from
	// what is left. Membership is by kind and plan index, which addresses exactly one pending upsert; comparing the
	// units themselves would fold two entries the diff happens to have emitted twice into one.
	int32 ClosedCursor = 0;
	for (const FCrowdyApplyUnit& Unit : Result.AllUnits)
	{
		if (Result.ClosedSet.IsValidIndex(ClosedCursor)
			&& Result.ClosedSet[ClosedCursor].Kind == Unit.Kind
			&& Result.ClosedSet[ClosedCursor].PlanIndex == Unit.PlanIndex)
		{
			++ClosedCursor;
		}
		else
		{
			Result.Remainder.Add(Unit);
		}
	}

	if (SelectedKeys.Num() > 0 && Result.SelectedKeys.Num() == 0)
	{
		Result.Refusal = ECrowdyApplyRefusal::SelectionIsStale;
	}
	else if (Result.Unresolvable.Num() > 0)
	{
		Result.Refusal = ECrowdyApplyRefusal::UnresolvablePrerequisite;
	}
	else if (Result.ClosedSet.Num() == 0)
	{
		Result.Refusal = ECrowdyApplyRefusal::NothingSelected;
	}

	Result.bSendable = Result.Refusal == ECrowdyApplyRefusal::None && Result.ClosedSet.Num() > 0;
	Result.Sheet = BuildSheet(
		Result.AppId, Result.ClosedSet, Result.Additions, Result.Remainder, Result.Refusal, Result.Unresolvable);
	Result.ConsentToken = MakeConsentToken(Result.AppId, Result.ClosedSet, Result.Refusal);
	return Result;
}
