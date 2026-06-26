// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * A full-bleed brand intro that plays once when the console opens: the CROWDED / KINGDOMS
 * wordmark scales and fades in over the panel background, holds briefly, then the whole
 * overlay dissolves to reveal the live console underneath (which defaults to Sign In).
 * Driven imperatively from an active timer so it needs no external ticking; click to skip.
 */
class SCrowdyLogoIntro : public SCompoundWidget
{
public:
	DECLARE_DELEGATE(FOnIntroFinished);

	SLATE_BEGIN_ARGS(SCrowdyLogoIntro) {}
		SLATE_EVENT(FOnIntroFinished, OnFinished)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	EActiveTimerReturnType OnIntroTick(double InCurrentTime, float InDeltaTime);
	void Finish();

	FOnIntroFinished OnFinished;
	TSharedPtr<SWidget> MarkBox;
	float Elapsed = 0.0f;
	bool bDone = false;
};
