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
#include "Widgets/Text/STextBlock.h"

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

bool FCrowdyEffectMagnitudeCustomization::IsRequired() const
{
	bool bValue = false;
	if (RequiredHandle.IsValid())
	{
		RequiredHandle->GetValue(bValue);
	}
	return bValue;
}

void FCrowdyEffectMagnitudeCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TSharedPtr<IPropertyHandle> NameHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Name));
	ValueTypeEnumHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, ValueTypeEnum));
	RequiredHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, bRequired));
	DefaultJsonHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, DefaultValueJson));

	if (!NameHandle.IsValid() || !ValueTypeEnumHandle.IsValid() || !RequiredHandle.IsValid()
		|| !DefaultJsonHandle.IsValid())
	{
		HeaderRow.NameContent()[ PropertyHandle->CreatePropertyNameWidget() ];
		return;
	}

	// Rebuild the typed default editor (and clear the now-mistyped default) whenever the value type changes.
	ValueTypeEnumHandle->SetOnPropertyValueChanged(
		FSimpleDelegate::CreateSP(this, &FCrowdyEffectMagnitudeCustomization::OnValueTypeChanged));

	// A required parameter has no default, so the editor for it is swapped out as the box is ticked.
	RequiredHandle->SetOnPropertyValueChanged(
		FSimpleDelegate::CreateSP(this, &FCrowdyEffectMagnitudeCustomization::OnRequiredChanged));

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
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.f, 0.f, 8.f, 0.f)
		[
			BuildRequiredCheckBox()
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
	// The curve and description are secondary; show them as ordinary child rows. Name, type, Required and the
	// default are handled inline in the header, and the legacy ValueType string + migration flags stay hidden.
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

TSharedRef<SWidget> FCrowdyEffectMagnitudeCustomization::BuildRequiredCheckBox()
{
	const TSharedPtr<IPropertyHandle> Required = RequiredHandle;
	if (!Required.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	return SNew(SCheckBox)
		.ToolTipText(LOCTEXT("RequiredTip",
			"Every caller must supply this parameter. A required parameter has no default value, so a Blueprint "
			"apply node fails to compile until its pin is wired or set."))
		.IsChecked_Lambda([Required]()
		{
			bool bValue = false;
			Required->GetValue(bValue);
			return bValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([Required](ECheckBoxState State)
		{
			Required->SetValue(State == ECheckBoxState::Checked);
		})
		[
			SNew(STextBlock).Text(LOCTEXT("RequiredLabel", "Required"))
		];
}

TSharedRef<SWidget> FCrowdyEffectMagnitudeCustomization::BuildDefaultEditor()
{
	const TSharedPtr<IPropertyHandle> Json = DefaultJsonHandle;
	if (!Json.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	// A required parameter's default is never read, so it is not offered for editing either: showing an editable
	// value the effect ignores is what made required-ness ambiguous in the first place. The stored text is left
	// alone, so clearing Required brings the authored default back.
	if (IsRequired())
	{
		return SNew(STextBlock)
			.Text(LOCTEXT("NoDefaultWhenRequired", "supplied by the caller"))
			.ToolTipText(LOCTEXT("NoDefaultWhenRequiredTip",
				"A required parameter has no default value. Clear Required to author one."))
			.IsEnabled(false);
	}

	const ECrowdyEffectValueType Type = GetValueType();

	if (Type == ECrowdyEffectValueType::Bool)
	{
		return SNew(SCheckBox)
			.ToolTipText(LOCTEXT("BoolDefaultTip",
				"The default value used when a caller supplies none. Unchecked means false; tick Required instead to "
				"make every caller supply the value."))
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

	// Int / Float / String / ContainerRef: a text box that canonicalizes on commit. The canonical JSON is what
	// BuildInvokeParams / the lowering read.
	return SNew(SEditableTextBox)
		.ToolTipText(LOCTEXT("DefaultTip",
			"The default value used when a caller supplies none. Numbers and booleans are stored as JSON; text is "
			"quoted automatically. Tick Required to make every caller supply the value instead."))
		.HintText(LOCTEXT("DefaultHint", "default"))
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
	//
	// Required-ness moves with it, because the two together are what the parameter means: a bool becomes optional
	// with an explicit false, since a checkbox cannot draw "no value" and so cannot help a designer author a
	// required bool; every other type is left with no default and therefore has to be supplied by the caller.
	if (DefaultJsonHandle.IsValid() && RequiredHandle.IsValid())
	{
		bool bRequired = false;
		FString DefaultJson;
		UCrowdyEffect::ApplyValueTypeChangeDefaults(GetValueType(), bRequired, DefaultJson);
		RequiredHandle->SetValue(bRequired);
		DefaultJsonHandle->SetValue(DefaultJson);
	}

	RebuildDefaultEditor();
}

void FCrowdyEffectMagnitudeCustomization::OnRequiredChanged()
{
	// An optional bool needs an explicit default, since its checkbox cannot draw the absence of one. This runs on
	// the way out of Required as well as into it, so clearing the box leaves a usable bool default behind.
	if (DefaultJsonHandle.IsValid() && !IsRequired() && GetValueType() == ECrowdyEffectValueType::Bool)
	{
		FString Current;
		DefaultJsonHandle->GetValue(Current);
		if (Current.IsEmpty())
		{
			DefaultJsonHandle->SetValue(FString(TEXT("false")));
		}
	}

	RebuildDefaultEditor();
}

void FCrowdyEffectMagnitudeCustomization::RebuildDefaultEditor()
{
	if (!DefaultEditorContainer.IsValid())
	{
		return;
	}
	DefaultEditorContainer->ClearChildren();
	DefaultEditorContainer->AddSlot()
	.FillWidth(1.f)
	[
		BuildDefaultEditor()
	];
}

#undef LOCTEXT_NAMESPACE
