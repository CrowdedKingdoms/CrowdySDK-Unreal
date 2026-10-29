#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMember.h"
#include "Subsystem/CrowdyChannels.h"
#include "Subsystem/CrowdyTeams.h"
#include "CrowdyKitSocial.generated.h"

class FCrowdyCppClient;
class FJsonObject;
struct FCrowdyCppJsonResult;

/** A party, a guild or a chat room: its team (0 for a chat room) and its chat channel (0 when it has none). */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyKitGroup
{
	GENERATED_BODY()

	/** The full name the kit uses: party:oakford, guild:wardens, or a chat room's own name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Social")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Social")
	int64 TeamId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Social")
	int64 ChannelId = 0;

	/** The user who owns the team (the channel for a chat room). Anyone can create a team named party:Bob first, so a game can check this is the player it expects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Social")
	int64 OwnerUserId = 0;

	FCrowdyKitGroup() = default;

	FCrowdyKitGroup(const FString& InName, int64 InTeamId, int64 InChannelId, int64 InOwnerUserId = 0)
		: Name(InName), TeamId(InTeamId), ChannelId(InChannelId), OwnerUserId(InOwnerUserId)
	{
	}
};

/** Why Send Chat did or did not send. */
UENUM(BlueprintType)
enum class ECrowdyKitChatResult : uint8
{
	/** The message left this client. The server delivers it to the channel's members; a refusal, such as from a player who is not one, arrives later on the realtime subsystem's On Send Refused. */
	Sent,
	EmptyText,
	/** More than 1024 bytes of UTF-8. */
	TooLong,
	/** The group has no chat channel. */
	NoChannel,
	/** The realtime connection is not up. */
	NotConnected
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyKitChatMessage, FCrowdyKitGroup, Group, FString, SenderUuid, FString, Text);

DECLARE_DELEGATE_OneParam(FCrowdyKitGroupDone, const FCrowdyKitGroup&);
DECLARE_DELEGATE_TwoParams(FCrowdyKitMembersDone, const FCrowdyKitGroup&, const TArray<FCrowdyTeamMember>&);
DECLARE_DELEGATE_OneParam(FCrowdyKitFailed, const FCrowdyTeamError&);

/**
 * Parties, guilds and chat rooms over the app's teams and channels, named the way CrowdyCPP and CrowdyJS name them, so
 * players on any SDK find and share the same groups. A party is a team and a channel both named party:<name>, a guild
 * guild:<name>, a chat room one channel under its own name.
 *
 * Every call answers through exactly one of its two delegates, including when the request never reaches the server.
 * Errors are team errors: NotFound for a missing group or one with no team, NetworkError when no client is available.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* PartyPrefix = TEXT("party:");
	static constexpr const TCHAR* GuildPrefix = TEXT("guild:");

	/** The most UTF-8 bytes one chat message may carry. */
	static constexpr int32 MaxChatBytes = 1024;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** A chat message from a party, guild or chat room of this kit. The sender uuid is what the sender's client claims, not proven. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Kit|Social")
	FOnCrowdyKitChatMessage OnChatMessage;

	/** Creates the party's team (invite only) and its chat channel; the caller leads the team. Fails when either cannot be created. */
	void CreateParty(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	void FindParty(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Adds the user to the party's team and chat channel. Needs the manage_members permission. */
	void InviteToParty(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Joins the party's team and chat channel as the signed-in player. */
	void JoinParty(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Leaves the chat channel, then the team. */
	void LeaveParty(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	void GetPartyMembers(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail);

	/** Creates the guild's team under Policy and its chat channel; the caller leads the team. */
	void CreateGuild(const FString& Name, ECrowdyTeamMembershipPolicy Policy, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	void FindGuild(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Asks to join the guild's team. The chat channel follows when a member accepts the request. */
	void JoinGuild(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Approves the user's join request on the guild's team and adds them to its chat channel. Needs manage_members. */
	void AcceptIntoGuild(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** The guild's members, pending join requests included. */
	void GetGuildRoster(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail);

	/** Replaces the member's roles with RoleIds. Needs the manage_roles permission. */
	void PromoteInGuild(const FCrowdyKitGroup& Group, int64 UserId, const TArray<int64>& RoleIds, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/** Finds the channel named Name or creates it, and joins it unless the player is already in. The first player to create a room owns it. */
	void FindOrCreateChatRoom(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	/**
	 * Grants the guild's members (or only RoleId's holders, when it is not 0) PermissionKeys on a grid; empty keys grant
	 * the defaults. Needs grid admin rights on the app.
	 */
	void ClaimGuildTerritory(const FCrowdyKitGroup& Group, int64 GridId, const TArray<FString>& PermissionKeys, int64 RoleId,
		FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);

	static TArray<FString> DefaultTerritoryPermissionKeys();

	/** Sends Text, as UTF-8 of at most 1024 bytes, to the group's chat channel. Nothing is sent unless the result is Sent. Sent means the message left this client: the server delivers it to the channel's members, and a refusal (a player who is not one, or whose access tier lacks use_text_chat) arrives later on the realtime subsystem's On Send Refused. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit|Social", meta = (ExpandEnumAsExecs = "ReturnValue"))
	ECrowdyKitChatResult SendChat(const FCrowdyKitGroup& Group, const FString& Text);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Kit|Social")
	static FString DescribeChatResult(ECrowdyKitChatResult Result);

	/** The verdict on a send before it goes out; pure so every refusal can be checked without a connection. */
	static ECrowdyKitChatResult CheckChat(const FString& Text, int64 ChannelId, bool bConnected);

	/** The sender id a chat message carries: the session uuid, or before the player's entity resolves one derived from the account. */
	static FString SenderUuidFor(const FString& SessionUuid, int64 UserId);

	/** The group a channel belongs to, as far as this player knows it. */
	FCrowdyKitGroup ResolveChannelGroup(int64 ChannelId) const;

	UFUNCTION()
	void HandleChannelMessage(int64 ChannelId, FString SenderUUID, const TArray<uint8>& Payload);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a subsystem collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, UCrowdyTeams* Teams = nullptr, UCrowdyChannels* Channels = nullptr);

	/** Stands in for the signed-in account's user id. */
	void SetUserIdForTest(int64 UserId) { UserIdForTest = UserId; }

	int32 NumKnownGroupsForTest() const { return KnownGroupsByChannel.Num() + JoinedChannelIds.Num(); }
#endif

private:
	enum class EKitRoute : uint8
	{
		Teams,
		Channels,
		GameApps
	};

	enum class EKitOutcome : uint8
	{
		None,
		Joined,
		Left
	};

	struct FKitStep
	{
		EKitRoute Route = EKitRoute::Teams;
		const TCHAR* Operation = nullptr;
		/** The object the answer must carry; null for an operation whose answer is only acknowledged. */
		const TCHAR* ResultField = nullptr;
		TSharedPtr<FJsonObject> Variables;
		/** Treats an already-member or not-a-member refusal as done, so a half-finished pair call can be repeated. */
		bool bTolerateRepeat = false;
	};

	TSharedPtr<uint8> LiveSessionToken;
	TMap<int64, FCrowdyKitGroup> KnownGroupsByChannel;
	TSet<int64> JoinedChannelIds;

	/** The account the state above belongs to, and a count a reply compares against to tell it was sent for another account. */
	int64 BoundUserId = 0;
	uint32 AccountEpoch = 0;

#if WITH_DEV_AUTOMATION_TESTS
	TOptional<int64> UserIdForTest;
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	TWeakObjectPtr<UCrowdyTeams> TeamsForTest;
	TWeakObjectPtr<UCrowdyChannels> ChannelsForTest;
#endif

	int64 GetAppId() const;
	FCrowdyCppClient* ResolveClient() const;
	UCrowdyTeams* ResolveTeams() const;
	UCrowdyChannels* ResolveChannels() const;
	int64 ResolveUserId() const;
	FString ResolveSenderUuid() const;

	/** Drops what was learned under another account and fails the replies still in flight for it. */
	void SyncAccount();
	bool IsInChannel(int64 ChannelId, const UCrowdyChannels* Channels) const;

	void Send(EKitRoute Route, const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(FCrowdyCppJsonResult)> OnReply);
	void RunSteps(TArray<FKitStep> Steps, int32 Index, const FCrowdyKitGroup& Group, EKitOutcome Outcome,
		FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void Settle(const FCrowdyKitGroup& Group, EKitOutcome Outcome);
	void RefreshCaches();
	bool RequireTeam(const FCrowdyKitGroup& Group, const FCrowdyKitFailed& OnFail) const;

	void CreatePair(const FString& FullName, ECrowdyTeamMembershipPolicy Policy, const FString& Description,
		FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void CreatePairChannel(const FCrowdyKitGroup& Team, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void FindPair(const FString& FullName, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void FindPairChannel(const FCrowdyKitGroup& Team, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void AddToPair(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void GetMembers(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail);
	void JoinChatRoom(const FCrowdyKitGroup& Room, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
	void CreateChatRoom(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail);
};
