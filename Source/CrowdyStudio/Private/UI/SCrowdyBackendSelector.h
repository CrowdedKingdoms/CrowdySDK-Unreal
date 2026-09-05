// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;

// The Dev/Prod/Custom backend picker, the custom Management API URL field, and the resulting
// management URL. Shown only on the Sign In page: a session is authenticated against one management
// plane, so switching backends is a sign-out-and-back-in operation rather than something to offer
// mid-session. Safe before sign-in: it only reads and writes local settings.
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
