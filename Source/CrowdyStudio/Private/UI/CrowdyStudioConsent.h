// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FCrowdyCppClient;
struct FCrowdyCppAppTokenResult;

namespace CrowdyStudioConsent
{
	/** The messages a flow reports when the prompt closes without an answer. */
	const FString& DeclinedMessage();
	const FString& DeveloperDeclinedMessage();
	FString AgeDeclinedMessage();

	/** True for a day after an age refusal in Studio: do not ask again. */
	bool IsAgeRefused();

	/** Closes any open window as declined: what it was asking belongs to a session that has ended. */
	void CloseAll();

	/** True when the mint was refused only for missing consent and no prompt has been shown for this attempt yet. */
	bool ShouldAskConsent(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked);

	/** True when the mint was refused because the account has no age and it has not been asked for this attempt yet. */
	bool ShouldAskAge(const FCrowdyCppAppTokenResult& Result, bool bAlreadyAsked);

	/**
	 * Opens the terms prompt in an editor window and records the consent on Client when the player accepts. AppId adds
	 * that app's own documents. OnResolved runs exactly once: true after the consent is recorded, false when the player
	 * declines or closes the window. A second request while the window is open joins it instead of opening another.
	 */
	void Ask(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool /*bRecorded*/)> OnResolved, const FString& AppId = FString());

	/** The same for the developer documents, asking for an age first if the server needs one. */
	void AskDeveloperTerms(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool /*bRecorded*/)> OnResolved);

	/** Opens the age screen in an editor window and records the answer on Client. Resolves as Ask does. */
	void AskAge(const TSharedRef<FCrowdyCppClient>& Client, TFunction<void(bool /*bRecorded*/)> OnResolved);
}
