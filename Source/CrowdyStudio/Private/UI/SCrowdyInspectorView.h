// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class UGameInstance;
class FActiveTimerHandle;

// Read-only inspector of a running Play-In-Editor (or standalone Game) session's Crowdy runtime
// state: session identity, the player's cached teams / channels / avatars, reliable-RPC channel
// readiness, and the entities this client owns. It is purely a debugging aid and never mutates the
// game. With nothing playing it shows an idle message; press Refresh to re-read after the session
// has connected or fetched data.
class SCrowdyInspectorView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyInspectorView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnRefreshClicked();

	// Re-reads the running session's subsystems and repopulates the body. Safe to call when nothing
	// is playing; it just shows the idle message.
	void Rebuild();

	// The GameInstance of the first running PIE (or standalone Game) world, or null when nothing is
	// playing. The console is editor-only, so it has to reach into the play world to read live state.
	static UGameInstance* FindRunningGameInstance();

	// Auto-refresh: re-run Rebuild on an interval while live state is changing, so the inspector
	// tracks a running session without manual Refresh clicks.
	void SetAutoRefresh(bool bEnable);
	void SetRefreshInterval(float Seconds);
	void RestartAutoRefreshTimer();
	EActiveTimerReturnType OnAutoRefreshTick(double InCurrentTime, float InDeltaTime);

	TSharedPtr<SVerticalBox> BodyBox;

	bool bAutoRefresh = false;
	float RefreshInterval = 2.0f;
	FString IntervalChoice = TEXT("2");
	TWeakPtr<FActiveTimerHandle> AutoRefreshTimer;
};
