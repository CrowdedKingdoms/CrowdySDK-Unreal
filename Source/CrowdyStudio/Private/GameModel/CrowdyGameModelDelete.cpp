// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyGameModelDelete.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyModelLedger.h"     // IsReservedAttribute / IsReservedFunction, Classify*
#include "GameModel/CrowdyModelSnapshot.h"   // CrowdyModelSnapshotKeys::KeysMatch, FCrowdyModelSnapshot
#include "GameModel/CrowdyModelVocabulary.h" // DeleteKindNoun, OnEventLabel, AttributeDisplayNameFromKey
#include "GameModel/CrowdySchemaSync.h"      // ScopedNameKey
#include "Misc/SecureHash.h"

namespace
{
	// Every helper here is prefixed, because the unity build merges this module's .cpp files into one translation
	// unit and a bare name such as GetData already exists in another of them.

	bool CrowdyDeleteIsIdentifierChar(TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TEXT('_');
	}

	// A fixed-width digest of a canonical text, over UTF-8 bytes so it does not depend on the character width the
	// editor happens to be built with. Nothing reads this back; it is only ever compared against another one.
	FString CrowdyDeleteDigest(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, sizeof(Digest));
	}

	// Add a server key to a disclosure list once. TArray::AddUnique compares with FString::operator==, which folds
	// case, so two entities differing only in case would collapse into one line and one off the count beside it.
	void CrowdyDeleteAddUniqueKey(TArray<FString>& Names, const FString& Name)
	{
		if (!Name.IsEmpty() && !CrowdyModelSnapshotKeys::Contains(Names, Name))
		{
			Names.Add(Name);
		}
	}

	FString CrowdyDeleteCountPhrase(int32 Count, ECrowdyDeleteKind Kind)
	{
		return FString::Printf(TEXT("%d %s"), Count, *CrowdyModelVocabulary::DeleteKindNoun(Kind, Count != 1));
	}

	// A count of mixed kinds. "entry" is the only word available once several kinds are in play, and it is at
	// least neutral: none of the bound nouns can stand in for the others.
	FString CrowdyDeleteEntryPhrase(int32 Count)
	{
		return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? TEXT("entry") : TEXT("entries"));
	}

	FString CrowdyDeleteJoinPhrases(const TArray<FString>& Parts)
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

	// The reading order the headline names kinds in. Deliberately not CommitOrder: that one is the dependency
	// order the server forces, while a sentence reads best from the largest thing down to what hangs off it.
	const TArray<ECrowdyDeleteKind>& CrowdyDeleteReadingOrder()
	{
		static const TArray<ECrowdyDeleteKind> Order = {
			ECrowdyDeleteKind::Model,
			ECrowdyDeleteKind::Attribute,
			ECrowdyDeleteKind::Function,
			ECrowdyDeleteKind::Automation,
			ECrowdyDeleteKind::LiveInstance
		};
		return Order;
	}

	int32 CrowdyDeleteCommitPosition(ECrowdyDeleteKind Kind)
	{
		const int32 Index = CrowdyGameModelDelete::CommitOrder().IndexOfByKey(Kind);
		return Index == INDEX_NONE ? CrowdyGameModelDelete::CommitOrder().Num() : Index;
	}

	// A mark nothing can act on. A name is what the server resolves, and an attribute is resolved by (model, key):
	// with no model there is no entity to name, and picking one would aim the delete at another model's key of the
	// same spelling. Such a mark contributes no operation and no finding rather than a guess at either.
	bool CrowdyDeleteMarkIsActionable(const FCrowdyDeleteMark& Mark)
	{
		if (Mark.Name.IsEmpty())
		{
			return false;
		}
		return !(Mark.Kind == ECrowdyDeleteKind::Attribute && Mark.OwningType.IsEmpty());
	}

	void CrowdyDeleteSortNames(TArray<FString>& Names)
	{
		Names.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive) < 0;
		});
	}

	// Fill a finding's disclosure, keeping the real total when the list has to stop.
	void CrowdyDeleteSetReferences(FCrowdyDeleteFinding& Finding, const TArray<FString>& All)
	{
		Finding.ReferenceCount = All.Num();
		Finding.bReferencesTruncated = All.Num() > CrowdyDeleteMaxDisclosureEntries;
		Finding.References.Reset();
		for (int32 Index = 0; Index < All.Num() && Index < CrowdyDeleteMaxDisclosureEntries; ++Index)
		{
			Finding.References.Add(All[Index]);
		}
	}

	FCrowdyDeleteFinding CrowdyDeleteMakeFinding(
		ECrowdyDeleteFindingKind Kind, ECrowdyDeleteSeverity Severity, const FCrowdyDeleteMark& Subject,
		const FString& Headline, const FString& Remedy)
	{
		FCrowdyDeleteFinding Finding;
		Finding.Kind = Kind;
		Finding.Severity = Severity;
		Finding.Subject = Subject;
		Finding.Headline = Headline;
		Finding.Remedy = Remedy;
		return Finding;
	}

	// One entity as a sentence fragment. Used wherever a reference has to name what it is as well as what it is
	// called, since "regen" alone does not say whether it is a function or an attribute.
	FString CrowdyDeleteDescribeScopedName(ECrowdyDeleteKind Kind, const FString& OwningType, const FString& Name)
	{
		const FString Noun = CrowdyModelVocabulary::DeleteKindNoun(Kind, /*bPlural*/ false);
		return OwningType.IsEmpty()
			? FString::Printf(TEXT("the %s %s"), *Noun, *Name)
			: FString::Printf(TEXT("the %s %s on %s"), *Noun, *Name, *OwningType);
	}

	// The verdict the ledger would give this entity, asked through the ledger's own classification so this page
	// and the Source column can never disagree. Authorship comes from the plan's DESIRED side inside those
	// functions; nothing here inverts a server-only list, which already has kit protection and skipped authors
	// folded into it and is therefore wrong in opposite directions for a kit type and a broken effect's type.
	ECrowdyModelProvenance CrowdyDeleteProvenanceOf(const FCrowdyDeleteEvidence& Evidence, const FCrowdyDeleteMark& Mark)
	{
		const FCrowdyModelSnapshot* Snapshot = Evidence.Snapshot.Get();
		switch (Mark.Kind)
		{
		case ECrowdyDeleteKind::Model:
			return CrowdyModelLedger::ClassifyModel(Snapshot, Mark.Name).Provenance;
		case ECrowdyDeleteKind::Attribute:
			return CrowdyModelLedger::ClassifyAttribute(Snapshot, Mark.OwningType, Mark.Name).Provenance;
		case ECrowdyDeleteKind::Function:
			return CrowdyModelLedger::ClassifyFunction(Snapshot, Mark.OwningType, Mark.Name).Provenance;
		case ECrowdyDeleteKind::Automation:
			return CrowdyModelLedger::ClassifyAutomation(Snapshot, Mark.Name).Provenance;
		default:
			return ECrowdyModelProvenance::Unknown;
		}
	}

	// Whether something outside this app's server state puts the entity back. Unknown is deliberately absent: it
	// means nobody has asked, which the gate already refuses on, and repeating it as a caution would fire on every
	// mark in an app nobody has planned and drown the cautions that are about real consequences.
	bool CrowdyDeleteIsRestoredByAuthor(ECrowdyModelProvenance Provenance)
	{
		return Provenance == ECrowdyModelProvenance::CodeSynced
			|| Provenance == ECrowdyModelProvenance::CodeNotPushed
			|| Provenance == ECrowdyModelProvenance::CodeDrifted
			|| Provenance == ECrowdyModelProvenance::KitOwned;
	}

	// Whether an automation runs against a model, by its own target or by an event trigger that narrows to the
	// model. The trigger half cannot live in AutomationTargetsModel, which is handed no triggers.
	bool CrowdyDeleteAutomationTouchesModel(
		const FStudioAutomation& Automation, const FCrowdyDeleteEvidence& Evidence, const FString& TypeName)
	{
		if (CrowdyGameModelDelete::AutomationTargetsModel(Automation, TypeName))
		{
			return true;
		}

		for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Evidence.AutomationTriggers)
		{
			if (Trigger.IsValid()
				&& CrowdyModelSnapshotKeys::KeysMatch(Trigger->AutomationName, Automation.Name)
				&& !Trigger->ContainerTypeName.IsEmpty()
				&& CrowdyModelSnapshotKeys::KeysMatch(Trigger->ContainerTypeName, TypeName))
			{
				return true;
			}
		}
		return false;
	}

	// Whether two operations are the same call on the same entity. The arguments alone are not enough: a function
	// delete carries only the bare name, so two functions of one name on two models produce byte-identical
	// arguments while being two entities. The scope is compared as well, which is what keeps the plan from
	// resolving the unsettled question of whether the mutation is app-wide by quietly assuming that it is.
	bool CrowdyDeleteOpsAreTheSameCall(const FCrowdyDeleteOp& A, const FCrowdyDeleteOp& B)
	{
		if (A.Kind != B.Kind
			|| !CrowdyModelSnapshotKeys::KeysMatch(A.OperationName, B.OperationName)
			|| !CrowdyModelSnapshotKeys::KeysMatch(A.Subject.OwningModelName(), B.Subject.OwningModelName())
			|| A.StringArgs.Num() != B.StringArgs.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < A.StringArgs.Num(); ++Index)
		{
			if (!CrowdyModelSnapshotKeys::KeysMatch(A.StringArgs[Index].Key, B.StringArgs[Index].Key)
				|| !CrowdyModelSnapshotKeys::KeysMatch(A.StringArgs[Index].Value, B.StringArgs[Index].Value))
			{
				return false;
			}
		}
		return true;
	}

	// Whether this plan removes a function, so a reference it holds is not a reference that survives. A function is
	// removed when it is marked, or when it is the SDK's own wiring on a marked model, which the plan deletes
	// itself. Nothing else counts: a function bound to a marked model but not marked is what the model delete is
	// refused over, so it is still there.
	bool CrowdyDeleteFunctionIsRemovedByPlan(const FStudioFunction& Function, const TArray<FCrowdyDeleteMark>& Marks)
	{
		if (Function.Name.IsEmpty())
		{
			return false;
		}
		if (CrowdyGameModelDelete::ContainsMark(
			Marks, CrowdyGameModelDelete::MarkFunction(Function.ContainerTypeName, Function.Name, Function.Name)))
		{
			return true;
		}
		if (!CrowdyModelLedger::IsReservedFunction(Function.Name) || Function.ContainerTypeName.IsEmpty())
		{
			return false;
		}
		return CrowdyGameModelDelete::ContainsMark(
			Marks, CrowdyGameModelDelete::MarkModel(Function.ContainerTypeName, Function.ContainerTypeName));
	}

	// Whether this plan removes an automation.
	bool CrowdyDeleteAutomationIsRemovedByPlan(const FString& AutomationName, const TArray<FCrowdyDeleteMark>& Marks)
	{
		return !AutomationName.IsEmpty()
			&& CrowdyGameModelDelete::ContainsMark(
				Marks, CrowdyGameModelDelete::MarkAutomation(AutomationName, AutomationName));
	}

	// Everything this plan LEAVES BEHIND that names one attribute key, as display lines. What the plan itself
	// deletes is excluded: a function that is going cannot be broken by a key going with it. Two destinations
	// rather than one, because a function and an automation break in different ways and the sheet says so
	// separately.
	void CrowdyDeleteCollectAttributeDependants(
		const FCrowdyDeleteEvidence& Evidence, const TArray<FCrowdyDeleteMark>& Marks, const FString& OwningType,
		const FString& Key, TArray<FString>& OutFunctions, TArray<FString>& OutAutomations)
	{
		if (Key.IsEmpty())
		{
			return;
		}

		if (Evidence.bFunctionsRead)
		{
			for (const TSharedPtr<FStudioFunction>& Function : Evidence.Functions)
			{
				if (!Function.IsValid() || Function->Name.IsEmpty()
					|| CrowdyDeleteFunctionIsRemovedByPlan(*Function, Marks))
				{
					continue;
				}
				if (CrowdyGameModelDelete::FunctionReferencesAttribute(*Function, OwningType, Key))
				{
					CrowdyDeleteAddUniqueKey(OutFunctions, CrowdyDeleteDescribeScopedName(
						ECrowdyDeleteKind::Function, Function->ContainerTypeName, Function->Name));
				}
			}
		}

		if (!Evidence.bAutomationsRead)
		{
			return;
		}

		for (const TSharedPtr<FStudioAutomation>& Automation : Evidence.Automations)
		{
			if (!Automation.IsValid() || Automation->Name.IsEmpty()
				|| CrowdyDeleteAutomationIsRemovedByPlan(Automation->Name, Marks))
			{
				continue;
			}
			if (CrowdyGameModelDelete::AutomationReferencesAttribute(*Automation, Key))
			{
				CrowdyDeleteAddUniqueKey(OutAutomations, Automation->Name);
			}
		}

		// An event trigger goes with its automation, so a trigger whose automation this plan deletes names nothing
		// that survives. One that stays waits forever on a key that will never change again.
		for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Evidence.AutomationTriggers)
		{
			if (!Trigger.IsValid() || Trigger->AutomationName.IsEmpty()
				|| CrowdyDeleteAutomationIsRemovedByPlan(Trigger->AutomationName, Marks))
			{
				continue;
			}
			if (CrowdyGameModelDelete::TriggerReferencesAttribute(*Trigger, OwningType, Key))
			{
				CrowdyDeleteAddUniqueKey(OutAutomations, Trigger->AutomationName);
			}
		}
	}

	// The gate's answer for one provenance, so a row in a table and a model in the list cannot be judged by two
	// different rules. What is being protected is the reader's work: an entity something else declares comes
	// straight back, so a delete here spends a destructive server write to achieve nothing.
	FCrowdyDeleteGate CrowdyDeleteGateForProvenance(
		ECrowdyModelProvenance Provenance, const FString& What, const FString& CodePath)
	{
		FCrowdyDeleteGate Gate;
		switch (Provenance)
		{
		case ECrowdyModelProvenance::ServerOnly:
			Gate.bAllowed = true;
			return Gate;

		case ECrowdyModelProvenance::CodeSynced:
		case ECrowdyModelProvenance::CodeNotPushed:
		case ECrowdyModelProvenance::CodeDrifted:
			Gate.Reason = FString::Printf(
				TEXT("This project declares %s in code, so the next Sync to Server would create it again. ")
				TEXT("Remove it from the project instead."), *What);
			Gate.CodePath = CodePath;
			return Gate;

		case ECrowdyModelProvenance::KitOwned:
			Gate.Reason = FString::Printf(
				TEXT("A Game Kit deployed %s, so deploying that kit again would create it. ")
				TEXT("Change what the kit deploys instead."), *What);
			Gate.CodePath = CodePath;
			return Gate;

		default:
			Gate.Reason = FString::Printf(
				TEXT("Nothing has checked yet whether this project declares %s, so deleting it could be undone by ")
				TEXT("the next sync. Press Preview changes, then try again."), *What);
			Gate.CodePath = CodePath;
			return Gate;
		}
	}
}

FString FCrowdyDeleteMark::OwningModelName() const
{
	return Kind == ECrowdyDeleteKind::Model ? Name : OwningType;
}

FString FCrowdyDeleteMark::IdentityKey() const
{
	// The kind leads, because an attribute and a function on one model can share a name and are two entities. The
	// rest is the diff's own scoped key, so a mark and the schema plan agree about which entity is which.
	return FString::Printf(
		TEXT("%d\n%s"), static_cast<int32>(Kind), *FCrowdySchemaSync::ScopedNameKey(OwningType, Name));
}

bool FCrowdyDeleteMark::operator==(const FCrowdyDeleteMark& Other) const
{
	// Display is not identity: two entities that happen to render the same label are still two entities.
	return Kind == Other.Kind
		&& CrowdyModelSnapshotKeys::KeysMatch(OwningType, Other.OwningType)
		&& CrowdyModelSnapshotKeys::KeysMatch(Name, Other.Name);
}

const TArray<TSharedPtr<FStudioPropertyDef>>* FCrowdyDeleteEvidence::FindAttributes(const FString& TypeName) const
{
	if (TypeName.IsEmpty())
	{
		return nullptr;
	}

	for (const FCrowdyDeleteModelAttributes& Entry : AttributesByModel)
	{
		if (CrowdyModelSnapshotKeys::KeysMatch(Entry.TypeName, TypeName))
		{
			return &Entry.Attributes;
		}
	}
	return nullptr;
}

const FCrowdyDeleteLiveCount* FCrowdyDeleteEvidence::FindLiveCountEntry(const FString& TypeName) const
{
	if (TypeName.IsEmpty())
	{
		return nullptr;
	}

	for (const FCrowdyDeleteLiveCount& Entry : LiveCounts)
	{
		if (CrowdyModelSnapshotKeys::KeysMatch(Entry.TypeName, TypeName))
		{
			return &Entry;
		}
	}
	return nullptr;
}

ECrowdyLiveCountState FCrowdyDeleteEvidence::FindLiveCount(const FString& TypeName, int32& OutCount) const
{
	OutCount = 0;

	const FCrowdyDeleteLiveCount* Entry = FindLiveCountEntry(TypeName);
	if (!Entry || Entry->State == ECrowdyLiveCountState::Unknown)
	{
		// An absent entry and a failed read are the same answer, and it is not zero.
		return ECrowdyLiveCountState::Unknown;
	}

	OutCount = Entry->Count;
	return Entry->State;
}

int32 FCrowdyDeletePlan::CountOps(ECrowdyDeleteKind Kind) const
{
	int32 Count = 0;
	for (const FCrowdyDeleteOp& Op : Ops)
	{
		if (Op.Kind == Kind)
		{
			++Count;
		}
	}
	return Count;
}

int32 FCrowdyDeletePlan::CountImpliedOps() const
{
	int32 Count = 0;
	for (const FCrowdyDeleteOp& Op : Ops)
	{
		if (Op.bImplied)
		{
			++Count;
		}
	}
	return Count;
}

const TArray<ECrowdyDeleteKind>& CrowdyGameModelDelete::CommitOrder()
{
	// The schema prune's existing order with live models inserted. Automations first, because an automation
	// references a function and deleting the function under one is refused, and deleting the automation takes its
	// event triggers with it so they need no slot. Live models next, because a model delete is refused while any
	// exist, and before attributes so that everything a model delete is refused over is cleared before anything
	// else touches that model. Attributes next: their delete refuses nothing and blocks nothing, so its position
	// is free, and here it cannot be blamed on a function that has already gone. Functions before models, since a
	// model with a bound function is refused. Models last, every refusal they carry now cleared.
	static const TArray<ECrowdyDeleteKind> Order = {
		ECrowdyDeleteKind::Automation,
		ECrowdyDeleteKind::LiveInstance,
		ECrowdyDeleteKind::Attribute,
		ECrowdyDeleteKind::Function,
		ECrowdyDeleteKind::Model
	};
	return Order;
}

ECrowdyDeleteKind CrowdyGameModelDelete::DeleteKindForRowKind(ECrowdyModelRowKind RowKind)
{
	switch (RowKind)
	{
	case ECrowdyModelRowKind::Model:        return ECrowdyDeleteKind::Model;
	case ECrowdyModelRowKind::Function:     return ECrowdyDeleteKind::Function;
	case ECrowdyModelRowKind::Automation:   return ECrowdyDeleteKind::Automation;
	case ECrowdyModelRowKind::LiveInstance: return ECrowdyDeleteKind::LiveInstance;
	default:                                return ECrowdyDeleteKind::Attribute;
	}
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkFromRow(const FCrowdyModelRow& Row)
{
	// The label the row itself shows, so the sheet names what the reader ticked in the words they read it under.
	// Name stays the identity on every branch below.
	const FString& Shown = Row.DisplayLabel();

	switch (DeleteKindForRowKind(Row.Kind))
	{
	case ECrowdyDeleteKind::Model:        return MarkModel(Row.Name, Shown);
	case ECrowdyDeleteKind::Function:     return MarkFunction(Row.OwningType, Row.Name, Shown);
	case ECrowdyDeleteKind::Automation:   return MarkAutomation(Row.Name, Shown);
	case ECrowdyDeleteKind::LiveInstance: return MarkLiveModel(Row.OwningType, Row.Name, Shown);
	default:                              return MarkAttribute(Row.OwningType, Row.Name, Shown);
	}
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkFromModel(const FCrowdyModelSummary& Model)
{
	return MarkModel(Model.TypeName, Model.Display);
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkModel(const FString& TypeName, const FString& Display)
{
	FCrowdyDeleteMark Mark;
	Mark.Kind = ECrowdyDeleteKind::Model;
	Mark.Name = TypeName;
	Mark.Display = Display.IsEmpty() ? TypeName : Display;
	return Mark;
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkAttribute(
	const FString& OwningType, const FString& Key, const FString& Display)
{
	FCrowdyDeleteMark Mark;
	Mark.Kind = ECrowdyDeleteKind::Attribute;
	Mark.OwningType = OwningType;
	Mark.Name = Key;
	Mark.Display = Display.IsEmpty() ? Key : Display;
	return Mark;
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkFunction(
	const FString& OwningType, const FString& Name, const FString& Display)
{
	FCrowdyDeleteMark Mark;
	Mark.Kind = ECrowdyDeleteKind::Function;
	Mark.OwningType = OwningType;
	Mark.Name = Name;
	Mark.Display = Display.IsEmpty() ? Name : Display;
	return Mark;
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkAutomation(const FString& Name, const FString& Display)
{
	FCrowdyDeleteMark Mark;
	Mark.Kind = ECrowdyDeleteKind::Automation;
	Mark.Name = Name;
	Mark.Display = Display.IsEmpty() ? Name : Display;
	return Mark;
}

FCrowdyDeleteMark CrowdyGameModelDelete::MarkLiveModel(
	const FString& OwningType, const FString& ContainerId, const FString& Display)
{
	FCrowdyDeleteMark Mark;
	Mark.Kind = ECrowdyDeleteKind::LiveInstance;
	Mark.OwningType = OwningType;
	Mark.Name = ContainerId;
	Mark.Display = Display.IsEmpty() ? ContainerId : Display;
	return Mark;
}

bool CrowdyGameModelDelete::ContainsMark(const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteMark& Mark)
{
	for (const FCrowdyDeleteMark& Candidate : Marks)
	{
		if (Candidate == Mark)
		{
			return true;
		}
	}
	return false;
}

FString CrowdyGameModelDelete::DescribeMark(const FCrowdyDeleteMark& Mark, const FCrowdyDeleteEvidence& Evidence)
{
	const FString Noun = CrowdyModelVocabulary::DeleteKindNoun(Mark.Kind, /*bPlural*/ false);
	const FString Named = Mark.Display.IsEmpty() ? Mark.Name : Mark.Display;
	const FString Owner = Mark.OwningType.TrimStartAndEnd();

	if (Mark.Kind == ECrowdyDeleteKind::Function && ScopesCarryingFunctionName(Evidence, Mark.Name).Num() >= 2)
	{
		// The marked list is the only place the reader is told what they picked before they open the review, so it
		// must not be the one place that promises a scoping the mutation may not honour.
		return FString::Printf(
			TEXT("%s %s (this app has that name on more than one model)"), *Noun, *Named);
	}

	if (Owner.IsEmpty())
	{
		return FString::Printf(TEXT("%s %s"), *Noun, *Named);
	}

	return Mark.Kind == ECrowdyDeleteKind::LiveInstance
		? FString::Printf(TEXT("%s %s of %s"), *Noun, *Named, *Owner)
		: FString::Printf(TEXT("%s %s on %s"), *Noun, *Named, *Owner);
}

bool CrowdyGameModelDelete::IsSubsumed(const FCrowdyDeleteMark& Mark, const TArray<FCrowdyDeleteMark>& Marks)
{
	// Only an attribute of a marked model. A function on a marked model is NOT subsumed: the model delete is
	// refused while it exists, so the function's own operation is what clears the refusal rather than something
	// the model absorbs. A live model is not subsumed for exactly the same reason.
	if (Mark.Kind != ECrowdyDeleteKind::Attribute || Mark.OwningType.IsEmpty())
	{
		return false;
	}

	for (const FCrowdyDeleteMark& Candidate : Marks)
	{
		if (Candidate.Kind == ECrowdyDeleteKind::Model
			&& CrowdyModelSnapshotKeys::KeysMatch(Candidate.Name, Mark.OwningType))
		{
			return true;
		}
	}
	return false;
}

FCrowdyDeleteGate CrowdyGameModelDelete::CanDeleteFromPrimaryView(const FCrowdyModelRow& Row)
{
	FCrowdyDeleteGate Gate;

	// Runtime state. No class declares a live model and no sync creates one, so the trap this gate exists for
	// cannot apply to it.
	if (Row.Kind == ECrowdyModelRowKind::LiveInstance)
	{
		Gate.bAllowed = !Row.Name.IsEmpty();
		if (!Gate.bAllowed)
		{
			Gate.Reason = TEXT("This live model has no id, so there is nothing here to delete.");
		}
		return Gate;
	}

	if (Row.Name.IsEmpty())
	{
		Gate.Reason = TEXT("This row has no name, so there is nothing here to delete.");
		return Gate;
	}

	if (Row.bCodeOnly)
	{
		Gate.Reason = TEXT("The server does not have this yet, so there is nothing on it to delete.");
		Gate.CodePath = Row.CodePath;
		return Gate;
	}

	// An attribute and a function are resolved within a model. A row whose model is empty, moved, or was never
	// worked out cannot be aimed at anything, and the branch that would otherwise catch it is the branch that
	// offers the delete.
	if ((Row.Kind == ECrowdyModelRowKind::Attribute || Row.Kind == ECrowdyModelRowKind::Function)
		&& Row.OwningType.IsEmpty())
	{
		Gate.Reason = TEXT("Which model this belongs to is not known here, so a delete could act on another ")
			TEXT("model's entry of the same name. Press Refresh, then try again.");
		Gate.CodePath = Row.CodePath;
		return Gate;
	}

	const FString& Shown = Row.DisplayLabel();
	const FString What = CrowdyDeleteDescribeScopedName(
		DeleteKindForRowKind(Row.Kind), Row.OwningType, Shown.IsEmpty() ? Row.Name : Shown);
	return CrowdyDeleteGateForProvenance(Row.Provenance, What, Row.CodePath);
}

FCrowdyDeleteGate CrowdyGameModelDelete::CanDeleteFromPrimaryView(const FCrowdyModelSummary& Model)
{
	FCrowdyDeleteGate Gate;

	if (Model.TypeName.IsEmpty())
	{
		Gate.Reason = TEXT("This model has no name, so there is nothing here to delete.");
		return Gate;
	}

	if (Model.bCodeOnly)
	{
		Gate.Reason = TEXT("The server does not have this model yet, so there is nothing on it to delete.");
		Gate.CodePath = Model.CodePath;
		return Gate;
	}

	const FString What = CrowdyDeleteDescribeScopedName(
		ECrowdyDeleteKind::Model, FString(), Model.Display.IsEmpty() ? Model.TypeName : Model.Display);
	return CrowdyDeleteGateForProvenance(Model.Provenance, What, Model.CodePath);
}

bool CrowdyGameModelDelete::IsEvidenceSufficient(const FCrowdyDeleteEvidence& Evidence, FString& OutReason)
{
	OutReason.Reset();

	if (Evidence.AppId == 0)
	{
		OutReason = TEXT("No app is selected, so there is nothing to delete from.");
		return false;
	}

	// Each of these lists answers a question a delete depends on, and an empty array means "this app has none"
	// only once its read has happened. Folding the two would let an unread function list clear the bound-function
	// refusal for every model in the app at once.
	if (!Evidence.bTypesRead)
	{
		OutReason = TEXT("This app's models have not been read yet. Press Refresh, then open this review again.");
		return false;
	}
	if (!Evidence.bFunctionsRead)
	{
		OutReason = TEXT("This app's functions have not been read yet, so nothing here can say what a delete ")
			TEXT("would break. Press Refresh, then open this review again.");
		return false;
	}
	if (!Evidence.bAutomationsRead)
	{
		OutReason = TEXT("This app's automations have not been read yet, so nothing here can say what a delete ")
			TEXT("would break. Press Refresh, then open this review again.");
		return false;
	}

	return true;
}

TArray<FCrowdyDeleteFinding> CrowdyGameModelDelete::BuildFindings(
	const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence)
{
	TArray<FCrowdyDeleteFinding> Findings;

	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		if (!CrowdyDeleteMarkIsActionable(Mark) || IsSubsumed(Mark, Marks))
		{
			// A subsumed attribute goes with its model, and a model delete is refused while live models exist, so
			// by the time it runs there is nothing left holding a value for that attribute.
			continue;
		}

		if (Mark.Kind == ECrowdyDeleteKind::Model)
		{
			const FString& TypeName = Mark.Name;

			int32 ProbedCount = 0;
			const ECrowdyLiveCountState CountState = Evidence.FindLiveCount(TypeName, ProbedCount);
			// What the plan leaves standing, not what the probe found: a live model marked in this plan is deleted
			// before the model is, exactly as a marked function is, so it is not what the server refuses over.
			const int32 LiveCount = LiveModelsBlockingModelDelete(TypeName, Marks, Evidence);
			if (CountState == ECrowdyLiveCountState::Unknown)
			{
				Findings.Add(CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::ModelLiveCountUnknown, ECrowdyDeleteSeverity::Blocker, Mark,
					FString::Printf(
						TEXT("Nothing has counted the live models of %s, so this cannot say whether the server ")
						TEXT("will refuse to delete it."), *Mark.Display),
					TEXT("Close this review and open it again. The count runs when the review opens.")));
			}
			else if (LiveCount > 0)
			{
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::ModelHasLiveModels, ECrowdyDeleteSeverity::Blocker, Mark,
					CountState == ECrowdyLiveCountState::AtLeast
						? FString::Printf(
							TEXT("%s has at least %d live models. The server refuses to delete a model while any ")
							TEXT("of them exist."), *Mark.Display, LiveCount)
						: FString::Printf(
							TEXT("%s has %s. The server refuses to delete a model while any of them exist."),
							*Mark.Display, *CrowdyDeleteCountPhrase(LiveCount, ECrowdyDeleteKind::LiveInstance)),
					TEXT("Mark those live models for deletion too. This plan then deletes them before the model."));

				if (const FCrowdyDeleteLiveCount* Entry = Evidence.FindLiveCountEntry(TypeName))
				{
					// Only the ones still standing. A sample id already marked in this plan is one the walk deletes
					// first, so listing it here would name it as the thing stopping its own delete.
					TArray<FString> Standing;
					for (const FString& SampleId : Entry->SampleIds)
					{
						if (!ContainsMark(Marks, MarkLiveModel(TypeName, SampleId, SampleId)))
						{
							Standing.Add(SampleId);
						}
					}
					CrowdyDeleteSetReferences(Finding, Standing);
					// The samples are a window onto the count, not the count itself, so the total stays the count.
					Finding.ReferenceCount = LiveCount;
					Finding.bReferencesTruncated = Finding.References.Num() < LiveCount;
				}
				Findings.Add(MoveTemp(Finding));
			}

			if (!Evidence.bFunctionsRead)
			{
				Findings.Add(CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::ModelHasBoundFunctions, ECrowdyDeleteSeverity::Blocker, Mark,
					FString::Printf(
						TEXT("Nothing has read this app's functions, so this cannot say whether any are still ")
						TEXT("bound to %s. The server refuses to delete a model while one is."), *Mark.Display),
					TEXT("Press Refresh, then open this review again.")));
			}
			else
			{
				const TArray<FString> Blocking = FunctionsBlockingModelDelete(TypeName, Marks, Evidence);
				if (Blocking.Num() > 0)
				{
					FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::ModelHasBoundFunctions, ECrowdyDeleteSeverity::Blocker, Mark,
						FString::Printf(
							TEXT("%s still has %s bound to it. The server refuses to delete a model while one is."),
							*Mark.Display,
							*CrowdyDeleteCountPhrase(Blocking.Num(), ECrowdyDeleteKind::Function)),
						TEXT("Mark those functions for deletion too. This plan then deletes them before the model."));
					CrowdyDeleteSetReferences(Finding, Blocking);
					Findings.Add(MoveTemp(Finding));
				}
			}

			if (Evidence.bAutomationsRead)
			{
				TArray<FString> Targeting;
				for (const TSharedPtr<FStudioAutomation>& Automation : Evidence.Automations)
				{
					if (!Automation.IsValid() || Automation->Name.IsEmpty())
					{
						continue;
					}
					if (ContainsMark(Marks, MarkAutomation(Automation->Name, Automation->Name)))
					{
						continue; // this plan deletes it, so nothing survives pointing at a model that has gone
					}
					if (CrowdyDeleteAutomationTouchesModel(*Automation, Evidence, TypeName))
					{
						CrowdyDeleteAddUniqueKey(Targeting, Automation->Name);
					}
				}

				if (Targeting.Num() > 0)
				{
					CrowdyDeleteSortNames(Targeting);
					FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::ModelTargetedByAutomations, ECrowdyDeleteSeverity::Caution, Mark,
						FString::Printf(
							TEXT("%s run against %s. They survive with a target that no longer exists."),
							*CrowdyDeleteCountPhrase(Targeting.Num(), ECrowdyDeleteKind::Automation), *Mark.Display),
						TEXT("Mark those automations for deletion too, or point them at another model."));
					CrowdyDeleteSetReferences(Finding, Targeting);
					Findings.Add(MoveTemp(Finding));
				}
			}

			const TArray<TSharedPtr<FStudioPropertyDef>>* Attributes = Evidence.FindAttributes(TypeName);
			if (!Attributes)
			{
				// Never read. A count would be invented, and zero is the one answer that is definitely wrong.
				Findings.Add(CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::ModelCascadesItsAttributes, ECrowdyDeleteSeverity::Info, Mark,
					FString::Printf(TEXT("Deleting %s deletes every attribute on it."), *Mark.Display),
					FString()));
			}
			else
			{
				TArray<FString> Keys;
				for (const TSharedPtr<FStudioPropertyDef>& Def : *Attributes)
				{
					if (Def.IsValid() && !Def->Key.IsEmpty() && !CrowdyModelLedger::IsReservedAttribute(Def->Key))
					{
						CrowdyDeleteAddUniqueKey(Keys, Def->Key);
					}
				}
				if (Keys.Num() > 0)
				{
					CrowdyDeleteSortNames(Keys);
					FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::ModelCascadesItsAttributes, ECrowdyDeleteSeverity::Info, Mark,
						FString::Printf(TEXT("Deleting %s deletes its %s with it."),
							*Mark.Display, *CrowdyDeleteCountPhrase(Keys.Num(), ECrowdyDeleteKind::Attribute)),
						FString());
					CrowdyDeleteSetReferences(Finding, Keys);
					Findings.Add(MoveTemp(Finding));
				}

				// The cascade above is the intended consequence; this is the part of it nobody asked for. Every key
				// the model takes with it can be named by a function or an automation somewhere else, and that
				// breaks exactly as it would if the key had been marked on its own. Marking the model rather than
				// the key does not make it a smaller consequence, so it carries the same severity.
				TArray<FString> Dependants;
				TArray<FString> AutomationDependants;
				for (const FString& Key : Keys)
				{
					CrowdyDeleteCollectAttributeDependants(
						Evidence, Marks, TypeName, Key, Dependants, AutomationDependants);
				}
				for (const FString& AutomationName : AutomationDependants)
				{
					CrowdyDeleteAddUniqueKey(Dependants, CrowdyDeleteDescribeScopedName(
						ECrowdyDeleteKind::Automation, FString(), AutomationName));
				}

				if (Dependants.Num() > 0)
				{
					CrowdyDeleteSortNames(Dependants);
					FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::ModelCascadeOrphansAttributeReaders,
						ECrowdyDeleteSeverity::Caution, Mark,
						FString::Printf(
							TEXT("%s elsewhere still name the attributes that go with %s. They survive, naming keys ")
							TEXT("that no longer exist."),
							*CrowdyDeleteEntryPhrase(Dependants.Num()), *Mark.Display),
						TEXT("Update or delete them too."));
					CrowdyDeleteSetReferences(Finding, Dependants);
					Findings.Add(MoveTemp(Finding));
				}
			}

			const TArray<FString> Plumbing = PlumbingFunctionsFor(TypeName, Evidence);
			if (Plumbing.Num() > 0)
			{
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::ModelCascadesItsPlumbing, ECrowdyDeleteSeverity::Info, Mark,
					FString::Printf(
						TEXT("%s also carries wiring the SDK keeps for itself, which is never shown as a row. ")
						TEXT("This plan deletes it first, because the server would otherwise refuse to delete %s ")
						TEXT("over something you were never shown."), *Mark.Display, *Mark.Display),
					FString());
				CrowdyDeleteSetReferences(Finding, Plumbing);
				Findings.Add(MoveTemp(Finding));

				// The wiring is deleted by the same bare-name mutation everything else is, so it carries the same
				// ambiguity and nobody marked it to be told about it. A model name that differs from another only by
				// case produces the same wiring name for both, so this is reachable without anyone doing anything
				// odd.
				TArray<FString> AmbiguousPlumbing;
				for (const FString& Name : Plumbing)
				{
					const TArray<FString> Scopes = ScopesCarryingFunctionName(Evidence, Name);
					if (Scopes.Num() >= 2)
					{
						for (const FString& Scope : Scopes)
						{
							CrowdyDeleteAddUniqueKey(AmbiguousPlumbing, Scope);
						}
					}
				}
				if (AmbiguousPlumbing.Num() > 0)
				{
					CrowdyDeleteSortNames(AmbiguousPlumbing);
					FCrowdyDeleteFinding Ambiguous = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp, ECrowdyDeleteSeverity::Caution, Mark,
						FString::Printf(
							TEXT("The wiring %s carries has the same name on more than one model, and the delete ")
							TEXT("takes the name alone. Every one of them may go, not only this model's."),
							*Mark.Display),
						TEXT("Rename the models that collide, run a sync, then delete."));
					CrowdyDeleteSetReferences(Ambiguous, AmbiguousPlumbing);
					Findings.Add(MoveTemp(Ambiguous));
				}
			}
		}
		else if (Mark.Kind == ECrowdyDeleteKind::Attribute)
		{
			Findings.Add(CrowdyDeleteMakeFinding(
				ECrowdyDeleteFindingKind::AttributeOrphansStoredValues, ECrowdyDeleteSeverity::Caution, Mark,
				FString::Printf(
					TEXT("Deleting %s leaves the value already stored for it on every live model of %s. The ")
					TEXT("server neither refuses this nor clears those values."), *Mark.Display, *Mark.OwningType),
				TEXT("Nothing undoes this. Check the Live tab for models still holding a value first.")));

			// Everything that names the key, gathered in one place: a function's writes, the value it answers with,
			// its authority gate, its notification arguments and its timers, plus the rule an automation picks its
			// targets by and the event a trigger waits for. Only one of those is a mutation, and a scan that read
			// mutations alone would report no dependants at all for the other six.
			TArray<FString> Readers;
			TArray<FString> Runners;
			CrowdyDeleteCollectAttributeDependants(
				Evidence, Marks, Mark.OwningType, Mark.Name, Readers, Runners);

			if (Readers.Num() > 0)
			{
				CrowdyDeleteSortNames(Readers);
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::AttributeReadByFunctions, ECrowdyDeleteSeverity::Caution, Mark,
					FString::Printf(
						TEXT("%s name %s. Their expressions keep naming a key that no longer exists."),
						*CrowdyDeleteCountPhrase(Readers.Num(), ECrowdyDeleteKind::Function), *Mark.Display),
					TEXT("Update or delete those functions too."));
				CrowdyDeleteSetReferences(Finding, Readers);
				Findings.Add(MoveTemp(Finding));
			}

			if (Runners.Num() > 0)
			{
				CrowdyDeleteSortNames(Runners);
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::AttributeUsedByAutomations, ECrowdyDeleteSeverity::Caution, Mark,
					FString::Printf(
						TEXT("%s name %s, to choose what they run against or to decide when to run. They survive, ")
						TEXT("and what they read is no longer there."),
						*CrowdyDeleteCountPhrase(Runners.Num(), ECrowdyDeleteKind::Automation), *Mark.Display),
					TEXT("Update or delete those automations too."));
				CrowdyDeleteSetReferences(Finding, Runners);
				Findings.Add(MoveTemp(Finding));
			}
		}
		else if (Mark.Kind == ECrowdyDeleteKind::Function)
		{
			// Raised from the same list it is disclosed from. A record whose model this app never reported is its
			// own scope and is what makes the delete ambiguous, so a disclosure built from models alone would omit
			// exactly the entity that raised the caution and read as a bug in the tool.
			const TArray<FString> Carriers = ScopesCarryingFunctionName(Evidence, Mark.Name);
			if (Carriers.Num() >= 2)
			{
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp, ECrowdyDeleteSeverity::Caution, Mark,
					FString::Printf(
						TEXT("This app has a function called %s on more than one model, and the delete takes the ")
						TEXT("name alone. Every one of them may go, not only the one you marked."), *Mark.Name),
					TEXT("Rename the ones you want to keep first, then delete."));
				CrowdyDeleteSetReferences(Finding, Carriers);
				Findings.Add(MoveTemp(Finding));
			}

			if (Evidence.bAutomationsRead)
			{
				TArray<FString> Runners;
				for (const TSharedPtr<FStudioAutomation>& Automation : Evidence.Automations)
				{
					if (!Automation.IsValid() || Automation->Name.IsEmpty())
					{
						continue;
					}
					if (ContainsMark(Marks, MarkAutomation(Automation->Name, Automation->Name)))
					{
						continue;
					}
					if (AutomationReferencesFunction(*Automation, Mark.Name))
					{
						CrowdyDeleteAddUniqueKey(Runners, Automation->Name);
					}
				}
				for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Evidence.AutomationTriggers)
				{
					if (!Trigger.IsValid() || Trigger->AutomationName.IsEmpty())
					{
						continue;
					}
					if (ContainsMark(Marks, MarkAutomation(Trigger->AutomationName, Trigger->AutomationName)))
					{
						continue;
					}
					if (TriggerReferencesFunction(*Trigger, Mark.Name))
					{
						CrowdyDeleteAddUniqueKey(Runners, Trigger->AutomationName);
					}
				}

				if (Runners.Num() > 0)
				{
					CrowdyDeleteSortNames(Runners);
					FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
						ECrowdyDeleteFindingKind::FunctionRunByAutomations, ECrowdyDeleteSeverity::Caution, Mark,
						FString::Printf(
							TEXT("%s run %s. Nothing repairs them: either the server refuses this delete while they ")
							TEXT("point at it, or it goes through and what they run no longer resolves."),
							*CrowdyDeleteCountPhrase(Runners.Num(), ECrowdyDeleteKind::Automation), *Mark.Display),
						TEXT("Mark those automations for deletion too, or point them at another function."));
					CrowdyDeleteSetReferences(Finding, Runners);
					Findings.Add(MoveTemp(Finding));
				}
			}
		}
		else if (Mark.Kind == ECrowdyDeleteKind::Automation)
		{
			TArray<FString> Triggers;
			for (const TSharedPtr<FStudioAutomationTrigger>& Trigger : Evidence.AutomationTriggers)
			{
				if (Trigger.IsValid() && CrowdyModelSnapshotKeys::KeysMatch(Trigger->AutomationName, Mark.Name))
				{
					const FString Phrase = CrowdyModelVocabulary::OnEventLabel(*Trigger);
					Triggers.Add(Phrase.IsEmpty() ? Trigger->OnEvent : Phrase);
				}
			}

			if (Triggers.Num() > 0)
			{
				FCrowdyDeleteFinding Finding = CrowdyDeleteMakeFinding(
					ECrowdyDeleteFindingKind::AutomationCascadesItsTriggers, ECrowdyDeleteSeverity::Info, Mark,
					Triggers.Num() == 1
						? FString::Printf(TEXT("Deleting %s removes what it reacts to with it."), *Mark.Display)
						: FString::Printf(TEXT("Deleting %s removes the %d things it reacts to with it."),
							*Mark.Display, Triggers.Num()),
					FString());
				CrowdyDeleteSetReferences(Finding, Triggers);
				Findings.Add(MoveTemp(Finding));
			}
		}
		else if (Mark.Kind == ECrowdyDeleteKind::LiveInstance)
		{
			Findings.Add(CrowdyDeleteMakeFinding(
				ECrowdyDeleteFindingKind::LiveModelCascadesItsValuesAndEdges, ECrowdyDeleteSeverity::Info, Mark,
				FString::Printf(
					TEXT("Deleting %s removes the values stored on it and the links joining it to other live ")
					TEXT("models."), *Mark.Display),
				FString()));
		}

		if (CrowdyDeleteIsRestoredByAuthor(CrowdyDeleteProvenanceOf(Evidence, Mark)))
		{
			Findings.Add(CrowdyDeleteMakeFinding(
				ECrowdyDeleteFindingKind::RecreatedByTheNextSync, ECrowdyDeleteSeverity::Caution, Mark,
				FString::Printf(
					TEXT("Something outside this app declares %s, so it comes back the next time that is applied. ")
					TEXT("Deleting it here is not permanent."), *Mark.Display),
				TEXT("Remove it from whatever declares it first, or this delete undoes itself.")));
		}
	}

	// Worst first, so the reader's eye lands on what stops the commit before what merely accompanies it, then a
	// fixed order within a severity so the same marked set always produces the same list.
	Findings.Sort([](const FCrowdyDeleteFinding& A, const FCrowdyDeleteFinding& B)
	{
		if (A.Severity != B.Severity)
		{
			return static_cast<uint8>(A.Severity) > static_cast<uint8>(B.Severity);
		}
		if (A.Kind != B.Kind)
		{
			return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
		}
		return A.Subject.IdentityKey().Compare(B.Subject.IdentityKey(), ESearchCase::CaseSensitive) < 0;
	});

	return Findings;
}

TArray<FCrowdyDeleteOp> CrowdyGameModelDelete::BuildOps(
	const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence)
{
	TArray<FCrowdyDeleteOp> Ops;

	// Two marks can resolve to one call on ONE entity (a marked function and the same function implied by its
	// model), and issuing that twice would report the second as already gone for no reason. Two marks naming the
	// same function on two DIFFERENT models are two entities, and folding them would decide the unsettled question
	// of whether the mutation is app-wide in the direction that loses one of them silently. Under the app-wide
	// reading the second call answers that there was nothing there, which this walk already counts as a success, so
	// keeping both costs one round trip and keeps every marked row accounted for.
	auto AddOp = [&Ops](FCrowdyDeleteOp&& Op)
	{
		for (const FCrowdyDeleteOp& Existing : Ops)
		{
			if (CrowdyDeleteOpsAreTheSameCall(Existing, Op))
			{
				return;
			}
		}
		Ops.Add(MoveTemp(Op));
	};

	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		if (!CrowdyDeleteMarkIsActionable(Mark) || IsSubsumed(Mark, Marks))
		{
			continue;
		}

		FCrowdyDeleteOp Op;
		Op.Kind = Mark.Kind;
		Op.Subject = Mark;

		switch (Mark.Kind)
		{
		case ECrowdyDeleteKind::Automation:
			Op.OperationName = TEXT("GameModelDeleteAutomation");
			Op.ResultField = TEXT("gameModelDeleteAutomation");
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("name"), Mark.Name));
			Op.Describe = FString::Printf(TEXT("the automation %s"), *Mark.Display);
			break;

		case ECrowdyDeleteKind::LiveInstance:
			Op.OperationName = TEXT("GameModelDeleteContainer");
			Op.ResultField = TEXT("gameModelDeleteContainer");
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("containerId"), Mark.Name));
			Op.Describe = Mark.OwningType.IsEmpty()
				? FString::Printf(TEXT("the live model %s"), *Mark.Display)
				: FString::Printf(TEXT("the live model %s of %s"), *Mark.Display, *Mark.OwningType);
			break;

		case ECrowdyDeleteKind::Attribute:
			Op.OperationName = TEXT("GameModelDeletePropertyDef");
			Op.ResultField = TEXT("gameModelDeletePropertyDef");
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("containerTypeName"), Mark.OwningType));
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("key"), Mark.Name));
			Op.Describe = FString::Printf(TEXT("the attribute %s on %s"), *Mark.Display, *Mark.OwningType);
			break;

		case ECrowdyDeleteKind::Function:
			Op.OperationName = TEXT("GameModelDeleteFunction");
			Op.ResultField = TEXT("gameModelDeleteFunction");
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("name"), Mark.Name));
			Op.bNameNotUniqueInApp = ScopesCarryingFunctionName(Evidence, Mark.Name).Num() >= 2;
			// The mutation takes the name alone. Where that name is on more than one model, naming one here would
			// promise a scoping the operation may not have, so the line says only what is certain.
			Op.Describe = (Op.bNameNotUniqueInApp || Mark.OwningType.IsEmpty())
				? FString::Printf(TEXT("the function %s"), *Mark.Display)
				: FString::Printf(TEXT("the function %s on %s"), *Mark.Display, *Mark.OwningType);
			break;

		case ECrowdyDeleteKind::Model:
		default:
			Op.OperationName = TEXT("GameModelDeleteContainerType");
			Op.ResultField = TEXT("gameModelDeleteContainerType");
			Op.StringArgs.Add(TPair<FString, FString>(TEXT("typeName"), Mark.Name));
			Op.Describe = FString::Printf(TEXT("the model %s"), *Mark.Display);
			break;
		}

		AddOp(MoveTemp(Op));

		if (Mark.Kind != ECrowdyDeleteKind::Model)
		{
			continue;
		}

		// The SDK provisions a revision attribute and a touch function on every model. Neither is ever a row, so
		// nobody can mark one, and the touch function is a bound function the server would refuse the model delete
		// over. The names come from what the server actually holds rather than from a naming rule, so this never
		// issues a delete for something that is not there. The attribute goes with the model and needs no
		// operation of its own.
		for (const FString& Plumbing : PlumbingFunctionsFor(Mark.Name, Evidence))
		{
			FCrowdyDeleteOp Implied;
			Implied.Kind = ECrowdyDeleteKind::Function;
			Implied.Subject = Mark;
			Implied.OperationName = TEXT("GameModelDeleteFunction");
			Implied.ResultField = TEXT("gameModelDeleteFunction");
			Implied.StringArgs.Add(TPair<FString, FString>(TEXT("name"), Plumbing));
			Implied.bImplied = true;
			// The wiring goes out as a bare name like everything else, so it carries the same ambiguity. Nobody
			// marked it, which is exactly why it has to be flagged here rather than left to the reader to notice.
			Implied.bNameNotUniqueInApp = ScopesCarryingFunctionName(Evidence, Plumbing).Num() >= 2;
			Implied.Describe = FString::Printf(
				TEXT("the wiring the SDK keeps for the model %s"), *Mark.Display);
			AddOp(MoveTemp(Implied));
		}
	}

	// Commit order between kinds; within a kind, owning model then name, so the same marked set produces the same
	// list whatever order the boxes were ticked in. That is what makes the consent token stable, and the last two
	// keys make the order total so two operations can never swap places between two builds of one plan.
	Ops.Sort([](const FCrowdyDeleteOp& A, const FCrowdyDeleteOp& B)
	{
		const int32 KindA = CrowdyDeleteCommitPosition(A.Kind);
		const int32 KindB = CrowdyDeleteCommitPosition(B.Kind);
		if (KindA != KindB)
		{
			return KindA < KindB;
		}

		const int32 ByModel = A.Subject.OwningModelName().Compare(
			B.Subject.OwningModelName(), ESearchCase::CaseSensitive);
		if (ByModel != 0)
		{
			return ByModel < 0;
		}

		const int32 ByName = A.Subject.Name.Compare(B.Subject.Name, ESearchCase::CaseSensitive);
		if (ByName != 0)
		{
			return ByName < 0;
		}

		if (A.bImplied != B.bImplied)
		{
			return !A.bImplied;
		}

		const FString ArgsA = A.StringArgs.Num() > 0 ? A.StringArgs.Last().Value : FString();
		const FString ArgsB = B.StringArgs.Num() > 0 ? B.StringArgs.Last().Value : FString();
		return ArgsA.Compare(ArgsB, ESearchCase::CaseSensitive) < 0;
	});

	return Ops;
}

FCrowdyDeleteSheet CrowdyGameModelDelete::BuildSheet(
	int64 AppId, const TArray<FCrowdyDeleteOp>& Ops, const TArray<FCrowdyDeleteMark>& SubsumedMarks,
	const TArray<FCrowdyDeleteFinding>& Findings, ECrowdyDeleteLadder Ladder)
{
	FCrowdyDeleteSheet Sheet;

	Sheet.AppLine = AppId == 0
		? FString(TEXT("No app is selected."))
		: FString::Printf(TEXT("Writes to app %lld. There is no undo."), AppId);

	// The counts on the sheet are the entries the reader ticked. What the plan added for itself is disclosed on
	// its own line rather than folded into these, so a number here always matches a row that was marked.
	int32 VisibleTotal = 0;
	int32 ImpliedTotal = 0;
	TArray<FString> HeadlineParts;
	for (const FCrowdyDeleteOp& Op : Ops)
	{
		if (Op.bImplied)
		{
			++ImpliedTotal;
		}
		else
		{
			++VisibleTotal;
		}
	}

	auto CountVisible = [&Ops](ECrowdyDeleteKind Kind)
	{
		int32 Count = 0;
		for (const FCrowdyDeleteOp& Op : Ops)
		{
			if (Op.Kind == Kind && !Op.bImplied)
			{
				++Count;
			}
		}
		return Count;
	};

	for (const ECrowdyDeleteKind Kind : CrowdyDeleteReadingOrder())
	{
		const int32 Count = CountVisible(Kind);
		if (Count > 0)
		{
			HeadlineParts.Add(CrowdyDeleteCountPhrase(Count, Kind));
		}
	}

	for (const ECrowdyDeleteKind Kind : CommitOrder())
	{
		const int32 Count = CountVisible(Kind);
		if (Count > 0)
		{
			Sheet.CountLines.Add(CrowdyDeleteCountPhrase(Count, Kind));
		}
	}

	if (Ops.Num() == 0)
	{
		Sheet.Headline = TEXT("Nothing to delete.");
	}
	else if (HeadlineParts.Num() == 0)
	{
		// Every operation was one the plan added for itself. Nothing was ticked to name, so the count is all
		// there is to say, and saying it is better than a sentence with a hole in it.
		Sheet.Headline = FString::Printf(TEXT("Delete %s?"), *CrowdyDeleteEntryPhrase(Ops.Num()));
	}
	else
	{
		Sheet.Headline = FString::Printf(TEXT("Delete %s?"), *CrowdyDeleteJoinPhrases(HeadlineParts));
	}

	if (ImpliedTotal > 0)
	{
		Sheet.ImpliedLine = FString::Printf(
			TEXT("This also deletes %s of wiring the SDK keeps for the models above. It is never shown as a row, ")
			TEXT("and the server refuses to delete a model while it is there."),
			*CrowdyDeleteEntryPhrase(ImpliedTotal));
	}

	if (SubsumedMarks.Num() > 0)
	{
		Sheet.SubsumedLine = FString::Printf(
			TEXT("%s you marked go with the model they belong to, so they are not counted separately."),
			*CrowdyDeleteEntryPhrase(SubsumedMarks.Num()));
	}

	// The button always names its count, so the action and the number it acts on cannot be read apart.
	if (VisibleTotal <= 0)
	{
		Sheet.ActionLabel = Ops.Num() > 0
			? FString::Printf(TEXT("Delete all %d"), Ops.Num())
			: FString(TEXT("Delete"));
	}
	else if (HeadlineParts.Num() == 1)
	{
		Sheet.ActionLabel = FString::Printf(TEXT("Delete %s"), *HeadlineParts[0]);
	}
	else
	{
		Sheet.ActionLabel = FString::Printf(TEXT("Delete %s"), *CrowdyDeleteEntryPhrase(VisibleTotal));
	}

	int32 CautionCount = 0;
	TArray<const FCrowdyDeleteFinding*> Blockers;
	for (const FCrowdyDeleteFinding& Finding : Findings)
	{
		if (Finding.Severity == ECrowdyDeleteSeverity::Blocker)
		{
			Blockers.Add(&Finding);
		}
		else if (Finding.Severity == ECrowdyDeleteSeverity::Caution)
		{
			++CautionCount;
		}
	}

	if (Ladder == ECrowdyDeleteLadder::Acknowledge)
	{
		Sheet.AcknowledgeLabel = CautionCount == 1
			? FString(TEXT("I have read the caution above and want to delete anyway."))
			: FString::Printf(
				TEXT("I have read the %d cautions above and want to delete anyway."), CautionCount);
	}

	if (Ladder == ECrowdyDeleteLadder::Empty)
	{
		Sheet.BlockedReason = TEXT("Nothing marked here has anything on the server to delete.");
	}
	else if (Ladder == ECrowdyDeleteLadder::Blocked && Blockers.Num() > 0)
	{
		const FCrowdyDeleteFinding& First = *Blockers[0];
		const FString FirstLine = First.Remedy.IsEmpty()
			? First.Headline
			: First.Headline + TEXT(" ") + First.Remedy;
		Sheet.BlockedReason = Blockers.Num() == 1
			? FirstLine
			: FString::Printf(
				TEXT("%d things have to change before this can run. %s"), Blockers.Num(), *FirstLine);
	}
	else if (Ladder == ECrowdyDeleteLadder::Blocked)
	{
		// Blocked with no blocker finding is the evidence case: the caller replaces this with what is missing.
		Sheet.BlockedReason = TEXT("This delete cannot run yet.");
	}

	return Sheet;
}

ECrowdyDeleteSeverity CrowdyGameModelDelete::WorstSeverity(const TArray<FCrowdyDeleteFinding>& Findings)
{
	ECrowdyDeleteSeverity Worst = ECrowdyDeleteSeverity::Info;
	for (const FCrowdyDeleteFinding& Finding : Findings)
	{
		if (static_cast<uint8>(Finding.Severity) > static_cast<uint8>(Worst))
		{
			Worst = Finding.Severity;
		}
	}
	return Worst;
}

ECrowdyDeleteLadder CrowdyGameModelDelete::LadderFor(const TArray<FCrowdyDeleteFinding>& Findings, int32 OpCount)
{
	if (OpCount <= 0)
	{
		// Nothing to consent to, whatever the findings say.
		return ECrowdyDeleteLadder::Empty;
	}

	switch (WorstSeverity(Findings))
	{
	case ECrowdyDeleteSeverity::Blocker: return ECrowdyDeleteLadder::Blocked;
	case ECrowdyDeleteSeverity::Caution: return ECrowdyDeleteLadder::Acknowledge;
	default:                             return ECrowdyDeleteLadder::Confirm;
	}
}

FCrowdyDeletePlan CrowdyGameModelDelete::BuildPlan(
	const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence)
{
	FCrowdyDeletePlan Plan;
	Plan.AppId = Evidence.AppId;
	Plan.Marks = Marks;

	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		if (IsSubsumed(Mark, Marks))
		{
			Plan.SubsumedMarks.Add(Mark);
		}
	}

	Plan.Findings = BuildFindings(Marks, Evidence);
	Plan.Ops = BuildOps(Marks, Evidence);
	Plan.Ladder = LadderFor(Plan.Findings, Plan.Ops.Num());

	FString EvidenceReason;
	const bool bEvidenceOk = IsEvidenceSufficient(Evidence, EvidenceReason);
	if (!bEvidenceOk && Plan.Ops.Num() > 0)
	{
		// Judged against lists nobody read, so the sheet would render a clean branch it has no grounds for.
		Plan.Ladder = ECrowdyDeleteLadder::Blocked;
	}

	Plan.Sheet = BuildSheet(Plan.AppId, Plan.Ops, Plan.SubsumedMarks, Plan.Findings, Plan.Ladder);
	if (!bEvidenceOk)
	{
		Plan.Sheet.BlockedReason = EvidenceReason;
	}

	Plan.bCommittable = bEvidenceOk
		&& Plan.Ops.Num() > 0
		&& Plan.Ladder != ECrowdyDeleteLadder::Blocked
		&& Plan.Ladder != ECrowdyDeleteLadder::Empty
		&& WorstSeverity(Plan.Findings) != ECrowdyDeleteSeverity::Blocker;

	Plan.ConsentToken = MakeConsentToken(Plan.AppId, Plan.Ops, Plan.Ladder);
	return Plan;
}

FString CrowdyGameModelDelete::MakeConsentToken(
	int64 AppId, const TArray<FCrowdyDeleteOp>& Ops, ECrowdyDeleteLadder Ladder)
{
	// Every argument of every operation, in order, so one argument changing while the count stays the same still
	// changes the token. The ladder is in it because consent given to a one-line confirm is not consent to a
	// sheet that has since grown a caution.
	FString Canonical = FString::Printf(
		TEXT("app=%lld\nladder=%d\ncount=%d\n"), AppId, static_cast<int32>(Ladder), Ops.Num());

	for (const FCrowdyDeleteOp& Op : Ops)
	{
		Canonical += FString::Printf(TEXT("%d|%s|%s|%d"),
			static_cast<int32>(Op.Kind), *Op.OperationName, *Op.ResultField, Op.bImplied ? 1 : 0);
		for (const TPair<FString, FString>& Arg : Op.StringArgs)
		{
			Canonical += TEXT("|") + Arg.Key + TEXT("=") + Arg.Value;
		}
		Canonical += TEXT("\n");
	}

	return CrowdyDeleteDigest(Canonical);
}

ECrowdyDeleteReply CrowdyGameModelDelete::ReadDeleteReply(
	const TSharedPtr<FJsonObject>& Envelope, const FString& ResultField)
{
	if (!Envelope.IsValid() || ResultField.IsEmpty())
	{
		return ECrowdyDeleteReply::Unrecognized;
	}

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("data"), Data) || !Data || !Data->IsValid())
	{
		return ECrowdyDeleteReply::Unrecognized;
	}

	bool bDeleted = false;
	if (!(*Data)->TryGetBoolField(ResultField, bDeleted))
	{
		// A clean envelope of an unexpected shape says nothing about whether the entity survived. Refusals arrive
		// on the failure path, so nothing legitimate lands here.
		return ECrowdyDeleteReply::Unrecognized;
	}

	return bDeleted ? ECrowdyDeleteReply::Removed : ECrowdyDeleteReply::AlreadyGone;
}

bool CrowdyGameModelDelete::IsDeleteReplySuccess(ECrowdyDeleteReply Reply)
{
	// AlreadyGone is a success. An entity that was not there needed no work, and a walk that read this as a
	// failure would stop on the first operation a previous attempt had already finished, so a second press after
	// a partial commit could never finish the job.
	return Reply == ECrowdyDeleteReply::Removed || Reply == ECrowdyDeleteReply::AlreadyGone;
}

TArray<FCrowdyDeleteOp> CrowdyGameModelDelete::Remainder(const TArray<FCrowdyDeleteOp>& Ops, int32 StoppedAtIndex)
{
	TArray<FCrowdyDeleteOp> Rest;
	if (StoppedAtIndex == INDEX_NONE || StoppedAtIndex < 0 || StoppedAtIndex >= Ops.Num())
	{
		return Rest;
	}

	// Starting AT the stopped index: that operation did not complete, so a second press has to run it again.
	Rest.Reserve(Ops.Num() - StoppedAtIndex);
	for (int32 Index = StoppedAtIndex; Index < Ops.Num(); ++Index)
	{
		Rest.Add(Ops[Index]);
	}
	return Rest;
}

bool CrowdyGameModelDelete::RemainderAppliesTo(
	const TArray<FCrowdyDeleteOp>& Remainder, const TArray<FCrowdyDeleteOp>& PlanOps)
{
	if (Remainder.Num() == 0)
	{
		return false; // nothing left over, so there is no sentence to show
	}

	for (const FCrowdyDeleteOp& Left : Remainder)
	{
		const bool bStillPlanned = PlanOps.ContainsByPredicate([&Left](const FCrowdyDeleteOp& Planned)
		{
			return CrowdyDeleteOpsAreTheSameCall(Planned, Left);
		});
		if (!bStillPlanned)
		{
			// The marked set has moved on. "Press Delete again to finish the remaining 3" would then be printed
			// above a button that runs something else, which is worse than saying nothing.
			return false;
		}
	}
	return true;
}

FString CrowdyGameModelDelete::StopText(const FCrowdyDeleteOutcome& Outcome)
{
	if (!Outcome.bStopped)
	{
		return FString();
	}

	const int32 Remaining = FMath::Max(0, Outcome.Total - Outcome.Completed);

	if (Outcome.bStoppedByCancel)
	{
		// A cancellation is this editor rebuilding its own connection, not the server refusing anything. Saying
		// otherwise sends the reader hunting for a blocker that was never there.
		return FString::Printf(
			TEXT("Stopped after %d of %d, because this editor rebuilt its connection. The server refused ")
			TEXT("nothing. What was deleted is gone. Press Delete again to finish the remaining %d."),
			Outcome.Completed, Outcome.Total, Remaining);
	}

	if (Outcome.StoppedOnDescription.IsEmpty())
	{
		return FString::Printf(
			TEXT("Stopped after %d of %d. Everything before it is deleted and re-running it does nothing, so ")
			TEXT("press Delete again to finish the remaining %d rather than starting over."),
			Outcome.Completed, Outcome.Total, Remaining);
	}

	return FString::Printf(
		TEXT("Stopped on %s, after %d of %d. Everything before it is deleted and re-running it does nothing, so ")
		TEXT("press Delete again to finish the remaining %d rather than starting over."),
		*Outcome.StoppedOnDescription, Outcome.Completed, Outcome.Total, Remaining);
}

FString CrowdyGameModelDelete::CompletionText(const FCrowdyDeleteOutcome& Outcome)
{
	if (Outcome.Total <= 0)
	{
		return TEXT("Nothing to delete.");
	}

	// Completed counts the ones that were already gone, so the number actually deleted is the difference. "16
	// deleted, 2 were already gone" and "18 deleted" are different facts and the second one would be untrue.
	const int32 Deleted = FMath::Max(0, Outcome.Completed - Outcome.AlreadyGone);
	if (Outcome.AlreadyGone <= 0)
	{
		return FString::Printf(TEXT("Deleted %s."), *CrowdyDeleteEntryPhrase(Deleted));
	}

	return FString::Printf(TEXT("Deleted %s, and %d %s already gone."),
		*CrowdyDeleteEntryPhrase(Deleted), Outcome.AlreadyGone,
		Outcome.AlreadyGone == 1 ? TEXT("was") : TEXT("were"));
}

TArray<FCrowdyDeleteMark> CrowdyGameModelDelete::MarkEverythingServerOnly(const FCrowdyDeleteEvidence& Evidence)
{
	TArray<FCrowdyDeleteMark> Marks;

	const FCrowdyModelSnapshot* Snapshot = Evidence.Snapshot.Get();
	if (!Snapshot)
	{
		// Server-only is a classification, and without a plan there is none. An empty result is the honest answer.
		return Marks;
	}

	TArray<FCrowdyDeleteMark> Models;
	TArray<FCrowdyDeleteMark> Attributes;
	TArray<FCrowdyDeleteMark> Functions;
	TArray<FCrowdyDeleteMark> Automations;

	for (const TSharedPtr<FStudioContainerType>& Type : Evidence.Types)
	{
		if (!Type.IsValid() || Type->TypeName.IsEmpty())
		{
			continue;
		}
		if (CrowdyModelLedger::ClassifyModel(Snapshot, Type->TypeName).Provenance
			!= ECrowdyModelProvenance::ServerOnly)
		{
			continue;
		}
		const FString Display = Type->DisplayName.TrimStartAndEnd();
		Models.Add(MarkModel(Type->TypeName, Display.IsEmpty() ? Type->TypeName : Display));
	}

	for (const FCrowdyDeleteModelAttributes& Entry : Evidence.AttributesByModel)
	{
		for (const TSharedPtr<FStudioPropertyDef>& Def : Entry.Attributes)
		{
			if (!Def.IsValid() || Def->Key.IsEmpty() || CrowdyModelLedger::IsReservedAttribute(Def->Key))
			{
				continue;
			}

			const FString Owner = Def->ContainerTypeName.IsEmpty() ? Entry.TypeName : Def->ContainerTypeName;
			if (Owner.IsEmpty())
			{
				continue; // no model to resolve the key in, so nothing here can name what would be deleted
			}
			if (CrowdyModelLedger::ClassifyAttribute(Snapshot, Owner, Def->Key).Provenance
				!= ECrowdyModelProvenance::ServerOnly)
			{
				continue;
			}
			// Server-only by the classification just applied, so no class in the project declares it and there is no
			// authored spelling to use. The key is reconstructed into a name instead, which is what the row for the
			// same attribute shows, so the marked list and the row it came from read alike.
			Attributes.Add(
				MarkAttribute(Owner, Def->Key, CrowdyModelVocabulary::AttributeDisplayNameFromKey(Def->Key)));
		}
	}

	for (const TSharedPtr<FStudioFunction>& Function : Evidence.Functions)
	{
		if (!Function.IsValid() || Function->Name.IsEmpty()
			|| CrowdyModelLedger::IsReservedFunction(Function->Name))
		{
			continue;
		}
		if (Function->ContainerTypeName.IsEmpty())
		{
			// A function whose model was never determined. Offering it here would answer the missing scope by
			// assuming nobody owns it, which is the assumption that deletes somebody's work.
			continue;
		}
		if (CrowdyModelLedger::ClassifyFunction(Snapshot, Function->ContainerTypeName, Function->Name).Provenance
			!= ECrowdyModelProvenance::ServerOnly)
		{
			continue;
		}
		Functions.Add(MarkFunction(Function->ContainerTypeName, Function->Name, Function->Name));
	}

	for (const TSharedPtr<FStudioAutomation>& Automation : Evidence.Automations)
	{
		if (!Automation.IsValid() || Automation->Name.IsEmpty())
		{
			continue;
		}
		if (CrowdyModelLedger::ClassifyAutomation(Snapshot, Automation->Name).Provenance
			!= ECrowdyModelProvenance::ServerOnly)
		{
			continue;
		}
		Automations.Add(MarkAutomation(Automation->Name, Automation->Name));
	}

	auto SortMarks = [](TArray<FCrowdyDeleteMark>& ToSort)
	{
		ToSort.Sort([](const FCrowdyDeleteMark& A, const FCrowdyDeleteMark& B)
		{
			return A.IdentityKey().Compare(B.IdentityKey(), ESearchCase::CaseSensitive) < 0;
		});
	};
	SortMarks(Models);
	SortMarks(Attributes);
	SortMarks(Functions);
	SortMarks(Automations);

	Marks.Append(Models);
	Marks.Append(Attributes);
	Marks.Append(Functions);
	Marks.Append(Automations);
	return Marks;
}

bool CrowdyGameModelDelete::CanMarkEverythingServerOnly(const FCrowdyDeleteEvidence& Evidence, FString& OutReason)
{
	OutReason.Reset();

	if (!Evidence.Snapshot.IsValid())
	{
		OutReason = TEXT("Nothing has checked this app against the project yet, so nothing here knows what is ")
			TEXT("server-only. Press Preview changes first.");
		return false;
	}

	if (!IsEvidenceSufficient(Evidence, OutReason))
	{
		// A bulk mark over half-read lists would mark less than everything while claiming to mark everything.
		return false;
	}

	const FCrowdyModelSnapshot& Snapshot = *Evidence.Snapshot;
	if (Snapshot.RecognizedKitTypePrefixes.Num() > 0
		|| Snapshot.RecognizedKitTypeNames.Num() > 0
		|| Snapshot.RecognizedKitFunctionNames.Num() > 0
		|| Snapshot.RecognizedKitAutomationNames.Num() > 0)
	{
		OutReason = TEXT("A Game Kit has been deployed to this app. Only the kit's functions are protected from ")
			TEXT("a bulk mark today, so this would offer the kit's own models and attributes for deletion. Mark ")
			TEXT("them one at a time instead.");
		return false;
	}

	return true;
}

bool CrowdyGameModelDelete::NamesIdentifier(const FString& Text, const FString& Identifier)
{
	if (Text.IsEmpty() || Identifier.IsEmpty())
	{
		return false;
	}

	int32 SearchFrom = 0;
	while (SearchFrom <= Text.Len() - Identifier.Len())
	{
		const int32 Found = Text.Find(Identifier, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (Found == INDEX_NONE)
		{
			return false;
		}

		const int32 After = Found + Identifier.Len();
		const bool bStartsWord = Found == 0 || !CrowdyDeleteIsIdentifierChar(Text[Found - 1]);
		const bool bEndsWord = After >= Text.Len() || !CrowdyDeleteIsIdentifierChar(Text[After]);
		if (bStartsWord && bEndsWord)
		{
			return true;
		}

		SearchFrom = Found + 1;
	}

	return false;
}

bool CrowdyGameModelDelete::FunctionReferencesAttribute(
	const FStudioFunction& Function, const FString& /*OwningType*/, const FString& Key)
{
	// The model deliberately narrows nothing. An expression reaches another model's attribute through a link, so
	// a function bound elsewhere can still name this key, and matching only functions bound to the model would
	// miss exactly those. Naming a function that turns out to write a same-named key on another model costs the
	// reader one line of a disclosure; missing one costs them a function that stops working.
	if (Key.IsEmpty())
	{
		return false;
	}

	for (const FStudioFunctionMutation& Mutation : Function.Mutations)
	{
		if (CrowdyModelSnapshotKeys::KeysMatch(Mutation.Property, Key)
			|| NamesIdentifier(Mutation.Target, Key)
			|| NamesIdentifier(Mutation.Expression, Key))
		{
			return true;
		}
	}

	if (NamesIdentifier(Function.ReturnExpression, Key))
	{
		return true;
	}

	// The authority gate. It is stored as JSON and one of its rule kinds is a model expression, so the key can sit
	// inside it and nothing else in this scan would ever see it. Searched as text: a rule that happens to carry the
	// key as a value costs one line of a disclosure, while missing the gate costs a function that can no longer be
	// invoked by anyone.
	if (NamesIdentifier(Function.InvokePolicyJson, Key))
	{
		return true;
	}

	// What the function tells the client after it commits. Each argument is an expression over the state the
	// invocation just wrote.
	for (const FCrowdyGameModelNotification& Notification : Function.Notifications)
	{
		for (const FCrowdyGameModelNotificationArg& Arg : Notification.Args)
		{
			if (NamesIdentifier(Arg.Expression, Key))
			{
				return true;
			}
		}
	}

	// The delayed invocations it arms. The delay, the dedupe key and every bound parameter are evaluated when the
	// timer is armed, so all three read model state and all three stop resolving once the key has gone.
	for (const FCrowdyGameModelTimer& Timer : Function.Timers)
	{
		if (NamesIdentifier(Timer.DelayMsExpression, Key)
			|| NamesIdentifier(Timer.DedupeKeyExpression, Key)
			|| NamesIdentifier(Timer.Target, Key))
		{
			return true;
		}
		for (const FCrowdyGameModelTimerParam& Param : Timer.Params)
		{
			if (NamesIdentifier(Param.Expression, Key))
			{
				return true;
			}
		}
	}

	return false;
}

bool CrowdyGameModelDelete::AutomationReferencesAttribute(const FStudioAutomation& Automation, const FString& Key)
{
	if (Key.IsEmpty())
	{
		return false;
	}

	// Both fields are JSON the server reads as model expressions: the selector names keys to filter and rank by,
	// and the static parameters are merged into every call it makes. Searched as text for the same reason the
	// authority gate above is.
	return NamesIdentifier(Automation.SelectorJson, Key)
		|| NamesIdentifier(Automation.ParamsJson, Key);
}

bool CrowdyGameModelDelete::TriggerReferencesAttribute(
	const FStudioAutomationTrigger& Trigger, const FString& OwningType, const FString& Key)
{
	if (Key.IsEmpty() || !CrowdyModelSnapshotKeys::KeysMatch(Trigger.PropertyKey, Key))
	{
		return false;
	}

	// An empty model on the trigger is a scope nobody determined, not a scope that is somebody else's. Reading it
	// as "not this model" is how a trigger that really does wait on this key goes unreported, and the cost of
	// reading it the other way is one extra line in a disclosure.
	return Trigger.ContainerTypeName.IsEmpty()
		|| OwningType.IsEmpty()
		|| CrowdyModelSnapshotKeys::KeysMatch(Trigger.ContainerTypeName, OwningType);
}

bool CrowdyGameModelDelete::AutomationReferencesFunction(
	const FStudioAutomation& Automation, const FString& FunctionName)
{
	return !FunctionName.IsEmpty()
		&& CrowdyModelSnapshotKeys::KeysMatch(Automation.FunctionName, FunctionName);
}

bool CrowdyGameModelDelete::TriggerReferencesFunction(
	const FStudioAutomationTrigger& Trigger, const FString& FunctionName)
{
	// One field carries both readings: the function a trigger runs and, for an invocation event, the function it
	// reacts to. Either way the trigger stops resolving once that function is gone.
	return !FunctionName.IsEmpty()
		&& CrowdyModelSnapshotKeys::KeysMatch(Trigger.FunctionName, FunctionName);
}

bool CrowdyGameModelDelete::AutomationTargetsModel(const FStudioAutomation& Automation, const FString& TypeName)
{
	// An empty type name matches nothing. An app-wide automation names no target, and letting those two meet
	// would report every app-wide automation as targeting whichever model was asked about.
	return !TypeName.IsEmpty()
		&& CrowdyModelSnapshotKeys::KeysMatch(Automation.TargetTypeName, TypeName);
}

TArray<FString> CrowdyGameModelDelete::ModelsCarryingFunctionName(
	const FCrowdyDeleteEvidence& Evidence, const FString& FunctionName)
{
	TArray<FString> Models;
	if (FunctionName.IsEmpty())
	{
		return Models;
	}

	for (const TSharedPtr<FStudioFunction>& Function : Evidence.Functions)
	{
		if (!Function.IsValid()
			|| !CrowdyModelSnapshotKeys::KeysMatch(Function->Name, FunctionName)
			|| Function->ContainerTypeName.IsEmpty())
		{
			continue;
		}
		if (!CrowdyModelSnapshotKeys::Contains(Models, Function->ContainerTypeName))
		{
			Models.Add(Function->ContainerTypeName);
		}
	}

	CrowdyDeleteSortNames(Models);
	return Models;
}

TArray<FString> CrowdyGameModelDelete::ScopesCarryingFunctionName(
	const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& FunctionName)
{
	TArray<FString> Scopes;
	if (FunctionName.IsEmpty())
	{
		return Scopes;
	}

	bool bUndeterminedScope = false;
	for (const TSharedPtr<FStudioFunction>& Function : Functions)
	{
		if (!Function.IsValid() || !CrowdyModelSnapshotKeys::KeysMatch(Function->Name, FunctionName))
		{
			continue;
		}
		if (Function->ContainerTypeName.IsEmpty())
		{
			// Its own scope. A record with no binding and a read that never carried one are indistinguishable
			// here, and either way it is a second entity the bare-name delete may reach.
			bUndeterminedScope = true;
			continue;
		}
		CrowdyDeleteAddUniqueKey(Scopes, Function->ContainerTypeName);
	}

	CrowdyDeleteSortNames(Scopes);
	if (bUndeterminedScope)
	{
		// Last, and phrased rather than named, because there is no name to give. Leaving it out is what made the
		// count and the list disagree: the caution would fire and the disclosure would show only the model the
		// reader already knew about.
		Scopes.Add(TEXT("a copy whose model this app did not report"));
	}
	return Scopes;
}

TArray<FString> CrowdyGameModelDelete::ScopesCarryingFunctionName(
	const FCrowdyDeleteEvidence& Evidence, const FString& FunctionName)
{
	return ScopesCarryingFunctionName(Evidence.Functions, FunctionName);
}

int32 CrowdyGameModelDelete::LiveModelsBlockingModelDelete(
	const FString& TypeName, const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence)
{
	int32 Counted = 0;
	if (TypeName.IsEmpty() || Evidence.FindLiveCount(TypeName, Counted) == ECrowdyLiveCountState::Unknown)
	{
		return 0; // an unknown count is its own blocker, and inventing a number here would replace it with one
	}

	int32 Marked = 0;
	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		if (Mark.Kind == ECrowdyDeleteKind::LiveInstance
			&& !Mark.Name.IsEmpty()
			&& CrowdyModelSnapshotKeys::KeysMatch(Mark.OwningType, TypeName))
		{
			++Marked;
		}
	}

	return FMath::Max(0, Counted - Marked);
}

TArray<FString> CrowdyGameModelDelete::FunctionsBlockingModelDelete(
	const FString& TypeName, const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence)
{
	TArray<FString> Blocking;
	if (TypeName.IsEmpty())
	{
		return Blocking;
	}

	for (const TSharedPtr<FStudioFunction>& Function : Evidence.Functions)
	{
		if (!Function.IsValid()
			|| Function->Name.IsEmpty()
			|| !CrowdyModelSnapshotKeys::KeysMatch(Function->ContainerTypeName, TypeName))
		{
			continue;
		}
		if (CrowdyModelLedger::IsReservedFunction(Function->Name))
		{
			continue; // the plan deletes the SDK's own wiring itself, so it never stops the model delete
		}
		// Only a mark that names THIS model clears it. A function mark with no model is a mark whose scope was
		// never determined, and reading that as "this one" would clear a refusal nothing has actually cleared.
		if (ContainsMark(Marks, MarkFunction(TypeName, Function->Name, Function->Name)))
		{
			continue;
		}
		if (!CrowdyModelSnapshotKeys::Contains(Blocking, Function->Name))
		{
			Blocking.Add(Function->Name);
		}
	}

	CrowdyDeleteSortNames(Blocking);
	return Blocking;
}

TArray<FString> CrowdyGameModelDelete::PlumbingFunctionsFor(
	const FString& TypeName, const FCrowdyDeleteEvidence& Evidence)
{
	TArray<FString> Plumbing;
	if (TypeName.IsEmpty() || !Evidence.bFunctionsRead)
	{
		return Plumbing;
	}

	// Read from what the server answered with rather than composed from the naming rule, so a plan never issues a
	// delete for something that is not there.
	for (const TSharedPtr<FStudioFunction>& Function : Evidence.Functions)
	{
		if (!Function.IsValid()
			|| Function->Name.IsEmpty()
			|| !CrowdyModelSnapshotKeys::KeysMatch(Function->ContainerTypeName, TypeName)
			|| !CrowdyModelLedger::IsReservedFunction(Function->Name))
		{
			continue;
		}
		if (!CrowdyModelSnapshotKeys::Contains(Plumbing, Function->Name))
		{
			Plumbing.Add(Function->Name);
		}
	}

	CrowdyDeleteSortNames(Plumbing);
	return Plumbing;
}
