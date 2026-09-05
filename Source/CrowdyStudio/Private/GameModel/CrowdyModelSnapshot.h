// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdySchemaSync.h" // ScopedNameKey + the kit predicates + the desired-schema and delta structs
#include "Misc/DateTime.h"

/**
 * One timestamped capture of everything needed to say where a model, attribute, function or automation came from and
 * how it differs from the project. Taken when a schema plan finishes and retained per app, so the browser can answer
 * that question for every row without re-running a plan.
 *
 * PURE: plain members only. No Slate, no HTTP, no UObject, no world, and no pointer back into the plan it was taken
 * from, so it outlives that run and can be shared by reference across frames.
 *
 * IDENTITY. Every scoped lookup goes through FCrowdySchemaSync::ScopedNameKey, the same key the diff groups by, so
 * the snapshot and the diff can never disagree about which entity is which. An attribute key is scoped by its
 * container type because an attribute key is only unique within one model: "Health" on Hero and "Health" on Goblin
 * are two different server property definitions with two different verdicts, and keying on the bare key would hand
 * one of them the other's answer.
 *
 * CASE. A type name, an attribute key, a function name and an automation name are all server keys, and FString
 * comparison (and therefore TSet<FString> / TMap<FString>, which hash case-insensitively) folds case. Every key set
 * below is a TArray searched with an explicit case-sensitive compare. The RecognizedKit* sets are the deliberate
 * exception: they are handed to FCrowdySchemaSync's own kit predicates unchanged, so what counts as kit-owned here
 * and what the prune protects are decided by exactly the same code.
 *
 * WHAT IS DELIBERATELY ABSENT: the delta's server-only lists. Whether something is authored in code is answered from
 * the code side, by asking whether the entity is in the captured desired schema. It must never be answered by
 * inverting a server-only list, because those lists already have kit protection and skipped-author exclusions folded
 * into them: reading "not in ServerOnlyTypes" as "declared in code" misclassifies a kit-owned type and a type owned
 * by a non-compiling effect, and does so in opposite directions. Not carrying those lists makes that inversion
 * impossible to write.
 */

namespace CrowdyModelSnapshotKeys
{
	// Every function here is inline and this is their only home. The unity build merges this module's .cpp files into
	// fewer translation units, so a second definition of any of these in a .cpp would redefine this one.

	// Two server keys name the same entity only when they match exactly. FString::Equals defaults to case-insensitive,
	// which would fold two distinct server entities into one.
	inline bool KeysMatch(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	inline bool Contains(const TArray<FString>& Keys, const FString& Key)
	{
		for (const FString& Candidate : Keys)
		{
			if (KeysMatch(Candidate, Key))
			{
				return true;
			}
		}
		return false;
	}

	inline void AddUnique(TArray<FString>& Keys, const FString& Key)
	{
		if (!Key.IsEmpty() && !Contains(Keys, Key))
		{
			Keys.Add(Key);
		}
	}
}

/** One attribute a container class declares, as the plan reflected it. */
struct FCrowdyModelSnapshotAttribute
{
	FString Key;
	FString ValueType;

	// The UPROPERTY spelling the class declares, e.g. "CropStage" for the key "crop_stage". Carried so a row can
	// be shown under the name it was written with; Key stays the identity, and nothing compares this.
	FString AuthoredName;
};

/** One container type the code declares, and the class that declares it. */
struct FCrowdyModelSnapshotType
{
	FString TypeName;
	FString DisplayName;

	// The declaring class as an FSoftClassPath string, so a reader can open the Blueprint or find the C++ class.
	// Empty for a type no class declares.
	FString OwningClassPath;

	TArray<FCrowdyModelSnapshotAttribute> Attributes;

	const FCrowdyModelSnapshotAttribute* FindAttribute(const FString& Key) const
	{
		for (const FCrowdyModelSnapshotAttribute& Attribute : Attributes)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Attribute.Key, Key))
			{
				return &Attribute;
			}
		}
		return nullptr;
	}
};

/** One function the code declares, scoped by the container type the server resolves it in. */
struct FCrowdyModelSnapshotFunction
{
	FString TypeName;
	FString Name;
	FString ReturnType;
	FString Description;

	// The effect asset that authored it.
	FString AssetPath;
};

/** One automation the code declares. An automation name is unique app-wide, so it carries no scope. */
struct FCrowdyModelSnapshotAutomation
{
	FString Name;
	FString Description;
	FString TargetTypeName;
	FString AssetPath;

	// Enough of the schedule and the event trigger to phrase when it runs, so a row for an automation that is not on
	// the server yet reads exactly as it will once it is.
	FString TriggerType;
	FString ScheduleKind;
	FString CronExpr;
	int32 IntervalMs = 0;
	FString OnEvent;
	FString OnEventPropertyKey;
	FString OnEventFunctionName;
};

/**
 * Which effect asset claims one function or automation name, and whether the plan could act on it. Carried so a row
 * that never reached the server can still name where it came from.
 */
struct FCrowdyModelSnapshotAuthor
{
	FString Scope; // the container type for a function; empty for an automation
	FString Name;
	FString AssetPath;
};

struct FCrowdyModelSnapshot
{
	int64 AppId = 0;
	FDateTime CapturedAt;

	// The code side: what the project declares, independent of what the server holds.
	TArray<FCrowdyModelSnapshotType> Types;
	TArray<FCrowdyModelSnapshotFunction> Functions;
	TArray<FCrowdyModelSnapshotAutomation> Automations;

	// The changes the plan would make, split so "this exists on the server and code changed it" can be told from
	// "this does not exist on the server yet". Types are keyed by their bare name; everything else by ScopedNameKey.
	TArray<FString> TypeCreates;
	TArray<FString> TypeUpdates;
	TArray<FString> AttributeCreates;
	TArray<FString> AttributeUpdates;
	TArray<FString> FunctionCreates;
	TArray<FString> FunctionUpdates;
	TArray<FString> AutomationCreates;
	TArray<FString> AutomationUpdates;

	// Names claimed by an effect the plan passed over (unmigrated, would not compile, or compiled to an unusable
	// name). Nothing can be said about how the server's copy compares to code, because the code side was never
	// produced. Anything the plan did produce is excluded, so a name a second, working effect also authors is not
	// dragged down to uncheckable by its broken sibling.
	TArray<FString> UncheckableFunctionKeys;
	TArray<FString> UncheckableAutomationKeys;

	// Names two or more effect assets claim. The plan syncs NEITHER author, so the name needs a decision from a human
	// before anything about it can move.
	TArray<FString> NeedsAFixFunctionKeys;
	TArray<FString> NeedsAFixAutomationKeys;

	TArray<FCrowdyModelSnapshotAuthor> FunctionAuthors;
	TArray<FCrowdyModelSnapshotAuthor> AutomationAuthors;

	// Handed to FCrowdySchemaSync's kit predicates verbatim: what this page calls kit-owned and what the prune
	// declines to offer are then the same answer, not two copies of one rule that can drift apart.
	TSet<FString> RecognizedKitTypePrefixes;
	TSet<FString> RecognizedKitTypeNames;
	TSet<FString> RecognizedKitFunctionNames;
	TSet<FString> RecognizedKitAutomationNames;

	const FCrowdyModelSnapshotType* FindType(const FString& TypeName) const
	{
		for (const FCrowdyModelSnapshotType& Type : Types)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Type.TypeName, TypeName))
			{
				return &Type;
			}
		}
		return nullptr;
	}

	const FCrowdyModelSnapshotFunction* FindFunction(const FString& TypeName, const FString& Name) const
	{
		for (const FCrowdyModelSnapshotFunction& Function : Functions)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Function.TypeName, TypeName)
				&& CrowdyModelSnapshotKeys::KeysMatch(Function.Name, Name))
			{
				return &Function;
			}
		}
		return nullptr;
	}

	// The one function of this name the code declares, whatever model it declares it on, or null when none does or
	// when two or more do. A single answer is the only usable one: with several, nothing here can say which of them a
	// given server function corresponds to, which is the same reason the diff's own rebind fallback requires it.
	const FCrowdyModelSnapshotFunction* FindOnlyFunctionNamed(const FString& Name) const
	{
		const FCrowdyModelSnapshotFunction* Found = nullptr;
		for (const FCrowdyModelSnapshotFunction& Function : Functions)
		{
			if (!CrowdyModelSnapshotKeys::KeysMatch(Function.Name, Name))
			{
				continue;
			}
			if (Found)
			{
				return nullptr;
			}
			Found = &Function;
		}
		return Found;
	}

	const FCrowdyModelSnapshotAutomation* FindAutomation(const FString& Name) const
	{
		for (const FCrowdyModelSnapshotAutomation& Automation : Automations)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(Automation.Name, Name))
			{
				return &Automation;
			}
		}
		return nullptr;
	}

	// Every attribute this project declares, flattened across every type into one server-key-to-authored-name map.
	// Flat rather than scoped by type because a cross-link line reaches some attribute keys with no owning model in
	// hand at all (a function's write through a reference, an automation's fired-by attribute), so there is no type
	// to scope the lookup by at the call site. If two types declare the same key under two different authored
	// names, the flattening keeps whichever this walk visits last; that is a pre-existing ambiguity in a bare-key
	// lookup, not one this map introduces. A key with no authored name is left out, so a caller's Find tracks
	// whether the project declares that attribute rather than always finding an entry.
	TMap<FString, FString> AttributeAuthoredNames() const
	{
		TMap<FString, FString> Names;
		for (const FCrowdyModelSnapshotType& Type : Types)
		{
			for (const FCrowdyModelSnapshotAttribute& Attribute : Type.Attributes)
			{
				if (!Attribute.AuthoredName.IsEmpty())
				{
					Names.Add(Attribute.Key, Attribute.AuthoredName);
				}
			}
		}
		return Names;
	}

	// The first asset claiming a scoped name, or empty when none does. A name that needs a fix has more than one
	// author by definition; the plan report's banner is where every one of them is listed.
	static FString FindAuthorPath(const TArray<FCrowdyModelSnapshotAuthor>& Authors, const FString& Key)
	{
		for (const FCrowdyModelSnapshotAuthor& Author : Authors)
		{
			if (CrowdyModelSnapshotKeys::KeysMatch(FCrowdySchemaSync::ScopedNameKey(Author.Scope, Author.Name), Key))
			{
				return Author.AssetPath;
			}
		}
		return FString();
	}

	bool IsKitOwnedType(const FString& TypeName) const
	{
		return FCrowdySchemaSync::IsRecognizedKitTypeName(TypeName, RecognizedKitTypePrefixes)
			|| RecognizedKitTypeNames.Contains(TypeName);
	}

	// Mirrors the three layers the prune protection applies to a server function: the exact names a deploy seeded,
	// the kit's snake-cased function-name prefix, and the kit's ownership of the whole container type (a composed kit
	// function can be verb-first, so its own name carries no prefix).
	bool IsKitOwnedFunction(const FString& TypeName, const FString& Name) const
	{
		return RecognizedKitFunctionNames.Contains(Name)
			|| FCrowdySchemaSync::IsRecognizedKitFunctionName(Name, RecognizedKitTypePrefixes)
			|| FCrowdySchemaSync::IsRecognizedKitTypeName(TypeName, RecognizedKitTypePrefixes);
	}

	// An automation is named from its entry-point function, which is the only handle the prune protection has on one.
	bool IsKitOwnedAutomation(const FString& Name) const
	{
		return RecognizedKitAutomationNames.Contains(Name)
			|| FCrowdySchemaSync::IsRecognizedKitFunctionName(Name, RecognizedKitTypePrefixes);
	}
};

/**
 * Everything one finished schema plan produced that a snapshot is built from. Pointers rather than copies: the plan
 * owns this data and it is read once, at capture time. A null member means that part of the plan produced nothing.
 */
struct FCrowdyModelSnapshotPlan
{
	int64 AppId = 0;
	const TArray<FCrowdyDesiredContainerType>* DesiredTypes = nullptr;
	const TArray<FCrowdyGameModelFunctionInput>* DesiredFunctions = nullptr;
	const TArray<FCrowdyGameModelAutomationInput>* DesiredAutomations = nullptr;
	const TArray<FCrowdyGameModelAutomationTriggerInput>* DesiredTriggers = nullptr;
	const TArray<FCrowdySchemaAuthorship>* FunctionAuthorship = nullptr;
	const TArray<FCrowdySchemaAuthorship>* AutomationAuthorship = nullptr;
	const FCrowdySchemaDelta* Delta = nullptr;
	const TSet<FString>* KitTypePrefixes = nullptr;
	const TSet<FString>* KitTypeNames = nullptr;
	const TSet<FString>* KitFunctionNames = nullptr;
	const TSet<FString>* KitAutomationNames = nullptr;
};

// Flatten one finished plan into the capture the browser reads. Inline, and this is its only home.
inline FCrowdyModelSnapshot CaptureModelSnapshot(const FCrowdyModelSnapshotPlan& Plan)
{
	FCrowdyModelSnapshot Snapshot;
	Snapshot.AppId = Plan.AppId;
	Snapshot.CapturedAt = FDateTime::UtcNow();

	if (Plan.KitTypePrefixes)      { Snapshot.RecognizedKitTypePrefixes = *Plan.KitTypePrefixes; }
	if (Plan.KitTypeNames)         { Snapshot.RecognizedKitTypeNames = *Plan.KitTypeNames; }
	if (Plan.KitFunctionNames)     { Snapshot.RecognizedKitFunctionNames = *Plan.KitFunctionNames; }
	if (Plan.KitAutomationNames)   { Snapshot.RecognizedKitAutomationNames = *Plan.KitAutomationNames; }

	if (Plan.DesiredTypes)
	{
		Snapshot.Types.Reserve(Plan.DesiredTypes->Num());
		for (const FCrowdyDesiredContainerType& Desired : *Plan.DesiredTypes)
		{
			FCrowdyModelSnapshotType Type;
			Type.TypeName = Desired.TypeName;
			Type.DisplayName = Desired.DisplayName;
			Type.OwningClassPath = Desired.OwningClassPath;
			Type.Attributes.Reserve(Desired.Props.Num());
			for (const FCrowdyDesiredPropertyDef& Prop : Desired.Props)
			{
				Type.Attributes.Add({ Prop.Key, Prop.ValueType, Prop.AuthoredName() });
			}
			Snapshot.Types.Add(MoveTemp(Type));
		}
	}

	if (Plan.DesiredFunctions)
	{
		Snapshot.Functions.Reserve(Plan.DesiredFunctions->Num());
		for (const FCrowdyGameModelFunctionInput& Desired : *Plan.DesiredFunctions)
		{
			FCrowdyModelSnapshotFunction Function;
			Function.TypeName = Desired.ContainerTypeName;
			Function.Name = Desired.Name;
			Function.ReturnType = Desired.ReturnType;
			Function.Description = Desired.Description;
			Snapshot.Functions.Add(MoveTemp(Function));
		}
	}

	if (Plan.DesiredAutomations)
	{
		Snapshot.Automations.Reserve(Plan.DesiredAutomations->Num());
		for (const FCrowdyGameModelAutomationInput& Desired : *Plan.DesiredAutomations)
		{
			FCrowdyModelSnapshotAutomation Automation;
			Automation.Name = Desired.Name;
			Automation.Description = Desired.Description;
			Automation.TargetTypeName = Desired.TargetTypeName;
			Automation.TriggerType = Desired.TriggerType;
			Automation.ScheduleKind = Desired.ScheduleKind;
			Automation.CronExpr = Desired.CronExpr;
			Automation.IntervalMs = Desired.IntervalMs;

			if (Plan.DesiredTriggers)
			{
				for (const FCrowdyGameModelAutomationTriggerInput& Trigger : *Plan.DesiredTriggers)
				{
					if (CrowdyModelSnapshotKeys::KeysMatch(Trigger.AutomationName, Desired.Name))
					{
						Automation.OnEvent = Trigger.OnEvent;
						Automation.OnEventPropertyKey = Trigger.PropertyKey;
						Automation.OnEventFunctionName = Trigger.FunctionName;
						break;
					}
				}
			}

			Snapshot.Automations.Add(MoveTemp(Automation));
		}
	}

	// The asset paths, from the authorship the gathers recorded for every effect the plan visited. A function the plan
	// accepted gets its path from the same list, so a code-declared row can name its asset whether or not it synced.
	if (Plan.FunctionAuthorship)
	{
		for (const FCrowdySchemaAuthorship& Authorship : *Plan.FunctionAuthorship)
		{
			if (Authorship.Name.IsEmpty())
			{
				continue; // an effect that compiled to no name at all names no entity
			}
			Snapshot.FunctionAuthors.Add({ Authorship.Scope, Authorship.Name, Authorship.AssetPath });
		}
	}
	if (Plan.AutomationAuthorship)
	{
		for (const FCrowdySchemaAuthorship& Authorship : *Plan.AutomationAuthorship)
		{
			if (Authorship.Name.IsEmpty())
			{
				continue;
			}
			Snapshot.AutomationAuthors.Add({ Authorship.Scope, Authorship.Name, Authorship.AssetPath });
		}
	}

	for (FCrowdyModelSnapshotFunction& Function : Snapshot.Functions)
	{
		Function.AssetPath = FCrowdyModelSnapshot::FindAuthorPath(
			Snapshot.FunctionAuthors, FCrowdySchemaSync::ScopedNameKey(Function.TypeName, Function.Name));
	}
	for (FCrowdyModelSnapshotAutomation& Automation : Snapshot.Automations)
	{
		Automation.AssetPath = FCrowdyModelSnapshot::FindAuthorPath(
			Snapshot.AutomationAuthors, FCrowdySchemaSync::ScopedNameKey(FString(), Automation.Name));
	}

	if (Plan.Delta)
	{
		for (const FCrowdySchemaTypeUpsert& Upsert : Plan.Delta->TypeUpserts)
		{
			CrowdyModelSnapshotKeys::AddUnique(
				Upsert.bIsNew ? Snapshot.TypeCreates : Snapshot.TypeUpdates, Upsert.Type.TypeName);
		}
		for (const FCrowdySchemaPropUpsert& Upsert : Plan.Delta->PropUpserts)
		{
			CrowdyModelSnapshotKeys::AddUnique(
				Upsert.bIsNew ? Snapshot.AttributeCreates : Snapshot.AttributeUpdates,
				FCrowdySchemaSync::ScopedNameKey(Upsert.ContainerTypeName, Upsert.Prop.Key));
		}
		for (const FCrowdySchemaFunctionUpsert& Upsert : Plan.Delta->FunctionUpserts)
		{
			CrowdyModelSnapshotKeys::AddUnique(
				Upsert.bIsNew ? Snapshot.FunctionCreates : Snapshot.FunctionUpdates,
				FCrowdySchemaSync::ScopedNameKey(Upsert.Function.ContainerTypeName, Upsert.Function.Name));
		}
		for (const FCrowdySchemaAutomationUpsert& Upsert : Plan.Delta->AutomationUpserts)
		{
			CrowdyModelSnapshotKeys::AddUnique(
				Upsert.bIsNew ? Snapshot.AutomationCreates : Snapshot.AutomationUpdates,
				FCrowdySchemaSync::ScopedNameKey(FString(), Upsert.Automation.Name));
		}
	}

	// Conflicted first, and never both: a duplicate name is also marked skipped by the gather, and "two assets claim
	// this, pick one" is the answer that leads somewhere, while "nothing could be checked" is not.
	if (Plan.FunctionAuthorship)
	{
		for (const FCrowdySchemaAuthorship& Authorship : *Plan.FunctionAuthorship)
		{
			if (Authorship.Name.IsEmpty())
			{
				continue;
			}
			const FString Key = FCrowdySchemaSync::ScopedNameKey(Authorship.Scope, Authorship.Name);
			if (Authorship.bConflicted)
			{
				CrowdyModelSnapshotKeys::AddUnique(Snapshot.NeedsAFixFunctionKeys, Key);
			}
			else if (Authorship.bSkipped && !Snapshot.FindFunction(Authorship.Scope, Authorship.Name))
			{
				CrowdyModelSnapshotKeys::AddUnique(Snapshot.UncheckableFunctionKeys, Key);
			}
		}
	}
	if (Plan.AutomationAuthorship)
	{
		for (const FCrowdySchemaAuthorship& Authorship : *Plan.AutomationAuthorship)
		{
			if (Authorship.Name.IsEmpty())
			{
				continue;
			}
			const FString Key = FCrowdySchemaSync::ScopedNameKey(Authorship.Scope, Authorship.Name);
			if (Authorship.bConflicted)
			{
				CrowdyModelSnapshotKeys::AddUnique(Snapshot.NeedsAFixAutomationKeys, Key);
			}
			else if (Authorship.bSkipped && !Snapshot.FindAutomation(Authorship.Name))
			{
				CrowdyModelSnapshotKeys::AddUnique(Snapshot.UncheckableAutomationKeys, Key);
			}
		}
	}

	return Snapshot;
}
