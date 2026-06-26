// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UI/CrowdyStudioPages.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;

// Guided first-run setup: a four-step checklist (sign in -> choose app -> link environment ->
// sync to project) that reads live state to show each step's completion and jumps to the
// relevant page. Power users can ignore it and use the nav rail directly.
class SCrowdySetupWizard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdySetupWizard) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_EVENT(FOnStudioNavigate, OnNavigate)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeStep(int32 Number, const FText& Title, const FText& Description,
		TFunction<bool()> IsDone, int32 NavPage, const FText& Cta);

	TSharedPtr<FCrowdyStudioController> Controller;
	FOnStudioNavigate OnNavigate;
};
