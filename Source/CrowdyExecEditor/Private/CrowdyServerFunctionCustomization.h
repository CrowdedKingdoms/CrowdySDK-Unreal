#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
class SWidget;

/** Rows a function's and a message's header share. */
namespace CrowdyServerMemberRows
{
	/** Label in subdued text, then Value, under one tooltip. */
	TSharedRef<SWidget> Labelled(const FText& Label, const FText& ToolTip, const TSharedRef<SWidget>& Value);
}

/** A Server Function as one collapsible row: its name and Callable By in its header, its inputs and outputs below. */
class FCrowdyServerFunctionCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, class IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils) override;
};
