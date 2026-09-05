#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyCppClientSubsystem.generated.h"

/**
 * Owns the one API client the running game issues every server call through, together with its completion pump and
 * its two bearer tokens.
 *
 * There is deliberately one client per game instance rather than one per caller. The client carries a completion
 * queue that has to be drained on the game thread, so every additional client is another pump to register, another
 * teardown to get right, and another set of in-flight requests to reason about. Callers ask for the client, issue on
 * it, and never own it.
 *
 * The client is built on first use and rebuilt only when the configured endpoints change, which is an editor or
 * configuration action rather than a gameplay one. Rebuilding completes any request still in flight on the previous
 * client as canceled, so a caller that needs to survive that sees the cancellation and can reissue. A datacenter
 * redirect is not a rebuild: the client moves itself and keeps its requests.
 *
 * Lifetime note for callers: this subsystem outlives the worlds that use it, so a completion issued by a world
 * subsystem can land after that world has torn down. A caller whose callback touches per-world state must guard it
 * against its own teardown; the client will no longer do that for it by dying alongside the world.
 */
UCLASS()
class CROWDYNET_API UCrowdyCppClientSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Nothing is built at initialization: the client appears on the first GetClient and is disposed here. That is
	// also why there is no game-instance null check on the way in, since nothing dereferences it before use.
	virtual void Deinitialize() override;

	// Resolve the host from any object with a world. Null outside a game instance (an editor-only context).
	static UCrowdyCppClientSubsystem* Get(const UObject* WorldContextObject);

	/**
	 * The runtime client for this configuration, built on first use. Returns null when the client could not be
	 * constructed or is not safe to hand out; the caller should treat the operation as failed rather than retry
	 * immediately.
	 *
	 * A client that has moved datacenter on its own still matches the config it was built from, so a redirect does
	 * not cause a rebuild. Only a change to the configured values does.
	 */
	FCrowdyCppClient* GetClient(const FCrowdyCppClientConfig& Config);

	// The bearer for gameplay calls (an app-scoped token). Safe to set before the client exists.
	void SetGameToken(const FString& Token);

	// The bearer for identity calls (a user session token). Safe to set before the client exists.
	void SetManagementToken(const FString& Token);

private:
	bool TickPollClient(float DeltaTime);

	// Held by shared pointer because an in-flight request completes through the client, so it must outlive the
	// call that issued it.
	TSharedPtr<FCrowdyCppClient> Client;

	// The configuration the live client was built from; a change to it rebuilds the client. Deliberately not read
	// back off the client: that would follow a datacenter move and make every later resolve look like a change.
	FCrowdyCppClientConfig ClientConfig;
	bool bHasClientConfig = false;

	FString GameToken;
	FString ManagementToken;

	FTSTicker::FDelegateHandle PollTickerHandle;
};
