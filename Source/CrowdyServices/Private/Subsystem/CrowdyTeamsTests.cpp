#include "Subsystem/CrowdyTeams.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyTeamsTestTypes.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags TeamsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const TeamsOneTeamAnswer = TEXT(R"({"data":{"myTeams":[{"group":{"groupId":"7","name":"Reavers"}}]}})");

	const TCHAR* const TeamsOtherTeamAnswer = TEXT(R"({"data":{"myTeams":[{"group":{"groupId":"8","name":"Wardens"}}]}})");

	const TCHAR* const TeamsUpdateAnswer = TEXT(R"({"data":{
		"updateTeam":{"groupId":"7","name":"Reavers Reborn"},
		"myTeams":[{"group":{"groupId":"7","name":"Reavers Reborn"}}]}})");

	// One body answers every operation the membership tests send, so each reads its own field from it.
	const TCHAR* const TeamsMembershipAnswer = TEXT(R"({"data":{
		"joinTeam":{"groupMemberId":"900","groupId":"7","userId":"142","status":"active"},
		"requestToJoinTeam":{"groupMemberId":"901","groupId":"8","userId":"142","status":"pending"},
		"myTeams":[{"group":{"groupId":"7","name":"Reavers"}}]}})");

	/** A teams subsystem on a transient game instance whose every call is answered by a scripted client. */
	struct FCrowdyTeamsTestRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyTeams> Teams;
		TStrongObjectPtr<UCrowdyTeamsTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FCrowdyTeamsTestRig(const TCHAR* CannedAnswer)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Teams(NewObject<UCrowdyTeams>(Instance.Get()))
			, Listener(NewObject<UCrowdyTeamsTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(CannedAnswer, 200))
		{
			Teams->InitializeForTest(Client);
			Teams->OnMyTeamsCacheChanged.AddDynamic(Listener.Get(), &UCrowdyTeamsTestListener::HandleCacheChanged);
		}

		/** Delivers every answer, including those to requests a completion sends. */
		void Drain()
		{
			for (int32 Pass = 0; Client && Pass < 8 && Client->NumPendingRequests() > 0; ++Pass)
			{
				Client->Poll();
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsClearEmptiesCacheTest,
	"CrowdySDK.CrowdyServices.TeamsClearEmptiesCache", TeamsTestFlags)
bool FCrowdyTeamsClearEmptiesCacheTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsOneTeamAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Teams->ClearMyTeamsCache();
	TestEqual(TEXT("clearing a cache that was never filled announces nothing"), Rig.Listener->CacheEvents.Num(), 0);

	Rig.Teams->GetMyTeams(FOnMyTeamsSuccess(), FOnTeamError());
	Rig.Drain();
	if (!TestTrue(TEXT("the answer fills the cache"), Rig.Teams->HasCachedTeams()))
	{
		return false;
	}
	TestTrue(TEXT("with the player's team"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("and announces it"), Rig.Listener->CacheEvents.Num(), 1);

	Rig.Teams->ClearMyTeamsCache();
	TestFalse(TEXT("a clear leaves the cache unpopulated"), Rig.Teams->HasCachedTeams());
	TestEqual(TEXT("and empty"), Rig.Teams->GetCachedMyTeams().Num(), 0);
	TestFalse(TEXT("so the player is in no team"), Rig.Teams->IsPlayerInTeam(7));
	if (TestEqual(TEXT("and announces the change"), Rig.Listener->CacheEvents.Num(), 2))
	{
		TestEqual(TEXT("with no teams"), Rig.Listener->CacheEvents[1].Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsAnswerBeforeClearNotCachedTest,
	"CrowdySDK.CrowdyServices.TeamsAnswerBeforeClearNotCached", TeamsTestFlags)
bool FCrowdyTeamsAnswerBeforeClearNotCachedTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsOneTeamAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnMyTeamsSuccess OnAnswered;
	OnAnswered.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleMyTeams);
	Rig.Teams->GetMyTeams(OnAnswered, FOnTeamError());
	Rig.Teams->ClearMyTeamsCache();
	Rig.Drain();

	TestEqual(TEXT("the caller still hears its answer"), Rig.Listener->MyTeamsAnswers.Num(), 1);
	TestFalse(TEXT("an answer to a request sent before the clear does not fill the cache"), Rig.Teams->HasCachedTeams());
	TestFalse(TEXT("so the player is in no team"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("and nothing is announced"), Rig.Listener->CacheEvents.Num(), 0);

	Rig.Teams->GetMyTeams(FOnMyTeamsSuccess(), FOnTeamError());
	Rig.Drain();
	TestTrue(TEXT("a request sent after the clear fills it"), Rig.Teams->IsPlayerInTeam(7));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsMembershipChangeRefreshesCacheTest,
	"CrowdySDK.CrowdyServices.TeamsMembershipChangeRefreshesCache", TeamsTestFlags)
bool FCrowdyTeamsMembershipChangeRefreshesCacheTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsMembershipAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnTeamMemberSuccess OnMember;
	OnMember.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleMember);
	FOnTeamError OnFailed;
	OnFailed.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleError);

	// A request to join leaves the player pending, so their teams have not changed yet.
	Rig.Teams->RequestToJoinTeam(8, OnMember, OnFailed);
	Rig.Drain();
	TestEqual(TEXT("the caller hears the request"), Rig.Listener->Successes, 1);
	TestFalse(TEXT("a pending request does not refresh the cache"), Rig.Teams->HasCachedTeams());

	Rig.Teams->JoinTeam(7, OnMember, OnFailed);
	Rig.Drain();
	TestEqual(TEXT("the caller hears the join"), Rig.Listener->Successes, 2);
	TestTrue(TEXT("a join refreshes the cache"), Rig.Teams->HasCachedTeams());
	TestTrue(TEXT("which now holds the joined team"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("and announces it once"), Rig.Listener->CacheEvents.Num(), 1);

	TArray<TPair<int32, FString>> UpdateAnswers;
	UpdateAnswers.Emplace(200, TEXT(R"({"data":{"updateTeam":{"groupId":"7","name":"Reavers Reborn"}}})"));
	UpdateAnswers.Emplace(200, TEXT(R"({"data":{"myTeams":[{"group":{"groupId":"7","name":"Reavers Reborn"}}]}})"));
	Rig.Client->SetTestResponseScript(MoveTemp(UpdateAnswers));

	FOnTeamSuccess OnTeam;
	OnTeam.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleTeam);
	Rig.Teams->UpdateTeam(7, TEXT("Reavers Reborn"), FString(), OnTeam, OnFailed);
	Rig.Drain();
	TestEqual(TEXT("the caller hears the update"), Rig.Listener->Successes, 3);
	FCrowdyTeamMembership Renamed;
	TestTrue(TEXT("an update refreshes the cache"), Rig.Teams->GetMyTeamById(7, Renamed)
		&& Renamed.Team.Name.Equals(TEXT("Reavers Reborn"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("and announces it"), Rig.Listener->CacheEvents.Num(), 2);

	TArray<TPair<int32, FString>> LeaveAnswers;
	LeaveAnswers.Emplace(200, TEXT(R"({"data":{"leaveTeam":true}})"));
	LeaveAnswers.Emplace(200, TEXT(R"({"data":{"myTeams":[]}})"));
	Rig.Client->SetTestResponseScript(MoveTemp(LeaveAnswers));

	FOnTeamVoidSuccess OnLeft;
	OnLeft.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleDone);
	Rig.Teams->LeaveTeam(7, OnLeft, OnFailed);
	Rig.Drain();
	TestEqual(TEXT("the caller hears the leave"), Rig.Listener->Successes, 4);
	TestTrue(TEXT("a leave refreshes the cache"), Rig.Teams->HasCachedTeams());
	TestFalse(TEXT("which no longer holds the team left"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("and announces it"), Rig.Listener->CacheEvents.Num(), 3);
	TestEqual(TEXT("nothing failed"), Rig.Listener->Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsOlderAnswerNotCachedTest,
	"CrowdySDK.CrowdyServices.TeamsOlderAnswerNotCached", TeamsTestFlags)
bool FCrowdyTeamsOlderAnswerNotCachedTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsOneTeamAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// The second request is sent while the first is in flight, and its answer is consumed and delivered first.
	TArray<TPair<int32, FString>> Answers;
	Answers.Emplace(200, TeamsOtherTeamAnswer);
	Answers.Emplace(200, TeamsOneTeamAnswer);
	Rig.Client->SetTestResponseScript(MoveTemp(Answers));

	bool bSecondSent = false;
	UCrowdyTeams* Teams = Rig.Teams.Get();
	Rig.Client->SetTestOnRequest([&bSecondSent, Teams](const FString&)
	{
		if (bSecondSent)
		{
			return;
		}
		bSecondSent = true;
		Teams->GetMyTeams(FOnMyTeamsSuccess(), FOnTeamError());
	});

	FOnMyTeamsSuccess OnAnswered;
	OnAnswered.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleMyTeams);
	Rig.Teams->GetMyTeams(OnAnswered, FOnTeamError());
	Rig.Drain();
	Rig.Client->SetTestOnRequest(nullptr);

	TestTrue(TEXT("the second request was sent"), bSecondSent);
	TestEqual(TEXT("the first caller still hears its answer"), Rig.Listener->MyTeamsAnswers.Num(), 1);
	TestTrue(TEXT("the cache holds the newer answer"), Rig.Teams->IsPlayerInTeam(8));
	TestFalse(TEXT("not the older one that arrived after it"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("and announced only the newer one"), Rig.Listener->CacheEvents.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsClearAndRefreshTest,
	"CrowdySDK.CrowdyServices.TeamsClearAndRefresh", TeamsTestFlags)
bool FCrowdyTeamsClearAndRefreshTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsOneTeamAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// What a sign-in does: the first answer is about the account before it, the second about the one signed in.
	TArray<TPair<int32, FString>> Answers;
	Answers.Emplace(200, TeamsOneTeamAnswer);
	Answers.Emplace(200, TeamsOtherTeamAnswer);
	Rig.Client->SetTestResponseScript(MoveTemp(Answers));

	Rig.Teams->GetMyTeams(FOnMyTeamsSuccess(), FOnTeamError());
	Rig.Teams->ClearMyTeamsCache();
	Rig.Teams->RefreshMyTeams();
	Rig.Drain();

	TestTrue(TEXT("the refresh fills the cache"), Rig.Teams->HasCachedTeams());
	TestTrue(TEXT("with the signed-in account's team"), Rig.Teams->IsPlayerInTeam(8));
	TestFalse(TEXT("and not the earlier account's"), Rig.Teams->IsPlayerInTeam(7));
	TestEqual(TEXT("announced once"), Rig.Listener->CacheEvents.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTeamsChangeBeforeClearDoesNotRefreshTest,
	"CrowdySDK.CrowdyServices.TeamsChangeBeforeClearDoesNotRefresh", TeamsTestFlags)
bool FCrowdyTeamsChangeBeforeClearDoesNotRefreshTest::RunTest(const FString& Parameters)
{
	FCrowdyTeamsTestRig Rig(TeamsUpdateAnswer);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnTeamSuccess OnTeam;
	OnTeam.BindDynamic(Rig.Listener.Get(), &UCrowdyTeamsTestListener::HandleTeam);
	Rig.Teams->UpdateTeam(7, TEXT("Reavers Reborn"), FString(), OnTeam, FOnTeamError());
	Rig.Teams->ClearMyTeamsCache();
	Rig.Drain();

	TestEqual(TEXT("the caller hears the update"), Rig.Listener->Successes, 1);
	TestFalse(TEXT("a change sent before a clear does not refill the cache"), Rig.Teams->HasCachedTeams());
	TestEqual(TEXT("and nothing is announced"), Rig.Listener->CacheEvents.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
