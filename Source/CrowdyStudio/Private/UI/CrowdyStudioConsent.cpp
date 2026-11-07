// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/CrowdyStudioConsent.h"

#include "CrowdyCppClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "UI/SCrowdyAgePrompt.h"
#include "UI/SCrowdyConsentPrompt.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudioConsent"

namespace
{
	// The age lock's name for Studio, apart from any game client on this machine.
	const TCHAR* const StudioLockSuffix = TEXT(".studio");

	TArray<FCrowdyLegalDocument> ToDocuments(const FCrowdyCppLegalDocumentsResult& Result)
	{
		TArray<FCrowdyLegalDocument> Documents;
		for (const FCrowdyCppLegalDocument& Listed : Result.Documents)
		{
			FCrowdyLegalDocument& Document = Documents.AddDefaulted_GetRef();
			Document.Title = FText::FromString(Listed.Title);
			Document.Url = Listed.Url;
		}
		return Documents;
	}

	void ShowWindow(const TSharedRef<SWindow>& Window)
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		if (Root.IsValid())
		{
			SlateApp.AddWindowAsNativeChild(Window, Root.ToSharedRef());
			return;
		}
		SlateApp.AddWindow(Window);
	}

	struct FConsentSession : public TSharedFromThis<FConsentSession>
	{
		TSharedPtr<FCrowdyCppClient> Client;
		TWeakPtr<SWindow> Window;
		TWeakPtr<SCrowdyConsentPrompt> Prompt;
		TArray<TFunction<void(bool)>> Waiters;
		FString AppId;
		bool bForDeveloper = false;
		bool bAppHasDocuments = false;
		bool bAgeAsked = false;
		bool bResolved = false;

		void Open();
		void LoadDocuments();
		void ShowDocuments(const TArray<FCrowdyLegalDocument>& Documents);
		void Accept();
		void OnRecorded(const FCrowdyCppBoolResult& Result);
		void Resolve(bool bRecorded);
	};

	struct FAgeSession : public TSharedFromThis<FAgeSession>
	{
		TSharedPtr<FCrowdyCppClient> Client;
		TWeakPtr<SWindow> Window;
		TWeakPtr<SCrowdyAgePrompt> Prompt;
		TArray<TFunction<void(bool)>> Waiters;
		bool bResolved = false;

		void Open();
		void Continue();
		void OnRecorded(const FCrowdyCppBoolResult& Result);
		void Resolve(bool bRecorded);
	};

	TWeakPtr<FConsentSession> GOpenSession;
	TWeakPtr<FAgeSession> GOpenAgeSession;

	void FConsentSession::Open()
	{
		const TSharedRef<FConsentSession> Self = AsShared();
		const TSharedRef<SCrowdyConsentPrompt> NewPrompt = SNew(SCrowdyConsentPrompt)
			.bShowBackdrop(false)
			.OnAccept(FSimpleDelegate::CreateLambda([Self]() { Self->Accept(); }))
			.OnDecline(FSimpleDelegate::CreateLambda([Self]() { Self->Resolve(false); }));
		NewPrompt->SetLoadingDocuments(true);

		const TSharedRef<SWindow> NewWindow = SNew(SWindow)
			.Title(bForDeveloper ? LOCTEXT("DeveloperWindowTitle", "Crowdy Studio: developer terms") : LOCTEXT("WindowTitle", "Crowdy Studio: terms"))
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
		ShowWindow(NewWindow);
		LoadDocuments();
	}

	void FConsentSession::LoadDocuments()
	{
		const TSharedRef<FConsentSession> Self = AsShared();
		if (bForDeveloper)
		{
			// The built-in list is the player's, so with no list there is nothing to agree to.
			Client->ListDeveloperDocuments([Self](FCrowdyCppLegalDocumentsResult Result)
			{
				if (Result.bOk && Result.Documents.Num() > 0)
				{
					Self->ShowDocuments(ToDocuments(Result));
					return;
				}
				if (const TSharedPtr<SCrowdyConsentPrompt> Shown = Self->Prompt.Pin())
				{
					Shown->SetError(LOCTEXT("NoDocuments", "The documents could not be loaded. Close this and sign in again."));
				}
			});
			return;
		}

		Client->ListLegalDocuments(FString(), [Self](FCrowdyCppLegalDocumentsResult Platform)
		{
			TArray<FCrowdyLegalDocument> Documents = Platform.bOk && Platform.Documents.Num() > 0
				? ToDocuments(Platform) : UCrowdyAuthentication::GetLegalDocuments();
			if (Self->AppId.IsEmpty())
			{
				Self->ShowDocuments(Documents);
				return;
			}
			Self->Client->ListLegalDocuments(Self->AppId, [Self, Documents](FCrowdyCppLegalDocumentsResult App) mutable
			{
				Self->bAppHasDocuments = App.bOk && App.Documents.Num() > 0;
				Documents.Append(ToDocuments(App));
				Self->ShowDocuments(Documents);
			});
		});
	}

	void FConsentSession::ShowDocuments(const TArray<FCrowdyLegalDocument>& Documents)
	{
		if (const TSharedPtr<SCrowdyConsentPrompt> Shown = Prompt.Pin())
		{
			Shown->SetDocuments(Documents);
			Shown->SetLoadingDocuments(false);
		}
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
		const auto Recorded = [Self = AsShared()](FCrowdyCppBoolResult Result) { Self->OnRecorded(Result); };
		if (bForDeveloper)
		{
			Client->RecordDeveloperConsents(Recorded);
			return;
		}
		Client->RecordPlayerConsents(true, false, Recorded, bAppHasDocuments ? AppId : FString());
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

		// A developer's documents need an age on the account first. One round: the player's click covers only that.
		if (bForDeveloper && !bAgeAsked && Result.ErrorCode.Equals(CrowdyCppAgeRequiredCode, ESearchCase::CaseSensitive))
		{
			bAgeAsked = true;
			if (const TSharedPtr<SWindow> Shown = Window.Pin())
			{
				Shown->HideWindow();
			}
			CrowdyStudioConsent::AskAge(Client.ToSharedRef(), [Self = AsShared()](bool bRecorded)
			{
				if (!bRecorded)
				{
					Self->Resolve(false);
					return;
				}
				if (const TSharedPtr<SWindow> Shown = Self->Window.Pin())
				{
					Shown->ShowWindow();
				}
				Self->Accept();
			});
			return;
		}

		const TSharedPtr<SCrowdyConsentPrompt> Shown = Prompt.Pin();
		if (!Shown.IsValid())
		{
			return;
		}
		Shown->SetBusy(false);
		if (bForDeveloper && Result.ErrorCode.Equals(CrowdyCppAgeRequirementNotMetCode, ESearchCase::CaseSensitive))
		{
			Shown->SetError(LOCTEXT("NotForThisAccount", "This account can't accept these terms."));
			return;
		}
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

	void FAgeSession::Open()
	{
		const TSharedRef<FAgeSession> Self = AsShared();
		const TSharedRef<SCrowdyAgePrompt> NewPrompt = SNew(SCrowdyAgePrompt)
			.bShowBackdrop(false)
			.OnContinue(FSimpleDelegate::CreateLambda([Self]() { Self->Continue(); }))
			.OnDecline(FSimpleDelegate::CreateLambda([Self]() { Self->Resolve(false); }));

		const TSharedRef<SWindow> NewWindow = SNew(SWindow)
			.Title(LOCTEXT("AgeWindowTitle", "Crowdy Studio: about you"))
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
		ShowWindow(NewWindow);
	}

	void FAgeSession::Continue()
	{
		const TSharedPtr<SCrowdyAgePrompt> Shown = Prompt.Pin();
		if (!Shown.IsValid() || !Client.IsValid())
		{
			Resolve(false);
			return;
		}

		Shown->SetBusy(true);
		Client->RecordPlayerAge(FString::Printf(TEXT("%04d-%02d"), Shown->GetBirthYear(), Shown->GetBirthMonth()), Shown->GetCountryCode(),
			Shown->HasParentalConfirmation(), [Self = AsShared()](FCrowdyCppBoolResult Result) { Self->OnRecorded(Result); });
	}

	void FAgeSession::OnRecorded(const FCrowdyCppBoolResult& Result)
	{
		if (bResolved)
		{
			return;
		}

		// An account that already has an age answers CONFLICT, which is what the caller was waiting for.
		if (Result.bOk || Result.ErrorCode.Equals(CrowdyCppConflictCode, ESearchCase::CaseSensitive))
		{
			Resolve(true);
			return;
		}

		if (Result.ErrorCode.Equals(CrowdyCppAgeRequirementNotMetCode, ESearchCase::CaseSensitive))
		{
			UCrowdyAuthentication::LockAgeEntryFor(StudioLockSuffix);
		}

		const TSharedPtr<SCrowdyAgePrompt> Shown = Prompt.Pin();
		if (!Shown.IsValid())
		{
			return;
		}
		Shown->SetBusy(false);
		if (CrowdyCppIsAccessRefusalCode(Result.ErrorCode))
		{
			Shown->ShowRefusal(FText::FromString(CrowdyCppPlayerRefusalText(Result.ErrorCode, Result.ErrorMessage)));
			return;
		}

		// The server's wording of a refused date is not known to be free of an age, so it is never shown.
		Shown->SetError(Result.ErrorCode.IsEmpty() ? FText::FromString(Result.ErrorMessage) : LOCTEXT("NotSaved", "Those details could not be saved."));
	}

	void FAgeSession::Resolve(bool bRecorded)
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

	void AskFor(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool)> OnResolved, const FString& AppId, bool bForDeveloper)
	{
		const TSharedPtr<FConsentSession> Open = GOpenSession.Pin();
		if (Open.IsValid() && !Open->bResolved)
		{
			// A window on a client that has since been replaced can record nothing.
			if (Open->Client != Client)
			{
				Open->Resolve(false);
			}
			else if (Open->bForDeveloper == bForDeveloper && Open->AppId == AppId)
			{
				Open->Waiters.Add(MoveTemp(OnResolved));
				return;
			}
			else
			{
				// Accepting one list says nothing about another, so this waits its turn.
				Open->Waiters.Add([Client, OnResolved = MoveTemp(OnResolved), AppId, bForDeveloper](bool) mutable
				{
					AskFor(Client, MoveTemp(OnResolved), AppId, bForDeveloper);
				});
				return;
			}
		}
		if (!FSlateApplication::IsInitialized())
		{
			OnResolved(false);
			return;
		}

		const TSharedRef<FConsentSession> Session = MakeShared<FConsentSession>();
		Session->Client = Client;
		Session->AppId = AppId;
		Session->bForDeveloper = bForDeveloper;
		Session->Waiters.Add(MoveTemp(OnResolved));
		GOpenSession = Session;
		Session->Open();
	}
}

const FString& CrowdyStudioConsent::DeclinedMessage()
{
	static const FString Message = TEXT("Studio needs the terms accepted for this account.");
	return Message;
}

const FString& CrowdyStudioConsent::DeveloperDeclinedMessage()
{
	static const FString Message = TEXT("Creating or managing apps needs the developer terms accepted for this account.");
	return Message;
}

FString CrowdyStudioConsent::AgeDeclinedMessage()
{
	// A refusal gives no reason and no way back to the date.
	return IsAgeRefused()
		? CrowdyCppPlayerRefusalText(CrowdyCppAgeRequirementNotMetCode, FString())
		: FString(TEXT("Studio needs your date of birth and country for this account."));
}

bool CrowdyStudioConsent::IsAgeRefused()
{
	return UCrowdyAuthentication::IsAgeEntryLockedFor(StudioLockSuffix);
}

void CrowdyStudioConsent::CloseAll()
{
	if (const TSharedPtr<FConsentSession> Open = GOpenSession.Pin())
	{
		Open->Resolve(false);
	}
	if (const TSharedPtr<FAgeSession> Open = GOpenAgeSession.Pin())
	{
		Open->Resolve(false);
	}
}

bool CrowdyStudioConsent::ShouldAskConsent(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked)
{
	return !Result.bOk && !bAlreadyAsked
		&& Result.ErrorCode.Equals(CrowdyCppLegalAcceptanceRequiredCode, ESearchCase::CaseSensitive);
}

bool CrowdyStudioConsent::ShouldAskAge(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked)
{
	return !Result.bOk && !bAlreadyAsked
		&& Result.ErrorCode.Equals(CrowdyCppAgeRequiredCode, ESearchCase::CaseSensitive);
}

void CrowdyStudioConsent::Ask(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool)> OnResolved, const FString& AppId)
{
	AskFor(Client, MoveTemp(OnResolved), AppId, false);
}

void CrowdyStudioConsent::AskDeveloperTerms(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool)> OnResolved)
{
	AskFor(Client, MoveTemp(OnResolved), FString(), true);
}

void CrowdyStudioConsent::AskAge(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool)> OnResolved)
{
	const TSharedPtr<FAgeSession> Open = GOpenAgeSession.Pin();
	if (Open.IsValid() && !Open->bResolved)
	{
		if (Open->Client == Client)
		{
			Open->Waiters.Add(MoveTemp(OnResolved));
			return;
		}
		Open->Resolve(false);
	}

	// A refusal offers no way back to the date.
	if (!FSlateApplication::IsInitialized() || UCrowdyAuthentication::IsAgeEntryLockedFor(StudioLockSuffix))
	{
		OnResolved(false);
		return;
	}

	const TSharedRef<FAgeSession> Session = MakeShared<FAgeSession>();
	Session->Client = Client;
	Session->Waiters.Add(MoveTemp(OnResolved));
	GOpenAgeSession = Session;
	Session->Open();
}

#undef LOCTEXT_NAMESPACE
