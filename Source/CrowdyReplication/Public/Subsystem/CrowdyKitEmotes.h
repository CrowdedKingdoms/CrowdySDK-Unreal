#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyKitEmotes.generated.h"

class AActor;
struct FCrowdyDelivery;
struct FKitEventRequest;

/** An emote reached this client. The actors are None when no proxy for that uuid exists here; the uuids are the text the sender's client wrote, and Target Uuid is empty when the emote is aimed at nobody. The sender and the emote text are only what the sender's client claims: treat them as untrusted, and map the text through your own list before using it as an asset key. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FCrowdyKitOnEmote, AActor*, SenderActor, const FString&, SenderUuid, const FString&, Emote, AActor*, TargetActor, const FString&, TargetUuid);

/**
 * Emotes: a short name such as wave or cheer that a player sends to the players near them, optionally aimed at one actor.
 *
 * It rides the Game Kit's client event 101 with a JSON body of the emote and the target's uuid, so players on
 * CrowdyCPP, CrowdyJS and CrowdyPy see each other's emotes. It is view state: nothing here is kept or checked, and
 * a game that must trust an emote asks a Server Object instead. Each sender has its own small budget (the
 * crowdy.kit.emotes.persender.maxpersecond and crowdy.kit.emotes.persender.burst console variables), and all
 * senders share one overall budget (crowdy.kit.emotes.maxpersecond and crowdy.kit.emotes.burst); the rest are dropped.
 * The sender id is what the sender's client claims, so a peer that forges a player's id can use up that player's own
 * budget; the shared budget still caps everyone together.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Crowdy Kit Emotes"))
class CROWDYREPLICATION_API UCrowdyKitEmotes : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Fires on the game thread for each emote that reaches this client. The sender, the target and the emote text are what the sender's client claims, not proven; treat the text as untrusted and map it through your own list before using it as an asset key. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Kit|Emotes")
	FCrowdyKitOnEmote OnEmote;

	/** The emotes subsystem of the world Context Object is in; None outside a game world. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Kit|Emotes", meta = (WorldContext = "WorldContextObject", DisplayName = "Get Kit Emotes"))
	static UCrowdyKitEmotes* GetKitEmotes(const UObject* WorldContextObject);

	/**
	 * Sends Emote from the local player pawn's position to the players within two chunks. Target is the actor it is aimed at, or None.
	 * False, with Reason saying why in plain words, when nothing was sent: the emote is empty, the emote and its target do not
	 * fit in 1024 bytes (the limit is the whole message, not the emote alone), the player is not signed in or not connected,
	 * there is no local pawn, or Target is not a replicated actor this client has heard from. True means the connection
	 * accepted it, not that anyone received it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit|Emotes", meta = (WorldContext = "WorldContextObject", DisplayName = "Send Emote"))
	static bool SendEmoteInWorld(const UObject* WorldContextObject, const FString& Emote, AActor* Target, FString& Reason);

	/** The same send on this world's subsystem. */
	bool SendEmote(const FString& Emote, AActor* Target, FString& Reason);

	/** An emote's JSON body as the kit writes it: {"emote":<text>,"targetUuid":<text or null>}. An empty TargetUuid writes null. */
	static bool EncodeEmoteBody(const FString& Emote, const FString& TargetUuid, TArray<uint8>& OutBody, FString& Reason);

	/** Reads a body from any client library. False for anything that is not a JSON object with a text emote, or that is over the size limit; a missing, null or non-text targetUuid reads as empty. */
	static bool DecodeEmoteBody(TConstArrayView<uint8> Body, FString& OutEmote, FString& OutTargetUuid);

	/** Fills Request as the event the kit sends: type 101, two chunks of reach, no decay, from Sender at the given chunk. */
	static void FillEmoteRequest(FKitEventRequest& Request, int64 AppId, const FCrowdyActorId& Sender,
		int64 ChunkX, int64 ChunkY, int64 ChunkZ, TArray<uint8>&& Body);

	/** Reads an inbound emote body and fires OnEmote, or drops it. */
	void ReceiveEmote(const FCrowdyActorId& Sender, TConstArrayView<uint8> Body);

	/** What the event subscription calls: takes a client event's sender and state and passes them to ReceiveEmote. */
	void HandleDelivery(const FCrowdyDelivery& Delivery);

	/** The most senders that keep a budget at once; past it the least recently seen sender's budget is dropped. */
	static constexpr int32 MaxEmoteSenders = 256;

	bool TryAdmitForTest(const FCrowdyActorId& Sender, const double Now) { return TryAdmit(Sender, Now); }
	bool TryAdmitEmoteForTest(const double Now) { return TryAdmitEmote(Now); }
	bool TryAdmitSenderForTest(const FCrowdyActorId& Sender, const double Now) { return TryAdmitSender(Sender, Now); }
	int32 NumEmotesDroppedForTest() const { return EmotesDropped; }
	int32 NumSenderBucketsForTest() const { return SenderBuckets.Num(); }

private:

	struct FEmoteSenderBucket
	{
		double Tokens = 0.0;
		double At = 0.0;
	};

	bool ResolveTargetUuid(AActor* Target, FString& OutTargetUuid, FString& Reason) const;

	/** Admits an emote past Sender's own budget and then the shared one; a shared refusal gives Sender its token back. */
	bool TryAdmit(const FCrowdyActorId& Sender, double Now);

	/** Removes the sender seen longest ago, keeping the table's allocation. */
	void EvictStalestSender();

	/** Takes one token from the shared budget, refilled at the CVar rate up to the CVar burst. False, and counted, when the budget is empty. */
	bool TryAdmitEmote(double Now);

	/** Takes one token from Sender's own budget, refilled at the per-sender CVar rate up to the per-sender burst. False, and counted, when it is empty. */
	bool TryAdmitSender(const FCrowdyActorId& Sender, double Now);

	FCrowdySubscription EmoteSubscription;

	TMap<FCrowdyActorId, FEmoteSenderBucket> SenderBuckets;

	double EmoteTokens = 0.0;
	double EmoteTokensAt = 0.0;
	int32 EmotesDropped = 0;
	bool bReportedEmoteFlood = false;
	bool bReportedSenderFlood = false;
};
