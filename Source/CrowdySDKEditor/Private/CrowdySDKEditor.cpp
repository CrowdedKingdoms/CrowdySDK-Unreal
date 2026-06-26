#include "CrowdySDKEditor.h"

#include "BlueprintCompilationManager.h"
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
#include "KismetNodes/SGraphNodeK2Event.h"
#include "CrowdyBlueprintCompilerExtension.h"
#include "CrowdyEditorEventMeta.h"
#include "Baking/CrowdyRegistryBaker.h"
#include "CrowdyStudioModule.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Customizations/CrowdyCustomEventCustomization.h"
#include "Menus/CrowdyStructEditorToolbar.h"
#include "Menus/CrowdyStructContextMenu.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SBoxPanel.h"

IMPLEMENT_MODULE(FCrowdySDKEditorModule, CrowdySDKEditor)

DEFINE_LOG_CATEGORY(LogCrowdyEditor)

namespace CrowdyMetaKeys
{
	const FName CrowdyEvent    (TEXT("CrowdyEvent"));
	const FName CrowdyPersistent   (TEXT("CrowdyPersistent"));
	const FName CrowdySingleton    (TEXT("CrowdySingleton"));
	const FName CrowdyEntity       (TEXT("CrowdyEntity"));
}

namespace
{
	static ECrowdyEventRecipient ResolveCrowdyRecipient(const FString& RecipientMetaValue)
	{
		if (RecipientMetaValue.Equals(TEXT("OwningPlayer"), ESearchCase::IgnoreCase)
			|| RecipientMetaValue.Equals(TEXT("Owning Player"), ESearchCase::IgnoreCase))
		{
			return ECrowdyEventRecipient::OwningClient;
		}

		const UEnum* EnumType = StaticEnum<ECrowdyEventRecipient>();
		const int64 Value = EnumType ? EnumType->GetValueByNameString(RecipientMetaValue) : INDEX_NONE;
		return Value == INDEX_NONE
			? ECrowdyEventRecipient::SpatialMulticast
			: static_cast<ECrowdyEventRecipient>(Value);
	}

	static FText GetCrowdyRecipientSubtitle(ECrowdyEventRecipient Recipient)
	{
		switch (Recipient)
		{
		case ECrowdyEventRecipient::OwningClient:
			return FText::FromString(TEXT("\nCrowdy Owning Client\nExecutes Locally Only"));
		case ECrowdyEventRecipient::Host:
			return FText::FromString(TEXT("\nCrowdy Host\nExecutes on Host"));
		case ECrowdyEventRecipient::Multicast:
			// Channel transport — every session-channel member, any distance, no decay.
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

		// Rebuild the node when its Crowdy subtitle changes — e.g. the recipient dropdown in the
		// details panel — so the Crowdy execution label updates live, without waiting for a recompile.
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

			return nullptr;
		}
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// Module lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdySDKEditorModule::StartupModule()
{
	RegisterStructContextMenu();
	RegisterFunctionEntryCustomization();
	RegisterGraphNodeFactory();
	RegisterCompilerExtension();
	RegisterBlueprintCompilerExtension();
	UCrowdyRegistryBaker::Register();

	// The Registry Inspector now lives in the CrowdyStudio console (Registry page). The deep rebuild
	// is editor-only, so hand the console a hook into the baker rather than CrowdyStudio depending on
	// this module. The captureless lambda is cleared in ShutdownModule so it never dangles.
	CrowdyStudioRegistry::SetRebuildHook(
		[](TFunction<void()> OnComplete) { UCrowdyRegistryBaker::RebuildAsync(MoveTemp(OnComplete)); });
}

void FCrowdySDKEditorModule::ShutdownModule()
{
	CrowdyStudioRegistry::SetRebuildHook(nullptr);

	if (FPropertyEditorModule* PM =
		FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PM->UnregisterCustomClassLayout("K2Node_CustomEvent");
	}

	FCrowdyStructEditorToolbar::Unregister();
	FCrowdyStructContextMenu::Unregister();

	if (GraphNodeFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);
		GraphNodeFactory.Reset();
	}

	if (GEditor && CompiledHandle.IsValid())
	{
		GEditor->OnBlueprintCompiled().Remove(CompiledHandle);
		CompiledHandle.Reset();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration helpers
// ─────────────────────────────────────────────────────────────────────────────
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

void FCrowdySDKEditorModule::RegisterCompilerExtension()
{
	if (!GEditor) return;

	CompiledHandle = GEditor->OnBlueprintCompiled().AddStatic(
		&FCrowdySDKEditorModule::OnBlueprintCompiled);
}

void FCrowdySDKEditorModule::RegisterBlueprintCompilerExtension()
{
	UCrowdyBlueprintCompilerExtension* Extension =
		NewObject<UCrowdyBlueprintCompilerExtension>(GetTransientPackage());

	FBlueprintCompilationManager::RegisterCompilerExtension(
		UBlueprint::StaticClass(),
		Extension);
}

void FCrowdySDKEditorModule::RegisterGraphNodeFactory()
{
	GraphNodeFactory = MakeShared<FCrowdyGraphPanelNodeFactory>();
	FEdGraphUtilities::RegisterVisualNodeFactory(GraphNodeFactory);
}

// ─────────────────────────────────────────────────────────────────────────────
// Post-compile hook
//
// A compile reinstances the Blueprint class, replacing its UFunctions and recomputing
// signature hashes. A registry built before the compile (e.g. a running PIE session's)
// now holds stale entries for that class, so refresh it. The cooked-build bake is handled
// separately by the compiler extension via UCrowdyRegistryBaker::UpdateForClass.
//
// We refresh ONLY the classes that recompiled, not every loaded class. The previous full
// RescanRpcFunctions() per registry walked every UClass/UFunction in the editor on each
// compile — a multi-tens-of-ms hitch, multiplied by client count under multi-client PIE.
// The compiler extension reports each recompiled class via NotePendingRpcRescan during the
// batch; this drains that set once the batch (and its reinstancing) has settled.
// ─────────────────────────────────────────────────────────────────────────────
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
			}
		}
	}

	GPendingRpcRescanClasses.Reset();
}
