#include "Network/GraphQL/FCrowdyQueryDescriptor.h"
#include "CrowdyNetLog.h"


// Single authoritative table.  One row per query this is the only place
// that needs editing when a new query is added to the SDK.
//
// Columns:
//   QueryID EGraphQLQuery value (what we send)
//   ResponseType EQueryResponseType value (what we expect back)
//   ApiTarget Management or Game endpoint
//   bRequiresAuth whether to attach the Bearer token
//   TimeoutSeconds per-query HTTP timeout (replaces the old 120s blanket)
//   MaxRetries automatic retries on transient failures (0 = none)
//
// Notes on MaxRetries:
//   UDP_Access = 1 because the game-token sync to the tenant DB can lag on dev.
//   The handoff doc explicitly says "if step 2 fails, log in once more" this
//   automates that. Retry fires after 1 s (see OnHttpsRequestComplete).

const TArray<FCrowdyQueryDescriptor>& FCrowdyQueryDescriptors::GetAll()
{
	static const TArray<FCrowdyQueryDescriptor> Table =
	{
		//  QueryID                              ResponseType                               ApiTarget                      Auth    Timeout  Retries
		{ EGraphQLQuery::Login,                  EQueryResponseType::Login,                 ECrowdyApiTarget::Management,  false,  10.f,    0 },
		{ EGraphQLQuery::Register,               EQueryResponseType::Register,              ECrowdyApiTarget::Management,  false,  10.f,    0 },

		// Passwordless authentication + app-scoped tokens (Management endpoint).
		// Sign-in mutations are public (None). mintAppToken uses the SESSION token
		// (Auto -> Session). refreshAppToken re-presents the current APP token, so
		// it is pinned to App scope even though it is a Management-plane mutation.
		{ EGraphQLQuery::RequestLoginLink,       EQueryResponseType::RequestLoginLink,      ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::CompleteLoginLink,      EQueryResponseType::CompleteLoginLink,     ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::DevLogin,               EQueryResponseType::DevLogin,              ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::MintAppToken,           EQueryResponseType::MintAppToken,          ECrowdyApiTarget::Management,  true,   10.f,    0,  ECrowdyTokenScope::Session },
		{ EGraphQLQuery::RefreshAppToken,        EQueryResponseType::RefreshAppToken,       ECrowdyApiTarget::Management,  true,   10.f,    0,  ECrowdyTokenScope::App },

		// Social sign-in + identities (M2, Management endpoint). Sign-in ops are public
		// (None); identity management requires the SESSION token (Session scope).
		{ EGraphQLQuery::SocialLoginStart,        EQueryResponseType::SocialLoginStart,        ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::SocialLoginComplete,     EQueryResponseType::SocialLoginComplete,     ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::AvailableLoginProviders, EQueryResponseType::AvailableLoginProviders, ECrowdyApiTarget::Management,  false,  10.f,    0,  ECrowdyTokenScope::None },
		{ EGraphQLQuery::MyIdentities,            EQueryResponseType::MyIdentities,            ECrowdyApiTarget::Management,  true,   10.f,    0,  ECrowdyTokenScope::Session },
		{ EGraphQLQuery::LinkIdentity,            EQueryResponseType::LinkIdentity,            ECrowdyApiTarget::Management,  true,   10.f,    0,  ECrowdyTokenScope::Session },
		{ EGraphQLQuery::UnlinkIdentity,          EQueryResponseType::UnlinkIdentity,          ECrowdyApiTarget::Management,  true,   10.f,    0,  ECrowdyTokenScope::Session },

		{ EGraphQLQuery::UDP_Access,             EQueryResponseType::UDP_Info,              ECrowdyApiTarget::Game,        true,    5.f,    10 },
		{ EGraphQLQuery::GetChunkByDistance,     EQueryResponseType::GetChunkByDistance,    ECrowdyApiTarget::Game,        true,   15.f,    0 },
		{ EGraphQLQuery::UpdateChunk,            EQueryResponseType::UpdateChunk,           ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::VoxelList,              EQueryResponseType::VoxelList,             ECrowdyApiTarget::Game,        true,   15.f,    0 },
		{ EGraphQLQuery::ListVoxelUpdatesByDistance, EQueryResponseType::ListVoxelUpdatesByDistance, ECrowdyApiTarget::Game, true, 15.f,   0 },
		{ EGraphQLQuery::CreateAvatar,           EQueryResponseType::CreateAvatar,          ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::MyAvatars,              EQueryResponseType::MyAvatars,             ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::UpdateAvatarName,       EQueryResponseType::UpdateAvatar,          ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::UpdateAvatarState,      EQueryResponseType::UpdateAvatarState,     ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::DeleteAvatar,           EQueryResponseType::DeleteAvatar,          ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::TeleportRequest,        EQueryResponseType::TeleportRequest,       ECrowdyApiTarget::Game,        true,   10.f,    0 },
		{ EGraphQLQuery::GetVersionInfo,         EQueryResponseType::VersionInfo,           ECrowdyApiTarget::Game,        false,  15.f,    0 },
		{ EGraphQLQuery::UpdateUserState,        EQueryResponseType::UpdateUserState,       ECrowdyApiTarget::Management,        true,   10.f,    0 },
		{ EGraphQLQuery::GetUserState,           EQueryResponseType::GetUserState,          ECrowdyApiTarget::Management,        true,   10.f,    0 },
		{ EGraphQLQuery::GameHost,               EQueryResponseType::GameHost,              ECrowdyApiTarget::Game,               true,   10.f,    0 },
		{ EGraphQLQuery::AmIGameHost,            EQueryResponseType::AmIGameHost,           ECrowdyApiTarget::Game,               true,   10.f,    0 },
		{ EGraphQLQuery::ActorOwner,             EQueryResponseType::ActorOwner,            ECrowdyApiTarget::Game,               true,   10.f,    0 },
		{ EGraphQLQuery::PersistencePull,        EQueryResponseType::PersistencePull,       ECrowdyApiTarget::Game,               true,   15.f,    0 },

		// Teams - queries
		{ EGraphQLQuery::MyTeams,            EQueryResponseType::MyTeams,            ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::Team,               EQueryResponseType::Team,               ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::Teams,              EQueryResponseType::Teams,              ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::TeamMembers,        EQueryResponseType::TeamMembers,        ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::TeamRoles,          EQueryResponseType::TeamRoles,          ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::TeamPolicy,         EQueryResponseType::TeamPolicy,         ECrowdyApiTarget::Game, true,  10.f, 0 },

		// Teams - mutations
		{ EGraphQLQuery::CreateTeam,         EQueryResponseType::CreateTeam,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::JoinTeam,           EQueryResponseType::JoinTeam,           ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::RequestToJoinTeam,  EQueryResponseType::RequestToJoinTeam,  ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::LeaveTeam,          EQueryResponseType::LeaveTeam,          ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::AddTeamMember,      EQueryResponseType::AddTeamMember,      ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::RemoveTeamMember,   EQueryResponseType::RemoveTeamMember,   ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::CreateTeamRole,     EQueryResponseType::CreateTeamRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::SetTeamMemberRoles, EQueryResponseType::SetTeamMemberRoles, ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::UpdateTeamRole,     EQueryResponseType::UpdateTeamRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::DeleteTeamRole,     EQueryResponseType::DeleteTeamRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::UpdateTeam,         EQueryResponseType::UpdateTeam,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::DeleteTeam,         EQueryResponseType::DeleteTeam,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::SetTeamPolicy,      EQueryResponseType::SetTeamPolicy,      ECrowdyApiTarget::Game, true,  10.f, 0 },

		// Avatar - new (inline query body — no data-asset entry needed)
		{ EGraphQLQuery::GetAvatar,            EQueryResponseType::GetAvatar,            ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::GetUserAvatars,       EQueryResponseType::GetUserAvatars,       ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::GetAvatarAppState,    EQueryResponseType::GetAvatarAppState,    ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::GetAvatarAppStates,   EQueryResponseType::GetAvatarAppStates,   ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::UpdateAvatarAppState, EQueryResponseType::UpdateAvatarAppState, ECrowdyApiTarget::Game, true,  10.f, 0 },

		// Channels - queries
		{ EGraphQLQuery::MyChannels,            EQueryResponseType::MyChannels,            ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::Channel,               EQueryResponseType::Channel,               ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::Channels,              EQueryResponseType::Channels,              ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::ChannelMembers,        EQueryResponseType::ChannelMembers,        ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::ChannelRoles,          EQueryResponseType::ChannelRoles,          ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::ChannelPolicy,         EQueryResponseType::ChannelPolicy,         ECrowdyApiTarget::Game, true,  10.f, 0 },

		// Channels - mutations
		{ EGraphQLQuery::CreateChannel,         EQueryResponseType::CreateChannel,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::UpdateChannel,         EQueryResponseType::UpdateChannel,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::DeleteChannel,         EQueryResponseType::DeleteChannel,         ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::JoinChannel,           EQueryResponseType::JoinChannel,           ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::RequestToJoinChannel,  EQueryResponseType::RequestToJoinChannel,  ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::LeaveChannel,          EQueryResponseType::LeaveChannel,          ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::AddChannelMember,      EQueryResponseType::AddChannelMember,      ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::RemoveChannelMember,   EQueryResponseType::RemoveChannelMember,   ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::CreateChannelRole,     EQueryResponseType::CreateChannelRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::UpdateChannelRole,     EQueryResponseType::UpdateChannelRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::DeleteChannelRole,     EQueryResponseType::DeleteChannelRole,     ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::SetChannelMemberRoles, EQueryResponseType::SetChannelMemberRoles, ECrowdyApiTarget::Game, true,  10.f, 0 },
		{ EGraphQLQuery::SetChannelPolicy,      EQueryResponseType::SetChannelPolicy,      ECrowdyApiTarget::Game, true,  10.f, 0 },
	};
	return Table;
}

const FCrowdyQueryDescriptor* FCrowdyQueryDescriptors::Find(EGraphQLQuery QueryID)
{
	for (const FCrowdyQueryDescriptor& Desc : GetAll())
	{
		if (Desc.QueryID == QueryID)
			return &Desc;
	}
	UE_LOG(LogCrowdyNet, Warning, TEXT("FCrowdyQueryDescriptors: No descriptor for query %d"),
		static_cast<int32>(QueryID));
	return nullptr;
}

bool FCrowdyQueryDescriptors::IsManagementQuery(EGraphQLQuery QueryID)
{
	if (const FCrowdyQueryDescriptor* Desc = Find(QueryID))
		return Desc->ApiTarget == ECrowdyApiTarget::Management;
	return false;
}

float FCrowdyQueryDescriptors::GetTimeout(EGraphQLQuery QueryID)
{
	if (const FCrowdyQueryDescriptor* Desc = Find(QueryID))
		return Desc->TimeoutSeconds;
	return 10.f;
}

int32 FCrowdyQueryDescriptors::GetMaxRetries(EGraphQLQuery QueryID)
{
	if (const FCrowdyQueryDescriptor* Desc = Find(QueryID))
		return Desc->MaxRetries;
	return 0;
}

EQueryResponseType FCrowdyQueryDescriptors::GetResponseType(EGraphQLQuery QueryID)
{
	if (const FCrowdyQueryDescriptor* Desc = Find(QueryID))
		return Desc->ResponseType;
	return EQueryResponseType::Error;
}

ECrowdyTokenScope FCrowdyQueryDescriptors::GetTokenScope(EGraphQLQuery QueryID)
{
	const FCrowdyQueryDescriptor* Desc = Find(QueryID);
	if (!Desc)
		return ECrowdyTokenScope::Auto;

	if (Desc->TokenScope != ECrowdyTokenScope::Auto)
		return Desc->TokenScope;

	// Auto: Session token for the Management plane, App token for gameplay.
	return Desc->ApiTarget == ECrowdyApiTarget::Management
		? ECrowdyTokenScope::Session
		: ECrowdyTokenScope::App;
}
