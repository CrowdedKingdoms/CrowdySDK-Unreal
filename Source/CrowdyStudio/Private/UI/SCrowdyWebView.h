// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SWebBrowser;

// Embedded web panel (CEF) used to host the web-console pages inside the editor, with a
// thin toolbar (reload + "open in external browser"). One instance is reused and navigated
// via LoadUrl rather than spawning a browser per page. Falls back to an external-browser
// prompt when CEF isn't available.
class SCrowdyWebView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyWebView) {}
		SLATE_ARGUMENT(FString, InitialUrl)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Navigate to Url, single-signing-on with AuthToken (injected as the web app's
	// localStorage auth_token so the console opens already authenticated). Pass an empty
	// token to navigate without injecting.
	void LoadUrl(const FString& Url, const FString& AuthToken);

	// Sign the embedded session out: clear the web app's stored token, drop its cookies, and
	// show a blank page. Called when the editor signs out.
	void ClearSession();

private:
	FReply OnReloadClicked();
	FReply OnOpenExternalClicked();
	void HandleUrlChanged(const FText& Url);
	void HandleLoadCompleted();

	TSharedPtr<SWebBrowser> Browser;
	FString CurrentUrl;

	// SSO state: we load the web origin once, write the token into its localStorage, then
	// navigate to the requested page (same origin =token persists for later tabs).
	FString AuthToken;
	FString PendingUrl;
	bool bInjectingAuth = false;
	bool bAuthInjected = false;
};
