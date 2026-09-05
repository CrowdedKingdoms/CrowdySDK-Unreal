#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"

#include "CrowdyCppClient.h"
#include "CrowdyNetLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

void UCrowdyCppClientSubsystem::Deinitialize()
{
	// Detach and pin before disposing, for the same reason GetClient does. Disposal delivers every pending
	// cancellation synchronously, and a completion is free to reissue from inside its own delivery: without a local
	// reference the member would be the only owner, so that reissue could rebuild the client here and release the
	// one whose teardown is still on the stack.
	const TSharedPtr<FCrowdyCppClient> Retiring = MoveTemp(Client);
	Client.Reset();
	ClientConfig = FCrowdyCppClientConfig();
	bHasClientConfig = false;
	GameToken.Empty();
	ManagementToken.Empty();

	if (Retiring.IsValid())
	{
		// Disposing fences the server's answers and completes every request still in flight as canceled. A caller
		// whose own state is already torn down guards its callback; one that is still waiting gets told the call is
		// over rather than waiting forever.
		Retiring->Close();
	}

	// Removed last: a reissue during the disposal above can register the ticker again, and it must not survive this
	// subsystem, which is what it is bound to.
	if (PollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
		PollTickerHandle.Reset();
	}

	Super::Deinitialize();
}

UCrowdyCppClientSubsystem* UCrowdyCppClientSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyCppClientSubsystem>() : nullptr;
}

void UCrowdyCppClientSubsystem::SetGameToken(const FString& Token)
{
	GameToken = Token;
	if (Client.IsValid())
	{
		Client->SetGameToken(Token);
	}
}

void UCrowdyCppClientSubsystem::SetManagementToken(const FString& Token)
{
	ManagementToken = Token;
	if (Client.IsValid())
	{
		Client->SetManagementToken(Token);
	}
}

FCrowdyCppClient* UCrowdyCppClientSubsystem::GetClient(const FCrowdyCppClientConfig& InConfig)
{
	if (Client.IsValid() && (!bHasClientConfig || ClientConfig != InConfig))
	{
		// A request in flight on the retiring client cannot follow it to the new endpoint, so each one completes as
		// canceled and its caller decides whether to reissue. Configuration changes are an editor action rather
		// than a gameplay one, so this is rare and worth naming when it happens.
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[CrowdyCpp] API endpoints changed; rebuilding the client. Any request in flight on the previous client is canceled."));

		// Detach the retiring client before disposing it. Disposal delivers those cancellations synchronously, and a
		// caller that reissues from inside one would otherwise be handed a client this call is about to release.
		const TSharedPtr<FCrowdyCppClient> Retiring = MoveTemp(Client);
		Client.Reset();
		ClientConfig = FCrowdyCppClientConfig();
		bHasClientConfig = false;
		Retiring->Close();
	}

	if (!Client.IsValid())
	{
		Client = FCrowdyCppClient::Make(InConfig);
		if (!Client.IsValid())
		{
			UE_LOG(LogCrowdyNet, Error, TEXT("[CrowdyCpp] Could not construct the API client."));
			ClientConfig = FCrowdyCppClientConfig();
			bHasClientConfig = false;
			return nullptr;
		}
		ClientConfig = InConfig;
		bHasClientConfig = true;

		// Drive the completion pump once per frame on the game thread, so callbacks land where engine objects are
		// safe to touch. Registered with the first client and removed on teardown.
		if (!PollTickerHandle.IsValid())
		{
			PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateUObject(this, &UCrowdyCppClientSubsystem::TickPollClient));
		}
	}

	if (!bHasClientConfig || ClientConfig != InConfig)
	{
		// Only reachable when a cancellation delivered during the disposal above reissued against a different
		// configuration and built the live client itself. Handing this caller that client would send its request
		// somewhere it did not ask for, which no log would make obvious, so refuse; the caller can retry next frame.
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[CrowdyCpp] The live client was rebuilt for different endpoints while this one was being disposed; not issuing on it."));
		return nullptr;
	}

	// Reinstall both bearers on every resolve so a sign-in or a re-mint that happened after the client was built is
	// picked up. A token is not part of the client's identity; only the endpoints are.
	Client->SetGameToken(GameToken);
	Client->SetManagementToken(ManagementToken);
	return Client.Get();
}

bool UCrowdyCppClientSubsystem::TickPollClient(float DeltaTime)
{
	// Pin a strong reference across the drain. Completions run synchronously inside the drain and one of them can
	// re-enter here and rebuild the client on an endpoint change, which would otherwise release the very client
	// whose drain is on the stack.
	if (const TSharedPtr<FCrowdyCppClient> Pinned = Client)
	{
		Pinned->Poll();
	}
	return true;
}
