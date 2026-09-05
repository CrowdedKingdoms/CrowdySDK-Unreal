// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "IPropertyTypeCustomization.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

class UCrowdyEffectGraphNode_Result;

/**
 * Details-panel customizations for the effect-graph nodes, shown in the graph editor's "Selected Node" panel. They
 * keep every field free-text (so a value never has to come from a list, matching the effect asset's own picker
 * idiom) but add a "Pick" dropdown of the sensible candidates - the container class's Server Owned attributes, the
 * effect's tuning magnitudes, or the builtin functions - so a designer stops hand-typing (and mistyping) names.
 */

/**
 * The one-click "return the new value of <attribute>" shortcut offered on a Result node. Answering with the
 * post-mutation value of the attribute the effect just wrote is the common shape, so it is a button rather than an
 * attribute node the author has to place and wire by hand. It is deliberately not an implicit rule: it cannot express
 * a read through another container's id, and applying it to every effect would declare a return type on effects
 * nobody edited.
 *
 * The decision and the edit are free functions so both can be exercised without the button.
 */
namespace CrowdyEffectReturnDefault
{
	/** Whether the shortcut is offered on a Result node, and what it would author. */
	struct FPlan
	{
		bool bAvailable = false;

		// The write the return would read back. Only meaningful when bAvailable.
		ECrowdyEffectRole Role = ECrowdyEffectRole::Target;
		FString Attribute;

		// One line for the author: what the shortcut would do, or why it is not offered.
		FText Message;
	};

	/** Reads the node only; offered when it has exactly one named write and does not already return a value. */
	FPlan Plan(const UCrowdyEffectGraphNode_Result* Result);

	/**
	 * Turns the return on, adds the attribute read it names, and wires that read into the Return pin, as one undo
	 * step. False when the shortcut was not offered or the edit could not complete, in which case nothing changes.
	 */
	bool Apply(UCrowdyEffectGraphNode_Result* Result);
}

/**
 * One detail customization shared by the Attribute, Read Ref, Tuning, Call, and Result nodes. It replaces a node's
 * single free-text field (Attribute / ParamName / Callee) with a text box plus a "Pick" combo of options resolved
 * from the owning effect, and gives the Result node its one-click return shortcut. Registered under each of those
 * node class names; the concrete node type it is customizing selects what it adds.
 */
class FCrowdyEffectGraphNodeCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};

/**
 * A compact one-row editor for a Result node's write, shown as "[role] [attribute] [operator]" on a single line
 * instead of the default three expanded struct members. The attribute is a free-text box plus a "Pick" dropdown of
 * the effect's Server Owned attributes.
 */
class FCrowdyEffectGraphWriteCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, class IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
};

/**
 * A compact one-row editor for a Result node's condition: its optional Note on a single line, since the boolean it
 * gates is wired into the condition's pin on the node rather than authored here.
 */
class FCrowdyEffectGraphRequireCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, class IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
};
