#include "Customizations/CrowdyHandlerCustomizationBase.h"

#include "CrowdySDKEditor.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraph/EdGraphPin.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	bool IsCrowdyHandlerPayloadPin(const UEdGraphPin* Pin)
	{
		if (!Pin) return false;
		if (Pin->ParentPin) return false;
		if (Pin->Direction != EGPD_Output) return false;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Schema && Schema->IsMetaPin(*Pin)) return false;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) return false;

		return true;
	}
}

void FCrowdyHandlerCustomizationBase::BuildCrowdyCategory(
	IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(
			"CrowdySDK",
			FText::FromString(TEXT("Crowdy SDK")),
			ECategoryPriority::Important);

	Category.AddCustomRow(FText::FromString(TEXT("Crowdy Event Handler")))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Crowdy Event Handler")))
		.Font(DetailBuilder.GetDetailFont())
		.ToolTipText(FText::FromString(
			TEXT("Marks this function as a Crowdy event handler.\n\n"
			     "Requirements:\n"
			     "  - Exactly one input parameter\n"
			     "  - Parameter must be a struct tagged with\n"
			     "    meta=(CrowdyRep=\"Event\") or\n"
			     "    meta=(CrowdyRep=\"ActorUpdate\")\n\n"
			     "Recompile the Blueprint after toggling to transfer the stamp\n"
			     "onto the generated UFunction.")))
	]
	.ValueContent()
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked(this, &FCrowdyHandlerCustomizationBase::GetCheckState)
			.OnCheckStateChanged(
				this, &FCrowdyHandlerCustomizationBase::OnCheckStateChanged)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.f, 0.f, 0.f, 0.f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &FCrowdyHandlerCustomizationBase::GetStatusText)
			.ColorAndOpacity(this, &FCrowdyHandlerCustomizationBase::GetStatusColor)
			.Font(DetailBuilder.GetDetailFont())
		]
	];

	Category.AddCustomRow(FText::FromString(TEXT("Signature Validation")))
	.NameContent()
	[
		SNew(SBox)
		.Visibility(this, &FCrowdyHandlerCustomizationBase::GetValidationVisibility)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Signature")))
			.Font(DetailBuilder.GetDetailFont())
		]
	]
	.ValueContent()
	[
		SNew(SBox)
		.Visibility(this, &FCrowdyHandlerCustomizationBase::GetValidationVisibility)
		[
			SNew(STextBlock)
			.Text(this, &FCrowdyHandlerCustomizationBase::GetValidationText)
			.ColorAndOpacity(this, &FCrowdyHandlerCustomizationBase::GetValidationColor)
			.Font(DetailBuilder.GetDetailFont())
			.AutoWrapText(true)
		]
	];
}

ECheckBoxState FCrowdyHandlerCustomizationBase::GetCheckState() const
{
	return IsStamped() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyHandlerCustomizationBase::OnCheckStateChanged(ECheckBoxState NewState)
{
	SetStamped(NewState == ECheckBoxState::Checked);
}

FText FCrowdyHandlerCustomizationBase::GetStatusText() const
{
	if (!IsStamped())
	{
		return FText::FromString(TEXT("Not a Crowdy handler"));
	}

	return InspectPins().IsValid()
		? FText::FromString(TEXT("Valid"))
		: FText::FromString(TEXT("Signature invalid"));
}

FSlateColor FCrowdyHandlerCustomizationBase::GetStatusColor() const
{
	if (!IsStamped())
	{
		return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
	}

	return InspectPins().IsValid()
		? FSlateColor(FLinearColor(0.2f, 0.9f, 0.4f))
		: FSlateColor(FLinearColor(0.9f, 0.5f, 0.1f));
}

FText FCrowdyHandlerCustomizationBase::GetValidationText() const
{
	const FPinInspectionResult Result = InspectPins();

	if (Result.bIsPrivate)
	{
		return FText::FromString(
			TEXT("Handler must be public - private Blueprint handlers cannot be called externally for replication"));
	}

	if (Result.bIsProtected)
	{
		return FText::FromString(
			TEXT("Handler must be public - protected Blueprint handlers cannot be called externally for replication"));
	}

	if (Result.bHasUnrealReplication)
	{
		return FText::FromString(
			TEXT("Disable Unreal replication on this handler - Crowdy replication and Unreal replication cannot both be enabled"));
	}

	if (Result.InputPinCount == 0)
	{
		return FText::FromString(TEXT("No parameters - add one struct input pin"));
	}

	if (Result.InputPinCount > 1)
	{
		return FText::FromString(FString::Printf(
			TEXT("Too many parameters (%d) - exactly one required"),
			Result.InputPinCount));
	}

	if (!Result.bHasStructPin)
	{
		return FText::FromString(
			TEXT("Parameter must be a struct tagged with CrowdyRep=\"Event\" or CrowdyRep=\"ActorUpdate\""));
	}

	if (!Result.bHasValidCrowdyRep)
	{
		if (Result.CrowdyRepValue.IsEmpty())
		{
			return FText::FromString(FString::Printf(
				TEXT("Struct '%s' is missing CrowdyRep"),
				*Result.StructTypeName));
		}

		return FText::FromString(FString::Printf(
			TEXT("Struct '%s' has unsupported CrowdyRep=\"%s\""),
			*Result.StructTypeName,
			*Result.CrowdyRepValue));
	}

	return FText::FromString(FString::Printf(
		TEXT("OK - receives '%s'"),
		*Result.StructTypeName));
}

FSlateColor FCrowdyHandlerCustomizationBase::GetValidationColor() const
{
	return InspectPins().IsValid()
		? FSlateColor(FLinearColor(0.2f, 0.9f, 0.4f))
		: FSlateColor(FLinearColor(0.9f, 0.3f, 0.2f));
}

EVisibility FCrowdyHandlerCustomizationBase::GetValidationVisibility() const
{
	return IsStamped()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FCrowdyHandlerCustomizationBase::FPinInspectionResult
FCrowdyHandlerCustomizationBase::InspectPins() const
{
	FPinInspectionResult Result;

	const int32 FunctionFlags = GetNodeFunctionFlags();
	Result.bIsPrivate = (FunctionFlags & FUNC_Private) != 0;
	Result.bIsProtected = (FunctionFlags & FUNC_Protected) != 0;
	Result.bHasUnrealReplication = (FunctionFlags & FUNC_NetFuncFlags) != 0;

	for (const UEdGraphPin* Pin : GetNodePins())
	{
		if (!IsCrowdyHandlerPayloadPin(Pin)) continue;

		++Result.InputPinCount;

		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			Result.bHasStructPin = true;

			const UScriptStruct* Struct = Cast<UScriptStruct>(
				Pin->PinType.PinSubCategoryObject.Get());
			Result.StructTypeName = Struct
				? Struct->GetName()
				: TEXT("Unknown");

			if (Struct)
			{
				Result.CrowdyRepValue = Struct->GetMetaData(CrowdyMetaKeys::CrowdyRep);
				Result.bHasValidCrowdyRep =
					Result.CrowdyRepValue == TEXT("Event")
					|| Result.CrowdyRepValue == TEXT("ActorUpdate");
			}
		}
	}

	return Result;
}
