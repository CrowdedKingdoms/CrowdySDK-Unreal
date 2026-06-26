#include "Subsystem/CrowdyTeams.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Async/Async.h"
#include "Queries/Data/Teams/Responses/FMyTeamsResponse.h"
#include "Queries/Data/Teams/Responses/FTeamResponse.h"
#include "Queries/Data/Teams/Responses/FTeamsResponse.h"
#include "Queries/Data/Teams/Responses/FTeamMembersResponse.h"
#include "Queries/Data/Teams/Responses/FTeamRolesResponse.h"
#include "Queries/Data/Teams/Responses/FTeamPolicyResponse.h"
#include "Queries/Data/Teams/Responses/FCreateTeamResponse.h"
#include "Queries/Data/Teams/Responses/FJoinTeamResponse.h"
#include "Queries/Data/Teams/Responses/FRequestToJoinTeamResponse.h"
#include "Queries/Data/Teams/Responses/FLeaveTeamResponse.h"
#include "Queries/Data/Teams/Responses/FAddTeamMemberResponse.h"
#include "Queries/Data/Teams/Responses/FRemoveTeamMemberResponse.h"
#include "Queries/Data/Teams/Responses/FCreateTeamRoleResponse.h"
#include "Queries/Data/Teams/Responses/FSetTeamMemberRolesResponse.h"
#include "Queries/Data/Teams/Responses/FUpdateTeamRoleResponse.h"
#include "Queries/Data/Teams/Responses/FDeleteTeamRoleResponse.h"
#include "Queries/Data/Teams/Responses/FUpdateTeamResponse.h"
#include "Queries/Data/Teams/Responses/FDeleteTeamResponse.h"
#include "Queries/Data/Teams/Responses/FSetTeamPolicyResponse.h"

namespace TeamQueries
{
	static const TCHAR* MyTeams =
		TEXT(
			"query MyTeams($appId: BigInt!) { myTeams(appId: $appId) { group { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } roles { groupRoleId groupId roleName rank isSystem permissions createdAt } permissions joinedAt } }");

	static const TCHAR* Team =
		TEXT(
			"query Team($groupId: BigInt!) { team(groupId: $groupId) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* Teams =
		TEXT(
			"query Teams($appId: BigInt!) { teams(appId: $appId) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* TeamMembers =
		TEXT(
			"query TeamMembers($groupId: BigInt!) { teamMembers(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* TeamRoles =
		TEXT(
			"query TeamRoles($groupId: BigInt!) { teamRoles(groupId: $groupId) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* TeamPolicy =
		TEXT(
			"query TeamPolicy($appId: BigInt!) { teamPolicy(appId: $appId) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser } }");

	static const TCHAR* CreateTeam =
		TEXT(
			"mutation CreateTeam($appId: BigInt!, $name: String!, $description: String, $membershipPolicy: String) { createTeam(input: { appId: $appId, name: $name, description: $description, membershipPolicy: $membershipPolicy }) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* UpdateTeam =
		TEXT(
			"mutation UpdateTeam($groupId: BigInt!, $name: String, $description: String) { updateTeam(input: { groupId: $groupId, name: $name, description: $description }) { groupId appId groupType name description ownerUserId membershipPolicy status createdAt } }");

	static const TCHAR* DeleteTeam =
		TEXT("mutation DeleteTeam($groupId: BigInt!) { deleteTeam(groupId: $groupId) }");

	static const TCHAR* JoinTeam =
		TEXT(
			"mutation JoinTeam($groupId: BigInt!) { joinTeam(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* RequestToJoinTeam =
		TEXT(
			"mutation RequestToJoinTeam($groupId: BigInt!) { requestToJoinTeam(groupId: $groupId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* LeaveTeam =
		TEXT("mutation LeaveTeam($groupId: BigInt!) { leaveTeam(groupId: $groupId) }");

	static const TCHAR* AddTeamMember =
		TEXT(
			"mutation AddTeamMember($groupId: BigInt!, $userId: BigInt!) { addTeamMember(groupId: $groupId, userId: $userId) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* RemoveTeamMember =
		TEXT(
			"mutation RemoveTeamMember($groupId: BigInt!, $userId: BigInt!) { removeTeamMember(groupId: $groupId, userId: $userId) }");

	static const TCHAR* CreateTeamRole =
		TEXT(
			"mutation CreateTeamRole($groupId: BigInt!, $roleName: String!, $permissions: [String!], $rank: Int) { createTeamRole(input: { groupId: $groupId, roleName: $roleName, permissions: $permissions, rank: $rank }) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* UpdateTeamRole =
		TEXT(
			"mutation UpdateTeamRole($roleId: BigInt!, $roleName: String, $permissions: [String!]) { updateTeamRole(input: { roleId: $roleId, roleName: $roleName, permissions: $permissions }) { groupRoleId groupId roleName rank isSystem permissions createdAt } }");

	static const TCHAR* DeleteTeamRole =
		TEXT("mutation DeleteTeamRole($roleId: BigInt!) { deleteTeamRole(roleId: $roleId) }");

	static const TCHAR* SetTeamMemberRoles =
		TEXT(
			"mutation SetTeamMemberRoles($groupId: BigInt!, $userId: BigInt!, $roleIds: [BigInt!]!) { setTeamMemberRoles(input: { groupId: $groupId, userId: $userId, roleIds: $roleIds }) { groupMemberId groupId userId status createdAt roles { groupRoleId groupId roleName rank isSystem permissions createdAt } } }");

	static const TCHAR* SetTeamPolicy =
		TEXT(
			"mutation SetTeamPolicy($appId: BigInt!, $creationPolicy: String!, $defaultMembershipPolicy: String!) { setTeamPolicy(input: { appId: $appId, creationPolicy: $creationPolicy, defaultMembershipPolicy: $defaultMembershipPolicy }) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser } }");
}

void UCrowdyTeams::InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem)
{
	if (InDataRegistry) InDataRegistry->RegisterLayer(this);
	QuerySubsystem = InQuerySubsystem;
}

void UCrowdyTeams::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyTeams::Deinitialize()
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.Empty();
	Super::Deinitialize();
}

TArray<EQueryResponseType> UCrowdyTeams::GetSupportedResponseType() const
{
	return {
		EQueryResponseType::MyTeams,
		EQueryResponseType::Team,
		EQueryResponseType::Teams,
		EQueryResponseType::TeamMembers,
		EQueryResponseType::TeamRoles,
		EQueryResponseType::TeamPolicy,
		EQueryResponseType::CreateTeam,
		EQueryResponseType::JoinTeam,
		EQueryResponseType::RequestToJoinTeam,
		EQueryResponseType::LeaveTeam,
		EQueryResponseType::AddTeamMember,
		EQueryResponseType::RemoveTeamMember,
		EQueryResponseType::CreateTeamRole,
		EQueryResponseType::SetTeamMemberRoles,
		EQueryResponseType::UpdateTeamRole,
		EQueryResponseType::DeleteTeamRole,
		EQueryResponseType::UpdateTeam,
		EQueryResponseType::DeleteTeam,
		EQueryResponseType::SetTeamPolicy,
	};
}

void UCrowdyTeams::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response.IsValid()) return;
	FireCallback(Response);
}

int64 UCrowdyTeams::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

void UCrowdyTeams::PushCallback(EQueryResponseType Type,
                                TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback)
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.FindOrAdd(Type).Add(MoveTemp(Callback));
}

void UCrowdyTeams::FireCallback(TSharedPtr<ICrowdyQueryResponse> Response)
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

TSharedPtr<FJsonObject> UCrowdyTeams::MakeVarsWithStringArray(
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

void UCrowdyTeams::GetMyTeams(FOnMyTeamsSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::MyTeams, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FMyTeamsResponse& R = static_cast<FMyTeamsResponse&>(*Resp);
			CachedMyTeams = R.Memberships;
			bCachePopulated = true;
			OnMyTeamsCacheChanged.Broadcast(CachedMyTeams);
			OnSuccess.ExecuteIfBound(R.Memberships);
		}
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::MyTeams, TeamQueries::MyTeams, Vars);
}

void UCrowdyTeams::GetTeam(int64 GroupId, FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Team, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FTeamResponse&>(*Resp).Group);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::Team, TeamQueries::Team, Vars);
}

void UCrowdyTeams::GetTeams(FOnTeamsSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Teams, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FTeamsResponse&>(*Resp).Groups);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::Teams, TeamQueries::Teams, Vars);
}

void UCrowdyTeams::GetTeamMembers(int64 GroupId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::TeamMembers, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FTeamMembersResponse&>(*Resp).Members);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::TeamMembers, TeamQueries::TeamMembers, Vars);
}

void UCrowdyTeams::GetTeamRoles(int64 GroupId, FOnTeamRolesSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::TeamRoles, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FTeamRolesResponse&>(*Resp).Roles);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::TeamRoles, TeamQueries::TeamRoles, Vars);
}

void UCrowdyTeams::GetTeamPolicy(FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::TeamPolicy, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FTeamPolicyResponse&>(*Resp).Policy);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::TeamPolicy, TeamQueries::TeamPolicy, Vars);
}

void UCrowdyTeams::CreateTeam(const FString& Name, const FString& Description,
                              ECrowdyTeamMembershipPolicy MembershipPolicy,
                              FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::CreateTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FCreateTeamResponse&>(*Resp).Group);
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateTeam, TeamQueries::CreateTeam, Vars);
}

void UCrowdyTeams::UpdateTeam(int64 GroupId, const FString& Name, const FString& Description,
                              FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUpdateTeamResponse&>(*Resp).Group);
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateTeam, TeamQueries::UpdateTeam, Vars);
}

void UCrowdyTeams::DeleteTeam(int64 GroupId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::DeleteTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::DeleteTeam, TeamQueries::DeleteTeam, Vars);
}

void UCrowdyTeams::JoinTeam(int64 GroupId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::JoinTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FJoinTeamResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::JoinTeam, TeamQueries::JoinTeam, Vars);
}

void UCrowdyTeams::RequestToJoinTeam(int64 GroupId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::RequestToJoinTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FRequestToJoinTeamResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::RequestToJoinTeam, TeamQueries::RequestToJoinTeam,
	                                                Vars);
}

void UCrowdyTeams::LeaveTeam(int64 GroupId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::LeaveTeam, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::LeaveTeam, TeamQueries::LeaveTeam, Vars);
}

void UCrowdyTeams::AddTeamMember(int64 GroupId, int64 UserId,
                                 FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::AddTeamMember, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAddTeamMemberResponse&>(*Resp).Member);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("groupId"), FString::Printf(TEXT("%lld"), GroupId));
	Vars->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::AddTeamMember, TeamQueries::AddTeamMember, Vars);
}

void UCrowdyTeams::RemoveTeamMember(int64 GroupId, int64 UserId,
                                    FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::RemoveTeamMember, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::RemoveTeamMember, TeamQueries::RemoveTeamMember,
	                                                Vars);
}

void UCrowdyTeams::CreateTeamRole(int64 GroupId, const FString& RoleName,
                                  FCrowdyRolePermissions Permissions, int32 Rank,
                                  FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::CreateTeamRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FCreateTeamRoleResponse&>(*Resp).Role);
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateTeamRole, TeamQueries::CreateTeamRole, Vars);
}

void UCrowdyTeams::UpdateTeamRole(int64 RoleId, const FString& RoleName,
                                  FCrowdyRolePermissions Permissions,
                                  FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateTeamRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUpdateTeamRoleResponse&>(*Resp).Role);
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeVarsWithStringArray(
		{{TEXT("roleId"), FString::Printf(TEXT("%lld"), RoleId)}, {TEXT("roleName"), RoleName}},
		{{TEXT("permissions"), Permissions.ToStringArray()}}
	);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateTeamRole, TeamQueries::UpdateTeamRole, Vars);
}

void UCrowdyTeams::DeleteTeamRole(int64 RoleId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::DeleteTeamRole, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid()) OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyTeamError Err = FCrowdyTeamError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("roleId"), FString::Printf(TEXT("%lld"), RoleId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::DeleteTeamRole, TeamQueries::DeleteTeamRole, Vars);
}

void UCrowdyTeams::SetTeamMemberRoles(int64 GroupId, int64 UserId, const TArray<int64>& RoleIds,
                                      FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::SetTeamMemberRoles, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FSetTeamMemberRolesResponse&>(*Resp).Member);
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

	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::SetTeamMemberRoles, TeamQueries::SetTeamMemberRoles,
	                                                Vars);
}

void UCrowdyTeams::SetTeamPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
                                 ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
                                 FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::SetTeamPolicy, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FSetTeamPolicyResponse&>(*Resp).Policy);
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::SetTeamPolicy, TeamQueries::SetTeamPolicy, Vars);
}

bool UCrowdyTeams::IsPlayerInTeam(int64 GroupId) const
{
	for (const FCrowdyGroupMembership& M : CachedMyTeams)
		if (M.Group.GroupId == GroupId) return true;
	return false;
}

bool UCrowdyTeams::GetMyTeamById(int64 GroupId, FCrowdyGroupMembership& OutMembership) const
{
	for (const FCrowdyGroupMembership& M : CachedMyTeams)
	{
		if (M.Group.GroupId == GroupId)
		{
			OutMembership = M;
			return true;
		}
	}
	return false;
}

bool UCrowdyTeams::IsInAnyTeam() const
{
	return CachedMyTeams.Num() > 0;
}

bool UCrowdyTeams::GetPrimaryMembership(FCrowdyGroupMembership& OutMembership) const
{
	if (CachedMyTeams.IsEmpty()) return false;
	OutMembership = CachedMyTeams[0];
	return true;
}

bool UCrowdyTeams::HasPermissionInTeam(int64 GroupId, ECrowdyTeamPermission Permission) const
{
	FCrowdyGroupMembership Membership;
	if (!GetMyTeamById(GroupId, Membership)) return false;
	return Membership.HasPermission(Permission);
}

void UCrowdyTeams::GetPendingJoinRequests(int64 GroupId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::TeamMembers, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			TArray<FCrowdyGroupMember> Members = static_cast<FTeamMembersResponse&>(*Resp).Members;
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
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::TeamMembers, TeamQueries::TeamMembers, Vars);
}
