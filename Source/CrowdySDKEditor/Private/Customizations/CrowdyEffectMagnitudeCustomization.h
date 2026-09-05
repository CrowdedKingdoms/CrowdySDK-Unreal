// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

class IPropertyHandle;
class SHorizontalBox;
class SWidget;

/**
 * Property-type customization for FCrowdyEffectMagnitude: renders the name, a value-type dropdown, and a typed
 * default-value editor (a numeric/text box, or a checkbox for bool) that writes canonical JSON into
 * DefaultValueJson, plus the curve and description. The legacy ValueType string and the migration flag are hidden.
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

	// Builds the default-value editor for the current value type. Rebuilt into DefaultEditorContainer whenever the
	// type dropdown changes.
	TSharedRef<SWidget> BuildDefaultEditor();

	// Called when the value-type dropdown changes: clears the now-mistyped default and rebuilds the editor.
	void OnValueTypeChanged();

	TSharedPtr<IPropertyHandle> ValueTypeEnumHandle;
	TSharedPtr<IPropertyHandle> DefaultJsonHandle;
	TSharedPtr<SHorizontalBox> DefaultEditorContainer;
};
