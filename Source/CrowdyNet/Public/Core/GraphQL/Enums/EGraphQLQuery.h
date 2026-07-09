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

	// Passwordless authentication + app-scoped tokens
	RequestLoginLink  UMETA(DisplayName = "Request Login Link Mutation"),
	CompleteLoginLink UMETA(DisplayName = "Complete Login Link Mutation"),
	DevLogin          UMETA(DisplayName = "Dev Login Mutation"),
	MintAppToken      UMETA(DisplayName = "Mint App Token Mutation"),
	RefreshAppToken   UMETA(DisplayName = "Refresh App Token Mutation"),

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
	AmIGameHost		UMETA(DisplayName = "Am I Game Host Query"),
	ActorOwner		UMETA(DisplayName = "Actor Owner Query"),

	// Persistence system (inline query body - no data-asset entry needed)
	PersistencePull UMETA(DisplayName = "Persistence Pull Query"),

	// Teams - queries
	MyTeams            UMETA(DisplayName = "My Teams Query"),
	Team               UMETA(DisplayName = "Team Query"),
	Teams              UMETA(DisplayName = "Teams Query"),
	TeamMembers        UMETA(DisplayName = "Team Members Query"),
	TeamRoles          UMETA(DisplayName = "Team Roles Query"),
	TeamPolicy         UMETA(DisplayName = "Team Policy Query"),

	// Teams - mutations
	CreateTeam         UMETA(DisplayName = "Create Team Mutation"),
	JoinTeam           UMETA(DisplayName = "Join Team Mutation"),
	RequestToJoinTeam  UMETA(DisplayName = "Request To Join Team Mutation"),
	LeaveTeam          UMETA(DisplayName = "Leave Team Mutation"),
	AddTeamMember      UMETA(DisplayName = "Add Team Member Mutation"),
	RemoveTeamMember   UMETA(DisplayName = "Remove Team Member Mutation"),
	CreateTeamRole     UMETA(DisplayName = "Create Team Role Mutation"),
	SetTeamMemberRoles UMETA(DisplayName = "Set Team Member Roles Mutation"),
	UpdateTeamRole     UMETA(DisplayName = "Update Team Role Mutation"),
	DeleteTeamRole     UMETA(DisplayName = "Delete Team Role Mutation"),
	UpdateTeam         UMETA(DisplayName = "Update Team Mutation"),
	DeleteTeam         UMETA(DisplayName = "Delete Team Mutation"),
	SetTeamPolicy      UMETA(DisplayName = "Set Team Policy Mutation"),

	// Avatar - new queries/mutations (inline query body — no data-asset entry needed)
	GetAvatar            UMETA(DisplayName = "Get Avatar Query"),
	GetUserAvatars       UMETA(DisplayName = "Get User Avatars Query"),
	GetAvatarAppState    UMETA(DisplayName = "Get Avatar App State Query"),
	GetAvatarAppStates   UMETA(DisplayName = "Get Avatar App States Query"),
	UpdateAvatarAppState UMETA(DisplayName = "Update Avatar App State Mutation"),

	// Channels - queries (channels share the group model with teams; group_type = channel)
	MyChannels            UMETA(DisplayName = "My Channels Query"),
	Channel               UMETA(DisplayName = "Channel Query"),
	Channels              UMETA(DisplayName = "Channels Query"),
	ChannelMembers        UMETA(DisplayName = "Channel Members Query"),
	ChannelRoles          UMETA(DisplayName = "Channel Roles Query"),
	ChannelPolicy         UMETA(DisplayName = "Channel Policy Query"),

	// Channels - mutations
	CreateChannel         UMETA(DisplayName = "Create Channel Mutation"),
	UpdateChannel         UMETA(DisplayName = "Update Channel Mutation"),
	DeleteChannel         UMETA(DisplayName = "Delete Channel Mutation"),
	JoinChannel           UMETA(DisplayName = "Join Channel Mutation"),
	RequestToJoinChannel  UMETA(DisplayName = "Request To Join Channel Mutation"),
	LeaveChannel          UMETA(DisplayName = "Leave Channel Mutation"),
	AddChannelMember      UMETA(DisplayName = "Add Channel Member Mutation"),
	RemoveChannelMember   UMETA(DisplayName = "Remove Channel Member Mutation"),
	CreateChannelRole     UMETA(DisplayName = "Create Channel Role Mutation"),
	UpdateChannelRole     UMETA(DisplayName = "Update Channel Role Mutation"),
	DeleteChannelRole     UMETA(DisplayName = "Delete Channel Role Mutation"),
	SetChannelMemberRoles UMETA(DisplayName = "Set Channel Member Roles Mutation"),
	SetChannelPolicy      UMETA(DisplayName = "Set Channel Policy Mutation"),

	// Social sign-in + identities (M2)
	SocialLoginStart         UMETA(DisplayName = "Social Login Start Mutation"),
	SocialLoginComplete      UMETA(DisplayName = "Social Login Complete Mutation"),
	AvailableLoginProviders  UMETA(DisplayName = "Available Login Providers Query"),
	MyIdentities             UMETA(DisplayName = "My Identities Query"),
	LinkIdentity             UMETA(DisplayName = "Link Identity Mutation"),
	UnlinkIdentity           UMETA(DisplayName = "Unlink Identity Mutation"),
};
