#include "Menus/CrowdyStructEditorToolbar.h"

#include "CrowdySDKEditor.h"
#include "Engine/UserDefinedStruct.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

FDelegateHandle FCrowdyStructEditorToolbar::ToolbarExtenderHandle;

namespace CrowdyStructEditorToolbar
{
	static const FString RepEvent(TEXT("Event"));
	static const FString RepActorUpdate(TEXT("ActorUpdate"));

	static FString GetCurrentRepValue(TWeakObjectPtr<::UUserDefinedStruct> WeakStruct)
	{
		return WeakStruct.IsValid()
			? WeakStruct->GetMetaData(CrowdyMetaKeys::CrowdyRep)
			: FString();
	}

	static FText GetCurrentRepLabel(TWeakObjectPtr<::UUserDefinedStruct> WeakStruct)
	{
		const FString CurrentValue = GetCurrentRepValue(WeakStruct);
		if (CurrentValue == RepEvent)
		{
			return FText::FromString(TEXT("Crowdy: Event"));
		}

		if (CurrentValue == RepActorUpdate)
		{
			return FText::FromString(TEXT("Crowdy: Actor Update"));
		}

		return FText::FromString(TEXT("Crowdy: None"));
	}

	static bool IsRepValueSelected(
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		FString TargetValue)
	{
		return GetCurrentRepValue(WeakStruct) == TargetValue;
	}

	static void SetRepValue(
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		FString TargetValue)
	{
		if (!WeakStruct.IsValid()) return;

		::UUserDefinedStruct* Struct = WeakStruct.Get();
		if (TargetValue.IsEmpty())
		{
			Struct->RemoveMetaData(CrowdyMetaKeys::CrowdyRep);
			Struct->RemoveMetaData(CrowdyMetaKeys::LegacyCrowdyCategory);

			UE_LOG(LogCrowdyEditor, Log,
				TEXT("[CrowdySDK] Removed Crowdy stamp from struct '%s'. Save to persist."),
				*Struct->GetName());
		}
		else
		{
			Struct->SetMetaData(CrowdyMetaKeys::CrowdyRep, *TargetValue);
			Struct->RemoveMetaData(CrowdyMetaKeys::LegacyCrowdyCategory);

			UE_LOG(LogCrowdyEditor, Log,
				TEXT("[CrowdySDK] Stamped struct '%s' CrowdyRep=%s. Save to persist."),
				*Struct->GetName(),
				*TargetValue);
		}

		Struct->MarkPackageDirty();
	}

	static ECheckBoxState GetFlagCheckState(
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		FName MetaKey)
	{
		return WeakStruct.IsValid() && WeakStruct->HasMetaData(MetaKey)
			? ECheckBoxState::Checked
			: ECheckBoxState::Unchecked;
	}

	static void SetFlagValue(
		ECheckBoxState NewState,
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		FName MetaKey)
	{
		if (!WeakStruct.IsValid()) return;

		::UUserDefinedStruct* Struct = WeakStruct.Get();
		const bool bEnabled = NewState == ECheckBoxState::Checked;
		if (bEnabled)
		{
			Struct->SetMetaData(MetaKey, TEXT(""));
		}
		else
		{
			Struct->RemoveMetaData(MetaKey);
		}

		Struct->MarkPackageDirty();

		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdySDK] %s key-only metadata '%s' on struct '%s'. Save to persist."),
			bEnabled ? TEXT("Set") : TEXT("Removed"),
			*MetaKey.ToString(),
			*Struct->GetName());
	}

	static TSharedRef<SWidget> MakeFlagToggle(
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		FName MetaKey,
		const FText& Label,
		const FText& ToolTip)
	{
		return SNew(SCheckBox)
			.ToolTipText(ToolTip)
			.IsChecked_Lambda([WeakStruct, MetaKey]()
			{
				return GetFlagCheckState(WeakStruct, MetaKey);
			})
			.OnCheckStateChanged(FOnCheckStateChanged::CreateStatic(
				&SetFlagValue,
				WeakStruct,
				MetaKey))
			[
				SNew(STextBlock)
				.Text(Label)
			];
	}

	static void AddRepMenuEntry(
		FMenuBuilder& MenuBuilder,
		TWeakObjectPtr<::UUserDefinedStruct> WeakStruct,
		const FName EntryName,
		const FText& Label,
		const FText& ToolTip,
		const FString& TargetValue)
	{
		MenuBuilder.AddMenuEntry(
			Label,
			ToolTip,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateStatic(
					&SetRepValue,
					WeakStruct,
					TargetValue),
				FCanExecuteAction(),
				FIsActionChecked::CreateStatic(
					&IsRepValueSelected,
					WeakStruct,
					TargetValue)),
			EntryName,
			EUserInterfaceActionType::RadioButton);
	}

	static TSharedRef<SWidget> GenerateRepMenu(TWeakObjectPtr<::UUserDefinedStruct> WeakStruct)
	{
		FMenuBuilder MenuBuilder(true, nullptr);

		AddRepMenuEntry(
			MenuBuilder,
			WeakStruct,
			FName(TEXT("CrowdyStructRepNone")),
			FText::FromString(TEXT("None")),
			FText::FromString(TEXT("Remove CrowdyRep metadata from this struct.")),
			FString());

		AddRepMenuEntry(
			MenuBuilder,
			WeakStruct,
			FName(TEXT("CrowdyStructRepEvent")),
			FText::FromString(TEXT("Crowdy Event")),
			FText::FromString(TEXT("Tags this struct with meta=(CrowdyRep=\"Event\").")),
			RepEvent);

		AddRepMenuEntry(
			MenuBuilder,
			WeakStruct,
			FName(TEXT("CrowdyStructRepActorUpdate")),
			FText::FromString(TEXT("Crowdy Actor Update")),
			FText::FromString(TEXT("Tags this struct with meta=(CrowdyRep=\"ActorUpdate\").")),
			RepActorUpdate);

		return MenuBuilder.MakeWidget();
	}
}

void FCrowdyStructEditorToolbar::Register()
{
	TArray<FAssetEditorExtender>& ToolbarExtenders =
		FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();

	const int32 NewIndex = ToolbarExtenders.Add(
		FAssetEditorExtender::CreateStatic(
			&FCrowdyStructEditorToolbar::CreateToolbarExtender));

	ToolbarExtenderHandle = ToolbarExtenders[NewIndex].GetHandle();
}

void FCrowdyStructEditorToolbar::Unregister()
{
	if (!ToolbarExtenderHandle.IsValid())
	{
		return;
	}

	TArray<FAssetEditorExtender>& ToolbarExtenders =
		FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();

	ToolbarExtenders.RemoveAll([](const FAssetEditorExtender& Extender)
	{
		return Extender.GetHandle() == ToolbarExtenderHandle;
	});

	ToolbarExtenderHandle.Reset();
}

TSharedRef<FExtender> FCrowdyStructEditorToolbar::CreateToolbarExtender(
	const TSharedRef<FUICommandList> CommandList,
	const TArray<UObject*> EditingObjects)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	for (UObject* EditingObject : EditingObjects)
	{
		if (::UUserDefinedStruct* Struct = Cast<::UUserDefinedStruct>(EditingObject))
		{
			Extender->AddToolBarExtension(
				FName(TEXT("UserDefinedStructure")),
				EExtensionHook::After,
				CommandList,
				FToolBarExtensionDelegate::CreateStatic(
					&FCrowdyStructEditorToolbar::FillToolbar,
					TWeakObjectPtr<::UUserDefinedStruct>(Struct)));
			break;
		}
	}

	return Extender;
}

void FCrowdyStructEditorToolbar::FillToolbar(
	FToolBarBuilder& ToolbarBuilder,
	TWeakObjectPtr<::UUserDefinedStruct> WeakStruct)
{
	if (!WeakStruct.IsValid()) return;

	ToolbarBuilder.AddWidget(
		SNew(SComboButton)
		.ContentPadding(FMargin(4.0f, 0.0f))
		.ToolTipText(FText::FromString(TEXT("Sets the CrowdyRep metadata for this struct.")))
		.OnGetMenuContent(FOnGetContent::CreateStatic(
			&CrowdyStructEditorToolbar::GenerateRepMenu,
			WeakStruct))
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateStatic(
				&CrowdyStructEditorToolbar::GetCurrentRepLabel,
				WeakStruct)))
		]);

	ToolbarBuilder.AddWidget(CrowdyStructEditorToolbar::MakeFlagToggle(
		WeakStruct,
		CrowdyMetaKeys::CrowdyPersistent,
		FText::FromString(TEXT("Persistent")),
		FText::FromString(TEXT("Toggles meta=(CrowdyPersistent) on this struct."))));

	ToolbarBuilder.AddWidget(CrowdyStructEditorToolbar::MakeFlagToggle(
		WeakStruct,
		CrowdyMetaKeys::CrowdyInstanced,
		FText::FromString(TEXT("Instanced")),
		FText::FromString(TEXT("Toggles meta=(CrowdyInstanced) on this struct."))));
}
