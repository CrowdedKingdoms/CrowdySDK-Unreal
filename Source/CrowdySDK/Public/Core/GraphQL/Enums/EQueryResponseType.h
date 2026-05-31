#pragma once
#include "CoreMinimal.h"
#include "EQueryResponseType.generated.h"

UENUM()
enum EQueryResponseType: uint8
{
	Login,
	Register,
	UDP_Info,
	GetChunkByDistance,
	UpdateChunk,
	VoxelList,
	CreateAvatar,
	MyAvatars,
	UpdateAvatar,
	UpdateAvatarState,
	Error,
	ListVoxelUpdatesByDistance,
	TeleportRequest,
	DeleteAvatar,
	VersionInfo,
	UpdateUserState,
	GetUserState,
	GameHost,
	PersistencePull,
};
