// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyStudioUserSettings.generated.h"

/**
 * Per-user, per-project editor preferences for the CrowdyStudio console. Non-secret values
 * only the admin token live in the DPAPI vault (FCrowdyTokenVault), never here. Persisted
 * to Saved/Config/.../EditorPerProjectUserSettings.ini.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class UCrowdyStudioUserSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Config)
	int64 LastOrgId = 0;

	UPROPERTY(Config)
	int64 LastAppId = 0;

	// Optional override for the web console base URL. Empty derives it from the management API
	// URL (api.* to app.*), which is the right host for both the shared tier and dedicated boxes.
	UPROPERTY(Config)
	FString WebConsoleUrlOverride;

	UPROPERTY(Config)
	bool bRememberToken = false;
};
