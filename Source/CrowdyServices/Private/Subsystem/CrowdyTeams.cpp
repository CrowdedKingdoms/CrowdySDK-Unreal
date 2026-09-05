#include "Subsystem/CrowdyTeams.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

using namespace CrowdyServiceApi;

namespace
{
	// The generated operation set these calls are looked up in, which is also what decides the endpoint each one
	// reaches and the bearer it carries.
	constexpr ECrowdyCppApiDomain TeamsDomain = ECrowdyCppApiDomain::Teams;

	constexpr const TCHAR* TeamsLogName = TEXT("CrowdyTeams");
}

void UCrowdyTeams::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyTeams::Deinitialize()
{
	// Released first: a completion can still arrive from the client's own pump after this point, and it must not
	// broadcast a cache change or run a Blueprint delegate while the game instance is shutting down.
	LiveSessionToken.Reset();

	Super::Deinitialize();
}

int64 UCrowdyTeams::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

void UCrowdyTeams::GetMyTeams(FOnMyTeamsSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	TWeakObjectPtr<UCrowdyTeams> WeakThis(this);
	Client->RunOp(TeamsDomain, TEXT("MyTeams"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeamMembership> Memberships;
			FCrowdyTeamError Error;
			if (!ReadArray(Result, TEXT("myTeams"), Memberships, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}

			if (UCrowdyTeams* Self = WeakThis.Get())
			{
				Self->CachedMyTeams = Memberships;
				Self->bCachePopulated = true;
				Self->OnMyTeamsCacheChanged.Broadcast(Self->CachedMyTeams);
			}

			OnSuccess.ExecuteIfBound(Memberships);
		}));
}

void UCrowdyTeams::GetTeam(int64 TeamId, FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("Team"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeam Team;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("team"), Team, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Team);
		}));
}

void UCrowdyTeams::GetTeams(FOnTeamsSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	Client->RunOp(TeamsDomain, TEXT("Teams"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeam> Teams;
			FCrowdyTeamError Error;
			if (!ReadArray(Result, TEXT("teams"), Teams, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Teams);
		}));
}

void UCrowdyTeams::GetTeamMembers(int64 TeamId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("TeamMembers"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeamMember> Members;
			FCrowdyTeamError Error;
			if (!ReadArray(Result, TEXT("teamMembers"), Members, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Members);
		}));
}

void UCrowdyTeams::GetPendingJoinRequests(int64 TeamId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	// The server has no pending-only query, so this is the full member list filtered to the ones still awaiting a
	// decision.
	Client->RunOp(TeamsDomain, TEXT("TeamMembers"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeamMember> Members;
			FCrowdyTeamError Error;
			if (!ReadArray(Result, TEXT("teamMembers"), Members, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}

			Members.RemoveAll([](const FCrowdyTeamMember& Member) { return Member.Status != TEXT("pending"); });
			OnSuccess.ExecuteIfBound(Members);
		}));
}

void UCrowdyTeams::GetTeamRoles(int64 TeamId, FOnTeamRolesSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("TeamRoles"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeamRole> Roles;
			FCrowdyTeamError Error;
			if (!ReadArray(Result, TEXT("teamRoles"), Roles, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Roles);
		}));
}

void UCrowdyTeams::GetTeamPolicy(FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	Client->RunOp(TeamsDomain, TEXT("TeamPolicy"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamPolicy Policy;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("teamPolicy"), Policy, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Policy);
		}));
}

void UCrowdyTeams::CreateTeam(const FString& Name, const FString& Description,
                              ECrowdyTeamMembershipPolicy MembershipPolicy,
                              FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);
	Input->SetStringField(TEXT("membershipPolicy"), FCrowdyTeam::MembershipPolicyToString(MembershipPolicy));

	Client->RunOp(TeamsDomain, TEXT("CreateTeam"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeam Team;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("createTeam"), Team, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Team);
		}));
}

void UCrowdyTeams::UpdateTeam(int64 TeamId, const FString& Name, const FString& Description,
                              FOnTeamSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(TeamId));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);

	Client->RunOp(TeamsDomain, TEXT("UpdateTeam"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeam Team;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("updateTeam"), Team, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Team);
		}));
}

void UCrowdyTeams::DeleteTeam(int64 TeamId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("DeleteTeam"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyTeams::JoinTeam(int64 TeamId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("JoinTeam"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamMember Member;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("joinTeam"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyTeams::RequestToJoinTeam(int64 TeamId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("RequestToJoinTeam"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamMember Member;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("requestToJoinTeam"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyTeams::LeaveTeam(int64 TeamId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));

	Client->RunOp(TeamsDomain, TEXT("LeaveTeam"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyTeams::AddTeamMember(int64 TeamId, int64 UserId,
                                 FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));

	Client->RunOp(TeamsDomain, TEXT("AddTeamMember"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamMember Member;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("addTeamMember"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyTeams::RemoveTeamMember(int64 TeamId, int64 UserId,
                                    FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupId"), BigInt(TeamId));
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));

	Client->RunOp(TeamsDomain, TEXT("RemoveTeamMember"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyTeams::CreateTeamRole(int64 TeamId, const FString& RoleName,
                                  FCrowdyTeamPermissions Permissions, int32 Rank,
                                  FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(TeamId));
	Input->SetStringField(TEXT("roleName"), RoleName);
	Input->SetNumberField(TEXT("rank"), Rank);
	SetPermissionKeys(Input, Permissions.ToStringArray());

	Client->RunOp(TeamsDomain, TEXT("CreateTeamRole"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamRole Role;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("createTeamRole"), Role, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Role);
		}));
}

void UCrowdyTeams::UpdateTeamRole(int64 TeamRoleId, const FString& RoleName,
                                  FCrowdyTeamPermissions Permissions,
                                  FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupRoleId"), BigInt(TeamRoleId));
	Input->SetStringField(TEXT("roleName"), RoleName);
	SetPermissionKeys(Input, Permissions.ToStringArray());

	Client->RunOp(TeamsDomain, TEXT("UpdateTeamRole"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamRole Role;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("updateTeamRole"), Role, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Role);
		}));
}

void UCrowdyTeams::DeleteTeamRole(int64 TeamRoleId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("groupRoleId"), BigInt(TeamRoleId));

	Client->RunOp(TeamsDomain, TEXT("DeleteTeamRole"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyTeams::SetTeamMemberRoles(int64 TeamId, int64 UserId, const TArray<int64>& RoleIds,
                                      FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), BigInt(TeamId));
	Input->SetStringField(TEXT("userId"), BigInt(UserId));

	TArray<TSharedPtr<FJsonValue>> RoleIdValues;
	for (int64 RoleId : RoleIds)
	{
		RoleIdValues.Add(MakeShared<FJsonValueString>(BigInt(RoleId)));
	}
	Input->SetArrayField(TEXT("roleIds"), RoleIdValues);

	Client->RunOp(TeamsDomain, TEXT("SetTeamMemberRoles"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamMember Member;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("setTeamMemberRoles"), Member, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Member);
		}));
}

void UCrowdyTeams::SetTeamPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
                                 ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
                                 FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), TeamsLogName);
	if (!Client)
	{
		const FCrowdyTeamError Error = ClientUnavailableError<FCrowdyTeamError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("creationPolicy"), FCrowdyTeamPolicy::CreationPolicyToString(CreationPolicy));
	Input->SetStringField(TEXT("defaultMembershipPolicy"),
		FCrowdyTeam::MembershipPolicyToString(DefaultMembershipPolicy));

	Client->RunOp(TeamsDomain, TEXT("SetTeamPolicy"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamPolicy Policy;
			FCrowdyTeamError Error;
			if (!ReadObject(Result, TEXT("setTeamPolicy"), Policy, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Policy);
		}));
}

bool UCrowdyTeams::IsPlayerInTeam(int64 TeamId) const
{
	for (const FCrowdyTeamMembership& Membership : CachedMyTeams)
	{
		if (Membership.Team.TeamId == TeamId) return true;
	}
	return false;
}

bool UCrowdyTeams::GetMyTeamById(int64 TeamId, FCrowdyTeamMembership& OutMembership) const
{
	for (const FCrowdyTeamMembership& Membership : CachedMyTeams)
	{
		if (Membership.Team.TeamId == TeamId)
		{
			OutMembership = Membership;
			return true;
		}
	}
	return false;
}

bool UCrowdyTeams::IsInAnyTeam() const
{
	return CachedMyTeams.Num() > 0;
}

bool UCrowdyTeams::GetPrimaryMembership(FCrowdyTeamMembership& OutMembership) const
{
	if (CachedMyTeams.IsEmpty()) return false;
	OutMembership = CachedMyTeams[0];
	return true;
}

bool UCrowdyTeams::HasPermissionInTeam(int64 TeamId, ECrowdyTeamPermission Permission) const
{
	FCrowdyTeamMembership Membership;
	if (!GetMyTeamById(TeamId, Membership)) return false;
	return Membership.HasPermission(Permission);
}
