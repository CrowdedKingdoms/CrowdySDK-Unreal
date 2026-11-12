#include "CrowdyActorOwnersTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/AsyncActions/CrowdyActorOwnersActions.h"
#include "Subsystem/CrowdyActorOwners.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"

namespace CrowdyActorOwnersTest
{
	constexpr EAutomationTestFlags ActorOwnersTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 TestAppId = 31337;
	constexpr int64 PlayerId = 42;
	constexpr int64 OtherPlayerId = 43;

	const TCHAR* const NoRecordsAnswer = TEXT("{\"data\":{\"batchLookupActors\":[]}}");
	const TCHAR* const CreatedAnswer = TEXT("{\"data\":{\"createActor\":{\"uuid\":\"created\"}}}");
	const TCHAR* const SomeoneElsesWarning = TEXT("Another player's actor record holds this player's actor id");
	const TCHAR* const GaveUpWarning = TEXT("Could not record this player's actor");

	FGuid ActorIdOf(int64 UserId)
	{
		return UHelperFunctions::GetDeterministicID(UserId);
	}

	FString DigitsOf(int64 UserId)
	{
		return ActorIdOf(UserId).ToString(EGuidFormats::Digits);
	}

	FString OneRecord(const FString& Uuid, const FString& UserId)
	{
		return FString::Printf(TEXT("{\"data\":{\"batchLookupActors\":[{\"uuid\":\"%s\",\"appId\":\"31337\",\"userId\":\"%s\",\"publicState\":null}]}}"),
			*Uuid, *UserId);
	}

	FString OwnerRefusal(const TCHAR* Code, const TCHAR* Extensions)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"%s\"%s}}],\"data\":null}"), Code, Extensions);
	}

	// Answers and Sends are declared first so they outlive the client, whose teardown still delivers what is pending.
	struct FActorOwnersRig
	{
		TArray<FCrowdyActorOwner> Answers;
		int32 Sends = 0;
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyActorOwners> Owners;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FActorOwnersRig(const FString& Answer)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Owners(NewObject<UCrowdyActorOwners>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Answer, 200))
		{
			if (!Client.IsValid())
			{
				return;
			}
			Client->SetTestOnRequest([this](const FString&) { ++Sends; });
			Owners->InitializeForTest(Client, TestAppId, Session.Get());
		}

		// Cancels the subsystem's tickers, which would otherwise run after the rig is gone.
		~FActorOwnersRig()
		{
			Owners->Deinitialize();
		}

		void Script(const TArray<FString>& Bodies)
		{
			TArray<TPair<int32, FString>> Responses;
			for (const FString& Body : Bodies)
			{
				Responses.Emplace(200, Body);
			}
			Client->SetTestResponseScript(MoveTemp(Responses));
		}

		void Ask(const FGuid& ActorId)
		{
			Owners->ResolveActorOwner(ActorId, [this](const FCrowdyActorOwner& Owner) { Answers.Add(Owner); });
		}

		void Drain()
		{
			Owners->FlushLookupsForTest();
			Client->Poll();
		}

		void SignInAs(int64 UserId, const FString& Uuid)
		{
			Session->SetUserID(UserId);
			Session->SetUUID(Uuid);
			Client->Poll();
		}

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}
	};

	// The one answer a single lookup for ActorId gets from Answer, or a default Failed when it got none or several.
	FCrowdyActorOwner ResolveOnce(FAutomationTestBase& Test, const FString& Answer, const FGuid& ActorId)
	{
		FActorOwnersRig Rig(Answer);
		FCrowdyActorOwner Missing;
		Missing.Result = ECrowdyActorOwnerResult::Failed;
		if (!Test.TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return Missing;
		}
		Rig.Ask(ActorId);
		Test.TestEqual(TEXT("never answered inside the call"), Rig.Answers.Num(), 0);
		Rig.Drain();
		return Test.TestEqual(TEXT("one answer"), Rig.Answers.Num(), 1) ? Rig.Answers[0] : Missing;
	}

	// Asks Count fresh ids at the rig's current time and lets every lookup answer.
	void AskFreshIds(FActorOwnersRig& Rig, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Rig.Ask(FGuid::NewGuid());
		}
		Rig.Drain();
	}
}

using namespace CrowdyActorOwnersTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersConfirmsAMatchingRecordTest,
	"CrowdySDK.Services.ActorOwners.ConfirmsAMatchingRecord", ActorOwnersTestFlags)

bool FCrowdyActorOwnersConfirmsAMatchingRecordTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(PlayerId), TEXT("42")), ActorIdOf(PlayerId));
	TestTrue(TEXT("confirmed"), Owner.Result == ECrowdyActorOwnerResult::Confirmed);
	TestEqual(TEXT("the owner"), Owner.UserId, PlayerId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSquattedRecordTest,
	"CrowdySDK.Services.ActorOwners.SquattedRecordIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSquattedRecordTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(PlayerId), TEXT("43")), ActorIdOf(PlayerId));
	TestTrue(TEXT("a record whose owner does not derive this id proves nothing"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersNoRecordTest,
	"CrowdySDK.Services.ActorOwners.NoRecordIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersNoRecordTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, NoRecordsAnswer, ActorIdOf(PlayerId));
	TestTrue(TEXT("unknown"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersOtherUuidTest,
	"CrowdySDK.Services.ActorOwners.RecordForAnotherIdIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersOtherUuidTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(OtherPlayerId), TEXT("43")), ActorIdOf(PlayerId));
	TestTrue(TEXT("a sound record for another actor says nothing about this one"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersMalformedUserIdTest,
	"CrowdySDK.Services.ActorOwners.MalformedUserIdIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersMalformedUserIdTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(PlayerId), TEXT("42abc")), ActorIdOf(PlayerId));
	TestTrue(TEXT("unknown"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersInvalidIdTest,
	"CrowdySDK.Services.ActorOwners.InvalidIdIsUnknownAtOnce", ActorOwnersTestFlags)

bool FCrowdyActorOwnersInvalidIdTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(FGuid());
	TestTrue(TEXT("answered at once as unknown"), Rig.Answers.Num() == 1 && Rig.Answers[0].Result == ECrowdyActorOwnerResult::Unknown);
	TestFalse(TEXT("nothing queued"), Rig.Owners->FlushLookupsForTest());
	Rig.Client->Poll();
	TestEqual(TEXT("nothing asked"), Rig.Sends, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSendsDigitsTest,
	"CrowdySDK.Services.ActorOwners.LookupSendsTheIdAsDigits", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSendsDigitsTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Drain();
	TestTrue(TEXT("the 32-digit id"), Rig.SentBody().Contains(FString::Printf(TEXT("\"uuids\":[\"%s\"]"), *DigitsOf(PlayerId))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersFailedNotCachedTest,
	"CrowdySDK.Services.ActorOwners.FailureIsNotCached", ActorOwnersTestFlags)

bool FCrowdyActorOwnersFailedNotCachedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT(""))});
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Drain();
	TestTrue(TEXT("failed"), Rig.Answers.Num() == 1 && Rig.Answers[0].Result == ECrowdyActorOwnerResult::Failed);
	TestEqual(TEXT("no one is named"), Rig.Answers.Num() == 1 ? Rig.Answers[0].UserId : -1LL, 0LL);

	Rig.Ask(ActorIdOf(PlayerId));
	TestEqual(TEXT("a failure is not answered from a cache"), Rig.Answers.Num(), 1);
	Rig.Drain();
	TestEqual(TEXT("asked again"), Rig.Sends, 2);
	TestTrue(TEXT("and confirmed"), Rig.Answers.Num() == 2 && Rig.Answers[1].Result == ECrowdyActorOwnerResult::Confirmed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersConfirmedCachedTest,
	"CrowdySDK.Services.ActorOwners.ConfirmedIsCached", ActorOwnersTestFlags)

bool FCrowdyActorOwnersConfirmedCachedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Drain();
	Rig.Ask(ActorIdOf(PlayerId));
	TestEqual(TEXT("the second ask is answered at once"), Rig.Answers.Num(), 2);
	Rig.Drain();
	TestEqual(TEXT("one call"), Rig.Sends, 1);
	TestTrue(TEXT("confirmed again"), Rig.Answers.Num() == 2 && Rig.Answers[1].Result == ECrowdyActorOwnerResult::Confirmed
		&& Rig.Answers[1].UserId == PlayerId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersUnknownCachedTest,
	"CrowdySDK.Services.ActorOwners.UnknownIsCachedForTenSeconds", ActorOwnersTestFlags)

bool FCrowdyActorOwnersUnknownCachedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Owners->SetNowForTest(1000.0);
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Drain();

	Rig.Owners->SetNowForTest(1009.5);
	Rig.Ask(ActorIdOf(PlayerId));
	TestEqual(TEXT("inside ten seconds it is answered at once"), Rig.Answers.Num(), 2);
	Rig.Drain();
	TestEqual(TEXT("without a call"), Rig.Sends, 1);

	Rig.Owners->SetNowForTest(1010.5);
	Rig.Ask(ActorIdOf(PlayerId));
	TestEqual(TEXT("after ten seconds it waits for the platform"), Rig.Answers.Num(), 2);
	Rig.Drain();
	TestEqual(TEXT("asked again"), Rig.Sends, 2);
	TestTrue(TEXT("every answer unknown"), Rig.Answers.Num() == 3 && Rig.Answers[2].Result == ECrowdyActorOwnerResult::Unknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersOneTickOneRequestTest,
	"CrowdySDK.Services.ActorOwners.IdsAskedInOneTickShareOneRequest", ActorOwnersTestFlags)

bool FCrowdyActorOwnersOneTickOneRequestTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Ask(ActorIdOf(OtherPlayerId));
	Rig.Ask(ActorIdOf(PlayerId));
	TestEqual(TEXT("nothing answered inside the calls"), Rig.Answers.Num(), 0);
	Rig.Drain();
	TestEqual(TEXT("one request"), Rig.Sends, 1);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("it carries the first id"), Body.Contains(DigitsOf(PlayerId)));
	TestTrue(TEXT("and the second"), Body.Contains(DigitsOf(OtherPlayerId)));
	TestEqual(TEXT("every ask answered once"), Rig.Answers.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersInFlightTest,
	"CrowdySDK.Services.ActorOwners.AnIdInFlightIsNotSentAgain", ActorOwnersTestFlags)

bool FCrowdyActorOwnersInFlightTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(ActorIdOf(PlayerId));
	TestFalse(TEXT("the second ask joins the request already out"), Rig.Owners->FlushLookupsForTest());
	Rig.Client->Poll();
	Rig.Drain();
	TestEqual(TEXT("one request"), Rig.Sends, 1);
	TestEqual(TEXT("both answered once"), Rig.Answers.Num(), 2);
	TestTrue(TEXT("both confirmed"), Rig.Answers.Num() == 2 && Rig.Answers[0].UserId == PlayerId && Rig.Answers[1].UserId == PlayerId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersLaterFlushSkipsInFlightTest,
	"CrowdySDK.Services.ActorOwners.ALaterRequestLeavesOutAnIdInFlight", ActorOwnersTestFlags)

bool FCrowdyActorOwnersLaterFlushSkipsInFlightTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Ask(ActorIdOf(OtherPlayerId));
	Rig.Owners->FlushLookupsForTest();
	const FString Body = Rig.SentBody();
	TestEqual(TEXT("a second request for the new id"), Rig.Sends, 2);
	TestTrue(TEXT("it carries the new id"), Body.Contains(DigitsOf(OtherPlayerId)));
	TestFalse(TEXT("and not the one already out"), Body.Contains(DigitsOf(PlayerId)));
	Rig.Client->Poll();
	TestEqual(TEXT("every ask answered once"), Rig.Answers.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSplitsRequestsTest,
	"CrowdySDK.Services.ActorOwners.SixtyIdsMakeTwoRequests", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSplitsRequestsTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	AskFreshIds(Rig, 60);
	TestEqual(TEXT("at most fifty ids a request"), Rig.Sends, 2);
	TestEqual(TEXT("every ask answered"), Rig.Answers.Num(), 60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersLeftOutTest,
	"CrowdySDK.Services.ActorOwners.AnIdLeftOutOfTheReplyIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersLeftOutTest::RunTest(const FString& Parameters)
{
	FCrowdyActorOwner Listed;
	FCrowdyActorOwner LeftOut;
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Owners->ResolveActorOwner(ActorIdOf(PlayerId), [&Listed](const FCrowdyActorOwner& Owner) { Listed = Owner; });
	Rig.Owners->ResolveActorOwner(ActorIdOf(OtherPlayerId), [&LeftOut](const FCrowdyActorOwner& Owner) { LeftOut = Owner; });
	LeftOut.Result = ECrowdyActorOwnerResult::Failed;
	Rig.Drain();
	TestEqual(TEXT("one request"), Rig.Sends, 1);
	TestTrue(TEXT("the listed id is confirmed"), Listed.Result == ECrowdyActorOwnerResult::Confirmed && Listed.UserId == PlayerId);
	TestTrue(TEXT("the one left out is unknown"), LeftOut.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("and names no one"), LeftOut.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersCapPrunesExpiredTest,
	"CrowdySDK.Services.ActorOwners.UnknownCapPrunesExpiredFirst", ActorOwnersTestFlags)

bool FCrowdyActorOwnersCapPrunesExpiredTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Owners->SetNowForTest(1000.0);
	AskFreshIds(Rig, UCrowdyActorOwners::MaxUnknownEntries);
	TestEqual(TEXT("full"), Rig.Owners->GetUnknownCountForTest(), UCrowdyActorOwners::MaxUnknownEntries);

	Rig.Owners->SetNowForTest(1020.0);
	AskFreshIds(Rig, 1);
	TestEqual(TEXT("every expired entry is gone"), Rig.Owners->GetUnknownCountForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersCapDropsSoonestTest,
	"CrowdySDK.Services.ActorOwners.UnknownCapDropsTheSoonestToExpire", ActorOwnersTestFlags)

bool FCrowdyActorOwnersCapDropsSoonestTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Owners->SetNowForTest(1000.0);
	AskFreshIds(Rig, UCrowdyActorOwners::MaxUnknownEntries);

	Rig.Owners->SetNowForTest(1005.0);
	const FGuid Newest = FGuid::NewGuid();
	Rig.Ask(Newest);
	Rig.Drain();
	TestEqual(TEXT("held at the cap"), Rig.Owners->GetUnknownCountForTest(), UCrowdyActorOwners::MaxUnknownEntries);

	const int32 Before = Rig.Answers.Num();
	Rig.Owners->SetNowForTest(1006.0);
	Rig.Ask(Newest);
	TestEqual(TEXT("the newest is kept"), Rig.Answers.Num(), Before + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersNoClientTest,
	"CrowdySDK.Services.ActorOwners.NoClientFailsOnALaterTick", ActorOwnersTestFlags)

bool FCrowdyActorOwnersNoClientTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("No API client host"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FActorOwnersRig Rig(NoRecordsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Client.Reset();
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Ask(ActorIdOf(OtherPlayerId));
	TestEqual(TEXT("not answered inside the call"), Rig.Answers.Num(), 0);
	Rig.Owners->FlushLookupsForTest();
	TestTrue(TEXT("both fail on the flush"), Rig.Answers.Num() == 2 && Rig.Answers[0].Result == ECrowdyActorOwnerResult::Failed
		&& Rig.Answers[1].Result == ECrowdyActorOwnerResult::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersDeinitializeTest,
	"CrowdySDK.Services.ActorOwners.DeinitializeAnswersEveryWaiter", ActorOwnersTestFlags)

bool FCrowdyActorOwnersDeinitializeTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerId), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(ActorIdOf(PlayerId));
	Rig.Ask(ActorIdOf(OtherPlayerId));
	Rig.Owners->Deinitialize();
	TestEqual(TEXT("in flight and queued, all answered"), Rig.Answers.Num(), 3);
	TestTrue(TEXT("as failed"), Rig.Answers.Num() == 3 && Rig.Answers[0].Result == ECrowdyActorOwnerResult::Failed
		&& Rig.Answers[2].Result == ECrowdyActorOwnerResult::Failed);
	TestFalse(TEXT("no flush left"), Rig.Owners->FlushLookupsForTest());
	Rig.Client->Poll();
	TestEqual(TEXT("and never again"), Rig.Answers.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersNodeDefersTest,
	"CrowdySDK.Services.ActorOwners.NodePinsFireOnALaterTick", ActorOwnersTestFlags)

bool FCrowdyActorOwnersNodeDefersTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorOwnersTestListener> Listener(NewObject<UCrowdyActorOwnersTestListener>());
	const TStrongObjectPtr<UCrowdyActorOwners_ResolveActorOwner> Node(NewObject<UCrowdyActorOwners_ResolveActorOwner>());
	Node->OnFailed.AddDynamic(Listener.Get(), &UCrowdyActorOwnersTestListener::HandleOwner);

	// With no world the node answers Failed without asking, through the same path every answer takes.
	Node->Activate();
	TestEqual(TEXT("no pin inside the node"), Listener->Owners.Num(), 0);
	FTSTicker::GetCoreTicker().Tick(0.1f);
	TestEqual(TEXT("one pin a tick later"), Listener->Owners.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersRecordsOnceTest,
	"CrowdySDK.Services.ActorOwners.RecordsThePlayersActorOnce", ActorOwnersTestFlags)

bool FCrowdyActorOwnersRecordsOnceTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	TestEqual(TEXT("one create"), Rig.Sends, 1);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the app as a string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	TestTrue(TEXT("the id the session holds"), Body.Contains(FString::Printf(TEXT("\"uuid\":\"%s\""), *DigitsOf(PlayerId))));
	TestTrue(TEXT("a chunk of strings"), Body.Contains(TEXT("\"x\":\"0\"")) && Body.Contains(TEXT("\"y\":\"0\"")) && Body.Contains(TEXT("\"z\":\"0\"")));

	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	TestEqual(TEXT("not again for the same player"), Rig.Sends, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSkipsUnderivedTest,
	"CrowdySDK.Services.ActorOwners.SkipsAnIdNotDerivedFromTheUser", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSkipsUnderivedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.SignInAs(PlayerId, FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Rig.SignInAs(PlayerId, DigitsOf(OtherPlayerId));
	Rig.SignInAs(0, DigitsOf(0));
	TestEqual(TEXT("nothing recorded"), Rig.Sends, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersOwnedExistsTest,
	"CrowdySDK.Services.ActorOwners.ExistingOwnActorIsDone", ActorOwnersTestFlags)

bool FCrowdyActorOwnersOwnedExistsTest::RunTest(const FString& Parameters)
{
	const TCHAR* const OwnedCases[] = {TEXT(",\"ownedByCaller\":true"), TEXT("")};
	for (const TCHAR* Owned : OwnedCases)
	{
		FActorOwnersRig Rig(OwnerRefusal(TEXT("ACTOR_EXISTS"), Owned));
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
		TestFalse(TEXT("no retry"), Rig.Owners->RunRecordRetryForTest());
		TestEqual(TEXT("owned, or unsaid, warns nothing"), Rig.Owners->GetRecordWarningsForTest(), 0);
		Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
		TestEqual(TEXT("done"), Rig.Sends, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSomeoneElsesTest,
	"CrowdySDK.Services.ActorOwners.SomeoneElsesActorWarnsOnce", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSomeoneElsesTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(SomeoneElsesWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FActorOwnersRig Rig(OwnerRefusal(TEXT("ACTOR_EXISTS"), TEXT(",\"ownedByCaller\":false")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	TestEqual(TEXT("one create"), Rig.Sends, 1);
	TestEqual(TEXT("one warning"), Rig.Owners->GetRecordWarningsForTest(), 1);
	TestFalse(TEXT("no retry"), Rig.Owners->RunRecordRetryForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersRetriesTest,
	"CrowdySDK.Services.ActorOwners.RetriesAnyFailureThreeTimes", ActorOwnersTestFlags)

bool FCrowdyActorOwnersRetriesTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(GaveUpWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	const TCHAR* const FailureCases[] = {
		TEXT(",\"blame\":\"PLATFORM\",\"retryable\":true"),
		TEXT(",\"blame\":\"CALLER\",\"retryable\":false"),
		TEXT("")};
	for (const TCHAR* Extensions : FailureCases)
	{
		FActorOwnersRig Rig(OwnerRefusal(TEXT("PLATFORM_BUSY"), Extensions));
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
		TestTrue(TEXT("a second try waits"), Rig.Owners->RunRecordRetryForTest());
		Rig.Client->Poll();
		TestTrue(TEXT("a third try waits"), Rig.Owners->RunRecordRetryForTest());
		Rig.Client->Poll();
		TestFalse(TEXT("no fourth"), Rig.Owners->RunRecordRetryForTest());
		TestEqual(TEXT("three creates"), Rig.Sends, 3);
		TestEqual(TEXT("one warning when it gives up"), Rig.Owners->GetRecordWarningsForTest(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersTriesAgainTest,
	"CrowdySDK.Services.ActorOwners.GivingUpWaitsForTheNextIdUpdate", ActorOwnersTestFlags)

bool FCrowdyActorOwnersTriesAgainTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(GaveUpWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Failure = OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT(""));
	Rig.Script({Failure, Failure, Failure});
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	Rig.Owners->RunRecordRetryForTest();
	Rig.Client->Poll();
	Rig.Owners->RunRecordRetryForTest();
	Rig.Client->Poll();
	TestEqual(TEXT("gave up after three"), Rig.Sends, 3);

	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	TestEqual(TEXT("the next id update tries again"), Rig.Sends, 4);
	TestFalse(TEXT("and it landed"), Rig.Owners->RunRecordRetryForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSignOutTest,
	"CrowdySDK.Services.ActorOwners.RecordsAgainAfterSignOut", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSignOutTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	Rig.Owners->HandleSignedOut();
	Rig.SignInAs(PlayerId, DigitsOf(PlayerId));
	TestEqual(TEXT("the next sign-in records again"), Rig.Sends, 2);
	return true;
}

#endif
