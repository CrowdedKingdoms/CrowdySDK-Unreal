// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectMagnitudeCustomization.h"

#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"

#include "Replication/GameModel/Effect/CrowdyEffectMagnitudeJson.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectMagnitudeCustomization"

TSharedRef<IPropertyTypeCustomization> FCrowdyEffectMagnitudeCustomization::MakeInstance()
{
	return MakeShared<FCrowdyEffectMagnitudeCustomization>();
}

ECrowdyEffectValueType FCrowdyEffectMagnitudeCustomization::GetValueType() const
{
	uint8 Value = static_cast<uint8>(ECrowdyEffectValueType::Int);
	if (ValueTypeEnumHandle.IsValid())
	{
		ValueTypeEnumHandle->GetValue(Value);
	}
	return static_cast<ECrowdyEffectValueType>(Value);
}

void FCrowdyEffectMagnitudeCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TSharedPtr<IPropertyHandle> NameHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Name));
	ValueTypeEnumHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, ValueTypeEnum));
	DefaultJsonHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, DefaultValueJson));

	if (!NameHandle.IsValid() || !ValueTypeEnumHandle.IsValid() || !DefaultJsonHandle.IsValid())
	{
		HeaderRow.NameContent()[ PropertyHandle->CreatePropertyNameWidget() ];
		return;
	}

	// Rebuild the typed default editor (and clear the now-mistyped default) whenever the value type changes.
	ValueTypeEnumHandle->SetOnPropertyValueChanged(
		FSimpleDelegate::CreateSP(this, &FCrowdyEffectMagnitudeCustomization::OnValueTypeChanged));

	HeaderRow
	.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(420.f)
	.MaxDesiredWidth(720.f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			SNew(SBox)
			.MinDesiredWidth(120.f)
			.ToolTipText(LOCTEXT("NameTip", "The $param name referenced in the effect (no '$')."))
			[
				NameHandle->CreatePropertyValueWidget()
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			SNew(SBox)
			.MinDesiredWidth(90.f)
			[
				ValueTypeEnumHandle->CreatePropertyValueWidget()
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			SAssignNew(DefaultEditorContainer, SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				BuildDefaultEditor()
			]
		]
	];
}

void FCrowdyEffectMagnitudeCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// The curve and description are secondary; show them as ordinary child rows. Name, type, and the default are
	// handled inline in the header, and the legacy ValueType string + migration flag stay hidden.
	if (const TSharedPtr<IPropertyHandle> CurveHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Curve)))
	{
		ChildBuilder.AddProperty(CurveHandle.ToSharedRef());
	}
	if (const TSharedPtr<IPropertyHandle> DescriptionHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Description)))
	{
		ChildBuilder.AddProperty(DescriptionHandle.ToSharedRef());
	}
}

TSharedRef<SWidget> FCrowdyEffectMagnitudeCustomization::BuildDefaultEditor()
{
	const TSharedPtr<IPropertyHandle> Json = DefaultJsonHandle;
	if (!Json.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	const ECrowdyEffectValueType Type = GetValueType();

	if (Type == ECrowdyEffectValueType::Bool)
	{
		return SNew(SCheckBox)
			.ToolTipText(LOCTEXT("BoolDefaultTip", "The default value; unchecked leaves it as authored."))
			.IsChecked_Lambda([Json]()
			{
				FString Current;
				Json->GetValue(Current);
				return Current.TrimStartAndEnd() == TEXT("true") ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Json](ECheckBoxState State)
			{
				Json->SetValue(State == ECheckBoxState::Checked ? FString(TEXT("true")) : FString(TEXT("false")));
			});
	}

	// Int / Float / String / ContainerRef: a text box that canonicalizes on commit. Empty means the magnitude is
	// required (no default). The canonical JSON is what BuildInvokeParams / the lowering read.
	return SNew(SEditableTextBox)
		.ToolTipText(LOCTEXT("DefaultTip",
			"The default value (leave empty to make the magnitude required). Numbers and booleans are stored as "
			"JSON; text is quoted automatically."))
		.HintText(LOCTEXT("DefaultHint", "default (empty = required)"))
		.Text_Lambda([Json, Type]()
		{
			FString Current;
			Json->GetValue(Current);
			return FText::FromString(CrowdyEffectMagnitudeJson::FromCanonicalJson(Type, Current));
		})
		.OnTextCommitted_Lambda([Json, Type](const FText& NewText, ETextCommit::Type)
		{
			Json->SetValue(CrowdyEffectMagnitudeJson::ToCanonicalJson(Type, NewText.ToString()));
		});
}

void FCrowdyEffectMagnitudeCustomization::OnValueTypeChanged()
{
	// The old default was canonical for the previous type, so it is likely mistyped now; clear it rather than
	// silently keep an int "5" as a string "5". The designer re-enters a default in the rebuilt typed editor.
	if (DefaultJsonHandle.IsValid())
	{
		DefaultJsonHandle->SetValue(FString());
	}

	if (DefaultEditorContainer.IsValid())
	{
		DefaultEditorContainer->ClearChildren();
		DefaultEditorContainer->AddSlot()
		.FillWidth(1.f)
		[
			BuildDefaultEditor()
		];
	}
}

#undef LOCTEXT_NAMESPACE
