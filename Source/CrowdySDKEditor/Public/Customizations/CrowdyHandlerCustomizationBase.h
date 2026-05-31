#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "EdGraphSchema_K2.h"
#include "Widgets/Input/SCheckBox.h"

class IDetailLayoutBuilder;
class IDetailCategoryBuilder;
class UEdGraphPin;

class FCrowdyHandlerCustomizationBase : public IDetailCustomization
{
protected:
	virtual bool IsStamped() const = 0;
	virtual void SetStamped(bool bStamped) = 0;

	virtual const TArray<UEdGraphPin*>& GetNodePins() const = 0;
	virtual int32 GetNodeFunctionFlags() const = 0;
	virtual FString GetNodeDisplayName() const = 0;

	void BuildCrowdyCategory(IDetailLayoutBuilder& DetailBuilder);

	struct FPinInspectionResult
	{
		int32 InputPinCount = 0;
		bool bIsPrivate = false;
		bool bIsProtected = false;
		bool bHasUnrealReplication = false;
		bool bHasStructPin = false;
		bool bHasValidCrowdyRep = false;
		FString StructTypeName;
		FString CrowdyRepValue;

		bool IsValid() const
		{
			return !bIsPrivate
				&& !bIsProtected
				&& !bHasUnrealReplication
				&& InputPinCount == 1
				&& bHasStructPin
				&& bHasValidCrowdyRep;
		}
	};

	FPinInspectionResult InspectPins() const;

private:
	ECheckBoxState GetCheckState() const;
	void OnCheckStateChanged(ECheckBoxState NewState);

	FText GetStatusText() const;
	FSlateColor GetStatusColor() const;

	FText GetValidationText() const;
	FSlateColor GetValidationColor() const;
	EVisibility GetValidationVisibility() const;
};
