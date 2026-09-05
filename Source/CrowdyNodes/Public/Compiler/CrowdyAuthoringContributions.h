#pragma once

#include "CoreMinimal.h"

class UFunction;

/**
 * The authoring surfaces that only mean something where remote entities are drawn as a crowd, which the SDK
 * shows only in a project whose settings select a backend that draws them that way.
 *
 * The SDK's own authoring UI is backend-agnostic: Crowdy Replicates and the recipient dropdown mean the
 * same thing whichever backend draws a remote entity. Two surfaces are not. Declaring an event a one-shot
 * action, and warning that a multicast body cannot do its work on a stand-in, both describe what happens
 * where an observer draws the entity as a crowd row. Shown in a project that draws every map with actors
 * they name concepts that do not exist there and a workflow whose node does nothing.
 *
 * Declared here rather than in the editor module because the two surfaces live in two different modules
 * (the details panel in CrowdySDKEditor, the compile warnings here) while one module supplies both, and
 * that module is uncooked-only and cannot depend on an editor module. Both can depend on this one.
 *
 * With nothing selected and nothing installed every query answers "no crowd", which is the correct answer
 * in a project that draws no crowds, and in a game process where there is no authoring going on at all.
 *
 * Same no-cycle hook pattern as CrowdyBlueprintCompileHooks and
 * CrowdyApplyEffectNodeShared::SetContainerBlueprintResolver.
 */
/**
 * One parameter a one-shot action event is read by, described well enough for an editor to create the pin
 * without knowing what an action is.
 *
 * Pin categories rather than reflected types, because the thing being created is a Blueprint pin. The
 * spellings are UEdGraphSchema_K2's own (PC_Int, PC_Real with PC_Double), passed through as plain names so
 * this header stays free of the graph module.
 */
struct FCrowdyActionParameterSpec
{
	FName Name;
	FName PinCategory;
	FName PinSubCategory;
};

namespace CrowdyAuthoringContributions
{
	/**
	 * Whether any map profile this project can resolve selects a backend that draws remote entities as crowd
	 * rows, which is what decides whether the crowd authoring surfaces are shown at all.
	 *
	 * Per-project and deliberately NOT per-map: a Blueprint cannot see which map profile will render it, and
	 * a project may put one map on a crowd backend and another on the actor pool, so a class authored
	 * anywhere in it can still end up drawn as a row. Hiding these surfaces for a class that will be a row is
	 * worse than showing them in a project that has one crowd map.
	 *
	 * Asked fresh every time rather than answered from a value recorded at startup, because the fact is a
	 * project setting: an author who adds a map profile or switches a Backend Class expects the next thing
	 * that asks to see the new answer without restarting the editor.
	 *
	 * The profiles it looks at are the ones a map can actually reach, in the order
	 * UCrowdySDKDeveloperSettings::ResolveProfileForWorld reaches them.
	 */
	CROWDYNODES_API bool IsCrowdRepresentationSelected();

	/**
	 * Installs the check that says why an event declared a one-shot action will start nothing.
	 *
	 * What an action event has to carry is the backend's business: it owns the parameter names, the types
	 * it reads them as, and the anim set the ids index. The compile pass owns only WHEN to ask, which is
	 * every time an author ticks the box. Returning an empty string means the event is fine.
	 *
	 * Hands back whatever it displaced, so a caller that installs over a live one can put it back rather
	 * than leaving the process with whichever installer ran last.
	 */
	CROWDYNODES_API TFunction<FString(const UFunction*)> SetActionEventValidator(
		TFunction<FString(const UFunction*)> Validator);

	// The installed check's verdict on Function, or an empty string when nothing is installed to ask. An
	// empty string is always "nothing to report", never "could not tell", so a caller warns on a non-empty
	// answer and says nothing otherwise.
	CROWDYNODES_API FString DescribeActionEventProblem(const UFunction* Function);

	/**
	 * Installs the list of parameters a one-shot action event is read by, in the order an author would want
	 * them created. Supplied by the same backend that supplies the validator, so the pins an editor offers
	 * to add and the parameters the runtime looks for cannot drift into two different spellings.
	 *
	 * Hands back what it displaced, for the same reason SetActionEventValidator does.
	 */
	CROWDYNODES_API TFunction<TArray<FCrowdyActionParameterSpec>()> SetActionParameterProvider(
		TFunction<TArray<FCrowdyActionParameterSpec>()> Provider);

	// The installed parameter list, or empty when nothing is installed. Empty means there is no offer to
	// make, which is the same thing an SDK-only project should see.
	CROWDYNODES_API TArray<FCrowdyActionParameterSpec> GetActionParameters();
}
