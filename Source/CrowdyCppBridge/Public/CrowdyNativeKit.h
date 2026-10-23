#pragma once

#include "CoreMinimal.h"
#include "CrowdyNativeExec.h"

// A server-driven actor a Game Kit hub shows the players near it (a mob, an NPC, a pet, a ball), as its realtime state carries it.
struct FCrowdyNativeKitActorState
{
	FString Kind;
	// Its id within the hub that shows it, the one its calls take.
	int64 Id = 0;
	// Where it is, in chunks.
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	TOptional<int64> Hp;
	TOptional<int64> MaxHp;
	TOptional<FString> Mode;
	// The player it belongs to, such as a pet's.
	TOptional<int64> Owner;
	TOptional<double> Yaw;
	TOptional<FString> Name;
};

namespace CrowdyNativeKit
{
	// False when Bytes are not a kit actor's state, a player's own state say. Any thread.
	CROWDYCPPBRIDGE_API bool DecodeActorState(TConstArrayView<uint8> Bytes, FCrowdyNativeKitActorState& Out);
}

// A Game Kit hub's answer to a call or a subscription: Message holds a coded refusal's words without its code.
struct FCrowdyNativeKitReply : public FCrowdyNativeExecReply
{
	// The hub's refusal code (`full`, `not_your_turn`, ...), empty when the call ran or the platform itself refused it.
	FString Code;
};

/**
 * The platform's Game Kit over a ck-exec connection the SDK already holds, made by FCrowdyCppClient::CreateGameKit.
 *
 * Calls are paced under the gateway's limit and sent again when that is safe; a hub's coded refusal is read into Code.
 * Game thread only. Callbacks run from the owning client's Poll(), and every call and subscribe completion is delivered
 * exactly once: one still pending when the kit or its client closes completes then, as Unavailable. The kit keeps the
 * connection it was made with, so make a new kit when that connection is replaced.
 */
class CROWDYCPPBRIDGE_API FCrowdyNativeKit
{
public:
	struct FImpl;

	// Built by the client; FImpl is private to the bridge.
	explicit FCrowdyNativeKit(TSharedRef<FImpl> InImpl);
	~FCrowdyNativeKit();

	FCrowdyNativeKit(const FCrowdyNativeKit&) = delete;
	FCrowdyNativeKit& operator=(const FCrowdyNativeKit&) = delete;

	// Args is one MessagePack value, or empty for none; CrowdyCPP sends a whole-number float as an integer. bOnce sends a
	// fresh action id, so the hub answers every resend of this call once. TimeoutMs 0 waits as long as the connection does.
	void Call(const FString& NodeType, const FString& Key, const FString& Method, const TArray<uint8>& Args, bool bOnce,
		int32 TimeoutMs, TFunction<void(const FCrowdyNativeKitReply&)> OnDone);

	// Returns 0, with OnDone given the code `limit`, when the kit already holds as many topics as one connection may.
	uint64 Subscribe(const FString& NodeType, const FString& Key, const FString& Topic,
		TFunction<void(const FCrowdyNativeExecPush&)> OnPush, TFunction<void(const FCrowdyNativeKitReply&)> OnDone);
	void Unsubscribe(uint64 Handle);
	int32 NumSubscriptions() const;

	// The player the hubs' `changed` messages are matched against.
	void SetPlayerId(int64 PlayerId);

	// Ends the kit's subscriptions and stops it; the connection it was made with stays open. Safe to call more than once.
	void Close();

	bool IsClosed() const;

	static const FString& CanceledMessage();

private:
	TSharedRef<FImpl> Impl;
};
