// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

/**
 * Pure, headless-testable option lists for the constrained effect-picker (FCrowdyEffectCustomization). No
 * Slate, no UObject mutation: every function takes plain data in and returns plain data out, so the picker's
 * correctness (which operators a value type allows, which attributes a class exposes) is unit-tested
 * independent of the Details-panel plumbing built on top of it (see CrowdyEffectPickerOptionsTests.cpp).
 */
namespace CrowdyEffectPickerOptions
{
	// The assignment operators the picker should offer for a Game Model value type. This mirrors
	// FCrowdyEffectLowering::LowerAssignment's own type gate EXACTLY (an arithmetic op on a non-numeric
	// attribute is a lowering Error): numeric ("int"/"float") accepts all five compound-assignment operators;
	// "bool"/"string"/anything else (including an empty/unresolved value type) accepts only Set, since that is
	// the only operator both sides ever agree is valid. This is a client-side guardrail, not the real
	// validation -- the compile preview and the server schema remain authoritative -- so it must never grey out
	// an operator the lowering would actually accept, nor offer one the lowering would reject.
	CROWDYSDKEDITOR_API TArray<ECrowdyEffectAssignmentOp> AllowedAssignmentOps(const FString& ValueType);

	// The same gate keyed off the typed magnitude value type. Forwards through the enum's wire string, so it stays
	// in lockstep with the string overload (and with the lowering's own type gate).
	CROWDYSDKEDITOR_API TArray<ECrowdyEffectAssignmentOp> AllowedAssignmentOps(ECrowdyEffectValueType ValueType);

	// The discovered Server Owned attributes of Class. A null Class returns empty. Thin wrapper over
	// FCrowdyAttributeRegistry::DiscoverForClass kept here so the picker has one seam to call (and so a test can
	// exercise the picker's own contract without reaching into the registry directly).
	CROWDYSDKEDITOR_API TArray<FCrowdyAttributeDef> AttributesForClass(const UClass* Class);

	// The Game Model value type ("int"|"float"|"bool"|"string") of the attribute named or keyed
	// AttributeKeyOrName on Class matched case-insensitively against either the property name or the server key,
	// the same two spellings FCrowdyEffectLowering::ResolveAttr accepts. Returns "" when Class is null, the name
	// is empty, or no attribute matches.
	CROWDYSDKEDITOR_API FString ValueTypeForAttribute(const UClass* Class, const FString& AttributeKeyOrName);

	// The verb shown for an assignment operator in the sentence-style picker: Set, Increase (Add), Decrease
	// (Subtract), Multiply, Divide. This is a pure two-way mapping - OpForVerbLabel is its exact inverse - so a
	// designer picks a verb and the effect keeps compiling to the same operator. Display text only; the operator
	// (not the verb) is what lowers.
	CROWDYSDKEDITOR_API FText VerbLabel(ECrowdyEffectAssignmentOp Op);

	// The inverse of VerbLabel: resolve a verb string back to its operator. Returns true and fills OutOp on a
	// match (case-insensitive); returns false and leaves OutOp untouched otherwise.
	CROWDYSDKEDITOR_API bool OpForVerbLabel(const FString& VerbText, ECrowdyEffectAssignmentOp& OutOp);

	// The affected-attribute phrase for a step sentence: "Target's <attr>" for the Target role, or
	// "<SourceRoleLabel>'s <attr>" for the Source role (SourceRoleLabel is the effect's cosmetic label for the
	// instigator, e.g. "Attacker"; an empty label falls back to "Source"). Pure display text; the role, not the
	// phrase, is what lowers.
	CROWDYSDKEDITOR_API FString TargetPhrase(ECrowdyEffectRole Role, const FString& Attribute, const FString& SourceRoleLabel);

	// The bare word for a role in a sentence: "Target", or SourceRoleLabel (falling back to "Source" when empty).
	CROWDYSDKEDITOR_API FString RoleWord(ECrowdyEffectRole Role, const FString& SourceRoleLabel);

	// The value to write into UCrowdyEffect::AutomationChangePropertyKey when a picker entry for Def is chosen.
	// The automation's property-change trigger sends this straight to the server as the property_changed
	// filter's propertyKey with no name/key resolution on the way (unlike an attribute reference inside the
	// effect body, which FCrowdyEffectLowering::ResolveAttr accepts spelled either way), and the schema sync
	// upserts every property definition under its server key (FCrowdySchemaSync writes Prop.Key = Def.Key), so
	// this must be the server key, never the raw UPROPERTY name, or the trigger matches nothing and never fires.
	CROWDYSDKEDITOR_API FString PropertyKeyForAutomationTrigger(const FCrowdyAttributeDef& Def);

	// The value the attribute picker (an Attribute operand, or an assignment step's Attribute field) writes when
	// Def is chosen: the server key, falling back to the property name only if a key was never assigned (a def
	// straight from discovery always has one; the fallback only guards a hand-built Def). This must be the same
	// spelling every other attribute reference resolves through (GetExpressionAttributeNames' autocomplete, an
	// operand's Name field), since using two different spellings of the same attribute inside one effect program
	// is a lowering error.
	CROWDYSDKEDITOR_API FString AttributePickerWrittenValue(const FCrowdyAttributeDef& Def);

	// The spelling the EffectScript body editor's autocomplete offers, which is deliberately NOT the one above: the
	// declared property name, falling back to the server key only for a hand-built Def with no name. An attribute
	// answers to either spelling, but using both inside one effect is a lowering error, so each surface has to pick
	// the one its author will otherwise type. A body is hand-authored against the container class the author is
	// looking at, so it is the declared name; a picker surface stores its choice next to other stored choices, so
	// it is the key. Diagnostics that name an attribute (an unknown-attribute "did you mean") use the declared name
	// too, which is what a body author sees.
	CROWDYSDKEDITOR_API FString AttributeNameForScriptCompletion(const FCrowdyAttributeDef& Def);

	// True when ClassName looks like a transient reflection artifact (a Blueprint recompile's stale skeleton or
	// reinstance class, or an editor-only placeholder) that must never be offered as a real container type even
	// if it happens to carry a CrowdyContainer tag. Mirrors FCrowdySchemaSync's own class-sweep hygiene.
	CROWDYSDKEDITOR_API bool IsTransientReflectionClassName(const FString& ClassName);

	// True when Class should be offered in the "known container type" picker: it declares a CrowdyContainer tag
	// (OutTypeName filled) and is not a test-only fixture (meta=(CrowdyContainerTest)). False (OutTypeName
	// untouched) for a null Class, an untagged class, or a test fixture.
	CROWDYSDKEDITOR_API bool IsOfferableContainerType(const UClass* Class, FString& OutTypeName);

	// Every distinct Game Model container type name reachable from a currently loaded UClass (IsOfferableContainerType,
	// skipping a transient reflection artifact), sorted and deduplicated. Does not force-load anything itself: the
	// caller (BuildContainerClassRow's session-scoped LoadAllTaggedAssets) already does that once per editor
	// session, so an unopened marked container Blueprint is missing here until that has run.
	CROWDYSDKEDITOR_API TArray<FString> KnownContainerTypeNames();
}
