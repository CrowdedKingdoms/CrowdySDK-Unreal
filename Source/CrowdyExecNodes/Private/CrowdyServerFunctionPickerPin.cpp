#include "CrowdyServerFunctionPickerPin.h"

#include "CrowdyK2Node_CallServerFunction.h"
#include "CrowdyServerObjectDefinition.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerFunctionPickerPin"

namespace CrowdyServerFunctionPickerPinPrivate
{
	UCrowdyK2Node_CallServerFunction* FindPickerNode(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->Direction != EGPD_Input || Pin->PinName != CrowdyServerCallPins::FunctionPicker)
		{
			return nullptr;
		}
		return Cast<UCrowdyK2Node_CallServerFunction>(Pin->GetOwningNodeUnchecked());
	}
}

void SCrowdyServerFunctionPickerPin::Construct(const FArguments& InArgs, UEdGraphPin* InPin)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InPin);
}

TSharedRef<SWidget> SCrowdyServerFunctionPickerPin::GetDefaultValueWidget()
{
	return SNew(SComboButton)
		.ContentPadding(FMargin(6.f, 2.f))
		.IsEnabled_Lambda([this]() { return GraphPinObj && !GraphPinObj->bDefaultValueIsReadOnly; })
		.OnGetMenuContent(this, &SCrowdyServerFunctionPickerPin::BuildMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyServerFunctionPickerPin::GetCurrentText)
			.Font(FAppStyle::GetFontStyle(TEXT("Graph.Node.PinName")))
		];
}

FText SCrowdyServerFunctionPickerPin::GetCurrentText() const
{
	const UCrowdyK2Node_CallServerFunction* Node = CrowdyServerFunctionPickerPinPrivate::FindPickerNode(GraphPinObj);
	return Node ? Node->GetPickedFunctionText() : FText::GetEmpty();
}

TSharedRef<SWidget> SCrowdyServerFunctionPickerPin::BuildMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);
	TArray<UCrowdyServerObjectDefinition*> Definitions;
	for (TObjectIterator<UCrowdyServerObjectDefinition> It; It; ++It)
	{
		if (IsValid(*It) && It->IsAsset())
		{
			Definitions.Add(*It);
		}
	}
	Definitions.Sort([](const UCrowdyServerObjectDefinition& A, const UCrowdyServerObjectDefinition& B) { return A.GetName() < B.GetName(); });

	if (Definitions.Num() == 0)
	{
		MenuBuilder.AddMenuEntry(LOCTEXT("NoDefinitions", "No Server Object definition is loaded"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	for (UCrowdyServerObjectDefinition* Definition : Definitions)
	{
		TArray<FName> Functions;
		UCrowdyK2Node_ServerObjectBase::GatherFunctionNames(Definition, Functions);
		MenuBuilder.BeginSection(NAME_None, FText::FromString(Definition->GetName()));
		for (const FName Function : Functions)
		{
			MenuBuilder.AddMenuEntry(FText::FromName(Function), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerFunctionPickerPin::OnPicked, MakeWeakObjectPtr(Definition), Function)));
		}
		MenuBuilder.EndSection();
	}
	return MenuBuilder.MakeWidget();
}

void SCrowdyServerFunctionPickerPin::OnPicked(TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition, FName Function)
{
	UCrowdyK2Node_CallServerFunction* Node = CrowdyServerFunctionPickerPinPrivate::FindPickerNode(GraphPinObj);
	if (!Node || !Definition.IsValid())
	{
		return;
	}
	Node->SetPickedFunction(Definition.Get(), Function);
}

TSharedPtr<SGraphPin> FCrowdyServerFunctionPickerPinFactory::CreatePin(UEdGraphPin* InPin) const
{
	if (!CrowdyServerFunctionPickerPinPrivate::FindPickerNode(InPin))
	{
		return nullptr;
	}
	return SNew(SCrowdyServerFunctionPickerPin, InPin);
}

#undef LOCTEXT_NAMESPACE
