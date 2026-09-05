#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelCreationPolicy.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelMembershipPolicy.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelPermission.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelError.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMember.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMembership.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPermissions.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPolicy.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelRole.h"
#include "Engine/TimerHandle.h"
#include "CrowdyChannels.generated.h"

class UCrowdyEventRouter;
struct FChannelMessageNotification;

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelSuccess, FCrowdyChannel, Channel);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelsSuccess, TArray<FCrowdyChannel>, Channels);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelMemberSuccess, FCrowdyChannelMember, Member);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelMembersSuccess, TArray<FCrowdyChannelMember>, Members);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelRoleSuccess, FCrowdyChannelRole, Role);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelRolesSuccess, TArray<FCrowdyChannelRole>, Roles);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMyChannelsSuccess, TArray<FCrowdyChannelMembership>, Memberships);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelPolicySuccess, FCrowdyChannelPolicy, Policy);

DECLARE_DYNAMIC_DELEGATE(FOnChannelVoidSuccess);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnChannelError, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMyChannelsCacheChanged, TArray<FCrowdyChannelMembership>, Memberships);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnChannelMessageReceived, int64, ChannelId, FString, SenderUUID,
                                               const TArray<uint8>&, Payload);

/**
 * Channels: named message groups within one app, and the transport reliable RPCs ride on.
 *
 * Every server call here is asynchronous and answers through exactly one of its two delegates, including when the
 * request never reaches the server. Publishing is not one of those calls: it goes out over UDP and is not
 * acknowledged.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyChannels : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Channels")
	FOnMyChannelsCacheChanged OnMyChannelsCacheChanged;

	// Fired on the game thread for every channel message delivered to this client (type 18).
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Channels")
	FOnChannelMessageReceived OnChannelMessageReceived;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool HasCachedChannels() const { return bCachePopulated; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	TArray<FCrowdyChannelMembership> GetCachedMyChannels() const { return CachedMyChannels; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool IsPlayerInChannel(int64 ChannelId) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Cache")
	bool GetMyChannelById(int64 ChannelId, FCrowdyChannelMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool IsInAnyChannel() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool HasPermissionInChannel(int64 ChannelId, ECrowdyChannelPermission Permission) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Request")
	void GetPendingJoinRequests(int64 ChannelId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetMyChannels(FOnMyChannelsSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetChannel(int64 ChannelId, FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetChannels(FOnChannelsSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Members")
	void GetChannelMembers(int64 ChannelId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Roles")
	void GetChannelRoles(int64 ChannelId, FOnChannelRolesSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Policy")
	void GetChannelPolicy(FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void CreateChannel(const FString& Name, const FString& Description,
	                   ECrowdyChannelMembershipPolicy MembershipPolicy, bool bMembersCanSend,
	                   FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void UpdateChannel(int64 ChannelId, const FString& Name, const FString& Description,
	                   FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void DeleteChannel(int64 ChannelId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void JoinChannel(int64 ChannelId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Request")
	void RequestToJoinChannel(int64 ChannelId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void LeaveChannel(int64 ChannelId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Members")
	void AddChannelMember(int64 ChannelId, int64 UserId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Members")
	void RemoveChannelMember(int64 ChannelId, int64 UserId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void CreateChannelRole(int64 ChannelId, const FString& RoleName, FCrowdyChannelPermissions Permissions,
	                       int32 Rank, FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void UpdateChannelRole(int64 ChannelRoleId, const FString& RoleName, FCrowdyChannelPermissions Permissions,
	                       FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void DeleteChannelRole(int64 ChannelRoleId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void SetChannelMemberRoles(int64 ChannelId, int64 UserId, const TArray<int64>& RoleIds,
	                           FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Policy")
	void SetChannelPolicy(ECrowdyChannelCreationPolicy CreationPolicy,
	                      ECrowdyChannelMembershipPolicy DefaultMembershipPolicy,
	                      FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError);

	// Publish a raw payload to a channel over UDP (type 17). Delivered to every active member
	// except the sender as a type-18 notification. The caller must already be a member with the
	// send_messages channel permission. RPC payloads ride over this transport.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Transport")
	void PublishChannelMessage(int64 ChannelId, const TArray<uint8>& Payload);

	// Joins all channels referenced by Multicast CrowdyEvents (by name, resolved against the app's
	// channel list) plus the default session channel, then flushes queued reliable sends. Idempotent
	// and safe to call again on reconnect; the SDK calls it once the UDP connection comes up.
	// A Multicast CrowdyEvent routes over a channel (every member, any distance, never decay-thinned),
	// and a client only receives on channels it has joined.
	void BootstrapReliableRpcChannels();

	/**
	 * The channel names this client must join, given the channels the app's Multicast CrowdyEvents name.
	 *
	 * The session channel is ALWAYS included. It carries the Game Model plane's model-changed pings and effect
	 * signals, and it is the target of a subsystem CrowdyState delta, none of which any Multicast CrowdyEvent
	 * declares. Deriving the set from RPC usage alone therefore leaves a project with no channel-routed events
	 * joined to nothing, and since a client only receives on channels it has joined, every signal and every
	 * re-pull ping is dropped with nothing logged anywhere.
	 *
	 * Sorted, with the session channel last, so the join order is deterministic. Pure and static so the rule is
	 * unit-tested without a live connection.
	 */
	static TArray<FString> BuildDesiredJoinNames(const TSet<FString>& MulticastChannelNames,
	                                             const FString& SessionChannelName);

	// Publishes an encoded reliable RPC payload over the named channel (empty = default session
	// channel). Sends made before the bootstrap finishes are queued and flushed when it does.
	void PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& Payload);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Transport")
	bool AreReliableChannelsReady() const { return bRpcChannelsReady; }

	// Registers an already-joined channel (by id and name) for reliable RPC send and receive.
	// The connect-time bootstrap only wires up channels that already existed; call this after
	// provisioning a channel at runtime (create + join) so a Multicast CrowdyEvent targeting it
	// routes instead of dropping. Idempotent.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Transport")
	void RegisterReliableRpcChannel(int64 ChannelId, const FString& Name);

	// The resolved id of the app-wide default session channel (0 until joined/created).
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Transport")
	int64 GetSessionChannelId() const { return SessionChannelId; }

private:
	TArray<FCrowdyChannelMembership> CachedMyChannels;
	bool bCachePopulated = false;

	// Inbound channel notifications (type 18). Subscribed lazily: the router does not exist yet when
	// this subsystem initializes, so the first opportunity to reach it is the UDP-connect bootstrap.
	FCrowdySubscription ChannelMessageSubscription;
	void EnsureChannelSubscription();
	void HandleChannelMessageNotification(const FChannelMessageNotification& Notification);

	/**
	 * Marks this subsystem's usable lifetime. Every completion handed to the shared API client holds it weakly, so
	 * a request still in flight at teardown lands on nothing rather than on a subsystem whose state has been
	 * cleared.
	 */
	TSharedPtr<uint8> LiveSessionToken;

	uint8 OutgoingSequence = 0;

	// Reliable RPC channel state. Written and read on the game thread only.
	bool bRpcChannelsReady = false;
	bool bBootstrapInFlight = false;
	int64 SessionChannelId = 0;                   // resolved id of the default session channel
	TSet<int64> RpcChannelIds;                    // channels we receive reliable RPCs on
	TMap<FString, int64> JoinedChannelNameToId;   // send-side name -> joined channel id
	FTimerHandle RpcChannelTimeoutTimer;

	// Self-retries spent on the current bootstrap. A failed bootstrap used to wait for a UDP reconnect or the next
	// reliable send, and a client that only consumes Game Model signals makes neither, so one transient read failure
	// cost it every signal for the rest of the session. Reset by a fresh external kick and by a successful bootstrap,
	// deliberately not by a self-retry, so the budget bounds the chain rather than the session.
	int32 BootstrapRetryCount = 0;
	FTimerHandle RpcChannelRetryTimer;

	// A reliable send queued until the bootstrap completes.
	struct FPendingReliableSend
	{
		FString ChannelName;
		TArray<uint8> Payload;
	};
	TArray<FPendingReliableSend> PendingReliableSends;

	// Bootstrap working state (valid only while a bootstrap is in flight).
	TSet<FString> ReferencedChannelNames;
	bool bUsesDefaultChannel = false;
	TMap<FString, int64> AppChannelNameToId;
	TSet<int64> MyChannelIds;
	TArray<TPair<int64, FString>> JoinQueue;
	bool bNeedCreateSessionChannel = false;

	// The well-known, app-wide session channel name. Deterministic so every client converges on it.
	FString GetSessionChannelName() const;

	// Bootstrap chain: list app channels -> list my memberships -> plan the joins -> join each channel
	// sequentially (create the session channel if missing) -> ready.
	// Arms the timeout and enters the chain. Shared by the external kick and the self-retry, which differ only in
	// whether they reset the retry budget first.
	void StartBootstrapAttempt();
	void RetryRpcChannelBootstrap();
	void BootstrapFetchAppChannels();
	void BootstrapFetchMyChannels();
	void BootstrapPlanJoins();
	void ProcessNextJoin();
	void RegisterJoinedChannel(int64 ChannelId, const FString& ChannelName);
	void FinishRpcChannelBootstrap();
	// By value so it can ride a timer delegate payload, which decays its bound argument types.
	void AbortRpcChannelBootstrap(FString Reason);

	// Resolves a channel name (empty = session) to a joined id and publishes, or warns and drops.
	void PublishToResolvedChannel(const FString& ChannelName, const TArray<uint8>& Payload);

	// Decodes a reliable-RPC channel notification into an RPC call and hands it to the event router.
	void ForwardChannelRpc(const TArray<uint8>& Payload);
	UCrowdyEventRouter* ResolveEventRouter() const;

	int64 GetAppId() const;
};
