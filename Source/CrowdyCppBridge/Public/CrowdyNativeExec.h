#pragma once

#include "CoreMinimal.h"

// A ck-exec reply status, by its wire value.
enum class ECrowdyNativeExecStatus : uint8
{
	Ok = 0,
	AppError = 1,
	Busy = 2,
	Moved = 3,
	NotFound = 4,
	DeadlineExceeded = 5,
	Denied = 6,
	RateLimited = 7,
	Unavailable = 8,
	Internal = 9,
	Trapped = 10,
	BadRequest = 11,
	AppPaused = 12,
	AppDisabled = 13,
	TypeDisabled = 14,
	Unknown = 255
};

struct FCrowdyNativeExecReply
{
	ECrowdyNativeExecStatus Status = ECrowdyNativeExecStatus::Unavailable;

	// MessagePack, for Ok.
	TArray<uint8> Payload;

	// The refusal's message, for anything but Ok.
	FString Message;

	bool bRetryable = false;
	bool bRateLimited = false;

	// The client gave up waiting (no reply within the call timeout), as opposed to the server's own deadline.
	bool bNoReply = false;

	// How long a rate-limit refusal asks the caller to wait, or -1 when it does not say.
	int32 RetryAfterMs = -1;

	bool IsOk() const { return Status == ECrowdyNativeExecStatus::Ok; }
};

struct FCrowdyNativeExecPush
{
	FString NodeType;
	FString Key;
	FString Topic;

	// MessagePack.
	TArray<uint8> Payload;
};

struct FCrowdyNativeExecOptions
{
	// Places the connection on the host running this instance. Calls may still name any node type and key.
	FString NodeType;
	FString Key;

	// The longest Time per Call a type may have, 10 s, plus time waiting behind other calls.
	int32 CallTimeoutMs = 20000;
	int32 OpenTimeoutMs = 10000;
	bool bReconnect = true;
	// Dials as the app's developer with a session token instead of as a player: calls arrive as Caller::Developer and may reach any node type.
	bool bAsDeveloper = false;
	// Domains, besides the Game API's own, whose exec gateways may be used, for self-hosted setups.
	TArray<FString> AllowedGatewayDomains;
};

/**
 * One connection to ck-exec, a player's or the app developer's, made by FCrowdyCppClient::CreateExecConnection.
 *
 * Game thread only. Callbacks run from the owning client's Poll(). Every call, ping, subscribe and connect completion
 * is delivered exactly once: a request still pending when the connection or its client closes completes then, as
 * Unavailable with CanceledMessage().
 */
class CROWDYCPPBRIDGE_API FCrowdyNativeExecConnection
{
public:
	struct FImpl;

	// Built by the client; FImpl is private to the bridge.
	explicit FCrowdyNativeExecConnection(TSharedRef<FImpl> InImpl);
	~FCrowdyNativeExecConnection();

	FCrowdyNativeExecConnection(const FCrowdyNativeExecConnection&) = delete;
	FCrowdyNativeExecConnection& operator=(const FCrowdyNativeExecConnection&) = delete;

	// Connect now rather than on the first call.
	void Connect(TFunction<void(bool bConnected, const FString& Error)> OnDone);

	// Payload is MessagePack. A call too large for one frame is refused as BadRequest without being sent.
	void Call(const FString& NodeType, const FString& Key, const FString& Method, TArray<uint8> Payload,
		TFunction<void(const FCrowdyNativeExecReply&)> OnDone);

	// OnDone carries the gateway's answer. A subscription renewed after a reconnect is not reported again, and pushes
	// published while disconnected are not replayed.
	uint64 Subscribe(const FString& NodeType, const FString& Key, const FString& Topic,
		TFunction<void(const FCrowdyNativeExecPush&)> OnPush, TFunction<void(const FCrowdyNativeExecReply&)> OnDone);
	void Unsubscribe(uint64 Handle);

	void Ping(TFunction<void(const FCrowdyNativeExecReply&)> OnDone);

	// Runs after every reconnect, with the new host.
	void OnReconnect(TFunction<void(const FString& Host)> Listener);

	FString GetHost() const;
	bool IsConnected() const;

	// True when the last dial was refused because the gateway is outside the Game API's estate and not allowed: no retry can fix it.
	bool IsRefusedForGood() const;

	// Why the last dial was refused, empty when it was not.
	FString GetRefusal() const;

	// Runs once, on the game thread, when the server closes the connection because the player's access ended (suspended, revoked, expired, signed out or blocked); the connection is then final. A listener added after the end runs at once.
	void OnAccessEnded(TFunction<void(const FString& Reason)> Listener);

	// True once the server closed the connection because the player's access ended: no call or reconnect on it can succeed.
	bool IsAccessEnded() const;

	// The server's words for why the access ended, empty until it did.
	FString GetAccessEndedReason() const;

	// Terminal: nothing reconnects, and pending completions are delivered as canceled. Safe to call more than once.
	void Close();

	static const FString& CanceledMessage();

private:
	TSharedRef<FImpl> Impl;
};
