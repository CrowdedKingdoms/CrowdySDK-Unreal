// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Templates/Function.h"

// Stable page indices for the console's SWidgetSwitcher. The switcher slots are added in this
// order and the nav rail + cross-view navigation reference these names instead of raw integers.
namespace CrowdyStudioPages
{
	enum Page : int32
	{
		SignIn = 0,
		Wizard,
		Home,
		Apps,        // the merged Project page: org/app pick, config sync, and the game server link
		Teams,
		Channels,
		Grid,
		GameModel,
		Inspector,
		Registry,
		WebConsole
	};
}

class SWidget;

// A page another editor module added through CrowdyStudioExtraPages::RegisterPage.
struct FCrowdyStudioExtraPage
{
	FName Id;
	FText Group;
	FText Label;
	FString Icon;
	TFunction<TSharedRef<SWidget>()> MakeWidget;
};

namespace CrowdyStudioExtraPages
{
	const TArray<FCrowdyStudioExtraPage>& GetPages();

	/** The page OpenPage asked for and nothing has shown yet, or None; clears it. */
	FName TakeRequestedPage();

	/** Broadcast when OpenPage asks for a page, after the console's tab is open. */
	FSimpleMulticastDelegate& OnPageRequested();
}

// Lets a view ask the window to switch to another page (used by the dashboard and setup wizard).
DECLARE_DELEGATE_OneParam(FOnStudioNavigate, int32 /*PageIndex*/);
