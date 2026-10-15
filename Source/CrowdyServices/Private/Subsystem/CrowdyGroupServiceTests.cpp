#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/AnyOf.h"
#include "CrowdyChannelsTestTypes.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServiceApiTestSupport.h"
#include "CrowdyTeamsTestTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyChannels.h"
#include "Subsystem/CrowdyTeams.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelError.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMember.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMembership.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The teams and channels services no longer carry their own GraphQL text: they name an operation and let
// FCrowdyCppClient::RunOp resolve it against the vendored CrowdyCPP generated tables, and they parse the answer with
// their own types rather than a shared group model. This file pins the two things that swap made load-bearing: that
// every operation name still resolves, and that the parsers read the shape the generated documents actually select,
// which is not the shape the hand-written queries used to ask for.
namespace
{
	constexpr EAutomationTestFlags GroupServiceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FRoutedOperation
	{
		ECrowdyCppApiDomain Domain;
		const TCHAR* OperationName;
	};

	// Every operation UCrowdyTeams and UCrowdyChannels issue, including the ones only the reliable-RPC channel
	// bootstrap reaches (Channels, MyChannels, JoinChannel, CreateChannel).
	const FRoutedOperation kRoutedOperations[] =
	{
		{ ECrowdyCppApiDomain::Teams, TEXT("MyTeams") },
		{ ECrowdyCppApiDomain::Teams, TEXT("Team") },
		{ ECrowdyCppApiDomain::Teams, TEXT("Teams") },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamMembers") },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamRoles") },
		{ ECrowdyCppApiDomain::Teams, TEXT("TeamPolicy") },
		{ ECrowdyCppApiDomain::Teams, TEXT("CreateTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("UpdateTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("DeleteTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("JoinTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("RequestToJoinTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("LeaveTeam") },
		{ ECrowdyCppApiDomain::Teams, TEXT("AddTeamMember") },
		{ ECrowdyCppApiDomain::Teams, TEXT("RemoveTeamMember") },
		{ ECrowdyCppApiDomain::Teams, TEXT("BanTeamMember") },
		{ ECrowdyCppApiDomain::Teams, TEXT("UnbanTeamMember") },
		{ ECrowdyCppApiDomain::Teams, TEXT("CreateTeamRole") },
		{ ECrowdyCppApiDomain::Teams, TEXT("UpdateTeamRole") },
		{ ECrowdyCppApiDomain::Teams, TEXT("DeleteTeamRole") },
		{ ECrowdyCppApiDomain::Teams, TEXT("SetTeamMemberRoles") },
		{ ECrowdyCppApiDomain::Teams, TEXT("SetTeamPolicy") },

		{ ECrowdyCppApiDomain::Channels, TEXT("MyChannels") },
		{ ECrowdyCppApiDomain::Channels, TEXT("Channel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("Channels") },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelMembers") },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelRoles") },
		{ ECrowdyCppApiDomain::Channels, TEXT("ChannelPolicy") },
		{ ECrowdyCppApiDomain::Channels, TEXT("CreateChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("UpdateChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("DeleteChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("JoinChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("RequestToJoinChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("LeaveChannel") },
		{ ECrowdyCppApiDomain::Channels, TEXT("AddChannelMember") },
		{ ECrowdyCppApiDomain::Channels, TEXT("RemoveChannelMember") },
		{ ECrowdyCppApiDomain::Channels, TEXT("BanChannelMember") },
		{ ECrowdyCppApiDomain::Channels, TEXT("UnbanChannelMember") },
		{ ECrowdyCppApiDomain::Channels, TEXT("CreateChannelRole") },
		{ ECrowdyCppApiDomain::Channels, TEXT("UpdateChannelRole") },
		{ ECrowdyCppApiDomain::Channels, TEXT("DeleteChannelRole") },
		{ ECrowdyCppApiDomain::Channels, TEXT("SetChannelMemberRoles") },
		{ ECrowdyCppApiDomain::Channels, TEXT("SetChannelPolicy") },
	};

	using CrowdyServiceApiTest::MakeResult;
	using CrowdyServiceApiTest::OperationResolves;
}

// Every operation the two group services issue must still resolve to a document in the vendored operation tables. A
// canned 200 stands in for the server, so this is pure name resolution with no network: a vendored bump that renames
// or drops one of these fails here rather than the first time a player presses the button.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceOperationsResolveTest,
	"CrowdySDK.CrowdyServices.GroupOperationsResolve", GroupServiceTestFlags)
bool FCrowdyGroupServiceOperationsResolveTest::RunTest(const FString& Parameters)
{
	for (const FRoutedOperation& Op : kRoutedOperations)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Op.OperationName), Client.IsValid()))
		{
			continue;
		}

		FString Error;
		TestTrue(FString::Printf(TEXT("%s resolves to a document"), Op.OperationName),
			OperationResolves(Client, Op.Domain, Op.OperationName, Error));
	}
	return true;
}

// The proof the resolves-check above can fail. A name the domain does not define must be refused without a round
// trip, which is what a renamed or dropped vendored operation would look like.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceUnknownOperationFailsTest,
	"CrowdySDK.CrowdyServices.GroupUnknownOperationFails", GroupServiceTestFlags)
bool FCrowdyGroupServiceUnknownOperationFailsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructs"), Client.IsValid()))
	{
		return false;
	}

	FString Error;
	TestFalse(TEXT("a bogus team operation does not resolve"),
		OperationResolves(Client, ECrowdyCppApiDomain::Teams, TEXT("ThisTeamOperationDoesNotExist"), Error));
	TestTrue(TEXT("the failure names the operation"), Error.Contains(TEXT("ThisTeamOperationDoesNotExist")));
	return true;
}

// The generated member and membership documents select each nested role WITHOUT its own groupId, where the
// hand-written queries asked for it. Left alone that would silently zero a Blueprint-readable field, so the parsers
// fill it from the group the role was read through. Sorting by rank must survive too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceNestedRoleIdsTest,
	"CrowdySDK.CrowdyServices.GroupNestedRoleIds", GroupServiceTestFlags)
bool FCrowdyGroupServiceNestedRoleIdsTest::RunTest(const FString& Parameters)
{
	// Exactly what the generated MyTeams document selects: no groupId and no createdAt inside roles, and the roles
	// deliberately out of rank order so the sort is observable.
	const FString MembershipJson = TEXT(R"({
		"group": { "groupId": "77", "appId": "5", "name": "Reavers", "ownerUserId": "9",
		           "membershipPolicy": "request", "status": "active", "createdAt": "2026-01-01" },
		"roles": [
			{ "groupRoleId": "302", "roleName": "officer", "rank": 5, "isSystem": false, "permissions": ["manage_members"] },
			{ "groupRoleId": "301", "roleName": "leader", "rank": 1, "isSystem": true, "permissions": ["manage_group"] }
		],
		"permissions": ["manage_group", "manage_members"],
		"joinedAt": "2026-02-02"
	})");

	TSharedPtr<FJsonObject> MembershipObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MembershipJson);
	if (!TestTrue(TEXT("the membership fixture parses"), FJsonSerializer::Deserialize(Reader, MembershipObj)))
	{
		return false;
	}

	FCrowdyTeamMembership Membership;
	TestTrue(TEXT("the membership reads"), FCrowdyTeamMembership::ParseFromJson(MembershipObj, Membership));

	TestEqual(TEXT("the team id reads"), Membership.Team.TeamId, static_cast<int64>(77));
	TestEqual(TEXT("the membership policy reads"), Membership.Team.MembershipPolicy,
		ECrowdyTeamMembershipPolicy::Request);

	if (!TestEqual(TEXT("both roles read"), Membership.Roles.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("roles sort by rank"), Membership.Roles[0].RoleName, FString(TEXT("leader")));
	TestEqual(TEXT("the nested role's team id is filled from its team"), Membership.Roles[0].TeamId,
		static_cast<int64>(77));
	TestEqual(TEXT("the second nested role's team id is filled too"), Membership.Roles[1].TeamId,
		static_cast<int64>(77));

	// The document does not select it, so it must stay empty rather than being invented.
	TestTrue(TEXT("the nested role's created time is absent"), Membership.Roles[0].CreatedAt.IsEmpty());

	// A member row carries its own group id, and its nested roles inherit that one.
	const FString MemberJson = TEXT(R"({
		"groupMemberId": "900", "groupId": "77", "userId": "142", "status": "active", "createdAt": "2026-03-03",
		"roles": [ { "groupRoleId": "301", "roleName": "leader", "rank": 1, "isSystem": true, "permissions": [] } ]
	})");

	TSharedPtr<FJsonObject> MemberObj;
	const TSharedRef<TJsonReader<>> MemberReader = TJsonReaderFactory<>::Create(MemberJson);
	if (!TestTrue(TEXT("the member fixture parses"), FJsonSerializer::Deserialize(MemberReader, MemberObj)))
	{
		return false;
	}

	FCrowdyChannelMember Member;
	TestTrue(TEXT("the member reads"), FCrowdyChannelMember::ParseFromJson(MemberObj, Member));
	TestEqual(TEXT("the channel id reads"), Member.ChannelId, static_cast<int64>(77));
	if (TestEqual(TEXT("the member's role reads"), Member.Roles.Num(), 1))
	{
		TestEqual(TEXT("the nested role's channel id is filled from its member"), Member.Roles[0].ChannelId,
			static_cast<int64>(77));
	}

	return true;
}

// send_messages decides who may publish to a channel and had no representation at all while teams and channels
// shared one permission type, so a channel role could never be given posting rights through the SDK. It must survive
// a round trip now, and the team type must still refuse to carry it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServicePermissionSetsTest,
	"CrowdySDK.CrowdyServices.GroupPermissionSets", GroupServiceTestFlags)
bool FCrowdyGroupServicePermissionSetsTest::RunTest(const FString& Parameters)
{
	const FCrowdyChannelPermissions Decoded =
		FCrowdyChannelPermissions::FromStringArray({ TEXT("send_messages"), TEXT("manage_roles") });

	TestTrue(TEXT("send_messages decodes"), Decoded.bSendMessages);
	TestTrue(TEXT("manage_roles decodes"), Decoded.bManageRoles);
	TestFalse(TEXT("a permission that was not granted stays off"), Decoded.bManageChannel);

	const TArray<FString> Encoded = Decoded.ToStringArray();
	TestTrue(TEXT("send_messages encodes back"), Encoded.Contains(TEXT("send_messages")));
	TestTrue(TEXT("manage_roles encodes back"), Encoded.Contains(TEXT("manage_roles")));
	TestEqual(TEXT("nothing else is encoded"), Encoded.Num(), 2);

	// The server spells the channel-wide and team-wide "manage everything" permission with the same key, because one
	// permission table serves both group types. The flag is named for the thing it manages; the key is not.
	FCrowdyChannelPermissions ManageChannel;
	ManageChannel.bManageChannel = true;
	TestTrue(TEXT("manage channel sends the server's group key"),
		ManageChannel.ToStringArray().Contains(TEXT("manage_group")));

	FCrowdyTeamPermissions ManageTeam;
	ManageTeam.bManageTeam = true;
	TestTrue(TEXT("manage team sends the same server key"),
		ManageTeam.ToStringArray().Contains(TEXT("manage_group")));

	// A team has no posting permission, so a server that lists one must not smuggle it into the team type.
	const FCrowdyTeamPermissions TeamDecoded =
		FCrowdyTeamPermissions::FromStringArray({ TEXT("send_messages"), TEXT("invite_members") });
	TestTrue(TEXT("a team still reads the permissions it does have"), TeamDecoded.bInviteMembers);
	TestEqual(TEXT("a team encodes only its own permissions"), TeamDecoded.ToStringArray().Num(), 1);

	return true;
}

// The per-user cap is one server field, maxGroupsPerUser, and each service reads it into a name that says which
// thing is capped. A parser wired to the wrong field would silently report no cap at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServicePolicyTest,
	"CrowdySDK.CrowdyServices.GroupPolicy", GroupServiceTestFlags)
bool FCrowdyGroupServicePolicyTest::RunTest(const FString& Parameters)
{
	const FString PolicyJson = TEXT(R"({
		"appId": "5", "groupType": "team", "creationPolicy": "member",
		"defaultMembershipPolicy": "invite", "maxMembers": 50, "maxGroupsPerUser": 3
	})");

	TSharedPtr<FJsonObject> PolicyObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PolicyJson);
	if (!TestTrue(TEXT("the policy fixture parses"), FJsonSerializer::Deserialize(Reader, PolicyObj)))
	{
		return false;
	}

	FCrowdyTeamPolicy TeamPolicy;
	TestTrue(TEXT("the team policy reads"), FCrowdyTeamPolicy::ParseFromJson(PolicyObj, TeamPolicy));
	TestEqual(TEXT("the app id reads"), TeamPolicy.AppId, static_cast<int64>(5));
	TestEqual(TEXT("the creation policy reads"), TeamPolicy.CreationPolicy, ECrowdyTeamCreationPolicy::Member);
	TestEqual(TEXT("the default membership policy reads"), TeamPolicy.DefaultMembershipPolicy,
		ECrowdyTeamMembershipPolicy::Invite);
	TestEqual(TEXT("the member cap reads"), TeamPolicy.MaxMembers, 50);
	TestEqual(TEXT("the per-user cap reads into the team-named field"), TeamPolicy.MaxTeamsPerUser, 3);

	FCrowdyChannelPolicy ChannelPolicy;
	TestTrue(TEXT("the channel policy reads"), FCrowdyChannelPolicy::ParseFromJson(PolicyObj, ChannelPolicy));
	TestEqual(TEXT("the per-user cap reads into the channel-named field"), ChannelPolicy.MaxChannelsPerUser, 3);

	// The round trip a Set*Policy call depends on: the enum has to reach the server as the string it came from.
	TestEqual(TEXT("the creation policy encodes back"),
		FCrowdyTeamPolicy::CreationPolicyToString(TeamPolicy.CreationPolicy), FString(TEXT("member")));

	return true;
}

// A cancellation is not a server verdict. FromMessage classifies by keyword, and "request canceled" matches none of
// them, so it would land as a generic ServerError and a caller would tell the player the server refused something it
// never saw. Both services route it through DescribeFailure instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceFailureClassificationTest,
	"CrowdySDK.CrowdyServices.GroupFailureClassification", GroupServiceTestFlags)
bool FCrowdyGroupServiceFailureClassificationTest::RunTest(const FString& Parameters)
{
	FCrowdyCppJsonResult Canceled;
	Canceled.bTransportOk = false;
	Canceled.ErrorMessage = FCrowdyCppClient::CanceledErrorMessage();

	const FCrowdyTeamError TeamCanceled = CrowdyServiceApi::DescribeFailure<FCrowdyTeamError>(Canceled);
	TestEqual(TEXT("a cancellation is not reported as a server error"), TeamCanceled.Code,
		ECrowdyTeamErrorCode::NetworkError);
	TestFalse(TEXT("the transport's own wording does not reach the caller"),
		TeamCanceled.Message.Equals(FCrowdyCppClient::CanceledErrorMessage()));

	const FCrowdyChannelError ChannelCanceled = CrowdyServiceApi::DescribeFailure<FCrowdyChannelError>(Canceled);
	TestEqual(TEXT("the channel service classifies it the same way"), ChannelCanceled.Code,
		ECrowdyChannelErrorCode::NetworkError);

	// A real server verdict must still be classified by what the server said.
	FCrowdyCppJsonResult Refused;
	Refused.bTransportOk = false;
	Refused.ErrorMessage = TEXT("Team not found");
	TestEqual(TEXT("a server verdict is still classified from its message"),
		CrowdyServiceApi::DescribeFailure<FCrowdyTeamError>(Refused).Code, ECrowdyTeamErrorCode::NotFound);

	// The request never issued at all is a network-class failure, not a server one.
	TestEqual(TEXT("an unavailable client is a network failure"),
		CrowdyServiceApi::ClientUnavailableError<FCrowdyChannelError>().Code,
		ECrowdyChannelErrorCode::NetworkError);

	return true;
}

// The readers decide whether a caller's success delegate fires at all, so each has to refuse a payload that is not
// the shape it asked for rather than handing back a default-constructed value that reads as an empty team.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceResponseReadersTest,
	"CrowdySDK.CrowdyServices.GroupResponseReaders", GroupServiceTestFlags)
bool FCrowdyGroupServiceResponseReadersTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamError Error;

	FCrowdyTeam Team;
	const FCrowdyCppJsonResult Good = MakeResult(TEXT(R"({ "team": { "groupId": "12", "name": "Reavers" } })"));
	if (TestTrue(TEXT("a well-shaped object reads"),
		CrowdyServiceApi::ReadObject(Good, TEXT("team"), Team, Error)))
	{
		TestEqual(TEXT("and carries the id"), Team.TeamId, static_cast<int64>(12));
	}

	// The answer is present but under a different field: reading it as success would hand back an empty team.
	FCrowdyTeam Missing;
	TestFalse(TEXT("a missing field is refused"),
		CrowdyServiceApi::ReadObject(Good, TEXT("createTeam"), Missing, Error));
	TestEqual(TEXT("and is reported as a server error"), Error.Code, ECrowdyTeamErrorCode::ServerError);

	// A transport failure must be reported with the server's reason, not as an unreadable payload.
	FCrowdyCppJsonResult Failed;
	Failed.bTransportOk = false;
	Failed.ErrorMessage = TEXT("permission denied");
	FCrowdyTeam Unreached;
	TestFalse(TEXT("a failed call does not read"),
		CrowdyServiceApi::ReadObject(Failed, TEXT("team"), Unreached, Error));
	TestEqual(TEXT("and keeps the server's classification"), Error.Code, ECrowdyTeamErrorCode::Forbidden);

	TArray<FCrowdyTeam> Teams;
	const FCrowdyCppJsonResult List =
		MakeResult(TEXT(R"({ "teams": [ { "groupId": "1" }, { "groupId": "2" } ] })"));
	if (TestTrue(TEXT("a list reads"), CrowdyServiceApi::ReadArray(List, TEXT("teams"), Teams, Error)))
	{
		TestEqual(TEXT("with every row"), Teams.Num(), 2);
	}

	// An empty list is a legitimate answer, not a failure: a player in no teams is not an error.
	TArray<FCrowdyTeam> None;
	TestTrue(TEXT("an empty list is a success"),
		CrowdyServiceApi::ReadArray(MakeResult(TEXT(R"({ "teams": [] })")), TEXT("teams"), None, Error));
	TestEqual(TEXT("and reads as no rows"), None.Num(), 0);

	// A delete answers with a boolean the SDK does not surface; what matters is that the call itself succeeded.
	TestTrue(TEXT("an acknowledgement reads"),
		CrowdyServiceApi::ReadAcknowledgement(MakeResult(TEXT(R"({ "deleteTeam": true })")), Error));
	TestFalse(TEXT("a failed acknowledgement does not"),
		CrowdyServiceApi::ReadAcknowledgement(Failed, Error));

	return true;
}

// Twelve of these operations take a single input wrapper where the hand-written queries passed flat scalars. A
// wrapped-variable mistake still compiles and still resolves, so this pins the helper every one of them builds with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceVariableShapeTest,
	"CrowdySDK.CrowdyServices.GroupVariableShape", GroupServiceTestFlags)
bool FCrowdyGroupServiceVariableShapeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("groupId"), CrowdyServiceApi::BigInt(77));
	CrowdyServiceApi::SetPermissionKeys(Input, { TEXT("manage_roles"), TEXT("send_messages") });

	const TSharedPtr<FJsonObject> Variables = CrowdyServiceApi::WrapInput(Input);

	const TSharedPtr<FJsonObject>* Wrapped = nullptr;
	if (!TestTrue(TEXT("the input is nested under 'input'"), Variables->TryGetObjectField(TEXT("input"), Wrapped)))
	{
		return false;
	}

	// A BigInt id must go out as a JSON string, never a number.
	FString GroupId;
	TestTrue(TEXT("the id is carried as a string"), (*Wrapped)->TryGetStringField(TEXT("groupId"), GroupId));
	TestEqual(TEXT("and is the id that was set"), GroupId, FString(TEXT("77")));

	const TArray<TSharedPtr<FJsonValue>>* Permissions = nullptr;
	if (TestTrue(TEXT("the permissions are a list"), (*Wrapped)->TryGetArrayField(TEXT("permissions"), Permissions)))
	{
		TestEqual(TEXT("with both keys"), Permissions->Num(), 2);
	}

	// The flat scalars the old hand-written queries used must NOT also be present at the top level, which is the
	// shape that would silently reach a server expecting only the wrapper.
	FString Unused;
	TestFalse(TEXT("nothing is left at the top level"), Variables->TryGetStringField(TEXT("groupId"), Unused));

	return true;
}

// GROUP_MEMBER_BANNED is the one failure a caller must tell apart from every other refusal, and the server is free to
// reword its message, so Banned comes from the error code alone. The message here deliberately matches another class.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceBannedCodeTest,
	"CrowdySDK.CrowdyServices.GroupBannedFromErrorCode", GroupServiceTestFlags)
bool FCrowdyGroupServiceBannedCodeTest::RunTest(const FString& Parameters)
{
	FCrowdyCppJsonResult Banned;
	Banned.bTransportOk = false;
	Banned.ErrorMessage = TEXT("Team not found");
	Banned.ErrorCode = TEXT("GROUP_MEMBER_BANNED");

	TestEqual(TEXT("a banned code maps to Banned on a team"),
		CrowdyServiceApi::DescribeFailure<FCrowdyTeamError>(Banned).Code, ECrowdyTeamErrorCode::Banned);
	TestEqual(TEXT("and on a channel"),
		CrowdyServiceApi::DescribeFailure<FCrowdyChannelError>(Banned).Code, ECrowdyChannelErrorCode::Banned);
	TestEqual(TEXT("and keeps the server's message to show"),
		CrowdyServiceApi::DescribeFailure<FCrowdyTeamError>(Banned).Message, FString(TEXT("Team not found")));

	FCrowdyCppJsonResult WordOnly;
	WordOnly.bTransportOk = false;
	WordOnly.ErrorMessage = TEXT("You are banned from this team");
	TestEqual(TEXT("the word banned in a message is not the code"),
		CrowdyServiceApi::DescribeFailure<FCrowdyTeamError>(WordOnly).Code, ECrowdyTeamErrorCode::ServerError);

	FCrowdyCppJsonResult OtherCode;
	OtherCode.bTransportOk = false;
	OtherCode.ErrorMessage = TEXT("Team not found");
	OtherCode.ErrorCode = TEXT("NOT_FOUND");
	TestEqual(TEXT("another code leaves the message classification alone"),
		CrowdyServiceApi::DescribeFailure<FCrowdyChannelError>(OtherCode).Code, ECrowdyChannelErrorCode::NotFound);

	return true;
}

// A ban reason longer than 500 characters never leaves the client: the call answers through its error delegate and no
// request is queued. Exactly 500 is allowed, so the boundary is pinned from both sides.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceBanReasonLimitTest,
	"CrowdySDK.CrowdyServices.GroupBanReasonLimit", GroupServiceTestFlags)
bool FCrowdyGroupServiceBanReasonLimitTest::RunTest(const FString& Parameters)
{
	const FString AtLimit = FString::ChrN(CrowdyServiceApi::MaxBanReasonLength, TEXT('a'));
	const FString OverLimit = AtLimit + TEXT("a");

	FCrowdyTeamError HelperError;
	TestFalse(TEXT("a reason of exactly 500 characters is not too long"),
		CrowdyServiceApi::BanReasonTooLong(AtLimit, HelperError));
	TestTrue(TEXT("a reason of 501 characters is"), CrowdyServiceApi::BanReasonTooLong(OverLimit, HelperError));
	TestEqual(TEXT("and is refused as a policy violation"), HelperError.Code, ECrowdyTeamErrorCode::PolicyViolation);

	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));

	const TStrongObjectPtr<UCrowdyTeams> Teams(NewObject<UCrowdyTeams>(Instance.Get()));
	const TStrongObjectPtr<UCrowdyTeamsTestListener> TeamListener(
		NewObject<UCrowdyTeamsTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT(R"({"data":{
		"banTeamMember":{"groupMemberId":"900","groupId":"7","userId":"142","status":"banned"},
		"myTeams":[]}})"), 200);
	if (!TestTrue(TEXT("the scripted client was built"), Client.IsValid()))
	{
		return false;
	}
	Teams->InitializeForTest(Client);

	FOnTeamMemberSuccess OnTeamBanned;
	OnTeamBanned.BindDynamic(TeamListener.Get(), &UCrowdyTeamsTestListener::HandleMember);
	FOnTeamError OnTeamError;
	OnTeamError.BindDynamic(TeamListener.Get(), &UCrowdyTeamsTestListener::HandleError);

	Teams->BanTeamMember(7, 142, OverLimit, OnTeamBanned, OnTeamError);
	TestEqual(TEXT("a team ban with a long reason answers its error delegate"), TeamListener->Errors.Num(), 1);
	TestEqual(TEXT("and sends nothing"), Client->NumPendingRequests(), 0);

	Teams->BanTeamMember(7, 142, AtLimit, OnTeamBanned, OnTeamError);
	TestTrue(TEXT("a team ban at the limit is sent"), Client->NumPendingRequests() > 0);
	for (int32 Pass = 0; Pass < 8 && Client->NumPendingRequests() > 0; ++Pass)
	{
		Client->Poll();
	}
	TestEqual(TEXT("and its answer reaches the success delegate"), TeamListener->Successes, 1);
	TestEqual(TEXT("without a further error"), TeamListener->Errors.Num(), 1);

	const TStrongObjectPtr<UCrowdyChannels> Channels(NewObject<UCrowdyChannels>(Instance.Get()));
	const TStrongObjectPtr<UCrowdyChannelsTestListener> ChannelListener(
		NewObject<UCrowdyChannelsTestListener>(GetTransientPackage(), NAME_None, RF_Transient));

	FOnChannelMemberSuccess OnChannelBanned;
	OnChannelBanned.BindDynamic(ChannelListener.Get(), &UCrowdyChannelsTestListener::HandleMember);
	FOnChannelError OnChannelError;
	OnChannelError.BindDynamic(ChannelListener.Get(), &UCrowdyChannelsTestListener::HandleError);

	Channels->BanChannelMember(7, 142, OverLimit, OnChannelBanned, OnChannelError);
	if (TestEqual(TEXT("a channel ban with a long reason answers its error delegate"),
		ChannelListener->ErrorCodes.Num(), 1))
	{
		TestEqual(TEXT("as a policy violation"), ChannelListener->ErrorCodes[0],
			ECrowdyChannelErrorCode::PolicyViolation);
	}

	// This subsystem has no client on a bare game instance, so a reason the limit lets through reaches the point where
	// the client is resolved and fails as a network error: a different code from the length refusal.
	Channels->BanChannelMember(7, 142, AtLimit, OnChannelBanned, OnChannelError);
	if (TestEqual(TEXT("a channel ban at the limit gets past the length check"), ChannelListener->ErrorCodes.Num(), 2))
	{
		TestEqual(TEXT("and is not refused for its length"), ChannelListener->ErrorCodes[1],
			ECrowdyChannelErrorCode::NetworkError);
	}

	return true;
}

// Unban answers a boolean, true when there was a ban to lift. That boolean is the whole answer, so a payload without it
// must be refused rather than read as "was not banned".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGroupServiceUnbanAnswerTest,
	"CrowdySDK.CrowdyServices.GroupUnbanAnswer", GroupServiceTestFlags)
bool FCrowdyGroupServiceUnbanAnswerTest::RunTest(const FString& Parameters)
{
	FCrowdyChannelError Error;
	bool bWasBanned = false;

	TestTrue(TEXT("a true answer reads"), CrowdyServiceApi::ReadBoolean(
		MakeResult(TEXT(R"({ "unbanChannelMember": true })")), TEXT("unbanChannelMember"), bWasBanned, Error));
	TestTrue(TEXT("as there having been a ban"), bWasBanned);

	TestTrue(TEXT("a false answer reads"), CrowdyServiceApi::ReadBoolean(
		MakeResult(TEXT(R"({ "unbanChannelMember": false })")), TEXT("unbanChannelMember"), bWasBanned, Error));
	TestFalse(TEXT("as there having been none"), bWasBanned);

	bWasBanned = true;
	TestFalse(TEXT("an answer without the field is refused"), CrowdyServiceApi::ReadBoolean(
		MakeResult(TEXT(R"({ "unbanTeamMember": true })")), TEXT("unbanChannelMember"), bWasBanned, Error));
	TestEqual(TEXT("and reported as a server error"), Error.Code, ECrowdyChannelErrorCode::ServerError);

	return true;
}

// A Blueprint async node builds its data pins from its first assignable delegate only, so On Error shows an Error and a
// Message pin only when every assignable delegate of the node shares one signature that carries them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAsyncNodesShareOneSignatureTest,
	"CrowdySDK.CrowdyServices.AsyncNodesShareOneSignature", GroupServiceTestFlags)
bool FCrowdyAsyncNodesShareOneSignatureTest::RunTest(const FString& Parameters)
{
	const TCHAR* const Prefixes[] = { TEXT("CrowdyChannels_"), TEXT("CrowdyTeams_"), TEXT("CrowdyAvatars_"), TEXT("CrowdyAuth_") };

	auto CollectParameters = [](const UFunction* Signature)
	{
		TArray<const FProperty*> Result;
		for (TFieldIterator<FProperty> It(Signature); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Result.Add(*It);
			}
		}
		return Result;
	};

	auto SameParameter = [](const FProperty* A, const FProperty* B)
	{
		if (A->GetFName() != B->GetFName() || A->GetClass() != B->GetClass())
		{
			return false;
		}
		const FStructProperty* StructA = CastField<const FStructProperty>(A);
		return !StructA || StructA->Struct == CastField<const FStructProperty>(B)->Struct;
	};

	int32 Checked = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		const UClass* Class = *It;
		if (!Class->IsChildOf(UBlueprintAsyncActionBase::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)
			|| Class->GetOutermost()->GetName() != TEXT("/Script/CrowdyServices"))
		{
			continue;
		}
		const FString ClassName = Class->GetName();
		const bool bChecked = Algo::AnyOf(Prefixes, [&ClassName](const TCHAR* Prefix) { return ClassName.StartsWith(Prefix); });
		if (!bChecked)
		{
			continue;
		}
		++Checked;

		TArray<const FProperty*> FirstParameters;
		bool bFirst = true;
		for (TFieldIterator<FMulticastDelegateProperty> Property(Class, EFieldIterationFlags::None); Property; ++Property)
		{
			if (!Property->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			const TArray<const FProperty*> Current = CollectParameters(Property->SignatureFunction);
			if (bFirst)
			{
				FirstParameters = Current;
				bFirst = false;
				continue;
			}
			bool bSame = Current.Num() == FirstParameters.Num();
			for (int32 Index = 0; bSame && Index < Current.Num(); ++Index)
			{
				bSame = SameParameter(Current[Index], FirstParameters[Index]);
			}
			TestTrue(FString::Printf(TEXT("%s: %s has the signature of its first delegate"), *ClassName, *Property->GetName()), bSame);
		}

		TestFalse(FString::Printf(TEXT("%s has an assignable delegate"), *ClassName), bFirst);
		const bool bHasError = FirstParameters.ContainsByPredicate([](const FProperty* P) { return P->GetFName() == TEXT("Error"); });
		const bool bHasMessage = FirstParameters.ContainsByPredicate([](const FProperty* P) { return P->GetFName() == TEXT("Message"); });
		// Sign-in failures carry only the server's text.
		TestTrue(FString::Printf(TEXT("%s: its signature has an Error parameter"), *ClassName), bHasError || ClassName.StartsWith(TEXT("CrowdyAuth_")));
		TestTrue(FString::Printf(TEXT("%s: its signature has a Message parameter"), *ClassName), bHasMessage);
	}

	TestTrue(FString::Printf(TEXT("the filter found the async nodes (%d)"), Checked), Checked >= 61);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
