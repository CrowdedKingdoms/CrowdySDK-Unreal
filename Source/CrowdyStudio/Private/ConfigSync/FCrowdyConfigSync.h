// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyUDPProtocol.h"
#include "Model/CrowdyStudioTypes.h"

/**
 * Writes the chosen app's identifiers and endpoints into UCrowdySDKDeveloperSettings, the object
 * the runtime already reads at startup, so connecting a game to a Crowdy app is one button instead
 * of hand-copying ids and URLs into DefaultGame.ini. It also owns the small bit of backend
 * selection (Dev/Prod/Custom) that the Project page exposes.
 */
class FCrowdyConfigSync
{
public:

	// Writes the chosen app's id, its org, and the game endpoints. The game endpoints come from the
	// app's own routing (gameApiUrl), which is where clients actually connect. The management URL is
	// not written here; it is owned by the backend selector (GetBackendMode / SetBackendMode).
	static void ApplyAppToSettings(const FStudioApp& App);

	static FStudioSettingsSnapshot ReadCurrentSettings();

	// What ApplyAppToSettings would leave the settings as, without writing anything. The Project
	// page shows this against ReadCurrentSettings as a before/after diff, so the displayed diff and
	// what a sync actually writes always agree.
	static FStudioSettingsSnapshot BuildProposedSettings(const FStudioApp& App);

	// Pushes the freshly written settings into any running PIE/standalone session so a sync takes
	// effect without relaunching Play. Returns how many live sessions picked it up (0 = none
	// running, so the change applies on the next Play). A fresh Play always reads the new values.
	// TODO: TESTING
	static int32 ApplyToRunningSessions();

	// Backend selector, stored as UCrowdySDKDeveloperSettings::Environment. Mode is one of "Dev",
	// "Prod", or "Custom". Custom uses the hand-set management URL; Dev/Prod use built-in hosts.
	// GetEffectiveManagementUrl is the URL that actually results from the current mode.
	static FString GetBackendMode();
	static void SetBackendMode(const FString& Mode);
	static FString GetCustomManagementUrl();
	static void SetCustomManagementUrl(const FString& Url);
	static FString GetEffectiveManagementUrl();

	// The realtime UDP connection knobs, stored on UCrowdySDKDeveloperSettings and consumed by the
	// runtime at connect time. Surfaced on the Project page so they are edited from the console like
	// every other network setting. Setters persist to DefaultGame.ini and clamp to the same ranges
	// the settings used to enforce (timeout 6-120 s, host poll 1-60 s).
	static ECrowdyUDPProtocol GetUdpProtocol();
	static void SetUdpProtocol(ECrowdyUDPProtocol Protocol);
	static float GetUdpTimeoutSeconds();
	static void SetUdpTimeoutSeconds(float Seconds);
	static float GetHostPollIntervalSeconds();
	static void SetHostPollIntervalSeconds(float Seconds);
};
