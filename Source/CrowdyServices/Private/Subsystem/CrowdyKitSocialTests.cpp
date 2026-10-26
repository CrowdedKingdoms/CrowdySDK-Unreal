#include "Subsystem/CrowdyKitSocial.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyKitSocialTestTypes.h"
#include "CrowdyServiceApiSupport.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyChannels.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"

namespace
{
	constexpr EAutomationTestFlags KitSocialTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const KitSocialOakfordTeams = TEXT(R"({"data":{"teams":[
		{"groupId":"1","name":"party:oak","ownerUserId":"100"},
		{"groupId":"8","name":"party:Oakford","ownerUserId":"101"},
		{"groupId":"2","name":"party:oakford","ownerUserId":"100"},
		{"groupId":"3","name":"guild:oakford","ownerUserId":"100"}]}})");

	// The first channel named party:oakford was created by someone other than the party's owner.
	const TCHAR* const KitSocialOakfordChannels = TEXT(R"({"data":{"channels":[
		{"groupId":"12","name":"party:oakford","ownerUserId":"666"},
		{"groupId":"10","name":"party:oakford","ownerUserId":"100"},
		{"groupId":"9","name":"party:oak","ownerUserId":"100"}]}})");

	const TCHAR* const KitSocialSquattedChannels = TEXT(R"({"data":{"channels":[
		{"groupId":"12","name":"party:oakford","ownerUserId":"666"},
		{"groupId":"13","name":"party:oakford"}]}})");

	const TCHAR* const KitSocialMemberAnswer = TEXT(R"({"data":{
		"addTeamMember":{"groupMemberId":"900","groupId":"11","userId":"142","status":"active"},
		"addChannelMember":{"groupMemberId":"901","groupId":"21","userId":"142"},
		"joinTeam":{"groupMemberId":"900","groupId":"11","userId":"142","status":"active"},
		"joinChannel":{"groupMemberId":"901","groupId":"21","userId":"142"},
		"requestToJoinTeam":{"groupMemberId":"902","groupId":"12","userId":"142","status":"pending"},
		"setTeamMemberRoles":{"groupMemberId":"903","groupId":"12","userId":"142","status":"active"}}})");

	const TCHAR* const KitSocialRefused = TEXT(
		R"({"errors":[{"message":"Requires app-admin permission","extensions":{"code":"FORBIDDEN"}}],"data":null})");

	TPair<int32, FString> KitSocialAnswer(const TCHAR* Json)
	{
		return TPair<int32, FString>(200, Json);
	}

	bool KitSocialSame(FAutomationTestBase& Test, const TCHAR* What, const FString& Actual, const TCHAR* Expected)
	{
		return Test.TestTrue(What, Actual.Equals(Expected, ESearchCase::CaseSensitive));
	}

	struct FKitSocialSentCall
	{
		FString Query;
		TSharedPtr<FJsonObject> Variables;

		bool Is(const TCHAR* Operation) const
		{
			return Query.Contains(FString::Printf(TEXT(" %s("), Operation), ESearchCase::CaseSensitive);
		}

		TSharedPtr<FJsonObject> Source() const
		{
			const TSharedPtr<FJsonObject>* Input = nullptr;
			return Variables->TryGetObjectField(TEXT("input"), Input) ? *Input : Variables;
		}

		bool Has(const TCHAR* Field) const
		{
			return Source()->HasField(Field);
		}

		FString Str(const TCHAR* Field) const
		{
			FString Value;
			Source()->TryGetStringField(Field, Value);
			return Value;
		}

		FString Strings(const TCHAR* Field) const
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Source()->TryGetArrayField(Field, Values))
			{
				return FString(TEXT("<missing>"));
			}

			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Items.Add(Value->AsString());
			}
			return FString::Join(Items, TEXT(","));
		}
	};

	/** A social kit on a transient game instance whose every call is answered by a scripted client. */
	struct FKitSocialRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyChannels> Channels;
		TStrongObjectPtr<UCrowdyKitSocial> Kit;
		TSharedPtr<FCrowdyCppClient> Client;
		TArray<FKitSocialSentCall> Sent;

		FKitSocialRig()
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Channels(NewObject<UCrowdyChannels>(Instance.Get()))
			, Kit(NewObject<UCrowdyKitSocial>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200))
		{
			Kit->InitializeForTest(Client, nullptr, Channels.Get());
			if (Client)
			{
				Client->SetTestOnRequest([this](const FString&) { Record(); });
			}
		}

		void Answer(TArray<TPair<int32, FString>> Answers)
		{
			Client->SetTestResponseScript(MoveTemp(Answers));
		}

		void Drain()
		{
			for (int32 Pass = 0; Client && Pass < 32 && Client->NumPendingRequests() > 0; ++Pass)
			{
				Client->Poll();
			}
		}

	private:
		void Record()
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);

			FKitSocialSentCall Call;
			TSharedPtr<FJsonObject> Parsed;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Parsed) && Parsed.IsValid())
			{
				Parsed->TryGetStringField(TEXT("query"), Call.Query);
				const TSharedPtr<FJsonObject>* Variables = nullptr;
				if (Parsed->TryGetObjectField(TEXT("variables"), Variables))
				{
					Call.Variables = *Variables;
				}
			}
			if (!Call.Variables.IsValid())
			{
				Call.Variables = MakeShared<FJsonObject>();
			}
			Sent.Add(MoveTemp(Call));
		}
	};

	struct FKitSocialOutcome
	{
		int32 Done = 0;
		int32 Failed = 0;
		FCrowdyKitGroup Group;
		FCrowdyTeamError Error;

		FCrowdyKitGroupDone OnDone()
		{
			return FCrowdyKitGroupDone::CreateLambda([this](const FCrowdyKitGroup& Answered)
			{
				++Done;
				Group = Answered;
			});
		}

		FCrowdyKitFailed OnFail()
		{
			return FCrowdyKitFailed::CreateLambda([this](const FCrowdyTeamError& Why)
			{
				++Failed;
				Error = Why;
			});
		}
	};

	bool KitSocialAnswered(FAutomationTestBase& Test, const FKitSocialOutcome& Outcome, bool bSucceeded)
	{
		Test.TestEqual(TEXT("exactly one completion fired"), Outcome.Done + Outcome.Failed, 1);
		return Test.TestEqual(TEXT("and it is the expected one"), Outcome.Done, bSucceeded ? 1 : 0);
	}

	FString KitSocialAppId()
	{
		return CrowdyServiceApi::BigInt(GetDefault<UCrowdySDKDeveloperSettings>()->AppID);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialPartyCreateTest,
	"CrowdySDK.Services.KitSocial.PartyCreateSendsConvention", KitSocialTestFlags)
bool FCrowdyKitSocialPartyCreateTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Answer({
		KitSocialAnswer(TEXT(R"({"data":{"createTeam":{"groupId":"11","name":"party:oakford","ownerUserId":"100"}}})")),
		KitSocialAnswer(TEXT(R"({"data":{"createChannel":{"groupId":"21","name":"party:oakford","ownerUserId":"100"}}})")) });

	FKitSocialOutcome Outcome;
	Rig.Kit->CreateParty(TEXT("oakford"), Outcome.OnDone(), Outcome.OnFail());
	Rig.Drain();
	if (!KitSocialAnswered(*this, Outcome, true) || !TestEqual(TEXT("a team call then a channel call"), Rig.Sent.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("the team is created first"), Rig.Sent[0].Is(TEXT("CreateTeam")));
	KitSocialSame(*this, TEXT("under the party prefix"), Rig.Sent[0].Str(TEXT("name")), TEXT("party:oakford"));
	KitSocialSame(*this, TEXT("described as a party"), Rig.Sent[0].Str(TEXT("description")), TEXT("Party"));
	KitSocialSame(*this, TEXT("invite only"), Rig.Sent[0].Str(TEXT("membershipPolicy")), TEXT("invite"));
	KitSocialSame(*this, TEXT("in this app"), Rig.Sent[0].Str(TEXT("appId")), *KitSocialAppId());

	TestTrue(TEXT("the channel is created second"), Rig.Sent[1].Is(TEXT("CreateChannel")));
	KitSocialSame(*this, TEXT("under the same name"), Rig.Sent[1].Str(TEXT("name")), TEXT("party:oakford"));
	KitSocialSame(*this, TEXT("described as its chat"), Rig.Sent[1].Str(TEXT("description")), TEXT("Chat for party:oakford"));
	TestFalse(TEXT("with the app's own channel policy"), Rig.Sent[1].Has(TEXT("membershipPolicy")));

	KitSocialSame(*this, TEXT("the group carries the full name"), Outcome.Group.Name, TEXT("party:oakford"));
	TestEqual(TEXT("and the team id"), Outcome.Group.TeamId, static_cast<int64>(11));
	TestEqual(TEXT("and the channel id"), Outcome.Group.ChannelId, static_cast<int64>(21));
	TestEqual(TEXT("and the owner a game can check"), Outcome.Group.OwnerUserId, static_cast<int64>(100));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialGuildCreateTest,
	"CrowdySDK.Services.KitSocial.GuildCreateSendsConvention", KitSocialTestFlags)
bool FCrowdyKitSocialGuildCreateTest::RunTest(const FString& Parameters)
{
	const TPair<int32, FString> Team = KitSocialAnswer(TEXT(R"({"data":{"createTeam":{"groupId":"12","name":"guild:wardens"}}})"));
	const TPair<int32, FString> Channel = KitSocialAnswer(TEXT(R"({"data":{"createChannel":{"groupId":"22","name":"guild:wardens"}}})"));

	FKitSocialRig Default;
	if (!TestTrue(TEXT("the scripted client was built"), Default.Client.IsValid()))
	{
		return false;
	}
	Default.Answer({ Team, Channel });
	FKitSocialOutcome DefaultOutcome;
	Default.Kit->CreateGuild(TEXT("wardens"), ECrowdyTeamMembershipPolicy::Request, DefaultOutcome.OnDone(), DefaultOutcome.OnFail());
	Default.Drain();
	if (!KitSocialAnswered(*this, DefaultOutcome, true) || !TestEqual(TEXT("two calls"), Default.Sent.Num(), 2))
	{
		return false;
	}
	KitSocialSame(*this, TEXT("the guild prefix"), Default.Sent[0].Str(TEXT("name")), TEXT("guild:wardens"));
	KitSocialSame(*this, TEXT("described as a guild"), Default.Sent[0].Str(TEXT("description")), TEXT("Guild"));
	KitSocialSame(*this, TEXT("by request"), Default.Sent[0].Str(TEXT("membershipPolicy")), TEXT("request"));
	KitSocialSame(*this, TEXT("its chat channel shares the name"), Default.Sent[1].Str(TEXT("name")), TEXT("guild:wardens"));
	KitSocialSame(*this, TEXT("and says whose chat it is"), Default.Sent[1].Str(TEXT("description")), TEXT("Chat for guild:wardens"));

	FKitSocialRig Open;
	Open.Answer({ Team, Channel });
	FKitSocialOutcome OpenOutcome;
	Open.Kit->CreateGuild(TEXT("wardens"), ECrowdyTeamMembershipPolicy::Open, OpenOutcome.OnDone(), OpenOutcome.OnFail());
	Open.Drain();
	if (TestEqual(TEXT("an open guild sends one team call first"), Open.Sent.Num(), 2))
	{
		KitSocialSame(*this, TEXT("with the policy chosen"), Open.Sent[0].Str(TEXT("membershipPolicy")), TEXT("open"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialChannelCreateFailureTest,
	"CrowdySDK.Services.KitSocial.FailedChannelCreateFailsTheCall", KitSocialTestFlags)
bool FCrowdyKitSocialChannelCreateFailureTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Answer({
		KitSocialAnswer(TEXT(R"({"data":{"createTeam":{"groupId":"11","name":"party:oakford"}}})")),
		KitSocialAnswer(KitSocialRefused) });

	FKitSocialOutcome Outcome;
	Rig.Kit->CreateParty(TEXT("oakford"), Outcome.OnDone(), Outcome.OnFail());
	Rig.Drain();
	if (KitSocialAnswered(*this, Outcome, false))
	{
		TestTrue(TEXT("a party whose chat channel cannot be created fails with the channel's error"),
			Outcome.Error.Code == ECrowdyTeamErrorCode::Forbidden);
		TestEqual(TEXT("after the team and the channel were both tried"), Rig.Sent.Num(), 2);
	}

	FKitSocialRig NoTeam;
	NoTeam.Answer({ KitSocialAnswer(KitSocialRefused) });
	FKitSocialOutcome Refused;
	NoTeam.Kit->CreateParty(TEXT("oakford"), Refused.OnDone(), Refused.OnFail());
	NoTeam.Drain();
	if (KitSocialAnswered(*this, Refused, false))
	{
		TestTrue(TEXT("a refused team fails the call"), Refused.Error.Code == ECrowdyTeamErrorCode::Forbidden);
		TestEqual(TEXT("and no channel is created for it"), NoTeam.Sent.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialFindExactNameTest,
	"CrowdySDK.Services.KitSocial.FindMatchesTheExactFullName", KitSocialTestFlags)
bool FCrowdyKitSocialFindExactNameTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Party;
	if (!TestTrue(TEXT("the scripted client was built"), Party.Client.IsValid()))
	{
		return false;
	}
	Party.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialOakfordChannels) });
	FKitSocialOutcome Found;
	Party.Kit->FindParty(TEXT("oakford"), Found.OnDone(), Found.OnFail());
	Party.Drain();
	if (!KitSocialAnswered(*this, Found, true))
	{
		return false;
	}
	TestEqual(TEXT("neither the prefix collision nor the other case wins the team"), Found.Group.TeamId, static_cast<int64>(2));
	TestEqual(TEXT("and the channel of the same name made by the team's owner is paired, not a squatter's"), Found.Group.ChannelId, static_cast<int64>(10));
	TestEqual(TEXT("the group carries the team's owner"), Found.Group.OwnerUserId, static_cast<int64>(100));
	KitSocialSame(*this, TEXT("under the full name"), Found.Group.Name, TEXT("party:oakford"));
	if (TestEqual(TEXT("it listed the app's teams then its channels"), Party.Sent.Num(), 2))
	{
		TestTrue(TEXT("teams first"), Party.Sent[0].Is(TEXT("Teams")));
		TestTrue(TEXT("channels second"), Party.Sent[1].Is(TEXT("Channels")));
		KitSocialSame(*this, TEXT("for this app"), Party.Sent[0].Str(TEXT("appId")), *KitSocialAppId());
	}

	FKitSocialRig Prefix;
	Prefix.Answer({ KitSocialAnswer(KitSocialOakfordTeams) });
	FKitSocialOutcome Missing;
	Prefix.Kit->FindParty(TEXT("oakf"), Missing.OnDone(), Missing.OnFail());
	Prefix.Drain();
	if (KitSocialAnswered(*this, Missing, false))
	{
		TestTrue(TEXT("a name that only starts like a party is not found"), Missing.Error.Code == ECrowdyTeamErrorCode::NotFound);
		TestEqual(TEXT("and no channel is listed for it"), Prefix.Sent.Num(), 1);
	}

	FKitSocialRig Guild;
	Guild.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialOakfordChannels) });
	FKitSocialOutcome GuildFound;
	Guild.Kit->FindGuild(TEXT("oakford"), GuildFound.OnDone(), GuildFound.OnFail());
	Guild.Drain();
	if (KitSocialAnswered(*this, GuildFound, true))
	{
		TestEqual(TEXT("a guild is found under its own prefix"), GuildFound.Group.TeamId, static_cast<int64>(3));
		TestEqual(TEXT("and has no channel when none carries its name"), GuildFound.Group.ChannelId, static_cast<int64>(0));
	}

	FKitSocialRig Squatted;
	Squatted.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialSquattedChannels) });
	FKitSocialOutcome SquatOutcome;
	Squatted.Kit->FindParty(TEXT("oakford"), SquatOutcome.OnDone(), SquatOutcome.OnFail());
	Squatted.Drain();
	if (KitSocialAnswered(*this, SquatOutcome, true))
	{
		TestEqual(TEXT("channels made by someone else, or with no owner given, are not the party's chat"), SquatOutcome.Group.ChannelId, static_cast<int64>(0));
		TestEqual(TEXT("while the team is still answered"), SquatOutcome.Group.TeamId, static_cast<int64>(2));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialInviteOrderTest,
	"CrowdySDK.Services.KitSocial.InviteAndAcceptAddTeamThenChannel", KitSocialTestFlags)
bool FCrowdyKitSocialInviteOrderTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitGroup Party(TEXT("party:oakford"), 11, 21);

	FKitSocialRig Invite;
	if (!TestTrue(TEXT("the scripted client was built"), Invite.Client.IsValid()))
	{
		return false;
	}
	Invite.Answer({ KitSocialAnswer(KitSocialMemberAnswer), KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Invited;
	Invite.Kit->InviteToParty(Party, 142, Invited.OnDone(), Invited.OnFail());
	Invite.Drain();
	if (KitSocialAnswered(*this, Invited, true) && TestEqual(TEXT("an invite makes two calls"), Invite.Sent.Num(), 2))
	{
		TestTrue(TEXT("the team member is added first"), Invite.Sent[0].Is(TEXT("AddTeamMember")));
		KitSocialSame(*this, TEXT("to the party's team"), Invite.Sent[0].Str(TEXT("groupId")), TEXT("11"));
		KitSocialSame(*this, TEXT("the user is a string id"), Invite.Sent[0].Str(TEXT("userId")), TEXT("142"));
		TestTrue(TEXT("the channel member second"), Invite.Sent[1].Is(TEXT("AddChannelMember")));
		KitSocialSame(*this, TEXT("to the party's channel"), Invite.Sent[1].Str(TEXT("groupId")), TEXT("21"));
		KitSocialSame(*this, TEXT("the same user"), Invite.Sent[1].Str(TEXT("userId")), TEXT("142"));
	}

	FKitSocialRig NoChannel;
	NoChannel.Answer({ KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Bare;
	NoChannel.Kit->InviteToParty(FCrowdyKitGroup(TEXT("party:oakford"), 11, 0), 142, Bare.OnDone(), Bare.OnFail());
	NoChannel.Drain();
	if (KitSocialAnswered(*this, Bare, true))
	{
		TestEqual(TEXT("a party without a channel is invited by its team alone"), NoChannel.Sent.Num(), 1);
	}

	FKitSocialRig Accept;
	Accept.Answer({ KitSocialAnswer(KitSocialMemberAnswer), KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Accepted;
	Accept.Kit->AcceptIntoGuild(FCrowdyKitGroup(TEXT("guild:wardens"), 12, 22), 142, Accepted.OnDone(), Accepted.OnFail());
	Accept.Drain();
	if (KitSocialAnswered(*this, Accepted, true) && TestEqual(TEXT("an acceptance makes two calls"), Accept.Sent.Num(), 2))
	{
		TestTrue(TEXT("the request is approved on the team first"), Accept.Sent[0].Is(TEXT("AddTeamMember")));
		KitSocialSame(*this, TEXT("the guild's team"), Accept.Sent[0].Str(TEXT("groupId")), TEXT("12"));
		TestTrue(TEXT("then the user joins the chat"), Accept.Sent[1].Is(TEXT("AddChannelMember")));
		KitSocialSame(*this, TEXT("the guild's channel"), Accept.Sent[1].Str(TEXT("groupId")), TEXT("22"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialJoinLeaveOrderTest,
	"CrowdySDK.Services.KitSocial.JoinTeamThenChannelLeaveChannelThenTeam", KitSocialTestFlags)
bool FCrowdyKitSocialJoinLeaveOrderTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitGroup Party(TEXT("party:oakford"), 11, 21);

	FKitSocialRig Join;
	if (!TestTrue(TEXT("the scripted client was built"), Join.Client.IsValid()))
	{
		return false;
	}
	Join.Answer({ KitSocialAnswer(KitSocialMemberAnswer), KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Joined;
	Join.Kit->JoinParty(Party, Joined.OnDone(), Joined.OnFail());
	Join.Drain();
	if (KitSocialAnswered(*this, Joined, true) && TestEqual(TEXT("a join makes two calls"), Join.Sent.Num(), 2))
	{
		TestTrue(TEXT("the team is joined first"), Join.Sent[0].Is(TEXT("JoinTeam")));
		KitSocialSame(*this, TEXT("the party's team"), Join.Sent[0].Str(TEXT("groupId")), TEXT("11"));
		TestTrue(TEXT("the channel second"), Join.Sent[1].Is(TEXT("JoinChannel")));
		KitSocialSame(*this, TEXT("the party's channel"), Join.Sent[1].Str(TEXT("groupId")), TEXT("21"));
	}

	FKitSocialRig Leave;
	Leave.Answer({
		KitSocialAnswer(TEXT(R"({"data":{"leaveChannel":true}})")),
		KitSocialAnswer(TEXT(R"({"data":{"leaveTeam":true}})")) });
	FKitSocialOutcome Left;
	Leave.Kit->LeaveParty(Party, Left.OnDone(), Left.OnFail());
	Leave.Drain();
	if (KitSocialAnswered(*this, Left, true) && TestEqual(TEXT("a leave makes two calls"), Leave.Sent.Num(), 2))
	{
		TestTrue(TEXT("the channel is left first"), Leave.Sent[0].Is(TEXT("LeaveChannel")));
		KitSocialSame(*this, TEXT("the party's channel"), Leave.Sent[0].Str(TEXT("groupId")), TEXT("21"));
		TestTrue(TEXT("the team second"), Leave.Sent[1].Is(TEXT("LeaveTeam")));
		KitSocialSame(*this, TEXT("the party's team"), Leave.Sent[1].Str(TEXT("groupId")), TEXT("11"));
	}

	FKitSocialRig Stuck;
	Stuck.Answer({ KitSocialAnswer(KitSocialRefused) });
	FKitSocialOutcome Refused;
	Stuck.Kit->LeaveParty(Party, Refused.OnDone(), Refused.OnFail());
	Stuck.Drain();
	if (KitSocialAnswered(*this, Refused, false))
	{
		TestEqual(TEXT("a refused channel leave stops before the team is left"), Stuck.Sent.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialGuildRolesTest,
	"CrowdySDK.Services.KitSocial.GuildJoinRequestsAndPromoteReplacesRoles", KitSocialTestFlags)
bool FCrowdyKitSocialGuildRolesTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitGroup Guild(TEXT("guild:wardens"), 12, 22);

	FKitSocialRig Request;
	if (!TestTrue(TEXT("the scripted client was built"), Request.Client.IsValid()))
	{
		return false;
	}
	Request.Answer({ KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Requested;
	Request.Kit->JoinGuild(Guild, Requested.OnDone(), Requested.OnFail());
	Request.Drain();
	if (KitSocialAnswered(*this, Requested, true) && TestEqual(TEXT("one call"), Request.Sent.Num(), 1))
	{
		TestTrue(TEXT("joining a guild asks to join its team"), Request.Sent[0].Is(TEXT("RequestToJoinTeam")));
		KitSocialSame(*this, TEXT("the guild's team"), Request.Sent[0].Str(TEXT("groupId")), TEXT("12"));
	}

	FKitSocialRig Promote;
	Promote.Answer({ KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Promoted;
	Promote.Kit->PromoteInGuild(Guild, 142, { 7, 9 }, Promoted.OnDone(), Promoted.OnFail());
	Promote.Drain();
	if (KitSocialAnswered(*this, Promoted, true) && TestEqual(TEXT("one call"), Promote.Sent.Num(), 1))
	{
		TestTrue(TEXT("promotion sets the member's roles"), Promote.Sent[0].Is(TEXT("SetTeamMemberRoles")));
		KitSocialSame(*this, TEXT("on the guild's team"), Promote.Sent[0].Str(TEXT("groupId")), TEXT("12"));
		KitSocialSame(*this, TEXT("for the member"), Promote.Sent[0].Str(TEXT("userId")), TEXT("142"));
		KitSocialSame(*this, TEXT("to exactly the roles given, as string ids"), Promote.Sent[0].Strings(TEXT("roleIds")), TEXT("7,9"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialNoTeamTest,
	"CrowdySDK.Services.KitSocial.TeamCallsOnAGroupWithoutATeamAreNotFound", KitSocialTestFlags)
bool FCrowdyKitSocialNoTeamTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	const FCrowdyKitGroup Room(TEXT("lobby"), 0, 31);
	FKitSocialOutcome Outcome;
	int32 MembersFailed = 0;
	Rig.Kit->InviteToParty(Room, 142, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->JoinParty(Room, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->LeaveParty(Room, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->JoinGuild(Room, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->AcceptIntoGuild(Room, 142, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->PromoteInGuild(Room, 142, { 7 }, Outcome.OnDone(), Outcome.OnFail());
	Rig.Kit->ClaimGuildTerritory(Room, 77, {}, 0, Outcome.OnDone(), Outcome.OnFail());
	const FCrowdyKitMembersDone CountMembers = FCrowdyKitMembersDone::CreateLambda(
		[&MembersFailed](const FCrowdyKitGroup&, const TArray<FCrowdyTeamMember>&) { --MembersFailed; });
	const FCrowdyKitFailed CountFailure = FCrowdyKitFailed::CreateLambda(
		[&MembersFailed](const FCrowdyTeamError&) { ++MembersFailed; });
	Rig.Kit->GetPartyMembers(Room, CountMembers, CountFailure);
	Rig.Kit->GetGuildRoster(Room, CountMembers, CountFailure);
	Rig.Drain();

	TestEqual(TEXT("each team call on a group with no team failed, once"), Outcome.Failed, 7);
	TestEqual(TEXT("none succeeded"), Outcome.Done, 0);
	TestEqual(TEXT("and so did both member lists"), MembersFailed, 2);
	TestTrue(TEXT("with Not Found"), Outcome.Error.Code == ECrowdyTeamErrorCode::NotFound);
	TestEqual(TEXT("without a request"), Rig.Sent.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialClaimTerritoryTest,
	"CrowdySDK.Services.KitSocial.ClaimTerritorySendsGridGrant", KitSocialTestFlags)
bool FCrowdyKitSocialClaimTerritoryTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitGroup Guild(TEXT("guild:wardens"), 12, 22);
	const TPair<int32, FString> Granted = KitSocialAnswer(TEXT(R"({"data":{"assignGroupToGrid":[{"appId":"1"}]}})"));

	FKitSocialRig Default;
	if (!TestTrue(TEXT("the scripted client was built"), Default.Client.IsValid()))
	{
		return false;
	}
	Default.Answer({ Granted });
	FKitSocialOutcome DefaultOutcome;
	Default.Kit->ClaimGuildTerritory(Guild, 77, {}, 0, DefaultOutcome.OnDone(), DefaultOutcome.OnFail());
	Default.Drain();
	if (KitSocialAnswered(*this, DefaultOutcome, true) && TestEqual(TEXT("one call"), Default.Sent.Num(), 1))
	{
		const FKitSocialSentCall& Call = Default.Sent[0];
		TestTrue(TEXT("the grant is assigned to the grid"), Call.Is(TEXT("AssignGroupToGrid")));
		KitSocialSame(*this, TEXT("for this app"), Call.Str(TEXT("appId")), *KitSocialAppId());
		KitSocialSame(*this, TEXT("on the grid"), Call.Str(TEXT("gridId")), TEXT("77"));
		KitSocialSame(*this, TEXT("to the guild's team"), Call.Str(TEXT("groupId")), TEXT("12"));
		KitSocialSame(*this, TEXT("with the default keys"), Call.Strings(TEXT("permissionKeys")), TEXT("access,update_voxel_data"));
		TestFalse(TEXT("to every member"), Call.Has(TEXT("groupRoleId")));
		TestEqual(TEXT("the guild is answered"), DefaultOutcome.Group.TeamId, static_cast<int64>(12));
	}

	FKitSocialRig Scoped;
	Scoped.Answer({ Granted });
	FKitSocialOutcome ScopedOutcome;
	Scoped.Kit->ClaimGuildTerritory(Guild, 77, { TEXT("update_voxel_data") }, 5, ScopedOutcome.OnDone(), ScopedOutcome.OnFail());
	Scoped.Drain();
	if (KitSocialAnswered(*this, ScopedOutcome, true) && TestEqual(TEXT("one call"), Scoped.Sent.Num(), 1))
	{
		KitSocialSame(*this, TEXT("the keys given replace the defaults"), Scoped.Sent[0].Strings(TEXT("permissionKeys")), TEXT("update_voxel_data"));
		KitSocialSame(*this, TEXT("scoped to the role"), Scoped.Sent[0].Str(TEXT("groupRoleId")), TEXT("5"));
	}

	FKitSocialRig Refused;
	Refused.Answer({ KitSocialAnswer(KitSocialRefused) });
	FKitSocialOutcome RefusedOutcome;
	Refused.Kit->ClaimGuildTerritory(Guild, 77, {}, 0, RefusedOutcome.OnDone(), RefusedOutcome.OnFail());
	Refused.Drain();
	if (KitSocialAnswered(*this, RefusedOutcome, false))
	{
		TestTrue(TEXT("a player without grid admin rights is refused"), RefusedOutcome.Error.Code == ECrowdyTeamErrorCode::Forbidden);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialChatRoomTest,
	"CrowdySDK.Services.KitSocial.ChatRoomFindsByExactNameOrCreates", KitSocialTestFlags)
bool FCrowdyKitSocialChatRoomTest::RunTest(const FString& Parameters)
{
	const TPair<int32, FString> Listed = KitSocialAnswer(TEXT(R"({"data":{"channels":[
		{"groupId":"5","name":"lobby-2"},
		{"groupId":"7","name":"Lobby"},
		{"groupId":"6","name":"lobby"}]}})"));

	FKitSocialRig Existing;
	if (!TestTrue(TEXT("the scripted client was built"), Existing.Client.IsValid()))
	{
		return false;
	}
	Existing.Answer({ Listed, KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Found;
	Existing.Kit->FindOrCreateChatRoom(TEXT("lobby"), Found.OnDone(), Found.OnFail());
	Existing.Drain();
	if (KitSocialAnswered(*this, Found, true) && TestEqual(TEXT("it lists then joins"), Existing.Sent.Num(), 2))
	{
		TestTrue(TEXT("the app's channels are listed"), Existing.Sent[0].Is(TEXT("Channels")));
		TestTrue(TEXT("the existing room is joined"), Existing.Sent[1].Is(TEXT("JoinChannel")));
		KitSocialSame(*this, TEXT("the one named exactly"), Existing.Sent[1].Str(TEXT("groupId")), TEXT("6"));
		TestEqual(TEXT("a room has a channel"), Found.Group.ChannelId, static_cast<int64>(6));
		TestEqual(TEXT("and no team"), Found.Group.TeamId, static_cast<int64>(0));
		KitSocialSame(*this, TEXT("under its own name, with no prefix"), Found.Group.Name, TEXT("lobby"));

		Existing.Answer({ Listed });
		FKitSocialOutcome Repeat;
		Existing.Kit->FindOrCreateChatRoom(TEXT("lobby"), Repeat.OnDone(), Repeat.OnFail());
		Existing.Drain();
		if (KitSocialAnswered(*this, Repeat, true))
		{
			TestEqual(TEXT("a room already joined is listed but not joined again"), Existing.Sent.Num(), 3);
		}

		Existing.Kit->SetUserIdForTest(9);
		Existing.Answer({ Listed, KitSocialAnswer(KitSocialMemberAnswer) });
		FKitSocialOutcome OtherAccount;
		Existing.Kit->FindOrCreateChatRoom(TEXT("lobby"), OtherAccount.OnDone(), OtherAccount.OnFail());
		Existing.Drain();
		if (KitSocialAnswered(*this, OtherAccount, true))
		{
			TestEqual(TEXT("another account has not joined it, whatever the last one did"), Existing.Sent.Num(), 5);
		}
	}

	FKitSocialRig Taken;
	Taken.Answer({ Listed, KitSocialAnswer(TEXT(R"({"errors":[{"message":"Already member of channel"}],"data":null})")) });
	FKitSocialOutcome Again;
	Taken.Kit->FindOrCreateChatRoom(TEXT("lobby"), Again.OnDone(), Again.OnFail());
	Taken.Drain();
	KitSocialAnswered(*this, Again, true);

	FKitSocialRig Absent;
	Absent.Answer({ Listed, KitSocialAnswer(TEXT(R"({"data":{"createChannel":{"groupId":"31","name":"lobby-3"}}})")) });
	FKitSocialOutcome Created;
	Absent.Kit->FindOrCreateChatRoom(TEXT("lobby-3"), Created.OnDone(), Created.OnFail());
	Absent.Drain();
	if (KitSocialAnswered(*this, Created, true) && TestEqual(TEXT("it lists then creates"), Absent.Sent.Num(), 2))
	{
		TestTrue(TEXT("the room is created"), Absent.Sent[1].Is(TEXT("CreateChannel")));
		KitSocialSame(*this, TEXT("under the name asked for, with no prefix"), Absent.Sent[1].Str(TEXT("name")), TEXT("lobby-3"));
		TestFalse(TEXT("and no policy of its own"), Absent.Sent[1].Has(TEXT("membershipPolicy")));
		TestEqual(TEXT("the new channel is answered"), Created.Group.ChannelId, static_cast<int64>(31));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialChatRefusalsTest,
	"CrowdySDK.Services.KitSocial.ChatRefusals", KitSocialTestFlags)
bool FCrowdyKitSocialChatRefusalsTest::RunTest(const FString& Parameters)
{
	using Result = ECrowdyKitChatResult;
	auto Check = [](const FString& Text, int64 Channel = 31, bool bConnected = true)
	{
		return UCrowdyKitSocial::CheckChat(Text, Channel, bConnected);
	};
	auto Repeat = [](TCHAR Character, int32 Count)
	{
		FString Text;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Text.AppendChar(Character);
		}
		return Text;
	};

	TestTrue(TEXT("1024 bytes is sent"), Check(Repeat(TEXT('a'), 1024)) == Result::Sent);
	TestTrue(TEXT("1025 bytes is too long"), Check(Repeat(TEXT('a'), 1025)) == Result::TooLong);
	TestTrue(TEXT("1024 bytes of two-byte characters is sent"), Check(Repeat(TCHAR(0x00E9), 512)) == Result::Sent);
	TestTrue(TEXT("1026 bytes of two-byte characters is too long"), Check(Repeat(TCHAR(0x00E9), 513)) == Result::TooLong);
	TestTrue(TEXT("1023 bytes of three-byte characters is sent"), Check(Repeat(TCHAR(0x20AC), 341)) == Result::Sent);
	TestTrue(TEXT("1026 bytes of three-byte characters is too long though only 342 characters"), Check(Repeat(TCHAR(0x20AC), 342)) == Result::TooLong);

	TestTrue(TEXT("empty text is refused first"), Check(FString(), 0, false) == Result::EmptyText);
	TestTrue(TEXT("a long text is refused before the channel is looked at"), Check(Repeat(TEXT('a'), 1025), 0) == Result::TooLong);
	TestTrue(TEXT("a group with no channel has nowhere to send"), Check(TEXT("hi"), 0) == Result::NoChannel);
	TestTrue(TEXT("a down connection is refused"), Check(TEXT("hi"), 31, false) == Result::NotConnected);
	TestTrue(TEXT("a connected send to a channel goes out, whoever the cache says is in it"), Check(TEXT("hi")) == Result::Sent);

	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FCrowdyKitGroup Party(TEXT("party:oakford"), 11, 21);
	TestTrue(TEXT("Send Chat refuses empty text"), Rig.Kit->SendChat(Party, FString()) == Result::EmptyText);
	TestTrue(TEXT("and text over the limit"), Rig.Kit->SendChat(Party, Repeat(TEXT('a'), 1025)) == Result::TooLong);
	TestTrue(TEXT("and a group with no channel"), Rig.Kit->SendChat(FCrowdyKitGroup(TEXT("party:oakford"), 11, 0), TEXT("hi")) == Result::NoChannel);
	TestTrue(TEXT("and a send with no realtime connection"), Rig.Kit->SendChat(Party, TEXT("hi")) == Result::NotConnected);
	for (uint8 Value = 0; Value <= static_cast<uint8>(Result::NotConnected); ++Value)
	{
		TestFalse(TEXT("every verdict has a reason"), UCrowdyKitSocial::DescribeChatResult(static_cast<Result>(Value)).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialChatMessageTest,
	"CrowdySDK.Services.KitSocial.ChatMessageDecodesAndResolvesGroup", KitSocialTestFlags)
bool FCrowdyKitSocialChatMessageTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyKitSocialTestListener> Listener(
		NewObject<UCrowdyKitSocialTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	Rig.Kit->OnChatMessage.AddDynamic(Listener.Get(), &UCrowdyKitSocialTestListener::HandleChat);

	Rig.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialOakfordChannels) });
	FKitSocialOutcome Found;
	Rig.Kit->FindParty(TEXT("oakford"), Found.OnDone(), Found.OnFail());
	Rig.Drain();
	if (!KitSocialAnswered(*this, Found, true))
	{
		return false;
	}

	FString Spoken(TEXT("h"));
	Spoken.AppendChar(TCHAR(0x00E9));
	Spoken += TEXT("llo ");
	Spoken.AppendChar(TCHAR(0x20AC));
	const FTCHARToUTF8 Utf8(*Spoken);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

	Rig.Kit->HandleChannelMessage(10, TEXT("uuid-a"), Bytes);
	Rig.Kit->HandleChannelMessage(99, TEXT("uuid-b"), Bytes);
	Rig.Kit->HandleChannelMessage(10, TEXT("uuid-c"), TArray<uint8>());
	if (!TestEqual(TEXT("only the kit's own channel announces, and only with text"), Listener->Texts.Num(), 1))
	{
		return false;
	}

	KitSocialSame(*this, TEXT("the text is decoded from UTF-8"), Listener->Texts[0], *Spoken);
	KitSocialSame(*this, TEXT("the sender comes through"), Listener->Senders[0], TEXT("uuid-a"));
	KitSocialSame(*this, TEXT("a channel the kit found is named"), Listener->Groups[0].Name, TEXT("party:oakford"));
	TestEqual(TEXT("with its team"), Listener->Groups[0].TeamId, static_cast<int64>(2));
	TestEqual(TEXT("and its channel"), Listener->Groups[0].ChannelId, static_cast<int64>(10));

	TestEqual(TEXT("and the owner"), Listener->Groups[0].OwnerUserId, static_cast<int64>(100));

	Rig.Answer({
		KitSocialAnswer(TEXT(R"({"data":{"channels":[]}})")),
		KitSocialAnswer(TEXT(R"({"data":{"createChannel":{"groupId":"31","name":"lobby","ownerUserId":"100"}}})")) });
	FKitSocialOutcome Room;
	Rig.Kit->FindOrCreateChatRoom(TEXT("lobby"), Room.OnDone(), Room.OnFail());
	Rig.Drain();
	if (KitSocialAnswered(*this, Room, true))
	{
		Rig.Kit->HandleChannelMessage(31, TEXT("uuid-d"), Bytes);
		if (TestEqual(TEXT("a room the player created announces too"), Listener->Texts.Num(), 2))
		{
			KitSocialSame(*this, TEXT("under its own name"), Listener->Groups[1].Name, TEXT("lobby"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialRepeatedStepsTest,
	"CrowdySDK.Services.KitSocial.HalfFinishedPairCallsCanBeRepeated", KitSocialTestFlags)
bool FCrowdyKitSocialRepeatedStepsTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitGroup Party(TEXT("party:oakford"), 11, 21);
	const TPair<int32, FString> Already = KitSocialAnswer(TEXT(R"({"errors":[{"message":"User is already member of this group"}],"data":null})"));
	const TPair<int32, FString> NotIn = KitSocialAnswer(TEXT(R"({"errors":[{"message":"You are not a member of this group"}],"data":null})"));

	FKitSocialRig Join;
	if (!TestTrue(TEXT("the scripted client was built"), Join.Client.IsValid()))
	{
		return false;
	}
	Join.Answer({ Already, KitSocialAnswer(KitSocialMemberAnswer) });
	FKitSocialOutcome Joined;
	Join.Kit->JoinParty(Party, Joined.OnDone(), Joined.OnFail());
	Join.Drain();
	if (KitSocialAnswered(*this, Joined, true))
	{
		TestEqual(TEXT("a join whose team step was already done goes on to the channel"), Join.Sent.Num(), 2);
	}

	FKitSocialRig Invite;
	Invite.Answer({ KitSocialAnswer(KitSocialMemberAnswer), Already });
	FKitSocialOutcome Invited;
	Invite.Kit->InviteToParty(Party, 142, Invited.OnDone(), Invited.OnFail());
	Invite.Drain();
	KitSocialAnswered(*this, Invited, true);

	FKitSocialRig Leave;
	Leave.Answer({ NotIn, NotIn });
	FKitSocialOutcome Left;
	Leave.Kit->LeaveParty(Party, Left.OnDone(), Left.OnFail());
	Leave.Drain();
	if (KitSocialAnswered(*this, Left, true))
	{
		TestEqual(TEXT("a leave of a group already left runs both steps and succeeds"), Leave.Sent.Num(), 2);
	}

	FKitSocialRig Promote;
	Promote.Answer({ Already });
	FKitSocialOutcome Promoted;
	Promote.Kit->PromoteInGuild(FCrowdyKitGroup(TEXT("guild:wardens"), 12, 22), 142, { 7 }, Promoted.OnDone(), Promoted.OnFail());
	Promote.Drain();
	KitSocialAnswered(*this, Promoted, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialAccountChangeTest,
	"CrowdySDK.Services.KitSocial.AccountChangeDropsStateAndFailsInFlightReplies", KitSocialTestFlags)
bool FCrowdyKitSocialAccountChangeTest::RunTest(const FString& Parameters)
{
	FKitSocialRig Rig;
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Kit->SetUserIdForTest(1);

	Rig.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialOakfordChannels) });
	FKitSocialOutcome First;
	Rig.Kit->FindParty(TEXT("oakford"), First.OnDone(), First.OnFail());
	Rig.Drain();
	if (!KitSocialAnswered(*this, First, true) || !TestEqual(TEXT("the kit learned the group"), Rig.Kit->NumKnownGroupsForTest(), 1))
	{
		return false;
	}

	Rig.Answer({ KitSocialAnswer(KitSocialOakfordTeams), KitSocialAnswer(KitSocialOakfordChannels) });
	FKitSocialOutcome Second;
	Rig.Kit->FindParty(TEXT("oakford"), Second.OnDone(), Second.OnFail());
	Rig.Kit->SetUserIdForTest(2);
	Rig.Drain();
	if (KitSocialAnswered(*this, Second, false))
	{
		TestTrue(TEXT("a reply sent for the account that left fails"), Second.Error.Code == ECrowdyTeamErrorCode::NetworkError);
	}
	TestEqual(TEXT("and nothing learned under it is kept"), Rig.Kit->NumKnownGroupsForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialSenderTest,
	"CrowdySDK.Services.KitSocial.ChatSenderFallsBackToTheAccountId", KitSocialTestFlags)
bool FCrowdyKitSocialSenderTest::RunTest(const FString& Parameters)
{
	const FString Session(TEXT("0123456789abcdef0123456789abcdef"));
	KitSocialSame(*this, TEXT("a resolved session uuid is the sender"), UCrowdyKitSocial::SenderUuidFor(Session, 5), *Session);

	const FString Derived = UCrowdyKitSocial::SenderUuidFor(FString(), 5);
	KitSocialSame(*this, TEXT("before the entity resolves the sender comes from the account"), Derived,
		*UHelperFunctions::GetDeterministicID(5).ToString(EGuidFormats::Digits));
	TestFalse(TEXT("so it is never empty or all zero"), Derived.IsEmpty() || Derived.Equals(TEXT("00000000000000000000000000000000")));
	KitSocialSame(*this, TEXT("an all-zero session uuid is not trusted either"),
		UCrowdyKitSocial::SenderUuidFor(TEXT("00000000000000000000000000000000"), 5), *Derived);
	TestTrue(TEXT("without an account there is no sender"), UCrowdyKitSocial::SenderUuidFor(FString(), 0).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSocialNoClientTest,
	"CrowdySDK.Services.KitSocial.NoClientFailsOnceWithNetworkError", KitSocialTestFlags)
bool FCrowdyKitSocialNoClientTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyKitSocial> Kit(NewObject<UCrowdyKitSocial>(Instance.Get()));
	Kit->InitializeForTest(nullptr);

	FKitSocialOutcome Outcome;
	Kit->FindParty(TEXT("oakford"), Outcome.OnDone(), Outcome.OnFail());
	Kit->CreateGuild(TEXT("wardens"), ECrowdyTeamMembershipPolicy::Request, Outcome.OnDone(), Outcome.OnFail());
	Kit->JoinParty(FCrowdyKitGroup(TEXT("party:oakford"), 11, 21), Outcome.OnDone(), Outcome.OnFail());
	Kit->FindOrCreateChatRoom(TEXT("lobby"), Outcome.OnDone(), Outcome.OnFail());

	TestEqual(TEXT("each call failed, once, without a poll"), Outcome.Failed, 4);
	TestEqual(TEXT("and none succeeded"), Outcome.Done, 0);
	TestTrue(TEXT("because no client was available"), Outcome.Error.Code == ECrowdyTeamErrorCode::NetworkError);
	return true;
}

#endif
