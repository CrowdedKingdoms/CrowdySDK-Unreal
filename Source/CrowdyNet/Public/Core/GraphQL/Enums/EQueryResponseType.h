#pragma once
#include "CoreMinimal.h"
#include "EQueryResponseType.generated.h"

UENUM()
enum EQueryResponseType: uint8
{
	Login,
	Register,

	// Passwordless authentication + app-scoped tokens
	RequestLoginLink,
	CompleteLoginLink,
	DevLogin,
	MintAppToken,
	RefreshAppToken,

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
	AmIGameHost,
	ActorOwner,
	PersistencePull,

	// Teams - queries
	MyTeams,
	Team,
	Teams,
	TeamMembers,
	TeamRoles,
	TeamPolicy,

	// Teams - mutations
	CreateTeam,
	JoinTeam,
	RequestToJoinTeam,
	LeaveTeam,
	AddTeamMember,
	RemoveTeamMember,
	CreateTeamRole,
	SetTeamMemberRoles,
	UpdateTeamRole,
	DeleteTeamRole,
	UpdateTeam,
	DeleteTeam,
	SetTeamPolicy,

	// Avatar - new
	GetAvatar,
	GetUserAvatars,
	GetAvatarAppState,
	GetAvatarAppStates,
	UpdateAvatarAppState,

	// Channels - queries
	MyChannels,
	Channel,
	Channels,
	ChannelMembers,
	ChannelRoles,
	ChannelPolicy,

	// Channels - mutations
	CreateChannel,
	UpdateChannel,
	DeleteChannel,
	JoinChannel,
	RequestToJoinChannel,
	LeaveChannel,
	AddChannelMember,
	RemoveChannelMember,
	CreateChannelRole,
	UpdateChannelRole,
	DeleteChannelRole,
	SetChannelMemberRoles,
	SetChannelPolicy,

	// Social sign-in + identities (M2)
	SocialLoginStart,
	SocialLoginComplete,
	AvailableLoginProviders,
	MyIdentities,
	LinkIdentity,
	UnlinkIdentity,
};
