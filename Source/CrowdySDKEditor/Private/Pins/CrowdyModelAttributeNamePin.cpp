#include "Pins/CrowdyModelAttributeNamePin.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyModel.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "CrowdyModelAttributeNamePin"

namespace
{
	const FName GKeyPinName(TEXT("Key"));
	const FName GEntityPinName(TEXT("Entity"));
}

FString SCrowdyModelAttributeNamePin::ValueTypeForGetter(FName GetterFunctionName)
{
	static const FName IntName = GET_FUNCTION_NAME_CHECKED(UCrowdyModel, GetInt);
	static const FName FloatName = GET_FUNCTION_NAME_CHECKED(UCrowdyModel, GetFloat);
	static const FName BoolName = GET_FUNCTION_NAME_CHECKED(UCrowdyModel, GetBool);
	static const FName StringName = GET_FUNCTION_NAME_CHECKED(UCrowdyModel, GetString);

	if (GetterFunctionName == IntName)
	{
		return TEXT("int");
	}
	if (GetterFunctionName == FloatName)
	{
		return TEXT("float");
	}
	if (GetterFunctionName == BoolName)
	{
		return TEXT("bool");
	}
	if (GetterFunctionName == StringName)
	{
		return TEXT("string");
	}
	return FString();
}

TArray<FName> SCrowdyModelAttributeNamePin::ComputeMatchingKeys(const UClass* Class, const FString& WantedValueType)
{
	TArray<FName> Keys;
	if (!Class || WantedValueType.IsEmpty())
	{
		return Keys;
	}

	for (const FCrowdyAttributeDef& Def : FCrowdyAttributeRegistry::DiscoverForClass(Class))
	{
		if (Def.ValueType == WantedValueType)
		{
			Keys.AddUnique(FName(*Def.Key));
		}
	}
	Keys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Keys;
}

void SCrowdyModelAttributeNamePin::Construct(const FArguments& InArgs, UEdGraphPin* InPin)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InPin);
}

TSharedRef<SWidget> SCrowdyModelAttributeNamePin::GetDefaultValueWidget()
{
	// A free-text box plus a dropdown arrow: typing commits any key (the picker never restricts to the discovered
	// set), while the arrow lists the target class's matching Server Owned keys for discoverability.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.Text(this, &SCrowdyModelAttributeNamePin::GetCurrentValueText)
			.Font(FAppStyle::GetFontStyle(TEXT("Graph.Node.PinName")))
			.SelectAllTextWhenFocused(true)
			.IsReadOnly_Lambda([this]() { return !GraphPinObj || GraphPinObj->bDefaultValueIsReadOnly; })
			.OnTextCommitted(this, &SCrowdyModelAttributeNamePin::OnKeyTextCommitted)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SComboButton)
			.ContentPadding(FMargin(2.f, 2.f))
			.HasDownArrow(true)
			.IsEnabled_Lambda([this]() { return GraphPinObj && !GraphPinObj->bDefaultValueIsReadOnly; })
			.OnGetMenuContent(this, &SCrowdyModelAttributeNamePin::BuildPickerMenu)
		];
}

FText SCrowdyModelAttributeNamePin::GetCurrentValueText() const
{
	if (!GraphPinObj)
	{
		return FText::GetEmpty();
	}
	return FText::FromString(GraphPinObj->GetDefaultAsString());
}

FString SCrowdyModelAttributeNamePin::ResolveWantedValueType() const
{
	if (!GraphPinObj)
	{
		return FString();
	}
	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(GraphPinObj->GetOwningNodeUnchecked());
	if (!CallNode)
	{
		return FString();
	}
	const UFunction* Function = CallNode->GetTargetFunction();
	return Function ? ValueTypeForGetter(Function->GetFName()) : FString();
}

UClass* SCrowdyModelAttributeNamePin::ResolveTargetClass() const
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

	// The getters take a plain const UObject* Entity (no DefaultToSelf), so the entity pin is the literal "Entity".
	if (const UEdGraphPin* EntityPin = CallNode->FindPin(GEntityPinName, EGPD_Input))
	{
		if (EntityPin->LinkedTo.Num() > 0)
		{
			// Entity is explicitly wired: resolve the linked reference's class. If it can't be classified (a
			// wildcard/unresolved pin), return null for an honest empty state rather than falling back to self,
			// which would list THIS Blueprint's keys for someone else's entity.
			const UEdGraphPin* SourcePin = EntityPin->LinkedTo[0];
			return SourcePin ? Cast<UClass>(SourcePin->PinType.PinSubCategoryObject.Get()) : nullptr;
		}
	}

	// Unconnected Entity reads as the Blueprint hosting this node. The skeleton class reflects the current class
	// even between compiles, so a container Blueprint's keys appear without a recompile.
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

TSharedRef<SWidget> SCrowdyModelAttributeNamePin::BuildPickerMenu()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("NoneEntry", "(None)"),
		LOCTEXT("NoneEntryTip", "Clear the attribute key."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SCrowdyModelAttributeNamePin::OnKeySelected, FName())));

	UClass* TargetClass = ResolveTargetClass();
	if (!TargetClass)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("NoTargetEntry", "Connect an Entity, or use this node in a container Blueprint"),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	const FString WantedValueType = ResolveWantedValueType();
	TArray<FName> Keys = ComputeMatchingKeys(TargetClass, WantedValueType);

	if (Keys.Num() == 0)
	{
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("NoKeysEntry", "No Server Owned {0} attributes on {1}"),
				FText::FromString(WantedValueType.IsEmpty() ? TEXT("matching") : WantedValueType),
				FText::FromString(TargetClass->GetName())),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.BeginSection(
		NAME_None,
		FText::Format(
			LOCTEXT("KeysHeader", "{0} - Server Owned {1} Attributes"),
			FText::FromString(TargetClass->GetName()),
			FText::FromString(WantedValueType)));
	for (const FName& Key : Keys)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromName(Key),
			FText::GetEmpty(),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SCrowdyModelAttributeNamePin::OnKeySelected, Key)));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void SCrowdyModelAttributeNamePin::OnKeyTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared)
	{
		return;
	}
	SetPinValue(FName(*NewText.ToString()));
}

void SCrowdyModelAttributeNamePin::OnKeySelected(FName Key)
{
	SetPinValue(Key);
}

void SCrowdyModelAttributeNamePin::SetPinValue(FName Key)
{
	if (!GraphPinObj)
	{
		return;
	}

	const FString NewValue = Key.IsNone() ? FString() : Key.ToString();
	if (GraphPinObj->GetDefaultAsString() == NewValue)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetModelAttributeKey", "Set Model Attribute Key"));
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

#undef LOCTEXT_NAMESPACE
