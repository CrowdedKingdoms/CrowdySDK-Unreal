// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "SGraphNode.h"

class SVerticalBox;
class SWidget;
class UCrowdyEffectGraphNode;
class UEdGraphNode;
enum class ECrowdyEffectRole : uint8;

/**
 * A node widget for the Crowdy effect graph that renders inline editors directly in the node body, so a designer sets
 * a Constant's value, an Attribute's role and name, an operator, a Tuning's $param, or a Call's callee right on the
 * node instead of hunting through the Selected Node panel. It is a plain SGraphNode otherwise, so it also renders the
 * engine's on-node error badge (stamped after each compile) with no custom widget.
 *
 * A node-body edit that changes the node's pins (a Call's argument count, a Unary's operator, a Constant's type) is
 * deferred to the next tick before reconstructing, so a Slate callback never rebuilds the pin it was dispatched from.
 */
class SCrowdyEffectGraphNode : public SGraphNode
{
public:
	SLATE_BEGIN_ARGS(SCrowdyEffectGraphNode) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UCrowdyEffectGraphNode* InNode);

	// Rebuilds the node widget when a compile stamps or clears this node's error badge, so the badge appears without
	// waiting for a full graph refresh.
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

protected:
	virtual void CreateBelowPinControls(TSharedPtr<SVerticalBox> MainBox) override;

	// Places a call node's result pin at the bottom of the output column instead of beside the first argument, so the
	// result reads as the outcome of the argument list rather than as a sibling of its first input.
	virtual void AddPin(const TSharedRef<SGraphPin>& PinToAdd) override;

private:
	// Builds the inline editor rows for the node's concrete type, or an empty widget when the type has no inline body.
	TSharedRef<SWidget> BuildInlineBody();

	// Applies a committed node-body edit. When the edit changes the node's pin set, the reconstruct (and the graph
	// change broadcast that may rebuild this very widget) is deferred to the next tick, so the current Slate callback
	// unwinds first.
	void CommitNodeChange(bool bReconstructPins);

	// A combo of a UENUM's values bound to a node property. Get reads the current value; Set writes it under a
	// transaction. bReconstructOnChange reallocates the node's pins after the change (for a property that reshapes pins).
	template <typename TEnum>
	TSharedRef<SWidget> MakeEnumCombo(
		TFunction<TEnum()> Get, TFunction<void(TEnum)> Set, bool bReconstructOnChange, const FText& TransactionLabel);

	// A single-line text field bound to a node string property, committing on enter / focus-loss under a transaction.
	TSharedRef<SWidget> MakeTextField(
		TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& HintText, const FText& TransactionLabel);

	// A true/false chooser for a Constant node's boolean literal (so it can only ever hold "true" or "false").
	TSharedRef<SWidget> MakeBoolLiteralField();

	// A picker for an attribute name: a combo listing the effect container class's Server Owned attributes, with a
	// type-it-yourself box at the top of the menu. The typed fallback stays because a Source-role read (or a ref read)
	// targets a container whose class is not known at author time, so the suggestion list can be incomplete.
	//
	// GetRole is read fresh every time the dropdown opens (the combo calls it from OnGetMenuContent, not once at
	// construction), so flipping the node's Role afterwards changes which container's keys the menu shows on its
	// next open. Pass a lambda that returns Target when the caller has no role of its own (a Read Ref node's
	// referenced container is resolved at runtime and is not Target or Source).
	TSharedRef<SWidget> MakeAttributePicker(
		TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& TransactionLabel,
		TFunction<ECrowdyEffectRole()> GetRole);

	TWeakObjectPtr<UCrowdyEffectGraphNode> EffectNode;

	// The last error state applied to the node body, so Tick can detect a compile that changed the badge and rebuild.
	bool bCachedHasError = false;
	int32 CachedErrorType = 0;
	FString CachedErrorMsg;
};

/**
 * Creates SCrowdyEffectGraphNode for every effect-graph node, so their inline body editors and error badges render.
 * Returns null for any other node, leaving it to the default factory.
 */
struct FCrowdyEffectGraphNodeFactory : public FGraphPanelNodeFactory
{
	virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override;
};
