#include "CrowdyServerListValueNameCustomization.h"

#include "CrowdyServerObjectDefinition.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "StructUtils/PropertyBag.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerListValueNameCustomization"

namespace CrowdyServerListValueNameCustomizationDetail
{
	void AddDisabledEntry(FMenuBuilder& Menu, const FText& Label)
	{
		Menu.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
	}
}

TSharedRef<IPropertyTypeCustomization> FCrowdyServerListValueNameCustomization::MakeInstance()
{
	return MakeShared<FCrowdyServerListValueNameCustomization>();
}

void FCrowdyServerListValueNameCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	HeaderRow.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	];
}

void FCrowdyServerListValueNameCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	TArray<UObject*> Outers;
	PropertyHandle->GetOuterObjects(Outers);
	Definition = Outers.Num() == 1 ? Cast<UCrowdyServerObjectDefinition>(Outers[0]) : nullptr;
	ListHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerListValueName, List));
	ValueIdHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerListValueName, ValueId));
	const TSharedPtr<IPropertyHandle> ServerNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerListValueName, ServerName));
	if (!ListHandle.IsValid() || !ValueIdHandle.IsValid() || !ServerNameHandle.IsValid())
	{
		return;
	}
	AddPickerRow(ChildBuilder, ListHandle.ToSharedRef(), &FCrowdyServerListValueNameCustomization::MakeListMenu, &FCrowdyServerListValueNameCustomization::GetListText);
	AddPickerRow(ChildBuilder, ValueIdHandle.ToSharedRef(), &FCrowdyServerListValueNameCustomization::MakeValueMenu, &FCrowdyServerListValueNameCustomization::GetValueText);
	ChildBuilder.AddProperty(ServerNameHandle.ToSharedRef());
}

void FCrowdyServerListValueNameCustomization::AddPickerRow(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& Handle,
	TSharedRef<SWidget> (FCrowdyServerListValueNameCustomization::*MakeMenu)(), FText (FCrowdyServerListValueNameCustomization::*GetText)() const)
{
	ChildBuilder.AddCustomRow(Handle->GetPropertyDisplayName())
		.NameContent()
		[
			Handle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(125.0f)
		.MaxDesiredWidth(400.0f)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, MakeMenu)
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text(this, GetText)
			]
		];
}

FName FCrowdyServerListValueNameCustomization::GetPickedList() const
{
	FName List;
	return ListHandle.IsValid() && ListHandle->GetValue(List) == FPropertyAccess::Success ? List : NAME_None;
}

FGuid FCrowdyServerListValueNameCustomization::GetPickedValueId() const
{
	FString Text;
	FGuid ValueId;
	if (ValueIdHandle.IsValid() && ValueIdHandle->GetValueAsFormattedString(Text) == FPropertyAccess::Success)
	{
		FGuid::Parse(Text, ValueId);
	}
	return ValueId;
}

const FInstancedPropertyBag* FCrowdyServerListValueNameCustomization::FindPickedListValues() const
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const FName Picked = GetPickedList();
	if (!Current || Picked.IsNone())
	{
		return nullptr;
	}
	TArray<FCrowdyServerNamedList> Lists;
	Current->GetLists(Lists);
	const FCrowdyServerNamedList* Found = Lists.FindByPredicate([Picked](const FCrowdyServerNamedList& List) { return FName(*List.Name) == Picked; });
	return Found ? Found->Values : nullptr;
}

TSharedRef<SWidget> FCrowdyServerListValueNameCustomization::MakeListMenu()
{
	TArray<FCrowdyServerNamedList> Lists;
	if (const UCrowdyServerObjectDefinition* Current = Definition.Get())
	{
		Current->GetLists(Lists);
	}
	FMenuBuilder Menu(true, nullptr);
	if (Lists.IsEmpty())
	{
		CrowdyServerListValueNameCustomizationDetail::AddDisabledEntry(Menu, LOCTEXT("NoLists", "This definition has no Lists"));
		return Menu.MakeWidget();
	}
	// Lists sharing a struct are baked under the first one's name, so only that one is offered.
	TSet<const UPropertyBag*> Offered;
	for (const FCrowdyServerNamedList& List : Lists)
	{
		bool bShared = false;
		Offered.Add(List.Values ? List.Values->GetPropertyBagStruct() : nullptr, &bShared);
		if (bShared)
		{
			continue;
		}
		Menu.AddMenuEntry(FText::FromString(List.Name), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerListValueNameCustomization::PickList, FName(*List.Name))));
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> FCrowdyServerListValueNameCustomization::MakeValueMenu()
{
	const FInstancedPropertyBag* Values = FindPickedListValues();
	const UPropertyBag* Bag = Values ? Values->GetPropertyBagStruct() : nullptr;
	FMenuBuilder Menu(true, nullptr);
	if (!Bag || Bag->GetPropertyDescs().IsEmpty())
	{
		CrowdyServerListValueNameCustomizationDetail::AddDisabledEntry(Menu, Values ? LOCTEXT("NoValues", "The List has no values") : LOCTEXT("ChooseList", "Choose the List first"));
		return Menu.MakeWidget();
	}
	for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
	{
		Menu.AddMenuEntry(FText::FromName(Desc.Name), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerListValueNameCustomization::PickValue, Desc.ID)));
	}
	return Menu.MakeWidget();
}

FText FCrowdyServerListValueNameCustomization::GetListText() const
{
	const FName Picked = GetPickedList();
	if (Picked.IsNone())
	{
		return LOCTEXT("NoList", "None");
	}
	return FindPickedListValues() ? FText::FromName(Picked) : FText::Format(LOCTEXT("MissingList", "{0} (missing)"), FText::FromName(Picked));
}

FText FCrowdyServerListValueNameCustomization::GetValueText() const
{
	const FGuid ValueId = GetPickedValueId();
	if (!ValueId.IsValid())
	{
		return LOCTEXT("NoValue", "None");
	}
	const FInstancedPropertyBag* Values = FindPickedListValues();
	const FPropertyBagPropertyDesc* Desc = Values ? Values->FindPropertyDescByID(ValueId) : nullptr;
	// A value is kept by its id alone, so a removed one has no name left to show.
	return Desc ? FText::FromName(Desc->Name) : LOCTEXT("MissingValue", "Unknown value (missing)");
}

void FCrowdyServerListValueNameCustomization::PickList(FName List)
{
	if (ListHandle.IsValid())
	{
		ListHandle->SetValue(List);
	}
}

void FCrowdyServerListValueNameCustomization::PickValue(FGuid ValueId)
{
	if (ValueIdHandle.IsValid())
	{
		ValueIdHandle->SetValueFromFormattedString(ValueId.ToString());
	}
}

#undef LOCTEXT_NAMESPACE
