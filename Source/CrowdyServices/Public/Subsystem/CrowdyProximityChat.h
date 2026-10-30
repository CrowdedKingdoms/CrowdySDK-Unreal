#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyProximityChat.generated.h"

struct FTextMessageNotification;

/** One line of proximity text from another player. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyProximityText
{
	GENERATED_BODY()

	/** The sender's actor id, as On Voice Speaker Talking names a speaker. The sender chooses it, so treat it as a claim. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Proximity Chat")
	FString SenderUuid;

	/** The entity Sender Uuid names, for Find Entity: a Crowdy SDK player's pawn; unset when the actor id is not one. Derived from that claim, so never trust it. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Proximity Chat")
	FGuid SenderEntityId;

	/** The line as it arrived: untrusted input, of any length up to what fits in one packet, line breaks included. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Proximity Chat")
	FString Text;
};

/** Why Send Proximity Text did or did not send. */
UENUM(BlueprintType)
enum class ECrowdyProximityChatResult : uint8
{
	/** The line left this client. There is no delivery receipt; a refusal arrives later on On Proximity Text Refused. */
	Sent,
	EmptyText,
	/** Over MaxTextBytes of UTF-8. */
	TooLong,
	/** No realtime connection yet, or the player has no actor id. */
	NotConnected,
	/** No local pawn to place the line at, since its reach is measured from the chunk the pawn stands in. */
	NoPosition
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyProximityTextReceived, const FCrowdyProximityText&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyProximityTextRefused, const FCrowdySendRefusal&, Refusal);

/**
 * Text chat with the players near you: a line reaches every player whose chunk is within the distance you give of the
 * chunk your pawn stands in. The text travels as UTF-8 and nothing else, so it reaches CrowdyJS and CrowdyCPP players
 * too, and carries no name. The sender names itself by an actor id the server does not check, so who sent a line is a claim.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyProximityChat : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The most UTF-8 a line may carry, which leaves it room in one packet. */
	static constexpr int32 MaxTextBytes = 1024;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Sends Text to every player within DistanceChunks chunks (0 to 8) of the chunk your pawn stands in. Nothing is sent
	 * unless the result is Sent, and Sent means only that the line left this client.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Proximity Chat", meta = (ExpandEnumAsExecs = "ReturnValue"))
	ECrowdyProximityChatResult SendProximityText(const FString& Text, int32 DistanceChunks);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Proximity Chat")
	static FString DescribeProximityChatResult(ECrowdyProximityChatResult Result);

	/** The verdict on a send before it goes out; pure so every refusal can be checked without a connection. */
	static ECrowdyProximityChatResult CheckText(const FString& Text, bool bConnected, bool bHasPosition);

	/** The entity id a sender's actor id names, or an unset id when it is not one. */
	static FGuid EntityIdFromSenderUuid(const FString& SenderUuid);

	/** The line Notification carries, or false when it is under the local player's own actor id. */
	static bool ReadReceivedText(const FTextMessageNotification& Notification, const FCrowdyActorId& LocalSenderId, FCrowdyProximityText& OutMessage);

	/** Whether Refusal is the server refusing proximity text. */
	static bool IsTextRefusal(const FCrowdySendRefusal& Refusal);

	/** Fires on the game thread for each line another player sends you. Your own lines never come back. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Proximity Chat")
	FOnCrowdyProximityTextReceived OnProximityTextReceived;

	/** Fires when the server refuses one of your lines, for example because your access tier lacks use_text_chat. Late, at most once a second, and on a busy connection it may name the wrong send: show it, do not act on it. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Proximity Chat")
	FOnCrowdyProximityTextRefused OnProximityTextRefused;

	/** Subscribes to received lines once the realtime registry exists; the Crowdy SDK subsystem calls it when it creates the registry. */
	void EnsureSubscription();

private:
	void HandleTextNotification(const FTextMessageNotification& Notification);

	UFUNCTION()
	void HandleOwnerUUIDUpdated(FString NewUUID);

	UFUNCTION()
	void HandleSendRefused(const FCrowdySendRefusal& Refusal);

	FCrowdySubscription TextSubscription;
};
