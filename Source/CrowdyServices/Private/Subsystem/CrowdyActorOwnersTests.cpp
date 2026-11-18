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

namespace CrowdyActorOwnersTest
{
	constexpr EAutomationTestFlags ActorOwnersTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 TestAppId = 31337;
	constexpr int64 PlayerId = 42;

	const TCHAR* const NoRecordsAnswer = TEXT("{\"data\":{\"batchLookupActors\":[]}}");
	const TCHAR* const CreatedAnswer = TEXT("{\"data\":{\"createActor\":{\"uuid\":\"created\"}}}");
	const TCHAR* const DeletedAnswer = TEXT("{\"data\":{\"deleteActor\":{\"uuid\":\"deleted\"}}}");
	const TCHAR* const SomeoneElsesWarning = TEXT("Another player's actor record holds this player's actor id");
	const TCHAR* const GaveUpWarning = TEXT("Could not record this player's actor");
	const TCHAR* const DeleteFailedWarning = TEXT("Could not delete this player's actor record");

	// Arbitrary ids the lookups ask about; no relation to a user id.
	const FGuid PlayerActor(0x1A2B3C4D, 0x5E6F7081, 0x92A3B4C5, 0xD6E7F809);
	const FGuid OtherActor(0x0F1E2D3C, 0x4B5A6978, 0x8796A5B4, 0xC3D2E1F0);

	FString DigitsOf(const FGuid& ActorId)
	{
		return ActorId.ToString(EGuidFormats::Digits);
	}

	FString RandomDigits()
	{
		return DigitsOf(FGuid::NewGuid());
	}

	FString OneRecord(const FString& Uuid, const FString& UserId)
	{
		return FString::Printf(TEXT("{\"data\":{\"batchLookupActors\":[{\"uuid\":\"%s\",\"appId\":\"31337\",\"userId\":\"%s\",\"publicState\":null}]}}"),
			*Uuid, *UserId);
	}

	// Whether a sent request body runs Operation; the body's spacing is not relied on.
	bool RunsOp(const FString& Body, const TCHAR* Operation)
	{
		return Body.Replace(TEXT(" "), TEXT("")).Contains(FString::Printf(TEXT("\"operationName\":\"%s\""), Operation), ESearchCase::CaseSensitive);
	}

	// Whether Body deletes the record of Uuid, under an idempotency key.
	bool DeletesRecord(const FString& Body, const FString& Uuid)
	{
		return RunsOp(Body, TEXT("DeleteActor")) && Body.Contains(Uuid) && Body.Contains(TEXT("\"idempotencyKey\""));
	}

	// Whether Body creates the record of Uuid.
	bool CreatesRecord(const FString& Body, const FString& Uuid)
	{
		return RunsOp(Body, TEXT("CreateActor")) && Body.Contains(FString::Printf(TEXT("\"uuid\":\"%s\""), *Uuid));
	}

	FString OwnerRefusal(const TCHAR* Code, const TCHAR* Extensions)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"%s\"%s}}],\"data\":null}"), Code, Extensions);
	}

	// Answers and Sends are declared first so they outlive the client, whose teardown still delivers what is pending.
	struct FActorOwnersRig
	{
		TArray<FCrowdyActorOwner> Answers;
		TArray<FString> Bodies;
		// The bearer each request went out with, in step with Bodies.
		TArray<FString> Authorizations;
		int32 Sends = 0;
		// How often the sign-in's realtime connection was let open, and how often a sign-out let its session end.
		int32 Opened = 0;
		int32 Ended = 0;
		// The request count when each of those happened, so a test can tell what had been sent first.
		TArray<int32> SendsWhenOpened;
		TArray<int32> SendsWhenEnded;
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
			Client->SetTestOnRequest([this](const FString&)
			{
				++Sends;
				FString Body;
				Client->GetLastTestRequestBody(Body);
				Bodies.Add(Body);
				FString Url;
				Client->GetLastTestRequest(Url, Authorizations.AddDefaulted_GetRef());
			});
			Owners->InitializeForTest(Client, TestAppId, Session.Get());
		}

		// Cancels the subsystem's tickers, which would otherwise run after the rig is gone.
		~FActorOwnersRig()
		{
			Owners->Deinitialize();
		}

		void Script(const TArray<FString>& Replies)
		{
			TArray<TPair<int32, FString>> Responses;
			for (const FString& Reply : Replies)
			{
				Responses.Emplace(200, Reply);
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

		// A request sent while an answer is delivered is answered on a later poll, so the chain is polled out.
		void Settle()
		{
			for (int32 Round = 0; Round < 4; ++Round)
			{
				Client->Poll();
			}
		}

		// What a completed sign-in does here: the session holds Uuid, then the record is made before the connection opens.
		void SignInAs(int64 UserId, const FString& Uuid)
		{
			Session->SetUserID(UserId);
			Session->SetUUID(Uuid);
			SignIn();
		}

		void SignIn()
		{
			Owners->HandleSignedIn([this]()
			{
				++Opened;
				SendsWhenOpened.Add(Sends);
			});
			Settle();
		}

		// The session's id changing during a sign-in.
		void ChangeId(const FString& Uuid)
		{
			Session->SetUUID(Uuid);
			Settle();
		}

		void SignOut()
		{
			Owners->HandleSignedOut([this]()
			{
				++Ended;
				SendsWhenEnded.Add(Sends);
			});
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
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(PlayerActor), TEXT("42")), PlayerActor);
	TestTrue(TEXT("confirmed"), Owner.Result == ECrowdyActorOwnerResult::Confirmed);
	TestEqual(TEXT("the owner"), Owner.UserId, PlayerId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersRecordNamesItsOwnerTest,
	"CrowdySDK.Services.ActorOwners.ARecordNamesItsOwnerByUserId", ActorOwnersTestFlags)

bool FCrowdyActorOwnersRecordNamesItsOwnerTest::RunTest(const FString& Parameters)
{
	const FGuid RandomActor = FGuid::NewGuid();
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(RandomActor), TEXT("43")), RandomActor);
	TestTrue(TEXT("a record for a random id confirms the user it names"), Owner.Result == ECrowdyActorOwnerResult::Confirmed);
	TestEqual(TEXT("the owner is the record's userId"), Owner.UserId, 43LL);

	const FCrowdyActorOwner NoUser = ResolveOnce(*this, OneRecord(DigitsOf(RandomActor), TEXT("0")), RandomActor);
	TestTrue(TEXT("a record with no positive userId names no one"), NoUser.Result == ECrowdyActorOwnerResult::Unknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersNoRecordTest,
	"CrowdySDK.Services.ActorOwners.NoRecordIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersNoRecordTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, NoRecordsAnswer, PlayerActor);
	TestTrue(TEXT("unknown"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersOtherUuidTest,
	"CrowdySDK.Services.ActorOwners.RecordForAnotherIdIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersOtherUuidTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(OtherActor), TEXT("43")), PlayerActor);
	TestTrue(TEXT("a sound record for another actor says nothing about this one"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersMalformedUserIdTest,
	"CrowdySDK.Services.ActorOwners.MalformedUserIdIsUnknown", ActorOwnersTestFlags)

bool FCrowdyActorOwnersMalformedUserIdTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorOwner Owner = ResolveOnce(*this, OneRecord(DigitsOf(PlayerActor), TEXT("42abc")), PlayerActor);
	TestTrue(TEXT("unknown"), Owner.Result == ECrowdyActorOwnerResult::Unknown);
	TestEqual(TEXT("no one is named"), Owner.UserId, 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersInvalidIdTest,
	"CrowdySDK.Services.ActorOwners.InvalidIdIsUnknownAtOnce", ActorOwnersTestFlags)

bool FCrowdyActorOwnersInvalidIdTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
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
	Rig.Ask(PlayerActor);
	Rig.Drain();
	TestTrue(TEXT("the 32-digit id"), Rig.SentBody().Contains(FString::Printf(TEXT("\"uuids\":[\"%s\"]"), *DigitsOf(PlayerActor))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersFailedNotCachedTest,
	"CrowdySDK.Services.ActorOwners.FailureIsNotCached", ActorOwnersTestFlags)

bool FCrowdyActorOwnersFailedNotCachedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT(""))});
	Rig.Ask(PlayerActor);
	Rig.Drain();
	TestTrue(TEXT("failed"), Rig.Answers.Num() == 1 && Rig.Answers[0].Result == ECrowdyActorOwnerResult::Failed);
	TestEqual(TEXT("no one is named"), Rig.Answers.Num() == 1 ? Rig.Answers[0].UserId : -1LL, 0LL);

	Rig.Ask(PlayerActor);
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
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(PlayerActor);
	Rig.Drain();
	Rig.Ask(PlayerActor);
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
	Rig.Ask(PlayerActor);
	Rig.Drain();

	Rig.Owners->SetNowForTest(1009.5);
	Rig.Ask(PlayerActor);
	TestEqual(TEXT("inside ten seconds it is answered at once"), Rig.Answers.Num(), 2);
	Rig.Drain();
	TestEqual(TEXT("without a call"), Rig.Sends, 1);

	Rig.Owners->SetNowForTest(1010.5);
	Rig.Ask(PlayerActor);
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
	Rig.Ask(PlayerActor);
	Rig.Ask(OtherActor);
	Rig.Ask(PlayerActor);
	TestEqual(TEXT("nothing answered inside the calls"), Rig.Answers.Num(), 0);
	Rig.Drain();
	TestEqual(TEXT("one request"), Rig.Sends, 1);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("it carries the first id"), Body.Contains(DigitsOf(PlayerActor)));
	TestTrue(TEXT("and the second"), Body.Contains(DigitsOf(OtherActor)));
	TestEqual(TEXT("every ask answered once"), Rig.Answers.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersInFlightTest,
	"CrowdySDK.Services.ActorOwners.AnIdInFlightIsNotSentAgain", ActorOwnersTestFlags)

bool FCrowdyActorOwnersInFlightTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(PlayerActor);
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(PlayerActor);
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
	Rig.Ask(PlayerActor);
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(PlayerActor);
	Rig.Ask(OtherActor);
	Rig.Owners->FlushLookupsForTest();
	const FString Body = Rig.SentBody();
	TestEqual(TEXT("a second request for the new id"), Rig.Sends, 2);
	TestTrue(TEXT("it carries the new id"), Body.Contains(DigitsOf(OtherActor)));
	TestFalse(TEXT("and not the one already out"), Body.Contains(DigitsOf(PlayerActor)));
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
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Owners->ResolveActorOwner(PlayerActor, [&Listed](const FCrowdyActorOwner& Owner) { Listed = Owner; });
	Rig.Owners->ResolveActorOwner(OtherActor, [&LeftOut](const FCrowdyActorOwner& Owner) { LeftOut = Owner; });
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
	Rig.Ask(PlayerActor);
	Rig.Ask(OtherActor);
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
	FActorOwnersRig Rig(OneRecord(DigitsOf(PlayerActor), TEXT("42")));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Ask(PlayerActor);
	Rig.Owners->FlushLookupsForTest();
	Rig.Ask(PlayerActor);
	Rig.Ask(OtherActor);
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
	const FString Id = RandomDigits();
	Rig.Script({CreatedAnswer});
	Rig.SignInAs(PlayerId, Id);
	TestEqual(TEXT("one request"), Rig.Sends, 1);
	TestTrue(TEXT("and the first request is the create"), Rig.Bodies.Num() == 1 && CreatesRecord(Rig.Bodies[0], Id));
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the app as a string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	TestTrue(TEXT("the id the session holds"), Body.Contains(FString::Printf(TEXT("\"uuid\":\"%s\""), *Id)));
	TestTrue(TEXT("a chunk of strings"), Body.Contains(TEXT("\"x\":\"0\"")) && Body.Contains(TEXT("\"y\":\"0\"")) && Body.Contains(TEXT("\"z\":\"0\"")));
	TestEqual(TEXT("the connection is let open once the create answers"), Rig.Opened, 1);

	Rig.ChangeId(Id);
	TestEqual(TEXT("the same id announced again sends nothing"), Rig.Sends, 1);

	// A create sent before this sign-in's token existed proves nothing, so a sign-in always makes the record afresh.
	Rig.Script({CreatedAnswer});
	Rig.SignIn();
	TestEqual(TEXT("a sign-in creates again for the same id"), Rig.Sends, 2);
	TestTrue(TEXT("the same record"), Rig.Bodies.Num() == 2 && CreatesRecord(Rig.Bodies[1], Id));
	TestEqual(TEXT("and lets the connection open after it answers"), Rig.Opened, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersConnectAfterCreateTest,
	"CrowdySDK.Services.ActorOwners.TheConnectionOpensOnlyOnceTheCreateAnswers", ActorOwnersTestFlags)

bool FCrowdyActorOwnersConnectAfterCreateTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Id = RandomDigits();
	Rig.Script({CreatedAnswer});
	Rig.Session->SetUserID(PlayerId);
	Rig.Session->SetUUID(Id);
	Rig.Owners->HandleSignedIn([&Rig]() { ++Rig.Opened; });
	TestEqual(TEXT("the create is out"), Rig.Sends, 1);
	TestEqual(TEXT("but the connection waits for its answer"), Rig.Opened, 0);
	TestTrue(TEXT("and says it is settling"), Rig.Owners->IsSettlingSignIn());
	Rig.Settle();
	TestEqual(TEXT("the answer lets it open"), Rig.Opened, 1);
	TestFalse(TEXT("and the sign-in is settled"), Rig.Owners->IsSettlingSignIn());

	Rig.SignOut();
	Rig.Settle();
	Rig.Session->SetUUID(RandomDigits());
	Rig.Settle();
	TestEqual(TEXT("signed out, an id is recorded by nobody"), Rig.Sends, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersRecordsOnlyAValidIdTest,
	"CrowdySDK.Services.ActorOwners.RecordsOnlyAValidIdOfASignedInUser", ActorOwnersTestFlags)

bool FCrowdyActorOwnersRecordsOnlyAValidIdTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.SignInAs(PlayerId, TEXT("short"));
	Rig.SignInAs(PlayerId, FString());
	Rig.SignInAs(PlayerId, FGuid().ToString(EGuidFormats::Digits));
	Rig.SignInAs(0, RandomDigits());
	TestEqual(TEXT("no id, a short id, a zero id or no user records nothing"), Rig.Sends, 0);
	TestEqual(TEXT("and lets each connection open at once"), Rig.Opened, 4);

	Rig.SignOut();
	Rig.Script({CreatedAnswer});
	Rig.SignInAs(PlayerId, RandomDigits());
	TestEqual(TEXT("a random id for a signed-in user is recorded"), Rig.Sends, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersIdChangeTest,
	"CrowdySDK.Services.ActorOwners.AnIdChangeCreatesTheNewRecordThenDeletesTheOld", ActorOwnersTestFlags)

bool FCrowdyActorOwnersIdChangeTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString First = RandomDigits();
	const FString Second = RandomDigits();
	Rig.Script({CreatedAnswer, CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, First);
	Rig.ChangeId(Second);
	if (!TestEqual(TEXT("two creates and one delete"), Rig.Bodies.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the first id is created"), CreatesRecord(Rig.Bodies[0], First));
	TestTrue(TEXT("then the new id is created"), CreatesRecord(Rig.Bodies[1], Second));
	TestTrue(TEXT("then the first id's record is deleted"), DeletesRecord(Rig.Bodies[2], First));
	TestFalse(TEXT("and the new one is not"), Rig.Bodies[2].Contains(Second));
	return true;
}

// The player always holds a record: the previous one goes only once the new one is made.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersKeepsPreviousUntilMadeTest,
	"CrowdySDK.Services.ActorOwners.ThePreviousRecordStaysUntilTheNewCreateSucceeds", ActorOwnersTestFlags)

bool FCrowdyActorOwnersKeepsPreviousUntilMadeTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString First = RandomDigits();
	const FString Second = RandomDigits();
	Rig.Script({CreatedAnswer, OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT("")), CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, First);
	Rig.ChangeId(Second);
	TestEqual(TEXT("a failed create of the new id deletes nothing"), Rig.Sends, 2);
	for (const FString& Body : Rig.Bodies)
	{
		TestFalse(TEXT("no delete yet"), RunsOp(Body, TEXT("DeleteActor")));
	}

	TestTrue(TEXT("the create is tried again"), Rig.Owners->RunRecordRetryForTest());
	Rig.Settle();
	if (!TestEqual(TEXT("the retry, then the delete"), Rig.Bodies.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("the retry creates the new id"), CreatesRecord(Rig.Bodies[2], Second));
	TestTrue(TEXT("and only then is the previous record deleted"), DeletesRecord(Rig.Bodies[3], First));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSignOutDeletesTest,
	"CrowdySDK.Services.ActorOwners.SignOutDeletesTheCurrentRecord", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSignOutDeletesTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Id = RandomDigits();
	Rig.Script({OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT("")), DeletedAnswer});
	Rig.SignInAs(PlayerId, Id);
	Rig.SignOut();
	TestEqual(TEXT("the server session waits for the delete"), Rig.Ended, 0);
	Rig.Settle();
	if (!TestEqual(TEXT("the create, then the delete"), Rig.Bodies.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the create is first"), CreatesRecord(Rig.Bodies[0], Id));
	TestTrue(TEXT("and sign-out deletes that record"), DeletesRecord(Rig.Bodies[1], Id));
	TestTrue(TEXT("the session ends once the delete answers, after it was sent"), Rig.Ended == 1 && Rig.SendsWhenEnded[0] == 2);
	TestFalse(TEXT("a create that failed before sign-out is not tried again"), Rig.Owners->RunRecordRetryForTest());
	TestEqual(TEXT("nor warned about"), Rig.Owners->GetRecordWarningsForTest(), 0);

	Rig.SignOut();
	TestEqual(TEXT("a second sign-out has nothing to delete, so its session ends at once"), Rig.Ended, 2);
	Rig.Settle();
	TestEqual(TEXT("and it sends nothing"), Rig.Sends, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSignOutDeadlineTest,
	"CrowdySDK.Services.ActorOwners.SignOutStopsWaitingForTheDeleteAfterItsDeadline", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSignOutDeadlineTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, RandomDigits());
	Rig.SignOut();
	TestEqual(TEXT("an unanswered delete holds the session's end"), Rig.Ended, 0);
	TestTrue(TEXT("until the deadline"), Rig.Owners->RunSignOutDeadlineForTest());
	TestEqual(TEXT("which ends it"), Rig.Ended, 1);
	Rig.Settle();
	TestEqual(TEXT("and the late answer ends nothing again"), Rig.Ended, 1);
	TestFalse(TEXT("nor leaves a deadline behind"), Rig.Owners->RunSignOutDeadlineForTest());
	return true;
}

// A sign-in over a live one ends the previous sign-in while its token is still installed: its record goes first, then
// the new sign-in's id is minted and recorded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSwitchUserTest,
	"CrowdySDK.Services.ActorOwners.ASignInAsAnotherUserDeletesTheOldRecordFirst", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSwitchUserTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, DeletedAnswer, CreatedAnswer});
	Rig.Session->SetUserID(PlayerId);
	Rig.Session->BeginSignIn();
	const FString First = Rig.Session->GetUUID();
	Rig.SignIn();

	Rig.Owners->HandleSignedOut(nullptr);
	Rig.Session->EndSignIn();
	Rig.Session->SetUserID(PlayerId + 1);
	Rig.Session->BeginSignIn();
	const FString Second = Rig.Session->GetUUID();
	Rig.SignIn();
	if (!TestEqual(TEXT("create, delete, create"), Rig.Bodies.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the first user's id is created"), CreatesRecord(Rig.Bodies[0], First));
	TestTrue(TEXT("then deleted, before the new id exists"), DeletesRecord(Rig.Bodies[1], First));
	TestTrue(TEXT("then the new user's fresh id is created"), Second != First && CreatesRecord(Rig.Bodies[2], Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersDeleteFailureTest,
	"CrowdySDK.Services.ActorOwners.ADeleteFailureWarnsAndKeepsTheNewRecord", ActorOwnersTestFlags)

bool FCrowdyActorOwnersDeleteFailureTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(DeleteFailedWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString First = RandomDigits();
	const FString Second = RandomDigits();
	Rig.Script({CreatedAnswer, CreatedAnswer, OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT(""))});
	Rig.SignInAs(PlayerId, First);
	Rig.ChangeId(Second);
	if (!TestEqual(TEXT("two creates and the failed delete"), Rig.Bodies.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the new id was created before the delete"), CreatesRecord(Rig.Bodies[1], Second));
	TestFalse(TEXT("a failed delete is not tried again"), Rig.Owners->RunRecordRetryForTest());
	TestEqual(TEXT("and is not a failed record"), Rig.Owners->GetRecordWarningsForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersHeldIdLifecycleTest,
	"CrowdySDK.Services.ActorOwners.EachSignInRecordsItsOwnIdAndNoListIsEverSent", ActorOwnersTestFlags)

bool FCrowdyActorOwnersHeldIdLifecycleTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, DeletedAnswer, CreatedAnswer, DeletedAnswer});
	const FString Early = DigitsOf(Rig.Session->EnsureActorID());
	Rig.Session->SetUserID(PlayerId);
	Rig.Settle();
	TestEqual(TEXT("an id minted before sign-in is not recorded while signed out"), Rig.Sends, 0);

	Rig.Session->BeginSignIn();
	Rig.SignIn();
	TestTrue(TEXT("the completed sign-in records the adopted id"), Rig.Bodies.Num() == 1 && CreatesRecord(Rig.Bodies[0], Early));

	Rig.SignOut();
	Rig.Settle();
	Rig.Session->ClearCurrentSessionData();
	Rig.Session->SetUserID(PlayerId);
	Rig.Session->BeginSignIn();
	const FString Next = Rig.Session->GetUUID();
	Rig.SignIn();
	Rig.SignOut();
	Rig.Settle();
	if (!TestEqual(TEXT("create, delete, create, delete"), Rig.Bodies.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("sign-out deletes the adopted record"), DeletesRecord(Rig.Bodies[1], Early));
	TestTrue(TEXT("the same user signing back in records a new id"), Next != Early && CreatesRecord(Rig.Bodies[2], Next));
	TestTrue(TEXT("and the next sign-out deletes that one"), DeletesRecord(Rig.Bodies[3], Next));
	for (const FString& Body : Rig.Bodies)
	{
		TestFalse(TEXT("no request lists actor records"), RunsOp(Body, TEXT("Actors")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersOwnedExistsTest,
	"CrowdySDK.Services.ActorOwners.ExistingOwnActorIsDone", ActorOwnersTestFlags)

bool FCrowdyActorOwnersOwnedExistsTest::RunTest(const FString& Parameters)
{
	const TCHAR* const OwnedCases[] = {TEXT(",\"ownedByCaller\":true"), TEXT("")};
	for (const TCHAR* Owned : OwnedCases)
	{
		FActorOwnersRig Rig(CreatedAnswer);
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		const FString Id = RandomDigits();
		Rig.Script({OwnerRefusal(TEXT("ACTOR_EXISTS"), Owned)});
		Rig.SignInAs(PlayerId, Id);
		TestFalse(TEXT("no retry"), Rig.Owners->RunRecordRetryForTest());
		TestEqual(TEXT("owned, or unsaid, warns nothing"), Rig.Owners->GetRecordWarningsForTest(), 0);
		TestEqual(TEXT("and lets the connection open"), Rig.Opened, 1);
		Rig.ChangeId(Id);
		TestEqual(TEXT("done: one create"), Rig.Sends, 1);
		TestEqual(TEXT("and the id is kept"), Rig.Session->GetUUID(), Id);
	}
	return true;
}

// The realtime servers refuse a send under an id another player's record holds, so the session takes a fresh one
// before the connection opens, and never deletes the record it does not own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersSomeoneElsesTest,
	"CrowdySDK.Services.ActorOwners.AnIdAnotherPlayersRecordHoldsIsReplaced", ActorOwnersTestFlags)

bool FCrowdyActorOwnersSomeoneElsesTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Squatted = RandomDigits();
	Rig.Script({OwnerRefusal(TEXT("ACTOR_EXISTS"), TEXT(",\"ownedByCaller\":false")), CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, Squatted);
	const FString Replacement = Rig.Session->GetUUID();
	if (!TestEqual(TEXT("the refused create, then the replacement's"), Rig.Bodies.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the session took a fresh id"), Replacement != Squatted && !Replacement.IsEmpty());
	TestTrue(TEXT("and records it"), CreatesRecord(Rig.Bodies[1], Replacement));
	TestEqual(TEXT("the connection opens once, after the replacement is made"), Rig.Opened, 1);
	TestTrue(TEXT("not before"), Rig.SendsWhenOpened.Num() == 1 && Rig.SendsWhenOpened[0] == 2);
	TestEqual(TEXT("no warning while a replacement lands"), Rig.Owners->GetRecordWarningsForTest(), 0);

	Rig.SignOut();
	Rig.Settle();
	if (!TestEqual(TEXT("sign-out deletes one record"), Rig.Bodies.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the player's own"), DeletesRecord(Rig.Bodies[2], Replacement));
	for (const FString& Body : Rig.Bodies)
	{
		TestFalse(TEXT("never the other player's"), RunsOp(Body, TEXT("DeleteActor")) && Body.Contains(Squatted));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersRemintBoundTest,
	"CrowdySDK.Services.ActorOwners.ReplacingAHeldIdStopsAfterItsBound", ActorOwnersTestFlags)

bool FCrowdyActorOwnersRemintBoundTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(SomeoneElsesWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Held = OwnerRefusal(TEXT("ACTOR_EXISTS"), TEXT(",\"ownedByCaller\":false"));
	TArray<FString> Replies;
	Replies.Init(Held, UCrowdyActorOwners::MaxRemints + 1);
	Rig.Script(Replies);
	Rig.SignInAs(PlayerId, RandomDigits());
	TestEqual(TEXT("the first id and every replacement are tried once"), Rig.Sends, UCrowdyActorOwners::MaxRemints + 1);
	TestEqual(TEXT("then it warns once"), Rig.Owners->GetRecordWarningsForTest(), 1);
	TestEqual(TEXT("and lets the connection open anyway"), Rig.Opened, 1);
	TestFalse(TEXT("with nothing left to retry"), Rig.Owners->RunRecordRetryForTest());

	Rig.SignOut();
	Rig.Settle();
	TestEqual(TEXT("sign-out deletes none of the records it was refused"), Rig.Sends, UCrowdyActorOwners::MaxRemints + 1);
	TestEqual(TEXT("so the session ends at once"), Rig.Ended, 1);
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
		const FString Refusal = OwnerRefusal(TEXT("PLATFORM_BUSY"), Extensions);
		FActorOwnersRig Rig(Refusal);
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.Script({Refusal, Refusal, Refusal});
		Rig.SignInAs(PlayerId, RandomDigits());
		TestTrue(TEXT("a second try waits"), Rig.Owners->RunRecordRetryForTest());
		Rig.Settle();
		TestTrue(TEXT("a third try waits"), Rig.Owners->RunRecordRetryForTest());
		Rig.Settle();
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
	const FString Id = RandomDigits();
	Rig.Script({Failure, Failure, Failure, CreatedAnswer});
	Rig.SignInAs(PlayerId, Id);
	Rig.Owners->RunRecordRetryForTest();
	Rig.Settle();
	Rig.Owners->RunRecordRetryForTest();
	Rig.Settle();
	TestEqual(TEXT("gave up after three creates"), Rig.Sends, 3);
	TestEqual(TEXT("and let the connection open"), Rig.Opened, 1);

	Rig.ChangeId(Id);
	TestEqual(TEXT("the next id update creates again, deleting nothing"), Rig.Sends, 4);
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
	const FString Id = RandomDigits();
	Rig.Script({CreatedAnswer, DeletedAnswer, CreatedAnswer});
	Rig.SignInAs(PlayerId, Id);
	Rig.SignOut();
	Rig.SignInAs(PlayerId, Id);
	TestEqual(TEXT("a create, the sign-out's delete, and the next sign-in's create"), Rig.Sends, 3);
	TestTrue(TEXT("the record is made again"), Rig.Bodies.Num() == 3 && CreatesRecord(Rig.Bodies[2], Id));
	return true;
}

// A delete sent while the create is still out could reach the server first, so sign-out waits for the create: one that
// lands is deleted under the token the sign-out held, which the session has forgotten by then.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersLateCreateDeletedTest,
	"CrowdySDK.Services.ActorOwners.ACreateInFlightAtSignOutIsDeletedUnderItsTokenOnceItLands", ActorOwnersTestFlags)

bool FCrowdyActorOwnersLateCreateDeletedTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Id = RandomDigits();
	Rig.Script({CreatedAnswer, DeletedAnswer});
	Rig.Session->SetGameToken(TEXT("app-signed-out"));
	Rig.Client->SetGameToken(TEXT("app-signed-out"));
	Rig.Session->SetUserID(PlayerId);
	Rig.Session->SetUUID(Id);
	Rig.Owners->HandleSignedIn(nullptr);
	TestEqual(TEXT("the create is out"), Rig.Sends, 1);

	Rig.SignOut();
	TestEqual(TEXT("no delete races it"), Rig.Sends, 1);
	TestEqual(TEXT("and the session waits for it"), Rig.Ended, 0);

	// What a sign-out does next: the session and the client forget the token.
	Rig.Session->SetGameToken(FString());
	Rig.Client->SetGameToken(FString());
	Rig.Settle();
	if (!TestEqual(TEXT("the create, then the delete"), Rig.Bodies.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the record that landed is deleted"), DeletesRecord(Rig.Bodies[1], Id));
	TestEqual(TEXT("under the token the sign-out held"), Rig.Authorizations[1], FString(TEXT("Bearer app-signed-out")));
	TestEqual(TEXT("and the session ends after it"), Rig.Ended, 1);

	FActorOwnersRig Failed(CreatedAnswer);
	if (!TestTrue(TEXT("second client built"), Failed.Client.IsValid()))
	{
		return false;
	}
	Failed.Script({OwnerRefusal(TEXT("INTERNAL_ERROR"), TEXT(""))});
	Failed.Session->SetUserID(PlayerId);
	Failed.Session->SetUUID(RandomDigits());
	Failed.Owners->HandleSignedIn(nullptr);
	Failed.SignOut();
	Failed.Settle();
	TestEqual(TEXT("a create that failed leaves nothing to delete"), Failed.Sends, 1);
	TestEqual(TEXT("so its session ends once it answers"), Failed.Ended, 1);
	TestFalse(TEXT("and it is not tried again"), Failed.Owners->RunRecordRetryForTest());
	return true;
}

// Two sign-outs in a row, each leaving a create in flight: the second folds into the first rather than ending its
// session early, both creates that land are deleted under their own sign-in's token, and the sessions end only after.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersTwoLateCreatesTest,
	"CrowdySDK.Services.ActorOwners.TwoSignOutsWithCreatesInFlightDeleteBothBeforeEitherSessionEnds", ActorOwnersTestFlags)

bool FCrowdyActorOwnersTwoLateCreatesTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, CreatedAnswer, DeletedAnswer, DeletedAnswer});
	const FString First = RandomDigits();
	const FString Second = RandomDigits();

	Rig.Session->SetUserID(PlayerId);
	Rig.Session->SetGameToken(TEXT("app-one"));
	Rig.Session->SetUUID(First);
	Rig.Owners->HandleSignedIn(nullptr);
	Rig.SignOut();

	Rig.Session->SetGameToken(TEXT("app-two"));
	Rig.Session->SetUUID(Second);
	Rig.Owners->HandleSignedIn(nullptr);
	Rig.SignOut();
	TestTrue(TEXT("two creates out, and neither session ended"), Rig.Sends == 2 && Rig.Ended == 0);

	Rig.Settle();
	const int32 FirstDelete = Rig.Bodies.IndexOfByPredicate([&First](const FString& Body) { return DeletesRecord(Body, First); });
	const int32 SecondDelete = Rig.Bodies.IndexOfByPredicate([&Second](const FString& Body) { return DeletesRecord(Body, Second); });
	if (!TestTrue(TEXT("both landed records are deleted"), FirstDelete != INDEX_NONE && SecondDelete != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("the first under its sign-in's token"), Rig.Authorizations[FirstDelete], FString(TEXT("Bearer app-one")));
	TestEqual(TEXT("the second under its own"), Rig.Authorizations[SecondDelete], FString(TEXT("Bearer app-two")));
	TestTrue(TEXT("both sessions end, after every delete went out"), Rig.Ended == 2 && Rig.SendsWhenEnded[0] == 4);
	return true;
}

// An existing record is the player's only when the answer says so: one it does not is used, but never deleted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersUnsaidOwnerKeptTest,
	"CrowdySDK.Services.ActorOwners.AnExistingRecordNotSaidToBeOursIsUsedButNeverDeleted", ActorOwnersTestFlags)

bool FCrowdyActorOwnersUnsaidOwnerKeptTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({OwnerRefusal(TEXT("ACTOR_EXISTS"), TEXT("")), DeletedAnswer});
	Rig.SignInAs(PlayerId, RandomDigits());
	TestEqual(TEXT("the record is used, so the connection opens"), Rig.Opened, 1);
	Rig.SignOut();
	Rig.Settle();
	TestEqual(TEXT("sign-out deletes nothing"), Rig.Sends, 1);
	TestEqual(TEXT("so the session ends at once"), Rig.Ended, 1);

	FActorOwnersRig Ours(CreatedAnswer);
	if (!TestTrue(TEXT("second client built"), Ours.Client.IsValid()))
	{
		return false;
	}
	const FString Id = RandomDigits();
	Ours.Script({OwnerRefusal(TEXT("ACTOR_EXISTS"), TEXT(",\"ownedByCaller\":true")), DeletedAnswer});
	Ours.SignInAs(PlayerId, Id);
	Ours.SignOut();
	Ours.Settle();
	TestTrue(TEXT("one said to be the player's is deleted at sign-out"), Ours.Bodies.Num() == 2 && DeletesRecord(Ours.Bodies[1], Id));
	return true;
}

// The game instance ending while a sign-out waits for its deletes ends the server session at once, rather than leaving
// it valid until it expires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersDeinitializeEndsSignOutTest,
	"CrowdySDK.Services.ActorOwners.EndingTheGameInstanceEndsAWaitingSignOutsSession", ActorOwnersTestFlags)

bool FCrowdyActorOwnersDeinitializeEndsSignOutTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, RandomDigits());
	Rig.SignOut();
	TestEqual(TEXT("the session waits for the delete"), Rig.Ended, 0);
	Rig.Owners->Deinitialize();
	TestEqual(TEXT("until the game instance ends"), Rig.Ended, 1);
	return true;
}

// A new id during a sign-in closes the connection, and only the new id's record reopens it, so nothing goes out under
// an id before its record answers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorOwnersIdChangeResettlesTest,
	"CrowdySDK.Services.ActorOwners.AnIdChangeDuringASignInReopensOnlyAfterItsRecord", ActorOwnersTestFlags)

bool FCrowdyActorOwnersIdChangeResettlesTest::RunTest(const FString& Parameters)
{
	FActorOwnersRig Rig(CreatedAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Script({CreatedAnswer, CreatedAnswer, DeletedAnswer});
	Rig.SignInAs(PlayerId, RandomDigits());
	TestEqual(TEXT("the sign-in's record opens the connection"), Rig.Opened, 1);

	Rig.Session->SetUUID(RandomDigits());
	TestTrue(TEXT("a new id waits for its own record"), Rig.Owners->IsSettlingSignIn());
	Rig.Settle();
	TestEqual(TEXT("whose answer opens the connection again"), Rig.Opened, 2);
	TestTrue(TEXT("after the new create went out"), Rig.SendsWhenOpened.Num() == 2 && Rig.SendsWhenOpened[1] >= 2);
	return true;
}

#endif
