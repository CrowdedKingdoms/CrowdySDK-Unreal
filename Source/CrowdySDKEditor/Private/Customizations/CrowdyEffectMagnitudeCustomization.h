// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

class IPropertyHandle;
class SHorizontalBox;
class SWidget;

/**
 * Property-type customization for FCrowdyEffectMagnitude: renders the name, a value-type dropdown, a Required
 * checkbox, and a typed default-value editor (a numeric/text box, or a checkbox for bool) that writes canonical
 * JSON into DefaultValueJson, plus the curve and description. The legacy ValueType string and the migration flags
 * are hidden. A required parameter has no default, so its default editor is replaced by a note saying the caller
 * supplies the value; the authored default is kept and comes back when Required is cleared.
 * Registered against the "CrowdyEffectMagnitude" struct in FCrowdySDKEditorModule.
 */
class FCrowdyEffectMagnitudeCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, class IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	ECrowdyEffectValueType GetValueType() const;

	// Whether the parameter is currently marked required, so the default editor knows to stand down.
	bool IsRequired() const;

	// Builds the default-value editor for the current value type, or the inert note shown while the parameter is
	// required. Rebuilt into DefaultEditorContainer whenever the type dropdown or the Required box changes.
	TSharedRef<SWidget> BuildDefaultEditor();

	// The Required checkbox shown between the value type and the default editor.
	TSharedRef<SWidget> BuildRequiredCheckBox();

	// Called when the value-type dropdown changes: replaces the now-mistyped default and required-ness with the pair
	// the new type calls for, then rebuilds the editor.
	void OnValueTypeChanged();

	// Called when the Required box changes: swaps the default editor for the note, or back.
	void OnRequiredChanged();

	// Replaces DefaultEditorContainer's contents with a freshly built default editor.
	void RebuildDefaultEditor();

	TSharedPtr<IPropertyHandle> ValueTypeEnumHandle;
	TSharedPtr<IPropertyHandle> RequiredHandle;
	TSharedPtr<IPropertyHandle> DefaultJsonHandle;
	TSharedPtr<SHorizontalBox> DefaultEditorContainer;
};
