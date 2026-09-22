#include "Subsystem/AsyncActions/CrowdyTeamsQueryActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/AsyncActions/CrowdyServicesActionSupport.h"
#include "Subsystem/CrowdyTeams.h"

UCrowdyTeams_GetMyTeams* UCrowdyTeams_GetMyTeams::GetMyTeams(UObject* WorldContextObject)
{
	UCrowdyTeams_GetMyTeams* Action = NewObject<UCrowdyTeams_GetMyTeams>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetMyTeams::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnMyTeamsSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetMyTeams::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetMyTeams::HandleError);
	Teams->GetMyTeams(S, E);
}

void UCrowdyTeams_GetMyTeams::HandleSuccess(const TArray<FCrowdyTeamMembership>& Memberships)
{
	FCrowdyMyTeamsResult Result;
	Result.Memberships = Memberships;
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetMyTeams::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyTeams_GetTeam* UCrowdyTeams_GetTeam::GetTeam(UObject* WorldContextObject, int64 TeamId)
{
	UCrowdyTeams_GetTeam* Action = NewObject<UCrowdyTeams_GetTeam>();
	Action->WorldContextObject = WorldContextObject;
	Action->TeamId = TeamId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetTeam::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetTeam::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetTeam::HandleError);
	Teams->GetTeam(TeamId, S, E);
}

void UCrowdyTeams_GetTeam::HandleSuccess(FCrowdyTeam Team)
{
	OnSuccess.Broadcast(Team);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetTeam::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_GetTeams* UCrowdyTeams_GetTeams::GetTeams(UObject* WorldContextObject)
{
	UCrowdyTeams_GetTeams* Action = NewObject<UCrowdyTeams_GetTeams>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetTeams::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamsSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetTeams::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetTeams::HandleError);
	Teams->GetTeams(S, E);
}

void UCrowdyTeams_GetTeams::HandleSuccess(const TArray<FCrowdyTeam>& Teams)
{
	FCrowdyTeamsResult Result;
	Result.Teams = Teams;
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetTeams::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_GetTeamMembers* UCrowdyTeams_GetTeamMembers::GetTeamMembers(UObject* WorldContextObject, int64 TeamId)
{
	UCrowdyTeams_GetTeamMembers* Action = NewObject<UCrowdyTeams_GetTeamMembers>();
	Action->WorldContextObject = WorldContextObject;
	Action->TeamId = TeamId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetTeamMembers::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMembersSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetTeamMembers::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetTeamMembers::HandleError);
	Teams->GetTeamMembers(TeamId, S, E);
}

void UCrowdyTeams_GetTeamMembers::HandleSuccess(const TArray<FCrowdyTeamMember>& Members)
{
	FCrowdyTeamMembersResult Result;
	Result.Members = Members;
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetTeamMembers::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_GetTeamRoles* UCrowdyTeams_GetTeamRoles::GetTeamRoles(UObject* WorldContextObject, int64 TeamId)
{
	UCrowdyTeams_GetTeamRoles* Action = NewObject<UCrowdyTeams_GetTeamRoles>();
	Action->WorldContextObject = WorldContextObject;
	Action->TeamId = TeamId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetTeamRoles::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamRolesSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetTeamRoles::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetTeamRoles::HandleError);
	Teams->GetTeamRoles(TeamId, S, E);
}

void UCrowdyTeams_GetTeamRoles::HandleSuccess(const TArray<FCrowdyTeamRole>& Roles)
{
	FCrowdyTeamRolesResult Result;
	Result.Roles = Roles;
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetTeamRoles::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_GetTeamPolicy* UCrowdyTeams_GetTeamPolicy::GetTeamPolicy(UObject* WorldContextObject)
{
	UCrowdyTeams_GetTeamPolicy* Action = NewObject<UCrowdyTeams_GetTeamPolicy>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetTeamPolicy::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamPolicySuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetTeamPolicy::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetTeamPolicy::HandleError);
	Teams->GetTeamPolicy(S, E);
}

void UCrowdyTeams_GetTeamPolicy::HandleSuccess(FCrowdyTeamPolicy Policy)
{
	OnSuccess.Broadcast(Policy);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetTeamPolicy::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}


UCrowdyTeams_GetPendingJoinRequests* UCrowdyTeams_GetPendingJoinRequests::GetPendingJoinRequests(
	UObject* WorldContextObject, int64 TeamId)
{
	UCrowdyTeams_GetPendingJoinRequests* Action = NewObject<UCrowdyTeams_GetPendingJoinRequests>();
	Action->WorldContextObject = WorldContextObject;
	Action->TeamId = TeamId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyTeams_GetPendingJoinRequests::Activate()
{
	UCrowdyTeams* Teams = GetTeamsSubsystem(WorldContextObject);
	if (!Teams)
	{
		const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(TEXT("UCrowdyTeams not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnTeamMembersSuccess S;
	S.BindDynamic(this, &UCrowdyTeams_GetPendingJoinRequests::HandleSuccess);
	FOnTeamError E;
	E.BindDynamic(this, &UCrowdyTeams_GetPendingJoinRequests::HandleError);
	Teams->GetPendingJoinRequests(TeamId, S, E);
}

void UCrowdyTeams_GetPendingJoinRequests::HandleSuccess(const TArray<FCrowdyTeamMember>& Members)
{
	FCrowdyTeamMembersResult Result;
	Result.Members = Members;
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyTeams_GetPendingJoinRequests::HandleError(FCrowdyTeamError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
