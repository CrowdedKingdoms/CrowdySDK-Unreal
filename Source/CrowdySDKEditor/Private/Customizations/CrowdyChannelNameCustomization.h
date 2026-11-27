#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyEditorModule.h"

/** The meta key that gives an FString property the Pick button: UPROPERTY(EditAnywhere, meta = (CrowdyChannelPicker)). */
inline constexpr const TCHAR* CrowdyChannelPickerMetaKey = TEXT("CrowdyChannelPicker");

/** Applies the channel name row only to FString properties that carry the CrowdyChannelPicker meta. */
class FCrowdyChannelNameTypeIdentifier final : public IPropertyTypeIdentifier
{
public:
	virtual bool IsPropertyTypeCustomized(const IPropertyHandle& PropertyHandle) const override;
};

/** An FString that names a channel: the usual text box, so any name can be typed, plus a Pick button listing the app's channels. */
class FCrowdyChannelNameCustomization final : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override
	{
	}
};
