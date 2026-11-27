#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class SWidget;

/** The Pick menu that lists the signed-in app's channels from Crowdy Studio, shared by every channel field in the editor. */
namespace CrowdyChannelPicker
{
	/** Starts loading the app's channel names when nothing is loading; does nothing without an app id or a Crowdy Studio sign-in. */
	void FetchChannelNamesIfStale();

	/** The menu: an entry that picks an empty name, labelled EmptyLabel, then the app's channels, then Refresh. It fills in by itself when the list arrives while open. */
	TSharedRef<SWidget> MakeMenu(TFunction<void(const FString&)> OnPick, const FText& EmptyLabel, const FText& EmptyToolTip);
}
