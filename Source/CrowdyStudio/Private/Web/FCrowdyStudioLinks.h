// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Builds URLs into the web management console — the single source for every
 * security-critical page the editor deliberately does NOT implement natively (members,
 * tokens, environments, billing/wallet, usage, quotas). The base URL is derived from the
 * management API URL (api.* → app.*); pages are org-scoped as /orgs/{slug}?tab={tab}.
 */
class FCrowdyStudioLinks
{
public:
	static FString BaseUrl();

	// The org dashboard (no specific tab).
	static FString Dashboard();

	// An org-scoped console tab, e.g. OrgTab("crowded-kingdom-studios", TEXT("members")).
	static FString OrgTab(const FString& OrgSlug, const TCHAR* Tab);

	// Opens a URL in the user's real external browser. Used for flows that don't behave in
	// embedded CEF (payments) or where the system browser's existing session is preferable.
	static void OpenExternal(const FString& Url);
};
