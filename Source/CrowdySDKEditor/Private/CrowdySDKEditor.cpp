#include "CrowdySDKEditor.h"

#include "BlueprintCompilationManager.h"
#include "PropertyEditorModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "KismetNodes/SGraphNodeK2Event.h"
#include "CrowdyBlueprintCompilerExtension.h"
#include "Customizations/CrowdyCustomEventCustomization.h"
#include "Customizations/CrowdyFunctionEntryCustomization.h"
#include "Menus/CrowdyStructEditorToolbar.h"
#include "Menus/CrowdyStructContextMenu.h"
#include "Widgets/SBoxPanel.h"

IMPLEMENT_MODULE(FCrowdySDKEditorModule, CrowdySDKEditor)

DEFINE_LOG_CATEGORY(LogCrowdyEditor)

namespace CrowdyMetaKeys
{
	const FName CrowdyEvent    (TEXT("CrowdyEvent"));
	const FName CrowdyRep      (TEXT("CrowdyRep"));
	const FName LegacyCrowdyCategory(TEXT("CrowdyCategory"));
	const FName CrowdyPersistent(TEXT("CrowdyPersistent"));
	const FName CrowdyInstanced (TEXT("CrowdyInstanced"));
}

namespace
{
	struct FCrowdyFunctionValidationResult
	{
		bool bIsValid = false;
		FString ErrorMessage;
	};

	static bool IsCrowdyPayloadPin(const UEdGraphPin* Pin)
	{
		if (!Pin) return false;
		if (Pin->ParentPin) return false;
		if (Pin->Direction != EGPD_Output) return false;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Schema && Schema->IsMetaPin(*Pin)) return false;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) return false;

		return true;
	}

	static bool IsCrowdyCustomEvent(UK2Node_CustomEvent* CustomEvent)
	{
		return CustomEvent
			&& CustomEvent->GetUserDefinedMetaData().HasMetaData(
				CrowdyMetaKeys::CrowdyEvent);
	}

	static bool IsCrowdyFunctionEntry(const UK2Node_FunctionEntry* FunctionEntry)
	{
		return FunctionEntry
			&& FunctionEntry->MetaData.HasMetaData(CrowdyMetaKeys::CrowdyEvent);
	}

	static bool IsValidCrowdyRepValue(const FString& CrowdyRepValue)
	{
		return CrowdyRepValue == TEXT("Event")
			|| CrowdyRepValue == TEXT("ActorUpdate");
	}

	static FCrowdyFunctionValidationResult ValidateCrowdyEventHandler(
		const UEdGraphNode* Node,
		int32 FunctionFlags)
	{
		FCrowdyFunctionValidationResult Result;

		if (!Node)
		{
			Result.ErrorMessage = TEXT("CrowdyEvent handler is invalid.");
			return Result;
		}

		if ((FunctionFlags & FUNC_Private) != 0)
		{
			Result.ErrorMessage =
				TEXT("CrowdyEvent handler @@ must be public; private Blueprint handlers cannot be called externally for replication.");
			return Result;
		}

		if ((FunctionFlags & FUNC_Protected) != 0)
		{
			Result.ErrorMessage =
				TEXT("CrowdyEvent handler @@ must be public; protected Blueprint handlers cannot be called externally for replication.");
			return Result;
		}

		if ((FunctionFlags & FUNC_NetFuncFlags) != 0)
		{
			Result.ErrorMessage =
				TEXT("CrowdyEvent handler @@ cannot also use Unreal replication. Disable Run On Server/Client/Multicast/Reliable before enabling Crowdy replication.");
			return Result;
		}

		int32 InputPinCount = 0;
		const UScriptStruct* PayloadStruct = nullptr;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsCrowdyPayloadPin(Pin)) continue;

			++InputPinCount;

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				PayloadStruct = Cast<UScriptStruct>(
					Pin->PinType.PinSubCategoryObject.Get());
			}
		}

		if (InputPinCount == 0)
		{
			Result.ErrorMessage =
				TEXT("CrowdyEvent handler @@ must have exactly one struct parameter tagged with CrowdyRep=\"Event\" or CrowdyRep=\"ActorUpdate\".");
			return Result;
		}

		if (InputPinCount > 1)
		{
			Result.ErrorMessage = FString::Printf(
				TEXT("CrowdyEvent handler @@ has %d parameters; exactly one struct parameter tagged with CrowdyRep=\"Event\" or CrowdyRep=\"ActorUpdate\" is required."),
				InputPinCount);
			return Result;
		}

		if (!PayloadStruct)
		{
			Result.ErrorMessage =
				TEXT("CrowdyEvent handler @@ parameter must be a struct tagged with CrowdyRep=\"Event\" or CrowdyRep=\"ActorUpdate\".");
			return Result;
		}

		const FString CrowdyRepValue =
			PayloadStruct->GetMetaData(CrowdyMetaKeys::CrowdyRep);

		if (!IsValidCrowdyRepValue(CrowdyRepValue))
		{
			Result.ErrorMessage = FString::Printf(
				TEXT("CrowdyEvent handler @@ parameter struct '%s' must be tagged with CrowdyRep=\"Event\" or CrowdyRep=\"ActorUpdate\"."),
				*PayloadStruct->GetName());
			return Result;
		}

		Result.bIsValid = true;
		return Result;
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
						&SGraphNodeCrowdyCustomEvent::GetCrowdyReceiverTitle)
				];
		}

	private:
		FText GetCrowdyReceiverTitle() const
		{
			return FText::FromString(TEXT("\nCrowdy Replicated Receiver"));
		}

		bool IsCrowdyReplicated() const
		{
			return IsCrowdyCustomEvent(Cast<UK2Node_CustomEvent>(GraphNode));
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

			return IsCrowdyReplicated()
				? EVisibility::Collapsed
				: EVisibility::Visible;
		}

		EVisibility GetCrowdySubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Collapsed;
			}

			return IsCrowdyReplicated()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		}
	};

	class SGraphNodeCrowdyFunctionEntry : public SGraphNodeK2Default
	{
	public:
		SLATE_BEGIN_ARGS(SGraphNodeCrowdyFunctionEntry) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, UK2Node_FunctionEntry* InNode)
		{
			GraphNode = InNode;
			SetCursor(EMouseCursor::CardinalCross);
			UpdateGraphNode();
		}

	protected:
		virtual TSharedRef<SWidget> CreateTitleWidget(
			TSharedPtr<SNodeTitle> NodeTitle) override
		{
			TSharedRef<SWidget> TitleWidget =
				SGraphNodeK2Default::CreateTitleWidget(NodeTitle);

			TitleWidget->SetVisibility(MakeAttributeSP(
				this, &SGraphNodeCrowdyFunctionEntry::GetTitleVisibility));

			if (NodeTitle.IsValid())
			{
				NodeTitle->SetVisibility(MakeAttributeSP(
					this,
					&SGraphNodeCrowdyFunctionEntry::GetDefaultSubtitleVisibility));
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
						&SGraphNodeCrowdyFunctionEntry::GetCrowdySubtitleVisibility)
					.Text(this,
						&SGraphNodeCrowdyFunctionEntry::GetCrowdyReceiverTitle)
				];
		}

	private:
		FText GetCrowdyReceiverTitle() const
		{
			return FText::FromString(TEXT("\nCrowdy Replicated Receiver"));
		}

		bool IsCrowdyReplicated() const
		{
			return IsCrowdyFunctionEntry(Cast<UK2Node_FunctionEntry>(GraphNode));
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

			return IsCrowdyReplicated()
				? EVisibility::Collapsed
				: EVisibility::Visible;
		}

		EVisibility GetCrowdySubtitleVisibility() const
		{
			if (UseLowDetailNodeTitles())
			{
				return EVisibility::Collapsed;
			}

			return IsCrowdyReplicated()
				? EVisibility::Visible
				: EVisibility::Collapsed;
		}
	};

	class FCrowdyGraphPanelNodeFactory : public FGraphPanelNodeFactory
	{
	public:
		virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override
		{
			if (UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return SNew(SGraphNodeCrowdyFunctionEntry, FunctionEntry);
			}

			if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				return SNew(SGraphNodeCrowdyCustomEvent, CustomEvent);
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
}

void FCrowdySDKEditorModule::ShutdownModule()
{
	if (FPropertyEditorModule* PM =
		FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PM->UnregisterCustomClassLayout("K2Node_FunctionEntry");
		PM->UnregisterCustomClassLayout("K2Node_CustomEvent");
	}

	FCrowdyStructEditorToolbar::Unregister();
	FCrowdyStructContextMenu::Unregister();

	if (GraphNodeFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);
		GraphNodeFactory.Reset();
	}

	if (GEditor && PreCompileHandle.IsValid())
	{
		GEditor->OnBlueprintPreCompile().Remove(PreCompileHandle);
		PreCompileHandle.Reset();
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
		"K2Node_FunctionEntry",
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FCrowdyFunctionEntryCustomization::MakeInstance));

	PM.RegisterCustomClassLayout(
		"K2Node_CustomEvent",
		FOnGetDetailCustomizationInstance::CreateStatic(
			&FCrowdyCustomEventCustomization::MakeInstance));
}

void FCrowdySDKEditorModule::RegisterCompilerExtension()
{
	if (!GEditor) return;

	PreCompileHandle = GEditor->OnBlueprintPreCompile().AddStatic(
		&FCrowdySDKEditorModule::OnBlueprintPreCompile);
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
// Pre-compile hook
//
// Transfers CrowdyEvent metadata from Blueprint function/custom event nodes
// onto the generated UFunctions (which are rebuilt on every compile).
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdySDKEditorModule::OnBlueprintPreCompile(UBlueprint* Blueprint)
{
	if (!Blueprint) return;

	UClass* ClassesToStamp[] = {
		Blueprint->SkeletonGeneratedClass,
		Blueprint->GeneratedClass
	};

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			FName FunctionName = NAME_None;
			int32 FunctionFlags = 0;

			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				if (!Entry->MetaData.HasMetaData(CrowdyMetaKeys::CrowdyEvent)) continue;

				FunctionName = Graph->GetFName();
				FunctionFlags = Entry->GetFunctionFlags();
			}
			else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				if (!CustomEvent->GetUserDefinedMetaData().HasMetaData(
					CrowdyMetaKeys::CrowdyEvent)) continue;

				FunctionName = CustomEvent->CustomFunctionName;
				FunctionFlags = CustomEvent->FunctionFlags;
			}
			else
			{
				continue;
			}

			const FCrowdyFunctionValidationResult ValidationResult =
				ValidateCrowdyEventHandler(Node, FunctionFlags);
			if (!ValidationResult.bIsValid)
			{
				Blueprint->Message_Error(ValidationResult.ErrorMessage, Node);
				continue;
			}

			if (FunctionName == NAME_None)
			{
				Blueprint->Message_Error(
					TEXT("CrowdyEvent handler @@ does not have a valid generated function name."),
					Node);
				continue;
			}

			for (UClass* Class : ClassesToStamp)
			{
				if (!Class) continue;

				if (UFunction* Func = Class->FindFunctionByName(FunctionName))
				{
					Func->SetMetaData(CrowdyMetaKeys::CrowdyEvent, TEXT(""));

					UE_LOG(LogCrowdyEditor, Verbose,
						TEXT("[CrowdySDK] Stamped UFunction '%s::%s' with CrowdyEvent"),
						*Class->GetName(), *FunctionName.ToString());
				}
			}
		}
	}
}
