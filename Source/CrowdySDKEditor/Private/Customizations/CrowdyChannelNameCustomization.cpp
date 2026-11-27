#include "Customizations/CrowdyChannelNameCustomization.h"

#include "Customizations/CrowdyChannelPicker.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

bool FCrowdyChannelNameTypeIdentifier::IsPropertyTypeCustomized(const IPropertyHandle& PropertyHandle) const
{
	return PropertyHandle.HasMetaData(FName(CrowdyChannelPickerMetaKey));
}

TSharedRef<IPropertyTypeCustomization> FCrowdyChannelNameCustomization::MakeInstance()
{
	return MakeShared<FCrowdyChannelNameCustomization>();
}

void FCrowdyChannelNameCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	CrowdyChannelPicker::FetchChannelNamesIfStale();

	const TWeakPtr<IPropertyHandle> WeakHandle = PropertyHandle;
	HeaderRow
	.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			PropertyHandle->CreatePropertyValueWidget()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SComboButton)
			.IsEnabled(PropertyHandle->IsEditable())
			.ToolTipText(FText::FromString(TEXT("Pick from the app's channels")))
			.OnGetMenuContent_Lambda([WeakHandle]
			{
				return CrowdyChannelPicker::MakeMenu(
					[WeakHandle](const FString& ChannelName)
					{
						if (const TSharedPtr<IPropertyHandle> Handle = WeakHandle.Pin())
						{
							Handle->SetValue(ChannelName);
						}
					},
					FText::FromString(TEXT("(None)")),
					FText::FromString(TEXT("Clear the channel name.")));
			})
			.ButtonContent()
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Pick")))
			]
		]
	];
}
