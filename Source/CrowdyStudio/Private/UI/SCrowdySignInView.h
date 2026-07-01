// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SEditableTextBox;
class SVerticalBox;

// The console's front door, shown only while signed out. Sign-in options are laid out by hierarchy so
// a user can gauge them at a glance: federated providers (Continue with X) up top when the server
// enables any, then the email card (password Log In + a passwordless "email me a link"), with dev
// sign-in and the management-only organization token tucked below as advanced options. Every method
// except the org token yields a mint-capable SESSION token, so game-plane authoring works after it.
class SCrowdySignInView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdySignInView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnLoginClicked();
	FReply OnDevLoginClicked();
	FReply OnMagicLinkClicked();
	FReply OnSignInWithTokenClicked();

	// Repopulate the provider button stack from Controller->GetLoginProviders(). Bound to the
	// controller's OnLoginProvidersChanged so it stays in sync when the provider list arrives.
	void RebuildProviders();
	// Re-probe the providers when the backend changes (a different backend can enable different
	// providers). Only while signed out — that's the only time the sign-in buttons are shown.
	void HandleBackendChanged();
	TSharedRef<SWidget> MakeProviderButton(const FString& Provider);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SEditableTextBox> EmailBox;
	TSharedPtr<SEditableTextBox> PasswordBox;
	TSharedPtr<SEditableTextBox> TokenBox;
	// Host for the dynamically built social provider buttons.
	TSharedPtr<SVerticalBox> ProvidersBox;

	bool bShowToken = false;
};
