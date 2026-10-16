#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SEditableTextBox;
class SWidgetSwitcher;

// The signed-in developer's own account: a header card with their name, then Password and Sessions tabs.
class SCrowdyAccountView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyAccountView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyAccountView() override;

private:
	TSharedRef<SWidget> BuildHeaderCard();
	TSharedRef<SWidget> BuildPasswordTab();
	TSharedRef<SWidget> BuildSessionsTab();

	void HandleSignInStateChanged();
	FReply OnSaveNameClicked();
	FReply OnPasswordClicked();

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SWidgetSwitcher> HeaderSwitcher;
	TSharedPtr<SWidgetSwitcher> TabSwitcher;
	TSharedPtr<SEditableTextBox> GamertagBox;
	TSharedPtr<SEditableTextBox> DisambiguationBox;
	TSharedPtr<SEditableTextBox> CurrentPasswordBox;
	TSharedPtr<SEditableTextBox> NewPasswordBox;
	FString ActiveTab = TEXT("password");
	// "change" for an account with a password, "add" for one made by magic link or social sign-in.
	FString PasswordMode = TEXT("change");
};
