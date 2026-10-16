#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SEditableTextBox;

// The signed-in developer's own account: their name, their password and their sessions.
class SCrowdyAccountView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyAccountView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void HandleSignInStateChanged();
	void HandleProfileChanged();

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SEditableTextBox> GamertagBox;
	TSharedPtr<SEditableTextBox> DisambiguationBox;
	TSharedPtr<SEditableTextBox> CurrentPasswordBox;
	TSharedPtr<SEditableTextBox> NewPasswordBox;
};
