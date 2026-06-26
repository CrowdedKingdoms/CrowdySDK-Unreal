// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyWebView.h"

#include "WebBrowserModule.h"
#include "IWebBrowserCookieManager.h"
#include "IWebBrowserSingleton.h"
#include "SWebBrowser.h"
#include "Web/FCrowdyStudioLinks.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyWebView::Construct(const FArguments& InArgs)
{
	CurrentUrl = InArgs._InitialUrl;

	// The WebBrowser module is loaded on demand force it up (and confirm CEF actually
	// initialized) before deciding whether the embedded view is usable.
	FModuleManager::Get().LoadModule(TEXT("WebBrowser"));
	const bool bWebBrowserAvailable =
		IWebBrowserModule::IsAvailable() && IWebBrowserModule::Get().GetSingleton() != nullptr;

	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	if (bWebBrowserAvailable)
	{
		Content = SAssignNew(Browser, SWebBrowser)
			.InitialURL(CurrentUrl)
			.ShowControls(false)
			.ShowAddressBar(false)
			.OnUrlChanged(this, &SCrowdyWebView::HandleUrlChanged)
			.OnLoadCompleted(this, &SCrowdyWebView::HandleLoadCompleted);
	}
	else
	{
		Content = SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("WebViewUnavailable",
					"The embedded browser isn't available here. Use \"Open in external browser\" to manage this in your browser."))
			];
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("WebReload", "Reload"))
				.IsEnabled_Lambda([this]() { return Browser.IsValid(); })
				.OnClicked(this, &SCrowdyWebView::OnReloadClicked)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("WebSecureNote", "Security-critical pages open here; payments and MFA may need your external browser."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("WebOpenExternal", "Open in external browser ↗"))
				.OnClicked(this, &SCrowdyWebView::OnOpenExternalClicked)
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			Content
		]
	];
}

void SCrowdyWebView::LoadUrl(const FString& Url, const FString& InAuthToken)
{
	if (!Browser.IsValid())
	{
		CurrentUrl = Url;
		return;
	}

	// A changed token means re-injecting (e.g. after a fresh sign-in).
	if (InAuthToken != AuthToken)
	{
		AuthToken = InAuthToken;
		bAuthInjected = false;
	}

	if (AuthToken.IsEmpty() || bAuthInjected)
	{
		CurrentUrl = Url;
		Browser->LoadURL(Url);
		return;
	}

	// First authenticated navigation: load the web origin, write the token into its
	// localStorage on completion, then go to the requested page.
	PendingUrl = Url;
	bInjectingAuth = true;
	Browser->LoadURL(FCrowdyStudioLinks::BaseUrl());
}

FReply SCrowdyWebView::OnReloadClicked()
{
	if (Browser.IsValid())
	{
		Browser->Reload();
	}
	return FReply::Handled();
}

FReply SCrowdyWebView::OnOpenExternalClicked()
{
	FCrowdyStudioLinks::OpenExternal(CurrentUrl);
	return FReply::Handled();
}

void SCrowdyWebView::HandleUrlChanged(const FText& Url)
{
	CurrentUrl = Url.ToString();
}

void SCrowdyWebView::ClearSession()
{
	// Drop the web app's stored token (origin-scoped localStorage) while we may still be on
	// its page, plus its cookies, then reset so the next sign-in re-injects fresh.
	if (Browser.IsValid())
	{
		Browser->ExecuteJavascript(TEXT("try{localStorage.removeItem('auth_token');}catch(e){}"));
	}

	if (IWebBrowserModule::IsAvailable() && IWebBrowserModule::Get().GetSingleton())
	{
		if (const TSharedPtr<IWebBrowserCookieManager> Cookies = IWebBrowserModule::Get().GetSingleton()->GetCookieManager())
		{
			Cookies->DeleteCookies(FCrowdyStudioLinks::BaseUrl(), FString());
		}
	}

	AuthToken.Empty();
	PendingUrl.Empty();
	bInjectingAuth = false;
	bAuthInjected = false;
	CurrentUrl = TEXT("about:blank");

	if (Browser.IsValid())
	{
		Browser->LoadURL(TEXT("about:blank"));
	}
}

void SCrowdyWebView::HandleLoadCompleted()
{
	if (!bInjectingAuth || !Browser.IsValid())
	{
		return;
	}

	// The origin is loaded stash the editor's session token where the web app looks for it,
	// then continue to the page the user asked for (now authenticated).
	bInjectingAuth = false;
	bAuthInjected = true;

	Browser->ExecuteJavascript(FString::Printf(
		TEXT("try{localStorage.setItem('auth_token','%s');}catch(e){}"), *AuthToken));

	CurrentUrl = PendingUrl;
	Browser->LoadURL(PendingUrl);
}

#undef LOCTEXT_NAMESPACE
