#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IDetailChildrenBuilder;
class IPropertyHandle;
class SWidget;
class UCrowdyServerObjectDefinition;
struct FInstancedPropertyBag;

/** A List Value Names entry whose List and value are picked from the definition's Lists rather than typed; the value is stored by its id. */
class FCrowdyServerListValueNameCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils) override;

private:
	void AddPickerRow(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& Handle, TSharedRef<SWidget> (FCrowdyServerListValueNameCustomization::*MakeMenu)(),
		FText (FCrowdyServerListValueNameCustomization::*GetText)() const);
	FName GetPickedList() const;
	FGuid GetPickedValueId() const;
	const FInstancedPropertyBag* FindPickedListValues() const;
	TSharedRef<SWidget> MakeListMenu();
	TSharedRef<SWidget> MakeValueMenu();
	FText GetListText() const;
	FText GetValueText() const;
	void PickList(FName List);
	void PickValue(FGuid ValueId);

	TWeakObjectPtr<const UCrowdyServerObjectDefinition> Definition;
	TSharedPtr<IPropertyHandle> ListHandle;
	TSharedPtr<IPropertyHandle> ValueIdHandle;
};
