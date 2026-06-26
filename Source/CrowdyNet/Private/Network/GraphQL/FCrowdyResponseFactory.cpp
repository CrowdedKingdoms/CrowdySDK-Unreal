#include "Network/GraphQL/FCrowdyResponseFactory.h"
#include "CrowdyNetLog.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

// All response types are included once here.
// CrowdyQuerySubsystem.cpp no longer needs any of these includes.
#include "Queries/Authentication/FLoginResponse.h"
#include "Queries/Authentication/FRegisterResponse.h"
#include "Queries/UDP/FUDPAddressNotify.h"
#include "Queries/Data/Chunks/FGetChunkResponse.h"
#include "Queries/Data/Chunks/FUpdateChunkResponse.h"
#include "Queries/Data/Voxels/FVoxelListResponse.h"
#include "Queries/Data/Voxels/FVoxelListByDistanceResponse.h"
#include "Queries/Data/Avatar/FAvatarCreateResponse.h"
#include "Queries/Data/Avatar/FAvatarNameUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarStateUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarDeleteResponse.h"
#include "Queries/Data/Avatar/Responses/FMyAvatarsResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarResponse.h"
#include "Queries/Data/Avatar/Responses/FUserAvatarsResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarAppStateResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarAppStatesResponse.h"
#include "Queries/Data/Avatar/Responses/FUpdateAvatarAppStateResponse.h"
#include "Queries/Permissions/FTeleportResponse.h"
#include "Queries/Data/Version/FVersionInfoResponse.h"
#include "Queries/Data/User/FGetUserStateResponse.h"
#include "Queries/Data/User/FUpdateUserStateResponse.h"
#include "Queries/Data/GameHost/FGameHostResponse.h"
#include "Queries/Data/Persistence/FPersistencePullResponse.h"
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
#include "Queries/Data/Channels/Responses/FChannelResponses.h"

FCrowdyResponseFactory& FCrowdyResponseFactory::Get()
{
	// Meyer's singleton — thread-safe construction guaranteed by C++11 and later.
	static FCrowdyResponseFactory Instance;
	return Instance;
}

FCrowdyResponseFactory::FCrowdyResponseFactory()
{
	RegisterAll();
}

void FCrowdyResponseFactory::Register(EQueryResponseType ResponseType, FFactoryFn Factory)
{
	Factories.Add(ResponseType, MoveTemp(Factory));
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterAll — one line per response type.
// To support a new response: add the include above and one Register() call here.
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdyResponseFactory::RegisterAll()
{
	Register(EQueryResponseType::Login,                      []() { return MakeShared<FLoginResponse>(); });
	Register(EQueryResponseType::Register,                   []() { return MakeShared<FRegisterResponse>(); });
	Register(EQueryResponseType::UDP_Info,                   []() { return MakeShared<FUDPAddressNotify>(); });
	Register(EQueryResponseType::GetChunkByDistance,         []() { return MakeShared<FGetChunkResponse>(); });
	Register(EQueryResponseType::UpdateChunk,                []() { return MakeShared<FUpdateChunkResponse>(); });
	Register(EQueryResponseType::VoxelList,                  []() { return MakeShared<FVoxelListResponse>(); });
	Register(EQueryResponseType::ListVoxelUpdatesByDistance, []() { return MakeShared<FVoxelListByDistanceResponse>(); });
	Register(EQueryResponseType::CreateAvatar,               []() { return MakeShared<FAvatarCreateResponse>(); });
	Register(EQueryResponseType::MyAvatars,                  []() { return MakeShared<FMyAvatarsResponse>(); });
	Register(EQueryResponseType::UpdateAvatar,               []() { return MakeShared<FAvatarNameUpdateResponse>(); });
	Register(EQueryResponseType::UpdateAvatarState,          []() { return MakeShared<FAvatarStateUpdateResponse>(); });
	Register(EQueryResponseType::DeleteAvatar,               []() { return MakeShared<FAvatarDeleteResponse>(); });
	Register(EQueryResponseType::GetAvatar,                  []() { return MakeShared<FAvatarResponse>(); });
	Register(EQueryResponseType::GetUserAvatars,             []() { return MakeShared<FUserAvatarsResponse>(); });
	Register(EQueryResponseType::GetAvatarAppState,          []() { return MakeShared<FAvatarAppStateResponse>(); });
	Register(EQueryResponseType::GetAvatarAppStates,         []() { return MakeShared<FAvatarAppStatesResponse>(); });
	Register(EQueryResponseType::UpdateAvatarAppState,       []() { return MakeShared<FUpdateAvatarAppStateResponse>(); });
	Register(EQueryResponseType::TeleportRequest,            []() { return MakeShared<FTeleportResponse>(); });
	Register(EQueryResponseType::VersionInfo,                []() { return MakeShared<FVersionInfoResponse>(); });
	Register(EQueryResponseType::GetUserState,               []() { return MakeShared<FGetUserStateResponse>(); });
	Register(EQueryResponseType::UpdateUserState,            []() { return MakeShared<FUpdateUserStateResponse>(); });
	Register(EQueryResponseType::GameHost,                   []() { return MakeShared<FGameHostResponse>(); });
	Register(EQueryResponseType::PersistencePull,            []() { return MakeShared<FPersistencePullResponse>(); });

	// Teams
	Register(EQueryResponseType::MyTeams,            []() { return MakeShared<FMyTeamsResponse>(); });
	Register(EQueryResponseType::Team,               []() { return MakeShared<FTeamResponse>(); });
	Register(EQueryResponseType::Teams,              []() { return MakeShared<FTeamsResponse>(); });
	Register(EQueryResponseType::TeamMembers,        []() { return MakeShared<FTeamMembersResponse>(); });
	Register(EQueryResponseType::TeamRoles,          []() { return MakeShared<FTeamRolesResponse>(); });
	Register(EQueryResponseType::TeamPolicy,         []() { return MakeShared<FTeamPolicyResponse>(); });
	Register(EQueryResponseType::CreateTeam,         []() { return MakeShared<FCreateTeamResponse>(); });
	Register(EQueryResponseType::JoinTeam,           []() { return MakeShared<FJoinTeamResponse>(); });
	Register(EQueryResponseType::RequestToJoinTeam,  []() { return MakeShared<FRequestToJoinTeamResponse>(); });
	Register(EQueryResponseType::LeaveTeam,          []() { return MakeShared<FLeaveTeamResponse>(); });
	Register(EQueryResponseType::AddTeamMember,      []() { return MakeShared<FAddTeamMemberResponse>(); });
	Register(EQueryResponseType::RemoveTeamMember,   []() { return MakeShared<FRemoveTeamMemberResponse>(); });
	Register(EQueryResponseType::CreateTeamRole,     []() { return MakeShared<FCreateTeamRoleResponse>(); });
	Register(EQueryResponseType::SetTeamMemberRoles, []() { return MakeShared<FSetTeamMemberRolesResponse>(); });
	Register(EQueryResponseType::UpdateTeamRole,     []() { return MakeShared<FUpdateTeamRoleResponse>(); });
	Register(EQueryResponseType::DeleteTeamRole,     []() { return MakeShared<FDeleteTeamRoleResponse>(); });
	Register(EQueryResponseType::UpdateTeam,         []() { return MakeShared<FUpdateTeamResponse>(); });
	Register(EQueryResponseType::DeleteTeam,         []() { return MakeShared<FDeleteTeamResponse>(); });
	Register(EQueryResponseType::SetTeamPolicy,      []() { return MakeShared<FSetTeamPolicyResponse>(); });

	// Channels
	Register(EQueryResponseType::MyChannels,            []() { return MakeShared<FMyChannelsResponse>(); });
	Register(EQueryResponseType::Channel,               []() { return MakeShared<FChannelResponse>(); });
	Register(EQueryResponseType::Channels,              []() { return MakeShared<FChannelsResponse>(); });
	Register(EQueryResponseType::ChannelMembers,        []() { return MakeShared<FChannelMembersResponse>(); });
	Register(EQueryResponseType::ChannelRoles,          []() { return MakeShared<FChannelRolesResponse>(); });
	Register(EQueryResponseType::ChannelPolicy,         []() { return MakeShared<FChannelPolicyResponse>(); });
	Register(EQueryResponseType::CreateChannel,         []() { return MakeShared<FCreateChannelResponse>(); });
	Register(EQueryResponseType::UpdateChannel,         []() { return MakeShared<FUpdateChannelResponse>(); });
	Register(EQueryResponseType::DeleteChannel,         []() { return MakeShared<FDeleteChannelResponse>(); });
	Register(EQueryResponseType::JoinChannel,           []() { return MakeShared<FJoinChannelResponse>(); });
	Register(EQueryResponseType::RequestToJoinChannel,  []() { return MakeShared<FRequestToJoinChannelResponse>(); });
	Register(EQueryResponseType::LeaveChannel,          []() { return MakeShared<FLeaveChannelResponse>(); });
	Register(EQueryResponseType::AddChannelMember,      []() { return MakeShared<FAddChannelMemberResponse>(); });
	Register(EQueryResponseType::RemoveChannelMember,   []() { return MakeShared<FRemoveChannelMemberResponse>(); });
	Register(EQueryResponseType::CreateChannelRole,     []() { return MakeShared<FCreateChannelRoleResponse>(); });
	Register(EQueryResponseType::UpdateChannelRole,     []() { return MakeShared<FUpdateChannelRoleResponse>(); });
	Register(EQueryResponseType::DeleteChannelRole,     []() { return MakeShared<FDeleteChannelRoleResponse>(); });
	Register(EQueryResponseType::SetChannelMemberRoles, []() { return MakeShared<FSetChannelMemberRolesResponse>(); });
	Register(EQueryResponseType::SetChannelPolicy,      []() { return MakeShared<FSetChannelPolicyResponse>(); });
}

TSharedPtr<ICrowdyQueryResponse> FCrowdyResponseFactory::Create(EQueryResponseType ResponseType) const
{
	const FFactoryFn* Factory = Factories.Find(ResponseType);
	if (!Factory)
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[CrowdyResponseFactory] No factory registered for response type %d — "
			     "add a Register() call to FCrowdyResponseFactory::RegisterAll()"),
			static_cast<int32>(ResponseType));
		return nullptr;
	}
	return (*Factory)();
}

bool FCrowdyResponseFactory::IsRegistered(EQueryResponseType ResponseType) const
{
	return Factories.Contains(ResponseType);
}
