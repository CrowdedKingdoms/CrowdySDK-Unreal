#include "Network/GraphQL/FCrowdyQueryDescriptor.h"

// ─────────────────────────────────────────────────────────────────────────────
// Single authoritative table.  One row per query — this is the only place
// that needs editing when a new query is added to the SDK.
//
// Columns:
//   QueryID              — EGraphQLQuery value (what we send)
//   ResponseType         — EQueryResponseType value (what we expect back)
//   ApiTarget            — Management or Game endpoint
//   bRequiresAuth        — whether to attach the Bearer token
//   TimeoutSeconds       — per-query HTTP timeout (replaces the old 120s blanket)
//   MaxRetries           — automatic retries on transient failures (0 = none)
//
// Notes on MaxRetries:
//   UDP_Access = 1 because the game-token sync to the tenant DB can lag on dev.
//   The handoff doc explicitly says "if step 2 fails, log in once more" — this
//   automates that. Retry fires after 1 s (see OnHttpsRequestComplete).
// ─────────────────────────────────────────────────────────────────────────────

const TArray<FCrowdyQueryDescriptor>& FCrowdyQueryDescriptors::GetAll()
{
	static const TArray<FCrowdyQueryDescriptor> Table =
	{
		//  QueryID                              ResponseType                               ApiTarget                      Auth    Timeout  Retries
		{ EGraphQLQuery::Login,                  EQueryResponseType::Login,                 ECrowdyApiTarget::Management,  false,  10.f,    0 },
		{ EGraphQLQuery::Register,               EQueryResponseType::Register,              ECrowdyApiTarget::Management,  false,  10.f,    0 },
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
		{ EGraphQLQuery::PersistencePull,        EQueryResponseType::PersistencePull,       ECrowdyApiTarget::Game,               true,   15.f,    0 },
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
	UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK] FCrowdyQueryDescriptors: No descriptor for query %d"),
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
