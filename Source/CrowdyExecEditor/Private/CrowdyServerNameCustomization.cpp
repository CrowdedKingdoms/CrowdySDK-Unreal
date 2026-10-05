#include "CrowdyServerNameCustomization.h"

#include "CrowdyServerObjectDefinition.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/UserDefinedEnum.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerNameCustomization"

namespace CrowdyServerNameCustomizationDetail
{
	void ListStructFields(const UObject* Owner, TArray<FName>& OutNames, TArray<FText>& OutLabels)
	{
		const UScriptStruct* Struct = Cast<UScriptStruct>(Owner);
		if (!Struct)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			OutNames.Add(FName(*It->GetAuthoredName()));
			OutLabels.Add(It->GetDisplayNameText());
		}
	}

	/** Named the way the definition names a value: the display name for a Blueprint enum, the short name for C++; hidden values and the generated _MAX left out. */
	void ListEnumValues(const UObject* Owner, TArray<FName>& OutNames, TArray<FText>& OutLabels)
	{
		const UEnum* Enum = Cast<UEnum>(Owner);
		if (!Enum)
		{
			return;
		}
		const int32 Count = Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
			const FText Label = Enum->GetDisplayNameTextByIndex(Index);
			OutNames.Add(FName(Cast<UUserDefinedEnum>(Enum) ? Label.ToString() : Enum->GetNameStringByIndex(Index)));
			OutLabels.Add(Label);
		}
	}
}

TSharedRef<IPropertyTypeCustomization> FCrowdyServerNameCustomization::MakeFieldInstance()
{
	return MakeShared<FCrowdyServerNameCustomization>(GET_MEMBER_NAME_CHECKED(FCrowdyServerFieldName, Struct), GET_MEMBER_NAME_CHECKED(FCrowdyServerFieldName, Field),
		LOCTEXT("ChooseStruct", "Choose the struct first"), &CrowdyServerNameCustomizationDetail::ListStructFields);
}

TSharedRef<IPropertyTypeCustomization> FCrowdyServerNameCustomization::MakeEnumValueInstance()
{
	return MakeShared<FCrowdyServerNameCustomization>(GET_MEMBER_NAME_CHECKED(FCrowdyServerEnumValueName, Enum), GET_MEMBER_NAME_CHECKED(FCrowdyServerEnumValueName, Value),
		LOCTEXT("ChooseEnum", "Choose the enum first"), &CrowdyServerNameCustomizationDetail::ListEnumValues);
}

FCrowdyServerNameCustomization::FCrowdyServerNameCustomization(FName InOwnerMember, FName InPickedMember, const FText& InNoOwnerText, FListOptions InListOptions)
	: OwnerMember(InOwnerMember)
	, PickedMember(InPickedMember)
	, NoOwnerText(InNoOwnerText)
	, ListOptions(MoveTemp(InListOptions))
{
}

void FCrowdyServerNameCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	HeaderRow.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	];
}

void FCrowdyServerNameCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	OwnerHandle = PropertyHandle->GetChildHandle(OwnerMember);
	uint32 Count = 0;
	PropertyHandle->GetNumChildren(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<IPropertyHandle> Child = PropertyHandle->GetChildHandle(Index);
		if (!Child.IsValid())
		{
			continue;
		}
		if (!Child->GetProperty() || Child->GetProperty()->GetFName() != PickedMember)
		{
			ChildBuilder.AddProperty(Child.ToSharedRef());
			continue;
		}
		PickedHandle = Child;
		ChildBuilder.AddCustomRow(Child->GetPropertyDisplayName())
			.NameContent()
			[
				Child->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(125.0f)
			.MaxDesiredWidth(400.0f)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &FCrowdyServerNameCustomization::MakeMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(this, &FCrowdyServerNameCustomization::GetPickedText)
				]
			];
	}
}

TSharedRef<SWidget> FCrowdyServerNameCustomization::MakeMenu()
{
	UObject* Owner = nullptr;
	const bool bHasOwner = OwnerHandle.IsValid() && OwnerHandle->GetValue(Owner) == FPropertyAccess::Success && Owner;
	TArray<FName> Names;
	TArray<FText> Labels;
	if (bHasOwner)
	{
		ListOptions(Owner, Names, Labels);
	}
	FMenuBuilder Menu(true, nullptr);
	if (Names.IsEmpty())
	{
		Menu.AddMenuEntry(bHasOwner ? LOCTEXT("NothingToPick", "Nothing to pick") : NoOwnerText, FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return Menu.MakeWidget();
	}
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		Menu.AddMenuEntry(Labels[Index], FText::FromName(Names[Index]), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerNameCustomization::Pick, Names[Index])));
	}
	return Menu.MakeWidget();
}

FText FCrowdyServerNameCustomization::GetPickedText() const
{
	FName Picked;
	if (!PickedHandle.IsValid() || PickedHandle->GetValue(Picked) != FPropertyAccess::Success)
	{
		return LOCTEXT("MultipleValues", "Multiple Values");
	}
	if (Picked.IsNone())
	{
		return LOCTEXT("PickedNone", "None");
	}
	UObject* Owner = nullptr;
	TArray<FName> Names;
	TArray<FText> Labels;
	if (OwnerHandle.IsValid() && OwnerHandle->GetValue(Owner) == FPropertyAccess::Success && Owner)
	{
		ListOptions(Owner, Names, Labels);
	}
	const int32 Found = Names.IndexOfByKey(Picked);
	return Found != INDEX_NONE ? Labels[Found] : FText::Format(LOCTEXT("PickedMissing", "{0} (missing)"), FText::FromName(Picked));
}

void FCrowdyServerNameCustomization::Pick(FName Name)
{
	if (PickedHandle.IsValid())
	{
		PickedHandle->SetValue(Name);
	}
}

#undef LOCTEXT_NAMESPACE
