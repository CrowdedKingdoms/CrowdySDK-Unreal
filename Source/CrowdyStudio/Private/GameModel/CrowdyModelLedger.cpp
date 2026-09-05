// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyModelLedger.h"

#include "Dom/JsonObject.h"
#include "GameModel/CrowdyModelSnapshot.h"
#include "GameModel/CrowdyModelVocabulary.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h" // IsReservedCollectionKey / IsReservedCollectionFunctionName
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// A model owns a function, an automation or an attribute by an exact type-name match. The names are server
	// keys, and FString::operator== is case-INSENSITIVE, so every ownership test here is explicitly
	// case-sensitive. An empty TypeName matches only the entries that name no type, which is how the app-wide
	// automations are addressed.
	bool ModelLedgerOwnedBy(const FString& EntryTypeName, const FString& TypeName)
	{
		return EntryTypeName.Equals(TypeName, ESearchCase::CaseSensitive);
	}

	template <typename ItemType>
	TArray<ItemType> ModelLedgerFilterBySearchKey(
		const TArray<ItemType>& Source, const FString& Query, ECrowdyModelSourceFilter Filter)
	{
		const FString Needle = Query.TrimStartAndEnd().ToLower();

		TArray<ItemType> Matches;
		Matches.Reserve(Source.Num());
		for (const ItemType& Item : Source)
		{
			if (!CrowdyModelLedger::MatchesSourceFilter(Item.Provenance, Filter))
			{
				continue;
			}
			// Both sides are already lowercase, so a case-sensitive Contains does the same work as a
			// case-insensitive one without folding every character of every key on every keystroke. An empty query
			// matches everything, which leaves the source setting as the only narrowing in play.
			if (Needle.IsEmpty() || Item.SearchKey.Contains(Needle, ESearchCase::CaseSensitive))
			{
				Matches.Add(Item);
			}
		}
		return Matches;
	}

	// The spelling an attribute was authored under, or empty when nothing in the project declares it. Deriving the
	// server key lowercases the property name, so the declaring class captured in the plan is the only place the
	// authored capitalization still exists; a server-only attribute has none anywhere on this side.
	FString ModelLedgerAuthoredAttributeName(
		const FCrowdyModelSnapshot* Snapshot, const FString& TypeName, const FString& Key)
	{
		const FCrowdyModelSnapshotType* Declared = Snapshot ? Snapshot->FindType(TypeName) : nullptr;
		const FCrowdyModelSnapshotAttribute* Attribute = Declared ? Declared->FindAttribute(Key) : nullptr;
		return Attribute ? Attribute->AuthoredName : FString();
	}

	// Stamp one entity's verdict onto the row or the summary that shows it, columns included. A row and a summary
	// carry the same five fields for the same reason, so one template fills both and they cannot drift apart.
	template <typename ItemType>
	void ModelLedgerApplyVerdict(ItemType& Item, const FCrowdyModelClassification& Verdict)
	{
		Item.Provenance = Verdict.Provenance;
		Item.Drift = Verdict.Drift;
		Item.ProvenanceText = CrowdyModelVocabulary::ProvenanceLabel(Verdict.Provenance);
		Item.DriftText = CrowdyModelVocabulary::DriftLabel(Verdict.Drift);
		Item.CodePath = Verdict.CodePath;
	}

	// Whether the server already answered with this entity. A synthesized row exists to stand in for something the
	// server does not have, so anything the server does have must never get a second, invented row: a plan snapshot
	// is only as fresh as the last plan, and an apply since then would otherwise double every row it created.
	bool ModelLedgerServerHasFunction(
		const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& TypeName, const FString& Name)
	{
		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (Function.IsValid()
				&& ModelLedgerOwnedBy(Function->ContainerTypeName, TypeName)
				&& Function->Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	bool ModelLedgerServerHasAutomation(const TArray<TSharedPtr<FStudioAutomation>>& Automations, const FString& Name)
	{
		for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
		{
			if (Automation.IsValid() && Automation->Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	bool ModelLedgerServerHasType(const TArray<TSharedPtr<FStudioContainerType>>& Types, const FString& TypeName)
	{
		for (const TSharedPtr<FStudioContainerType>& Type : Types)
		{
			if (Type.IsValid() && Type->TypeName.Equals(TypeName, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	// A function the project declares that the last plan would have to create. The SDK's own collection plumbing is
	// in the plan's desired schema too (a reserved revision attribute and a per-type touch function on every model),
	// so without this exclusion synthesizing the code side would put back exactly the two rows per model that the
	// browser deliberately never shows: eighty models is a hundred and sixty rows of wiring nobody authored.
	bool ModelLedgerIsPendingFunction(const FCrowdyModelSnapshot& Snapshot, const FCrowdyModelSnapshotFunction& Function)
	{
		return !CrowdyModelLedger::IsReservedFunction(Function.Name)
			&& CrowdyModelSnapshotKeys::Contains(
				Snapshot.FunctionCreates, FCrowdySchemaSync::ScopedNameKey(Function.TypeName, Function.Name));
	}

	bool ModelLedgerIsPendingAttribute(
		const FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const FCrowdyModelSnapshotAttribute& Attribute)
	{
		return !CrowdyModelLedger::IsReservedAttribute(Attribute.Key)
			&& CrowdyModelSnapshotKeys::Contains(
				Snapshot.AttributeCreates, FCrowdySchemaSync::ScopedNameKey(TypeName, Attribute.Key));
	}

	bool ModelLedgerIsPendingAutomation(
		const FCrowdyModelSnapshot& Snapshot, const FCrowdyModelSnapshotAutomation& Automation)
	{
		return CrowdyModelSnapshotKeys::Contains(
			Snapshot.AutomationCreates, FCrowdySchemaSync::ScopedNameKey(FString(), Automation.Name));
	}

	// When a not-yet-synced automation runs, phrased from the project exactly as it will be phrased from the server
	// once it is there, so the row does not change its wording the moment it syncs.
	FString ModelLedgerPendingAutomationPhrase(const FCrowdyModelSnapshotAutomation& Automation)
	{
		FStudioAutomation AsRead;
		AsRead.Name = Automation.Name;
		AsRead.TargetTypeName = Automation.TargetTypeName;
		AsRead.TriggerType = Automation.TriggerType;
		AsRead.ScheduleKind = Automation.ScheduleKind;
		AsRead.IntervalMs = Automation.IntervalMs;
		AsRead.CronExpr = Automation.CronExpr;

		FString Phrase = CrowdyModelVocabulary::AutomationTriggerLabel(AsRead);

		FStudioAutomationTrigger AsTrigger;
		AsTrigger.AutomationName = Automation.Name;
		AsTrigger.OnEvent = Automation.OnEvent;
		AsTrigger.PropertyKey = Automation.OnEventPropertyKey;
		AsTrigger.FunctionName = Automation.OnEventFunctionName;
		const FString EventPhrase = CrowdyModelVocabulary::OnEventLabel(AsTrigger);
		if (!EventPhrase.IsEmpty())
		{
			Phrase = EventPhrase;
		}
		return Phrase;
	}

	// Every event this automation reacts to, as one phrase. An automation may legitimately have no trigger at all
	// (interval and cron automations carry their schedule on the automation itself) and it may equally have several,
	// since a trigger is identified by its event and its filters rather than by its automation alone. Showing only
	// the first would understate what the automation responds to, and the server does not promise a stable order, so
	// the phrases are sorted here: two identical reads must produce the same row.
	FString ModelLedgerTriggerPhrase(
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers, const FString& AutomationName)
	{
		TArray<FString> Phrases;
		for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Triggers)
		{
			if (!Trigger.IsValid() || !Trigger->AutomationName.Equals(AutomationName, ESearchCase::CaseSensitive))
			{
				continue;
			}

			const FString Phrase = CrowdyModelVocabulary::OnEventLabel(*Trigger);
			if (!Phrase.IsEmpty())
			{
				Phrases.AddUnique(Phrase);
			}
		}

		Phrases.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
		return FString::Join(Phrases, TEXT("; "));
	}

	void ModelLedgerSortRows(TArray<FCrowdyModelRow>& Rows)
	{
		Rows.Sort([](const FCrowdyModelRow& A, const FCrowdyModelRow& B)
		{
			const int32 ByPrimary = A.Primary.Compare(B.Primary, ESearchCase::IgnoreCase);
			return ByPrimary != 0 ? ByPrimary < 0 : A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
		});
	}

	// The longest one value may run on the summary line. A designer attribute is free to hold a paragraph, and the
	// line it lands on is one row tall and does not scroll, so an unbounded value would push every property after
	// it past the edge with nothing on screen to say any were left.
	constexpr int32 ModelLedgerMaxValueChars = 48;

	FString ModelLedgerShorten(const FString& Value)
	{
		return Value.Len() <= ModelLedgerMaxValueChars
			? Value
			: Value.Left(ModelLedgerMaxValueChars) + TEXT("...");
	}

	// One property value as the short phrase the summary line shows. A number keeps only the digits it needs: the
	// wire carries every number as a double, so a whole one would otherwise read as 84.000000 and look like a
	// precision problem rather than a count. An object or an array is named by what it is instead of being
	// expanded, because a nested blob flattened onto this line is both unreadable and unbounded.
	FString ModelLedgerValuePhrase(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("nothing");
		}

		switch (Value->Type)
		{
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("yes") : TEXT("no");

		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber(), 0);

		case EJson::String:
			return ModelLedgerShorten(Value->AsString());

		case EJson::Object:
			return CrowdyModelVocabulary::ValueTypeLabel(TEXT("object")).ToLower();

		case EJson::Array:
			return CrowdyModelVocabulary::ValueTypeLabel(TEXT("array")).ToLower();

		default:
			return TEXT("nothing");
		}
	}
}

bool CrowdyModelLedger::IsReservedAttribute(const FString& Key)
{
	return CrowdyGameModelMetaKeys::IsReservedCollectionKey(Key);
}

bool CrowdyModelLedger::IsReservedFunction(const FString& Name)
{
	return CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(Name);
}

FCrowdyModelClassification CrowdyModelLedger::ClassifyModel(
	const FCrowdyModelSnapshot* Snapshot, const FString& TypeName)
{
	FCrowdyModelClassification Verdict;
	if (!Snapshot || TypeName.IsEmpty())
	{
		return Verdict;
	}

	if (const FCrowdyModelSnapshotType* Declared = Snapshot->FindType(TypeName))
	{
		Verdict.CodePath = Declared->OwningClassPath;
		if (CrowdyModelSnapshotKeys::Contains(Snapshot->TypeCreates, TypeName))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeNotPushed;
			Verdict.Drift = ECrowdyModelDrift::NotOnServerYet;
		}
		else if (CrowdyModelSnapshotKeys::Contains(Snapshot->TypeUpdates, TypeName))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
			Verdict.Drift = ECrowdyModelDrift::ChangedInCode;
		}
		else
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeSynced;
		}
		return Verdict;
	}

	if (Snapshot->IsKitOwnedType(TypeName))
	{
		// A kit owns its whole namespace, so this is neither drift nor an orphan and the prune declines to offer it.
		Verdict.Provenance = ECrowdyModelProvenance::KitOwned;
		return Verdict;
	}

	Verdict.Provenance = ECrowdyModelProvenance::ServerOnly;
	Verdict.Drift = ECrowdyModelDrift::OnlyOnServer;
	return Verdict;
}

FCrowdyModelClassification CrowdyModelLedger::ClassifyAttribute(
	const FCrowdyModelSnapshot* Snapshot, const FString& TypeName, const FString& Key)
{
	FCrowdyModelClassification Verdict;
	if (!Snapshot || TypeName.IsEmpty() || Key.IsEmpty())
	{
		return Verdict;
	}

	const FCrowdyModelSnapshotType* Declared = Snapshot->FindType(TypeName);
	if (Declared && Declared->FindAttribute(Key))
	{
		// An attribute is declared by the class that declares its model, so that class is what a reader opens.
		Verdict.CodePath = Declared->OwningClassPath;
		const FString ScopedKey = FCrowdySchemaSync::ScopedNameKey(TypeName, Key);
		if (CrowdyModelSnapshotKeys::Contains(Snapshot->AttributeCreates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeNotPushed;
			Verdict.Drift = ECrowdyModelDrift::NotOnServerYet;
		}
		else if (CrowdyModelSnapshotKeys::Contains(Snapshot->AttributeUpdates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
			Verdict.Drift = ECrowdyModelDrift::ChangedInCode;
		}
		else
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeSynced;
		}
		return Verdict;
	}

	if (Snapshot->IsKitOwnedType(TypeName))
	{
		Verdict.Provenance = ECrowdyModelProvenance::KitOwned;
		return Verdict;
	}

	Verdict.Provenance = ECrowdyModelProvenance::ServerOnly;
	Verdict.Drift = ECrowdyModelDrift::OnlyOnServer;
	return Verdict;
}

FCrowdyModelClassification CrowdyModelLedger::ClassifyFunction(
	const FCrowdyModelSnapshot* Snapshot, const FString& TypeName, const FString& Name)
{
	FCrowdyModelClassification Verdict;
	if (!Snapshot || Name.IsEmpty())
	{
		return Verdict;
	}

	const FString ScopedKey = FCrowdySchemaSync::ScopedNameKey(TypeName, Name);

	// The desired schema is asked first. An entity the plan actually produced is one the plan could check, even if
	// some other, broken effect happens to claim the same name.
	if (const FCrowdyModelSnapshotFunction* Declared = Snapshot->FindFunction(TypeName, Name))
	{
		Verdict.CodePath = Declared->AssetPath;
		if (CrowdyModelSnapshotKeys::Contains(Snapshot->FunctionCreates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeNotPushed;
			Verdict.Drift = ECrowdyModelDrift::NotOnServerYet;
		}
		else if (CrowdyModelSnapshotKeys::Contains(Snapshot->FunctionUpdates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
			Verdict.Drift = ECrowdyModelDrift::ChangedInCode;
		}
		else
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeSynced;
		}
		return Verdict;
	}

	// Two assets claim this name and the plan synced neither, so what the server holds is not what either of them
	// says. It came from code all the same, which is the part a reader needs to act on.
	if (CrowdyModelSnapshotKeys::Contains(Snapshot->NeedsAFixFunctionKeys, ScopedKey))
	{
		Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
		Verdict.Drift = ECrowdyModelDrift::NeedsAFix;
		Verdict.CodePath = FCrowdyModelSnapshot::FindAuthorPath(Snapshot->FunctionAuthors, ScopedKey);
		return Verdict;
	}

	if (CrowdyModelSnapshotKeys::Contains(Snapshot->UncheckableFunctionKeys, ScopedKey))
	{
		Verdict.Drift = ECrowdyModelDrift::CannotBeChecked;
		Verdict.CodePath = FCrowdyModelSnapshot::FindAuthorPath(Snapshot->FunctionAuthors, ScopedKey);
		return Verdict;
	}

	// The same answer for an author that could not name a model at all. An effect is scoped by the container class it
	// targets, and an effect whose target class was deleted, untagged or left unloaded resolves no type, so its name is
	// filed with an empty scope that no model's key can ever match. The name is still claimed by an asset in the
	// project, and the plan keeps the server's copy off the prune list on exactly that basis, so calling it an orphan
	// here would have the page contradict the plan about the same entity.
	const FString UnscopedKey = FCrowdySchemaSync::ScopedNameKey(FString(), Name);
	if (CrowdyModelSnapshotKeys::Contains(Snapshot->UncheckableFunctionKeys, UnscopedKey))
	{
		Verdict.Drift = ECrowdyModelDrift::CannotBeChecked;
		Verdict.CodePath = FCrowdyModelSnapshot::FindAuthorPath(Snapshot->FunctionAuthors, UnscopedKey);
		return Verdict;
	}

	// A function whose effect was repointed at another model. With no server function on the new model, the plan
	// matches the one that still bears the name and plans an ordinary update that rebinds it in place, so this row is
	// what the code declares and is drifting from, not something sitting on the server unclaimed. Two conditions, both
	// needed: the name has to lead to exactly one declaration (with several, nothing here can say which of them this
	// row becomes, which is why the diff's own fallback requires the same), and that declaration has to be one the plan
	// is actually updating. Without the second, a stale server copy left beside a function that merely changed would be
	// told a rebind is coming for it when nothing is planned for it at all.
	if (const FCrowdyModelSnapshotFunction* Rebound = Snapshot->FindOnlyFunctionNamed(Name))
	{
		const FString ReboundKey = FCrowdySchemaSync::ScopedNameKey(Rebound->TypeName, Rebound->Name);
		if (CrowdyModelSnapshotKeys::Contains(Snapshot->FunctionUpdates, ReboundKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
			Verdict.Drift = ECrowdyModelDrift::ChangedInCode;
			Verdict.CodePath = Rebound->AssetPath;
			return Verdict;
		}
	}

	if (Snapshot->IsKitOwnedFunction(TypeName, Name))
	{
		Verdict.Provenance = ECrowdyModelProvenance::KitOwned;
		return Verdict;
	}

	Verdict.Provenance = ECrowdyModelProvenance::ServerOnly;
	Verdict.Drift = ECrowdyModelDrift::OnlyOnServer;
	return Verdict;
}

FCrowdyModelClassification CrowdyModelLedger::ClassifyAutomation(
	const FCrowdyModelSnapshot* Snapshot, const FString& Name)
{
	FCrowdyModelClassification Verdict;
	if (!Snapshot || Name.IsEmpty())
	{
		return Verdict;
	}

	// An automation name is unique app-wide on the server, so it is scoped by nothing, unlike a function name.
	const FString ScopedKey = FCrowdySchemaSync::ScopedNameKey(FString(), Name);

	if (const FCrowdyModelSnapshotAutomation* Declared = Snapshot->FindAutomation(Name))
	{
		Verdict.CodePath = Declared->AssetPath;
		if (CrowdyModelSnapshotKeys::Contains(Snapshot->AutomationCreates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeNotPushed;
			Verdict.Drift = ECrowdyModelDrift::NotOnServerYet;
		}
		else if (CrowdyModelSnapshotKeys::Contains(Snapshot->AutomationUpdates, ScopedKey))
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
			Verdict.Drift = ECrowdyModelDrift::ChangedInCode;
		}
		else
		{
			Verdict.Provenance = ECrowdyModelProvenance::CodeSynced;
		}
		return Verdict;
	}

	if (CrowdyModelSnapshotKeys::Contains(Snapshot->NeedsAFixAutomationKeys, ScopedKey))
	{
		Verdict.Provenance = ECrowdyModelProvenance::CodeDrifted;
		Verdict.Drift = ECrowdyModelDrift::NeedsAFix;
		Verdict.CodePath = FCrowdyModelSnapshot::FindAuthorPath(Snapshot->AutomationAuthors, ScopedKey);
		return Verdict;
	}

	if (CrowdyModelSnapshotKeys::Contains(Snapshot->UncheckableAutomationKeys, ScopedKey))
	{
		Verdict.Drift = ECrowdyModelDrift::CannotBeChecked;
		Verdict.CodePath = FCrowdyModelSnapshot::FindAuthorPath(Snapshot->AutomationAuthors, ScopedKey);
		return Verdict;
	}

	if (Snapshot->IsKitOwnedAutomation(Name))
	{
		Verdict.Provenance = ECrowdyModelProvenance::KitOwned;
		return Verdict;
	}

	Verdict.Provenance = ECrowdyModelProvenance::ServerOnly;
	Verdict.Drift = ECrowdyModelDrift::OnlyOnServer;
	return Verdict;
}

bool CrowdyModelLedger::MatchesSourceFilter(ECrowdyModelProvenance Provenance, ECrowdyModelSourceFilter Filter)
{
	if (Filter == ECrowdyModelSourceFilter::All || Provenance == ECrowdyModelProvenance::Unknown)
	{
		return true;
	}

	const bool bInCode = Provenance == ECrowdyModelProvenance::CodeSynced
		|| Provenance == ECrowdyModelProvenance::CodeNotPushed
		|| Provenance == ECrowdyModelProvenance::CodeDrifted;

	return Filter == ECrowdyModelSourceFilter::InCode ? bInCode : !bInCode;
}

FString CrowdyModelLedger::MakeSearchKey(const TArray<FString>& Parts)
{
	FString Key;
	for (const FString& Part : Parts)
	{
		const FString Trimmed = Part.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}
		if (!Key.IsEmpty())
		{
			Key.AppendChar(TEXT(' '));
		}
		Key += Trimmed;
	}
	return Key.ToLower();
}

TArray<FCrowdyModelSummary> CrowdyModelLedger::BuildModelList(
	const TArray<TSharedPtr<FStudioContainerType>>& Types,
	const TArray<TSharedPtr<FStudioFunction>>& Functions,
	const TArray<TSharedPtr<FStudioAutomation>>& Automations,
	const FCrowdyModelSnapshot* Snapshot)
{
	TArray<FCrowdyModelSummary> Models;
	Models.Reserve(Types.Num());

	// The functions and automations the project declares for a model but the server does not have yet. They are rows
	// in the model's tables, so they are counted here too: a count that disagrees with the table it opens onto reads
	// as one of the two being broken.
	const auto CountPending = [Snapshot, &Functions, &Automations](FCrowdyModelSummary& Summary)
	{
		if (!Snapshot)
		{
			return;
		}
		for (const FCrowdyModelSnapshotFunction& Function : Snapshot->Functions)
		{
			if (ModelLedgerOwnedBy(Function.TypeName, Summary.TypeName)
				&& ModelLedgerIsPendingFunction(*Snapshot, Function)
				&& !ModelLedgerServerHasFunction(Functions, Summary.TypeName, Function.Name))
			{
				++Summary.FunctionCount;
			}
		}
		for (const FCrowdyModelSnapshotAutomation& Automation : Snapshot->Automations)
		{
			if (ModelLedgerOwnedBy(Automation.TargetTypeName, Summary.TypeName)
				&& ModelLedgerIsPendingAutomation(*Snapshot, Automation)
				&& !ModelLedgerServerHasAutomation(Automations, Automation.Name))
			{
				++Summary.AutomationCount;
			}
		}
	};

	for (const TSharedPtr<FStudioContainerType>& Type : Types)
	{
		if (!Type.IsValid())
		{
			continue;
		}

		FCrowdyModelSummary Summary;
		Summary.TypeName = Type->TypeName;
		const FString DisplayName = Type->DisplayName.TrimStartAndEnd();
		Summary.Display = DisplayName.IsEmpty() ? Type->TypeName : DisplayName;
		Summary.Description = Type->Description;

		for (const TSharedPtr<FStudioFunction>& Function : Functions)
		{
			if (Function.IsValid()
				&& ModelLedgerOwnedBy(Function->ContainerTypeName, Summary.TypeName)
				&& !IsReservedFunction(Function->Name))
			{
				++Summary.FunctionCount;
			}
		}

		for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
		{
			if (Automation.IsValid() && ModelLedgerOwnedBy(Automation->TargetTypeName, Summary.TypeName))
			{
				++Summary.AutomationCount;
			}
		}

		CountPending(Summary);
		ModelLedgerApplyVerdict(Summary, ClassifyModel(Snapshot, Summary.TypeName));

		// The type name is in the haystack alongside the display name because a developer reading a class types
		// the class name, not whatever display name the schema happens to carry.
		Summary.SearchKey = MakeSearchKey({ Summary.TypeName, Summary.Display, Summary.Description });
		Models.Add(MoveTemp(Summary));
	}

	// A model the project declares that the server has never seen has no server read to come from, so without this
	// it would be absent from the browser entirely and "not on server yet" could never be said about anything.
	if (Snapshot)
	{
		for (const FCrowdyModelSnapshotType& Declared : Snapshot->Types)
		{
			if (!CrowdyModelSnapshotKeys::Contains(Snapshot->TypeCreates, Declared.TypeName)
				|| ModelLedgerServerHasType(Types, Declared.TypeName))
			{
				continue;
			}

			FCrowdyModelSummary Summary;
			Summary.TypeName = Declared.TypeName;
			const FString DisplayName = Declared.DisplayName.TrimStartAndEnd();
			Summary.Display = DisplayName.IsEmpty() ? Declared.TypeName : DisplayName;
			Summary.bCodeOnly = true;

			// The server holds nothing for this model, so its attributes are entirely what the class declares and
			// this count is final. Leaving it not-loaded-yet would leave the browser waiting on a read that has
			// nothing to return.
			Summary.AttributeCount = 0;
			for (const FCrowdyModelSnapshotAttribute& Attribute : Declared.Attributes)
			{
				if (!IsReservedAttribute(Attribute.Key))
				{
					++Summary.AttributeCount;
				}
			}

			CountPending(Summary);
			ModelLedgerApplyVerdict(Summary, ClassifyModel(Snapshot, Summary.TypeName));
			Summary.SearchKey = MakeSearchKey({ Summary.TypeName, Summary.Display, Summary.Description });
			Models.Add(MoveTemp(Summary));
		}
	}

	Models.Sort([](const FCrowdyModelSummary& A, const FCrowdyModelSummary& B)
	{
		const int32 ByDisplay = A.Display.Compare(B.Display, ESearchCase::IgnoreCase);
		return ByDisplay != 0 ? ByDisplay < 0 : A.TypeName.Compare(B.TypeName, ESearchCase::IgnoreCase) < 0;
	});
	return Models;
}

TArray<FCrowdyModelRow> CrowdyModelLedger::BuildAttributeRows(
	const TArray<TSharedPtr<FStudioPropertyDef>>& Defs,
	const FCrowdyModelSnapshot* Snapshot,
	const FString& OwningTypeName)
{
	TArray<FCrowdyModelRow> Rows;
	Rows.Reserve(Defs.Num());

	for (const TSharedPtr<FStudioPropertyDef>& Def : Defs)
	{
		if (!Def.IsValid() || IsReservedAttribute(Def->Key))
		{
			continue;
		}

		FCrowdyModelRow Row;
		Row.Kind = ECrowdyModelRowKind::Attribute;
		Row.Name = Def->Key;
		Row.OwningType = Def->ContainerTypeName;
		Row.Primary = Def->Key;
		Row.Display = CrowdyModelVocabulary::AttributeDisplayName(
			Def->Key, ModelLedgerAuthoredAttributeName(Snapshot, Def->ContainerTypeName, Def->Key));
		Row.Secondary = CrowdyModelVocabulary::ValueTypeLabel(Def->ValueType);
		Row.Detail = Def->Description;
		// Each def carries its own model, which is what the verdict is scoped by: the same key on two models is two
		// server property definitions with two independent answers.
		ModelLedgerApplyVerdict(Row, ClassifyAttribute(Snapshot, Def->ContainerTypeName, Def->Key));
		// The key is in the haystack alongside the shown name: a reader who knows an attribute as hp_max types that,
		// and a reader who only ever sees HpMax types that, and both have to find the row.
		Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Display, Row.Detail });
		Rows.Add(MoveTemp(Row));
	}

	if (Snapshot)
	{
		// A model with no server read at all has no def to take its name from, so the caller names it. For a model
		// the server does have, the defs answer, which keeps every existing caller working unchanged.
		FString OwningType = OwningTypeName;
		if (OwningType.IsEmpty())
		{
			for (const TSharedPtr<FStudioPropertyDef>& Def : Defs)
			{
				if (Def.IsValid())
				{
					OwningType = Def->ContainerTypeName;
					break;
				}
			}
		}

		if (const FCrowdyModelSnapshotType* Declared = Snapshot->FindType(OwningType))
		{
			for (const FCrowdyModelSnapshotAttribute& Attribute : Declared->Attributes)
			{
				const bool bAlreadyRead = Rows.ContainsByPredicate([&Attribute, &OwningType](const FCrowdyModelRow& Existing)
				{
					return Existing.Name.Equals(Attribute.Key, ESearchCase::CaseSensitive)
						&& ModelLedgerOwnedBy(Existing.OwningType, OwningType);
				});
				if (bAlreadyRead || !ModelLedgerIsPendingAttribute(*Snapshot, OwningType, Attribute))
				{
					continue;
				}

				FCrowdyModelRow Row;
				Row.Kind = ECrowdyModelRowKind::Attribute;
				Row.Name = Attribute.Key;
				Row.OwningType = OwningType;
				Row.Primary = Attribute.Key;
				// Declared in the project and not on the server yet, so the authored spelling is right here.
				Row.Display = CrowdyModelVocabulary::AttributeDisplayName(Attribute.Key, Attribute.AuthoredName);
				Row.Secondary = CrowdyModelVocabulary::ValueTypeLabel(Attribute.ValueType);
				Row.bCodeOnly = true;
				ModelLedgerApplyVerdict(Row, ClassifyAttribute(Snapshot, OwningType, Attribute.Key));
				Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Display, Row.Detail });
				Rows.Add(MoveTemp(Row));
			}
		}
	}

	ModelLedgerSortRows(Rows);
	return Rows;
}

TArray<FCrowdyModelRow> CrowdyModelLedger::BuildFunctionRows(
	const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& TypeName, const FCrowdyModelSnapshot* Snapshot)
{
	TArray<FCrowdyModelRow> Rows;
	Rows.Reserve(Functions.Num());

	for (const TSharedPtr<FStudioFunction>& Function : Functions)
	{
		if (!Function.IsValid()
			|| !ModelLedgerOwnedBy(Function->ContainerTypeName, TypeName)
			|| IsReservedFunction(Function->Name))
		{
			continue;
		}

		FCrowdyModelRow Row;
		Row.Kind = ECrowdyModelRowKind::Function;
		Row.Name = Function->Name;
		Row.OwningType = Function->ContainerTypeName;
		Row.Primary = Function->Name;
		Row.Secondary = Function->ReturnType.TrimStartAndEnd().IsEmpty()
			? FString(TEXT("No result"))
			: CrowdyModelVocabulary::ValueTypeLabel(Function->ReturnType);
		Row.Detail = Function->Description;
		ModelLedgerApplyVerdict(Row, ClassifyFunction(Snapshot, Function->ContainerTypeName, Function->Name));
		Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Detail });
		Rows.Add(MoveTemp(Row));
	}

	if (Snapshot)
	{
		for (const FCrowdyModelSnapshotFunction& Declared : Snapshot->Functions)
		{
			if (!ModelLedgerOwnedBy(Declared.TypeName, TypeName)
				|| !ModelLedgerIsPendingFunction(*Snapshot, Declared)
				|| ModelLedgerServerHasFunction(Functions, TypeName, Declared.Name))
			{
				continue;
			}

			FCrowdyModelRow Row;
			Row.Kind = ECrowdyModelRowKind::Function;
			Row.Name = Declared.Name;
			Row.OwningType = Declared.TypeName;
			Row.Primary = Declared.Name;
			Row.Secondary = Declared.ReturnType.TrimStartAndEnd().IsEmpty()
				? FString(TEXT("No result"))
				: CrowdyModelVocabulary::ValueTypeLabel(Declared.ReturnType);
			Row.Detail = Declared.Description;
			Row.bCodeOnly = true;
			ModelLedgerApplyVerdict(Row, ClassifyFunction(Snapshot, Declared.TypeName, Declared.Name));
			Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Detail });
			Rows.Add(MoveTemp(Row));
		}
	}

	ModelLedgerSortRows(Rows);
	return Rows;
}

TArray<FCrowdyModelRow> CrowdyModelLedger::BuildAutomationRows(
	const TArray<TSharedPtr<FStudioAutomation>>& Automations,
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
	const FString& TypeName,
	const FCrowdyModelSnapshot* Snapshot)
{
	TArray<FCrowdyModelRow> Rows;
	Rows.Reserve(Automations.Num());

	for (const TSharedPtr<FStudioAutomation>& Automation : Automations)
	{
		if (!Automation.IsValid() || !ModelLedgerOwnedBy(Automation->TargetTypeName, TypeName))
		{
			continue;
		}

		FCrowdyModelRow Row;
		Row.Kind = ECrowdyModelRowKind::Automation;
		Row.Name = Automation->Name;
		Row.OwningType = Automation->TargetTypeName;
		Row.Primary = Automation->Name;

		// The schedule phrase is the baseline, so an automation with no event trigger still says when it runs. The
		// triggers replace it only when they actually name an event to run on.
		Row.Secondary = CrowdyModelVocabulary::AutomationTriggerLabel(*Automation);
		const FString TriggerPhrase = ModelLedgerTriggerPhrase(Triggers, Automation->Name);
		if (!TriggerPhrase.IsEmpty())
		{
			Row.Secondary = TriggerPhrase;
		}

		Row.Detail = Automation->Description;
		ModelLedgerApplyVerdict(Row, ClassifyAutomation(Snapshot, Automation->Name));
		Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Detail });
		Rows.Add(MoveTemp(Row));
	}

	if (Snapshot)
	{
		for (const FCrowdyModelSnapshotAutomation& Declared : Snapshot->Automations)
		{
			if (!ModelLedgerOwnedBy(Declared.TargetTypeName, TypeName)
				|| !ModelLedgerIsPendingAutomation(*Snapshot, Declared)
				|| ModelLedgerServerHasAutomation(Automations, Declared.Name))
			{
				continue;
			}

			FCrowdyModelRow Row;
			Row.Kind = ECrowdyModelRowKind::Automation;
			Row.Name = Declared.Name;
			Row.OwningType = Declared.TargetTypeName;
			Row.Primary = Declared.Name;
			Row.Secondary = ModelLedgerPendingAutomationPhrase(Declared);
			Row.Detail = Declared.Description;
			Row.bCodeOnly = true;
			ModelLedgerApplyVerdict(Row, ClassifyAutomation(Snapshot, Declared.Name));
			Row.SearchKey = MakeSearchKey({ Row.Name, Row.Primary, Row.Detail });
			Rows.Add(MoveTemp(Row));
		}
	}

	ModelLedgerSortRows(Rows);
	return Rows;
}

TArray<FCrowdyModelRow> CrowdyModelLedger::BuildLiveRows(
	const TArray<TSharedPtr<FStudioContainer>>& Containers, const FString& TypeName)
{
	TArray<FCrowdyModelRow> Rows;
	Rows.Reserve(Containers.Num());

	for (const TSharedPtr<FStudioContainer>& Container : Containers)
	{
		if (!Container.IsValid() || !ModelLedgerOwnedBy(Container->TypeName, TypeName))
		{
			continue;
		}

		FCrowdyModelRow Row;
		Row.Kind = ECrowdyModelRowKind::LiveInstance;
		Row.Name = Container->ContainerId;
		Row.OwningType = Container->TypeName;

		// Nothing guarantees a live instance was given a name, and a blank first column is indistinguishable
		// from a failed read, so the id stands in for it.
		const FString DisplayName = Container->DisplayName.TrimStartAndEnd();
		Row.Primary = DisplayName.IsEmpty() ? Container->ContainerId : DisplayName;

		Row.Secondary = Container->ContainerId;
		Row.Owner = Container->OwnerUserId != 0
			? FString::Printf(TEXT("owner #%lld"), Container->OwnerUserId)
			: FString(TEXT("unowned"));
		Row.Session = Container->SessionId;
		Row.Binding = Container->BindingKey;
		Row.SearchKey = MakeSearchKey({ Row.Primary, Row.Name, Row.Session, Row.Binding });
		Rows.Add(MoveTemp(Row));
	}

	return Rows;
}

FString CrowdyModelLedger::FormatPropertySummary(const FString& PropertiesJson, int32 MaxEntries)
{
	const FString Trimmed = PropertiesJson.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(Trimmed))
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FString();
	}

	TArray<TPair<FString, TSharedPtr<FJsonValue>>> Properties;
	Properties.Reserve(Root->Values.Num());
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : Root->Values)
	{
		// The revision key and its kin are plumbing the runtime keeps for itself. Naming one here would put wire
		// vocabulary in the one line on this page written for a reader who does not have any.
		if (!IsReservedAttribute(Property.Key))
		{
			Properties.Add(Property);
		}
	}

	// A JSON object promises no key order, so two identical reads would otherwise produce two different lines.
	Properties.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B)
	{
		return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0;
	});

	// A cap below one would produce a line that names nothing and only counts, which costs a row of the page and
	// says less than the row it replaces.
	const int32 Cap = FMath::Max(MaxEntries, 1);
	const int32 Shown = FMath::Min(Properties.Num(), Cap);

	TArray<FString> Phrases;
	Phrases.Reserve(Shown);
	for (int32 Index = 0; Index < Shown; ++Index)
	{
		Phrases.Add(Properties[Index].Key + TEXT(" ") + ModelLedgerValuePhrase(Properties[Index].Value));
	}

	FString Summary = FString::Join(Phrases, TEXT(", "));

	const int32 Remaining = Properties.Num() - Shown;
	if (Remaining > 0)
	{
		if (!Summary.IsEmpty())
		{
			Summary += TEXT(", ");
		}
		Summary += FString::Printf(TEXT("and %d more"), Remaining);
	}
	return Summary;
}

TArray<FCrowdyModelSummary> CrowdyModelLedger::FilterModels(
	const TArray<FCrowdyModelSummary>& Models, const FString& Query, ECrowdyModelSourceFilter Filter)
{
	return ModelLedgerFilterBySearchKey(Models, Query, Filter);
}

TArray<FCrowdyModelRow> CrowdyModelLedger::FilterRows(
	const TArray<FCrowdyModelRow>& Rows, const FString& Query, ECrowdyModelSourceFilter Filter)
{
	return ModelLedgerFilterBySearchKey(Rows, Query, Filter);
}
