#include "Subsystem/AsyncActions/CrowdyKitSocialActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/AsyncActions/CrowdyServicesActionSupport.h"

void UCrowdyKitSocialGroupAction::Activate()
{
	UCrowdyKitSocial* Social = GetServiceSubsystem<UCrowdyKitSocial>(WorldContextObject);
	if (!Social)
	{
		HandleFailed(FCrowdyTeamError::FromMessage(TEXT("UCrowdyKitSocial not found")));
		return;
	}

	Run(*Social,
		FCrowdyKitGroupDone::CreateUObject(this, &UCrowdyKitSocialGroupAction::HandleDone),
		FCrowdyKitFailed::CreateUObject(this, &UCrowdyKitSocialGroupAction::HandleFailed));
}

void UCrowdyKitSocialGroupAction::HandleDone(const FCrowdyKitGroup& Group)
{
	Completed.Broadcast(Group, FCrowdyTeamError(), FString());
	SetReadyToDestroy();
}

void UCrowdyKitSocialGroupAction::HandleFailed(const FCrowdyTeamError& Error)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Error.Message);
	Failed.Broadcast(FCrowdyKitGroup(), Error, Error.Message);
	SetReadyToDestroy();
}

void UCrowdyKitSocialMembersAction::Activate()
{
	UCrowdyKitSocial* Social = GetServiceSubsystem<UCrowdyKitSocial>(WorldContextObject);
	if (!Social)
	{
		HandleFailed(FCrowdyTeamError::FromMessage(TEXT("UCrowdyKitSocial not found")));
		return;
	}

	Run(*Social,
		FCrowdyKitMembersDone::CreateUObject(this, &UCrowdyKitSocialMembersAction::HandleDone),
		FCrowdyKitFailed::CreateUObject(this, &UCrowdyKitSocialMembersAction::HandleFailed));
}

void UCrowdyKitSocialMembersAction::HandleDone(const FCrowdyKitGroup& Answered, const TArray<FCrowdyTeamMember>& Members)
{
	Completed.Broadcast(Answered, Members, FCrowdyTeamError(), FString());
	SetReadyToDestroy();
}

void UCrowdyKitSocialMembersAction::HandleFailed(const FCrowdyTeamError& Error)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Error.Message);
	Failed.Broadcast(FCrowdyKitGroup(), TArray<FCrowdyTeamMember>(), Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyKitSocial_CreateParty* UCrowdyKitSocial_CreateParty::CreateParty(UObject* WorldContextObject, const FString& Name)
{
	UCrowdyKitSocial_CreateParty* Action = NewObject<UCrowdyKitSocial_CreateParty>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_CreateParty::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.CreateParty(Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_FindParty* UCrowdyKitSocial_FindParty::FindParty(UObject* WorldContextObject, const FString& Name)
{
	UCrowdyKitSocial_FindParty* Action = NewObject<UCrowdyKitSocial_FindParty>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_FindParty::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.FindParty(Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_InviteToParty* UCrowdyKitSocial_InviteToParty::InviteToParty(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group, int64 UserId)
{
	UCrowdyKitSocial_InviteToParty* Action = NewObject<UCrowdyKitSocial_InviteToParty>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_InviteToParty::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.InviteToParty(Group, UserId, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_JoinParty* UCrowdyKitSocial_JoinParty::JoinParty(UObject* WorldContextObject, const FCrowdyKitGroup& Group)
{
	UCrowdyKitSocial_JoinParty* Action = NewObject<UCrowdyKitSocial_JoinParty>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_JoinParty::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.JoinParty(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_LeaveParty* UCrowdyKitSocial_LeaveParty::LeaveParty(UObject* WorldContextObject, const FCrowdyKitGroup& Group)
{
	UCrowdyKitSocial_LeaveParty* Action = NewObject<UCrowdyKitSocial_LeaveParty>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_LeaveParty::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.LeaveParty(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_GetPartyMembers* UCrowdyKitSocial_GetPartyMembers::GetPartyMembers(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group)
{
	UCrowdyKitSocial_GetPartyMembers* Action = NewObject<UCrowdyKitSocial_GetPartyMembers>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_GetPartyMembers::Run(UCrowdyKitSocial& Social, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.GetPartyMembers(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_CreateGuild* UCrowdyKitSocial_CreateGuild::CreateGuild(UObject* WorldContextObject, const FString& Name,
	ECrowdyTeamMembershipPolicy MembershipPolicy)
{
	UCrowdyKitSocial_CreateGuild* Action = NewObject<UCrowdyKitSocial_CreateGuild>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->MembershipPolicy = MembershipPolicy;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_CreateGuild::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.CreateGuild(Name, MembershipPolicy, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_FindGuild* UCrowdyKitSocial_FindGuild::FindGuild(UObject* WorldContextObject, const FString& Name)
{
	UCrowdyKitSocial_FindGuild* Action = NewObject<UCrowdyKitSocial_FindGuild>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_FindGuild::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.FindGuild(Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_JoinGuild* UCrowdyKitSocial_JoinGuild::JoinGuild(UObject* WorldContextObject, const FCrowdyKitGroup& Group)
{
	UCrowdyKitSocial_JoinGuild* Action = NewObject<UCrowdyKitSocial_JoinGuild>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_JoinGuild::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.JoinGuild(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_AcceptIntoGuild* UCrowdyKitSocial_AcceptIntoGuild::AcceptIntoGuild(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group, int64 UserId)
{
	UCrowdyKitSocial_AcceptIntoGuild* Action = NewObject<UCrowdyKitSocial_AcceptIntoGuild>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_AcceptIntoGuild::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.AcceptIntoGuild(Group, UserId, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_GetGuildRoster* UCrowdyKitSocial_GetGuildRoster::GetGuildRoster(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group)
{
	UCrowdyKitSocial_GetGuildRoster* Action = NewObject<UCrowdyKitSocial_GetGuildRoster>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_GetGuildRoster::Run(UCrowdyKitSocial& Social, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.GetGuildRoster(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_PromoteInGuild* UCrowdyKitSocial_PromoteInGuild::PromoteInGuild(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group, int64 UserId, const TArray<int64>& RoleIds)
{
	UCrowdyKitSocial_PromoteInGuild* Action = NewObject<UCrowdyKitSocial_PromoteInGuild>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->UserId = UserId;
	Action->RoleIds = RoleIds;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_PromoteInGuild::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.PromoteInGuild(Group, UserId, RoleIds, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_FindOrCreateChatRoom* UCrowdyKitSocial_FindOrCreateChatRoom::FindOrCreateChatRoom(
	UObject* WorldContextObject, const FString& Name)
{
	UCrowdyKitSocial_FindOrCreateChatRoom* Action = NewObject<UCrowdyKitSocial_FindOrCreateChatRoom>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_FindOrCreateChatRoom::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.FindOrCreateChatRoom(Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

UCrowdyKitSocial_ClaimGuildTerritory* UCrowdyKitSocial_ClaimGuildTerritory::ClaimGuildTerritory(UObject* WorldContextObject,
	const FCrowdyKitGroup& Group, int64 GridId, const TArray<FString>& PermissionKeys, int64 RoleId)
{
	UCrowdyKitSocial_ClaimGuildTerritory* Action = NewObject<UCrowdyKitSocial_ClaimGuildTerritory>();
	Action->WorldContextObject = WorldContextObject;
	Action->Group = Group;
	Action->GridId = GridId;
	Action->PermissionKeys = PermissionKeys;
	Action->RoleId = RoleId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyKitSocial_ClaimGuildTerritory::Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	Social.ClaimGuildTerritory(Group, GridId, PermissionKeys, RoleId, MoveTemp(OnDone), MoveTemp(OnFail));
}
