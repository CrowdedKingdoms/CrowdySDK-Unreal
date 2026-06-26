// Fill out your copyright notice in the Description page of Project Settings.

#include "Web/FCrowdyStudioLinks.h"

#include "HAL/PlatformProcess.h"
#include "Settings/CrowdyStudioUserSettings.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

FString FCrowdyStudioLinks::BaseUrl()
{
	// Explicit override wins.
	if (const UCrowdyStudioUserSettings* User = GetDefault<UCrowdyStudioUserSettings>())
	{
		if (!User->WebConsoleUrlOverride.IsEmpty())
		{
			FString Override = User->WebConsoleUrlOverride;
			Override.RemoveFromEnd(TEXT("/"));
			return Override;
		}
	}

	// Otherwise derive from the management API URL — the web console lives on the same host
	// under the app.* subdomain (holds for the shared tier and for dedicated boxes alike).
	FString Url = TEXT("https://app.crowdedkingdoms.com");
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		const FString Management = Settings->GetManagementApiUrl();
		if (!Management.IsEmpty())
		{
			Url = Management.Replace(TEXT("://api."), TEXT("://app."));
		}
	}
	Url.RemoveFromEnd(TEXT("/"));
	return Url;
}

FString FCrowdyStudioLinks::Dashboard()
{
	return BaseUrl();
}

FString FCrowdyStudioLinks::OrgTab(const FString& OrgSlug, const TCHAR* Tab)
{
	return FString::Printf(TEXT("%s/orgs/%s?tab=%s"), *BaseUrl(), *OrgSlug, Tab);
}

void FCrowdyStudioLinks::OpenExternal(const FString& Url)
{
	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
}
