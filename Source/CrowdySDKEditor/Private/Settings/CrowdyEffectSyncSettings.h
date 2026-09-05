// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CrowdyEffectSyncSettings.generated.h"

/**
 * Editor-only preferences for the per-effect Game Model sync. Project-scoped (DefaultEditor.ini), so the team shares
 * the setting.
 */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Crowdy SDK Editor"))
class UCrowdyEffectSyncSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Crowdy SDK Editor"); }

	/** When on, pressing Play consults the already-known sync status of your effect assets and, if any are out of
	 *  sync with the server, offers a one-click "Sync now". No server request is made on Play; only cached statuses
	 *  are read. Turn this off to skip the check entirely. */
	UPROPERTY(Config, EditAnywhere, Category = "Game Model",
		meta = (DisplayName = "Check effect drift before Play"))
	bool bCheckEffectDriftBeforePlay = true;
};
