#include "Menus/CrowdyStructEditorToolbar.h"

#include "CrowdySDKEditor.h"
#include "Menus/CrowdyStructMetaUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"

FDelegateHandle FCrowdyStructEditorToolbar::ToolbarExtenderHandle;

namespace CrowdyStructEditorToolbar
{
	static const TCHAR* EnabledFlagValue = TEXT("true");

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
		Struct->Modify();
		if (bEnabled)
		{
			Struct->SetMetaData(MetaKey, EnabledFlagValue);
		}
		else
		{
			Struct->RemoveMetaData(MetaKey);
		}

		NotifyStructMetadataChanged(Struct);

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

	ToolbarBuilder.AddWidget(CrowdyStructEditorToolbar::MakeFlagToggle(
		WeakStruct,
		CrowdyMetaKeys::CrowdyPersistent,
		FText::FromString(TEXT("Persistent")),
		FText::FromString(TEXT("Toggles meta=(CrowdyPersistent) on this struct."))));

	ToolbarBuilder.AddWidget(CrowdyStructEditorToolbar::MakeFlagToggle(
		WeakStruct,
		CrowdyMetaKeys::CrowdySingleton,
		FText::FromString(TEXT("Singleton")),
		FText::FromString(TEXT("Toggles meta=(CrowdySingleton) on this struct. By default all persistent structs are instanced."))));
}
