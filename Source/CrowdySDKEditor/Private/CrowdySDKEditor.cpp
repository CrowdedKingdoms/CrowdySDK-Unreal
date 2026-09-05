#include "CrowdySDKEditor.h"

#include "BlueprintEditorModule.h"
#include "GameFramework/Actor.h"
#include "Replication/CrowdyMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "PropertyEditorModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "KismetNodes/SGraphNodeK2Event.h"
#include "KismetNodes/SGraphNodeK2Var.h"
#include "SNodePanel.h" // FOverlayBrushInfo
#include "Styling/SlateBrush.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetActions/CrowdyEffectAssetTypeActions.h"
#include "Compiler/CrowdyBlueprintCompileHooks.h"
#include "CrowdyEditorEventMeta.h"
#include "Baking/CrowdyRegistryBaker.h"
#include "CrowdyStudioModule.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Customizations/CrowdyCustomEventCustomization.h"
#include "Customizations/CrowdyEffectCustomization.h"
#include "Customizations/CrowdyEffectGraphNodeCustomizations.h"
#include "Customizations/CrowdyEffectMagnitudeCustomization.h"
#include "Customizations/CrowdyReplicatedVariableCustomization.h"
#include "Customizations/CrowdyReplicationMode.h"
#include "GameModel/CrowdyContainerAssetTags.h"
#include "GameModel/CrowdyContainerBlueprintExtension.h"
#include "GameModel/CrowdyEffectDuplicateFunctionIndex.h"
#include "GameModel/CrowdyRetagAssetsCommand.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphCompiler.h"
#include "Graph/SCrowdyEffectGraphNode.h"
#include "Menus/CrowdyContainerEditorToolbar.h"
#include "Nodes/CrowdyApplyEffectContainerResolver.h"
#include "Menus/CrowdyEffectEditorToolbar.h"
#include "Menus/CrowdyStructEditorToolbar.h"
#include "Menus/CrowdyStructContextMenu.h"
#include "Pins/CrowdyStatePropertyNamePin.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Settings/CrowdyEffectSyncSettings.h"
#include "CrowdyStudioSyncService.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "TimerManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"

IMPLEMENT_MODULE(FCrowdySDKEditorModule, CrowdySDKEditor)

DEFINE_LOG_CATEGORY(LogCrowdyEditor)

namespace
{
	static FText GetCrowdyRecipientSubtitle(ECrowdyEventRecipient Recipient)
	{
		switch (Recipient)
		{
		case ECrowdyEventRecipient::OwningClient:
			return FText::FromString(TEXT("\nCrowdy Owning Client\nExecutes Locally Only"));
		case ECrowdyEventRecipient::Host:
			return FText::FromString(TEXT("\nCrowdy Host\nExecutes on Host"));
		case ECrowdyEventRecipient::Multicast:
			// Channel transport every session-channel member, any distance, no decay.
			return FText::FromString(TEXT("\nCrowdy Multicast\nEveryone on the Channel"));
		case ECrowdyEventRecipient::SpatialMulticast:
		default:
			return FText::FromString(TEXT("\nCrowdy Spatial Multicast\nEveryone In Range"));
		}
	}

	// Subtitle drawn under a Crowdy-marked custom event / function entry on the graph, or empty
	// when the node has no Crowdy marking. A replicated event names who it routes to, the way
	// Unreal tags a replicated event; a struct handler is labelled as a receiver. The leading
	// newline drops it onto its own line beneath the node title.
	static FText GetCrowdyNodeSubtitle(const FKismetUserDeclaredFunctionMetadata& Meta)
	{
		if (HasCrowdyReplicatesMeta(Meta))
		{
			const FString RecipientMeta = Meta.HasMetaData(FName(CrowdyRpcMetaKeys::Recipient))
				? Meta.GetMetaData(FName(CrowdyRpcMetaKeys::Recipient))
				: FString();
			return GetCrowdyRecipientSubtitle(ResolveCrowdyRecipient(RecipientMeta));
		}

		return FText::GetEmpty();
	}

	static FText GetCrowdyNodeSubtitle(const UFunction* Function)
	{
		if (!Function)
		{
			return FText::GetEmpty();
		}

		if (CrowdyRpcMetaKeys::HasReplicatesMeta(Function))
		{
			return GetCrowdyRecipientSubtitle(
				ResolveCrowdyRecipient(Function->GetMetaData(CrowdyRpcMetaKeys::Recipient)));
		}

		return FText::GetEmpty();
	}

	static FText GetCrowdySourceNodeSubtitle(const UK2Node_CallFunction* CallNode)
	{
		if (!CallNode || CallNode->GetFunctionName() == NAME_None)
		{
			return FText::GetEmpty();
		}

		UBlueprint* Blueprint = CallNode->GetBlueprint();
		if (!Blueprint)
		{
			return FText::GetEmpty();
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(GraphNode))
				{
					if (GetFunctionEntryName(Entry) == CallNode->GetFunctionName())
					{
						return GetCrowdyNodeSubtitle(Entry->MetaData);
					}
				}
				else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(GraphNode))
				{
					if (CustomEvent->CustomFunctionName == CallNode->GetFunctionName())
					{
						return GetCrowdyNodeSubtitle(CustomEvent->GetUserDefinedMetaData());
					}
				}
			}
		}

		return FText::GetEmpty();
	}

	static FText GetCrowdyCallFunctionSubtitle(const UK2Node_CallFunction* CallNode)
	{
		if (!CallNode)
		{
			return FText::GetEmpty();
		}

		if (const UFunction* Function = CallNode->GetTargetFunction())
		{
			const FText FunctionSubtitle = GetCrowdyNodeSubtitle(Function);
			if (!FunctionSubtitle.IsEmpty())
			{
				return FunctionSubtitle;
			}
		}

		return GetCrowdySourceNodeSubtitle(CallNode);
	}

	class SGraphNodeCrowdyCustomEvent : public SGraphNodeK2Event
	{
	public:
		SLATE_BEGIN_ARGS(SGraphNodeCrowdyCustomEvent) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, UK2Node_CustomEvent* InNode)
		{
			GraphNode = InNode;
			SetCursor(EMouseCursor::CardinalCross);
			UpdateGraphNode();
			CachedSubtitle = GetCrowdySubtitleText();
		}

		// Rebuild the node when its Crowdy subtitle changes e.g. the recipient dropdown in the
		// details panel so the Crowdy execution label updates live, without waiting for a recompile.
		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
		{
			SGraphNodeK2Event::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

			const FText Current = GetCrowdySubtitleText();
			if (!Current.EqualTo(CachedSubtitle))
			{
				CachedSubtitle = Current;
				UpdateGraphNode();
			}
		}

	protected:
		virtual TSharedRef<SWidget> CreateTitleWidget(
			TSharedPtr<SNodeTitle> NodeTitle) override
		{
			TSharedRef<SWidget> TitleWidget =
				SGraphNodeK2Default::CreateTitleWidget(NodeTitle);

			TitleWidget->SetVisibility(MakeAttributeSP(
				this, &SGraphNodeCrowdyCustomEvent::GetTitleVisibility));

			if (NodeTitle.IsValid())
			{
				NodeTitle->SetVisibility(MakeAttributeSP(
					this,
					&SGraphNodeCrowdyCustomEvent::GetDefaultSubtitleVisibility));
			}

			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					TitleWidget
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SNodeTitle, GraphNode)
					.Visibility(this,
						&SGraphNodeCrowdyCustomEvent::GetCrowdySubtitleVisibility)
					.Text(this,
						&SGraphNodeCrowdyCustomEvent::GetCrowdySubtitleText)
				];
		}

	private:
		FText GetCrowdySubtitleText() const
		{
			UK2Node_CustomEvent* Node = Cast<UK2Node_CustomEvent>(GraphNode);
			return Node ? GetCrowdyNodeSubtitle(Node->GetUserDefinedMetaData()) : FText::GetEmpty();
		}

		bool HasCrowdySubtitle() const
		{
			return !GetCrowdySubtitleText().IsEmpty();
		}

		EVisibility GetTitleVisibility() const
		{
			return UseLowDetailNodeTitles()
				? EVisibility::Hidden
				: EVisibility::Visible;
		}

		EVisibility GetDefaultSubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Hidden;
			}

			return HasCrowdySubtitle()
				? EVisibility::Collapsed
				: EVisibility::Visible;
		}

		EVisibility GetCrowdySubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Collapsed;
			}

			return HasCrowdySubtitle()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		}

		// Last subtitle shown, so Tick can detect a details-panel change and rebuild the node.
		FText CachedSubtitle;
	};

	class SGraphNodeCrowdyCallFunction : public SGraphNodeK2Default
	{
	public:
		SLATE_BEGIN_ARGS(SGraphNodeCrowdyCallFunction) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, UK2Node_CallFunction* InNode)
		{
			GraphNode = InNode;
			SetCursor(EMouseCursor::CardinalCross);
			UpdateGraphNode();
			CachedSubtitle = GetCrowdySubtitleText();
		}

		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
		{
			SGraphNodeK2Default::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

			const FText Current = GetCrowdySubtitleText();
			if (!Current.EqualTo(CachedSubtitle))
			{
				CachedSubtitle = Current;
				UpdateGraphNode();
			}
		}

	protected:
		virtual TSharedRef<SWidget> CreateTitleWidget(
			TSharedPtr<SNodeTitle> NodeTitle) override
		{
			TSharedRef<SWidget> TitleWidget =
				SGraphNodeK2Default::CreateTitleWidget(NodeTitle);

			TitleWidget->SetVisibility(MakeAttributeSP(
				this, &SGraphNodeCrowdyCallFunction::GetTitleVisibility));

			if (NodeTitle.IsValid())
			{
				NodeTitle->SetVisibility(MakeAttributeSP(
					this,
					&SGraphNodeCrowdyCallFunction::GetDefaultSubtitleVisibility));
			}

			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					TitleWidget
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SNodeTitle, GraphNode)
					.Visibility(this,
						&SGraphNodeCrowdyCallFunction::GetCrowdySubtitleVisibility)
					.Text(this,
						&SGraphNodeCrowdyCallFunction::GetCrowdySubtitleText)
				];
		}

	private:
		FText GetCrowdySubtitleText() const
		{
			const UK2Node_CallFunction* Node = Cast<UK2Node_CallFunction>(GraphNode);
			return Node ? GetCrowdyCallFunctionSubtitle(Node) : FText::GetEmpty();
		}

		bool HasCrowdySubtitle() const
		{
			return !GetCrowdySubtitleText().IsEmpty();
		}

		EVisibility GetTitleVisibility() const
		{
			return UseLowDetailNodeTitles()
				? EVisibility::Hidden
				: EVisibility::Visible;
		}

		EVisibility GetDefaultSubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Hidden;
			}

			return HasCrowdySubtitle()
				? EVisibility::Collapsed
				: EVisibility::Visible;
		}

		EVisibility GetCrowdySubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Collapsed;
			}

			return HasCrowdySubtitle()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		}

		FText CachedSubtitle;
	};

	// True when Node is a Blueprint variable get/set node whose underlying property is CrowdyState-
	// replicated. CrowdyState and native replication are mutually exclusive, so a Crowdy-replicated variable
	// has its CPF_Net flag cleared (FCrowdyReplicatedVariableCustomization::ClearNativeReplication); that is
	// exactly why the engine's own UK2Node_Variable::GetCornerIcon never lights up the replication badge for
	// these variables, and why we supply our own overlay below. Resolves against the generated class, falling
	// back to the skeleton, which reflects a just-stamped metadata change before a full compile. HasStateMeta
	// is editor-only (live metadata), which is all this editor-time icon needs.
	static bool IsCrowdyReplicatedVariableNode(const UEdGraphNode* Node)
	{
		const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
		if (!VariableNode || !VariableNode->DrawNodeAsVariable())
		{
			return false;
		}

		const FProperty* Property = VariableNode->GetPropertyForVariable();
		if (!Property)
		{
			Property = VariableNode->GetPropertyForVariableFromSkeleton();
		}
		return CrowdyStateMetaKeys::HasStateMeta(Property);
	}

	// A variable get/set node widget that draws the engine's replication corner badge for a CrowdyState-
	// replicated variable, exactly the way Unreal badges a natively-replicated variable. It is identical to
	// the stock SGraphNodeK2Var (its base) in every other respect and only adds one overlay brush. The badge
	// is re-evaluated each paint in GetOverlayBrushes, so it also disappears live if the variable stops being
	// Crowdy-replicated while the widget still exists.
	class SGraphNodeCrowdyVariable : public SGraphNodeK2Var
	{
	public:
		SLATE_BEGIN_ARGS(SGraphNodeCrowdyVariable) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, UK2Node* InNode)
		{
			// Mirror SGraphNodeK2Var::Construct (there is no base initializer to chain to).
			GraphNode = InNode;
			SetCursor(EMouseCursor::CardinalCross);
			UpdateGraphNode();
		}

		virtual void GetOverlayBrushes(
			bool bSelected, const FVector2f& WidgetSize, TArray<FOverlayBrushInfo>& Brushes) const override
		{
			SGraphNodeK2Var::GetOverlayBrushes(bSelected, WidgetSize, Brushes);

			if (!IsCrowdyReplicatedVariableNode(GraphNode))
			{
				return;
			}

			// Same brush and top-right placement the engine uses for a replicated variable's corner icon
			// (SGraphNodeK2Base::GetOverlayBrushes). CrowdyState leaves CPF_Net clear, so the base never adds
			// this brush itself; there is no double-draw.
			FOverlayBrushInfo CrowdyReplicationOverlay;
			CrowdyReplicationOverlay.Brush = GetStyleSet().GetBrush(TEXT("Graph.Replication.Replicated"));
			if (CrowdyReplicationOverlay.Brush)
			{
				CrowdyReplicationOverlay.OverlayOffset.X =
					(WidgetSize.X - (CrowdyReplicationOverlay.Brush->ImageSize.X / 2.f)) - 3.f;
				CrowdyReplicationOverlay.OverlayOffset.Y =
					(CrowdyReplicationOverlay.Brush->ImageSize.Y / -2.f) + 2.f;
				Brushes.Add(CrowdyReplicationOverlay);
			}
		}
	};

	class FCrowdyGraphPanelNodeFactory : public FGraphPanelNodeFactory
	{
	public:
		virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override
		{
			if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				return SNew(SGraphNodeCrowdyCustomEvent, CustomEvent);
			}

			if (UK2Node_CallFunction* CallFunction = Cast<UK2Node_CallFunction>(Node))
			{
				if (!GetCrowdyCallFunctionSubtitle(CallFunction).IsEmpty())
				{
					return SNew(SGraphNodeCrowdyCallFunction, CallFunction);
				}
			}

			// Badge a CrowdyState-replicated variable's get/set nodes; leave every other variable node to the
			// default factory (return nullptr) so we never override an unrelated variable-node customization.
			if (IsCrowdyReplicatedVariableNode(Node))
			{
				return SNew(SGraphNodeCrowdyVariable, Cast<UK2Node>(Node));
			}

			return nullptr;
		}
	};
}


// Per-effect Game Model sync: the asset-editor toolbar drives it; the pre-play check consults the cached status.

namespace
{
	FDelegateHandle GEffectPrePIEHandle;
	FDelegateHandle GEffectEndPIEHandle;
	FDelegateHandle GEffectPropertyChangedHandle;
	FDelegateHandle GObjectRenamedHandle;

	// The one live drift prompt, if any. The prompt has to survive until the user answers it, so it is deliberately
	// non-expiring; that also means nothing takes it down on its own, and Play can be pressed any number of times.
	// Holding the live one here is what keeps a second press from leaving another permanent copy on the list.
	TWeakPtr<SNotificationItem> GEffectDriftNotification;

	// Visual node factory for the Crowdy effect graph's inline node-body editors and error badges. Registered in
	// StartupModule and unregistered in ShutdownModule. Held here rather than as a module member so the registration
	// stays self-contained.
	TSharedPtr<FGraphPanelNodeFactory> GEffectGraphNodeFactory;

	// An edit may change what an effect compiles to, so a cached "Synced" is no longer trustworthy: reset it to
	// Unknown. The asset toolbar re-reads a fresh status when it next rebuilds, and the pre-play check treats Unknown
	// as "not known to be out of sync", so an edit never raises a false drift prompt.
	void InvalidateEffectStatusOnEdit(UObject* Object, FPropertyChangedEvent& /*Event*/)
	{
		if (const UCrowdyEffect* Effect = Cast<UCrowdyEffect>(Object))
		{
			CrowdyStudioSyncService::InvalidateCachedStatus(Effect);
		}
	}

	// Move every Crowdy RepNotify binding that named a just-renamed function graph onto its new name. Unreal's own
	// graph rename repoints every reference it owns; CrowdyOnRep names its notify in metadata instead, so without
	// this the binding is left naming a function that no longer exists and the variable silently stops notifying.
	//
	// The write is queued for the next tick because this runs from UObject::PostRename, part-way through
	// FBlueprintEditorUtils::RenameGraph while it is still walking the graph's nodes: stamping the metadata there
	// recompiles the skeleton and reconstructs those nodes underneath it. Weak-bound, so a Blueprint discarded
	// before the tick does nothing.
	void RetargetOnRepBindingsAfterGraphRename(UObject* Object, UObject* /*OldOuter*/, FName OldName)
	{
		UEdGraph* Graph = Cast<UEdGraph>(Object);
		if (!Graph || !GEditor)
		{
			return;
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter());
		if (!Blueprint || !Blueprint->FunctionGraphs.Contains(Graph))
		{
			return;
		}

		// A rename during load or compilation is the engine moving graphs around (regeneration, an old graph moved
		// aside), not an author renaming their function, and the bindings are already consistent with it.
		if (Blueprint->bIsRegeneratingOnLoad || Blueprint->bBeingCompiled)
		{
			return;
		}

		// Resolved up front: the stamp below recompiles the skeleton, which rebuilds the descriptions this reads.
		const FName NewName = Graph->GetFName();
		const FName OnRepKey(CrowdyStateMetaKeys::OnRep);
		TArray<FName> Bound;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			// GetMetaData asserts on an absent key, so the presence check is load-bearing rather than a fast path.
			if (Variable.HasMetaData(OnRepKey)
				&& CrowdyReplicationModeDecision::OnRepFollowsGraphRename(
					Variable.GetMetaData(OnRepKey), OldName, NewName))
			{
				Bound.Add(Variable.VarName);
			}
		}
		if (Bound.IsEmpty())
		{
			return;
		}

		TWeakObjectPtr<UBlueprint> WeakBlueprint(Blueprint);
		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(Blueprint,
			[WeakBlueprint, Bound, OldName, NewName]()
			{
				UBlueprint* Target = WeakBlueprint.Get();
				if (!Target)
				{
					return;
				}
				for (const FName VarName : Bound)
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Target, VarName, /*InLocalVarScope*/ nullptr,
						FName(CrowdyStateMetaKeys::OnRep), NewName.ToString());
				}
				UE_LOG(LogCrowdyEditor, Log,
					TEXT("[CrowdySDK] Function '%s' renamed to '%s'; moved %d Crowdy RepNotify binding(s) with it."),
					*OldName.ToString(), *NewName.ToString(), Bound.Num());
			}));
	}

	void ExpireNotification(TWeakPtr<SNotificationItem> WeakItem)
	{
		if (const TSharedPtr<SNotificationItem> Item = WeakItem.Pin())
		{
			Item->SetExpireDuration(0.0f);
			Item->ExpireAndFadeout();
		}
	}

	// Report a Sync now once the whole set has answered. A sync can fail for reasons the user has to act on (signed
	// out, no app configured, a compile error), and a failed one leaves the effect out of sync, so an unreported
	// failure reads as "the button did nothing" and the prompt returns on the next Play with no explanation.
	void ReportSyncOutcome(const TSharedRef<int32>& Remaining, const TSharedRef<TArray<FString>>& Failures, int32 Total)
	{
		if (--(*Remaining) > 0)
		{
			return;
		}

		const bool bOk = Failures->IsEmpty();
		FNotificationInfo Info(FText::FromString(bOk
			? FString::Printf(TEXT("Synced %d Game Model effect(s) to the server."), Total)
			: FString::Printf(TEXT("Synced %d of %d effect(s). %s"),
				Total - Failures->Num(), Total, *FString::Join(*Failures, TEXT(" ")))));
		Info.bFireAndForget = true;
		Info.ExpireDuration = bOk ? 4.0f : 10.0f;

		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	void SyncOutOfSyncEffects(TArray<TWeakObjectPtr<UCrowdyEffect>> Effects, TWeakPtr<SNotificationItem> WeakItem)
	{
		ExpireNotification(WeakItem);

		TArray<UCrowdyEffect*> Live;
		for (const TWeakObjectPtr<UCrowdyEffect>& Weak : Effects)
		{
			if (UCrowdyEffect* Effect = Weak.Get())
			{
				Live.Add(Effect);
			}
		}
		if (Live.IsEmpty())
		{
			return;
		}

		// Fixed up front so the join fires exactly once, after every sync has answered and never before: a guarded
		// effect answers synchronously, from inside SyncEffect, before its siblings are even issued.
		const int32 Total = Live.Num();
		const TSharedRef<int32> Remaining = MakeShared<int32>(Total);
		const TSharedRef<TArray<FString>> Failures = MakeShared<TArray<FString>>();
		for (UCrowdyEffect* Effect : Live)
		{
			CrowdyStudioSyncService::SyncEffect(Effect,
				[Remaining, Failures, Total](bool bOk, const FString& Message)
				{
					if (!bOk)
					{
						Failures->Add(Message);
					}
					ReportSyncOutcome(Remaining, Failures, Total);
				});
		}
	}

	// When Play is pressed, consult ONLY the cached effect statuses (never a blocking server round-trip) and, if any
	// effects are out of sync, offer a one-click "Sync now". Gated by the opt-out project setting.
	void CheckEffectDriftBeforePlay(const bool /*bIsSimulating*/)
	{
		// Whatever this press decides, the previous press's prompt is about a session that has already been and gone.
		// Taken down first, and unconditionally, so a prompt can never outlive the drift that raised it: the setting
		// may have been turned off, or the drift may be gone, and neither reaches the code below.
		ExpireNotification(GEffectDriftNotification);
		GEffectDriftNotification.Reset();

		const UCrowdyEffectSyncSettings* Settings = GetDefault<UCrowdyEffectSyncSettings>();
		if (!Settings || !Settings->bCheckEffectDriftBeforePlay)
		{
			return;
		}

		const TArray<TWeakObjectPtr<UCrowdyEffect>> OutOfSync = CrowdyStudioSyncService::GetCachedOutOfSyncEffects();
		if (OutOfSync.Num() == 0)
		{
			return;
		}

		const TSharedRef<TWeakPtr<SNotificationItem>> ItemHolder = MakeShared<TWeakPtr<SNotificationItem>>();

		FNotificationInfo Info(FText::FromString(FString::Printf(
			TEXT("%d Game Model effect(s) out of sync with the server."), OutOfSync.Num())));
		Info.bFireAndForget = false;
		Info.FadeOutDuration = 0.5f;
		Info.ExpireDuration = 0.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			FText::FromString(TEXT("Sync now")),
			FText::FromString(TEXT("Sync each out-of-sync effect to the server.")),
			FSimpleDelegate::CreateLambda([OutOfSync, ItemHolder]() { SyncOutOfSyncEffects(OutOfSync, *ItemHolder); }),
			SNotificationItem::CS_None));
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			FText::FromString(TEXT("Dismiss")),
			FText::GetEmpty(),
			FSimpleDelegate::CreateLambda([ItemHolder]() { ExpireNotification(*ItemHolder); }),
			SNotificationItem::CS_None));

		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			*ItemHolder = Item;
			GEffectDriftNotification = Item;
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}

	// When Play ends, take the prompt down and re-derive each flagged effect's status from the server. A cached status
	// has no other reader that can correct it -- the asset toolbar only reads one that is Unknown -- so a verdict made
	// false somewhere else (a console Apply, a second editor, a teammate) would otherwise raise the prompt on every
	// Play forever. Re-reading here keeps the Play path itself free of any server round-trip, and costs nothing at all
	// in the normal case, where nothing is flagged.
	void RefreshEffectDriftAfterPlay(const bool /*bIsSimulating*/)
	{
		ExpireNotification(GEffectDriftNotification);
		GEffectDriftNotification.Reset();

		const UCrowdyEffectSyncSettings* Settings = GetDefault<UCrowdyEffectSyncSettings>();
		if (!Settings || !Settings->bCheckEffectDriftBeforePlay)
		{
			return;
		}

		for (const TWeakObjectPtr<UCrowdyEffect>& Weak : CrowdyStudioSyncService::GetCachedOutOfSyncEffects())
		{
			if (UCrowdyEffect* Effect = Weak.Get())
			{
				CrowdyStudioSyncService::RequestEffectSyncStatus(Effect, nullptr);
			}
		}
	}
}


// Module lifecycle

void FCrowdySDKEditorModule::StartupModule()
{
	RegisterStructContextMenu();
	FCrowdyContainerEditorToolbar::Register();
	FCrowdyEffectEditorToolbar::Register();
	GEffectPrePIEHandle = FEditorDelegates::PreBeginPIE.AddStatic(&CheckEffectDriftBeforePlay);
	GEffectEndPIEHandle = FEditorDelegates::EndPIE.AddStatic(&RefreshEffectDriftAfterPlay);
	GEffectPropertyChangedHandle =
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&InvalidateEffectStatusOnEdit);
	RegisterFunctionEntryCustomization();
	RegisterEffectCustomization();
	RegisterEffectAssetTypeActions();
	RegisterVariableCustomization();
	RegisterGraphNodeFactory();
	RegisterGraphPinFactory();

	// The effect-graph node widgets (inline body editors + error badges) render through their own visual node factory.
	GEffectGraphNodeFactory = MakeShared<FCrowdyEffectGraphNodeFactory>();
	FEdGraphUtilities::RegisterVisualNodeFactory(GEffectGraphNodeFactory);
	RegisterCompilerExtension();
	InstallBlueprintCompileHooks();
	UCrowdyRegistryBaker::Register();

	// The Registry Inspector now lives in the CrowdyStudio console (Registry page). The deep rebuild
	// is editor-only, so hand the console a hook into the baker rather than CrowdyStudio depending on
	// this module. The captureless lambda is cleared in ShutdownModule so it never dangles.
	CrowdyStudioRegistry::SetRebuildHook(
		[](TFunction<void()> OnComplete) { UCrowdyRegistryBaker::RebuildAsync(MoveTemp(OnComplete)); });

	// Let the console's schema sync bring in container Blueprints that were marked but never opened this session, so
	// an unopened container still reaches the plan. Streamed rather than force-loaded: this runs on the most-used
	// button on that page, and the synchronous form froze the editor for the whole load.
	CrowdyStudioRegistry::SetLoadContainerAssetsHook(
		[](TFunction<void()> OnComplete) { UCrowdyRegistryBaker::LoadAllTaggedAssetsAsync(MoveTemp(OnComplete)); });

	// Publish the container scan tags on every Blueprint save, so the Game Model schema scan can answer "is this a
	// container" straight from the asset registry instead of loading the package to find out.
	CrowdyContainerAssetTags::Register();

	// The one-time migration that catches the rest of the project up to those tags in a single deliberate pass.
	CrowdyRetagAssetsCommand::Register();

	// Let a Graph-sourced UCrowdyEffect (in the runtime CrowdyReplication module) compile through the editor-only
	// graph compiler. Same no-cycle hook pattern: the runtime module holds the entry point, this editor module
	// supplies the implementation. Cleared in ShutdownModule so the captureless lambda never dangles.
	CrowdyEffectGraphCompile::SetCompileHook(
		[](const UEdGraph* Graph, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics) -> FCrowdyEffectSpec
		{
			return FCrowdyEffectGraphCompiler::CompileToSpec(Cast<UCrowdyEffectGraph>(Graph), OutDiagnostics);
		});

	// Let a UCrowdyEffect (in the runtime CrowdyReplication module) ask IsDataValid's cross-asset duplicate
	// function-name question through the same no-cycle hook pattern: the asset-registry sweep this needs is
	// editor-only.
	CrowdyEffectDuplicateFunctionIndex::Register();

	// Let the Apply Crowdy Effect nodes judge an unwired Target against the persisted container marker rather than
	// the class tag alone. The marker is true from the moment a Blueprint is marked, while the tag only appears on
	// the generated class after a compile, so without this a marked container is reported as not one. Same no-cycle
	// hook pattern as above; cleared in ShutdownModule so the captureless lambda never dangles.
	CrowdyApplyEffectNodeShared::SetContainerBlueprintResolver(
		[](const UBlueprint* Blueprint) { return CrowdyContainerMarker::IsGameModelContainerClass(Blueprint); });

	GObjectRenamedHandle =
		FCoreUObjectDelegates::OnObjectRenamed.AddStatic(&RetargetOnRepBindingsAfterGraphRename);
}

void FCrowdySDKEditorModule::ShutdownModule()
{
	CrowdyStudioRegistry::SetRebuildHook(nullptr);
	CrowdyStudioRegistry::SetLoadContainerAssetsHook(nullptr);
	CrowdyEffectGraphCompile::SetCompileHook(nullptr);
	CrowdyContainerAssetTags::Unregister();
	CrowdyRetagAssetsCommand::Unregister();
	CrowdyEffectDuplicateFunctionIndex::Unregister();
	CrowdyApplyEffectNodeShared::SetContainerBlueprintResolver(nullptr);
	CrowdyBlueprintCompileHooks::SetContainerStampHook(nullptr);
	CrowdyBlueprintCompileHooks::SetClassCompiledHook(nullptr);

	if (FPropertyEditorModule* PM =
		FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PM->UnregisterCustomClassLayout("K2Node_CustomEvent");
		PM->UnregisterCustomClassLayout("CrowdyEffect");
		PM->UnregisterCustomPropertyTypeLayout("CrowdyEffectMagnitude");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_Attribute");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_ReadRef");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_Tuning");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_Call");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_ServerCall");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_Constant");
		PM->UnregisterCustomClassLayout("CrowdyEffectGraphNode_Result");
		PM->UnregisterCustomPropertyTypeLayout("CrowdyEffectGraphWrite");
		PM->UnregisterCustomPropertyTypeLayout("CrowdyEffectGraphRequire");
	}

	if (BlueprintVariableCustomizationHandle.IsValid())
	{
		if (FBlueprintEditorModule* BlueprintEditorModule =
			FModuleManager::GetModulePtr<FBlueprintEditorModule>("Kismet"))
		{
			BlueprintEditorModule->UnregisterVariableCustomization(
				FProperty::StaticClass(), BlueprintVariableCustomizationHandle);
		}
		BlueprintVariableCustomizationHandle.Reset();
	}

	if (GEffectPrePIEHandle.IsValid())
	{
		FEditorDelegates::PreBeginPIE.Remove(GEffectPrePIEHandle);
		GEffectPrePIEHandle.Reset();
	}
	if (GEffectEndPIEHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(GEffectEndPIEHandle);
		GEffectEndPIEHandle.Reset();
	}
	// The prompt's buttons call back into this module, so it must not outlive it.
	ExpireNotification(GEffectDriftNotification);
	GEffectDriftNotification.Reset();

	if (GEffectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(GEffectPropertyChangedHandle);
		GEffectPropertyChangedHandle.Reset();
	}
	if (GObjectRenamedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectRenamed.Remove(GObjectRenamedHandle);
		GObjectRenamedHandle.Reset();
	}

	if (EffectAssetTypeActions.IsValid())
	{
		if (FAssetToolsModule* AssetToolsModule = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools"))
		{
			AssetToolsModule->Get().UnregisterAssetTypeActions(EffectAssetTypeActions.ToSharedRef());
		}
		EffectAssetTypeActions.Reset();
	}

	FCrowdyStructEditorToolbar::Unregister();
	FCrowdyContainerEditorToolbar::Unregister();
	FCrowdyEffectEditorToolbar::Unregister();
	FCrowdyStructContextMenu::Unregister();

	if (GraphNodeFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);
		GraphNodeFactory.Reset();
	}

	if (GEffectGraphNodeFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualNodeFactory(GEffectGraphNodeFactory);
		GEffectGraphNodeFactory.Reset();
	}

	if (GraphPinFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualPinFactory(GraphPinFactory);
		GraphPinFactory.Reset();
	}

	if (GEditor && CompiledHandle.IsValid())
	{
		GEditor->OnBlueprintCompiled().Remove(CompiledHandle);
		CompiledHandle.Reset();
	}
}


// Registration helpers

void FCrowdySDKEditorModule::RegisterStructContextMenu()
{
	FCrowdyStructContextMenu::Register();
	FCrowdyStructEditorToolbar::Register();
}

void FCrowdySDKEditorModule::RegisterFunctionEntryCustomization()
{
	FPropertyEditorModule& PM =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	PM.RegisterCustomClassLayout(
		"K2Node_CustomEvent",
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FCrowdyCustomEventCustomization::MakeInstance));
}

void FCrowdySDKEditorModule::RegisterEffectCustomization()
{
	FPropertyEditorModule& PM =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	PM.RegisterCustomClassLayout(
		"CrowdyEffect",
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FCrowdyEffectCustomization::MakeInstance));

	PM.RegisterCustomPropertyTypeLayout(
		"CrowdyEffectMagnitude",
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FCrowdyEffectMagnitudeCustomization::MakeInstance));

	// The graph-editor "Selected Node" panel gets attribute / magnitude / function pick-lists in place of hand-typed
	// names, and the Result node gets its one-click return shortcut. One shared detail customization covers all of
	// them; the Result node's writes additionally get a compact one-row struct customization with the attribute
	// pick-list inline.
	// A class layout is registered per exact class name, not inherited, so the Server Call node needs its own entry
	// even though it derives from the Call node.
	for (const TCHAR* NodeClass : { TEXT("CrowdyEffectGraphNode_Attribute"), TEXT("CrowdyEffectGraphNode_ReadRef"),
		TEXT("CrowdyEffectGraphNode_Tuning"), TEXT("CrowdyEffectGraphNode_Call"),
		TEXT("CrowdyEffectGraphNode_ServerCall"), TEXT("CrowdyEffectGraphNode_Constant"),
		TEXT("CrowdyEffectGraphNode_Result") })
	{
		PM.RegisterCustomClassLayout(
			NodeClass,
			FOnGetDetailCustomizationInstance::CreateStatic(&FCrowdyEffectGraphNodeCustomization::MakeInstance));
	}

	PM.RegisterCustomPropertyTypeLayout(
		"CrowdyEffectGraphWrite",
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FCrowdyEffectGraphWriteCustomization::MakeInstance));

	PM.RegisterCustomPropertyTypeLayout(
		"CrowdyEffectGraphRequire",
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FCrowdyEffectGraphRequireCustomization::MakeInstance));
}

void FCrowdySDKEditorModule::RegisterEffectAssetTypeActions()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Group under the same "Crowdy" advanced category the effect factory uses, so the asset editor and the create
	// menu stay consistent.
	const FName CategoryKey(TEXT("Crowdy"));
	EAssetTypeCategories::Type Category = AssetTools.FindAdvancedAssetCategory(CategoryKey);
	if (Category == EAssetTypeCategories::Misc)
	{
		Category = AssetTools.RegisterAdvancedAssetCategory(
			CategoryKey, NSLOCTEXT("CrowdySDKEditor", "CrowdyAssetCategory", "Crowdy"));
	}

	EffectAssetTypeActions = MakeShared<FCrowdyEffectAssetTypeActions>(Category);
	AssetTools.RegisterAssetTypeActions(EffectAssetTypeActions.ToSharedRef());
}

void FCrowdySDKEditorModule::RegisterVariableCustomization()
{
	// The Blueprint editor lives in the "Kismet" module. It may not be loaded yet (or at all, in a
	// commandlet), so probe rather than load-checked. FProperty::StaticClass() covers every variable type;
	// MakeInstance itself filters to actor / actor-component or Game Model container Blueprints and greys each
	// mode per the variable's type.
	if (FBlueprintEditorModule* BlueprintEditorModule =
		FModuleManager::GetModulePtr<FBlueprintEditorModule>("Kismet"))
	{
		BlueprintVariableCustomizationHandle = BlueprintEditorModule->RegisterVariableCustomization(
			FProperty::StaticClass(),
			FOnGetVariableCustomizationInstance::CreateStatic(
				&FCrowdyReplicatedVariableCustomization::MakeInstance));
	}
}

void FCrowdySDKEditorModule::RegisterCompilerExtension()
{
	if (!GEditor) return;

	CompiledHandle = GEditor->OnBlueprintCompiled().AddStatic(
		&FCrowdySDKEditorModule::OnBlueprintCompiled);
}

void FCrowdySDKEditorModule::InstallBlueprintCompileHooks()
{
	CrowdyBlueprintCompileHooks::SetContainerStampHook(
		[](const UBlueprint* Blueprint, UClass* NewClass)
		{
			// A Blueprint marked as a Game Model container (its persisted CrowdyContainerBlueprintExtension)
			// stamps the CrowdyContainer type-name tag on its class, so discovery, the schema sync, and the
			// bake find it; an unmarked class has any stale tag removed. Same opt-in-drives-the-tag pattern
			// as the entity marker the compile pass stamps immediately before calling this.
			const FString ContainerType = CrowdyContainerMarker::ResolveContainerTypeName(Blueprint);
			if (!ContainerType.IsEmpty())
			{
				NewClass->SetMetaData(CrowdyGameModelMetaKeys::Container, *ContainerType);

				// A container actor needs a Crowdy entity component to get a NetID and register, or auto-bind
				// never fires. Marking the class normally adds one; warn if it is still missing (a class
				// marked before that behavior existed, or one whose component was removed) so the gap is not
				// silent. The editor cannot add a component to a C++ base, so a C++ container author must add
				// it in code. The entity marker was just stamped, so the class itself carries the answer.
				if (NewClass->IsChildOf<AActor>() && !NewClass->HasMetaData(CrowdyMetaKeys::CrowdyEntity))
				{
					UE_LOG(LogCrowdyEditor, Warning,
						TEXT("[CrowdySDK] '%s' is a Game Model container actor but has no Crowdy Entity component; it will not register or auto-bind. Add a Crowdy Entity component to the class."),
						*NewClass->GetName());
				}
			}
			else
			{
				NewClass->RemoveMetaData(CrowdyGameModelMetaKeys::Container);
			}

			// The container type's pull-on-start setting, from the same marker. A marked container writes its
			// answer either way, because the lookup takes the nearest answer up the class chain: a container
			// derived from one that turned the pull off would otherwise inherit that while its own setting
			// showed the pull enabled. Only a class that is not a marked container leaves the tag off, where
			// absence already means it pulls.
			switch (CrowdyContainerMarker::ResolvePullOnStartStamp(Blueprint))
			{
			case ECrowdyPullOnStartStamp::On:
				NewClass->SetMetaData(CrowdyGameModelMetaKeys::PullOnStart, TEXT("True"));
				break;
			case ECrowdyPullOnStartStamp::Off:
				NewClass->SetMetaData(CrowdyGameModelMetaKeys::PullOnStart, TEXT("False"));
				break;
			case ECrowdyPullOnStartStamp::None:
				NewClass->RemoveMetaData(CrowdyGameModelMetaKeys::PullOnStart);
				break;
			}
		});

	CrowdyBlueprintCompileHooks::SetClassCompiledHook(
		[](UClass* NewClass)
		{
			// Keep the cooked registry in step with the just-stamped metadata so the packaged build discovers
			// this class. Marks the asset dirty; the next save (or a full 'Rebuild Crowdy Registry') persists it.
			UCrowdyRegistryBaker::UpdateForClass(NewClass);

			// And keep any live (PIE) runtime registry in step, incrementally. Recording the class here lets
			// OnBlueprintCompiled refresh just this class at batch end instead of resweeping every loaded
			// class, the fix for the per-compile editor hitch.
			FCrowdySDKEditorModule::NotePendingRpcRescan(NewClass);
		});
}

void FCrowdySDKEditorModule::RegisterGraphNodeFactory()
{
	GraphNodeFactory = MakeShared<FCrowdyGraphPanelNodeFactory>();
	FEdGraphUtilities::RegisterVisualNodeFactory(GraphNodeFactory);
}

void FCrowdySDKEditorModule::RegisterGraphPinFactory()
{
	GraphPinFactory = MakeShared<FCrowdyStatePropertyPinFactory>();
	FEdGraphUtilities::RegisterVisualPinFactory(GraphPinFactory);
}


// Post-compile hook

// A compile resistances the Blueprint class, replacing its UFunctions and recomputing
// signature hashes. A registry built before the compile (e.g. a running PIE session)
// now holds stale entries for that class, so refresh it. The cooked-build bake is handled
// separately by the compiler extension via UCrowdyRegistryBaker::UpdateForClass.

// We refresh ONLY the classes that recompiled, not every loaded class. The previous full
// RescanRpcFunctions() per registry walked every UClass/UFunction in the editor on each
// compile a multi-tens-of-ms hitch, multiplied by client count under multi-client PIE.
// The compiler extension reports each recompiled class via NotePendingRpcRescan during the
// batch; this drains that set once the batch (and its reinstancing) has settled.

namespace
{
	// Classes recompiled in the current Blueprint compile batch, awaiting an incremental rescan.
	// Weak so a class torn down before the drain is simply skipped.
	TSet<TWeakObjectPtr<UClass>> GPendingRpcRescanClasses;
}

void FCrowdySDKEditorModule::NotePendingRpcRescan(UClass* Class)
{
	if (Class)
	{
		GPendingRpcRescanClasses.Add(Class);
	}
}

void FCrowdySDKEditorModule::OnBlueprintCompiled()
{
	if (GPendingRpcRescanClasses.IsEmpty()) return;

	for (TObjectIterator<UCrowdyAutoRegistry> It; It; ++It)
	{
		UCrowdyAutoRegistry* Registry = *It;
		if (!IsValid(Registry) || Registry->HasAnyFlags(RF_ClassDefaultObject)) continue;

		for (const TWeakObjectPtr<UClass>& WeakClass : GPendingRpcRescanClasses)
		{
			if (UClass* Class = WeakClass.Get())
			{
				Registry->UpdateClassRpcFunctions(Class);
				Registry->UpdateClassRepLayout(Class);
			}
		}
	}

	// A recompiled class's functions are replaced, and the compile has just stamped this batch's routing
	// metadata, so any routing info cached before now describes functions that no longer exist.
	FCrowdyRPC::InvalidateFnInfoCache();

	GPendingRpcRescanClasses.Reset();
}
