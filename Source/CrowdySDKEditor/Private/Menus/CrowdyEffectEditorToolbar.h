// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FExtender;
class FToolBarBuilder;
class FUICommandList;
class UCrowdyEffect;

/**
 * Adds a "Sync to Server" button and a live Synced / Unsynced / Not-on-server status indicator to the effect asset
 * editor toolbar, so a designer can push one effect's compiled server function and its container type to the Game
 * Model schema without opening the console. The per-asset sync never prunes; it reuses CrowdyStudioSyncService.
 */
class FCrowdyEffectEditorToolbar
{
public:
	static void Register();
	static void Unregister();

private:
	// Offer the extender only when one of the editing objects is a UCrowdyEffect.
	static TSharedRef<FExtender> CreateToolbarExtender(
		const TSharedRef<FUICommandList> CommandList, const TArray<UObject*> EditingObjects);

	static void FillToolbar(FToolBarBuilder& ToolbarBuilder, TWeakObjectPtr<UCrowdyEffect> WeakEffect);

	// Invoked by the toolbar button: syncs the one effect and shows a completion notification.
	static void SyncClicked(TWeakObjectPtr<UCrowdyEffect> WeakEffect);

	static FDelegateHandle ToolbarExtenderHandle;
};
