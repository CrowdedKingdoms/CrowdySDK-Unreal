// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h"    // ECrowdyModelRowKind, FCrowdyModelRow
#include "GameModel/CrowdyModelLoadState.h" // ECrowdyModelLoadState
#include "Model/CrowdyStudioTypes.h"        // FStudioFunction, FStudioAutomation, FStudioAutomationTrigger
#include "Templates/SharedPointer.h"

/**
 * What one entity in the Models browser is connected to, phrased for a line the reader can click.
 *
 * PURE: no Slate, no HTTP, no UObject, no world. The caller hands over the lists it already holds and this
 * answers from them; nothing here reads a server, a widget or an asset, and nothing here issues a read. Every
 * answer is computable from the three app-wide lists the Models tab already has plus the open model's cached
 * attributes.
 *
 * COMPOSED, NEVER RE-DERIVED. Every reference scan behind these answers is CrowdyGameModelDelete's own, so what
 * a delete warns about and what this list shows are one answer rather than two copies that drift apart. Two
 * scoping rules come with them and are inherited unchanged, because reversing either is wrong:
 *
 *   - A function is matched whatever model it is bound to. An expression reaches another model's attribute
 *     through a link, so narrowing to the model would miss exactly the functions that do, and naming one extra
 *     function costs the reader a line while missing one costs them a function that stops working.
 *   - A trigger whose model is empty is matched. A missing scope is a scope nobody determined, not a scope
 *     belonging to somebody else, and reading it the second way is how a real dependant goes unreported.
 *
 * CASE. A model name, an attribute key, a function name and an automation name are all server keys, and FString
 * comparison folds case by default. Every comparison here is case-sensitive. Two entities that differ only in
 * case are two entities.
 */

/** One thing on the other end of a link, as a navigation target the caller can act on without re-deriving it. */
struct FCrowdyModelLink
{
	// What the target is, in the row vocabulary, because the target IS a row somewhere.
	ECrowdyModelRowKind Kind = ECrowdyModelRowKind::Attribute;

	// The model to open and the section to open on it. Section is "attributes", "functions" or "automations".
	FString TargetModel;
	FString TargetSection;

	// The entity on that model, verbatim from the server.
	FString TargetName;

	// The line the reader sees. Already phrased, never built in a Slate binding: every answer here is a string
	// and a bound lambda would rebuild it on every painted frame.
	FString Display;

	// Why this link exists, in a few words, so a list of six lines stays readable.
	FString Relation;

	// False when the target model cannot be named: a write to another model that nothing in the function names,
	// a trigger whose model was never determined, an automation that belongs to no model. Such a link is shown
	// and is NOT clickable, with the reason in Display, because reporting nothing would hide a real dependency
	// and guessing a model would send the reader somewhere wrong.
	bool bNavigable = true;
};

namespace CrowdyModelCrossLinks
{
	// Everything that names one attribute: the functions that write it, the functions that name it anywhere
	// else, the automations whose selector or static parameters name it, and the triggers that wait on it. A
	// function that both writes and reads the key is one line, reported as the write, because that is the
	// stronger relationship and two lines about one function is a list the reader skips.
	TArray<FCrowdyModelLink> ForAttribute(
		const FString& OwningType, const FString& Key,
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const TArray<TSharedPtr<FStudioAutomation>>& Automations,
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers);

	// Everything one function connects to: the attributes it writes, the automations that run it, the triggers
	// that name it, and every OTHER model in this app carrying the same function name. That last one is what
	// makes a bare-name ambiguity visible: the server resolves some of these calls by name alone, so a reader
	// looking at one copy needs to know the others exist.
	//
	// AttributeDisplayNames names the attributes this project authors, server key to authored spelling, so the
	// links this draws to an attribute read the same name the grid beside it shows. Defaulted to empty, which
	// makes every attribute link fall back to the key reconstructed into words; callers with nothing to offer
	// need not build one.
	TArray<FCrowdyModelLink> ForFunction(
		const FStudioFunction& Function,
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const TArray<TSharedPtr<FStudioAutomation>>& Automations,
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
		const TMap<FString, FString>& AttributeDisplayNames = TMap<FString, FString>());

	// Everything one automation connects to: the function it runs, the model it targets, and what its event
	// triggers wait on. AttributeDisplayNames is the same authored-name lookup as ForFunction's, for the same
	// reason: a trigger's "fires this" line can name an attribute.
	TArray<FCrowdyModelLink> ForAutomation(
		const FStudioAutomation& Automation,
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
		const TMap<FString, FString>& AttributeDisplayNames = TMap<FString, FString>());

	// The links for whichever row the caller has, dispatched on Row.Kind. One entry point, so a caller has one
	// call site and a new row kind is one case here rather than a branch in a widget. A row whose entity the
	// app-wide lists do not carry yields nothing: there is no entity to answer about.
	TArray<FCrowdyModelLink> ForRow(
		const FCrowdyModelRow& Row,
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const TArray<TSharedPtr<FStudioAutomation>>& Automations,
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
		const TMap<FString, FString>& AttributeDisplayNames = TMap<FString, FString>());

	// The one line above the list. An empty result from a list nobody read is not "nothing names this", so the
	// caller passes the state of every list the answer was computed from and this says which situation it is in
	// rather than claiming a zero it cannot stand behind.
	//
	// The event triggers are a THIRD state and not part of the automations one, because they are a second read
	// chained behind the automations and it can fail on its own. Every "waits on it" and "names it" line an
	// automation gets comes from that read: fold the two and a failed trigger read removes those lines while this
	// line still reports a flat zero, which is the exact claim this function exists to refuse.
	FString SummaryLine(
		int32 LinkCount, ECrowdyModelLoadState FunctionsState, ECrowdyModelLoadState AutomationsState,
		ECrowdyModelLoadState AutomationTriggersState);
}
