// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

/**
 * The neutral age screen: month and year of birth, and country. It picks no answer, shows no age, and shows the parental
 * line only for a date that needs it. The widget only reports the answer; recording it is the host's job.
 */
class CROWDYSERVICES_API SCrowdyAgePrompt : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyAgePrompt)
		: _bShowBackdrop(true)
	{}
		SLATE_EVENT(FSimpleDelegate, OnContinue)
		SLATE_EVENT(FSimpleDelegate, OnDecline)
		SLATE_ARGUMENT(bool, bShowBackdrop)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** While busy the buttons are disabled and any shown error is cleared. */
	void SetBusy(bool bInBusy);
	void SetError(const FText& InError);

	/** Replaces the form with Text and a Close button. There is no way back to the date. */
	void ShowRefusal(const FText& Text);
	bool IsShowingRefusal() const { return bRefused; }
	FText GetError() const { return ErrorText; }

	int32 GetBirthYear() const;
	int32 GetBirthMonth() const { return BirthMonth; }
	FString GetCountryCode() const { return CountryCode; }
	bool HasParentalConfirmation() const { return bDateFixed && bParentalChecked; }

	/** Whether a date makes a player 13 to 17, counted from the last day of the month as the platform counts it. */
	static bool DateNeedsParentalLine(int32 Year, int32 Month, const FDateTime& Now);

	bool NeedsParentalLine() const;
	bool CanContinue() const;

	/** Fixes a date that needs the parental line and shows it first, so editing the date cannot probe for the ages. */
	void Continue();
	void Decline();

	TSharedRef<SWidget> GetInitialFocus();

#if WITH_DEV_AUTOMATION_TESTS
	void SetAnswersForTest(int32 Year, int32 Month, const FString& Country);
	void SetParentalCheckedForTest(bool bChecked) { bParentalChecked = bChecked; }
	bool IsParentalLineShownForTest() const { return bDateFixed; }
#endif

	virtual bool SupportsKeyboardFocus() const override { return true; }

	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override
	{
		return FNavigationReply::Wrap();
	}

private:
	TSharedPtr<SWidget> FirstControl;
	FSimpleDelegate OnContinueDelegate;
	FSimpleDelegate OnDeclineDelegate;
	FText ErrorText;
	FString YearText;
	FString CountryCode;
	FText CountryName;
	TArray<TSharedPtr<int32>> MonthOptions;
	TArray<TSharedPtr<int32>> CountryOptions;
	int32 BirthMonth = 0;
	bool bParentalChecked = false;
	bool bDateFixed = false;
	bool bRefused = false;
	bool bBusy = false;
};
