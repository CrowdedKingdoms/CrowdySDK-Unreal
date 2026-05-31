#include "Network/GraphQL/Subscription/CrowdySubscriptionSubsystem.h"
#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Subsystem initialised."));
}

void UCrowdySubscriptionSubsystem::Deinitialize()
{
	// Tear down timers before destroying the socket.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PingTimerHandle);
		World->GetTimerManager().ClearTimer(ReconnectTimerHandle);
	}

	if (Socket)
	{
		// Close gracefully — no need to send individual "complete" messages;
		// the server cleans up when the transport drops.
		Socket->Close();
		Socket = nullptr;
	}

	ActiveSubscriptions.Empty();
	SetState(ESubscriptionConnectionState::Idle);

	Super::Deinitialize();
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection control
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::Connect(const FString& AuthToken)
{
	const ESubscriptionConnectionState Current = GetSubscriptionState();
	if (Current == ESubscriptionConnectionState::Connecting ||
		Current == ESubscriptionConnectionState::Ready)
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Already connected/connecting — ignoring Connect()."));
		return;
	}

	StoredAuthToken   = AuthToken;
	ReconnectAttempts = 0;

	CreateAndConnectSocket();
}

void UCrowdySubscriptionSubsystem::Disconnect()
{
	// Stop auto-reconnect so the explicit Disconnect is respected.
	ReconnectAttempts = MaxReconnectAttempts;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PingTimerHandle);
		World->GetTimerManager().ClearTimer(ReconnectTimerHandle);
	}

	if (Socket)
	{
		Socket->Close(1000, TEXT("Client disconnected"));
		Socket = nullptr;
	}

	SetState(ESubscriptionConnectionState::Idle);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Disconnected."));
}

ESubscriptionConnectionState UCrowdySubscriptionSubsystem::GetSubscriptionState() const
{
	return static_cast<ESubscriptionConnectionState>(State.load(std::memory_order_relaxed));
}

// ─────────────────────────────────────────────────────────────────────────────
// Subscription management
// ─────────────────────────────────────────────────────────────────────────────

FCrowdySubscriptionHandle UCrowdySubscriptionSubsystem::Subscribe(
	const FString& Query,
	const TMap<FString, FString>& Variables,
	ICrowdySubscriptionHandler* Handler)
{
	if (!Handler)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySubscriptions] Subscribe() called with a null handler — ignored."));
		return FCrowdySubscriptionHandle::Invalid();
	}

	if (Query.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySubscriptions] Subscribe() called with an empty query — ignored."));
		return FCrowdySubscriptionHandle::Invalid();
	}

	const FString ID = MakeSubscriptionID();

	FSubscriptionEntry Entry;
	Entry.Query     = Query;
	Entry.Variables = Variables;
	Entry.Handler   = Handler;

	ActiveSubscriptions.Add(ID, MoveTemp(Entry));

	if (GetSubscriptionState() == ESubscriptionConnectionState::Ready)
	{
		SendSubscribeMessage(ID, ActiveSubscriptions[ID]);
	}
	else
	{
		UE_LOG(LogTemp, Log,
			TEXT("[CrowdySubscriptions] Subscription '%s' queued — will send after connection_ack."), *ID);
	}

	return { ID };
}

void UCrowdySubscriptionSubsystem::Unsubscribe(const FCrowdySubscriptionHandle& Handle)
{
	if (!Handle.IsValid())
		return;

	if (!ActiveSubscriptions.Contains(Handle.ID))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySubscriptions] Unsubscribe: handle '%s' not found."), *Handle.ID);
		return;
	}

	if (GetSubscriptionState() == ESubscriptionConnectionState::Ready)
		SendUnsubscribeMessage(Handle.ID);

	ActiveSubscriptions.Remove(Handle.ID);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Unsubscribed '%s'."), *Handle.ID);
}

void UCrowdySubscriptionSubsystem::UnsubscribeAll()
{
	if (GetSubscriptionState() == ESubscriptionConnectionState::Ready)
	{
		for (const auto& Pair : ActiveSubscriptions)
			SendUnsubscribeMessage(Pair.Key);
	}

	ActiveSubscriptions.Empty();
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] All subscriptions cancelled."));
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket creation
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::CreateAndConnectSocket()
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings || Settings->GameApiWsUrl.IsEmpty())
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CrowdySubscriptions] GameApiWsUrl is not set in Project Settings → Plugins → Crowdy SDK."));
		return;
	}

	SetState(ESubscriptionConnectionState::Connecting);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Connecting to %s"), *Settings->GameApiWsUrl);

	// graphql-transport-ws is the modern GraphQL-over-WS sub-protocol.
	Socket = FWebSocketsModule::Get().CreateWebSocket(
		Settings->GameApiWsUrl, TEXT("graphql-transport-ws"));

	Socket->OnConnected().AddUObject(this, &UCrowdySubscriptionSubsystem::HandleConnected);
	Socket->OnConnectionError().AddUObject(this, &UCrowdySubscriptionSubsystem::HandleConnectionError);
	Socket->OnClosed().AddUObject(this, &UCrowdySubscriptionSubsystem::HandleClosed);
	Socket->OnMessage().AddUObject(this, &UCrowdySubscriptionSubsystem::HandleMessage);

	Socket->Connect();
}

// ─────────────────────────────────────────────────────────────────────────────
// WS event handlers  (all fire on the game thread)
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::HandleConnected()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] WebSocket connected — sending connection_init."));
	// graphql-transport-ws: send connection_init immediately after connect.
	SendConnectionInit();
}

void UCrowdySubscriptionSubsystem::HandleConnectionError(const FString& Error)
{
	UE_LOG(LogTemp, Error, TEXT("[CrowdySubscriptions] WebSocket error: %s"), *Error);
	SetState(ESubscriptionConnectionState::Reconnecting);
	OnDisconnected.Broadcast(Error);
	ScheduleReconnect();
}

void UCrowdySubscriptionSubsystem::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(PingTimerHandle);

	UE_LOG(LogTemp, Log,
		TEXT("[CrowdySubscriptions] WebSocket closed — code=%d reason='%s' clean=%d"),
		StatusCode, *Reason, bWasClean ? 1 : 0);

	if (GetSubscriptionState() != ESubscriptionConnectionState::Idle)
	{
		// Unexpected close — schedule reconnect.
		SetState(ESubscriptionConnectionState::Reconnecting);
		OnDisconnected.Broadcast(Reason);
		ScheduleReconnect();
	}
}

void UCrowdySubscriptionSubsystem::HandleMessage(const FString& MessageText)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MessageText);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CrowdySubscriptions] Failed to parse server message: %s"), *MessageText);
		return;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySubscriptions] Server message has no 'type' field."));
		return;
	}

	if (Type == TEXT("connection_ack"))
	{
		OnConnectionAck();
	}
	else if (Type == TEXT("next"))
	{
		// Extract { data: { ... } } from the payload
		FString ID;
		Root->TryGetStringField(TEXT("id"), ID);

		TSharedPtr<FJsonObject> DataObj;
		const TSharedPtr<FJsonObject>* Payload;
		if (Root->TryGetObjectField(TEXT("payload"), Payload))
		{
			const TSharedPtr<FJsonObject>* Data;
			if ((*Payload)->TryGetObjectField(TEXT("data"), Data))
				DataObj = *Data;
		}

		OnNext(ID, DataObj);
	}
	else if (Type == TEXT("error"))
	{
		FString ID;
		Root->TryGetStringField(TEXT("id"), ID);
		const TArray<TSharedPtr<FJsonValue>>* Errors;
		if (Root->TryGetArrayField(TEXT("payload"), Errors))
			OnError(ID, *Errors);
	}
	else if (Type == TEXT("complete"))
	{
		FString ID;
		Root->TryGetStringField(TEXT("id"), ID);
		OnServerComplete(ID);
	}
	else if (Type == TEXT("ping"))
	{
		// Server-initiated ping — reply with pong as the spec requires.
		OnServerPing();
	}
	else if (Type == TEXT("pong"))
	{
		// Response to our own ping — nothing to do.
	}
	else if (Type == TEXT("connection_error"))
	{
		// Non-standard but some servers send this.
		FString Msg;
		Root->TryGetStringField(TEXT("payload"), Msg);
		UE_LOG(LogTemp, Error, TEXT("[CrowdySubscriptions] connection_error from server: %s"), *Msg);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("[CrowdySubscriptions] Unhandled message type '%s'."), *Type);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// graphql-transport-ws outgoing messages
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::SendConnectionInit() const
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	if (!StoredAuthToken.IsEmpty())
		Payload->SetStringField(TEXT("Authorization"),
			FString::Printf(TEXT("Bearer %s"), *StoredAuthToken));

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("connection_init"));
	Msg->SetObjectField(TEXT("payload"), Payload);

	SendRawJson(Msg);
}

void UCrowdySubscriptionSubsystem::SendSubscribeMessage(
	const FString& ID, const FSubscriptionEntry& Entry) const
{
	// Build the variables object from the string map.
	TSharedRef<FJsonObject> VarsObj = MakeShared<FJsonObject>();
	for (const auto& Pair : Entry.Variables)
		VarsObj->SetStringField(Pair.Key, Pair.Value);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("query"), Entry.Query);
	Payload->SetObjectField(TEXT("variables"), VarsObj);

	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("subscribe"));
	Msg->SetStringField(TEXT("id"), ID);
	Msg->SetObjectField(TEXT("payload"), Payload);

	SendRawJson(Msg);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Sent subscribe '%s'."), *ID);
}

void UCrowdySubscriptionSubsystem::SendUnsubscribeMessage(const FString& ID) const
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("complete"));
	Msg->SetStringField(TEXT("id"), ID);
	SendRawJson(Msg);
}

void UCrowdySubscriptionSubsystem::SendPing() const
{
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("ping"));
	SendRawJson(Msg);
}

void UCrowdySubscriptionSubsystem::SendRawJson(const TSharedRef<FJsonObject>& Msg) const
{
	if (!Socket || !Socket->IsConnected())
		return;

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (FJsonSerializer::Serialize(Msg, Writer))
		Socket->Send(Output);
}

// ─────────────────────────────────────────────────────────────────────────────
// graphql-transport-ws incoming message handlers
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::OnConnectionAck()
{
	UE_LOG(LogTemp, Log,
		TEXT("[CrowdySubscriptions] connection_ack received — connection is Ready."));

	ReconnectAttempts = 0;
	SetState(ESubscriptionConnectionState::Ready);

	// Start the keep-alive ping timer.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			PingTimerHandle,
			this,
			&UCrowdySubscriptionSubsystem::SendPingFromTimer,
			PingIntervalSeconds, /*bLoop=*/true);
	}

	// Re-establish any subscriptions that were active before a reconnect.
	ResubscribeAll();

	OnConnected.Broadcast();
}

void UCrowdySubscriptionSubsystem::OnNext(
	const FString& ID, const TSharedPtr<FJsonObject>& DataObject)
{
	ICrowdySubscriptionHandler** HandlerPtr = nullptr;

	if (FSubscriptionEntry* Entry = ActiveSubscriptions.Find(ID))
		HandlerPtr = &Entry->Handler;

	if (!HandlerPtr || !(*HandlerPtr))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CrowdySubscriptions] Received 'next' for unknown subscription '%s'."), *ID);
		return;
	}

	if (!DataObject.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CrowdySubscriptions] 'next' payload for '%s' has no valid 'data' object."), *ID);
		return;
	}

	(*HandlerPtr)->OnSubscriptionData(DataObject);
}

void UCrowdySubscriptionSubsystem::OnError(
	const FString& ID, const TArray<TSharedPtr<FJsonValue>>& Errors)
{
	// Collect all error messages into one string for the handler.
	TArray<FString> Messages;
	for (const TSharedPtr<FJsonValue>& ErrVal : Errors)
	{
		if (!ErrVal.IsValid()) continue;

		const TSharedPtr<FJsonObject>* ErrObj;
		FString Msg;
		if (ErrVal->TryGetObject(ErrObj) && (*ErrObj)->TryGetStringField(TEXT("message"), Msg))
			Messages.Add(Msg);
	}

	const FString Combined = Messages.Num() > 0
		? FString::Join(Messages, TEXT("; "))
		: TEXT("Unknown GraphQL error");

	UE_LOG(LogTemp, Warning,
		TEXT("[CrowdySubscriptions] Error on subscription '%s': %s"), *ID, *Combined);

	if (FSubscriptionEntry* Entry = ActiveSubscriptions.Find(ID))
	{
		if (Entry->Handler)
			Entry->Handler->OnSubscriptionError(Combined);
	}
}

void UCrowdySubscriptionSubsystem::OnServerComplete(const FString& ID)
{
	UE_LOG(LogTemp, Log,
		TEXT("[CrowdySubscriptions] Server completed subscription '%s'."), *ID);

	if (FSubscriptionEntry* Entry = ActiveSubscriptions.Find(ID))
	{
		if (Entry->Handler)
			Entry->Handler->OnSubscriptionComplete();
	}

	ActiveSubscriptions.Remove(ID);
}

void UCrowdySubscriptionSubsystem::OnServerPing()
{
	// Reply with pong as the graphql-transport-ws spec requires.
	TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
	Pong->SetStringField(TEXT("type"), TEXT("pong"));
	SendRawJson(Pong);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconnect
// ─────────────────────────────────────────────────────────────────────────────

void UCrowdySubscriptionSubsystem::SetState(const ESubscriptionConnectionState NewState)
{
	State.store(static_cast<uint8>(NewState), std::memory_order_relaxed);
}

void UCrowdySubscriptionSubsystem::ScheduleReconnect()
{
	if (ReconnectAttempts >= MaxReconnectAttempts)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CrowdySubscriptions] Max reconnect attempts (%d) reached. Giving up."),
			MaxReconnectAttempts);
		SetState(ESubscriptionConnectionState::Idle);
		return;
	}

	// Exponential back-off: 1 s, 2 s, 4 s, … capped at MaxReconnectDelay.
	const float Delay =
		FMath::Min(BaseReconnectDelay * FMath::Pow(2.f, (float)ReconnectAttempts), MaxReconnectDelay);
	++ReconnectAttempts;

	UE_LOG(LogTemp, Log,
		TEXT("[CrowdySubscriptions] Reconnect attempt %d/%d in %.1f s."),
		ReconnectAttempts, MaxReconnectAttempts, Delay);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			ReconnectTimerHandle,
			this,
			&UCrowdySubscriptionSubsystem::DoReconnect,
			Delay, /*bLoop=*/false);
	}
}

void UCrowdySubscriptionSubsystem::DoReconnect()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySubscriptions] Attempting reconnect..."));
	CreateAndConnectSocket();
}

void UCrowdySubscriptionSubsystem::ResubscribeAll() const
{
	if (ActiveSubscriptions.IsEmpty())
		return;

	UE_LOG(LogTemp, Log,
		TEXT("[CrowdySubscriptions] Re-sending %d subscription(s) after reconnect."),
		ActiveSubscriptions.Num());

	for (const auto& Pair : ActiveSubscriptions)
		SendSubscribeMessage(Pair.Key, Pair.Value);
}

void UCrowdySubscriptionSubsystem::SendPingFromTimer()
{
	SendPing();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UCrowdySubscriptionSubsystem::MakeSubscriptionID()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}
