// Fill out your copyright notice in the Description page of Project Settings.

#include "Model/FCrowdyStudioController.h"

#include "CrowdyStudioModule.h"
#include "Auth/FCrowdyTokenVault.h"
#include "ConfigSync/FCrowdyConfigSync.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonSerializer.h"
#include "Network/GraphQL/FCrowdyGraphQLClient.h"
#include "Settings/CrowdyStudioUserSettings.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Web/FCrowdyStudioLinks.h"

namespace
{
	// BigInt travels as a decimal string on the wire, never a JSON number.
	void SetBigIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64 Value)
	{
		Object->SetStringField(Field, FString::Printf(TEXT("%lld"), Value));
	}

	// A nullable Int cap: a positive value is sent as a JSON number; 0 (unlimited) is sent as JSON null
	// so the server clears any existing cap instead of treating it as a real zero.
	void SetCapField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Value)
	{
		if (Value > 0)
		{
			Object->SetNumberField(Field, Value);
		}
		else
		{
			Object->SetField(Field, MakeShared<FJsonValueNull>());
		}
	}

	void SetStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Value : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Items);
	}

	// Optional string inputs are dropped when empty so an upsert leaves the server's existing value
	// alone instead of blanking it.
	void SetOptionalStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Object->SetStringField(Field, Value);
		}
	}

	bool ParseJsonArray(const FString& JsonText, TArray<TSharedPtr<FJsonValue>>& OutArray)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutArray);
	}

	bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	// The operation name sits between the "query"/"mutation" keyword and the opening brace; enough
	// to identify a traced call without dumping the whole body (and the body never holds the token).
	FString DescribeQuery(const FString& Query)
	{
		FString Header = Query.TrimStart();
		int32 BracePos = INDEX_NONE;
		if (Header.FindChar(TEXT('{'), BracePos))
		{
			Header = Header.Left(BracePos);
		}
		return Header.TrimEnd();
	}

	// One place that turns a GraphQL result into a user-facing error, empty when the call succeeded.
	// The HTTP status separates the cases the UI treats differently: a bad/expired token (401, sign
	// in again), a token that authenticated but lacks the permission (403), a transport failure, and
	// a plain GraphQL validation/business error from the errors array.
	FString DescribeGqlError(const FCrowdyGqlResult& Result)
	{
		if (Result.HttpCode == 0)
		{
			return TEXT("Network error: the request never reached the server.");
		}
		if (Result.HttpCode == 401)
		{
			return TEXT("Authentication failed (HTTP 401). Sign in again.");
		}
		if (Result.HttpCode == 403)
		{
			return TEXT("Permission denied (HTTP 403). The signed-in token can't perform this action.");
		}
		if (Result.Errors.Num() > 0)
		{
			return Result.Errors[0];
		}
		if (!Result.bSuccess)
		{
			return FString::Printf(TEXT("Server returned HTTP %d."), Result.HttpCode);
		}
		return FString();
	}
}

FCrowdyStudioController::FCrowdyStudioController()
{
}

void FCrowdyStudioController::Initialize()
{
	if (const UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		SelectedOrgId = User->LastOrgId;
		SelectedAppId = User->LastAppId;
	}

	FString Saved;
	if (FCrowdyTokenVault::Load(Saved) && !Saved.IsEmpty())
	{
		SignInWithToken(Saved);
	}
}



void FCrowdyStudioController::SignInWithToken(const FString& OrgToken)
{
	if (OrgToken.IsEmpty())
	{
		SetStatus(TEXT("Enter an organization token to sign in."), true);
		return;
	}

	AuthToken = OrgToken;
	AuthScope = ECrowdyStudioAuthScope::OrgToken;

	// The token proves itself by listing the organizations it can see.
	SendManagement(CrowdyStudioGql::MyOrganizationsQuery(), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseOrganizations(Envelope, Organizations);
			bSignedIn = true;
			FCrowdyTokenVault::Save(AuthToken);
			if (UCrowdyStudioUserSettings* User = GetUserSettings())
			{
				User->bRememberToken = true;
				User->SaveConfig();
			}
			SetStatus(FString::Printf(TEXT("Signed in — %d organization(s)."), Organizations.Num()), false);
			OnSignInStateChanged.Broadcast();
			OnOrganizationsChanged.Broadcast();
			FetchAppsAndEnvironments();
		},
		[this]()
		{
			AuthToken.Empty();
			AuthScope = ECrowdyStudioAuthScope::None;
			bSignedIn = false;
			FCrowdyTokenVault::Clear();
			OnSignInStateChanged.Broadcast();
		});
}

void FCrowdyStudioController::LoginWithEmail(const FString& Email, const FString& Password)
{
	if (Email.IsEmpty() || Password.IsEmpty())
	{
		SetStatus(TEXT("Enter both an email and a password."), true);
		return;
	}

	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("email"), Email);
	Input->SetStringField(TEXT("password"), Password);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("loginUserInput"), Input);

	SendManagement(CrowdyStudioGql::LoginMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FString Token;
			int64 SignedInUserId = 0;
			if (!CrowdyStudioGql::ParseLogin(Envelope, Token, SignedInUserId))
			{
				SetStatus(TEXT("Login succeeded but no session token came back."), true);
				return;
			}

			AuthToken = Token;
			AuthScope = ECrowdyStudioAuthScope::Session;
			UserId = SignedInUserId;
			bSignedIn = true;
			FCrowdyTokenVault::Save(AuthToken);
			if (UCrowdyStudioUserSettings* User = GetUserSettings())
			{
				User->bRememberToken = true;
				User->SaveConfig();
			}
			SetStatus(TEXT("Logged in."), false);
			OnSignInStateChanged.Broadcast();
			FetchMyOrganizations();
			FetchAppsAndEnvironments();
		});
}

void FCrowdyStudioController::SignOut()
{
	FCrowdyTokenVault::Clear();

	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;
	bSignedIn = false;
	UserId = 0;

	Organizations.Reset();
	Apps.Reset();
	Environments.Reset();
	SelectedOrgId = 0;
	SelectedAppId = 0;
	SelectedEnvironmentSlug.Empty();

	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->bRememberToken = false;
		User->SaveConfig();
	}

	SetStatus(TEXT("Signed out."), false);
	OnSignInStateChanged.Broadcast();
	OnOrganizationsChanged.Broadcast();
	OnAppsChanged.Broadcast();
	OnEnvironmentsChanged.Broadcast();
}



void FCrowdyStudioController::FetchMyOrganizations()
{
	SendManagement(CrowdyStudioGql::MyOrganizationsQuery(), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseOrganizations(Envelope, Organizations);
			SetStatus(FString::Printf(TEXT("%d organization(s)."), Organizations.Num()), false);
			OnOrganizationsChanged.Broadcast();
		});
}

void FCrowdyStudioController::CreateOrganization(const FString& Name, const FString& Slug)
{
	if (Name.IsEmpty() || Slug.IsEmpty())
	{
		SetStatus(TEXT("A new organization needs both a name and a slug."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("slug"), Slug);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(CrowdyStudioGql::CreateOrganizationMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioOrg> Created = CrowdyStudioGql::ParseOrganization(Envelope, TEXT("createOrganization"));
			SetStatus(Created.IsValid()
				? FString::Printf(TEXT("Created organization '%s'."), *Created->Name)
				: TEXT("Created organization."), false);
			FetchMyOrganizations();
		});
}



void FCrowdyStudioController::FetchApps()
{
	SendManagement(CrowdyStudioGql::MyAppsQuery(), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseApps(Envelope, Apps);
			SetStatus(FString::Printf(TEXT("%d app(s)."), Apps.Num()), false);
			OnAppsChanged.Broadcast();
		});
}

void FCrowdyStudioController::CreateApp(int64 OrgId, const FString& Name, const FString& Slug,
	const FString& Status, const FString& Visibility)
{
	if (Name.IsEmpty() || Slug.IsEmpty())
	{
		SetStatus(TEXT("A new app needs both a name and a slug."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("orgId"), FString::Printf(TEXT("%lld"), OrgId));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("slug"), Slug);
	// status/visibility are the AppStatus/AppVisibility enums — passed as their value names.
	if (!Status.IsEmpty())
	{
		Input->SetStringField(TEXT("status"), Status);
	}
	if (!Visibility.IsEmpty())
	{
		Input->SetStringField(TEXT("visibility"), Visibility);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(CrowdyStudioGql::CreateAppMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioApp> Created = CrowdyStudioGql::ParseApp(Envelope, TEXT("createApp"));
			SetStatus(Created.IsValid()
				? FString::Printf(TEXT("Created app '%s'."), *Created->Name)
				: TEXT("Created app."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::UpdateApp(int64 AppId, const FString& Name, const FString& Status, const FString& Visibility)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	if (!Name.IsEmpty())
	{
		Input->SetStringField(TEXT("name"), Name);
	}
	if (!Status.IsEmpty())
	{
		Input->SetStringField(TEXT("status"), Status);
	}
	if (!Visibility.IsEmpty())
	{
		Input->SetStringField(TEXT("visibility"), Visibility);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(CrowdyStudioGql::UpdateAppMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("App updated."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::ArchiveApp(int64 AppId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));

	SendManagement(CrowdyStudioGql::ArchiveAppMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("App archived."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::FetchApp(int64 AppId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));

	SendManagement(CrowdyStudioGql::AppQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioApp> Detail = CrowdyStudioGql::ParseApp(Envelope, TEXT("app"));
			if (!Detail.IsValid())
			{
				return;
			}

			// Fold the endpoint detail back into the list entry the views already show.
			bool bMerged = false;
			for (TSharedPtr<FStudioApp>& Existing : Apps)
			{
				if (Existing->AppId == Detail->AppId)
				{
					*Existing = *Detail;
					bMerged = true;
					break;
				}
			}
			if (!bMerged)
			{
				Apps.Add(Detail);
			}

			OnAppsChanged.Broadcast();
		});
}



void FCrowdyStudioController::FetchEnvironments(int64 OrgId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("orgId"), FString::Printf(TEXT("%lld"), OrgId));

	SendManagement(CrowdyStudioGql::OrgEnvironmentsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseEnvironments(Envelope, Environments);
			SetStatus(FString::Printf(TEXT("%d environment(s)."), Environments.Num()), false);
			OnEnvironmentsChanged.Broadcast();
		});
}

void FCrowdyStudioController::LinkAppToEnvironment(int64 AppId, const FString& EnvironmentSlug)
{
	if (AppId == 0 || EnvironmentSlug.IsEmpty())
	{
		SetStatus(TEXT("Pick both an app and an environment to link."), true);
		return;
	}

	const int64 OrgId = SelectedOrgId;

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("orgId"), FString::Printf(TEXT("%lld"), OrgId));
	Input->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));
	Input->SetStringField(TEXT("environmentSlug"), EnvironmentSlug);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(CrowdyStudioGql::LinkAppToEnvironmentMutation(), Variables,
		[this, AppId, OrgId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Linked app to environment."), false);
			FetchApp(AppId);
			FetchEnvironments(OrgId);
		});
}



void FCrowdyStudioController::SyncConfig()
{
	const TSharedPtr<FStudioApp> App = GetSelectedApp();
	if (!App.IsValid())
	{
		SetStatus(TEXT("Select an app before syncing to the project."), true);
		return;
	}

	FCrowdyConfigSync::ApplyAppToSettings(*App);
	const int32 LiveSessions = FCrowdyConfigSync::ApplyToRunningSessions();

	SetStatus(LiveSessions > 0
		? FString::Printf(TEXT("Synced to project settings and applied live to %d running session(s)."), LiveSessions)
		: TEXT("Synced to project settings. Takes effect on the next Play — no editor restart needed."), false);
	OnConfigChanged.Broadcast();
}

FStudioSettingsSnapshot FCrowdyStudioController::GetCurrentSettings() const
{
	return FCrowdyConfigSync::ReadCurrentSettings();
}

FStudioSettingsSnapshot FCrowdyStudioController::BuildProposedSettings() const
{
	// The single definition of what a sync writes lives in FCrowdyConfigSync, so the diff shown
	// here and the values actually written can't drift. With no app selected, nothing changes.
	if (const TSharedPtr<FStudioApp> App = GetSelectedApp())
	{
		return FCrowdyConfigSync::BuildProposedSettings(*App);
	}
	return GetCurrentSettings();
}

FString FCrowdyStudioController::GetBackendMode() const
{
	return FCrowdyConfigSync::GetBackendMode();
}

void FCrowdyStudioController::SetBackendMode(const FString& Mode)
{
	FCrowdyConfigSync::SetBackendMode(Mode);
	SetStatus(TEXT("Backend changed. If your apps don't load, sign in again for this backend."), false);
	OnConfigChanged.Broadcast();
}

FString FCrowdyStudioController::GetCustomManagementUrl() const
{
	return FCrowdyConfigSync::GetCustomManagementUrl();
}

void FCrowdyStudioController::SetCustomManagementUrl(const FString& Url)
{
	FCrowdyConfigSync::SetCustomManagementUrl(Url);
	OnConfigChanged.Broadcast();
}

FString FCrowdyStudioController::GetEffectiveManagementUrl() const
{
	return FCrowdyConfigSync::GetEffectiveManagementUrl();
}


void FCrowdyStudioController::FetchTeams()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list teams."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(CrowdyStudioGql::TeamsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroups(Envelope, TEXT("teams"), Teams);
			SetStatus(FString::Printf(TEXT("%d team(s)."), Teams.Num()), false);
			OnTeamsChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchTeamPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its team policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(CrowdyStudioGql::TeamPolicyQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("teamPolicy"), TeamPolicy);
			OnTeamPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetTeamPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting team policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));
	Variables->SetStringField(TEXT("creationPolicy"), CreationPolicy);
	Variables->SetStringField(TEXT("defaultMembershipPolicy"), DefaultMembershipPolicy);
	SetCapField(Variables, TEXT("maxMembers"), MaxMembers);
	SetCapField(Variables, TEXT("maxGroupsPerUser"), MaxGroupsPerUser);

	SendGame(CrowdyStudioGql::SetTeamPolicyMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("setTeamPolicy"), TeamPolicy);
			SetStatus(TEXT("Team policy updated."), false);
			OnTeamPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchChannels()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list channels."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(CrowdyStudioGql::ChannelsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroups(Envelope, TEXT("channels"), Channels);
			SetStatus(FString::Printf(TEXT("%d channel(s)."), Channels.Num()), false);
			OnChannelsChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchChannelPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its channel policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(CrowdyStudioGql::ChannelPolicyQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("channelPolicy"), ChannelPolicy);
			OnChannelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetChannelPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting channel policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));
	Variables->SetStringField(TEXT("creationPolicy"), CreationPolicy);
	Variables->SetStringField(TEXT("defaultMembershipPolicy"), DefaultMembershipPolicy);
	SetCapField(Variables, TEXT("maxMembers"), MaxMembers);
	SetCapField(Variables, TEXT("maxGroupsPerUser"), MaxGroupsPerUser);

	SendGame(CrowdyStudioGql::SetChannelPolicyMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("setChannelPolicy"), ChannelPolicy);
			SetStatus(TEXT("Channel policy updated."), false);
			OnChannelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::CreateChannel(const FString& Name, const FString& Description, bool bMembersCanSend, const FString& MembershipPolicy)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A new channel needs a selected app and a name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));
	Variables->SetStringField(TEXT("name"), Name);
	Variables->SetStringField(TEXT("description"), Description);
	Variables->SetBoolField(TEXT("membersCanSend"), bMembersCanSend);
	// Empty membership policy lets the server fall back to the app's default channel policy.
	if (MembershipPolicy.IsEmpty())
	{
		Variables->SetField(TEXT("membershipPolicy"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Variables->SetStringField(TEXT("membershipPolicy"), MembershipPolicy);
	}

	SendGame(CrowdyStudioGql::CreateChannelMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Created channel."), false);
			FetchChannels();
		});
}

void FCrowdyStudioController::CreateTeam(const FString& Name, const FString& Description, const FString& MembershipPolicy)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A new team needs a selected app and a name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));
	Variables->SetStringField(TEXT("name"), Name);
	Variables->SetStringField(TEXT("description"), Description);
	if (MembershipPolicy.IsEmpty())
	{
		Variables->SetField(TEXT("membershipPolicy"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Variables->SetStringField(TEXT("membershipPolicy"), MembershipPolicy);
	}

	SendGame(CrowdyStudioGql::CreateTeamMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Created team."), false);
			FetchTeams();
		});
}

void FCrowdyStudioController::CreateSessionChannel()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before creating its session channel."), true);
		return;
	}

	// Must match the runtime's deterministic name (UCrowdyChannels::GetSessionChannelName) so every
	// client converges on the same channel: __crowdy_session_<appId>.
	const FString SessionName = FString::Printf(TEXT("__crowdy_session_%lld"), SelectedAppId);
	CreateChannel(SessionName, TEXT("Reliable-RPC session channel (auto)"), /*bMembersCanSend*/ true, TEXT("open"));
}

void FCrowdyStudioController::SelectGroup(ECrowdyGroupKind Kind, int64 GroupId)
{
	SelectedGroupKind = Kind;
	SelectedGroupId = GroupId;
	GroupMembers.Reset();
	GroupRoles.Reset();
	OnGroupDetailChanged.Broadcast();

	if (GroupId == 0)
	{
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TCHAR* MembersOp = bChannel ? TEXT("channelMembers") : TEXT("teamMembers");
	const TCHAR* RolesOp = bChannel ? TEXT("channelRoles") : TEXT("teamRoles");

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);

	SendGame(CrowdyStudioGql::GroupMembersQuery(bChannel), Vars,
		[this, GroupId, MembersOp](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedGroupId != GroupId)
			{
				return; // selection moved on before the reply arrived; drop the stale data
			}
			CrowdyStudioGql::ParseGroupMembers(Envelope, MembersOp, GroupMembers);
			OnGroupDetailChanged.Broadcast();
		});

	SendGame(CrowdyStudioGql::GroupRolesQuery(bChannel), Vars,
		[this, GroupId, RolesOp](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedGroupId != GroupId)
			{
				return;
			}
			CrowdyStudioGql::ParseGroupRoles(Envelope, RolesOp, GroupRoles);
			OnGroupDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::AddGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Adding a member needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);
	SetBigIntField(Vars, TEXT("userId"), InUserId);

	SendGame(CrowdyStudioGql::AddGroupMemberMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member added."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::RemoveGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Removing a member needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);
	SetBigIntField(Vars, TEXT("userId"), InUserId);

	SendGame(CrowdyStudioGql::RemoveGroupMemberMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member removed."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::SetGroupMemberRoles(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId, const TArray<int64>& RoleIds)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Setting roles needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	SetBigIntField(Input, TEXT("userId"), InUserId);

	TArray<TSharedPtr<FJsonValue>> Ids;
	for (int64 RoleId : RoleIds)
	{
		Ids.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), RoleId)));
	}
	Input->SetArrayField(TEXT("roleIds"), Ids);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::SetGroupMemberRolesMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member roles updated."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::CreateGroupRole(ECrowdyGroupKind Kind, int64 GroupId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank)
{
	if (GroupId == 0 || RoleName.IsEmpty())
	{
		SetStatus(TEXT("A new role needs a selected group and a role name."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	Input->SetStringField(TEXT("roleName"), RoleName);
	SetStringArrayField(Input, TEXT("permissions"), Permissions);
	Input->SetNumberField(TEXT("rank"), Rank);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::CreateGroupRoleMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role created."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::UpdateGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank)
{
	if (GroupRoleId == 0)
	{
		SetStatus(TEXT("Select a role to update."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	// roleName is ignored server-side for system roles; omit when empty to leave it unchanged.
	if (!RoleName.IsEmpty())
	{
		Input->SetStringField(TEXT("roleName"), RoleName);
	}
	SetStringArrayField(Input, TEXT("permissions"), Permissions);
	Input->SetNumberField(TEXT("rank"), Rank);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	const int64 GroupId = SelectedGroupId;
	SendGame(CrowdyStudioGql::UpdateGroupRoleMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role updated."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::DeleteGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId)
{
	if (GroupRoleId == 0)
	{
		SetStatus(TEXT("Select a role to delete."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupRoleId"), GroupRoleId);

	const int64 GroupId = SelectedGroupId;
	SendGame(CrowdyStudioGql::DeleteGroupRoleMutation(bChannel), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role deleted."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::DeleteGroup(ECrowdyGroupKind Kind, int64 GroupId)
{
	if (GroupId == 0)
	{
		SetStatus(TEXT("Select a team or channel to delete."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);

	SendGame(CrowdyStudioGql::DeleteGroupMutation(bChannel), Vars,
		[this, bChannel](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(bChannel ? TEXT("Channel deleted.") : TEXT("Team deleted."), false);

			// The selected group is gone: clear the shared detail panels and refresh the list.
			SelectedGroupId = 0;
			GroupMembers.Reset();
			GroupRoles.Reset();
			OnGroupDetailChanged.Broadcast();
			if (bChannel) { FetchChannels(); } else { FetchTeams(); }
		});
}

void FCrowdyStudioController::UpdateGroup(ECrowdyGroupKind Kind, int64 GroupId, const FString& Name, const FString& Description, const FString& MembershipPolicy)
{
	if (GroupId == 0)
	{
		SetStatus(TEXT("Select a team or channel to edit."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	// Partial update: only send the fields the user filled in; the server leaves omitted ones as-is.
	if (!Name.IsEmpty())             { Input->SetStringField(TEXT("name"), Name); }
	if (!Description.IsEmpty())      { Input->SetStringField(TEXT("description"), Description); }
	if (!MembershipPolicy.IsEmpty()) { Input->SetStringField(TEXT("membershipPolicy"), MembershipPolicy); }

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::UpdateGroupMutation(bChannel), Vars,
		[this, bChannel](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(bChannel ? TEXT("Channel updated.") : TEXT("Team updated."), false);
			if (bChannel) { FetchChannels(); } else { FetchTeams(); }
		});
}

void FCrowdyStudioController::FetchNearbyGrids(int64 InUserId, int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to scan for grids."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Low = MakeShared<FJsonObject>();
	SetBigIntField(Low, TEXT("x"), LowX);
	SetBigIntField(Low, TEXT("y"), LowY);
	SetBigIntField(Low, TEXT("z"), LowZ);

	const TSharedPtr<FJsonObject> High = MakeShared<FJsonObject>();
	SetBigIntField(High, TEXT("x"), HighX);
	SetBigIntField(High, TEXT("y"), HighY);
	SetBigIntField(High, TEXT("z"), HighZ);

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	Input->SetObjectField(TEXT("lowChunk"), Low);
	Input->SetObjectField(TEXT("highChunk"), High);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::NearbyGridPermissionsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseNearbyGrids(Envelope, NearbyGrids);
			SetStatus(FString::Printf(TEXT("%d grid(s) in region."), NearbyGrids.Num()), false);
			OnNearbyGridsChanged.Broadcast();
		});
}

namespace
{
	// Turn a createGrid UDP error code into a sentence a developer can act on. The server returns the
	// enum name (e.g. NO_MATCHING_GRID_ASSIGNMENT); unknown codes pass through unchanged.
	FString DescribeGridError(const FString& Code)
	{
		if (Code == TEXT("NO_MATCHING_GRID_ASSIGNMENT"))
		{
			return TEXT("NO_MATCHING_GRID_ASSIGNMENT - those chunk coordinates are not inside any of the app's world bounds (grid assignments). A new grid must fit within an assigned world region. Scan a region to find the app's default world-spanning grid and keep the corners inside it.");
		}
		if (Code == TEXT("GRID_OUTSIDE_ASSIGNMENT"))
		{
			return TEXT("GRID_OUTSIDE_ASSIGNMENT - the chunk range extends past the app's assigned world region. Shrink it so it fits entirely inside one.");
		}
		if (Code == TEXT("GRID_OVERLAPS_EXISTING"))
		{
			return TEXT("GRID_OVERLAPS_EXISTING - the chunk range overlaps an existing grid. Pick a range that does not overlap one.");
		}
		if (Code == TEXT("GRID_ALREADY_EXISTS"))
		{
			return TEXT("GRID_ALREADY_EXISTS - a grid already exists at these coordinates.");
		}
		if (Code == TEXT("INVALID_GRID_COORDINATES"))
		{
			return TEXT("INVALID_GRID_COORDINATES - the chunk coordinates are invalid.");
		}
		if (Code == TEXT("USER_NOT_APP_ADMIN"))
		{
			return TEXT("USER_NOT_APP_ADMIN - creating grids needs the manage_apps permission on the app's organization.");
		}
		return Code;
	}
}

void FCrowdyStudioController::CreateGrid(int64 C1X, int64 C1Y, int64 C1Z, int64 C2X, int64 C2Y, int64 C2Z)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before creating a grid."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Corner1 = MakeShared<FJsonObject>();
	SetBigIntField(Corner1, TEXT("x"), C1X);
	SetBigIntField(Corner1, TEXT("y"), C1Y);
	SetBigIntField(Corner1, TEXT("z"), C1Z);

	const TSharedPtr<FJsonObject> Corner2 = MakeShared<FJsonObject>();
	SetBigIntField(Corner2, TEXT("x"), C2X);
	SetBigIntField(Corner2, TEXT("y"), C2Y);
	SetBigIntField(Corner2, TEXT("z"), C2Z);

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetObjectField(TEXT("corner1"), Corner1);
	Input->SetObjectField(TEXT("corner2"), Corner2);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::CreateGridMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FStudioGrid Created;
			FString GridError;
			if (CrowdyStudioGql::ParseCreateGrid(Envelope, Created, GridError))
			{
				SetStatus(FString::Printf(TEXT("Created grid #%lld."), Created.GridId), false);
			}
			else
			{
				SetStatus(GridError.IsEmpty()
					? TEXT("Grid was not created.")
					: FString::Printf(TEXT("Grid not created: %s"), *DescribeGridError(GridError)), true);
			}
		});
}

void FCrowdyStudioController::FetchGridPermissionLimits(int64 GridId)
{
	if (SelectedAppId == 0 || GridId == 0)
	{
		SetStatus(TEXT("Select an app and a grid to read its whitelist."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);

	SendGame(CrowdyStudioGql::GridPermissionLimitsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("gridPermissionLimits"), GridWhitelistKeys);
			OnGridWhitelistChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetGridPermissionLimits(int64 GridId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0)
	{
		SetStatus(TEXT("Select an app and a grid to set its whitelist."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::SetGridPermissionLimitsMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("setGridPermissionLimits"), GridWhitelistKeys);
			SetStatus(TEXT("Grid whitelist updated."), false);
			OnGridWhitelistChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchGridGroupGrants(int64 GridId, int64 GroupId)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and group to list grants."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);
	SetBigIntField(Variables, TEXT("groupId"), GroupId);

	SendGame(CrowdyStudioGql::GridGroupGrantsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("gridGroupGrants"), GridGroupGrants);
			SetStatus(FString::Printf(TEXT("%d group grant(s)."), GridGroupGrants.Num()), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::AssignGroupToGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0 || PermissionKeys.Num() == 0)
	{
		SetStatus(TEXT("A group grant needs an app, grid, group, and at least one key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	if (bHasRole)
	{
		SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	}
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::AssignGroupToGridMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("assignGroupToGrid"), GridGroupGrants);
			SetStatus(TEXT("Granted permissions to group."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::RevokeGroupFromGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and group to revoke."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	if (bHasRole)
	{
		SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	}
	if (PermissionKeys.Num() > 0)
	{
		SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::RevokeGroupFromGridMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("revokeGroupFromGrid"), GridGroupGrants);
			SetStatus(TEXT("Revoked group grant."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchGridUserPermissions(int64 GridId, int64 InUserId)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and user to read effective permissions."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);
	SetBigIntField(Variables, TEXT("userId"), InUserId);

	SendGame(CrowdyStudioGql::GridUserPermissionsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("gridUserPermissions"), GridUserEffectiveKeys);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::GrantGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0 || PermissionKeys.Num() == 0)
	{
		SetStatus(TEXT("A user grant needs an app, grid, user, and at least one key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::GrantGridPermissionsMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("grantGridPermissions"), GridUserEffectiveKeys);
			SetStatus(TEXT("Granted permissions to user."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::RevokeGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and user to revoke."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	if (PermissionKeys.Num() > 0)
	{
		SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::RevokeGridPermissionsMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("revokeGridPermissions"), GridUserEffectiveKeys);
			SetStatus(TEXT("Revoked user grant."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchRuntimePermissions()
{
	// The catalog is global and PUBLIC, so there is no app or sign-in guard. It rides the management
	// endpoint with whatever token is set (ignored when absent), independent of the game-plane grid ops.
	SendManagement(CrowdyStudioGql::RuntimePermissionsQuery(), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseRuntimePermissions(Envelope, RuntimePermissions);
			OnRuntimePermissionsChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchContainerTypes()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list container types."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(CrowdyStudioGql::ContainerTypesQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseContainerTypes(Envelope, TEXT("gameModelContainerTypes"), ContainerTypes);
			SetStatus(FString::Printf(TEXT("%d container type(s)."), ContainerTypes.Num()), false);
			OnContainerTypesChanged.Broadcast();
		});
}

void FCrowdyStudioController::UpsertContainerType(const FString& TypeName, const FString& DisplayName, const FString& Description,
	const FString& InstantiableBy, const FString& DefaultPropertyVisibility)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty() || DisplayName.IsEmpty())
	{
		SetStatus(TEXT("A container type needs an app, type name, and display name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("typeName"), TypeName);
	Input->SetStringField(TEXT("displayName"), DisplayName);
	SetOptionalStringField(Input, TEXT("description"), Description);
	SetOptionalStringField(Input, TEXT("instantiableBy"), InstantiableBy);
	SetOptionalStringField(Input, TEXT("defaultPropertyVisibility"), DefaultPropertyVisibility);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::UpsertContainerTypeMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved container type."), false);
			FetchContainerTypes();
		});
}

void FCrowdyStudioController::FetchPropertyDefs(const FString& TypeName)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty())
	{
		PropertyDefs.Reset();
		OnPropertyDefsChanged.Broadcast();
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("typeName"), TypeName);

	SendGame(CrowdyStudioGql::PropertyDefsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParsePropertyDefs(Envelope, TEXT("gameModelPropertyDefs"), PropertyDefs);
			OnPropertyDefsChanged.Broadcast();
		});
}

void FCrowdyStudioController::UpsertPropertyDef(const FString& TypeName, const FString& Key, const FString& ValueType,
	const FString& DefaultValueJson, const FString& Visibility, const FString& Writable, const FString& Description)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty() || Key.IsEmpty() || ValueType.IsEmpty())
	{
		SetStatus(TEXT("A property needs an app, type, key, and value type."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("containerTypeName"), TypeName);
	Input->SetStringField(TEXT("key"), Key);
	Input->SetStringField(TEXT("valueType"), ValueType);
	SetOptionalStringField(Input, TEXT("defaultValueJson"), DefaultValueJson);
	SetOptionalStringField(Input, TEXT("visibility"), Visibility);
	SetOptionalStringField(Input, TEXT("writable"), Writable);
	SetOptionalStringField(Input, TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::UpsertPropertyDefMutation(), Variables,
		[this, TypeName](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved property."), false);
			FetchPropertyDefs(TypeName);
		});
}

void FCrowdyStudioController::FetchFunctions(const FString& ContainerTypeFilter)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list functions."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	if (ContainerTypeFilter.IsEmpty())
	{
		Variables->SetField(TEXT("containerTypeName"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Variables->SetStringField(TEXT("containerTypeName"), ContainerTypeFilter);
	}

	SendGame(CrowdyStudioGql::FunctionsQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseFunctions(Envelope, TEXT("gameModelFunctions"), Functions);
			SetStatus(FString::Printf(TEXT("%d function(s)."), Functions.Num()), false);
			OnFunctionsChanged.Broadcast();
		});
}

void FCrowdyStudioController::UpsertFunction(const FString& Name, const FString& ContainerTypeName, const FString& Description,
	const FString& ReturnType, const FString& InvokeScope, const FString& ReturnExpression,
	const FString& ParametersJson, const FString& MutationsJson, const FString& InvokePolicyJson)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A function needs an app and a name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("name"), Name);
	SetOptionalStringField(Input, TEXT("containerTypeName"), ContainerTypeName);
	SetOptionalStringField(Input, TEXT("description"), Description);
	SetOptionalStringField(Input, TEXT("returnType"), ReturnType);
	SetOptionalStringField(Input, TEXT("invokeScope"), InvokeScope);
	SetOptionalStringField(Input, TEXT("returnExpression"), ReturnExpression);

	// The editor saves the whole function each time, so an empty policy means "no requirements". Send
	// an explicit JSON null (not an omitted field) so the server clears any existing policy and the
	// runtime falls back to "anyone entitled may invoke", instead of keeping the previous policy.
	if (InvokePolicyJson.IsEmpty())
	{
		Input->SetField(TEXT("invokePolicyJson"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Input->SetStringField(TEXT("invokePolicyJson"), InvokePolicyJson);
	}

	if (!ParametersJson.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Params;
		if (!ParseJsonArray(ParametersJson, Params))
		{
			SetStatus(TEXT("Parameters must be a JSON array."), true);
			return;
		}
		Input->SetArrayField(TEXT("parameters"), Params);
	}

	if (!MutationsJson.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Muts;
		if (!ParseJsonArray(MutationsJson, Muts))
		{
			SetStatus(TEXT("Mutations must be a JSON array."), true);
			return;
		}
		Input->SetArrayField(TEXT("mutations"), Muts);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::UpsertFunctionMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FStudioFunction Saved;
			if (CrowdyStudioGql::ParseFunction(Envelope, TEXT("gameModelUpsertFunction"), Saved) && Saved.Warnings.Num() > 0)
			{
				// The warning detail shows in the function editor's warnings panel; keep the status terse.
				SetStatus(FString::Printf(TEXT("Saved function (%d static-analysis warning(s))."), Saved.Warnings.Num()), false);
			}
			else
			{
				SetStatus(TEXT("Saved function."), false);
			}
			FetchFunctions(FString());
		});
}

void FCrowdyStudioController::DeleteFunction(const FString& Name)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a function to delete."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("name"), Name);

	SendGame(CrowdyStudioGql::DeleteFunctionMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Deleted function."), false);
			FetchFunctions(FString());
		});
}

void FCrowdyStudioController::FetchFeatures()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list features."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(CrowdyStudioGql::FeaturesQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseFeatures(Envelope, TEXT("gameModelFeatures"), Features);
			SetStatus(FString::Printf(TEXT("%d feature(s)."), Features.Num()), false);
			OnFeaturesChanged.Broadcast();
		});
}

void FCrowdyStudioController::DefineFeature(const FString& FeatureKey, const FString& Description)
{
	if (SelectedAppId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("A feature needs an app and a feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);
	SetOptionalStringField(Input, TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::DefineFeatureMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved feature."), false);
			FetchFeatures();
		});
}

void FCrowdyStudioController::FetchTierFeatures()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list tier-feature grants."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(CrowdyStudioGql::TierFeaturesQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseTierFeatures(Envelope, TEXT("gameModelTierFeatures"), TierFeatures);
			SetStatus(FString::Printf(TEXT("%d tier-feature grant(s)."), TierFeatures.Num()), false);
			OnTierFeaturesChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchAppAccessTiers()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list access tiers."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendManagement(CrowdyStudioGql::AppAccessTiersQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseAccessTiers(Envelope, TEXT("appAccessTiers"), AccessTiers);
			OnAccessTiersChanged.Broadcast();
		});
}

void FCrowdyStudioController::GrantTierFeature(int64 TierId, const FString& FeatureKey)
{
	if (SelectedAppId == 0 || TierId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("Granting a tier feature needs an app, tier id, and feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("tierId"), TierId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::GrantTierFeatureMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Granted feature to tier."), false);
			FetchTierFeatures();
		});
}

void FCrowdyStudioController::RevokeTierFeature(int64 TierId, const FString& FeatureKey)
{
	if (SelectedAppId == 0 || TierId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("Revoking a tier feature needs an app, tier id, and feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("tierId"), TierId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::RevokeTierFeatureMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Revoked feature from tier."), false);
			FetchTierFeatures();
		});
}

void FCrowdyStudioController::FetchGameModelPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its game-model policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(CrowdyStudioGql::GameModelPolicyQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGameModelPolicy(Envelope, TEXT("gameModelPolicy"), GameModelPolicy);
			OnGameModelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetGameModelPolicy(const FString& SessionCreationPolicy, const FString& DefaultParticipantRole)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting game-model policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetOptionalStringField(Input, TEXT("sessionCreationPolicy"), SessionCreationPolicy);
	SetOptionalStringField(Input, TEXT("defaultParticipantRole"), DefaultParticipantRole);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::SetGameModelPolicyMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGameModelPolicy(Envelope, TEXT("gameModelSetPolicy"), GameModelPolicy);
			SetStatus(TEXT("Game-model policy updated."), false);
			OnGameModelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SeedGameModel(const FString& SeedJson)
{
	if (SelectedAppId == 0 || SeedJson.IsEmpty())
	{
		SetStatus(TEXT("Seeding needs a selected app and a JSON body."), true);
		return;
	}

	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonObject(SeedJson, Input))
	{
		SetStatus(TEXT("Seed body must be a JSON object."), true);
		return;
	}

	SetBigIntField(Input, TEXT("appId"), SelectedAppId);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(CrowdyStudioGql::SeedGameModelMutation(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FString Summary;
			CrowdyStudioGql::ParseSeedResult(Envelope, Summary);
			SetStatus(Summary.IsEmpty() ? TEXT("Seed complete.") : Summary, false);
			FetchContainerTypes();
		});
}

void FCrowdyStudioController::FetchContainers(const FString& TypeNameFilter, const FString& SessionIdFilter)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list live containers."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	// Optional filters: an empty box means "all", sent as JSON null.
	if (TypeNameFilter.IsEmpty()) { Variables->SetField(TEXT("typeName"), MakeShared<FJsonValueNull>()); }
	else { Variables->SetStringField(TEXT("typeName"), TypeNameFilter); }
	if (SessionIdFilter.IsEmpty()) { Variables->SetField(TEXT("sessionId"), MakeShared<FJsonValueNull>()); }
	else { Variables->SetStringField(TEXT("sessionId"), SessionIdFilter); }

	SendGame(CrowdyStudioGql::ContainersQuery(), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Containers);
			SetStatus(FString::Printf(TEXT("%d live container(s)."), Containers.Num()), false);
			OnContainersChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchContainerState(const FString& ContainerId)
{
	if (SelectedAppId == 0 || ContainerId.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a container to read its state."), true);
		return;
	}

	SelectedContainerId = ContainerId;
	ContainerState = FStudioContainerState();

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("containerId"), ContainerId);

	SendGame(CrowdyStudioGql::ContainerStateQuery(), Variables,
		[this, ContainerId](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedContainerId != ContainerId)
			{
				return; // selection moved on before the reply arrived
			}
			CrowdyStudioGql::ParseContainerState(Envelope, TEXT("gameModelContainerState"), ContainerState);
			OnContainerStateChanged.Broadcast();
		});
}

void FCrowdyStudioController::SelectOrg(int64 OrgId)
{
	SelectedOrgId = OrgId;
	SelectedEnvironmentSlug.Empty();
	PersistSelection();
	FetchEnvironments(OrgId);
}

void FCrowdyStudioController::SelectApp(int64 AppId)
{
	SelectedAppId = AppId;

	// An app carries its own org, so selecting one sets the active org and pulls that org's
	// environments no separate org pick needed.
	if (const TSharedPtr<FStudioApp> App = GetSelectedApp())
	{
		SelectedOrgId = App->OrgId;
	}
	PersistSelection();

	FetchApp(AppId);
	if (SelectedOrgId != 0)
	{
		FetchEnvironments(SelectedOrgId);
	}

	// Let app-scoped views (teams, channels, grid, game model) reload for the new app.
	OnSelectedAppChanged.Broadcast();
}

void FCrowdyStudioController::SelectEnvironment(const FString& EnvironmentSlug)
{
	SelectedEnvironmentSlug = EnvironmentSlug;
}

TSharedPtr<FStudioOrg> FCrowdyStudioController::GetSelectedOrg() const
{
	for (const TSharedPtr<FStudioOrg>& Org : Organizations)
	{
		if (Org.IsValid() && Org->OrgId == SelectedOrgId)
		{
			return Org;
		}
	}
	return nullptr;
}

TSharedPtr<FStudioApp> FCrowdyStudioController::GetSelectedApp() const
{
	for (const TSharedPtr<FStudioApp>& App : Apps)
	{
		if (App.IsValid() && App->AppId == SelectedAppId)
		{
			return App;
		}
	}
	return nullptr;
}

bool FCrowdyStudioController::HasOrgPermission(const FString& PermissionKey) const
{
	const TSharedPtr<FStudioOrg> Org = GetSelectedOrg();
	return Org.IsValid() && Org->Permissions.Contains(PermissionKey);
}

bool FCrowdyStudioController::CanManageApps() const
{
	return HasOrgPermission(TEXT("manage_apps"));
}

bool FCrowdyStudioController::CanManageEnvironments() const
{
	return HasOrgPermission(TEXT("manage_environments"));
}



FString FCrowdyStudioController::ResolveManagementBaseUrl() const
{
	// Single source of truth: the same enum-driven URL the runtime uses, so the editor and
	// the packaged game always talk to the same backend.
	FString Base;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		Base = Settings->GetManagementApiUrl();
	}
	Base.RemoveFromEnd(TEXT("/"));
	return Base;
}

FString FCrowdyStudioController::ResolveManagementUrl() const
{
	return ResolveManagementBaseUrl() + TEXT("/graphql");
}

FString FCrowdyStudioController::ResolveGameUrl() const
{
	// The game endpoint already includes the /graphql path (unlike the management base URL).
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->GetGameApiHttpUrl();
	}
	return FString();
}

void FCrowdyStudioController::SendManagement(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure)
{
	FCrowdyGqlRequest Request;
	Request.Endpoint = ResolveManagementUrl();
	Request.BearerToken = AuthToken;
	Request.Query = Query;
	Request.Variables = Variables;

	if (CrowdyStudioTrace::Enabled())
	{
		UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] management op '%s' -> %s"), *DescribeQuery(Query), *Request.Endpoint);
	}

	BeginRequest();

	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	FCrowdyGraphQLClient::Send(Request,
		[WeakThis, OnSuccess = MoveTemp(OnSuccess), OnFailure = MoveTemp(OnFailure)](FCrowdyGqlResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->EndRequest();

			if (CrowdyStudioTrace::Enabled())
			{
				UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] management response http=%d errors=%d ok=%d"),
					Result.HttpCode, Result.Errors.Num(), Result.bSuccess ? 1 : 0);
			}

			const FString Error = DescribeGqlError(Result);
			if (!Error.IsEmpty())
			{
				Self->SetStatus(Error, true);
				if (OnFailure)
				{
					OnFailure();
				}
				return;
			}

			if (OnSuccess)
			{
				OnSuccess(Result.Data);
			}
		});
}

void FCrowdyStudioController::SendGame(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess)
{
	const FString GameUrl = ResolveGameUrl();
	if (GameUrl.IsEmpty())
	{
		SetStatus(TEXT("No game endpoint configured — sync an app to the project first."), true);
		return;
	}

	FCrowdyGqlRequest Request;
	Request.Endpoint = GameUrl;
	Request.BearerToken = AuthToken;
	Request.Query = Query;
	Request.Variables = Variables;

	if (CrowdyStudioTrace::Enabled())
	{
		UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] game op '%s' -> %s"), *DescribeQuery(Query), *Request.Endpoint);
	}

	BeginRequest();

	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	FCrowdyGraphQLClient::Send(Request,
		[WeakThis, OnSuccess = MoveTemp(OnSuccess)](FCrowdyGqlResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->EndRequest();

			if (CrowdyStudioTrace::Enabled())
			{
				UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] game response http=%d errors=%d ok=%d"),
					Result.HttpCode, Result.Errors.Num(), Result.bSuccess ? 1 : 0);
			}

			const FString Error = DescribeGqlError(Result);
			if (!Error.IsEmpty())
			{
				Self->SetStatus(Error, true);
				return;
			}

			if (OnSuccess)
			{
				OnSuccess(Result.Data);
			}
		});
}

void FCrowdyStudioController::SetStatus(const FString& Message, bool bIsError)
{
	StatusMessage = Message;
	bStatusWasError = bIsError;

	UE_CLOG(!bIsError, LogCrowdyStudio, Log, TEXT("%s"), *Message);
	UE_CLOG(bIsError, LogCrowdyStudio, Warning, TEXT("%s"), *Message);

	OnStatusMessage.Broadcast(Message, bIsError);
}

void FCrowdyStudioController::BeginRequest()
{
	++InFlightCount;
	OnBusyChanged.Broadcast();
}

void FCrowdyStudioController::EndRequest()
{
	if (InFlightCount > 0)
	{
		--InFlightCount;
	}
	OnBusyChanged.Broadcast();
}

void FCrowdyStudioController::FetchAppsAndEnvironments()
{
	FetchApps();
	if (SelectedOrgId != 0)
	{
		FetchEnvironments(SelectedOrgId);
	}

	// On sign-in a remembered app is already selected (set in Initialize from saved settings), but it
	// never went through SelectApp, so announce it here too so the team/channel views auto-load.
	if (SelectedAppId != 0)
	{
		OnSelectedAppChanged.Broadcast();
	}
}

void FCrowdyStudioController::PersistSelection() const
{
	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->LastOrgId = SelectedOrgId;
		User->LastAppId = SelectedAppId;
		User->SaveConfig();
	}
}

UCrowdyStudioUserSettings* FCrowdyStudioController::GetUserSettings() const
{
	return GetMutableDefault<UCrowdyStudioUserSettings>();
}
