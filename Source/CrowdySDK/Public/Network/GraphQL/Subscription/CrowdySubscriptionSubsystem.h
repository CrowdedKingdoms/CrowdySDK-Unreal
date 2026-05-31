#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "Subsystems/GameInstanceSubsystem.h"
#include "Network/GraphQL/Subscription/ICrowdySubscriptionHandler.h"
#include "Network/GraphQL/Subscription/FCrowdySubscriptionHandle.h"
#include "CrowdySubscriptionSubsystem.generated.h"

class IWebSocket;

// ─────────────────────────────────────────────────────────────────────────────
// Connection state — matches the graphql-transport-ws handshake lifecycle.
// ─────────────────────────────────────────────────────────────────────────────
UENUM(BlueprintType)
enum class ESubscriptionConnectionState : uint8
{
	/** Not connected. Call Connect() to start. */
	Idle         UMETA(DisplayName = "Idle"),
	/** WebSocket handshake in progress. */
	Connecting   UMETA(DisplayName = "Connecting"),
	/** connection_ack received — subscriptions are active. */
	Ready        UMETA(DisplayName = "Ready"),
	/** Connection lost; automatic reconnect is scheduled. */
	Reconnecting UMETA(DisplayName = "Reconnecting"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSubscriptionConnected);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSubscriptionDisconnected, FString, Reason);


/**
 * Manages all GraphQL subscriptions over a single persistent WebSocket,
 * using the graphql-transport-ws sub-protocol.
 *
 * ── Typical usage ──────────────────────────────────────────────────────────
 *
 *   // 1. Get the subsystem (usually from UCrowdySDKSubsystem after login)
 *   auto* Sub = GetGameInstance()->GetSubsystem<UCrowdySubscriptionSubsystem>();
 *
 *   // 2. Connect (pass the auth token from login)
 *   Sub->Connect(GameToken);
 *
 *   // 3. Subscribe — keep the handle to cancel later
 *   FCrowdySubscriptionHandle Handle = Sub->Subscribe(
 *       TEXT("subscription OnActorUpdate($appId: BigInt!) { actorUpdate(appId:$appId) { ... } }"),
 *       { { TEXT("appId"), TEXT("1") } },
 *       &MyHandlerInstance);
 *
 *   // 4. When done
 *   Sub->Unsubscribe(Handle);
 *
 * ── Reconnect behaviour ────────────────────────────────────────────────────
 *   If the connection drops, the subsystem reconnects automatically with
 *   exponential back-off (1 s → 2 s → 4 s … capped at 30 s, up to 10 tries).
 *   All active subscriptions are re-established transparently once the new
 *   connection_ack arrives.
 *
 * ── Thread safety ──────────────────────────────────────────────────────────
 *   All public methods must be called from the game thread.
 *   GetSubscriptionState() is safe to call from any thread.
 */
UCLASS(meta=(DisplayName="Crowdy Subscription Subsystem"))
class CROWDYSDK_API UCrowdySubscriptionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Delegates ────────────────────────────────────────────────────────────

	/** Fired on the game thread when connection_ack is received from the server. */
	UPROPERTY(BlueprintAssignable, Category="CrowdySDK|Subscriptions")
	FOnSubscriptionConnected OnConnected;

	/**
	 * Fired when the connection closes or is lost.
	 * The subsystem will attempt to reconnect automatically.
	 */
	UPROPERTY(BlueprintAssignable, Category="CrowdySDK|Subscriptions")
	FOnSubscriptionDisconnected OnDisconnected;

	// ── Connection control ───────────────────────────────────────────────────

	/**
	 * Open the WebSocket using the URL from Developer Settings (GameApiWsUrl).
	 * Call this after a successful login and pass the game auth token; it is
	 * forwarded in the connection_init payload as an Authorization header.
	 *
	 * Safe to call multiple times — a no-op if already connected or connecting.
	 */
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Subscriptions")
	void Connect(const FString& AuthToken);

	/**
	 * Gracefully close the WebSocket and cancel all active subscriptions.
	 * Does not schedule a reconnect.
	 */
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Subscriptions")
	void Disconnect();

	/**
	 * Current state of the WebSocket connection.
	 * Thread-safe — safe to poll from any thread for UI updates.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="CrowdySDK|Subscriptions",
		meta=(DisplayName="Get Subscription State"))
	ESubscriptionConnectionState GetSubscriptionState() const;

	// ── Subscription management ──────────────────────────────────────────────

	/**
	 * Begin a GraphQL subscription.
	 *
	 * @param Query      Full subscription document.
	 *                   e.g. "subscription Foo($id: ID!) { foo(id:$id) { bar } }"
	 * @param Variables  Variable name → string-value pairs for the operation.
	 * @param Handler    Receives data/error/complete events. Must remain valid
	 *                   until Unsubscribe() is called or the subscription ends.
	 *
	 * @return  A handle for this subscription. IsValid() == false if the call
	 *          failed (null handler, etc.). The subscription is queued even if
	 *          the connection is not yet Ready — it will be sent after the next
	 *          connection_ack.
	 */
	FCrowdySubscriptionHandle Subscribe(
		const FString& Query,
		const TMap<FString, FString>& Variables,
		ICrowdySubscriptionHandler* Handler);

	/**
	 * Cancel the subscription identified by Handle.
	 * Sends a graphql-transport-ws "complete" to the server if connected.
	 * The handler will not receive any further callbacks after this returns.
	 */
	void Unsubscribe(const FCrowdySubscriptionHandle& Handle);

	/**
	 * Cancel every active subscription without closing the connection.
	 */
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Subscriptions")
	void UnsubscribeAll();

private:

	// ── Active subscriptions ─────────────────────────────────────────────────

	struct FSubscriptionEntry
	{
		FString                  Query;
		TMap<FString, FString>   Variables;
		ICrowdySubscriptionHandler* Handler = nullptr;
	};

	TMap<FString, FSubscriptionEntry> ActiveSubscriptions;

	// ── WebSocket ────────────────────────────────────────────────────────────

	TSharedPtr<IWebSocket> Socket;
	std::atomic<uint8>     State { 0 };   // ESubscriptionConnectionState::Idle

	FString StoredAuthToken;
	uint32  ReconnectAttempts = 0;

	static constexpr uint32 MaxReconnectAttempts   = 10;
	static constexpr float  PingIntervalSeconds     = 30.f;
	static constexpr float  BaseReconnectDelay      = 1.f;
	static constexpr float  MaxReconnectDelay       = 30.f;

	FTimerHandle PingTimerHandle;
	FTimerHandle ReconnectTimerHandle;

	// ── Internal helpers ─────────────────────────────────────────────────────

	void CreateAndConnectSocket();

	// WS event callbacks
	void HandleConnected();
	void HandleConnectionError(const FString& Error);
	void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void HandleMessage(const FString& MessageText);

	// graphql-transport-ws outgoing messages
	void SendConnectionInit() const;
	void SendSubscribeMessage(const FString& ID, const FSubscriptionEntry& Entry) const;
	void SendUnsubscribeMessage(const FString& ID) const;
	void SendPing() const;
	void SendRawJson(const TSharedRef<FJsonObject>& Msg) const;

	// graphql-transport-ws incoming message handlers
	void OnConnectionAck();
	void OnNext(const FString& ID, const TSharedPtr<FJsonObject>& DataObject);
	void OnError(const FString& ID, const TArray<TSharedPtr<FJsonValue>>& Errors);
	void OnServerComplete(const FString& ID);
	void OnServerPing();

	// Reconnect
	void SetState(ESubscriptionConnectionState NewState);
	void ScheduleReconnect();

	UFUNCTION()
	void DoReconnect();

	UFUNCTION()
	void SendPingFromTimer();

	void ResubscribeAll() const;

	/** Generates a unique subscription ID string. */
	static FString MakeSubscriptionID();
};
