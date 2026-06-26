#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "Queries/Data/Teams/Types/FCrowdyRolePermissions.h"
#include "Engine/TimerHandle.h"
#include "CrowdyChannels.generated.h"

class UCrowdyQuerySubsystem;
class FCrowdyDataRegistry;
class UCrowdyEventRouter;

// Channels reuse the same group model and error type as teams (group_type = channel); the
// delegate payloads are the shared FCrowdyGroup / FCrowdyGroupMember / FCrowdyGroupRole /
// FCrowdyGroupMembership / FCrowdyAppGroupPolicy / FCrowdyTeamError.
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelSuccess, FCrowdyGroup, Channel);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelsSuccess, TArray<FCrowdyGroup>, Channels);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelMemberSuccess, FCrowdyGroupMember, Member);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelMembersSuccess, TArray<FCrowdyGroupMember>, Members);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelRoleSuccess, FCrowdyGroupRole, Role);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelRolesSuccess, TArray<FCrowdyGroupRole>, Roles);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMyChannelsSuccess, TArray<FCrowdyGroupMembership>, Memberships);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnChannelPolicySuccess, FCrowdyAppGroupPolicy, Policy);

DECLARE_DYNAMIC_DELEGATE(FOnChannelVoidSuccess);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnChannelError, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMyChannelsCacheChanged, TArray<FCrowdyGroupMembership>, Memberships);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnChannelMessageReceived, int64, ChannelId, FString, SenderUUID,
                                               const TArray<uint8>&, Payload);

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels : public UGameInstanceSubsystem, public ICrowdyQueryReceptionLayer,
                                           public ICrowdyReceptionLayer
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem);

	// GraphQL response plane (channel CRUD over the Game endpoint).
	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override;

	// UDP message plane (inbound channel notifications, type 18 only).
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Channels")
	FOnMyChannelsCacheChanged OnMyChannelsCacheChanged;

	// Fired on the game thread for every channel message delivered to this client (type 18).
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Channels")
	FOnChannelMessageReceived OnChannelMessageReceived;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool HasCachedChannels() const { return bCachePopulated; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	TArray<FCrowdyGroupMembership> GetCachedMyChannels() const { return CachedMyChannels; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool IsPlayerInChannel(int64 GroupId) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Cache")
	bool GetMyChannelById(int64 GroupId, FCrowdyGroupMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool IsInAnyChannel() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Channels|Cache")
	bool HasPermissionInChannel(int64 GroupId, ECrowdyTeamPermission Permission) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Request")
	void GetPendingJoinRequests(int64 GroupId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetMyChannels(FOnMyChannelsSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetChannel(int64 GroupId, FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Channel")
	void GetChannels(FOnChannelsSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Members")
	void GetChannelMembers(int64 GroupId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Roles")
	void GetChannelRoles(int64 GroupId, FOnChannelRolesSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Queries|Policy")
	void GetChannelPolicy(FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void CreateChannel(const FString& Name, const FString& Description,
	                   ECrowdyTeamMembershipPolicy MembershipPolicy, bool bMembersCanSend,
	                   FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void UpdateChannel(int64 GroupId, const FString& Name, const FString& Description,
	                   FOnChannelSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void DeleteChannel(int64 GroupId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void JoinChannel(int64 GroupId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Request")
	void RequestToJoinChannel(int64 GroupId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Channel")
	void LeaveChannel(int64 GroupId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Members")
	void AddChannelMember(int64 GroupId, int64 UserId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Members")
	void RemoveChannelMember(int64 GroupId, int64 UserId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void CreateChannelRole(int64 GroupId, const FString& RoleName, FCrowdyRolePermissions Permissions,
	                       int32 Rank, FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void UpdateChannelRole(int64 GroupRoleId, const FString& RoleName, FCrowdyRolePermissions Permissions,
	                       FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void DeleteChannelRole(int64 GroupRoleId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Roles")
	void SetChannelMemberRoles(int64 GroupId, int64 UserId, const TArray<int64>& RoleIds,
	                           FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Mutations|Policy")
	void SetChannelPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
	                      ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
	                      FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError);

	// Publish a raw payload to a channel over UDP (type 17). Delivered to every active member
	// except the sender as a type-18 notification. The caller must already be a member with the
	// send_messages channel permission. RPC payloads ride this in Phase 3.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Channels|Transport")
	void PublishChannelMessage(int64 ChannelId, const TArray<uint8>& Payload);

	// ── Reliable RPC channels ────────────────────────────────────────────────
	// A Multicast CrowdyEvent routes over a channel (every member, any distance, never decay-thinned).
	// On connect the SDK joins every channel any Multicast references plus the default session channel,
	// since a client only receives on channels it has joined.

	// Joins all channels referenced by Multicast CrowdyEvents (by name, resolved against the app's
	// channel list) plus the default session channel, then flushes queued reliable sends. Idempotent
	// and safe to call again on reconnect; the SDK calls it once the UDP connection comes up.
	void BootstrapReliableRpcChannels();

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
	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;

	mutable FCriticalSection CallbackMutex;
	TMap<EQueryResponseType, TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>> PendingCallbacks;

	TArray<FCrowdyGroupMembership> CachedMyChannels;
	bool bCachePopulated = false;

	uint8 OutgoingSequence = 0;

	// Reliable RPC channel state. Written and read on the game thread only.
	bool bRpcChannelsReady = false;
	bool bBootstrapInFlight = false;
	int64 SessionChannelId = 0;                   // resolved id of the default session channel
	TSet<int64> RpcChannelIds;                    // channels we receive reliable RPCs on
	TMap<FString, int64> JoinedChannelNameToId;   // send-side name -> joined channel id
	FTimerHandle RpcChannelTimeoutTimer;

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

	void PushCallback(EQueryResponseType Type, TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback);
	void FireCallback(TSharedPtr<ICrowdyQueryResponse> Response);

	static TSharedPtr<FJsonObject> MakeVarsWithStringArray(
		const TMap<FString, FString>& ScalarFields,
		const TMap<FString, TArray<FString>>& StringArrayFields);
};
