#include "Subsystem/CrowdyChannels.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Dom/JsonObject.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"

using namespace CrowdyServiceApi;

namespace
{
	// How long the session-channel bootstrap may take before queued reliable sends are dropped.
	constexpr float SessionChannelBootstrapTimeoutSeconds = 10.f;

	// Cap on reliable sends held while the bootstrap is in flight, so a never-ready channel can't
	// grow the queue without bound. The newest send is dropped once the cap is hit.
	constexpr int32 MaxPendingReliablePayloads = 64;

	// Self-retries a failed bootstrap makes before giving up, and the base delay it multiplies by the attempt
	// number. Bounded because a genuine misconfiguration (an app whose channel policy forbids member creation)
	// fails identically every time, and hammering it helps nobody.
	constexpr int32 MaxBootstrapRetries = 3;
	constexpr float BootstrapRetryBaseSeconds = 2.f;

	// The generated operation set these calls are looked up in, which is also what decides the endpoint each one
	// reaches and the bearer it carries.
	constexpr ECrowdyCppApiDomain ChannelsDomain = ECrowdyCppApiDomain::Channels;

	constexpr const TCHAR* ChannelsLogName = TEXT("CrowdyChannels");
}

void UCrowdyChannels::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);

	// The router does not exist yet at this point in subsystem startup; EnsureChannelSubscription is
	// called again once the UDP connection comes up, which is the earliest point any inbound message
	// could actually arrive.
	EnsureChannelSubscription();
}

void UCrowdyChannels::Deinitialize()
{
	// Released first: a completion can still arrive from the client's own pump after this point, and it must not
	// advance the bootstrap chain or run a Blueprint delegate while the game instance is shutting down.
	LiveSessionToken.Reset();
	ChannelMessageSubscription.Release();

	Super::Deinitialize();
}

void UCrowdyChannels::EnsureChannelSubscription()
{
	if (ChannelMessageSubscription.IsValid())
		return;

	UGameInstance* GameInstance = GetGameInstance();
	UCrowdySDKBridgeSubsystem* Bridge = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>() : nullptr;
	if (!Bridge || !Bridge->ServiceRegistry)
		return;

	FCrowdySubscriptionOptions Options;
	Options.Role = ECrowdySubscriptionRole::Observe;
	Options.SubscriberName = TEXT("CrowdyChannels");

	TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
	ChannelMessageSubscription = Bridge->ServiceRegistry->SubscribeToOpcode<FChannelMessageNotification>(
		ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION, Options,
		[WeakThis](const FChannelMessageNotification& Notification, const FCrowdyDelivery&)
		{
			if (UCrowdyChannels* Self = WeakThis.Get())
				Self->HandleChannelMessageNotification(Notification);
		});
}

void UCrowdyChannels::HandleChannelMessageNotification(const FChannelMessageNotification& Notification)
{
	// Two Game Model notification kinds also ride the session channel, delivered to every member alongside
	// reliable-RPC and chat traffic, and both are handled by the Game Model subsystem - not here. One is the
	// model-changed re-pull hint ("cmc:"), the other is an effect signal ("csg:"). Skip both so neither is
	// mis-decoded as a reliable-RPC frame (which would read bogus lengths out of the trailing text). Every
	// subscriber to this one opcode has to ignore the others' frames, and has to recognize every encoding the
	// Game Model decoders accept, which is why the test lives beside those decoders rather than being inlined here.
	if (CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(Notification.Payload))
	{
		return;
	}

	const int64 ChannelId = Notification.ChannelId;
	const FString SenderUUID = Notification.UUID.ToString();
	const TArray<uint8>& Payload = Notification.Payload;

	// Traffic on a reliable-RPC channel is decoded and run through the event router, not surfaced
	// to gameplay listeners. Other channels (general app messages) still broadcast below.
	if (bRpcChannelsReady && RpcChannelIds.Contains(ChannelId))
	{
		ForwardChannelRpc(Payload);
		return;
	}

	OnChannelMessageReceived.Broadcast(ChannelId, SenderUUID, Payload);
}

int64 UCrowdyChannels::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

void UCrowdyChannels::GetMyChannels(FOnMyChannelsSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
	Client->RunOp(ChannelsDomain, TEXT("MyChannels"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannelMembership> Memberships;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("myChannels"), Memberships, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}

			if (UCrowdyChannels* Self = WeakThis.Get())
			{
				Self->CachedMyChannels = Memberships;
				Self->bCachePopulated = true;
				Self->OnMyChannelsCacheChanged.Broadcast(Self->CachedMyChannels);
			}

			OnSuccess.ExecuteIfBound(Memberships);
		}));
}

void UCrowdyChannels::GetChannel(int64 ChannelId, FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("Channel"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannel Channel;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("channel"), Channel, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Channel);
		}));
}

void UCrowdyChannels::GetChannels(FOnChannelsSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	Client->RunOp(ChannelsDomain, TEXT("Channels"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannel> Channels;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("channels"), Channels, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Channels);
		}));
}

void UCrowdyChannels::GetChannelMembers(int64 ChannelId, FOnChannelMembersSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("ChannelMembers"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannelMember> Members;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("channelMembers"), Members, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Members);
		}));
}

void UCrowdyChannels::GetPendingJoinRequests(int64 ChannelId, FOnChannelMembersSuccess OnSuccess,
                                             FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	// The server has no pending-only query, so this is the full member list filtered to the ones still awaiting a
	// decision.
	Client->RunOp(ChannelsDomain, TEXT("ChannelMembers"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannelMember> Members;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("channelMembers"), Members, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}

			Members.RemoveAll([](const FCrowdyChannelMember& Member) { return Member.Status != TEXT("pending"); });
			OnSuccess.ExecuteIfBound(Members);
		}));
}

void UCrowdyChannels::GetChannelRoles(int64 ChannelId, FOnChannelRolesSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("ChannelRoles"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannelRole> Roles;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("channelRoles"), Roles, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Roles);
		}));
}

void UCrowdyChannels::GetChannelPolicy(FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	Client->RunOp(ChannelsDomain, TEXT("ChannelPolicy"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelPolicy Policy;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("channelPolicy"), Policy, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Policy);
		}));
}

void UCrowdyChannels::CreateChannel(const FString& Name, const FString& Description,
                                    ECrowdyChannelMembershipPolicy MembershipPolicy, bool bMembersCanSend,
                                    FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);
	Input->SetStringField(TEXT("membershipPolicy"), FCrowdyChannel::MembershipPolicyToString(MembershipPolicy));
	Input->SetBoolField(TEXT("membersCanSend"), bMembersCanSend);

	Client->RunOp(ChannelsDomain, TEXT("CreateChannel"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannel Channel;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("createChannel"), Channel, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Channel);
		}));
}

void UCrowdyChannels::UpdateChannel(int64 ChannelId, const FString& Name, const FString& Description,
                                    FOnChannelSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(ChannelId));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);

	Client->RunOp(ChannelsDomain, TEXT("UpdateChannel"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannel Channel;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("updateChannel"), Channel, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Channel);
		}));
}

void UCrowdyChannels::DeleteChannel(int64 ChannelId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("DeleteChannel"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyChannels::JoinChannel(int64 ChannelId, FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("JoinChannel"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelMember Member;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("joinChannel"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyChannels::RequestToJoinChannel(int64 ChannelId, FOnChannelMemberSuccess OnSuccess,
                                           FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("RequestToJoinChannel"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelMember Member;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("requestToJoinChannel"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyChannels::LeaveChannel(int64 ChannelId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));

	Client->RunOp(ChannelsDomain, TEXT("LeaveChannel"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyChannels::AddChannelMember(int64 ChannelId, int64 UserId,
                                       FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));

	Client->RunOp(ChannelsDomain, TEXT("AddChannelMember"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelMember Member;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("addChannelMember"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyChannels::RemoveChannelMember(int64 ChannelId, int64 UserId,
                                          FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(ChannelId));
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));

	Client->RunOp(ChannelsDomain, TEXT("RemoveChannelMember"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyChannels::CreateChannelRole(int64 ChannelId, const FString& RoleName,
                                        FCrowdyChannelPermissions Permissions, int32 Rank,
                                        FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(ChannelId));
	Input->SetStringField(TEXT("roleName"), RoleName);
	Input->SetNumberField(TEXT("rank"), Rank);
	SetPermissionKeys(Input, Permissions.ToStringArray());

	Client->RunOp(ChannelsDomain, TEXT("CreateChannelRole"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelRole Role;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("createChannelRole"), Role, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Role);
		}));
}

void UCrowdyChannels::UpdateChannelRole(int64 ChannelRoleId, const FString& RoleName,
                                        FCrowdyChannelPermissions Permissions,
                                        FOnChannelRoleSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupRoleId"), BigInt(ChannelRoleId));
	Input->SetStringField(TEXT("roleName"), RoleName);
	SetPermissionKeys(Input, Permissions.ToStringArray());

	Client->RunOp(ChannelsDomain, TEXT("UpdateChannelRole"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelRole Role;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("updateChannelRole"), Role, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Role);
		}));
}

void UCrowdyChannels::DeleteChannelRole(int64 ChannelRoleId, FOnChannelVoidSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupRoleId"), BigInt(ChannelRoleId));

	Client->RunOp(ChannelsDomain, TEXT("DeleteChannelRole"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyChannels::SetChannelMemberRoles(int64 ChannelId, int64 UserId, const TArray<int64>& RoleIds,
                                            FOnChannelMemberSuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(ChannelId));
	Input->SetStringField(TEXT("userId"), BigInt(UserId));

	TArray<TSharedPtr<FJsonValue>> RoleIdValues;
	for (int64 RoleId : RoleIds)
	{
		RoleIdValues.Add(MakeShared<FJsonValueString>(BigInt(RoleId)));
	}
	Input->SetArrayField(TEXT("roleIds"), RoleIdValues);

	Client->RunOp(ChannelsDomain, TEXT("SetChannelMemberRoles"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelMember Member;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("setChannelMemberRoles"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyChannels::SetChannelPolicy(ECrowdyChannelCreationPolicy CreationPolicy,
                                       ECrowdyChannelMembershipPolicy DefaultMembershipPolicy,
                                       FOnChannelPolicySuccess OnSuccess, FOnChannelError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		const FCrowdyChannelError Error = ClientUnavailableError<FCrowdyChannelError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("creationPolicy"), FCrowdyChannelPolicy::CreationPolicyToString(CreationPolicy));
	Input->SetStringField(TEXT("defaultMembershipPolicy"),
		FCrowdyChannel::MembershipPolicyToString(DefaultMembershipPolicy));

	Client->RunOp(ChannelsDomain, TEXT("SetChannelPolicy"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannelPolicy Policy;
			FCrowdyChannelError Error;
			if (!ReadObject(Result, TEXT("setChannelPolicy"), Policy, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Policy);
		}));
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
	Request.UUID = FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID());
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
	// The UDP connection is up by the time this is called, so the router now exists even if it did not
	// at Initialize time.
	EnsureChannelSubscription();

	if (bRpcChannelsReady || bBootstrapInFlight)
		return;

	if (GetAppId() <= 0)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Reliable RPC channel bootstrap skipped - AppID is not set."));
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();

	// Which channels do this app's Multicast CrowdyEvents target? We must join them all (a client only
	// receives on channels it has joined). The session channel is joined regardless of what this reports,
	// because the Game Model plane rides it without any CrowdyEvent naming it (see BuildDesiredJoinNames);
	// bUsesDefaultChannel therefore describes only whether reliable RPCs also route over it.
	ReferencedChannelNames.Reset();
	bUsesDefaultChannel = false;
	if (GameInstance)
	{
		if (UCrowdyAutoRegistry* AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>())
			AutoRegistry->CollectMulticastChannels(ReferencedChannelNames, bUsesDefaultChannel);
	}

	// An external kick, so the retry budget starts over: this is a genuinely new attempt, not a continuation of a
	// chain that already gave up.
	BootstrapRetryCount = 0;
	StartBootstrapAttempt();
}

void UCrowdyChannels::StartBootstrapAttempt()
{
	bBootstrapInFlight = true;

	// Drop queued reliable sends if the whole chain hasn't completed in time, so a misconfigured
	// channel policy can't strand them forever.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(
			RpcChannelTimeoutTimer,
			FTimerDelegate::CreateUObject(this, &UCrowdyChannels::AbortRpcChannelBootstrap,
				FString(TEXT("timed out"))),
			SessionChannelBootstrapTimeoutSeconds, /*bLoop*/false);
	}

	UE_LOG(LogCrowdyServices, Log,
		TEXT("[CrowdyChannels] Bootstrapping channels (%d named + the session channel%s)."),
		ReferencedChannelNames.Num(), bUsesDefaultChannel ? TEXT(", which also carries reliable RPCs") : TEXT(""));
	BootstrapFetchAppChannels();
}

void UCrowdyChannels::RetryRpcChannelBootstrap()
{
	// Another path may have succeeded or started while this retry was pending.
	if (bRpcChannelsReady || bBootstrapInFlight || GetAppId() <= 0)
	{
		return;
	}

	// The channel names were collected by the kick that started this chain and do not change between attempts, so
	// the retry re-enters at the request phase rather than re-reading the registry.
	StartBootstrapAttempt();
}

TArray<FString> UCrowdyChannels::BuildDesiredJoinNames(const TSet<FString>& MulticastChannelNames,
	const FString& SessionChannelName)
{
	TArray<FString> Names;
	Names.Reserve(MulticastChannelNames.Num() + 1);
	for (const FString& Name : MulticastChannelNames)
	{
		// A CrowdyEvent naming the session channel explicitly must not queue it twice: the second join would
		// be a redundant round trip, and a duplicate create attempt if the channel does not exist yet.
		if (!Name.IsEmpty() && Name != SessionChannelName)
		{
			Names.Add(Name);
		}
	}
	Names.Sort();

	// An empty name means the app id was never resolved, in which case there is no session channel to name.
	if (!SessionChannelName.IsEmpty())
	{
		Names.Add(SessionChannelName);
	}
	return Names;
}

void UCrowdyChannels::BootstrapFetchAppChannels()
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		AbortRpcChannelBootstrap(TEXT("no API client"));
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
	Client->RunOp(ChannelsDomain, TEXT("Channels"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis](FCrowdyCppJsonResult Result)
		{
			UCrowdyChannels* Self = WeakThis.Get();
			if (!Self)
				return;

			TArray<FCrowdyChannel> Channels;
			FCrowdyChannelError Error;
			if (!ReadArray(Result, TEXT("channels"), Channels, Error))
			{
				Self->AbortRpcChannelBootstrap(TEXT("could not list channels"));
				return;
			}

			Self->AppChannelNameToId.Reset();
			for (const FCrowdyChannel& Channel : Channels)
				Self->AppChannelNameToId.Add(Channel.Name, Channel.ChannelId);

			Self->BootstrapFetchMyChannels();
		}));
}

void UCrowdyChannels::BootstrapFetchMyChannels()
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
	if (!Client)
	{
		AbortRpcChannelBootstrap(TEXT("no API client"));
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
	Client->RunOp(ChannelsDomain, TEXT("MyChannels"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis](FCrowdyCppJsonResult Result)
		{
			UCrowdyChannels* Self = WeakThis.Get();
			if (!Self)
				return;

			Self->MyChannelIds.Reset();

			TArray<FCrowdyChannelMembership> Memberships;
			FCrowdyChannelError Error;
			if (ReadArray(Result, TEXT("myChannels"), Memberships, Error))
			{
				for (const FCrowdyChannelMembership& Membership : Memberships)
					Self->MyChannelIds.Add(Membership.Channel.ChannelId);
			}

			// Proceed even if the membership lookup failed - the joins below just attempt anyway.
			Self->BootstrapPlanJoins();
		}));
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

	const FString SessionName = GetSessionChannelName();
	for (const FString& Name : BuildDesiredJoinNames(ReferencedChannelNames, SessionName))
	{
		if (PlanChannel(Name))
		{
			continue;
		}

		// The session channel is the only one we create ourselves, so a fresh app needs nothing authored in
		// Studio before Game Model signals and re-pull pings arrive. A named channel is a designer's, and
		// inventing one under a guessed name would hide the typo it usually is.
		if (Name == SessionName)
		{
			bNeedCreateSessionChannel = true;
		}
		else
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[CrowdyChannels] Multicast channel '%s' was not found for this app - RPCs targeting it will drop. Create it (or fix the name) in Crowdy Studio."),
				*Name);
		}
	}

	ProcessNextJoin();
}

void UCrowdyChannels::ProcessNextJoin()
{
	// One outstanding request at a time. The server is asked to change membership here, so the joins stay
	// sequential rather than fanning out.
	if (JoinQueue.Num() > 0)
	{
		const TPair<int64, FString> Item = JoinQueue.Pop(EAllowShrinking::No);

		FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
		if (!Client)
		{
			AbortRpcChannelBootstrap(TEXT("no API client"));
			return;
		}

		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("groupId"), BigInt(Item.Key));

		TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
		Client->RunOp(ChannelsDomain, TEXT("JoinChannel"), Variables,
			GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, Item](FCrowdyCppJsonResult Result)
			{
				UCrowdyChannels* Self = WeakThis.Get();
				if (!Self)
					return;

				FCrowdyChannelMember Member;
				FCrowdyChannelError Error;
				if (ReadObject(Result, TEXT("joinChannel"), Member, Error))
				{
					Self->RegisterJoinedChannel(Member.ChannelId, Item.Value);
				}
				else
				{
					UE_LOG(LogCrowdyServices, Warning,
						TEXT("[CrowdyChannels] Could not join channel '%s' - RPCs targeting it will drop (its membership policy must allow it). %s"),
						*Item.Value, *Error.Message);
				}

				Self->ProcessNextJoin();
			}));
		return;
	}

	if (bNeedCreateSessionChannel)
	{
		bNeedCreateSessionChannel = false;

		FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), ChannelsLogName);
		if (!Client)
		{
			AbortRpcChannelBootstrap(TEXT("no API client"));
			return;
		}

		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
		Input->SetStringField(TEXT("name"), GetSessionChannelName());
		Input->SetStringField(TEXT("description"), TEXT("Crowdy reliable RPC session channel"));
		Input->SetStringField(TEXT("membershipPolicy"),
			FCrowdyChannel::MembershipPolicyToString(ECrowdyChannelMembershipPolicy::Open));
		Input->SetBoolField(TEXT("membersCanSend"), true);

		TWeakObjectPtr<UCrowdyChannels> WeakThis(this);
		Client->RunOp(ChannelsDomain, TEXT("CreateChannel"), WrapInput(Input),
			GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis](FCrowdyCppJsonResult Result)
			{
				UCrowdyChannels* Self = WeakThis.Get();
				if (!Self)
					return;

				FCrowdyChannel Channel;
				FCrowdyChannelError Error;
				if (ReadObject(Result, TEXT("createChannel"), Channel, Error))
				{
					Self->RegisterJoinedChannel(Channel.ChannelId, Self->GetSessionChannelName());
				}
				else
				{
					UE_LOG(LogCrowdyServices, Warning,
						TEXT("[CrowdyChannels] Could not create the session channel, so Game Model signals and model-changed pings will not arrive and default-channel RPCs will drop (its creation policy must allow members, or create it once in Crowdy Studio). %s"),
						*Error.Message);
				}

				Self->ProcessNextJoin();
			}));
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
	BootstrapRetryCount = 0;

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(RpcChannelTimeoutTimer);
		GameInstance->GetTimerManager().ClearTimer(RpcChannelRetryTimer);
	}

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

	UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance)
		GameInstance->GetTimerManager().ClearTimer(RpcChannelTimeoutTimer);

	// Queued sends are stale by the time a whole attempt has failed, and holding them across a retry chain would
	// let a misconfigured policy strand them for as long as the chain runs.
	const int32 Dropped = PendingReliableSends.Num();
	PendingReliableSends.Reset();

	if (BootstrapRetryCount < MaxBootstrapRetries && GameInstance)
	{
		++BootstrapRetryCount;
		const float Delay = BootstrapRetryBaseSeconds * BootstrapRetryCount;

		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyChannels] Channel bootstrap failed (%s); dropped %d queued send(s). Retrying in %.0fs (attempt %d of %d)."),
			*Reason, Dropped, Delay, BootstrapRetryCount, MaxBootstrapRetries);

		GameInstance->GetTimerManager().SetTimer(
			RpcChannelRetryTimer,
			FTimerDelegate::CreateUObject(this, &UCrowdyChannels::RetryRpcChannelBootstrap),
			Delay, /*bLoop*/false);
		return;
	}

	// Error, not Warning: nothing else will report that the whole Game Model notification path is dead, and the
	// symptom (signals and re-pull pings simply never arriving) looks like a server problem from the game's side.
	UE_LOG(LogCrowdyServices, Error,
		TEXT("[CrowdyChannels] Channel bootstrap failed (%s) after %d attempt(s); dropped %d queued send(s). This client has joined no channels, so Game Model signals and model-changed pings will not arrive and channel RPCs will drop. A later reconnect retries."),
		*Reason, BootstrapRetryCount + 1, Dropped);
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
			TEXT("[CrowdyChannels] Reliable send dropped - %d already queued waiting for the RPC channels."),
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
			TEXT("[CrowdyChannels] Reliable RPC dropped - no joined channel for '%s'."),
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

bool UCrowdyChannels::IsPlayerInChannel(int64 ChannelId) const
{
	for (const FCrowdyChannelMembership& Membership : CachedMyChannels)
	{
		if (Membership.Channel.ChannelId == ChannelId) return true;
	}
	return false;
}

bool UCrowdyChannels::GetMyChannelById(int64 ChannelId, FCrowdyChannelMembership& OutMembership) const
{
	for (const FCrowdyChannelMembership& Membership : CachedMyChannels)
	{
		if (Membership.Channel.ChannelId == ChannelId)
		{
			OutMembership = Membership;
			return true;
		}
	}
	return false;
}

bool UCrowdyChannels::IsInAnyChannel() const
{
	return CachedMyChannels.Num() > 0;
}

bool UCrowdyChannels::HasPermissionInChannel(int64 ChannelId, ECrowdyChannelPermission Permission) const
{
	FCrowdyChannelMembership Membership;
	if (!GetMyChannelById(ChannelId, Membership)) return false;
	return Membership.HasPermission(Permission);
}
