// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FCrowdyCppClient;
struct FCrowdyCppAppTokenResult;

namespace CrowdyStudioConsent
{
	/** The message a flow reports when the player closes the prompt without accepting. */
	const FString& DeclinedMessage();

	/** True when the mint was refused only for missing consent and no prompt has been shown for this attempt yet. */
	bool ShouldAskConsent(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked);

	/**
	 * Opens the terms and age prompt in an editor window and records the consent on Client when the player accepts.
	 * OnResolved runs exactly once: true after the consent is recorded, false when the player declines or closes the
	 * window. A second request while the window is open joins it instead of opening another.
	 */
	void Ask(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool /*bRecorded*/)> OnResolved);
}
