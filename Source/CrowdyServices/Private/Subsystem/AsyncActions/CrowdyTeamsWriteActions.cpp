#include "Subsystem/AsyncActions/CrowdyTeamsWriteActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyTeams.h"

static UCrowdyTeams* GetTeamsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyTeams>() : nullptr;
}


UCrowdyTeams_CreateTeam* UCrowdyTeams_CreateTeam::CreateTeam(UObject* WorldContextObject, const FString& Name,
                                                             const FString& Description,
                                                             ECrowdyTeamMembershipPolicy MembershipPolicy)
{
	UCrowdyTeams_CreateTeam* Action = NewObject<UCrowdyTeams_CreateTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->Description = Description;
	Action->MembershipPolicy = MembershipPolicy;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_CreateTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_CreateTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_CreateTeam::HandleError);
	Teams->CreateTeam(Name, Description, MembershipPolicy, S, E);
}

void UCrowdyTeams_CreateTeam::HandleSuccess(FCrowdyGroup Group)
{
	OnSuccess.Broadcast(Group);
	SetReadyToDestroy();
}

void UCrowdyTeams_CreateTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_UpdateTeam* UCrowdyTeams_UpdateTeam::UpdateTeam(UObject* WorldContextObject, int64 GroupId,
                                                             const FString& Name, const FString& Description)
{
	UCrowdyTeams_UpdateTeam* Action = NewObject<UCrowdyTeams_UpdateTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->Name = Name;
	Action->Description = Description;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_UpdateTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_UpdateTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_UpdateTeam::HandleError);
	Teams->UpdateTeam(GroupId, Name, Description, S, E);
}

void UCrowdyTeams_UpdateTeam::HandleSuccess(FCrowdyGroup Group)
{
	OnSuccess.Broadcast(Group);
	SetReadyToDestroy();
}

void UCrowdyTeams_UpdateTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_DeleteTeam* UCrowdyTeams_DeleteTeam::DeleteTeam(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyTeams_DeleteTeam* Action = NewObject<UCrowdyTeams_DeleteTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_DeleteTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamVoidSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_DeleteTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_DeleteTeam::HandleError);
	Teams->DeleteTeam(GroupId, S, E);
}

void UCrowdyTeams_DeleteTeam::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyTeams_DeleteTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_JoinTeam* UCrowdyTeams_JoinTeam::JoinTeam(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyTeams_JoinTeam* Action = NewObject<UCrowdyTeams_JoinTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_JoinTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMemberSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_JoinTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_JoinTeam::HandleError);
	Teams->JoinTeam(GroupId, S, E);
}

void UCrowdyTeams_JoinTeam::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyTeams_JoinTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_RequestToJoinTeam* UCrowdyTeams_RequestToJoinTeam::RequestToJoinTeam(
	UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyTeams_RequestToJoinTeam* Action = NewObject<UCrowdyTeams_RequestToJoinTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_RequestToJoinTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMemberSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_RequestToJoinTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_RequestToJoinTeam::HandleError);
	Teams->RequestToJoinTeam(GroupId, S, E);
}

void UCrowdyTeams_RequestToJoinTeam::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyTeams_RequestToJoinTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_LeaveTeam* UCrowdyTeams_LeaveTeam::LeaveTeam(UObject* WorldContextObject, int64 GroupId)
{
	UCrowdyTeams_LeaveTeam* Action = NewObject<UCrowdyTeams_LeaveTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_LeaveTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamVoidSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_LeaveTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_LeaveTeam::HandleError);
	Teams->LeaveTeam(GroupId, S, E);
}

void UCrowdyTeams_LeaveTeam::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyTeams_LeaveTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_AddTeamMember* UCrowdyTeams_AddTeamMember::AddTeamMember(UObject* WorldContextObject, int64 GroupId,
                                                                      int64 UserId)
{
	UCrowdyTeams_AddTeamMember* Action = NewObject<UCrowdyTeams_AddTeamMember>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_AddTeamMember::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMemberSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_AddTeamMember::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_AddTeamMember::HandleError);
	Teams->AddTeamMember(GroupId, UserId, S, E);
}

void UCrowdyTeams_AddTeamMember::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyTeams_AddTeamMember::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_RemoveTeamMember* UCrowdyTeams_RemoveTeamMember::RemoveTeamMember(
	UObject* WorldContextObject, int64 GroupId, int64 UserId)
{
	UCrowdyTeams_RemoveTeamMember* Action = NewObject<UCrowdyTeams_RemoveTeamMember>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_RemoveTeamMember::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamVoidSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_RemoveTeamMember::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_RemoveTeamMember::HandleError);
	Teams->RemoveTeamMember(GroupId, UserId, S, E);
}

void UCrowdyTeams_RemoveTeamMember::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyTeams_RemoveTeamMember::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_SetTeamMemberRoles* UCrowdyTeams_SetTeamMemberRoles::SetTeamMemberRoles(
	UObject* WorldContextObject, int64 GroupId, int64 UserId, const TArray<int64>& RoleIds)
{
	UCrowdyTeams_SetTeamMemberRoles* Action = NewObject<UCrowdyTeams_SetTeamMemberRoles>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->UserId = UserId;
	Action->RoleIds = RoleIds;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_SetTeamMemberRoles::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMemberSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_SetTeamMemberRoles::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_SetTeamMemberRoles::HandleError);
	Teams->SetTeamMemberRoles(GroupId, UserId, RoleIds, S, E);
}

void UCrowdyTeams_SetTeamMemberRoles::HandleSuccess(FCrowdyGroupMember Member)
{
	OnSuccess.Broadcast(Member);
	SetReadyToDestroy();
}

void UCrowdyTeams_SetTeamMemberRoles::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_CreateTeamRole* UCrowdyTeams_CreateTeamRole::CreateTeamRole(
	UObject* WorldContextObject, int64 GroupId, const FString& RoleName, FCrowdyRolePermissions Permissions, int32 Rank)
{
	UCrowdyTeams_CreateTeamRole* Action = NewObject<UCrowdyTeams_CreateTeamRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->GroupId = GroupId;
	Action->RoleName = RoleName;
	Action->Permissions = Permissions;
	Action->Rank = Rank;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_CreateTeamRole::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamRoleSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_CreateTeamRole::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_CreateTeamRole::HandleError);
	Teams->CreateTeamRole(GroupId, RoleName, Permissions, Rank, S, E);
}

void UCrowdyTeams_CreateTeamRole::HandleSuccess(FCrowdyGroupRole Role)
{
	OnSuccess.Broadcast(Role);
	SetReadyToDestroy();
}

void UCrowdyTeams_CreateTeamRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_UpdateTeamRole* UCrowdyTeams_UpdateTeamRole::UpdateTeamRole(
	UObject* WorldContextObject, int64 RoleId, const FString& RoleName, FCrowdyRolePermissions Permissions)
{
	UCrowdyTeams_UpdateTeamRole* Action = NewObject<UCrowdyTeams_UpdateTeamRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->RoleId = RoleId;
	Action->RoleName = RoleName;
	Action->Permissions = Permissions;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_UpdateTeamRole::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamRoleSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_UpdateTeamRole::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_UpdateTeamRole::HandleError);
	Teams->UpdateTeamRole(RoleId, RoleName, Permissions, S, E);
}

void UCrowdyTeams_UpdateTeamRole::HandleSuccess(FCrowdyGroupRole Role)
{
	OnSuccess.Broadcast(Role);
	SetReadyToDestroy();
}

void UCrowdyTeams_UpdateTeamRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_DeleteTeamRole* UCrowdyTeams_DeleteTeamRole::DeleteTeamRole(UObject* WorldContextObject, int64 RoleId)
{
	UCrowdyTeams_DeleteTeamRole* Action = NewObject<UCrowdyTeams_DeleteTeamRole>();
	Action->WorldContextObject = WorldContextObject;
	Action->RoleId = RoleId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_DeleteTeamRole::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamVoidSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_DeleteTeamRole::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_DeleteTeamRole::HandleError);
	Teams->DeleteTeamRole(RoleId, S, E);
}

void UCrowdyTeams_DeleteTeamRole::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyTeams_DeleteTeamRole::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_SetTeamPolicy* UCrowdyTeams_SetTeamPolicy::SetTeamPolicy(UObject* WorldContextObject,
                                                                      ECrowdyTeamCreationPolicy CreationPolicy,
                                                                      ECrowdyTeamMembershipPolicy
                                                                      DefaultMembershipPolicy)
{
	UCrowdyTeams_SetTeamPolicy* Action = NewObject<UCrowdyTeams_SetTeamPolicy>();
	Action->WorldContextObject = WorldContextObject;
	Action->CreationPolicy = CreationPolicy;
	Action->MembershipPolicy = DefaultMembershipPolicy;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_SetTeamPolicy::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamPolicySuccess S;
	S.BindDynamic(this, &UCrowdyTeams_SetTeamPolicy::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_SetTeamPolicy::HandleError);
	Teams->SetTeamPolicy(CreationPolicy, MembershipPolicy, S, E);
}

void UCrowdyTeams_SetTeamPolicy::HandleSuccess(FCrowdyAppGroupPolicy Policy)
{
	OnSuccess.Broadcast(Policy);
	SetReadyToDestroy();
}

void UCrowdyTeams_SetTeamPolicy::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
