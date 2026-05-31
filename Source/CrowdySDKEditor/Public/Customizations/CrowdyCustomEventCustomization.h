#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyHandlerCustomizationBase.h"
#include "K2Node_CustomEvent.h"

class FCrowdyCustomEventCustomization : public FCrowdyHandlerCustomizationBase
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCrowdyCustomEventCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

protected:
	virtual bool IsStamped() const override;
	virtual void SetStamped(bool bStamped) override;
	virtual const TArray<UEdGraphPin*>& GetNodePins() const override;
	virtual int32 GetNodeFunctionFlags() const override;
	virtual FString GetNodeDisplayName() const override;

private:
	TWeakObjectPtr<UK2Node_CustomEvent> EditedNode;
};
