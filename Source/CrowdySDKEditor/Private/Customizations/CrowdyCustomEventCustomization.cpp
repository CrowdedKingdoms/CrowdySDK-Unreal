#include "Customizations/CrowdyCustomEventCustomization.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "CrowdySDKEditor.h"
#include "CrowdyEditorEventMeta.h"
#include "CrowdyStudioModule.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailsView.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "ScopedTransaction.h"
#include "SEnumCombo.h"
#include "Textures/SlateIcon.h"
#include "UObject/Class.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// App channel names for the picker dropdown, fetched live at edit time and shared across the
	// (transient) detail-customization instances. Edit-time only, game thread only.
	TArray<FString> GCachedChannelNames;
	bool GChannelsFetchInFlight = false;

	// Pulls the signed-in app's channel names for the dropdown. Channels live on the Game API, which
	// only accepts an app-scoped token so this defers to CrowdyStudio, which mints one from the
	// session token and queries channels. (Posting the raw session token straight at the Game API, as
	// this used to, is rejected under the two-token model and left the dropdown empty.) No-op when the
	// app id is unset or no one is signed into Crowdy Studio (the field stays free-text).
	void FetchChannelListIfStale()
	{
		if (GChannelsFetchInFlight) return;

		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		if (!Settings || Settings->AppID <= 0) return;

		if (CrowdyStudioAuth::GetSignedInToken().IsEmpty()) return; // not signed in leave the dropdown empty

		GChannelsFetchInFlight = true;
		CrowdyStudioAuth::FetchAppChannelNames(Settings->AppID, [](const TArray<FString>& Names)
		{
			GChannelsFetchInFlight = false;
			GCachedChannelNames = Names;
		});
	}

	// The value (non-object) categories the RPC serializer can carry: primitives, byte/enum,
	// name/string/text, and structs.
	bool IsReplicatableLeafCategory(const FName Category)
	{
		return Category == UEdGraphSchema_K2::PC_Boolean
			|| Category == UEdGraphSchema_K2::PC_Byte
			|| Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| Category == UEdGraphSchema_K2::PC_Real
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_String
			|| Category == UEdGraphSchema_K2::PC_Text
			|| Category == UEdGraphSchema_K2::PC_Struct;
	}

	// The object and class reference categories the serializer carries by identity. Allowed as a
	// scalar or as the element of a TArray; sets and maps of object references are not supported.
	bool IsReplicatableObjectCategory(const FName Category)
	{
		return Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass;
	}

	// True when a parameter pin can ride the RPC serializer. A scalar may be a value type or an
	// object/class reference; a TArray may carry either; a set or map carries value-type elements
	// only (a map's value must be a value type too). Mirrors FCrowdyRPC::IsSupportedParamType at the
	// pin level so the live editor status matches what registration accepts.
	bool IsReplicatablePinType(const FEdGraphPinType& PinType)
	{
		const FName Category = PinType.PinCategory;
		if (!IsReplicatableLeafCategory(Category) && !IsReplicatableObjectCategory(Category))
		{
			return false;
		}

		// A scalar may be a value type or an object/class reference.
		if (PinType.ContainerType == EPinContainerType::None)
		{
			return true;
		}

		// An array may carry value-type elements or object/class references (Tier 3).
		if (PinType.ContainerType == EPinContainerType::Array)
		{
			return true;
		}

		// A set or map carries value-type elements only object keys/values are out of scope.
		if (!IsReplicatableLeafCategory(Category))
		{
			return false;
		}

		if (PinType.ContainerType == EPinContainerType::Map)
		{
			return IsReplicatableLeafCategory(PinType.PinValueType.TerminalCategory);
		}

		return true;
	}

	bool IsReplicateParameterPin(const UEdGraphPin* Pin)
	{
		if (!Pin) return false;
		if (Pin->ParentPin) return false;
		if (Pin->Direction != EGPD_Output) return false;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Schema && Schema->IsMetaPin(*Pin)) return false;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) return false;

		return true;
	}

	void SetCrowdyReplicatesMeta(FKismetUserDeclaredFunctionMetadata& Meta)
	{
		Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Replicates), FString());
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::LegacyReplicate));
	}

	void RemoveCrowdyReplicatesMeta(FKismetUserDeclaredFunctionMetadata& Meta)
	{
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Replicates));
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::LegacyReplicate));
	}

	bool ClearUnrealReplicationFlags(UK2Node_CustomEvent* Node)
	{
		if (!Node) return false;

		const uint32 PreviousFlags = Node->FunctionFlags;
		Node->FunctionFlags &= ~static_cast<uint32>(FUNC_NetFuncFlags);
		return Node->FunctionFlags != PreviousFlags;
	}

	bool WidgetContainsExactText(const TSharedRef<SWidget>& Widget, const TCHAR* Text)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			const TSharedRef<STextBlock> TextBlock = StaticCastSharedRef<STextBlock>(Widget);
			if (TextBlock->GetText().ToString().Equals(Text, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		FChildren* Children = Widget->GetAllChildren();
		if (!Children) return false;

		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (WidgetContainsExactText(Children->GetChildAt(Index), Text))
			{
				return true;
			}
		}

		return false;
	}

	bool CollapseRowsContainingText(const TSharedRef<SWidget>& Widget, const TCHAR* Text)
	{
		bool bCollapsedAny = false;

		if (Widget->GetType() == FName(TEXT("SDetailSingleItemRow"))
			&& WidgetContainsExactText(Widget, Text))
		{
			Widget->SetVisibility(EVisibility::Collapsed);
			bCollapsedAny = true;
		}

		FChildren* Children = Widget->GetAllChildren();
		if (!Children) return bCollapsedAny;

		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			bCollapsedAny |= CollapseRowsContainingText(Children->GetChildAt(Index), Text);
		}

		return bCollapsedAny;
	}

	void HideUnrealReplicationRow(IDetailLayoutBuilder& DetailBuilder, TWeakObjectPtr<UK2Node_CustomEvent> EditedNode)
	{
		TSharedPtr<IDetailsView> DetailsView = DetailBuilder.GetDetailsViewSharedPtr();
		if (!DetailsView.IsValid()) return;

		TWeakPtr<IDetailsView> WeakDetailsView = DetailsView;
		DetailsView->RegisterActiveTimer(0.f,
			FWidgetActiveTimerDelegate::CreateLambda(
				[WeakDetailsView, EditedNode, Attempts = 0](double /*InCurrentTime*/, float /*InDeltaTime*/) mutable
				{
					if (!EditedNode.IsValid() || !HasCrowdyReplicatesMeta(EditedNode->GetUserDefinedMetaData()))
					{
						return EActiveTimerReturnType::Stop;
					}

					const TSharedPtr<IDetailsView> PinnedDetailsView = WeakDetailsView.Pin();
					if (!PinnedDetailsView.IsValid())
					{
						return EActiveTimerReturnType::Stop;
					}

					const bool bCollapsed = CollapseRowsContainingText(PinnedDetailsView.ToSharedRef(), TEXT("Replicates"));
					++Attempts;
					return (bCollapsed || Attempts > 20)
						? EActiveTimerReturnType::Stop
						: EActiveTimerReturnType::Continue;
				}));
	}
}

void FCrowdyCustomEventCustomization::CustomizeDetails(
	IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.IsEmpty()) return;

	EditedNode = Cast<UK2Node_CustomEvent>(Objects[0].Get());
	if (!EditedNode.IsValid()) return;

	if (IsReplicated())
	{
		DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UK2Node_Event, FunctionFlags));
		HideUnrealReplicationRow(DetailBuilder, EditedNode);

		// Warm the channel-picker dropdown so it's ready by the time the author opens it.
		FetchChannelListIfStale();
	}

	BuildReplicateCategory(DetailBuilder);
}

const TArray<UEdGraphPin*>& FCrowdyCustomEventCustomization::GetNodePins() const
{
	static const TArray<UEdGraphPin*> Empty;
	return EditedNode.IsValid() ? EditedNode->Pins : Empty;
}

int32 FCrowdyCustomEventCustomization::GetNodeFunctionFlags() const
{
	return EditedNode.IsValid() ? EditedNode->FunctionFlags : 0;
}

void FCrowdyCustomEventCustomization::BuildReplicateCategory(
	IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(
			"CrowdySDK",
			FText::FromString(TEXT("Crowdy SDK")),
			ECategoryPriority::Important);

	Category.AddCustomRow(FText::FromString(TEXT("Crowdy Replicates")))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Crowdy Replicates")))
		.Font(DetailBuilder.GetDetailFont())
		.ToolTipText(FText::FromString(
			TEXT("Replicate this event like a C++ CROWDY_EVENT.\n\n"
			     "Call it normally and the Crowdy SDK serializes its parameters, routes them\n"
			     "over the transport, and runs the body on every client. The owning client is\n"
			     "authoritative; a non-owner's call is delegated to the owner.\n\n"
			     "Parameters may be primitives, enums, structs, object/class references, and arrays of any of these (sets and maps carry value types only). Recompile after toggling.")))
	]
	.ValueContent()
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked(this, &FCrowdyCustomEventCustomization::GetReplicateCheckState)
			.OnCheckStateChanged(
				this, &FCrowdyCustomEventCustomization::OnReplicateCheckChanged)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.f, 0.f, 0.f, 0.f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &FCrowdyCustomEventCustomization::GetReplicateStatusText)
			.ColorAndOpacity(this, &FCrowdyCustomEventCustomization::GetReplicateStatusColor)
			.Font(DetailBuilder.GetDetailFont())
		]
	];

	// Only where some map profile selects a backend that draws remote entities as crowd rows. Everything
	// this row describes happens on that representation, so in a project that draws every map with actors it
	// names a concept that does not exist there, offers pins for a handler that never runs, and its tooltip
	// sends the reader to an animation set asset the project has no class for.
	//
	// Asked as this panel is built, so switching a profile's Backend Class shows or hides the row the next
	// time a node's details are shown, with no editor restart.
	if (CrowdyAuthoringContributions::IsCrowdRepresentationSelected())
	{
		Category.AddCustomRow(FText::FromString(TEXT("Is A One-Shot Action")))
		.Visibility(MakeAttributeSP(this, &FCrowdyCustomEventCustomization::GetRoutingVisibility))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Is A One-Shot Action")))
			.Font(DetailBuilder.GetDetailFont())
			.ToolTipText(FText::FromString(
				TEXT("Declares that this event's parameters describe a one-shot action: a swing, a flinch, an emote.\n\n"
				     "It changes nothing where a client holds the entity as a real actor, which still runs this body.\n"
				     "It is read by clients that draw the entity as one of a crowd, which have no actor to run a body\n"
				     "on: those start the action with nothing registered and no extra graph to author.\n\n"
				     "Tick it and use Add Parameters to create the pins it is read by. Action ids start at 1 and\n"
				     "follow the animation set's Actions row order; 0 means no action.")))
		]
		.ValueContent()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked(this, &FCrowdyCustomEventCustomization::GetActionCheckState)
				.OnCheckStateChanged(this, &FCrowdyCustomEventCustomization::OnActionCheckChanged)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.f, 0.f, 0.f, 0.f)
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Add Parameters")))
				.ToolTipText(this, &FCrowdyCustomEventCustomization::GetAddActionParametersToolTip)
				.Visibility(MakeAttributeSP(this, &FCrowdyCustomEventCustomization::GetAddActionParametersVisibility))
				.OnClicked(this, &FCrowdyCustomEventCustomization::OnAddActionParametersClicked)
			]
		];
	}

	AddRoutingRow(Category, DetailBuilder,
		FText::FromString(TEXT("Recipient")),
		FText::FromString(TEXT("Which clients run the event, and over which transport. Spatial Multicast = everyone in range (chunk-based, decay-thinned); Multicast = every member of the app's session channel regardless of distance (over the channel transport, ignores chunk coordinates); Owning Client = only the target entity's owner (others request it); Host = only the elected host (others request it).")),
		FName(CrowdyRpcMetaKeys::Recipient), StaticEnum<ECrowdyEventRecipient>(),
		static_cast<int32>(ECrowdyEventRecipient::SpatialMulticast));

	AddRoutingRow(Category, DetailBuilder,
		FText::FromString(TEXT("Decay Rate")),
		FText::FromString(TEXT("Server-side spatial decay applied before the event reaches remote clients. Only applies to Spatial Multicast. The channel and targeted transports ignore it.")),
		FName(CrowdyRpcMetaKeys::Decay), StaticEnum<ECrowdyDecayRate>(),
		static_cast<int32>(ECrowdyDecayRate::No_Decay), /*bSpatialOnly*/ true);

	AddRoutingRow(Category, DetailBuilder,
		FText::FromString(TEXT("Replication Distance")),
		FText::FromString(TEXT("Maximum chunk distance the event travels from the target entity. Only applies to Spatial Multicast. The channel and targeted transports ignore it.")),
		FName(CrowdyRpcMetaKeys::Distance), StaticEnum<ECrowdyReplicationDistance>(),
		static_cast<int32>(ECrowdyReplicationDistance::Eight_Chunks), /*bSpatialOnly*/ true);

	Category.AddCustomRow(FText::FromString(TEXT("Channel")))
	.Visibility(MakeAttributeSP(this, &FCrowdyCustomEventCustomization::GetChannelRowVisibility))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Channel")))
		.ToolTipText(FText::FromString(
			TEXT("Which channel this Multicast routes over, by name. Leave empty for the app-wide default\n"
			     "session channel. Pick from the dropdown (your app's channels, loaded from Crowdy Studio) or\n"
			     "type a name. The client joins every referenced channel on connect.")))
		.Font(DetailBuilder.GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(220.f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.Text(this, &FCrowdyCustomEventCustomization::GetChannelText)
			.OnTextCommitted(this, &FCrowdyCustomEventCustomization::OnChannelTextCommitted)
			.HintText(FText::FromString(TEXT("Default session channel")))
			.Font(DetailBuilder.GetDetailFont())
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.f, 0.f, 0.f, 0.f)
		.VAlign(VAlign_Center)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, &FCrowdyCustomEventCustomization::BuildChannelPickerMenu)
			.ToolTipText(FText::FromString(TEXT("Pick from the app's channels")))
			.ButtonContent()
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Pick")))
			]
		]
	];
}

void FCrowdyCustomEventCustomization::AddRoutingRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
	const FText& Label, const FText& ToolTip, FName MetaKey, const UEnum* EnumType, int32 DefaultValue,
	bool bSpatialOnly)
{
	if (!EnumType) return;

	FDetailWidgetRow& Row = Category.AddCustomRow(Label);
	Row.Visibility(MakeAttributeSP(this, &FCrowdyCustomEventCustomization::GetRoutingVisibility));

	// Greys the whole row (label + value) out when the chosen transport ignores this setting.
	if (bSpatialOnly)
	{
		Row.IsEnabled(MakeAttributeSP(this, &FCrowdyCustomEventCustomization::IsSpatialRoutingEnabled));
	}

	Row.NameContent()
	[
		SNew(STextBlock)
		.Text(Label)
		.ToolTipText(ToolTip)
		.Font(DetailBuilder.GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(180.f)
	[
		SNew(SEnumComboBox, EnumType)
		.CurrentValue(this, &FCrowdyCustomEventCustomization::GetRoutingValue, MetaKey, EnumType, DefaultValue)
		.OnEnumSelectionChanged(this, &FCrowdyCustomEventCustomization::OnRoutingChanged, MetaKey, EnumType)
		.Font(DetailBuilder.GetDetailFont())
	];
}

bool FCrowdyCustomEventCustomization::IsSpatialRoutingEnabled() const
{
	const int32 Recipient = GetRoutingValue(
		FName(CrowdyRpcMetaKeys::Recipient), StaticEnum<ECrowdyEventRecipient>(),
		static_cast<int32>(ECrowdyEventRecipient::SpatialMulticast));
	return Recipient == static_cast<int32>(ECrowdyEventRecipient::SpatialMulticast);
}

bool FCrowdyCustomEventCustomization::IsReplicated() const
{
	if (!EditedNode.IsValid()) return false;
	return HasCrowdyReplicatesMeta(EditedNode->GetUserDefinedMetaData());
}

void FCrowdyCustomEventCustomization::SetReplicated(bool bReplicated)
{
	if (!EditedNode.IsValid()) return;

	EditedNode->Modify();

	FKismetUserDeclaredFunctionMetadata& Meta = EditedNode->GetUserDefinedMetaData();

	if (bReplicated)
	{
		SetCrowdyReplicatesMeta(Meta);

		ClearUnrealReplicationFlags(EditedNode.Get());
	}
	else
	{
		RemoveCrowdyReplicatesMeta(Meta);
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Recipient));
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Decay));
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Distance));

		// Cleared with the rest, and not because an unreplicated event has no use for it. Every row that
		// could show or untick this marker is hidden while the event is not replicated, so leaving it set
		// would leave the node carrying a declaration nothing can display and nobody can withdraw, which
		// re-arms itself the moment anyone ticks Crowdy Replicates again.
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Action));
	}

	UBlueprint* Blueprint = EditedNode->GetBlueprint();
	if (!Blueprint) return;

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Custom event '%s' Crowdy Replicates %s. Recompile to activate."),
		*EditedNode->CustomFunctionName.ToString(),
		bReplicated ? TEXT("enabled") : TEXT("disabled"));
}

ECheckBoxState FCrowdyCustomEventCustomization::GetReplicateCheckState() const
{
	return IsReplicated() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyCustomEventCustomization::OnReplicateCheckChanged(ECheckBoxState NewState)
{
	SetReplicated(NewState == ECheckBoxState::Checked);
}

EVisibility FCrowdyCustomEventCustomization::GetRoutingVisibility() const
{
	return IsReplicated() ? EVisibility::Visible : EVisibility::Collapsed;
}

bool FCrowdyCustomEventCustomization::IsDeclaredAction() const
{
	if (!EditedNode.IsValid()) return false;
	return EditedNode->GetUserDefinedMetaData().HasMetaData(FName(CrowdyRpcMetaKeys::Action));
}

void FCrowdyCustomEventCustomization::SetDeclaredAction(bool bIsAction)
{
	if (!EditedNode.IsValid()) return;

	EditedNode->Modify();

	// A valueless key, exactly as the C++ side spells it: the runtime asks whether the key is present,
	// never what it says, so writing a value here would be a second spelling of the same declaration.
	FKismetUserDeclaredFunctionMetadata& Meta = EditedNode->GetUserDefinedMetaData();
	if (bIsAction)
	{
		Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Action), FString());
	}
	else
	{
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Action));
	}

	if (UBlueprint* Blueprint = EditedNode->GetBlueprint())
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
}

ECheckBoxState FCrowdyCustomEventCustomization::GetActionCheckState() const
{
	return IsDeclaredAction() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyCustomEventCustomization::OnActionCheckChanged(ECheckBoxState NewState)
{
	SetDeclaredAction(NewState == ECheckBoxState::Checked);
}

TArray<FCrowdyActionParameterSpec> FCrowdyCustomEventCustomization::GetMissingActionParameters() const
{
	TArray<FCrowdyActionParameterSpec> Missing;
	if (!EditedNode.IsValid())
	{
		return Missing;
	}

	// Matched against the node's own pins rather than against a compiled signature, because the point of
	// the offer is to be usable before the first compile, which is exactly when the box has just been
	// ticked. A pin of the right name and the wrong type is deliberately NOT counted as missing: creating
	// a second one beside it would be a silently renamed duplicate. The compile warning names the type
	// problem instead, for each of these parameters, in language the author can act on.
	for (const FCrowdyActionParameterSpec& Spec : CrowdyAuthoringContributions::GetActionParameters())
	{
		const bool bPresent = EditedNode->Pins.ContainsByPredicate([&Spec](const UEdGraphPin* Pin)
		{
			return Pin && Pin->Direction == EGPD_Output && Pin->PinName == Spec.Name;
		});

		if (!bPresent)
		{
			Missing.Add(Spec);
		}
	}

	return Missing;
}

bool FCrowdyCustomEventCustomization::CanAddActionParameters() const
{
	// A custom event wired to a Bind Event node takes its signature from the delegate, and the engine
	// refuses to edit its pins for exactly that reason (IsEditable is false while OutputDelegate is
	// linked). CreateUserDefinedPin does no checking of its own, so without this the pins would be
	// appended and then silently thrown away by the next reconstruct, which rebuilds them from the
	// delegate.
	return EditedNode.IsValid() && EditedNode->IsEditable();
}

EVisibility FCrowdyCustomEventCustomization::GetAddActionParametersVisibility() const
{
	// Only while there is an offer to make. An event already carrying all three has nothing to add, an
	// unticked one has not asked for any of them, and a delegate-bound one cannot be given any.
	return IsDeclaredAction() && CanAddActionParameters() && GetMissingActionParameters().Num() > 0
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText FCrowdyCustomEventCustomization::GetAddActionParametersToolTip() const
{
	const TArray<FCrowdyActionParameterSpec> Missing = GetMissingActionParameters();
	if (Missing.IsEmpty())
	{
		return FText::GetEmpty();
	}

	FString Names;
	for (const FCrowdyActionParameterSpec& Spec : Missing)
	{
		if (!Names.IsEmpty())
		{
			Names += TEXT(", ");
		}
		Names += Spec.Name.ToString();
	}

	// Names what will be created before it is created, because this writes pins onto the author's node and
	// the button's own label cannot say which ones are still missing.
	return FText::FromString(FString::Printf(
		TEXT("Adds the input pins this event is read by that it does not have yet: %s."), *Names));
}

FReply FCrowdyCustomEventCustomization::OnAddActionParametersClicked()
{
	const TArray<FCrowdyActionParameterSpec> Missing = GetMissingActionParameters();
	if (!CanAddActionParameters() || Missing.IsEmpty())
	{
		return FReply::Handled();
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Add Crowdy Action Parameters")));
	EditedNode->Modify();

	int32 Added = 0;
	for (const FCrowdyActionParameterSpec& Spec : Missing)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = Spec.PinCategory;
		PinType.PinSubCategory = Spec.PinSubCategory;

		// bUseUniqueName false: the name IS the contract, and a pin the runtime cannot find by it would be
		// worse than no pin at all. GetMissingActionParameters has already established there is none.
		Added += EditedNode->CreateUserDefinedPin(Spec.Name, PinType, EGPD_Output, /*bUseUniqueName*/false)
			!= nullptr ? 1 : 0;
	}

	if (Added == 0)
	{
		// Nothing was written, so there is nothing to undo. Left on the stack this would be an empty entry
		// the author has to step past to reach the edit they actually made.
		Transaction.Cancel();
		return FReply::Handled();
	}

	// The same finish the engine's own Add Input flow performs. Reconstructing rebuilds this node around
	// its new pins, and the schema call is what reconstructs the CALL sites, including the ones in other
	// Blueprints: without it a caller in another asset keeps the old pin set and passes nothing for the
	// parameter that was just added.
	EditedNode->ReconstructNode();
	if (const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>())
	{
		Schema->HandleParameterDefaultValueChanged(EditedNode.Get());
	}

	if (UBlueprint* Blueprint = EditedNode->GetBlueprint())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return FReply::Handled();
}

int32 FCrowdyCustomEventCustomization::GetRoutingValue(
	FName MetaKey, const UEnum* EnumType, int32 DefaultValue) const
{
	if (!EditedNode.IsValid() || !EnumType) return DefaultValue;

	const FKismetUserDeclaredFunctionMetadata& Meta = EditedNode->GetUserDefinedMetaData();
	if (!Meta.HasMetaData(MetaKey)) return DefaultValue;

	const int64 Value = EnumType->GetValueByNameString(Meta.GetMetaData(MetaKey));
	return Value == INDEX_NONE ? DefaultValue : static_cast<int32>(Value);
}

void FCrowdyCustomEventCustomization::SetRoutingValue(
	FName MetaKey, const UEnum* EnumType, int32 NewValue)
{
	if (!EditedNode.IsValid() || !EnumType) return;

	EditedNode->GetUserDefinedMetaData().SetMetaData(
		MetaKey, EnumType->GetNameStringByValue(NewValue));

	if (UBlueprint* Blueprint = EditedNode->GetBlueprint())
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();
	}
}

void FCrowdyCustomEventCustomization::OnRoutingChanged(
	int32 NewValue, ESelectInfo::Type /*SelectInfo*/, FName MetaKey, const UEnum* EnumType)
{
	SetRoutingValue(MetaKey, EnumType, NewValue);
}

EVisibility FCrowdyCustomEventCustomization::GetChannelRowVisibility() const
{
	if (!IsReplicated()) return EVisibility::Collapsed;

	// The channel only applies to a channel-routed Multicast.
	const int32 Recipient = GetRoutingValue(
		FName(CrowdyRpcMetaKeys::Recipient), StaticEnum<ECrowdyEventRecipient>(),
		static_cast<int32>(ECrowdyEventRecipient::SpatialMulticast));
	return Recipient == static_cast<int32>(ECrowdyEventRecipient::Multicast)
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText FCrowdyCustomEventCustomization::GetChannelText() const
{
	if (!EditedNode.IsValid()) return FText::GetEmpty();

	const FKismetUserDeclaredFunctionMetadata& Meta = EditedNode->GetUserDefinedMetaData();
	return Meta.HasMetaData(FName(CrowdyRpcMetaKeys::Channel))
		? FText::FromString(Meta.GetMetaData(FName(CrowdyRpcMetaKeys::Channel)))
		: FText::GetEmpty();
}

void FCrowdyCustomEventCustomization::OnChannelTextCommitted(const FText& NewText, ETextCommit::Type /*CommitType*/)
{
	WriteChannelMeta(NewText.ToString().TrimStartAndEnd());
}

void FCrowdyCustomEventCustomization::SetChannelValueAction(FString ChannelName)
{
	WriteChannelMeta(ChannelName);
}

void FCrowdyCustomEventCustomization::WriteChannelMeta(const FString& ChannelName)
{
	if (!EditedNode.IsValid()) return;

	EditedNode->Modify();

	FKismetUserDeclaredFunctionMetadata& Meta = EditedNode->GetUserDefinedMetaData();
	if (ChannelName.IsEmpty())
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Channel));
	else
		Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Channel), ChannelName);

	if (UBlueprint* Blueprint = EditedNode->GetBlueprint())
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();
	}
}

TSharedRef<SWidget> FCrowdyCustomEventCustomization::BuildChannelPickerMenu()
{
	// The channel list is fetched async; kick it now (no-op if already in flight or unavailable).
	FetchChannelListIfStale();

	TSharedRef<SBox> MenuBox = SNew(SBox);
	MenuBox->SetContent(BuildChannelPickerMenuContent());

	// OnGetMenuContent builds the popup once, so a fetch that lands while it's open would never show
	// and the user would have to close and reopen. Poll the shared fetch state and rebuild the content in
	// place only when it changes, then stop once the fetch settles (or the menu/customization is gone).
	TWeakPtr<SBox> WeakMenuBox = MenuBox;
	TWeakPtr<FCrowdyCustomEventCustomization> WeakSelf =
		StaticCastSharedRef<FCrowdyCustomEventCustomization>(AsShared());

	MenuBox->RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateLambda(
		[WeakSelf, WeakMenuBox, LastCount = GCachedChannelNames.Num(), LastInFlight = GChannelsFetchInFlight]
		(double, float) mutable -> EActiveTimerReturnType
		{
			TSharedPtr<SBox> Box = WeakMenuBox.Pin();
			TSharedPtr<FCrowdyCustomEventCustomization> Self = WeakSelf.Pin();
			if (!Box.IsValid() || !Self.IsValid())
			{
				return EActiveTimerReturnType::Stop;
			}

			const int32 Count = GCachedChannelNames.Num();
			const bool bInFlight = GChannelsFetchInFlight;
			if (Count != LastCount || bInFlight != LastInFlight)
			{
				Box->SetContent(Self->BuildChannelPickerMenuContent());
				LastCount = Count;
				LastInFlight = bInFlight;
			}

			return bInFlight ? EActiveTimerReturnType::Continue : EActiveTimerReturnType::Stop;
		}));

	return MenuBox;
}

TSharedRef<SWidget> FCrowdyCustomEventCustomization::BuildChannelPickerMenuContent()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("(Default session channel)")),
		FText::FromString(TEXT("Route over the app-wide default session channel.")),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyCustomEventCustomization::SetChannelValueAction, FString())));

	if (CrowdyStudioAuth::GetSignedInToken().IsEmpty())
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(TEXT("Sign in to Crowdy Studio to load channels")),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
		return MenuBuilder.MakeWidget();
	}

	if (GCachedChannelNames.Num() == 0)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(GChannelsFetchInFlight ? TEXT("Loading channels…") : TEXT("No channels found")),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
	}
	else
	{
		MenuBuilder.BeginSection(NAME_None, FText::FromString(TEXT("Channels")));
		for (const FString& Name : GCachedChannelNames)
		{
			MenuBuilder.AddMenuEntry(
				FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &FCrowdyCustomEventCustomization::SetChannelValueAction, Name)));
		}
		MenuBuilder.EndSection();
	}

	MenuBuilder.AddSeparator();
	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Refresh")), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([] { FetchChannelListIfStale(); })));

	return MenuBuilder.MakeWidget();
}

FText FCrowdyCustomEventCustomization::GetReplicateStatusText() const
{
	if (!IsReplicated())
	{
		return FText::FromString(TEXT("Not replicated"));
	}

	const FRpcSignatureResult Result = InspectRpcSignature();
	if (!Result.bValid)
	{
		return FText::FromString(Result.Problem);
	}

	return FText::FromString(FString::Printf(
		TEXT("Replicated (%d parameter%s)"),
		Result.ParamCount, Result.ParamCount == 1 ? TEXT("") : TEXT("s")));
}

FSlateColor FCrowdyCustomEventCustomization::GetReplicateStatusColor() const
{
	if (!IsReplicated())
	{
		return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
	}

	return InspectRpcSignature().bValid
		? FSlateColor(FLinearColor(0.2f, 0.9f, 0.4f))
		: FSlateColor(FLinearColor(0.9f, 0.3f, 0.2f));
}

FCrowdyCustomEventCustomization::FRpcSignatureResult
FCrowdyCustomEventCustomization::InspectRpcSignature() const
{
	FRpcSignatureResult Result;

	if ((GetNodeFunctionFlags() & FUNC_Private) != 0)
	{
		Result.Problem = TEXT("Set Access Specifier to Public - Crowdy Replicates cannot be private");
		return Result;
	}

	if ((GetNodeFunctionFlags() & FUNC_Protected) != 0)
	{
		Result.Problem = TEXT("Set Access Specifier to Public - Crowdy Replicates cannot be protected");
		return Result;
	}

	if ((GetNodeFunctionFlags() & FUNC_NetFuncFlags) != 0)
	{
		Result.Problem = TEXT("Disable Unreal replication - it cannot combine with Crowdy replication");
		return Result;
	}

	for (const UEdGraphPin* Pin : GetNodePins())
	{
		if (!IsReplicateParameterPin(Pin)) continue;

		if (!IsReplicatablePinType(Pin->PinType))
		{
			Result.Problem = FString::Printf(
				TEXT("Parameter '%s' is not replicatable - use a primitive, enum, struct, object/class reference, an array of any of these, or a set/map of value types"),
				*Pin->GetDisplayName().ToString());
			return Result;
		}

		++Result.ParamCount;
	}

	Result.bValid = true;
	return Result;
}
