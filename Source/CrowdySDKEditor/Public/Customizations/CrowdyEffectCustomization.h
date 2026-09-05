// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailCategoryBuilder;
class IDetailChildrenBuilder;
class IPropertyHandle;
class IPropertyUtilities;
class UCrowdyEffect;
enum class ECrowdyEffectSource : uint8;

/**
 * The Details customization for a UCrowdyEffect: the authoring-mode switch (Script / Graph), a class-filtered
 * Container Class picker, a Source Container Type picker, and the tuning-parameters section with its
 * missing-parameters helper. The EffectScript body is edited in its own window (the Script editor's canvas); the
 * node graph is edited in its own toolkit. Both docked toolkits show their own live Compile() readout, so this
 * panel does not duplicate one.
 *
 * Edits go straight through the real property handles rather than a detached working copy, so undo/redo,
 * transactions, and asset-dirtying all work exactly like any other Details panel edit.
 */
class FCrowdyEffectCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCrowdyEffectCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	// A segmented "Script | Graph" authoring-mode switch at the top of the panel. Selecting a mode sets the effect's
	// Source and reopens the asset in the editor that mode uses (the property panel for Script, the node-graph toolkit
	// for Graph), deferred a tick so it never tears down the very Details panel whose click is still on the stack.
	void BuildAuthoringModeRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		ECrowdyEffectSource CurrentSource);
	void SwitchAuthoringMode(ECrowdyEffectSource NewSource);

	// The tuning parameters list (the Magnitudes array, which displays as "Tuning Parameters" and keeps its own type
	// customization), followed by the missing-parameters helper. Shown for every authoring surface, since a tuning
	// parameter balances a script effect as much as a graph one.
	void BuildTuningParametersSection(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		TSharedRef<IPropertyHandle> MagnitudesHandle);

	// Replaces the ContainerClass row with a class picker filtered to Game Model container classes (a native or
	// Blueprint class carrying the CrowdyContainer tag, or a marked-but-not-yet-recompiled container Blueprint).
	// Force-loads the tagged assets first so an unopened marked container appears.
	void BuildContainerClassRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		TSharedRef<IPropertyHandle> ContainerClassHandle);

	// Replaces the SourceContainerType row with a dropdown of the container types the project declares, whose first
	// entry ("Same as target") writes the empty value that makes the source another container of the target's own
	// type. A container type name is a server key the compile matches exactly, so it is picked, never typed: a
	// hand-typed name that matches nothing is refused rather than silently checked against the wrong attributes.
	void BuildSourceContainerTypeRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		TSharedRef<IPropertyHandle> SourceContainerTypeHandle);
	TSharedRef<SWidget> BuildSourceContainerTypeMenu(TSharedRef<IPropertyHandle> Handle);
	FText GetSourceContainerTypeLabel(TSharedPtr<IPropertyHandle> Handle) const;

	// A one-click "Missing Parameters" row: the parameters the effect references but never declared become new tuning
	// parameters (int, required). ComputeMissingMagnitudes is the pure list, shared by the button label and the action.
	void BuildAddMissingTuningParametersRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		TSharedRef<IPropertyHandle> MagnitudesHandle);
	TArray<FString> ComputeMissingMagnitudes() const;
	void AddMissingMagnitudes(TSharedRef<IPropertyHandle> MagnitudesHandle);

	// Gives the three free-text Automation fields that name a property or a container type a picker, in place,
	// without disturbing the rest of the (otherwise default-generated) Automation category: each property keeps
	// its normal row position and EditConditionHides behaviour, only its value widget grows a "Pick" dropdown
	// alongside the existing free-text box.
	void BuildAutomationSection(IDetailLayoutBuilder& DetailBuilder);

	// "On Property Key": free text plus a picker of the target container class's Server Owned attributes. The
	// picker writes the attribute's server KEY (CrowdyEffectPickerOptions::PropertyKeyForAutomationTrigger), not
	// its property name, since this string rides straight to the server as the property_changed trigger's
	// propertyKey filter with no name/key resolution on the way.
	void BuildAutomationPropertyKeyRow(IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> Handle);
	TSharedRef<SWidget> BuildAutomationPropertyKeyPickerMenu(TSharedRef<IPropertyHandle> Handle);

	// "On Container Type" and "Target Type Override": free text plus a picker of every known container type name
	// (CrowdyEffectPickerOptions::KnownContainerTypeNames), shared by both fields since they hold the same kind
	// of value.
	void BuildAutomationContainerTypeRow(IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> Handle);
	TSharedRef<SWidget> BuildContainerTypePickerMenu(TSharedRef<IPropertyHandle> Handle);

	// The effect's target container class, resolved the same way UCrowdyEffect::Compile() resolves it.
	UClass* ResolveContainerClass() const;

	// ContainerClass / Source change the row SET (the attribute list), so they request a deferred full-panel refresh
	// via the cached IPropertyUtilities (mirrors FCrowdyReplicatedVariableCustomization::SetMode's
	// RequestForceRefresh, deferred to next tick so it never tears down the very Details widgets whose Slate
	// callback is still on the stack); guarded against re-entrancy. Everything else that feeds Compile() just
	// recomputes the cached missing-parameters list in place, with no structural rebuild.
	void RequestStructuralRefresh();
	void RefreshCompilePreview();

	TWeakObjectPtr<UCrowdyEffect> EditedEffect;
	TSharedPtr<IPropertyUtilities> PropertyUtilities;
	bool bRefreshRequested = false;

	// Refreshed by RefreshCompilePreview so the "Add missing magnitudes" button reads a cached list rather than
	// re-parsing the effect every Slate tick.
	TArray<FString> CachedMissingParams;
};
