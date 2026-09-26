// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

/**
 * Opens an asset in its real editor, the way a double-click in the Content Browser does, and closes it on the way out.
 *
 * A headless test run still has Slate and the editor, just no window on screen, so the toolkit, its tabs and its
 * widgets are all built for real and a case can drive the editor's own handlers rather than a copy of them.
 */
class FCrowdyAssetEditorTestRig
{
public:
	explicit FCrowdyAssetEditorTestRig(UObject* InAsset)
		: Asset(InAsset)
	{
		UAssetEditorSubsystem* Editors = GetEditors();
		if (!Asset || !Editors || !FSlateApplication::IsInitialized())
		{
			return;
		}

		Editors->OpenEditorForAsset(Asset);
		Instance = Editors->FindEditorForAsset(Asset, /*bFocusIfOpen*/ false);
	}

	~FCrowdyAssetEditorTestRig()
	{
		if (UAssetEditorSubsystem* Editors = Asset ? GetEditors() : nullptr)
		{
			Editors->CloseAllEditorsForAsset(Asset);
		}
	}

	FCrowdyAssetEditorTestRig(const FCrowdyAssetEditorTestRig&) = delete;
	FCrowdyAssetEditorTestRig& operator=(const FCrowdyAssetEditorTestRig&) = delete;

	// Why a case cannot run here, or empty when the editor opened.
	FString WhyUnavailable() const
	{
		if (!FSlateApplication::IsInitialized())
		{
			return TEXT("Slate is not running, so no asset editor can be built.");
		}
		return Instance ? FString() : TEXT("The asset editor did not open.");
	}

	// The open toolkit as TToolkit, or null when a different editor opened for the asset.
	template <typename TToolkit>
	TToolkit* GetToolkit(const FName ToolkitName) const
	{
		return Instance && Instance->GetEditorName() == ToolkitName ? static_cast<TToolkit*>(Instance) : nullptr;
	}

private:
	static UAssetEditorSubsystem* GetEditors()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	}

	UObject* Asset = nullptr;
	IAssetEditorInstance* Instance = nullptr;
};

#endif
