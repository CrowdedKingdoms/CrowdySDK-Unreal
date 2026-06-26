#include "Subsystem/AsyncActions/CrowdyChannelsQueryActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyChannels.h"

static UCrowdyChannels* GetChannelsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyChannels>() : nullptr;
}


UCrowdyChannels_GetMyChannels* UCrowdyChannels_GetMyChannels::GetMyChannels(UObject* WorldContextObject)
{
	UCrowdyChannels_GetMyChannels* Action = NewObject<UCrowdyChannels_GetMyChannels>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetMyChannels::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnMyChannelsSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetMyChannels::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetMyChannels::HandleError);
	Channels->GetMyChannels(S, E);
}

void UCrowdyChannels_GetMyChannels::HandleSuccess(TArray<FCrowdyGroupMembership> Memberships)
{
	FCrowdyMyChannelsResult Result;
	Result.Memberships = MoveTemp(Memberships);
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetMyChannels::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetChannel* UCrowdyChannels_GetChannel::GetChannel(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_GetChannel* Action = NewObject<UCrowdyChannels_GetChannel>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetChannel::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetChannel::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetChannel::HandleError);
	Channels->GetChannel(GroupId, S, E);
}

void UCrowdyChannels_GetChannel::HandleSuccess(FCrowdyGroup Channel)
{
	OnSuccess.Broadcast(Channel);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetChannel::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetChannels* UCrowdyChannels_GetChannels::GetChannels(UObject* WorldContextObject)
{
	UCrowdyChannels_GetChannels* Action = NewObject<UCrowdyChannels_GetChannels>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetChannels::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelsSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetChannels::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetChannels::HandleError);
	Channels->GetChannels(S, E);
}

void UCrowdyChannels_GetChannels::HandleSuccess(TArray<FCrowdyGroup> ChannelList)
{
	FCrowdyChannelsResult Result;
	Result.Channels = MoveTemp(ChannelList);
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetChannels::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetChannelMembers* UCrowdyChannels_GetChannelMembers::GetChannelMembers(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_GetChannelMembers* Action = NewObject<UCrowdyChannels_GetChannelMembers>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetChannelMembers::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMembersSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetChannelMembers::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetChannelMembers::HandleError);
	Channels->GetChannelMembers(GroupId, S, E);
}

void UCrowdyChannels_GetChannelMembers::HandleSuccess(TArray<FCrowdyGroupMember> Members)
{
	FCrowdyChannelMembersResult Result;
	Result.Members = MoveTemp(Members);
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetChannelMembers::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetChannelRoles* UCrowdyChannels_GetChannelRoles::GetChannelRoles(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_GetChannelRoles* Action = NewObject<UCrowdyChannels_GetChannelRoles>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetChannelRoles::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelRolesSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetChannelRoles::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetChannelRoles::HandleError);
	Channels->GetChannelRoles(GroupId, S, E);
}

void UCrowdyChannels_GetChannelRoles::HandleSuccess(TArray<FCrowdyGroupRole> Roles)
{
	FCrowdyChannelRolesResult Result;
	Result.Roles = MoveTemp(Roles);
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetChannelRoles::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetChannelPolicy* UCrowdyChannels_GetChannelPolicy::GetChannelPolicy(UObject* WorldContextObject)
{
	UCrowdyChannels_GetChannelPolicy* Action = NewObject<UCrowdyChannels_GetChannelPolicy>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetChannelPolicy::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelPolicySuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetChannelPolicy::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetChannelPolicy::HandleError);
	Channels->GetChannelPolicy(S, E);
}

void UCrowdyChannels_GetChannelPolicy::HandleSuccess(FCrowdyAppGroupPolicy Policy)
{
	OnSuccess.Broadcast(Policy);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetChannelPolicy::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyChannels_GetPendingJoinRequests* UCrowdyChannels_GetPendingJoinRequests::GetPendingJoinRequests(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyChannels_GetPendingJoinRequests* Action = NewObject<UCrowdyChannels_GetPendingJoinRequests>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyChannels_GetPendingJoinRequests::Activate()
{
	UCrowdyChannels* Channels = GetChannelsSubsystem(WorldContextObject);
	if (!Channels)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyChannels not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnChannelMembersSuccess S;
	S.BindDynamic(this, &UCrowdyChannels_GetPendingJoinRequests::HandleSuccess);
	FOnChannelError E;
	E.BindDynamic(this, &UCrowdyChannels_GetPendingJoinRequests::HandleError);
	Channels->GetPendingJoinRequests(GroupId, S, E);
}

void UCrowdyChannels_GetPendingJoinRequests::HandleSuccess(TArray<FCrowdyGroupMember> Members)
{
	FCrowdyChannelMembersResult Result;
	Result.Members = MoveTemp(Members);
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyChannels_GetPendingJoinRequests::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
