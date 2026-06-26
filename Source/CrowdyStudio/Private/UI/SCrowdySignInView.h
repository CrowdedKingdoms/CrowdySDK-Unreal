// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SEditableTextBox;

// The console's front door, shown only while signed out: a brand hero and an email-first sign-in
// card (with an organization token tucked behind a toggle for management-only access). Once signed
// in, the window navigates away and the header carries the Sign Out button.
class SCrowdySignInView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdySignInView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnLoginClicked();
	FReply OnSignInWithTokenClicked();

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SEditableTextBox> EmailBox;
	TSharedPtr<SEditableTextBox> PasswordBox;
	TSharedPtr<SEditableTextBox> TokenBox;

	bool bShowToken = false;
};
