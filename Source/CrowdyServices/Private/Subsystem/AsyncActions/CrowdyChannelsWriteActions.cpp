#include "Subsystem/AsyncActions/CrowdyChannelsWriteActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyChannels.h"

static UCrowdyChannels* GetChannelsSubsystemW(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyChannels>() : nullptr;
}


UCrowdyChannels_CreateChannel* UCrowdyChannels_CreateChannel::CreateChannel(
	UObject* WorldContextObject, const FString& Name, const FString& Description,
	ECrowdyTeamMembershipPolicy MembershipPolicy, bool bMembersCanSend)
{
	UCrowdyChannels_CreateChannel* Action = NewObject<UCrowdyChannels_CreateChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->Description = Description;
	Action->MembershipPolicy = MembershipPolicy;
	Action->bMembersCanSend = bMembersCanSend;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_CreateChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_CreateChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_CreateChannel::HandleError);
	Channels->CreateChannel(Name, Description, MembershipPolicy, bMembersCanSend, S, E);
}

void UCrowdyChannels_CreateChannel::HandleSuccess(FCrowdyGroup Channel)
{
	OnSuccess.Broadcast(Channel);
	SetReadyToDestroy();
}

void UCrowdyChannels_CreateChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_UpdateChannel* UCrowdyChannels_UpdateChannel::UpdateChannel(
	UObject* WorldContextObject, int64 GroupId, const FString& Name, const FString& Description)
{
	UCrowdyChannels_UpdateChannel* Action = NewObject<UCrowdyChannels_UpdateChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->Name = Name;
	Action->Description = Description;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_UpdateChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_UpdateChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_UpdateChannel::HandleError);
	Channels->UpdateChannel(GroupId, Name, Description, S, E);
}

void UCrowdyChannels_UpdateChannel::HandleSuccess(FCrowdyGroup Channel)
{
	OnSuccess.Broadcast(Channel);
	SetReadyToDestroy();
}

void UCrowdyChannels_UpdateChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_DeleteChannel* UCrowdyChannels_DeleteChannel::DeleteChannel(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_DeleteChannel* Action = NewObject<UCrowdyChannels_DeleteChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_DeleteChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelVoidSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_DeleteChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_DeleteChannel::HandleError);
	Channels->DeleteChannel(GroupId, S, E);
}

void UCrowdyChannels_DeleteChannel::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyChannels_DeleteChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_JoinChannel* UCrowdyChannels_JoinChannel::JoinChannel(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_JoinChannel* Action = NewObject<UCrowdyChannels_JoinChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_JoinChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMemberSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_JoinChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_JoinChannel::HandleError);
	Channels->JoinChannel(GroupId, S, E);
}

void UCrowdyChannels_JoinChannel::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyChannels_JoinChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_RequestToJoinChannel* UCrowdyChannels_RequestToJoinChannel::RequestToJoinChannel(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_RequestToJoinChannel* Action = NewObject<UCrowdyChannels_RequestToJoinChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_RequestToJoinChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMemberSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_RequestToJoinChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_RequestToJoinChannel::HandleError);
	Channels->RequestToJoinChannel(GroupId, S, E);
}

void UCrowdyChannels_RequestToJoinChannel::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyChannels_RequestToJoinChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_LeaveChannel* UCrowdyChannels_LeaveChannel::LeaveChannel(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_LeaveChannel* Action = NewObject<UCrowdyChannels_LeaveChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_LeaveChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelVoidSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_LeaveChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_LeaveChannel::HandleError);
	Channels->LeaveChannel(GroupId, S, E);
}

void UCrowdyChannels_LeaveChannel::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyChannels_LeaveChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_AddChannelMember* UCrowdyChannels_AddChannelMember::AddChannelMember(
	UObject* WorldContextObject, int64 GroupId, int64 UserId)
{
	UCrowdyChannels_AddChannelMember* Action = NewObject<UCrowdyChannels_AddChannelMember>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_AddChannelMember::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMemberSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_AddChannelMember::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_AddChannelMember::HandleError);
	Channels->AddChannelMember(GroupId, UserId, S, E);
}

void UCrowdyChannels_AddChannelMember::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyChannels_AddChannelMember::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_RemoveChannelMember* UCrowdyChannels_RemoveChannelMember::RemoveChannelMember(
	UObject* WorldContextObject, int64 GroupId, int64 UserId)
{
	UCrowdyChannels_RemoveChannelMember* Action = NewObject<UCrowdyChannels_RemoveChannelMember>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_RemoveChannelMember::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelVoidSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_RemoveChannelMember::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_RemoveChannelMember::HandleError);
	Channels->RemoveChannelMember(GroupId, UserId, S, E);
}

void UCrowdyChannels_RemoveChannelMember::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyChannels_RemoveChannelMember::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_SetChannelMemberRoles* UCrowdyChannels_SetChannelMemberRoles::SetChannelMemberRoles(
	UObject* WorldContextObject, int64 GroupId, int64 UserId, const TArray<int64>& RoleIds)
{
	UCrowdyChannels_SetChannelMemberRoles* Action = NewObject<UCrowdyChannels_SetChannelMemberRoles>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RoleIds = RoleIds;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_SetChannelMemberRoles::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMemberSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_SetChannelMemberRoles::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_SetChannelMemberRoles::HandleError);
	Channels->SetChannelMemberRoles(GroupId, UserId, RoleIds, S, E);
}

void UCrowdyChannels_SetChannelMemberRoles::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyChannels_SetChannelMemberRoles::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_CreateChannelRole* UCrowdyChannels_CreateChannelRole::CreateChannelRole(
	UObject* WorldContextObject, int64 GroupId, const FString& RoleName,
	FCrowdyRolePermissions Permissions, int32 Rank)
{
	UCrowdyChannels_CreateChannelRole* Action = NewObject<UCrowdyChannels_CreateChannelRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RoleName = RoleName;
	Action->Permissions = Permissions;
	Action->Rank = Rank;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_CreateChannelRole::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelRoleSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_CreateChannelRole::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_CreateChannelRole::HandleError);
	Channels->CreateChannelRole(GroupId, RoleName, Permissions, Rank, S, E);
}

void UCrowdyChannels_CreateChannelRole::HandleSuccess(FCrowdyGroupRole Role)
{
	OnSuccess.Broadcast(Role);
	SetReadyToDestroy();
}

void UCrowdyChannels_CreateChannelRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_UpdateChannelRole* UCrowdyChannels_UpdateChannelRole::UpdateChannelRole(
	UObject* WorldContextObject, int64 RoleId, const FString& RoleName, FCrowdyRolePermissions Permissions)
{
	UCrowdyChannels_UpdateChannelRole* Action = NewObject<UCrowdyChannels_UpdateChannelRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->RoleId = RoleId;
	Action->RoleName = RoleName;
	Action->Permissions = Permissions;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_UpdateChannelRole::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelRoleSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_UpdateChannelRole::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_UpdateChannelRole::HandleError);
	Channels->UpdateChannelRole(RoleId, RoleName, Permissions, S, E);
}

void UCrowdyChannels_UpdateChannelRole::HandleSuccess(FCrowdyGroupRole Role)
{
	OnSuccess.Broadcast(Role);
	SetReadyToDestroy();
}

void UCrowdyChannels_UpdateChannelRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_DeleteChannelRole* UCrowdyChannels_DeleteChannelRole::DeleteChannelRole(
	UObject* WorldContextObject, int64 RoleId)
{
	UCrowdyChannels_DeleteChannelRole* Action = NewObject<UCrowdyChannels_DeleteChannelRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->RoleId = RoleId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_DeleteChannelRole::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelVoidSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_DeleteChannelRole::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_DeleteChannelRole::HandleError);
	Channels->DeleteChannelRole(RoleId, S, E);
}

void UCrowdyChannels_DeleteChannelRole::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyChannels_DeleteChannelRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_SetChannelPolicy* UCrowdyChannels_SetChannelPolicy::SetChannelPolicy(
	UObject* WorldContextObject, ECrowdyTeamCreationPolicy CreationPolicy,
	ECrowdyTeamMembershipPolicy DefaultMembershipPolicy)
{
	UCrowdyChannels_SetChannelPolicy* Action = NewObject<UCrowdyChannels_SetChannelPolicy>();
	Action->WorldContextObject = WorldContextObject;
	Action->CreationPolicy = CreationPolicy;
	Action->MembershipPolicy = DefaultMembershipPolicy;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_SetChannelPolicy::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystemW(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelPolicySuccess S;
	S.BindDynamic(this, &UCrowdyChannels_SetChannelPolicy::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_SetChannelPolicy::HandleError);
	Channels->SetChannelPolicy(CreationPolicy, MembershipPolicy, S, E);
}

void UCrowdyChannels_SetChannelPolicy::HandleSuccess(FCrowdyAppGroupPolicy Policy)
{
	OnSuccess.Broadcast(Policy);
	SetReadyToDestroy();
}

void UCrowdyChannels_SetChannelPolicy::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
