// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UI/CrowdyStudioPages.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;

// Overview dashboard: at-a-glance status cards (account, organization, active app, project
// sync) plus quick-action buttons that jump to the relevant pages. All values are live-bound,
// so it needs no delegate wiring.
class SCrowdyHomeView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyHomeView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_EVENT(FOnStudioNavigate, OnNavigate)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FCrowdyStudioController> Controller;
	FOnStudioNavigate OnNavigate;
};
