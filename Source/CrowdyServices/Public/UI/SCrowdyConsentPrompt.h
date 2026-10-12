// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/**
 * The branded terms and age-of-majority prompt: the legal documents as links, two tick boxes, and
 * Decline / Accept. Accept stays disabled until both boxes are ticked and while the prompt is busy.
 * The widget only reports the choice; recording the consent is the host's job.
 */
class CROWDYSERVICES_API SCrowdyConsentPrompt : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyConsentPrompt)
		: _bShowBackdrop(true)
	{}
		SLATE_EVENT(FSimpleDelegate, OnAccept)
		SLATE_EVENT(FSimpleDelegate, OnDecline)

		/** Dims everything behind the card. Turn it off when the prompt fills its own window. */
		SLATE_ARGUMENT(bool, bShowBackdrop)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** While busy the buttons are disabled and any shown error is cleared. */
	void SetBusy(bool bInBusy);

	/** Shows the text under the tick boxes; empty hides the line. */
	void SetError(const FText& InError);
	FText GetError() const { return ErrorText; }

#if WITH_DEV_AUTOMATION_TESTS
	void SetLegalChecked(bool bChecked) { bLegalChecked = bChecked; }
	void SetAgeChecked(bool bChecked) { bAgeChecked = bChecked; }
#endif

	bool CanAccept() const { return bLegalChecked && bAgeChecked && !bBusy; }

	/** What the Accept button does: fires OnAccept when CanAccept. */
	void Accept();

	/** What the Decline button does: fires OnDecline unless busy. */
	void Decline();

	/** The first tick box: where keyboard and gamepad focus should start. */
	TSharedRef<SWidget> GetInitialFocus() { return FirstControl.IsValid() ? FirstControl.ToSharedRef() : AsShared(); }

	virtual bool SupportsKeyboardFocus() const override { return true; }

	/** Keeps Tab and gamepad navigation inside the prompt. */
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override
	{
		return FNavigationReply::Wrap();
	}

private:
	TSharedRef<SWidget> MakeCheckRow(const FText& Label, bool* bState);
	TSharedRef<SWidget> MakeDocumentLink(const FText& Title, const FString& Url);

	TSharedPtr<SWidget> FirstControl;
	FSimpleDelegate OnAcceptDelegate;
	FSimpleDelegate OnDeclineDelegate;
	FText ErrorText;
	bool bLegalChecked = false;
	bool bAgeChecked = false;
	bool bBusy = false;
};
