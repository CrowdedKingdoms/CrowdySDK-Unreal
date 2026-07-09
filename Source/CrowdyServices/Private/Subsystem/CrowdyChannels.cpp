#include "Subsystem/CrowdyChannels.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "CrowdyServicesLog.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Queries/Data/Channels/Responses/FChannelResponses.h"

namespace
{
	// How long the session-channel bootstrap may take before queued reliable sends are dropped.
	constexpr float SessionChannelBootstrapTimeoutSeconds = 10.f;

	// Cap on reliable sends held while the bootstrap is in flight, so a never-ready channel can't
	// grow the queue without bound. The newest send is dropped once the cap is hit.
	constexpr int32 MaxPendingReliablePayloads = 64;
}

namespace ChannelQueries
{
	static const TCHAR* MyChannels =
		TEXT(
			"query MyChannels($appId: BigInt!) { myChannels(appId: $appId) { group { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } roles { groupRoleId groupId roleName rank isSystem permissions createdAt } permissions joinedAt } }");

	static const TCHAR* Channel =
		TEXT(
			"query Channel($groupId: BigInt!) { channel(groupId: $groupId) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* Channels =
		TEXT(
			"query Channels($appId: BigInt!) { channels(appId: $appId) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* ChannelMembers =
		TEXT(
			"query ChannelMembers($groupId: BigInt!) { channelMembers(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* ChannelRoles =
		TEXT(
			"query ChannelRoles($groupId: BigInt!) { channelRoles(groupId: $groupId) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* ChannelPolicy =
		TEXT(
			"query ChannelPolicy($appId: BigInt!) { channelPolicy(appId: $appId) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser } }");

	static const TCHAR* CreateChannel =
		TEXT(
			"mutation CreateChannel($appId: BigInt!, $name: String!, $description: String, $membershipPolicy: String, $membersCanSend: Boolean) { createChannel(input: { appId: $appId, name: $name, description: $description, membershipPolicy: $membershipPolicy, membersCanSend: $membersCanSend }) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* UpdateChannel =
		TEXT(
			"mutation UpdateChannel($groupId: BigInt!, $name: String, $description: String) { updateChannel(input: { groupId: $groupId, name: $name, description: $description }) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* DeleteChannel =
		TEXT("mutation DeleteChannel($groupId: BigInt!) { deleteChannel(groupId: $groupId) }");

	static const TCHAR* JoinChannel =
		TEXT(
			"mutation JoinChannel($groupId: BigInt!) { joinChannel(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* RequestToJoinChannel =
		TEXT(
			"mutation RequestToJoinChannel($groupId: BigInt!) { requestToJoinChannel(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* LeaveChannel =
		TEXT("mutation LeaveChannel($groupId: BigInt!) { leaveChannel(groupId: $groupId) }");

	static const TCHAR* AddChannelMember =
		TEXT(
			"mutation AddChannelMember($groupId: BigInt!, $userId: BigInt!) { addChannelMember(groupId: $groupId, userId: $userId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* RemoveChannelMember =
		TEXT(
			"mutation RemoveChannelMember($groupId: BigInt!, $userId: BigInt!) { removeChannelMember(groupId: $groupId, userId: $userId) }");

	static const TCHAR* CreateChannelRole =
		TEXT(
			"mutation CreateChannelRole($groupId: BigInt!, $roleName: String!, $permissions: [String!], $rank: Int) { createChannelRole(input: { groupId: $groupId, roleName: $roleName, permissions: $permissions, rank: $rank }) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* UpdateChannelRole =
		TEXT(
			"mutation UpdateChannelRole($groupRoleId: BigInt!, $roleName: String, $permissions: [String!]) { updateChannelRole(input: { groupRoleId: $groupRoleId, roleName: $roleName, permissions: $permissions }) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* DeleteChannelRole =
		TEXT("mutation DeleteChannelRole($groupRoleId: BigInt!) { deleteChannelRole(groupRoleId: $groupRoleId) }");

	static const TCHAR* SetChannelMemberRoles =
		TEXT(
			"mutation SetChannelMemberRoles($groupId: BigInt!, $userId: BigInt!, $roleIds: [BigInt!]!) { setChannelMemberRoles(input: { groupId: $groupId, userId: $userId, roleIds: $roleIds }) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* SetChannelPolicy =
		TEXT(
			"mutation SetChannelPolicy($appId: BigInt!, $creationPolicy: String!, $defaultMembershipPolicy: String!) { setChannelPolicy(input: { appId: $appId, creationPolicy: $creationPolicy, defaultMembershipPolicy: $defaultMembershipPolicy }) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser } }");
}

void UCrowdyChannels::InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem)
{
	if (InDataRegistry) InDataRegistry->RegisterLayer(this);
	QuerySubsystem = InQuerySubsystem;
}

void UCrowdyChannels::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyChannels::Deinitialize()
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.Empty();
	Super::Deinitialize();
}

TArray<EQueryResponseType> UCrowdyChannels::GetSupportedResponseType() const
{
	return {
		EQueryResponseType::MyChannels,
		EQueryResponseType::Channel,
		EQueryResponseType::Channels,
		EQueryResponseType::ChannelMembers,
		EQueryResponseType::ChannelRoles,
		EQueryResponseType::ChannelPolicy,
		EQueryResponseType::CreateChannel,
		EQueryResponseType::UpdateChannel,
		EQueryResponseType::DeleteChannel,
		EQueryResponseType::JoinChannel,
		EQueryResponseType::RequestToJoinChannel,
		EQueryResponseType::LeaveChannel,
		EQueryResponseType::AddChannelMember,
		EQueryResponseType::RemoveChannelMember,
		EQueryResponseType::CreateChannelRole,
		EQueryResponseType::UpdateChannelRole,
		EQueryResponseType::DeleteChannelRole,
		EQueryResponseType::SetChannelMemberRoles,
		EQueryResponseType::SetChannelPolicy,
	};
}

void UCrowdyChannels::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response.IsValid()) return;
	FireCallback(Response);
}

TArray<ECrowdyMessageType> UCrowdyChannels::GetSupportedResponseTypes() const
{
	// Inbound channel deliveries only — never re-subscribe to anything else (a layer that claims
	// more would double-handle messages other layers own).
	return {ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION};
}

void UCrowdyChannels::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	if (Message->GetType() != ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION)
		return;

	const FChannelMessageNotification& Notification = static_cast<const FChannelMessageNotification&>(*Message);

	// May arrive on the network thread; hop to the game thread before touching Blueprint delegates.
	const int64 ChannelId = Notification.ChannelId;
	const FString SenderUUID = Notification.UUID;
	const TArray<uint8> Payload = Notification.Payload;

	TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, ChannelId, SenderUUID, Payload]()
	{
		UCrowdyChannels* Self = WeakThis.Get();
		if (!Self)
			return;

		// Traffic on a reliable-RPC channel is decoded and run through the event router, not surfaced
		// to gameplay listeners. Other channels (general app messages) still broadcast below.
		if (Self->bRpcChannelsReady && Self->RpcChannelIds.Contains(ChannelId))
		{
			Self->ForwardChannelRpc(Payload);
			return;
		}

		Self->OnChannelMessageReceived.Broadcast(ChannelId, SenderUUID, Payload);
	});
}

int64 UCrowdyChannels::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

void UCrowdyChannels::PushCallback(EQueryResponseType Type,
                                   TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback)
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.FindOrAdd(Type).Add(MoveTemp(Callback));
}

void UCrowdyChannels::FireCallback(TSharedPtr<ICrowdyQueryResponse> Response)
{
	TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback;
	{
		FScopeLock Lock(&CallbackMutex);
		TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Queue =
			PendingCallbacks.Find(Response->GetResponseType());
		if (Queue && Queue->Num() > 0)
		{
			Callback = MoveTemp((*Queue)[0]);
			Queue->RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	if (Callback)
	{
		TSharedPtr<ICrowdyQueryResponse> ResponseCopy = Response;
		AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), ResponseCopy]()
		{
			Callback(ResponseCopy);
		});
	}
}

TSharedPtr<FJsonObject> UCrowdyChannels::MakeVarsWithStringArray(
	const TMap<FString, FString>& ScalarFields,
	const TMap<FString, TArray<FString>>& StringArrayFields)
{
	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();

	for (const auto& Pair : ScalarFields)
		Vars->SetStringField(Pair.Key, Pair.Value);

	for (const auto& Pair : StringArrayFields)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArr;
		for (const FString& S : Pair.Value)
			JsonArr.Add(MakeShared<FJsonValueString>(S));
		Vars->SetArrayField(Pair.Key, JsonArr);
	}
	return Vars;
}

void UCrowdyChannels::GetMyChannels(FOnMyChannelsSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::MyChannels, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FMyChannelsResponse& R = static_cast<FMyChannelsResponse&>(*Resp);
			CachedMyChannels = R.Memberships;
			bCachePopulated = true;
			OnMyChannelsCacheChanged.Broadcast(CachedMyChannels);
			OnSuccess.ExecuteIfBound(CachedMyChannels);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::MyChannels, ChannelQueries::MyChannels, Vars);
}

void UCrowdyChannels::GetChannel(int64 GroupId, FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Channel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FChannelResponse&>(*Resp).Group);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::Channel, ChannelQueries::Channel, Vars);
}

void UCrowdyChannels::GetChannels(FOnChannelsSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Channels, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			TArray<FCrowdyGroup> Groups = static_cast<FChannelsResponse&>(*Resp).Groups;
			OnSuccess.ExecuteIfBound(Groups);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::Channels, ChannelQueries::Channels, Vars);
}

void UCrowdyChannels::GetChannelMembers(int64 GroupId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::ChannelMembers, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			TArray<FCrowdyGroupMember> Members = static_cast<FChannelMembersResponse&>(*Resp).Members;
			OnSuccess.ExecuteIfBound(Members);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::ChannelMembers, ChannelQueries::ChannelMembers, Vars);
}

void UCrowdyChannels::GetChannelRoles(int64 GroupId, FOnChannelRolesSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::ChannelRoles, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			TArray<FCrowdyGroupRole> Roles = static_cast<FChannelRolesResponse&>(*Resp).Roles;
			OnSuccess.ExecuteIfBound(Roles);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::ChannelRoles, ChannelQueries::ChannelRoles, Vars);
}

void UCrowdyChannels::GetChannelPolicy(FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::ChannelPolicy, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FChannelPolicyResponse&>(*Resp).Policy);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::ChannelPolicy, ChannelQueries::ChannelPolicy, Vars);
}

void UCrowdyChannels::CreateChannel(const FString& Name, const FString& Description,
                                    ECrowdyTeamMembershipPolicy MembershipPolicy, bool bMembersCanSend,
                                    FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::CreateChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FCreateChannelResponse&>(*Resp).Group);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	Vars->SetStringField(TEXT("name"), Name);
	Vars->SetStringField(TEXT("description"), Description);
	Vars->SetStringField(TEXT("membershipPolicy"), FCrowdyGroup::MembershipPolicyToString(MembershipPolicy));
	Vars->SetBoolField(TEXT("membersCanSend"), bMembersCanSend);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateChannel, ChannelQueries::CreateChannel, Vars);
}

void UCrowdyChannels::UpdateChannel(int64 GroupId, const FString& Name, const FString& Description,
                                    FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUpdateChannelResponse&>(*Resp).Group);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	Vars->SetStringField(TEXT("name"), Name);
	Vars->SetStringField(TEXT("description"), Description);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateChannel, ChannelQueries::UpdateChannel, Vars);
}

void UCrowdyChannels::DeleteChannel(int64 GroupId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::DeleteChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid()) OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::DeleteChannel, ChannelQueries::DeleteChannel, Vars);
}

void UCrowdyChannels::JoinChannel(int64 GroupId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::JoinChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FJoinChannelResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::JoinChannel, ChannelQueries::JoinChannel, Vars);
}

void UCrowdyChannels::RequestToJoinChannel(int64 GroupId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::RequestToJoinChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FRequestToJoinChannelResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::RequestToJoinChannel, ChannelQueries::RequestToJoinChannel,
	                                                Vars);
}

void UCrowdyChannels::LeaveChannel(int64 GroupId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::LeaveChannel, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid()) OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::LeaveChannel, ChannelQueries::LeaveChannel, Vars);
}

void UCrowdyChannels::AddChannelMember(int64 GroupId, int64 UserId,
                                       FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::AddChannelMember, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAddChannelMemberResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	Vars->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::AddChannelMember, ChannelQueries::AddChannelMember, Vars);
}

void UCrowdyChannels::RemoveChannelMember(int64 GroupId, int64 UserId,
                                          FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::RemoveChannelMember, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid()) OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	Vars->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::RemoveChannelMember, ChannelQueries::RemoveChannelMember,
	                                                Vars);
}

void UCrowdyChannels::CreateChannelRole(int64 GroupId, const FString& RoleName,
                                        FCrowdyRolePermissions Permissions, int32 Rank,
                                        FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::CreateChannelRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FCreateChannelRoleResponse&>(*Resp).Role);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeVarsWithStringArray(
		{
			{TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId)},
			{TEXT("roleName"), RoleName},
			{TEXT("rank"), FString::FromInt(Rank)}
		},
		{{TEXT("permissions"), Permissions.ToStringArray()}}
	);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateChannelRole, ChannelQueries::CreateChannelRole, Vars);
}

void UCrowdyChannels::UpdateChannelRole(int64 GroupRoleId, const FString& RoleName,
                                        FCrowdyRolePermissions Permissions,
                                        FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateChannelRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUpdateChannelRoleResponse&>(*Resp).Role);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeVarsWithStringArray(
		{{TEXT("groupRoleId"), FString::Printf(TEXT("%lld"), GroupRoleId)}, {TEXT("roleName"), RoleName}},
		{{TEXT("permissions"), Permissions.ToStringArray()}}
	);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateChannelRole, ChannelQueries::UpdateChannelRole, Vars);
}

void UCrowdyChannels::DeleteChannelRole(int64 GroupRoleId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::DeleteChannelRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid()) OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupRoleId"), FString::Printf(TEXT("%lld"), GroupRoleId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::DeleteChannelRole, ChannelQueries::DeleteChannelRole, Vars);
}

void UCrowdyChannels::SetChannelMemberRoles(int64 GroupId, int64 UserId, const TArray<int64>& RoleIds,
                                            FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::SetChannelMemberRoles, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FSetChannelMemberRolesResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	Vars->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));

	TArray<TSharedPtr<FJsonValue>> RoleIdsJson;
	for (int64 RoleId : RoleIds)
		RoleIdsJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), RoleId)));
	Vars->SetArrayField(TEXT("roleIds"), RoleIdsJson);

	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::SetChannelMemberRoles,
	                                                ChannelQueries::SetChannelMemberRoles, Vars);
}

void UCrowdyChannels::SetChannelPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
                                       ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
                                       FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::SetChannelPolicy, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FSetChannelPolicyResponse&>(*Resp).Policy);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	auto CreationStr = [](ECrowdyTeamCreationPolicy P) -> FString
	{
		switch (P)
		{
		case ECrowdyTeamCreationPolicy::Admin: return TEXT("admin");
		case ECrowdyTeamCreationPolicy::Member: return TEXT("member");
		default: return TEXT("anyone");
		}
	};

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	Vars->SetStringField(TEXT("creationPolicy"), CreationStr(CreationPolicy));
	Vars->SetStringField(
		TEXT("defaultMembershipPolicy"), FCrowdyGroup::MembershipPolicyToString(DefaultMembershipPolicy));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::SetChannelPolicy, ChannelQueries::SetChannelPolicy, Vars);
}

void UCrowdyChannels::PublishChannelMessage(int64 ChannelId, const TArray<uint8>& Payload)
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance) return;

	UCrowdySDKBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
	if (!Bridge || !Bridge->SendMessageFn || !IsValid(GameSession))
		return;

	FChannelMessageRequest Request;
	Request.ChannelId = ChannelId;
	Request.UUID = GameSession->GetUUID();
	Request.Payload = Payload;
	Request.SequenceNumber = OutgoingSequence++;

	Bridge->SendMessageFn(Request);
}

FString UCrowdyChannels::GetSessionChannelName() const
{
	// App-wide and deterministic so every client of this app converges on the same channel.
	return FString::Printf(TEXT("__crowdy_session_%lld"), GetAppId());
}

void UCrowdyChannels::BootstrapReliableRpcChannels()
{
	if (bRpcChannelsReady || bBootstrapInFlight)
		return;

	if (!QuerySubsystem)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable RPC channel bootstrap skipped — query subsystem unavailable."));
		return;
	}

	if (GetAppId() <= 0)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable RPC channel bootstrap skipped — AppID is not set."));
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();

	// Which channels do this app's Multicast CrowdyEvents target? We must join them all (a client only
	// receives on channels it has joined), plus the default session channel for unnamed events.
	ReferencedChannelNames.Reset();
	bUsesDefaultChannel = false;
	if (GameInstance)
	{
		if (UCrowdyAutoRegistry* AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>())
			AutoRegistry->CollectMulticastChannels(ReferencedChannelNames, bUsesDefaultChannel);
	}

	if (ReferencedChannelNames.Num() == 0 && !bUsesDefaultChannel)
	{
		// No channel-routed CrowdyEvents at all — nothing to join, trivially ready.
		bRpcChannelsReady = true;
		return;
	}

	bBootstrapInFlight = true;

	// Drop queued reliable sends if the whole chain hasn't completed in time, so a misconfigured
	// channel policy can't strand them forever.
	if (GameInstance)
	{
		GameInstance->GetTimerManager().SetTimer(
			RpcChannelTimeoutTimer,
			FTimerDelegate::CreateUObject(this, &UCrowdyChannels::AbortRpcChannelBootstrap,
				FString(TEXT("timed out"))),
			SessionChannelBootstrapTimeoutSeconds, /*bLoop*/false);
	}

	UE_LOG(LogCrowdyServices, Log,
		TEXT("[CrowdyChannels] Bootstrapping reliable RPC channels (%d named%s)."),
		ReferencedChannelNames.Num(), bUsesDefaultChannel ? TEXT(" + session") : TEXT(""));
	BootstrapFetchAppChannels();
}

void UCrowdyChannels::BootstrapFetchAppChannels()
{
	PushCallback(EQueryResponseType::Channels,
		[this](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (!Resp->IsValid())
		{
			AbortRpcChannelBootstrap(TEXT("could not list channels"));
			return;
		}

		AppChannelNameToId.Reset();
		for (const FCrowdyGroup& G : static_cast<FChannelsResponse&>(*Resp).Groups)
			AppChannelNameToId.Add(G.Name, G.GroupId);

		BootstrapFetchMyChannels();
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::Channels, ChannelQueries::Channels, Vars);
}

void UCrowdyChannels::BootstrapFetchMyChannels()
{
	PushCallback(EQueryResponseType::MyChannels,
		[this](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		MyChannelIds.Reset();
		if (Resp->IsValid())
		{
			for (const FCrowdyGroupMembership& M : static_cast<FMyChannelsResponse&>(*Resp).Memberships)
				MyChannelIds.Add(M.Group.GroupId);
		}
		// Proceed even if the membership lookup failed — the joins below just attempt anyway.
		BootstrapPlanJoins();
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::MyChannels, ChannelQueries::MyChannels, Vars);
}

void UCrowdyChannels::BootstrapPlanJoins()
{
	JoinQueue.Reset();
	bNeedCreateSessionChannel = false;

	// Resolve a channel name to its id: register it immediately if we're already a member, otherwise
	// queue a join. Returns false if the channel doesn't exist for this app.
	auto PlanChannel = [this](const FString& Name) -> bool
	{
		const int64* FoundId = AppChannelNameToId.Find(Name);
		if (!FoundId)
			return false;

		if (MyChannelIds.Contains(*FoundId))
			RegisterJoinedChannel(*FoundId, Name);
		else
			JoinQueue.Add(TPair<int64, FString>(*FoundId, Name));
		return true;
	};

	for (const FString& Name : ReferencedChannelNames)
	{
		if (!PlanChannel(Name))
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[CrowdyChannels] Multicast channel '%s' was not found for this app — RPCs targeting it will drop. Create it (or fix the name) in Crowdy Studio."),
				*Name);
		}
	}

	// The default session channel is the only one we create if missing (member-creation policy).
	if (bUsesDefaultChannel && !PlanChannel(GetSessionChannelName()))
		bNeedCreateSessionChannel = true;

	ProcessNextJoin();
}

void UCrowdyChannels::ProcessNextJoin()
{
	// One outstanding request at a time keeps the FIFO callback queue unambiguous.
	if (JoinQueue.Num() > 0)
	{
		const TPair<int64, FString> Item = JoinQueue.Pop(EAllowShrinking::No);

		PushCallback(EQueryResponseType::JoinChannel,
			[this, Item](TSharedPtr<ICrowdyQueryResponse> Resp)
		{
			if (Resp->IsValid())
				RegisterJoinedChannel(static_cast<FJoinChannelResponse&>(*Resp).Member.GroupId, Item.Value);
			else
				UE_LOG(LogCrowdyServices, Warning,
					TEXT("[CrowdyChannels] Could not join channel '%s' — RPCs targeting it will drop (its membership policy must allow it)."),
					*Item.Value);
			ProcessNextJoin();
		});

		TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
		Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), Item.Key));
		QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::JoinChannel, ChannelQueries::JoinChannel, Vars);
		return;
	}

	if (bNeedCreateSessionChannel)
	{
		bNeedCreateSessionChannel = false;

		PushCallback(EQueryResponseType::CreateChannel,
			[this](TSharedPtr<ICrowdyQueryResponse> Resp)
		{
			if (Resp->IsValid())
				RegisterJoinedChannel(static_cast<FCreateChannelResponse&>(*Resp).Group.GroupId, GetSessionChannelName());
			else
				UE_LOG(LogCrowdyServices, Warning,
					TEXT("[CrowdyChannels] Could not create the session channel — default-channel RPCs will drop (its creation policy must allow members)."));
			ProcessNextJoin();
		});

		TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
		Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
		Vars->SetStringField(TEXT("name"), GetSessionChannelName());
		Vars->SetStringField(TEXT("description"), TEXT("Crowdy reliable RPC session channel"));
		Vars->SetStringField(TEXT("membershipPolicy"),
			FCrowdyGroup::MembershipPolicyToString(ECrowdyTeamMembershipPolicy::Open));
		Vars->SetBoolField(TEXT("membersCanSend"), true);
		QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateChannel, ChannelQueries::CreateChannel, Vars);
		return;
	}

	FinishRpcChannelBootstrap();
}

void UCrowdyChannels::RegisterJoinedChannel(int64 ChannelId, const FString& ChannelName)
{
	if (ChannelId == 0)
		return;

	RpcChannelIds.Add(ChannelId);
	JoinedChannelNameToId.Add(ChannelName, ChannelId);
	if (ChannelName == GetSessionChannelName())
		SessionChannelId = ChannelId;
}

void UCrowdyChannels::RegisterReliableRpcChannel(int64 ChannelId, const FString& Name)
{
	RegisterJoinedChannel(ChannelId, Name);

	// A runtime-provisioned channel can arrive after the bootstrap already marked the
	// system ready; flip the flag so reliable sends to it route now instead of queueing
	// for a bootstrap that will not run again.
	bRpcChannelsReady = true;
}

void UCrowdyChannels::FinishRpcChannelBootstrap()
{
	bRpcChannelsReady = true;
	bBootstrapInFlight = false;

	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(RpcChannelTimeoutTimer);

	UE_LOG(LogCrowdyServices, Log,
		TEXT("[CrowdyChannels] Reliable RPC channels ready (%d joined); flushing %d queued send(s)."),
		RpcChannelIds.Num(), PendingReliableSends.Num());

	for (const FPendingReliableSend& Send : PendingReliableSends)
		PublishToResolvedChannel(Send.ChannelName, Send.Payload);
	PendingReliableSends.Reset();
}

void UCrowdyChannels::AbortRpcChannelBootstrap(FString Reason)
{
	bBootstrapInFlight = false;

	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(RpcChannelTimeoutTimer);

	const int32 Dropped = PendingReliableSends.Num();
	PendingReliableSends.Reset();

	UE_LOG(LogCrowdyServices, Warning,
		TEXT("[CrowdyChannels] Reliable RPC channel bootstrap failed (%s); dropped %d queued send(s). A later reconnect retries."),
		*Reason, Dropped);
}

void UCrowdyChannels::PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& Payload)
{
	if (bRpcChannelsReady)
	{
		PublishToResolvedChannel(ChannelName, Payload);
		return;
	}

	if (PendingReliableSends.Num() >= MaxPendingReliablePayloads)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable send dropped — %d already queued waiting for the RPC channels."),
			PendingReliableSends.Num());
		return;
	}

	PendingReliableSends.Add({ ChannelName, Payload });

	// First reliable send before the SDK kicked the bootstrap (or after a failed attempt): start it.
	if (!bBootstrapInFlight)
		BootstrapReliableRpcChannels();
}

void UCrowdyChannels::PublishToResolvedChannel(const FString& ChannelName, const TArray<uint8>& Payload)
{
	const int64 ChannelId = ChannelName.IsEmpty() ? SessionChannelId : JoinedChannelNameToId.FindRef(ChannelName);
	if (ChannelId != 0)
	{
		if (FCrowdyRPC::IsReliableTraceEnabled())
		{
			UE_LOG(LogCrowdyServices, Log,
				TEXT("[CrowdyChannels] reliable publish channel='%s' id=%lld bytes=%d"),
				ChannelName.IsEmpty() ? TEXT("<session>") : *ChannelName, ChannelId, Payload.Num());
		}
		PublishChannelMessage(ChannelId, Payload);
	}
	else
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable RPC dropped — no joined channel for '%s'."),
			ChannelName.IsEmpty() ? TEXT("<session>") : *ChannelName);
	}
}

void UCrowdyChannels::ForwardChannelRpc(const TArray<uint8>& Payload)
{
	// Discriminate a CrowdyState channel payload from an RPC one by the leading kind tag. A state payload
	// leads with CrowdyChannelStateDeltaTag (0xC5); an RPC payload leads with CrowdyChannelRpcVersion (a
	// small int, never 0xC5), so the two wire formats never collide (see CrowdyChannelStateDeltaTag). The
	// RPC path below is unchanged.
	if (Payload.Num() > 0 && Payload[0] == CrowdyChannelStateDeltaTag)
	{
		FCrowdyStateDelta Delta;
		if (!FCrowdyStateCodec::DecodeChannelStateDelta(Payload, Delta))
			return; // DecodeChannelStateDelta already logged why

		if (FCrowdyRPC::IsReliableTraceEnabled())
		{
			UE_LOG(LogCrowdyServices, Log,
				TEXT("[CrowdyChannels] reliable receive state ClassID=%lld entity=%s bytes=%d"),
				Delta.ClassID, *Delta.EntityID.ToString(), Payload.Num());
		}

		if (UCrowdyEventRouter* Router = ResolveEventRouter())
			Router->ReceiveChannelStateDelta(Delta);
		else
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[CrowdyChannels] Reliable state delta received but no event router in the current world; dropping."));
		return;
	}

	FCrowdyRpcCall Call;
	uint8 Flags = 0;
	if (!FCrowdyRPC::DecodeChannelRpc(Payload, Call, Flags))
		return; // DecodeChannelRpc already logged why

	if (FCrowdyRPC::IsReliableTraceEnabled())
	{
		UE_LOG(LogCrowdyServices, Log,
			TEXT("[CrowdyChannels] reliable receive ClassID=%lld FunctionID=%lld entity=%s bytes=%d"),
			Call.ClassID, Call.FunctionID, *Call.EntityID.ToString(), Payload.Num());
	}

	if (UCrowdyEventRouter* Router = ResolveEventRouter())
		Router->ReceiveChannelRpcCall(Call);
	else
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable RPC received but no event router in the current world; dropping."));
}

UCrowdyEventRouter* UCrowdyChannels::ResolveEventRouter() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UCrowdyEventRouter>() : nullptr;
}

bool UCrowdyChannels::IsPlayerInChannel(int64 GroupId) const
{
	for (const FCrowdyGroupMembership& M : CachedMyChannels)
		if (M.Group.GroupId == GroupId) return true;
	return false;
}

bool UCrowdyChannels::GetMyChannelById(int64 GroupId, FCrowdyGroupMembership& OutMembership) const
{
	for (const FCrowdyGroupMembership& M : CachedMyChannels)
	{
		if (M.Group.GroupId == GroupId)
		{
			OutMembership = M;
			return true;
		}
	}
	return false;
}

bool UCrowdyChannels::IsInAnyChannel() const
{
	return CachedMyChannels.Num() > 0;
}

bool UCrowdyChannels::HasPermissionInChannel(int64 GroupId, ECrowdyTeamPermission Permission) const
{
	FCrowdyGroupMembership Membership;
	if (!GetMyChannelById(GroupId, Membership)) return false;
	return Membership.HasPermission(Permission);
}

void UCrowdyChannels::GetPendingJoinRequests(int64 GroupId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::ChannelMembers, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			TArray<FCrowdyGroupMember> Members = static_cast<FChannelMembersResponse&>(*Resp).Members;
			Members.RemoveAll([](const FCrowdyGroupMember& M) { return M.Status != TEXT("pending"); });
			OnSuccess.ExecuteIfBound(Members);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::ChannelMembers, ChannelQueries::ChannelMembers, Vars);
}
