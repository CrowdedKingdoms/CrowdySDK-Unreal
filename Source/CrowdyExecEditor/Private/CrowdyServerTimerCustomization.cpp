#include "CrowdyServerTimerCustomization.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IPropertyTypeCustomization> FCrowdyServerTimerCustomization::MakeInstance()
{
	return MakeShared<FCrowdyServerTimerCustomization>();
}

void FCrowdyServerTimerCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	FName Name;
	const TSharedPtr<IPropertyHandle> NameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerTimer, Name));
	if (NameHandle.IsValid())
	{
		NameHandle->GetValue(Name);
	}
	HeaderRow
		.FilterString(FText::FromName(Name))
		.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text_Lambda([PropertyHandle]()
			{
				TArray<const void*> Data;
				PropertyHandle->AccessRawData(Data);
				const FCrowdyServerTimer* Timer = Data.Num() == 1 ? static_cast<const FCrowdyServerTimer*>(Data[0]) : nullptr;
				return Timer ? CrowdyServerObjectText::TimerTitle(*Timer) : FText::GetEmpty();
			})
		];
}

void FCrowdyServerTimerCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	const TSharedPtr<IPropertyHandle> Repeat = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerTimer, Repeat));
	uint32 Count = 0;
	PropertyHandle->GetNumChildren(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<IPropertyHandle> Child = PropertyHandle->GetChildHandle(Index);
		if (!Child.IsValid())
		{
			continue;
		}
		IDetailPropertyRow& Row = ChildBuilder.AddProperty(Child.ToSharedRef());
		if (Child->GetProperty() && Child->GetProperty()->GetFName() == GET_MEMBER_NAME_CHECKED(FCrowdyServerTimer, Seconds))
		{
			CrowdyServerObjectRows::ShowSeconds(Row, Child.ToSharedRef());
			// The custom row replaces the property row that Edit Condition hides, so it hides itself for a schedule.
			Row.Visibility(TAttribute<EVisibility>::CreateLambda([Repeat]()
			{
				uint8 Value = 0;
				const bool bSchedule = Repeat.IsValid() && Repeat->GetValue(Value) == FPropertyAccess::Success && Value == static_cast<uint8>(ECrowdyServerTimerRepeat::Schedule);
				return bSchedule ? EVisibility::Collapsed : EVisibility::Visible;
			}));
		}
	}
}
