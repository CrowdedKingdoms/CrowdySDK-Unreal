// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/CrowdyStudioConsent.h"

#include "CrowdyCppClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "UI/SCrowdyConsentPrompt.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudioConsent"

namespace
{
	struct FConsentSession : public TSharedFromThis<FConsentSession>
	{
		TSharedPtr<FCrowdyCppClient> Client;
		TWeakPtr<SWindow> Window;
		TWeakPtr<SCrowdyConsentPrompt> Prompt;
		TArray<TFunction<void(bool)>> Waiters;
		bool bResolved = false;

		void Open();
		void Accept();
		void OnRecorded(const FCrowdyCppBoolResult& Result);
		void Resolve(bool bRecorded);
	};

	TWeakPtr<FConsentSession> GOpenSession;

	void FConsentSession::Open()
	{
		const TSharedRef<FConsentSession> Self = AsShared();
		const TSharedRef<SCrowdyConsentPrompt> NewPrompt = SNew(SCrowdyConsentPrompt)
			.bShowBackdrop(false)
			.OnAccept(FSimpleDelegate::CreateLambda([Self]() { Self->Accept(); }))
			.OnDecline(FSimpleDelegate::CreateLambda([Self]() { Self->Resolve(false); }));

		const TSharedRef<SWindow> NewWindow = SNew(SWindow)
			.Title(LOCTEXT("WindowTitle", "Crowdy Studio: terms and age"))
			.SizingRule(ESizingRule::Autosized)
			.AutoCenter(EAutoCenter::PreferredWorkArea)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				NewPrompt
			];
		NewWindow->SetOnWindowClosed(FOnWindowClosed::CreateLambda(
			[Self](const TSharedRef<SWindow>&) { Self->Resolve(false); }));
		NewWindow->SetWidgetToFocusOnActivate(NewPrompt->GetInitialFocus());

		Prompt = NewPrompt;
		Window = NewWindow;

		FSlateApplication& SlateApp = FSlateApplication::Get();
		const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		if (Root.IsValid())
		{
			SlateApp.AddWindowAsNativeChild(NewWindow, Root.ToSharedRef());
			return;
		}
		SlateApp.AddWindow(NewWindow);
	}

	void FConsentSession::Accept()
	{
		const TSharedPtr<SCrowdyConsentPrompt> Shown = Prompt.Pin();
		if (!Shown.IsValid() || !Client.IsValid())
		{
			Resolve(false);
			return;
		}

		Shown->SetBusy(true);
		Client->RecordPlayerConsents(true, true,
			[Self = AsShared()](FCrowdyCppBoolResult Result) { Self->OnRecorded(Result); });
	}

	void FConsentSession::OnRecorded(const FCrowdyCppBoolResult& Result)
	{
		if (bResolved)
		{
			return;
		}
		if (Result.bOk && Result.bValue)
		{
			Resolve(true);
			return;
		}

		const TSharedPtr<SCrowdyConsentPrompt> Shown = Prompt.Pin();
		if (!Shown.IsValid())
		{
			return;
		}
		Shown->SetBusy(false);
		Shown->SetError(Result.ErrorMessage.IsEmpty()
			? LOCTEXT("NotRecorded", "The server did not record the consent.")
			: FText::FromString(Result.ErrorMessage));
	}

	void FConsentSession::Resolve(bool bRecorded)
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		TArray<TFunction<void(bool)>> Pending = MoveTemp(Waiters);
		Waiters.Reset();
		if (const TSharedPtr<SWindow> Shown = Window.Pin())
		{
			Shown->RequestDestroyWindow();
		}
		for (TFunction<void(bool)>& Waiter : Pending)
		{
			Waiter(bRecorded);
		}
	}
}

const FString& CrowdyStudioConsent::DeclinedMessage()
{
	static const FString Message =
		TEXT("Studio needs the terms and the age confirmation accepted for this account.");
	return Message;
}

bool CrowdyStudioConsent::ShouldAskConsent(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked)
{
	return !Result.bOk && !bAlreadyAsked
		&& Result.ErrorCode.Equals(CrowdyCppLegalAcceptanceRequiredCode, ESearchCase::CaseSensitive);
}

void CrowdyStudioConsent::Ask(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool)> OnResolved)
{
	const TSharedPtr<FConsentSession> Open = GOpenSession.Pin();
	if (Open.IsValid() && !Open->bResolved)
	{
		Open->Waiters.Add(MoveTemp(OnResolved));
		return;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OnResolved(false);
		return;
	}

	const TSharedRef<FConsentSession> Session = MakeShared<FConsentSession>();
	Session->Client = Client;
	Session->Waiters.Add(MoveTemp(OnResolved));
	GOpenSession = Session;
	Session->Open();
}

#undef LOCTEXT_NAMESPACE
