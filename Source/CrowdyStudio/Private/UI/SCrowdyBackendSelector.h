// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;

// The Dev/Prod/Custom backend picker, the custom Management API URL field, and the resulting
// management URL. Shown on the Sign In page (so a fresh project can choose a backend before it
// signs in) and on the Project page. Safe before sign-in: it only reads and writes local settings.
class SCrowdyBackendSelector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyBackendSelector) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FCrowdyStudioController> Controller;
};
