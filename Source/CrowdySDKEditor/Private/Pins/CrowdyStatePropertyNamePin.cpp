#include "Pins/CrowdyStatePropertyNamePin.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Replication/State/CrowdyStateBlueprintLibrary.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStatePropertyNamePin"

namespace
{
	const FName GPropertyNamePinName(TEXT("PropertyName"));
}

void SCrowdyStatePropertyNamePin::Construct(const FArguments& InArgs, UEdGraphPin* InPin)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InPin);
}

TSharedRef<SWidget> SCrowdyStatePropertyNamePin::GetDefaultValueWidget()
{
	return SNew(SComboButton)
		.ContentPadding(FMargin(6.f, 2.f))
		.IsEnabled_Lambda([this]() { return GraphPinObj && !GraphPinObj->bDefaultValueIsReadOnly; })
		.OnGetMenuContent(this, &SCrowdyStatePropertyNamePin::BuildPickerMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyStatePropertyNamePin::GetCurrentValueText)
			.Font(FAppStyle::GetFontStyle(TEXT("Graph.Node.PinName")))
		];
}

FText SCrowdyStatePropertyNamePin::GetCurrentValueText() const
{
	if (!GraphPinObj)
	{
		return FText::GetEmpty();
	}
	const FString Value = GraphPinObj->GetDefaultAsString();
	return Value.IsEmpty() ? LOCTEXT("NoneValue", "(None)") : FText::FromString(Value);
}

UClass* SCrowdyStatePropertyNamePin::ResolveTargetClass() const
{
	if (!GraphPinObj)
	{
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(GraphPinObj->GetOwningNodeUnchecked());
	if (!CallNode)
	{
		return nullptr;
	}

	// The Target pin keeps its declared parameter name; read the function's DefaultToSelf meta to find it
	// rather than assuming the literal "Target", so a later rename can't silently break the picker.
	FName TargetPinName(TEXT("Target"));
	if (const UFunction* Function = CallNode->GetTargetFunction())
	{
		const FString SelfParam = Function->GetMetaData(TEXT("DefaultToSelf"));
		if (!SelfParam.IsEmpty())
		{
			TargetPinName = FName(*SelfParam);
		}
	}

	if (const UEdGraphPin* TargetPin = CallNode->FindPin(TargetPinName, EGPD_Input))
	{
		if (TargetPin->LinkedTo.Num() > 0)
		{
			// Target is explicitly wired: resolve the linked reference's class. If it can't be classified (a
			// wildcard/unresolved pin), return null for an honest empty state rather than falling back to self,
			// which would list THIS Blueprint's properties for someone else's actor.
			const UEdGraphPin* SourcePin = TargetPin->LinkedTo[0];
			return SourcePin ? Cast<UClass>(SourcePin->PinType.PinSubCategoryObject.Get()) : nullptr;
		}
	}

	// Unconnected DefaultToSelf targets this Blueprint. The skeleton class reflects variables added since the
	// last compile, so a just-added CrowdyManualDirty variable appears in the dropdown before recompiling.
	if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(CallNode))
	{
		if (Blueprint->SkeletonGeneratedClass)
		{
			return Blueprint->SkeletonGeneratedClass;
		}
		return Blueprint->GeneratedClass;
	}

	return nullptr;
}

void SCrowdyStatePropertyNamePin::GatherManualDirtyPropertyNames(const UClass* Class, TArray<FName>& Out)
{
	if (!Class)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		const FProperty* Property = *It;
		if (CrowdyStateMetaKeys::HasStateMeta(Property)
			&& Property->HasMetaData(CrowdyStateMetaKeys::ManualDirty)
			&& FCrowdyStateLayoutBuilder::IsStateReplicatable(Property))
		{
			Out.AddUnique(Property->GetFName());
		}
	}
}

TSharedRef<SWidget> SCrowdyStatePropertyNamePin::BuildPickerMenu()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("NoneEntry", "(None)"),
		LOCTEXT("NoneEntryTip", "Clear the property name."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SCrowdyStatePropertyNamePin::OnPropertySelected, FName())));

	UClass* TargetClass = ResolveTargetClass();
	if (!TargetClass)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("NoTargetEntry", "Connect a Target, or use this node in an Actor Blueprint"),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	if (!TargetClass->IsChildOf(AActor::StaticClass()))
	{
		// Self resolved to a non-actor (e.g. a Component Blueprint): an AActor* Target cannot default to self
		// here, so point the author at the fix rather than showing an empty or misleading list.
		MenuBuilder.AddMenuEntry(
			LOCTEXT("NonActorSelfEntry", "Wire an Actor into Target — self is not an Actor here"),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	TArray<FName> Names;
	GatherManualDirtyPropertyNames(TargetClass, Names);
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	if (Names.Num() == 0)
	{
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("NoPropsEntry", "No Crowdy manual-dirty properties on {0}"),
				FText::FromString(TargetClass->GetName())),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.BeginSection(
		NAME_None,
		FText::Format(
			LOCTEXT("PropsHeader", "{0} — Manual-Dirty Properties"),
			FText::FromString(TargetClass->GetName())));
	for (const FName& Name : Names)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromName(Name),
			FText::GetEmpty(),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SCrowdyStatePropertyNamePin::OnPropertySelected, Name)));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void SCrowdyStatePropertyNamePin::OnPropertySelected(FName PropertyName)
{
	if (!GraphPinObj)
	{
		return;
	}

	const FString NewValue = PropertyName.IsNone() ? FString() : PropertyName.ToString();
	if (GraphPinObj->GetDefaultAsString() == NewValue)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetCrowdyStateProperty", "Set Crowdy State Property"));
	GraphPinObj->Modify();
	if (const UEdGraphSchema* Schema = GraphPinObj->GetSchema())
	{
		Schema->TrySetDefaultValue(*GraphPinObj, NewValue);
	}
	else
	{
		GraphPinObj->DefaultValue = NewValue;
	}
}

TSharedPtr<SGraphPin> FCrowdyStatePropertyPinFactory::CreatePin(UEdGraphPin* InPin) const
{
	if (!InPin || InPin->Direction != EGPD_Input)
	{
		return nullptr;
	}
	if (InPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Name || InPin->PinName != GPropertyNamePinName)
	{
		return nullptr;
	}

	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(InPin->GetOwningNodeUnchecked());
	if (!CallNode)
	{
		return nullptr;
	}

	const UFunction* Function = CallNode->GetTargetFunction();
	if (!Function || Function->GetOwnerClass() != UCrowdyStateBlueprintLibrary::StaticClass())
	{
		return nullptr;
	}

	static const FName MarkFunctionName = GET_FUNCTION_NAME_CHECKED(UCrowdyStateBlueprintLibrary, MarkCrowdyStateDirty);
	if (Function->GetFName() != MarkFunctionName)
	{
		return nullptr;
	}

	return SNew(SCrowdyStatePropertyNamePin, InPin);
}

#undef LOCTEXT_NAMESPACE
