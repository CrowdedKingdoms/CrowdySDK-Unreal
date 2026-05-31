#include "Network/GraphQL/FCrowdyResponseFactory.h"
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
#include "Queries/Data/Avatar/FFetchAvatarsResponse.h"
#include "Queries/Data/Avatar/FAvatarNameUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarStateUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarDeleteResponse.h"
#include "Queries/Permissions/FTeleportResponse.h"
#include "Queries/Data/Version/FVersionInfoResponse.h"
#include "Queries/Data/User/FGetUserStateResponse.h"
#include "Queries/Data/User/FUpdateUserStateResponse.h"
#include "Queries/Data/GameHost/FGameHostResponse.h"
#include "Queries/Data/Persistence/FPersistencePullResponse.h"

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
	Register(EQueryResponseType::MyAvatars,                  []() { return MakeShared<FFetchAvatarsResponse>(); });
	Register(EQueryResponseType::UpdateAvatar,               []() { return MakeShared<FAvatarNameUpdateResponse>(); });
	Register(EQueryResponseType::UpdateAvatarState,          []() { return MakeShared<FAvatarStateUpdateResponse>(); });
	Register(EQueryResponseType::DeleteAvatar,               []() { return MakeShared<FAvatarDeleteResponse>(); });
	Register(EQueryResponseType::TeleportRequest,            []() { return MakeShared<FTeleportResponse>(); });
	Register(EQueryResponseType::VersionInfo,                []() { return MakeShared<FVersionInfoResponse>(); });
	Register(EQueryResponseType::GetUserState,               []() { return MakeShared<FGetUserStateResponse>(); });
	Register(EQueryResponseType::UpdateUserState,            []() { return MakeShared<FUpdateUserStateResponse>(); });
	Register(EQueryResponseType::GameHost,                   []() { return MakeShared<FGameHostResponse>(); });
	Register(EQueryResponseType::PersistencePull,            []() { return MakeShared<FPersistencePullResponse>(); });
}

TSharedPtr<ICrowdyQueryResponse> FCrowdyResponseFactory::Create(EQueryResponseType ResponseType) const
{
	const FFactoryFn* Factory = Factories.Find(ResponseType);
	if (!Factory)
	{
		UE_LOG(LogTemp, Warning,
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
