#include "Customizations/CrowdyReplicatedVariableCustomization.h"

#include "BlueprintEditorModule.h" // IBlueprintEditor (no standalone IBlueprintEditor.h in UE 5.8)
#include "Components/ActorComponent.h"
#include "CrowdySDKEditor.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "GameModel/CrowdyContainerBlueprintExtension.h"
#include "IDetailGroup.h"
#include "IPropertyUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "ScopedTransaction.h"
#include "Styling/SlateColor.h"
#include "Textures/SlateIcon.h"
#include "UObject/Class.h"
#include "UObject/CoreNetTypes.h" // ELifetimeCondition / COND_None
#include "UObject/PropertyWrapper.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyReplicatedVariableCustomization"

namespace
{
	// True when a Blueprint targets a class that can carry a CrowdyState view property: an AActor or a
	// UActorComponent (CrowdyState replicates entities' view state). Falls back to the parent class when the
	// generated class is not yet available.
	bool IsActorOrComponentBlueprint(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return false;
		}

		const UClass* Class = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get();
		return Class && (Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()));
	}

}

TSharedPtr<IDetailCustomization> FCrowdyReplicatedVariableCustomization::MakeInstance(
	TSharedPtr<IBlueprintEditor> InBlueprintEditor)
{
	const TArray<UObject*>* Objects =
		(InBlueprintEditor.IsValid() ? InBlueprintEditor->GetObjectsCurrentlyBeingEdited() : nullptr);
	if (!Objects || Objects->Num() != 1)
	{
		return nullptr;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>((*Objects)[0]);
	// Show the dropdown where either plane is plausible: an actor/component (CrowdyState view state) OR a Game
	// Model container class (its Server Owned attributes). Each mode is then greyed per the variable's type and
	// the class below.
	if (!IsActorOrComponentBlueprint(Blueprint) && !CrowdyContainerMarker::IsGameModelContainerClass(Blueprint))
	{
		return nullptr;
	}

	TSharedRef<FCrowdyReplicatedVariableCustomization> Instance =
		MakeShared<FCrowdyReplicatedVariableCustomization>();
	Instance->Blueprint = Blueprint;
	return Instance;
}

void FCrowdyReplicatedVariableCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);
	if (ObjectsBeingCustomized.IsEmpty())
	{
		return;
	}

	// The details panel edits a UPropertyWrapper standing in for the BP variable; unwrap it to the live
	// FProperty on the generated class (mirrors the ModioUI variable customization).
	UPropertyWrapper* PropertyWrapper = Cast<UPropertyWrapper>(ObjectsBeingCustomized[0].Get());
	VariableProperty = PropertyWrapper ? PropertyWrapper->GetProperty() : nullptr;
	if (!VariableProperty.IsValid())
	{
		return;
	}

	// Cache the property utilities so SetMode can defer a panel rebuild when the mode changes (the
	// Replicated-only rows are structural, not just visibility-toggled). GetPropertyUtilities returns a
	// TSharedRef, so the member is always valid after this.
	PropertyUtilities = DetailBuilder.GetPropertyUtilities();

	BuildReplicationCategory(DetailBuilder);
}

TArray<ECrowdyReplicationMode> FCrowdyReplicatedVariableCustomization::SelectableModes()
{
	// The widget code below iterates this list, so a mode's availability is data, not Slate. Server Owned is
	// greyed per-entry when the variable's type is not a supported Server Owned scalar/string (see the combo).
	return { ECrowdyReplicationMode::None, ECrowdyReplicationMode::Replicated, ECrowdyReplicationMode::ServerOwned };
}

FText FCrowdyReplicatedVariableCustomization::ModeDisplayText(ECrowdyReplicationMode Mode)
{
	switch (Mode)
	{
	case ECrowdyReplicationMode::Replicated:
		return LOCTEXT("ModeReplicated", "Replicated");
	case ECrowdyReplicationMode::ServerOwned:
		return LOCTEXT("ModeServerOwned", "Server Owned");
	case ECrowdyReplicationMode::None:
	default:
		return LOCTEXT("ModeNone", "None");
	}
}

FText FCrowdyReplicatedVariableCustomization::ModeTooltipText(ECrowdyReplicationMode Mode)
{
	switch (Mode)
	{
	case ECrowdyReplicationMode::Replicated:
		return LOCTEXT("ModeReplicatedTip",
			"CrowdyState: the fast, client-authoritative view plane. The owning client diffs this variable "
			"each tick and ships only the changed value to peers, where it is written onto the live actor "
			"and its RepNotify runs. Use it for movement, animation, and other high-frequency view state, "
			"NOT for authoritative or cheat-sensitive data (that belongs in a Game Model). Recompile after "
			"changing.");
	case ECrowdyReplicationMode::ServerOwned:
		return LOCTEXT("ModeServerOwnedTip",
			"Game Model: server-authoritative truth over GraphQL. The value lives on the server; a client pulls "
			"it and its RepNotify runs. Use it for authoritative or cheat-sensitive data (health, gold, "
			"inventory), NOT high-frequency view state (that is Replicated). The variable's class must be a "
			"CrowdyContainer for the attribute to be discovered. Recompile after changing.");
	case ECrowdyReplicationMode::None:
	default:
		return LOCTEXT("ModeNoneTip", "Not networked by Crowdy.");
	}
}

void FCrowdyReplicatedVariableCustomization::BuildReplicationCategory(IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		"CrowdySDK",
		LOCTEXT("CrowdyReplicationCategory", "Crowdy Replication"),
		ECategoryPriority::Important);

	Category.AddCustomRow(LOCTEXT("ModeRowFilter", "Crowdy Replication"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("ModeLabel", "Replication"))
		.ToolTipText(LOCTEXT("ModeLabelTip",
			"How this variable participates in Crowdy networking. Replicated puts it on the CrowdyState "
			"view plane (client-authoritative, diffed and shipped to peers)."))
		.Font(DetailBuilder.GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(200.f)
	[
		SNew(SComboButton)
		.OnGetMenuContent(this, &FCrowdyReplicatedVariableCustomization::BuildModeComboContent)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &FCrowdyReplicatedVariableCustomization::GetCurrentModeText)
			.Font(DetailBuilder.GetDetailFont())
		]
	];

	// Inline reason shown whenever the variable's type cannot ride CrowdyState, so the greyed-out Replicated
	// entry is not a silent dead end. Gated on the type alone (NOT on the current mode): an unsupported-type
	// variable can never be switched into Replicated mode, so a mode-gated warning would be unreachable
	// exactly when it is needed.
	Category.AddCustomRow(LOCTEXT("TypeWarnFilter", "Crowdy Replication"))
	.Visibility(MakeAttributeSP(this, &FCrowdyReplicatedVariableCustomization::GetTypeWarningVisibility))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.5f, 0.2f)))
		.Text(LOCTEXT("TypeUnsupported",
			"This type cannot be CrowdyState-replicated: object references, containers, and static arrays "
			"are not supported."))
		.Font(DetailBuilder.GetDetailFont())
	];

	// The mode-specific rows are built structurally for the current mode only. An IDetailGroup has no per-row
	// Visibility hook (unlike a FDetailWidgetRow), so a mode-gated .Visibility() cannot hide the Advanced
	// group's disclosure header, it would linger as an empty triangle in the wrong mode. Making these rows
	// structural (created only for the current mode) is the clean fix; SetMode calls RequestForceRefresh so
	// switching mode regenerates this category with the right row set. The mode combo and the type-warning
	// row above stay unconditional (the warning is type-gated, not mode-gated).
	const ECrowdyReplicationMode CurrentMode = GetCurrentMode();
	if (CurrentMode == ECrowdyReplicationMode::Replicated)
	{
		BuildRepNotifyRow(Category, DetailBuilder);

		// A muted note that Crowdy now owns this variable's replication. This is the honest, header-safe
		// substitute for hiding the engine's native Replication rows: there is no verifiable UE 5.8 API to hide
		// only those two engine-built custom rows from a layered customization (they are not property rows, and
		// EditDefaultProperty's own doc forbids customizing another customization's rows). The native combo
		// self-corrects to "None" and greys its condition once ClearNativeReplication clears CPF_Net.
		Category.AddCustomRow(LOCTEXT("NativeRepNoteFilter", "Crowdy Replication"))
		.WholeRowContent()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("NativeRepNote",
				"Crowdy manages replication for this variable. Unreal's native variable Replication is turned "
				"off while it is Crowdy-replicated."))
			.Font(DetailBuilder.GetDetailFont())
		];

		// Advanced sub-group: heartbeat + owner-only + manual-dirty. Grouped so the secondary flags read as
		// advanced, not primary.
		IDetailGroup& Advanced = Category.AddGroup(
			FName(TEXT("CrowdyStateAdvanced")),
			LOCTEXT("AdvancedGroup", "Advanced"),
			/*bForAdvanced*/ false,
			/*bStartExpanded*/ false);

		// Keyframe heartbeat: default On for a newly-Replicated BP variable (SetMode pre-writes it). Turning it
		// off keeps on-change replication but stops the periodic "nothing changed, here it is anyway" re-send.
		Advanced.AddWidgetRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("HeartbeatLabel", "Keyframe heartbeat"))
			.ToolTipText(LOCTEXT("HeartbeatTip",
				"Periodically re-send this variable even when unchanged (every map keyframe interval) so a late or "
				"packet-loss-desynced observer converges without waiting for the next change. On by default. Turn "
				"it off for high-frequency values that self-heal on their next change, or when a Game Model holds "
				"the durable truth. On-change replication is unaffected either way."))
			.Font(DetailBuilder.GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked(this, &FCrowdyReplicatedVariableCustomization::GetHeartbeatCheckState)
			.OnCheckStateChanged(this, &FCrowdyReplicatedVariableCustomization::OnHeartbeatCheckChanged)
		];

		Advanced.AddWidgetRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("OwnerOnlyLabel", "Only send to owner"))
			.ToolTipText(LOCTEXT("OwnerOnlyTip",
				"Deliver this variable only to the entity's owning client (a targeted send), not to everyone in "
				"range. Delivery scope, not secrecy."))
			.Font(DetailBuilder.GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked(this, &FCrowdyReplicatedVariableCustomization::GetOwnerOnlyCheckState)
			.OnCheckStateChanged(this, &FCrowdyReplicatedVariableCustomization::OnOwnerOnlyCheckChanged)
		];

		Advanced.AddWidgetRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ManualDirtyLabel", "Update manually"))
			.ToolTipText(LOCTEXT("ManualDirtyTip",
				"Skip the per-tick diff for this variable; it is sent only when you call MarkStateDirty for it. "
				"Use for big or rarely-changing values you do not want compared every tick."))
			.Font(DetailBuilder.GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked(this, &FCrowdyReplicatedVariableCustomization::GetManualDirtyCheckState)
			.OnCheckStateChanged(this, &FCrowdyReplicatedVariableCustomization::OnManualDirtyCheckChanged)
		];
	}
	else if (CurrentMode == ECrowdyReplicationMode::ServerOwned)
	{
		BuildServerOwnedCategory(Category, DetailBuilder);
	}
}

// The shared parameterless-RepNotify (CrowdyOnRep) row: one GAS-style notify convention across both planes,
// so Replicated and Server Owned present the identical row (text box + a Pick menu of the Blueprint's own
// zero-parameter functions).
void FCrowdyReplicatedVariableCustomization::BuildRepNotifyRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder)
{
	Category.AddCustomRow(LOCTEXT("RepNotifyFilter", "RepNotify"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("RepNotifyLabel", "RepNotify"))
		.ToolTipText(LOCTEXT("RepNotifyTip",
			"A parameterless function (GAS-style, no previous value) run on the receiver right after this "
			"variable is written by a Crowdy update (a CrowdyState delta or a Game Model pull). Leave empty for "
			"none. Pick from your zero-parameter functions or type a name."))
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
			.Text(this, &FCrowdyReplicatedVariableCustomization::GetRepNotifyText)
			.OnTextCommitted(this, &FCrowdyReplicatedVariableCustomization::OnRepNotifyCommitted)
			.HintText(LOCTEXT("RepNotifyHint", "None"))
			.Font(DetailBuilder.GetDetailFont())
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.f, 0.f, 0.f, 0.f)
		.VAlign(VAlign_Center)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, &FCrowdyReplicatedVariableCustomization::BuildRepNotifyPickerMenu)
			.ToolTipText(LOCTEXT("RepNotifyPickTip", "Pick a zero-parameter function"))
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("RepNotifyPick", "Pick"))
			]
		]
	];
}

// The Server Owned rows: a container-tag note (when the class is not yet a CrowdyContainer), the shared
// RepNotify row, the native Min/Max clamp the attribute inherits, and the read-Visibility dropdown.
void FCrowdyReplicatedVariableCustomization::BuildServerOwnedCategory(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder)
{
	// A note (only when the class is not a container yet) that a Server Owned variable is authored but not yet
	// discoverable until its class becomes a container. Not an error: the marker is still written. The button
	// marks the class in one click; the Crowdy toolbar menu or a CrowdyContainer C++ base do the same.
	Category.AddCustomRow(LOCTEXT("ContainerTagFilter", "Crowdy Replication"))
	.Visibility(MakeAttributeSP(this, &FCrowdyReplicatedVariableCustomization::GetContainerTagWarningVisibility))
	.WholeRowContent()
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.5f, 0.2f)))
			.Text(LOCTEXT("ContainerTagWarning",
				"This class is not a Game Model container yet, so its Server Owned variables will not be synced. "
				"Make it one with the button below. You can change this later from the Crowdy SDK toolbar."))
			.Font(DetailBuilder.GetDetailFont())
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 4.f, 0.f, 0.f)
		.HAlign(HAlign_Left)
		[
			SNew(SButton)
			.Text(LOCTEXT("MakeContainerButton", "Make this class a Game Model class"))
			.ToolTipText(LOCTEXT("MakeContainerButtonTip",
				"Mark this Blueprint as a Game Model container so its Server Owned attributes are discovered and "
				"synced. Recompile to apply."))
			.OnClicked(this, &FCrowdyReplicatedVariableCustomization::OnMakeContainerClicked)
		]
	];

	BuildRepNotifyRow(Category, DetailBuilder);

	// Min/Max (native ClampMin/ClampMax): the authoritative bounds the effect lowering wraps every write in.
	Category.AddCustomRow(LOCTEXT("ClampFilter", "Min Max"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("ClampLabel", "Min / Max"))
		.ToolTipText(LOCTEXT("ClampTip",
			"The authoritative clamp for this attribute (Unreal's native ClampMin / ClampMax). Every effect "
			"write is wrapped in these bounds on the server, so the value can never leave the range. Leave both "
			"empty for no clamp."))
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
			.Text(MakeAttributeSP(this, &FCrowdyReplicatedVariableCustomization::GetClampText, TEXT("ClampMin")))
			.OnTextCommitted(this, &FCrowdyReplicatedVariableCustomization::OnClampCommitted, TEXT("ClampMin"))
			.HintText(LOCTEXT("ClampMinHint", "Min"))
			.Font(DetailBuilder.GetDetailFont())
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(4.f, 0.f, 0.f, 0.f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.Text(MakeAttributeSP(this, &FCrowdyReplicatedVariableCustomization::GetClampText, TEXT("ClampMax")))
			.OnTextCommitted(this, &FCrowdyReplicatedVariableCustomization::OnClampCommitted, TEXT("ClampMax"))
			.HintText(LOCTEXT("ClampMaxHint", "Max"))
			.Font(DetailBuilder.GetDetailFont())
		]
	];

	// Read-Visibility (CrowdyVisibility): public | owner | hidden. Who may READ the attribute on the server.
	Category.AddCustomRow(LOCTEXT("VisibilityFilter", "Visibility"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("VisibilityLabel", "Visibility"))
		.ToolTipText(LOCTEXT("VisibilityTip",
			"Who may READ this attribute on the server: Public (anyone who can read the container), Owner (only "
			"the container's owner), or Hidden (server/admin only). Defaults to Public."))
		.Font(DetailBuilder.GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(200.f)
	[
		SNew(SComboButton)
		.OnGetMenuContent(this, &FCrowdyReplicatedVariableCustomization::BuildVisibilityPickerMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &FCrowdyReplicatedVariableCustomization::GetVisibilityText)
			.Font(DetailBuilder.GetDetailFont())
		]
	];
}

TSharedRef<SWidget> FCrowdyReplicatedVariableCustomization::BuildModeComboContent()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	const bool bReplicatable = IsVariableStateReplicatable();
	const bool bServerOwnable = IsVariableServerOwnable();
	// CrowdyState replicates only actor / actor-component entities. A plain-UObject CrowdyContainer Blueprint
	// reaches this customization for its Server Owned attributes, but cannot ride the view plane whatever the
	// property type, so Replicated must be greyed there too (not just when the type is unsupported).
	const bool bClassSupportsState = IsActorOrComponentBlueprint(Blueprint.Get());

	for (const ECrowdyReplicationMode Mode : SelectableModes())
	{
		// A mode is disabled (with an explanatory tooltip) when the variable's type or class cannot ride that
		// plane; None is always available so a mis-set variable can be cleared.
		bool bEnabled = true;
		FText Tooltip = ModeTooltipText(Mode);
		if (Mode == ECrowdyReplicationMode::Replicated && (!bReplicatable || !bClassSupportsState))
		{
			bEnabled = false;
			Tooltip = !bClassSupportsState
				? LOCTEXT("ModeReplicatedClassTip",
					"CrowdyState replicates only actor and actor-component entities. This class is neither, so the "
					"variable cannot ride the view plane; use Server Owned for a Game Model container.")
				: LOCTEXT("ModeReplicatedDisabledTip",
					"This variable's type cannot be CrowdyState-replicated (object references, containers, and "
					"static arrays are not supported).");
		}
		else if (Mode == ECrowdyReplicationMode::ServerOwned && !bServerOwnable)
		{
			bEnabled = false;
			Tooltip = LOCTEXT("ModeServerOwnedDisabledTip",
				"This variable's type cannot be a Server Owned Game Model attribute (only integers, floats, "
				"booleans, strings, and byte/enum values are supported).");
		}

		// Bind SetMode with the enum carried as a by-value payload (the FString-payload idiom the channel
		// picker in FCrowdyCustomEventCustomization uses); the CanExecute lambda greys the disabled entry.
		MenuBuilder.AddMenuEntry(
			ModeDisplayText(Mode),
			Tooltip,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &FCrowdyReplicatedVariableCustomization::SetMode, Mode),
				FCanExecuteAction::CreateLambda([bEnabled]() { return bEnabled; })));
	}

	return MenuBuilder.MakeWidget();
}

ECrowdyReplicationMode FCrowdyReplicatedVariableCustomization::GetCurrentMode() const
{
	// The marker present on the variable decides the mode. A field lives in exactly one plane, so at most one
	// marker is set; the mode-switch scrub keeps it that way.
	if (HasVariableMeta(CrowdyStateMetaKeys::Replicate))
	{
		return ECrowdyReplicationMode::Replicated;
	}
	if (HasVariableMeta(CrowdyGameModelMetaKeys::Model))
	{
		return ECrowdyReplicationMode::ServerOwned;
	}
	return ECrowdyReplicationMode::None;
}

FText FCrowdyReplicatedVariableCustomization::GetCurrentModeText() const
{
	return ModeDisplayText(GetCurrentMode());
}

void FCrowdyReplicatedVariableCustomization::SetMode(ECrowdyReplicationMode NewMode)
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return;
	}

	// Read the mode BEFORE any mutation so we know whether it actually changed (the panel rebuild below is
	// only needed on a real change; re-picking the same mode is a no-op for the row set).
	const ECrowdyReplicationMode PreviousMode = GetCurrentMode();

	// One atomic undo unit for the whole switch: the per-stamp FScopedTransactions inside StampVariableMeta and
	// ClearNativeReplication NEST into this outer one (UE commits only the outermost), so a single undo reverts
	// the ENTIRE mode change. Without it, one undo reverts only the last step and can leave a partial state
	// (e.g. native rep restored while CrowdyModel persists - the very dual-rep collapse the scrub prevents).
	FScopedTransaction ModeTransaction(LOCTEXT("ChangeReplicationModeTxn", "Change Crowdy Replication mode"));

	// Scrub the keys the previous mode owned that the new mode does not: this is the single place stale plane
	// metadata is removed, so a Server Owned -> Replicated switch can never leave a key override or Visibility
	// behind (the native Min/Max clamp is deliberately preserved: it is plain Unreal metadata, not a plane key),
	// and the shared CrowdyOnRep survives a plane-to-plane switch. Unit-tested in
	// CrowdyReplicationModeDecision::KeysToScrubOnSwitch; each stamp opens its own transaction, then one compile.
	for (const TCHAR* ScrubKey : CrowdyReplicationModeDecision::KeysToScrubOnSwitch(PreviousMode, NewMode))
	{
		StampVariableMeta(ScrubKey, TOptional<FString>(),
			LOCTEXT("ScrubModeKey", "Change Crowdy Replication mode"));
	}

	if (NewMode == ECrowdyReplicationMode::Replicated)
	{
		// Add the marker; the shared OnRep and (below) the heartbeat default are the only sub-options seeded.
		StampVariableMeta(CrowdyStateMetaKeys::Replicate, FString(),
			LOCTEXT("SetReplicated", "Set Crowdy Replication: Replicated"));

		EnsureDefaultOnRep();

		// Heartbeat defaults On for Blueprint authors, unlike C++ (which is opt-in, key absent by default): BP
		// authors expect "replicated = stays in sync", so pre-write CrowdyHeartbeat the FIRST time a variable
		// enters Replicated mode. Gated on the actual transition (PreviousMode != Replicated) so re-picking the
		// current mode never re-enables a heartbeat the user explicitly unchecked; the Advanced checkbox toggles
		// it thereafter, and switching away scrubs it.
		if (PreviousMode != ECrowdyReplicationMode::Replicated && !HasVariableMeta(CrowdyStateMetaKeys::Heartbeat))
		{
			StampVariableMeta(CrowdyStateMetaKeys::Heartbeat, FString(),
				LOCTEXT("SetHeartbeatDefault", "Set Crowdy Heartbeat"));
		}

		// Crowdy and native replication are mutually exclusive: force native rep off for this variable so both
		// cannot be active at once.
		ClearNativeReplication();

		RecompileForMetaChange(CrowdyStateMetaKeys::Replicate, /*bValueSet*/ true);
	}
	else if (NewMode == ECrowdyReplicationMode::ServerOwned)
	{
		// The Game Model marker. Min/Max (native clamp) and Visibility are authored via the sub-rows; an absent
		// Visibility reads as public (the type default), so nothing is pre-seeded. OnRep is the SAME shared key.
		StampVariableMeta(CrowdyGameModelMetaKeys::Model, FString(),
			LOCTEXT("SetServerOwned", "Set Crowdy Replication: Server Owned"));

		EnsureDefaultOnRep();

		// A Server Owned attribute is server truth, never also native-replicated.
		ClearNativeReplication();

		RecompileForMetaChange(CrowdyGameModelMetaKeys::Model, /*bValueSet*/ true);
	}
	else // None: the scrub above already removed every Crowdy key. Do NOT re-enable native rep (the user
	{    // re-enables it via the native combo if they want it back).
		RecompileForMetaChange(CrowdyStateMetaKeys::Replicate, /*bValueSet*/ false);
	}

	// Rebuild the panel when the mode actually changed: the mode-specific rows are structural (created only
	// for the current mode in BuildReplicationCategory), so a mode switch has to regenerate the layout, not just
	// re-evaluate a visibility attribute. RequestForceRefresh defers the rebuild to next tick, which is
	// required here: this runs inside the combo/menu execute callback, so an immediate ForceRefresh would tear
	// down and recreate the very Details widgets whose Slate callback is still on the stack (the same
	// re-entrancy reason RecompileForMetaChange avoids a synchronous compile). No object is being deleted, so
	// the immediate variant's "remove invalid references" rationale does not apply.
	if (PreviousMode != NewMode && PropertyUtilities.IsValid())
	{
		PropertyUtilities->RequestForceRefresh();
	}
}

FName FCrowdyReplicatedVariableCustomization::EnsureOnRepGraph()
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return NAME_None;
	}

	UBlueprint* BlueprintPtr = Blueprint.Get();

	// Match the engine's native RepNotify naming exactly (FBlueprintVarActionDetails::OnChangeReplication).
	const FString OnRepName = FString::Printf(TEXT("OnRep_%s"), *VariableProperty->GetName());
	const FName OnRepFName(*OnRepName);

	// If a usable parameterless function of that name already exists (the user wrote or overrode it), reuse it
	// and do NOT create a graph. Resolve on the skeleton class, which reflects newly-added function graphs
	// after a skeleton-only recompile.
	if (const UClass* SkeletonClass = BlueprintPtr->SkeletonGeneratedClass.Get())
	{
		if (const UFunction* Existing = SkeletonClass->FindFunctionByName(OnRepFName))
		{
			if (Existing->NumParms == 0 && Existing->GetReturnProperty() == nullptr)
			{
				return OnRepFName;
			}
		}
	}

	// Otherwise create it, guarding on FindObject first: CreateNewGraph renames a name-colliding UEdGraph aside
	// rather than returning it, so calling it unconditionally would clobber a user's existing OnRep body. This
	// mirrors the native path (BlueprintDetailsCustomization.cpp OnChangeReplication RepNotify branch).
	if (!FindObject<UEdGraph>(BlueprintPtr, *OnRepName))
	{
		FScopedTransaction Transaction(LOCTEXT("CreateOnRepGraph", "Create Crowdy RepNotify Function"));
		BlueprintPtr->Modify();

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BlueprintPtr, OnRepFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

		// bIsUserCreated = false and a null UClass signature produce a bare parameterless entry/result pair
		// (exactly the engine's native OnRep graph). AddFunctionGraph marks the Blueprint structurally modified.
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			BlueprintPtr, NewGraph, /*bIsUserCreated*/ false, static_cast<UClass*>(nullptr));
	}

	return OnRepFName;
}

void FCrowdyReplicatedVariableCustomization::EnsureDefaultOnRep()
{
	// Auto-create OnRep_<Var> and point CrowdyOnRep at it, but only when none is set yet, so a user-chosen name
	// is never overwritten. EnsureOnRepGraph itself refuses to clobber an existing OnRep body (it guards on
	// FindObject / FindFunctionByName before CreateNewGraph). Shared by Replicated and Server Owned.
	if (HasVariableMeta(CrowdyStateMetaKeys::OnRep))
	{
		return;
	}
	const FName OnRepName = EnsureOnRepGraph();
	if (!OnRepName.IsNone())
	{
		StampVariableMeta(CrowdyStateMetaKeys::OnRep, OnRepName.ToString(),
			LOCTEXT("SetOnRepAuto", "Set Crowdy RepNotify"));
	}
}

void FCrowdyReplicatedVariableCustomization::ClearNativeReplication()
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return;
	}

	UBlueprint* BlueprintPtr = Blueprint.Get();
	const FName VarName = VariableProperty->GetFName();

	FScopedTransaction Transaction(LOCTEXT("ClearNativeReplication", "Disable native replication (Crowdy)"));
	BlueprintPtr->Modify();

	// Clear the native replication + repnotify property flags through the same uint64* the engine's own
	// OnChangeReplication None-branch writes (there is deliberately no SetBlueprintVariablePropertyFlags).
	if (uint64* PropFlagPtr = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(BlueprintPtr, VarName))
	{
		*PropFlagPtr &= ~CPF_Net;
		*PropFlagPtr &= ~CPF_RepNotify;
	}

	// Clear the native RepNotify function name (separate BP-descriptor state, not a flag). This is the native
	// RepNotify binding, NOT the Crowdy CrowdyOnRep metadata, so it does not touch our own RepNotify.
	FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(BlueprintPtr, VarName, NAME_None);

	// Reset the native replication condition to COND_None (parity with the engine's None branch).
	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(BlueprintPtr, VarName);
	if (VarIndex != INDEX_NONE)
	{
		BlueprintPtr->NewVariables[VarIndex].ReplicationCondition = COND_None;
	}
}

bool FCrowdyReplicatedVariableCustomization::IsVariableStateReplicatable() const
{
	// Defer to the single shared classifier (now public CrowdyReplication API) rather than a local copy, so
	// this editor gate can never drift from what discovery (FCrowdyStateLayoutBuilder::BuildLayout) accepts.
	return FCrowdyStateLayoutBuilder::IsStateReplicatable(VariableProperty.Get());
}

EVisibility FCrowdyReplicatedVariableCustomization::GetTypeWarningVisibility() const
{
	return IsVariableStateReplicatable() ? EVisibility::Collapsed : EVisibility::Visible;
}

bool FCrowdyReplicatedVariableCustomization::IsVariableServerOwnable() const
{
	// A Server Owned attribute maps to a Game Model scalar/string/bool/enum value; anything else (struct,
	// object, container, static array) returns empty from the shared classifier. Reuse it so this editor gate
	// can never drift from what discovery (FCrowdyAttributeRegistry::DiscoverForClass) actually accepts.
	return !FCrowdyAttributeRegistry::MapPropertyToValueType(VariableProperty.Get()).IsEmpty();
}

bool FCrowdyReplicatedVariableCustomization::IsGameModelContainerClass() const
{
	// One shared container check (marker, or the compiled CrowdyContainer tag) so the dropdown gate and this note
	// can never disagree about container-hood for the same Blueprint.
	return CrowdyContainerMarker::IsGameModelContainerClass(Blueprint.Get());
}

EVisibility FCrowdyReplicatedVariableCustomization::GetContainerTagWarningVisibility() const
{
	// The note is only meaningful in Server Owned mode, and only when the class is not a container yet.
	return (GetCurrentMode() == ECrowdyReplicationMode::ServerOwned && !IsGameModelContainerClass())
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FReply FCrowdyReplicatedVariableCustomization::OnMakeContainerClicked()
{
	if (Blueprint.IsValid())
	{
		CrowdyContainerMarker::SetMarked(Blueprint.Get(), true);

		// Refresh so the now-redundant note collapses (IsGameModelContainerClass reads the marker immediately).
		if (PropertyUtilities.IsValid())
		{
			PropertyUtilities->RequestForceRefresh();
		}
	}
	return FReply::Handled();
}

FText FCrowdyReplicatedVariableCustomization::GetRepNotifyText() const
{
	return HasVariableMeta(CrowdyStateMetaKeys::OnRep)
		? FText::FromString(GetVariableMeta(CrowdyStateMetaKeys::OnRep))
		: FText::GetEmpty();
}

void FCrowdyReplicatedVariableCustomization::OnRepNotifyCommitted(
	const FText& NewText, ETextCommit::Type /*CommitType*/)
{
	const FString FunctionName = NewText.ToString().TrimStartAndEnd();
	WriteVariableMeta(CrowdyStateMetaKeys::OnRep,
		FunctionName.IsEmpty() ? TOptional<FString>() : TOptional<FString>(FunctionName),
		LOCTEXT("SetRepNotify", "Set Crowdy RepNotify"));
}

void FCrowdyReplicatedVariableCustomization::SetRepNotifyValue(FString FunctionName)
{
	WriteVariableMeta(CrowdyStateMetaKeys::OnRep,
		FunctionName.IsEmpty() ? TOptional<FString>() : TOptional<FString>(FunctionName),
		LOCTEXT("SetRepNotify", "Set Crowdy RepNotify"));
}

TSharedRef<SWidget> FCrowdyReplicatedVariableCustomization::BuildRepNotifyPickerMenu()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("RepNotifyNone", "(None)"),
		LOCTEXT("RepNotifyNoneTip", "No RepNotify function."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyReplicatedVariableCustomization::SetRepNotifyValue, FString())));

	// List only THIS Blueprint's own user-created functions that are parameterless (a CrowdyOnRep notify takes
	// no args). Blueprint->FunctionGraphs is the authoritative set of the BP's top-level functions (the engine
	// uses it the same way, e.g. SMyBlueprint category gathering), so it excludes inherited/native engine
	// functions and the ubergraph. Each graph resolves to its UFunction on the SkeletonGeneratedClass (not the
	// GeneratedClass): the skeleton reflects a just-added function graph after the skeleton-only recompile that
	// AddFunctionGraph triggers, so an auto-created OnRep_<Var> shows up here immediately, whereas the
	// GeneratedClass lags until a full Compile. The text box stays authoritative, so a not-yet-compiled name
	// can still be typed.
	const UBlueprint* BP = Blueprint.Get();
	const UClass* SkeletonClass = BP ? BP->SkeletonGeneratedClass.Get() : nullptr;
	TArray<FString> FunctionNames;
	if (BP && SkeletonClass)
	{
		for (const UEdGraph* FunctionGraph : BP->FunctionGraphs)
		{
			if (!FunctionGraph)
			{
				continue;
			}
			if (const UFunction* Function = SkeletonClass->FindFunctionByName(FunctionGraph->GetFName()))
			{
				// Parameterless and non-returning, matching CrowdyOnRep and the engine's native RepNotify check.
				if (Function->NumParms == 0 && Function->GetReturnProperty() == nullptr)
				{
					FunctionNames.AddUnique(Function->GetName());
				}
			}
		}
	}
	FunctionNames.Sort();

	if (FunctionNames.Num() == 0)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("RepNotifyNoFunctions", "No zero-parameter functions"),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
	}
	else
	{
		MenuBuilder.BeginSection(NAME_None, LOCTEXT("RepNotifyFunctionsSection", "Functions"));
		for (const FString& Name : FunctionNames)
		{
			MenuBuilder.AddMenuEntry(
				FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &FCrowdyReplicatedVariableCustomization::SetRepNotifyValue, Name)));
		}
		MenuBuilder.EndSection();
	}

	return MenuBuilder.MakeWidget();
}

FText FCrowdyReplicatedVariableCustomization::GetClampText(const TCHAR* ClampKey) const
{
	return HasVariableMeta(ClampKey) ? FText::FromString(GetVariableMeta(ClampKey)) : FText::GetEmpty();
}

void FCrowdyReplicatedVariableCustomization::OnClampCommitted(
	const FText& NewText, ETextCommit::Type /*CommitType*/, const TCHAR* ClampKey)
{
	const FString Trimmed = NewText.ToString().TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		// An empty entry removes the bound (no clamp on that side).
		WriteVariableMeta(ClampKey, TOptional<FString>(), LOCTEXT("ClearClamp", "Clear Crowdy Clamp"));
		return;
	}
	// A non-numeric entry is rejected (keep the previous value); the text box re-reads the stored value.
	if (!Trimmed.IsNumeric())
	{
		return;
	}
	WriteVariableMeta(ClampKey, Trimmed, LOCTEXT("SetClamp", "Set Crowdy Clamp"));
}

FText FCrowdyReplicatedVariableCustomization::GetVisibilityText() const
{
	// Absent CrowdyVisibility reads as the default "public".
	if (!HasVariableMeta(CrowdyGameModelMetaKeys::Visibility))
	{
		return FText::FromString(CrowdyGameModelMetaKeys::DefaultVisibility);
	}
	const FString Vis = GetVariableMeta(CrowdyGameModelMetaKeys::Visibility);
	return FText::FromString(Vis.IsEmpty() ? FString(CrowdyGameModelMetaKeys::DefaultVisibility) : Vis);
}

TSharedRef<SWidget> FCrowdyReplicatedVariableCustomization::BuildVisibilityPickerMenu()
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	auto AddVisibility = [this, &MenuBuilder](const TCHAR* Value, const FText& Tooltip)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(FString(Value)),
			Tooltip,
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(
				this, &FCrowdyReplicatedVariableCustomization::SetVisibilityValue, FString(Value))));
	};

	AddVisibility(TEXT("public"), LOCTEXT("VisPublicTip", "Anyone who can read the container may read this attribute."));
	AddVisibility(TEXT("owner"), LOCTEXT("VisOwnerTip", "Only the container's owner may read this attribute."));
	AddVisibility(TEXT("hidden"), LOCTEXT("VisHiddenTip", "Only the server or an admin may read this attribute."));

	return MenuBuilder.MakeWidget();
}

void FCrowdyReplicatedVariableCustomization::SetVisibilityValue(FString Visibility)
{
	// "public" is the default, so store it as an ABSENT key (keeps the asset clean and matches discovery, which
	// defaults an absent CrowdyVisibility to public). Any other value is written verbatim.
	const bool bDefault = (Visibility == CrowdyGameModelMetaKeys::DefaultVisibility);
	WriteVariableMeta(CrowdyGameModelMetaKeys::Visibility,
		bDefault ? TOptional<FString>() : TOptional<FString>(Visibility),
		LOCTEXT("SetVisibility", "Set Crowdy Visibility"));
}

ECheckBoxState FCrowdyReplicatedVariableCustomization::GetOwnerOnlyCheckState() const
{
	return HasVariableMeta(CrowdyStateMetaKeys::OwnerOnly) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyReplicatedVariableCustomization::OnOwnerOnlyCheckChanged(ECheckBoxState NewState)
{
	WriteVariableMeta(CrowdyStateMetaKeys::OwnerOnly,
		NewState == ECheckBoxState::Checked ? TOptional<FString>(FString()) : TOptional<FString>(),
		LOCTEXT("ToggleOwnerOnly", "Toggle Crowdy Owner Only"));
}

ECheckBoxState FCrowdyReplicatedVariableCustomization::GetManualDirtyCheckState() const
{
	return HasVariableMeta(CrowdyStateMetaKeys::ManualDirty) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyReplicatedVariableCustomization::OnManualDirtyCheckChanged(ECheckBoxState NewState)
{
	WriteVariableMeta(CrowdyStateMetaKeys::ManualDirty,
		NewState == ECheckBoxState::Checked ? TOptional<FString>(FString()) : TOptional<FString>(),
		LOCTEXT("ToggleManualDirty", "Toggle Crowdy Manual Dirty"));
}

ECheckBoxState FCrowdyReplicatedVariableCustomization::GetHeartbeatCheckState() const
{
	return HasVariableMeta(CrowdyStateMetaKeys::Heartbeat) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FCrowdyReplicatedVariableCustomization::OnHeartbeatCheckChanged(ECheckBoxState NewState)
{
	WriteVariableMeta(CrowdyStateMetaKeys::Heartbeat,
		NewState == ECheckBoxState::Checked ? TOptional<FString>(FString()) : TOptional<FString>(),
		LOCTEXT("ToggleHeartbeat", "Toggle Crowdy Heartbeat"));
}

bool FCrowdyReplicatedVariableCustomization::HasVariableMeta(const TCHAR* Key) const
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return false;
	}

	FString Unused;
	return FBlueprintEditorUtils::GetBlueprintVariableMetaData(
		Blueprint.Get(), VariableProperty->GetFName(), /*InLocalVarScope*/ nullptr, FName(Key), Unused);
}

FString FCrowdyReplicatedVariableCustomization::GetVariableMeta(const TCHAR* Key) const
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return FString();
	}

	FString Value;
	FBlueprintEditorUtils::GetBlueprintVariableMetaData(
		Blueprint.Get(), VariableProperty->GetFName(), /*InLocalVarScope*/ nullptr, FName(Key), Value);
	return Value;
}

void FCrowdyReplicatedVariableCustomization::StampVariableMeta(
	const TCHAR* Key, const TOptional<FString>& Value, const FText& TransactionLabel)
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return;
	}

	UBlueprint* BlueprintPtr = Blueprint.Get();
	const FName VarName = VariableProperty->GetFName();

	FScopedTransaction Transaction(TransactionLabel);
	BlueprintPtr->Modify();

	if (Value.IsSet())
	{
		// SetBlueprintVariableMetaData pokes the metadata straight onto the skeleton and generated FProperty
		// synchronously (so HasStateMeta sees it at once) and internally marks the Blueprint structurally
		// modified with a skeleton-only recompile.
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(
			BlueprintPtr, VarName, /*InLocalVarScope*/ nullptr, FName(Key), Value.GetValue());
	}
	else
	{
		FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
			BlueprintPtr, VarName, /*InLocalVarScope*/ nullptr, FName(Key));
	}
}

void FCrowdyReplicatedVariableCustomization::RecompileForMetaChange(const TCHAR* LoggedKey, bool bValueSet)
{
	if (!Blueprint.IsValid() || !VariableProperty.IsValid())
	{
		return;
	}

	UBlueprint* BlueprintPtr = Blueprint.Get();

	// Mark the Blueprint structurally modified + the package dirty, mirroring the "Crowdy Replicates" event
	// checkbox (FCrowdyCustomEventCustomization::SetReplicated). SetBlueprintVariableMetaData already put the
	// key on the live generated + skeleton FProperty synchronously, so discovery (CrowdyStateMetaKeys::
	// HasStateMeta) sees it at once. The user's next Compile is what fires the incremental
	// UpdateClassRepLayout (via the compiler extension -> OnBlueprintCompiled), no full sweep; this is the
	// same activation contract the event checkbox uses, and it deliberately avoids forcing a synchronous
	// FKismetEditorUtilities::CompileBlueprint inside this Slate callback (which would reinstance the class
	// and tear down the Details panel mid-handler).
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BlueprintPtr);
	BlueprintPtr->MarkPackageDirty();

	// The stamp above changed the metadata on the live property without replacing it, so nothing about the
	// class or the property has changed identity and the cached Game Model attribute tables cannot tell they
	// are out of date. Drop them, so the running apply path reads the new markers on its next pull the same
	// way it did when it re-read the metadata on every lookup. Without this, taking a variable off the server
	// plane leaves the server still writing to it and firing its notify until the next compile.
	FCrowdyModelAttributeLookup::InvalidateAll();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Variable '%s' Crowdy metadata '%s' %s. Recompile to activate."),
		*VariableProperty->GetFName().ToString(), LoggedKey, bValueSet ? TEXT("set") : TEXT("cleared"));
}

void FCrowdyReplicatedVariableCustomization::WriteVariableMeta(
	const TCHAR* Key, const TOptional<FString>& Value, const FText& TransactionLabel)
{
	StampVariableMeta(Key, Value, TransactionLabel);
	RecompileForMetaChange(Key, Value.IsSet());
}

#undef LOCTEXT_NAMESPACE
