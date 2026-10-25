#include "Subsystem/CrowdyKitSocial.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystem/CrowdyChannels.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyTeams.h"
#include "Utils/HelperFunctions.h"

using namespace CrowdyServiceApi;

namespace
{
	constexpr const TCHAR* KitSocialLogName = TEXT("CrowdyKitSocial");

	// Stands in for a server code on the result handed back when no client could be resolved.
	constexpr const TCHAR* KitSocialNoClientCode = TEXT("KIT_SOCIAL_NO_CLIENT");

	FCrowdyTeamError KitSocialNotFound(const FString& Message)
	{
		FCrowdyTeamError Error;
		Error.Code = ECrowdyTeamErrorCode::NotFound;
		Error.Message = Message;
		return Error;
	}

	FCrowdyTeamError KitSocialGoneError()
	{
		return MakeNetworkError<FCrowdyTeamError>(TEXT("The social kit shut down before the request finished."));
	}

	// The result handed back for a reply that belongs to an account that has since signed out.
	constexpr const TCHAR* KitSocialAccountChangedCode = TEXT("KIT_SOCIAL_ACCOUNT_CHANGED");

	bool KitSocialNoClient(const FCrowdyCppJsonResult& Result, FCrowdyTeamError& OutError)
	{
		if (Result.ErrorCode == KitSocialAccountChangedCode)
		{
			OutError = MakeNetworkError<FCrowdyTeamError>(TEXT("The signed-in account changed before the request finished."));
			return true;
		}
		if (Result.ErrorCode != KitSocialNoClientCode)
		{
			return false;
		}
		OutError = ClientUnavailableError<FCrowdyTeamError>();
		return true;
	}

	template <typename ValueType>
	bool KitSocialReadObject(const FCrowdyCppJsonResult& Result, const TCHAR* FieldName, ValueType& Out, FCrowdyTeamError& OutError)
	{
		return !KitSocialNoClient(Result, OutError) && ReadObject(Result, FieldName, Out, OutError);
	}

	template <typename ValueType>
	bool KitSocialReadArray(const FCrowdyCppJsonResult& Result, const TCHAR* FieldName, TArray<ValueType>& Out, FCrowdyTeamError& OutError)
	{
		return !KitSocialNoClient(Result, OutError) && ReadArray(Result, FieldName, Out, OutError);
	}

	bool KitSocialAccept(const FCrowdyCppJsonResult& Result, const TCHAR* FieldName, FCrowdyTeamError& OutError)
	{
		if (KitSocialNoClient(Result, OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Payload = ReadPayload(Result, OutError);
		if (!Payload)
		{
			return false;
		}

		if (FieldName && !(*Payload)->HasTypedField<EJson::Object>(FieldName))
		{
			OutError = UnreadableResponseError<FCrowdyTeamError>();
			return false;
		}

		return true;
	}

	bool KitSocialNamed(const FString& Candidate, const FString& FullName)
	{
		return Candidate.Equals(FullName, ESearchCase::CaseSensitive);
	}

	TSharedPtr<FJsonObject> KitSocialAppVariables(int64 AppId)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), BigInt(AppId));
		return Variables;
	}

	TSharedPtr<FJsonObject> KitSocialMemberVariables(int64 GroupId, int64 UserId = 0)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("groupId"), BigInt(GroupId));
		if (UserId != 0)
		{
			Variables->SetStringField(TEXT("userId"), BigInt(UserId));
		}
		return Variables;
	}
}

void UCrowdyKitSocial::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);

	Collection.InitializeDependency<UCrowdyTeams>();
	if (UCrowdyChannels* Channels = Collection.InitializeDependency<UCrowdyChannels>())
	{
		Channels->OnChannelMessageReceived.AddDynamic(this, &UCrowdyKitSocial::HandleChannelMessage);
	}
}

void UCrowdyKitSocial::Deinitialize()
{
	LiveSessionToken.Reset();

	if (UCrowdyChannels* Channels = ResolveChannels())
	{
		Channels->OnChannelMessageReceived.RemoveAll(this);
	}

	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyKitSocial::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, UCrowdyTeams* Teams, UCrowdyChannels* Channels)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	TeamsForTest = Teams;
	ChannelsForTest = Channels;
}
#endif

int64 UCrowdyKitSocial::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

FCrowdyCppClient* UCrowdyKitSocial::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), KitSocialLogName);
}

UCrowdyTeams* UCrowdyKitSocial::ResolveTeams() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyTeams* Override = TeamsForTest.Get())
	{
		return Override;
	}
#endif
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyTeams>() : nullptr;
}

UCrowdyChannels* UCrowdyKitSocial::ResolveChannels() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyChannels* Override = ChannelsForTest.Get())
	{
		return Override;
	}
#endif
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyChannels>() : nullptr;
}

int64 UCrowdyKitSocial::ResolveUserId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UserIdForTest.IsSet())
	{
		return UserIdForTest.GetValue();
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session ? Session->GetUserID() : 0;
}

FString UCrowdyKitSocial::ResolveSenderUuid() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	if (!Session)
	{
		return FString();
	}

	return SenderUuidFor(Session->GetUUID(), Session->GetUserID());
}

FString UCrowdyKitSocial::SenderUuidFor(const FString& SessionUuid, int64 UserId)
{
	FCrowdyActorId Id;
	if (FCrowdyActorId::TryFromString(SessionUuid, Id) && !SessionUuid.Equals(FGuid().ToString(EGuidFormats::Digits)))
	{
		return SessionUuid;
	}
	return UserId > 0 ? UHelperFunctions::GetDeterministicID(UserId).ToString(EGuidFormats::Digits) : FString();
}

void UCrowdyKitSocial::SyncAccount()
{
	const int64 UserId = ResolveUserId();
	if (UserId == BoundUserId)
	{
		return;
	}

	BoundUserId = UserId;
	++AccountEpoch;
	KnownGroupsByChannel.Reset();
	JoinedChannelIds.Reset();
}

bool UCrowdyKitSocial::IsInChannel(int64 ChannelId, const UCrowdyChannels* Channels) const
{
	if (!Channels || !Channels->HasCachedChannels())
	{
		return JoinedChannelIds.Contains(ChannelId);
	}

	FCrowdyChannelMembership Membership;
	return Channels->GetMyChannelById(ChannelId, Membership);
}

TArray<FString> UCrowdyKitSocial::DefaultTerritoryPermissionKeys()
{
	return { TEXT("access"), TEXT("update_voxel_data") };
}

void UCrowdyKitSocial::Send(EKitRoute Route, const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(FCrowdyCppJsonResult)> OnReply)
{
	SyncAccount();

	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	const uint32 Epoch = AccountEpoch;
	OnReply = [WeakThis, Epoch, Inner = MoveTemp(OnReply)](FCrowdyCppJsonResult Result)
	{
		UCrowdyKitSocial* Self = WeakThis.Get();
		if (Self)
		{
			Self->SyncAccount();
		}
		if (Self && Self->AccountEpoch != Epoch)
		{
			Result = FCrowdyCppJsonResult();
			Result.ErrorCode = KitSocialAccountChangedCode;
		}
		Inner(MoveTemp(Result));
	};

	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FCrowdyCppJsonResult Unavailable;
		Unavailable.ErrorCode = KitSocialNoClientCode;
		OnReply(MoveTemp(Unavailable));
		return;
	}

	ECrowdyCppApiDomain Domain = ECrowdyCppApiDomain::Teams;
	TOptional<ECrowdyCppTokenPlane> Plane;
	if (Route == EKitRoute::Channels)
	{
		Domain = ECrowdyCppApiDomain::Channels;
	}
	else if (Route == EKitRoute::GameApps)
	{
		Domain = ECrowdyCppApiDomain::GameApps;
		Plane = ECrowdyCppTokenPlane::Game;
	}

	Client->RunOp(Domain, Operation, Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, MoveTemp(OnReply)), Plane);
}

void UCrowdyKitSocial::RunSteps(TArray<FKitStep> Steps, int32 Index, const FCrowdyKitGroup& Group, EKitOutcome Outcome,
	FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!Steps.IsValidIndex(Index))
	{
		Settle(Group, Outcome);
		OnDone.ExecuteIfBound(Group);
		return;
	}

	const FKitStep Step = Steps[Index];
	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(Step.Route, Step.Operation, Step.Variables,
		[WeakThis, Steps = MoveTemp(Steps), Index, Step, Group, Outcome, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeamError Error;
			const bool bAccepted = KitSocialAccept(Result, Step.ResultField, Error);
			const bool bRepeat = Error.Code == ECrowdyTeamErrorCode::AlreadyMember || Error.Code == ECrowdyTeamErrorCode::NotMember;
			if (!bAccepted && !(Step.bTolerateRepeat && bRepeat))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			UCrowdyKitSocial* Self = WeakThis.Get();
			if (!Self)
			{
				OnFail.ExecuteIfBound(KitSocialGoneError());
				return;
			}
			Self->RunSteps(Steps, Index + 1, Group, Outcome, OnDone, OnFail);
		});
}

void UCrowdyKitSocial::Settle(const FCrowdyKitGroup& Group, EKitOutcome Outcome)
{
	if (Outcome == EKitOutcome::None)
	{
		return;
	}

	if (Group.ChannelId != 0 && Outcome == EKitOutcome::Joined)
	{
		KnownGroupsByChannel.Add(Group.ChannelId, Group);
		JoinedChannelIds.Add(Group.ChannelId);
	}
	else if (Group.ChannelId != 0)
	{
		JoinedChannelIds.Remove(Group.ChannelId);
	}

	RefreshCaches();
}

void UCrowdyKitSocial::RefreshCaches()
{
	if (UCrowdyTeams* Teams = ResolveTeams())
	{
		Teams->RefreshMyTeams();
	}
	if (UCrowdyChannels* Channels = ResolveChannels())
	{
		Channels->RefreshMyChannels();
	}
}

bool UCrowdyKitSocial::RequireTeam(const FCrowdyKitGroup& Group, const FCrowdyKitFailed& OnFail) const
{
	if (Group.TeamId != 0)
	{
		return true;
	}
	OnFail.ExecuteIfBound(KitSocialNotFound(FString::Printf(TEXT("The group '%s' has no team."), *Group.Name)));
	return false;
}

void UCrowdyKitSocial::CreatePair(const FString& FullName, ECrowdyTeamMembershipPolicy Policy, const FString& Description,
	FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("name"), FullName);
	Input->SetStringField(TEXT("description"), Description);
	Input->SetStringField(TEXT("membershipPolicy"), FCrowdyTeam::MembershipPolicyToString(Policy));

	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Teams, TEXT("CreateTeam"), WrapInput(Input),
		[WeakThis, FullName, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			FCrowdyTeam Team;
			FCrowdyTeamError Error;
			if (!KitSocialReadObject(Result, TEXT("createTeam"), Team, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			UCrowdyKitSocial* Self = WeakThis.Get();
			if (!Self)
			{
				OnFail.ExecuteIfBound(KitSocialGoneError());
				return;
			}
			Self->CreatePairChannel(FCrowdyKitGroup(FullName, Team.TeamId, 0, Team.OwnerUserId), OnDone, OnFail);
		});
}

void UCrowdyKitSocial::CreatePairChannel(const FCrowdyKitGroup& Team, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("name"), Team.Name);
	Input->SetStringField(TEXT("description"), TEXT("Chat for ") + Team.Name);

	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Channels, TEXT("CreateChannel"), WrapInput(Input),
		[WeakThis, Team, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannel Channel;
			FCrowdyTeamError Error;
			UCrowdyKitSocial* Self = WeakThis.Get();
			if (!KitSocialReadObject(Result, TEXT("createChannel"), Channel, Error))
			{
				if (Self)
				{
					Self->RefreshCaches();
				}
				OnFail.ExecuteIfBound(Error);
				return;
			}

			const FCrowdyKitGroup Group(Team.Name, Team.TeamId, Channel.ChannelId, Team.OwnerUserId);
			if (Self)
			{
				Self->Settle(Group, EKitOutcome::Joined);
			}
			OnDone.ExecuteIfBound(Group);
		});
}

void UCrowdyKitSocial::FindPair(const FString& FullName, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Teams, TEXT("Teams"), KitSocialAppVariables(GetAppId()),
		[WeakThis, FullName, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeam> Teams;
			FCrowdyTeamError Error;
			if (!KitSocialReadArray(Result, TEXT("teams"), Teams, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			const FCrowdyTeam* Match = Teams.FindByPredicate(
				[&FullName](const FCrowdyTeam& Team) { return KitSocialNamed(Team.Name, FullName); });
			if (!Match)
			{
				OnFail.ExecuteIfBound(KitSocialNotFound(FString::Printf(TEXT("No group named '%s' was found."), *FullName)));
				return;
			}

			UCrowdyKitSocial* Self = WeakThis.Get();
			if (!Self)
			{
				OnFail.ExecuteIfBound(KitSocialGoneError());
				return;
			}
			Self->FindPairChannel(FCrowdyKitGroup(FullName, Match->TeamId, 0, Match->OwnerUserId), OnDone, OnFail);
		});
}

void UCrowdyKitSocial::FindPairChannel(const FCrowdyKitGroup& Team, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Channels, TEXT("Channels"), KitSocialAppVariables(GetAppId()),
		[WeakThis, Team, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannel> Channels;
			FCrowdyTeamError Error;
			if (!KitSocialReadArray(Result, TEXT("channels"), Channels, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			// A channel named like the team but owned by someone else was squatted, and its chat is not the group's.
			const FCrowdyChannel* Match = Channels.FindByPredicate(
				[&Team](const FCrowdyChannel& Channel)
				{
					return KitSocialNamed(Channel.Name, Team.Name) && Channel.OwnerUserId != 0 && Channel.OwnerUserId == Team.OwnerUserId;
				});
			const FCrowdyKitGroup Group(Team.Name, Team.TeamId, Match ? Match->ChannelId : 0, Team.OwnerUserId);
			if (UCrowdyKitSocial* Self = WeakThis.Get(); Self && Group.ChannelId != 0)
			{
				Self->KnownGroupsByChannel.Add(Group.ChannelId, Group);
			}
			OnDone.ExecuteIfBound(Group);
		});
}

void UCrowdyKitSocial::AddToPair(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::Teams, TEXT("AddTeamMember"), TEXT("addTeamMember"), KitSocialMemberVariables(Group.TeamId, UserId), true});
	if (Group.ChannelId != 0)
	{
		Steps.Add(FKitStep{EKitRoute::Channels, TEXT("AddChannelMember"), TEXT("addChannelMember"), KitSocialMemberVariables(Group.ChannelId, UserId), true});
	}
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::None, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::GetMembers(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	Send(EKitRoute::Teams, TEXT("TeamMembers"), KitSocialMemberVariables(Group.TeamId),
		[Group, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyTeamMember> Members;
			FCrowdyTeamError Error;
			if (!KitSocialReadArray(Result, TEXT("teamMembers"), Members, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}
			OnDone.ExecuteIfBound(Group, Members);
		});
}

void UCrowdyKitSocial::JoinChatRoom(const FCrowdyKitGroup& Room, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	SyncAccount();
	if (IsInChannel(Room.ChannelId, ResolveChannels()))
	{
		Settle(Room, EKitOutcome::Joined);
		OnDone.ExecuteIfBound(Room);
		return;
	}

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::Channels, TEXT("JoinChannel"), TEXT("joinChannel"), KitSocialMemberVariables(Room.ChannelId), true});
	RunSteps(MoveTemp(Steps), 0, Room, EKitOutcome::Joined, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::CreateChatRoom(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("name"), Name);

	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Channels, TEXT("CreateChannel"), WrapInput(Input),
		[WeakThis, Name, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			FCrowdyChannel Channel;
			FCrowdyTeamError Error;
			if (!KitSocialReadObject(Result, TEXT("createChannel"), Channel, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			const FCrowdyKitGroup Room(Name, 0, Channel.ChannelId, Channel.OwnerUserId);
			if (UCrowdyKitSocial* Self = WeakThis.Get())
			{
				Self->Settle(Room, EKitOutcome::Joined);
			}
			OnDone.ExecuteIfBound(Room);
		});
}

void UCrowdyKitSocial::CreateParty(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	CreatePair(FString(PartyPrefix) + Name, ECrowdyTeamMembershipPolicy::Invite, TEXT("Party"), MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::FindParty(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	FindPair(FString(PartyPrefix) + Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::InviteToParty(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	AddToPair(Group, UserId, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::JoinParty(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::Teams, TEXT("JoinTeam"), TEXT("joinTeam"), KitSocialMemberVariables(Group.TeamId), true});
	if (Group.ChannelId != 0)
	{
		Steps.Add(FKitStep{EKitRoute::Channels, TEXT("JoinChannel"), TEXT("joinChannel"), KitSocialMemberVariables(Group.ChannelId), true});
	}
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::Joined, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::LeaveParty(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TArray<FKitStep> Steps;
	if (Group.ChannelId != 0)
	{
		Steps.Add(FKitStep{EKitRoute::Channels, TEXT("LeaveChannel"), nullptr, KitSocialMemberVariables(Group.ChannelId), true});
	}
	Steps.Add(FKitStep{EKitRoute::Teams, TEXT("LeaveTeam"), nullptr, KitSocialMemberVariables(Group.TeamId), true});
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::Left, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::GetPartyMembers(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail)
{
	GetMembers(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::CreateGuild(const FString& Name, ECrowdyTeamMembershipPolicy Policy, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	CreatePair(FString(GuildPrefix) + Name, Policy, TEXT("Guild"), MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::FindGuild(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	FindPair(FString(GuildPrefix) + Name, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::JoinGuild(const FCrowdyKitGroup& Group, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::Teams, TEXT("RequestToJoinTeam"), TEXT("requestToJoinTeam"), KitSocialMemberVariables(Group.TeamId), true});
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::None, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::AcceptIntoGuild(const FCrowdyKitGroup& Group, int64 UserId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	AddToPair(Group, UserId, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::GetGuildRoster(const FCrowdyKitGroup& Group, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail)
{
	GetMembers(Group, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::PromoteInGuild(const FCrowdyKitGroup& Group, int64 UserId, const TArray<int64>& RoleIds,
	FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TSharedPtr<FJsonObject> Input = KitSocialMemberVariables(Group.TeamId, UserId);
	TArray<TSharedPtr<FJsonValue>> RoleIdValues;
	for (const int64 RoleId : RoleIds)
	{
		RoleIdValues.Add(MakeShared<FJsonValueString>(BigInt(RoleId)));
	}
	Input->SetArrayField(TEXT("roleIds"), RoleIdValues);

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::Teams, TEXT("SetTeamMemberRoles"), TEXT("setTeamMemberRoles"), WrapInput(Input)});
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::None, MoveTemp(OnDone), MoveTemp(OnFail));
}

void UCrowdyKitSocial::FindOrCreateChatRoom(const FString& Name, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	TWeakObjectPtr<UCrowdyKitSocial> WeakThis(this);
	Send(EKitRoute::Channels, TEXT("Channels"), KitSocialAppVariables(GetAppId()),
		[WeakThis, Name, OnDone, OnFail](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyChannel> Channels;
			FCrowdyTeamError Error;
			if (!KitSocialReadArray(Result, TEXT("channels"), Channels, Error))
			{
				OnFail.ExecuteIfBound(Error);
				return;
			}

			UCrowdyKitSocial* Self = WeakThis.Get();
			if (!Self)
			{
				OnFail.ExecuteIfBound(KitSocialGoneError());
				return;
			}

			const FCrowdyChannel* Match = Channels.FindByPredicate(
				[&Name](const FCrowdyChannel& Channel) { return KitSocialNamed(Channel.Name, Name); });
			if (!Match)
			{
				Self->CreateChatRoom(Name, OnDone, OnFail);
				return;
			}
			Self->JoinChatRoom(FCrowdyKitGroup(Name, 0, Match->ChannelId, Match->OwnerUserId), OnDone, OnFail);
		});
}

void UCrowdyKitSocial::ClaimGuildTerritory(const FCrowdyKitGroup& Group, int64 GridId, const TArray<FString>& PermissionKeys,
	int64 RoleId, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail)
{
	if (!RequireTeam(Group, OnFail))
	{
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("gridId"), BigInt(GridId));
	Input->SetStringField(TEXT("groupId"), BigInt(Group.TeamId));
	if (RoleId != 0)
	{
		Input->SetStringField(TEXT("groupRoleId"), BigInt(RoleId));
	}

	TArray<TSharedPtr<FJsonValue>> KeyValues;
	for (const FString& Key : PermissionKeys.IsEmpty() ? DefaultTerritoryPermissionKeys() : PermissionKeys)
	{
		KeyValues.Add(MakeShared<FJsonValueString>(Key));
	}
	Input->SetArrayField(TEXT("permissionKeys"), KeyValues);

	TArray<FKitStep> Steps;
	Steps.Add(FKitStep{EKitRoute::GameApps, TEXT("AssignGroupToGrid"), nullptr, WrapInput(Input)});
	RunSteps(MoveTemp(Steps), 0, Group, EKitOutcome::None, MoveTemp(OnDone), MoveTemp(OnFail));
}

ECrowdyKitChatResult UCrowdyKitSocial::CheckChat(const FString& Text, int64 ChannelId, bool bConnected)
{
	if (Text.IsEmpty())
	{
		return ECrowdyKitChatResult::EmptyText;
	}

	const FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() > MaxChatBytes)
	{
		return ECrowdyKitChatResult::TooLong;
	}
	if (ChannelId == 0)
	{
		return ECrowdyKitChatResult::NoChannel;
	}
	return bConnected ? ECrowdyKitChatResult::Sent : ECrowdyKitChatResult::NotConnected;
}

ECrowdyKitChatResult UCrowdyKitSocial::SendChat(const FCrowdyKitGroup& Group, const FString& Text)
{
	UCrowdyChannels* Channels = ResolveChannels();
	const bool bConnected = Channels && Channels->CanPublishChannelMessage();
	const ECrowdyKitChatResult Verdict = CheckChat(Text, Group.ChannelId, bConnected);
	if (Verdict != ECrowdyKitChatResult::Sent)
	{
		return Verdict;
	}

	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Channels->PublishChannelMessageAs(Group.ChannelId, ResolveSenderUuid(), Payload);
	return Verdict;
}

FString UCrowdyKitSocial::DescribeChatResult(ECrowdyKitChatResult Result)
{
	switch (Result)
	{
	case ECrowdyKitChatResult::Sent:
		return TEXT("The message left this client. The server delivers it to the channel's members; a refusal arrives later on On Send Refused.");
	case ECrowdyKitChatResult::EmptyText:
		return TEXT("There is no text to send.");
	case ECrowdyKitChatResult::TooLong:
		return FString::Printf(TEXT("A chat message can be at most %d bytes of UTF-8."), MaxChatBytes);
	case ECrowdyKitChatResult::NoChannel:
		return TEXT("The group has no chat channel.");
	case ECrowdyKitChatResult::NotConnected:
		return TEXT("The realtime connection is not up.");
	}
	return FString();
}

FCrowdyKitGroup UCrowdyKitSocial::ResolveChannelGroup(int64 ChannelId) const
{
	if (const FCrowdyKitGroup* Known = KnownGroupsByChannel.Find(ChannelId))
	{
		return *Known;
	}

	FCrowdyKitGroup Group;
	Group.ChannelId = ChannelId;

	const UCrowdyChannels* Channels = ResolveChannels();
	FCrowdyChannelMembership Membership;
	if (!Channels || !Channels->GetMyChannelById(ChannelId, Membership))
	{
		return Group;
	}
	Group.Name = Membership.Channel.Name;

	const UCrowdyTeams* Teams = ResolveTeams();
	const bool bPairName = Group.Name.StartsWith(PartyPrefix, ESearchCase::CaseSensitive)
		|| Group.Name.StartsWith(GuildPrefix, ESearchCase::CaseSensitive);
	if (!Teams || !bPairName)
	{
		return Group;
	}

	for (const FCrowdyTeamMembership& Mine : Teams->GetCachedMyTeams())
	{
		if (KitSocialNamed(Mine.Team.Name, Group.Name))
		{
			Group.TeamId = Mine.Team.TeamId;
			break;
		}
	}
	return Group;
}

void UCrowdyKitSocial::HandleChannelMessage(int64 ChannelId, FString SenderUUID, const TArray<uint8>& Payload)
{
	if (!OnChatMessage.IsBound())
	{
		return;
	}

	SyncAccount();
	const UCrowdyChannels* Channels = ResolveChannels();
	if (Channels && ChannelId != 0 && ChannelId == Channels->GetSessionChannelId())
	{
		return;
	}

	const FCrowdyKitGroup Group = ResolveChannelGroup(ChannelId);
	const bool bOwned = KnownGroupsByChannel.Contains(ChannelId) || JoinedChannelIds.Contains(ChannelId)
		|| Group.Name.StartsWith(PartyPrefix, ESearchCase::CaseSensitive)
		|| Group.Name.StartsWith(GuildPrefix, ESearchCase::CaseSensitive);
	if (!bOwned || Payload.IsEmpty())
	{
		return;
	}

	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
	OnChatMessage.Broadcast(Group, SenderUUID, FString(Converted.Length(), Converted.Get()));
}
