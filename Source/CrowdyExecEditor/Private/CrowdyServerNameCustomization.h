#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
class SWidget;

/** A Server Names override whose field or enum value is picked from the chosen struct or enum rather than typed. */
class FCrowdyServerNameCustomization : public IPropertyTypeCustomization
{
public:
	/** Lists what can be picked from Owner (the chosen struct or enum): the names to store and how to show them. */
	using FListOptions = TFunction<void(const UObject* Owner, TArray<FName>& OutNames, TArray<FText>& OutLabels)>;

	/** For FCrowdyServerFieldName: Field is picked from Struct's fields. */
	static TSharedRef<IPropertyTypeCustomization> MakeFieldInstance();
	/** For FCrowdyServerEnumValueName: Value is picked from Enum's values. */
	static TSharedRef<IPropertyTypeCustomization> MakeEnumValueInstance();

	FCrowdyServerNameCustomization(FName InOwnerMember, FName InPickedMember, const FText& InNoOwnerText, FListOptions InListOptions);

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, class IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils) override;

private:
	TSharedRef<SWidget> MakeMenu();
	FText GetPickedText() const;
	void Pick(FName Name);

	FName OwnerMember;
	FName PickedMember;
	FText NoOwnerText;
	FListOptions ListOptions;
	TSharedPtr<IPropertyHandle> OwnerHandle;
	TSharedPtr<IPropertyHandle> PickedHandle;
};
