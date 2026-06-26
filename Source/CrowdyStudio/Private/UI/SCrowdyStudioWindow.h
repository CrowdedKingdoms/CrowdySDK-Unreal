// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SCrowdyLogoIntro;
class SCrowdyWebView;
class SWidgetSwitcher;

// Root of the CrowdyStudio console. A brand header (logo + signed-in identity), a grouped
// icon navigation rail, a page area that cross-fades between views, and a status bar. A
// one-shot animated logo intro plays on top when the tab opens and dissolves to the console.
//
// The rail keeps only what a game uses the SDK for: pointing the project at an app (sign-in,
// the Project page with config sync and a game server link) and authoring the runtime pieces (teams, channels,
// grid, game model). Pure account/billing management opens the embedded web console instead.
class SCrowdyStudioWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyStudioWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildNavRail();
	TSharedRef<SWidget> BuildStatusBar();

	TSharedRef<SWidget> MakeNavButton(const FText& Label, const TCHAR* Icon, int32 PageIndex, bool bRequiresSignIn);
	TSharedRef<SWidget> MakeWebNavButton(const FText& Label, const TCHAR* Icon, TFunction<FString()> UrlGetter);
	TSharedRef<SWidget> MakeHairline();

	// Switches the active page and replays the page transition.
	void ShowPage(int32 PageIndex);

	// On sign-out, clear the embedded web session and return to Sign In.
	void HandleSignInStateChanged();

	// Surfaces an error status as a transient editor toast. Success stays on the status line.
	void HandleStatusMessage(const FString& Message, bool bIsError);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SWidgetSwitcher> PageSwitcher;
	TSharedPtr<SCrowdyWebView> WebView;
	TSharedPtr<SCrowdyLogoIntro> LogoIntro;

	// Resolves the web console URL for the current org; set before the nav rail is built.
	TFunction<FString()> WebOverviewLink;

	int32 ActiveIndex = 0;
	FCurveSequence PageAnim;
	FCurveHandle PageFade;
};
