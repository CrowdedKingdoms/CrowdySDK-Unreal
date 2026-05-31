#pragma once
#include "CoreMinimal.h"
#include "EGraphQLQuery.generated.h"

/**
 * Enum representing different GraphQL queries for use within the application.
 *
 * This enumeration is used to specify the type of GraphQL query to execute.
 * Values are represented as uint8 and are BlueprintType, allowing them to be
 * used effectively within Unreal Engine's Blueprints.
 *
 * Enumerator:
 * - Login: Represents a "Login Query" to be executed.
 * - Register: Represents a "Register Query" to be executed.
 * - GetChunk: Represents a "Get Chunk Query" to be executed.
 * - UpdateChunk: Represents a "Update Chunk Query" to be executed.
 * - VoxelList: Represents a "Voxel List Query" to be executed.
 * - UDP_Access: Represents a "UDP Access Query" to be executed.
 */
UENUM(BlueprintType)
enum class EGraphQLQuery : uint8
{
	// Login and Register
	Login       UMETA(DisplayName = "Login Query"),
	Register    UMETA(DisplayName = "Register Query"),

	//UDP
	UDP_Access  UMETA(DisplayName = "UDP Access Query"),

	// Chunk Related
	GetChunkByDistance    UMETA(DisplayName = "Get Chunk by Distance Query"),
	UpdateChunk UMETA(DisplayName = "Update Chunk Query"),
	VoxelList   UMETA(DisplayName = "Voxel List Query"),

	// Avatar Related
	CreateAvatar		UMETA(DisplayName = "Create Avatar Query"),
	MyAvatars			UMETA(DisplayName = "Fetch My Avatars Query"),
	UpdateAvatarName	UMETA(DisplayName = "Update Avatar Name Query"),
	UpdateAvatarState	UMETA(DisplayName = "Update Avatar State Query"),

	ListVoxelUpdatesByDistance UMETA(DisplayName = "List Voxel Updates by Distance Query"),

	// Teleport
	TeleportRequest UMETA(DisplayName = "Teleport Request Query"),

	DeleteAvatar	UMETA(DisplayName = "Delete Avatar Query"),
	GetVersionInfo	UMETA(DisplayName = "Get Version Info Query"),

	// User State
	UpdateUserState UMETA(DisplayName = "Update User State Query"),
	GetUserState	UMETA(DisplayName = "Get User State Query"),

	// Game Host
	GameHost		UMETA(DisplayName = "Game Host Query"),

	// Persistence system (inline query body — no data-asset entry needed)
	PersistencePull UMETA(DisplayName = "Persistence Pull Query"),
};
