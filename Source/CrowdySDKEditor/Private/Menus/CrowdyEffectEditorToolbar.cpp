#include "Menus/CrowdyEffectEditorToolbar.h"

#include "CrowdyStudioSyncService.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Menus/CrowdyToolbarStyle.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Styling/SlateColor.h"
#include "Textures/SlateIcon.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectEditorToolbar"

FDelegateHandle FCrowdyEffectEditorToolbar::ToolbarExtenderHandle;

namespace
{
	FText StatusLabel(ECrowdyEffectSyncStatus Status)
	{
		switch (Status)
		{
		case ECrowdyEffectSyncStatus::Synced:      return LOCTEXT("StatusSynced", "Synced");
		case ECrowdyEffectSyncStatus::Drifted:     return LOCTEXT("StatusDrifted", "Unsynced");
		case ECrowdyEffectSyncStatus::NotOnServer: return LOCTEXT("StatusNotOnServer", "Not on server");
		case ECrowdyEffectSyncStatus::Unknown:
		default:                                   return LOCTEXT("StatusUnknown", "Unknown");
		}
	}

	FSlateColor StatusColor(ECrowdyEffectSyncStatus Status)
	{
		switch (Status)
		{
		case ECrowdyEffectSyncStatus::Synced:      return FSlateColor(FLinearColor(0.35f, 0.78f, 0.42f));  // green
		case ECrowdyEffectSyncStatus::Drifted:     return FSlateColor(FLinearColor(0.95f, 0.71f, 0.24f));  // amber
		case ECrowdyEffectSyncStatus::NotOnServer: return FSlateColor(FLinearColor(0.45f, 0.62f, 0.95f));  // blue
		case ECrowdyEffectSyncStatus::Unknown:
		default:                                   return FSlateColor::UseSubduedForeground();
		}
	}
}

void FCrowdyEffectEditorToolbar::Register()
{
	TArray<FAssetEditorExtender>& ToolbarExtenders =
		FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();

	const int32 NewIndex = ToolbarExtenders.Add(
		FAssetEditorExtender::CreateStatic(&FCrowdyEffectEditorToolbar::CreateToolbarExtender));

	ToolbarExtenderHandle = ToolbarExtenders[NewIndex].GetHandle();
}

void FCrowdyEffectEditorToolbar::Unregister()
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

TSharedRef<FExtender> FCrowdyEffectEditorToolbar::CreateToolbarExtender(
	const TSharedRef<FUICommandList> CommandList, const TArray<UObject*> EditingObjects)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	for (UObject* EditingObject : EditingObjects)
	{
		UCrowdyEffect* Effect = Cast<UCrowdyEffect>(EditingObject);
		if (!Effect)
		{
			continue;
		}

		// "Asset" is the default section every asset-editor toolbar registers, so it is a stable anchor.
		Extender->AddToolBarExtension(
			FName(TEXT("Asset")),
			EExtensionHook::After,
			CommandList,
			FToolBarExtensionDelegate::CreateStatic(
				&FCrowdyEffectEditorToolbar::FillToolbar,
				TWeakObjectPtr<UCrowdyEffect>(Effect)));
		break;
	}

	return Extender;
}

void FCrowdyEffectEditorToolbar::FillToolbar(
	FToolBarBuilder& ToolbarBuilder, TWeakObjectPtr<UCrowdyEffect> WeakEffect)
{
	UCrowdyEffect* Effect = WeakEffect.Get();
	if (!Effect)
	{
		return;
	}

	// Refresh the status when the toolbar is built, but only when it is not already known, so opening the asset (or
	// reopening after an edit reset it to Unknown) computes a fresh status. The service coalesces a duplicate request,
	// so a rapid rebuild does not fan out repeated server reads; the status indicator below re-reads the cache live.
	if (CrowdyStudioSyncService::GetCachedStatus(Effect) == ECrowdyEffectSyncStatus::Unknown)
	{
		CrowdyStudioSyncService::RequestEffectSyncStatus(Effect, nullptr);
	}

	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateStatic(&FCrowdyEffectEditorToolbar::SyncClicked, WeakEffect)),
		NAME_None,
		LOCTEXT("SyncToServerLabel", "Sync to Server"),
		LOCTEXT("SyncToServerTip",
			"Sync this effect's server function and its container type to the Game Model schema on the server. "
			"Only this effect is affected; other server schema is never deleted."),
		FSlateIcon(CrowdyToolbarStyle::StyleSetName, CrowdyToolbarStyle::GameModelIconBrush));

	ToolbarBuilder.AddWidget(
		SNew(SBox)
		.VAlign(VAlign_Center)
		.Padding(FMargin(8.f, 0.f, 4.f, 0.f))
		[
			SNew(STextBlock)
			.Text_Lambda([WeakEffect]()
			{
				return StatusLabel(CrowdyStudioSyncService::GetCachedStatus(WeakEffect.Get()));
			})
			.ColorAndOpacity_Lambda([WeakEffect]()
			{
				return StatusColor(CrowdyStudioSyncService::GetCachedStatus(WeakEffect.Get()));
			})
			.ToolTipText(LOCTEXT("StatusIndicatorTip",
				"Whether this effect matches the Game Model schema on the server. Re-read whenever the status is not "
				"already known: when the asset opens, after an edit, after a sync, and after anything else writes the "
				"server schema."))
		]);
}

void FCrowdyEffectEditorToolbar::SyncClicked(TWeakObjectPtr<UCrowdyEffect> WeakEffect)
{
	UCrowdyEffect* Effect = WeakEffect.Get();
	if (!Effect)
	{
		return;
	}

	CrowdyStudioSyncService::SyncEffect(Effect, [](bool bOk, const FString& Message)
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = bOk ? 4.0f : 8.0f;
		Info.bFireAndForget = true;
		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	});
}

#undef LOCTEXT_NAMESPACE
