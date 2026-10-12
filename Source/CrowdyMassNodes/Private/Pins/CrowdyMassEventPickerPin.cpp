#include "Pins/CrowdyMassEventPickerPin.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Nodes/CrowdyK2Node_CallCrowdyEventOnEntity.h"
#include "Nodes/CrowdyK2Node_ReceiveCrowdyEventOnEntity.h"
#include "Nodes/CrowdyMassEventCatalog.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyMassEventPickerPin"

namespace CrowdyMassEventPickerPinPrivate
{
	/**
	 * Which of the two event nodes owns a pin, if either does.
	 *
	 * The two nodes declare the same three entry points the dropdown needs and share no base class, so the
	 * choice of which to call is made here. They are deliberately kept apart: giving them a common base
	 * would change the class of every call node already placed in a project's Blueprints, to spare this
	 * one function.
	 */
	struct FEventPickerHost
	{
		UCrowdyK2Node_CallCrowdyEventOnEntity* CallNode = nullptr;
		UCrowdyK2Node_ReceiveCrowdyEventOnEntity* ReceiveNode = nullptr;

		bool IsSet() const
		{
			return CallNode != nullptr || ReceiveNode != nullptr;
		}

		FString GetPickedEventDisplayName() const
		{
			if (CallNode)
			{
				return CallNode->GetPickedEventDisplayName();
			}
			return ReceiveNode ? ReceiveNode->GetPickedEventDisplayName() : FString();
		}

		void SetPickedEvent(UClass* DeclaringClass, const FName FunctionName) const
		{
			if (CallNode)
			{
				CallNode->SetPickedEvent(DeclaringClass, FunctionName);
				return;
			}
			if (ReceiveNode)
			{
				ReceiveNode->SetPickedEvent(DeclaringClass, FunctionName);
			}
		}

		void ClearPickedEvent() const
		{
			if (CallNode)
			{
				CallNode->ClearPickedEvent();
				return;
			}
			if (ReceiveNode)
			{
				ReceiveNode->ClearPickedEvent();
			}
		}
	};

	// The node behind a pin, but only when that pin is that node's own Event dropdown. A pin of the same
	// name on any other node is not this widget's to draw.
	FEventPickerHost FindEventPickerHost(const UEdGraphPin* Pin)
	{
		FEventPickerHost Host;
		if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Name)
		{
			return Host;
		}

		UEdGraphNode* OwningNode = Pin->GetOwningNodeUnchecked();
		if (Pin->PinName == CrowdyMassCallEventPins::EventPicker)
		{
			Host.CallNode = Cast<UCrowdyK2Node_CallCrowdyEventOnEntity>(OwningNode);
		}
		if (!Host.CallNode && Pin->PinName == CrowdyMassReceiveEventPins::EventPicker)
		{
			Host.ReceiveNode = Cast<UCrowdyK2Node_ReceiveCrowdyEventOnEntity>(OwningNode);
		}

		return Host;
	}
}

void SCrowdyMassEventPickerPin::Construct(const FArguments& InArgs, UEdGraphPin* InPin)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InPin);
}

TSharedRef<SWidget> SCrowdyMassEventPickerPin::GetDefaultValueWidget()
{
	return SNew(SComboButton)
		.ContentPadding(FMargin(6.f, 2.f))
		.IsEnabled_Lambda([this]() { return GraphPinObj && !GraphPinObj->bDefaultValueIsReadOnly; })
		.OnGetMenuContent(this, &SCrowdyMassEventPickerPin::BuildPickerMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyMassEventPickerPin::GetCurrentValueText)
			.Font(FAppStyle::GetFontStyle(TEXT("Graph.Node.PinName")))
		];
}

FText SCrowdyMassEventPickerPin::GetCurrentValueText() const
{
	const FString Picked = CrowdyMassEventPickerPinPrivate::FindEventPickerHost(GraphPinObj).GetPickedEventDisplayName();
	return Picked.IsEmpty() ? LOCTEXT("NoEventPicked", "(Pick an event)") : FText::FromString(Picked);
}

TSharedRef<SWidget> SCrowdyMassEventPickerPin::BuildPickerMenu()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("ClearEntry", "(None)"),
		LOCTEXT("ClearEntryTip", "Clear the picked event."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SCrowdyMassEventPickerPin::OnEventCleared)));

	// Which node this pin belongs to decides which list it gets, so the widget asks before it builds. A
	// receive node offered an event the crowd plane applies itself would let an author bind a graph that
	// compiles clean and never runs.
	const CrowdyMassEventPickerPinPrivate::FEventPickerHost Host =
		CrowdyMassEventPickerPinPrivate::FindEventPickerHost(GraphPinObj);

	FCrowdyMassEventCatalog Catalog;
	CrowdyMassEventCatalog::Build(Catalog, Host.ReceiveNode
		? ECrowdyMassEventCatalogPurpose::Receive
		: ECrowdyMassEventCatalogPurpose::Send);

	if (Catalog.Groups.Num() == 0)
	{
		// A miss while the registry is still filling is not an answer, and saying so is the difference
		// between "there are none" and "nobody has looked at all of them yet".
		const FText Message = Catalog.bAssetRegistryStillScanning
			? LOCTEXT("StillScanning", "The asset registry is still scanning; reopen this in a moment")
			: LOCTEXT("NoEvents", "No configured entity class declares a Crowdy event");

		MenuBuilder.AddMenuEntry(Message, FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return MenuBuilder.MakeWidget();
	}

	// Said even when the list is not empty, and that is the point of saying it here rather than only in the
	// empty branch. A partial list looks exactly like a complete one, and on the call node the SDK's own
	// C++ events are always in it, so a list holding only those reads as finished while a project's own
	// entity classes are still being found.
	if (Catalog.bAssetRegistryStillScanning)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StillScanningWithEntries", "The asset registry is still scanning; classes may be missing"),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
	}

	// With one class there is nothing to tell apart, so the headers would only add noise.
	const bool bShowClassGrouping = Catalog.Groups.Num() > 1;

	for (const FCrowdyMassEventClassGroup& Group : Catalog.Groups)
	{
		if (bShowClassGrouping)
		{
			MenuBuilder.BeginSection(NAME_None, FText::FromString(Group.DisplayName));
		}

		for (const FCrowdyMassEventEntry& Entry : Group.Events)
		{
			UClass* DeclaringClass = Entry.DeclaringClass.Get();
			if (!DeclaringClass)
			{
				continue;
			}

			MenuBuilder.AddMenuEntry(
				FText::FromString(Entry.DisplayName),
				Entry.Tooltip,
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &SCrowdyMassEventPickerPin::OnEventPicked,
					DeclaringClass, Entry.FunctionName)));
		}

		if (bShowClassGrouping)
		{
			MenuBuilder.EndSection();
		}
	}

	return MenuBuilder.MakeWidget();
}

void SCrowdyMassEventPickerPin::OnEventPicked(UClass* DeclaringClass, FName FunctionName)
{
	const CrowdyMassEventPickerPinPrivate::FEventPickerHost Host =
		CrowdyMassEventPickerPinPrivate::FindEventPickerHost(GraphPinObj);
	if (!Host.IsSet())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("PickCrowdyMassEvent", "Pick Crowdy Event"));
	Host.SetPickedEvent(DeclaringClass, FunctionName);
}

void SCrowdyMassEventPickerPin::OnEventCleared()
{
	const CrowdyMassEventPickerPinPrivate::FEventPickerHost Host =
		CrowdyMassEventPickerPinPrivate::FindEventPickerHost(GraphPinObj);
	if (!Host.IsSet())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ClearCrowdyMassEvent", "Clear Crowdy Event"));
	Host.ClearPickedEvent();
}

TSharedPtr<SGraphPin> FCrowdyMassEventPickerPinFactory::CreatePin(UEdGraphPin* InPin) const
{
	if (!CrowdyMassEventPickerPinPrivate::FindEventPickerHost(InPin).IsSet())
	{
		return nullptr;
	}

	return SNew(SCrowdyMassEventPickerPin, InPin);
}

#undef LOCTEXT_NAMESPACE
