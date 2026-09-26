#include "CrowdyCppClient.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"

// A request the platform refuses and says to retry is sent again: queries and ensures by the bridge, invokes by the
// subsystem. Everything else, and everything with the switch off, reaches the caller at once.
namespace CrowdyBusyRetryTestSupport
{
	constexpr EAutomationTestFlags BusyRetryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const BusyRetryTestEndpoint = TEXT("https://game.test");

	// A thrown refusal; RetryAfterMs below zero leaves the field out.
	FString RefusalBody(const TCHAR* Code, const TCHAR* Blame = TEXT("PLATFORM"), bool bRetryable = true,
		int32 RetryAfterMs = 0)
	{
		const FString After = RetryAfterMs >= 0 ? FString::Printf(TEXT(",\"retryAfterMs\":%d"), RetryAfterMs) : FString();
		return FString::Printf(
			TEXT("{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"%s\",\"blame\":\"%s\",\"retryable\":%s%s}}],\"data\":null}"),
			Code, Blame, bRetryable ? TEXT("true") : TEXT("false"), *After);
	}

	FString BusyBody(const TCHAR* Blame = TEXT("PLATFORM"), bool bRetryable = true, int32 RetryAfterMs = 0)
	{
		return RefusalBody(TEXT("PLATFORM_BUSY"), Blame, bRetryable, RetryAfterMs);
	}

	TArray<TPair<int32, FString>> Repeat(const FString& Body, int32 Count)
	{
		TArray<TPair<int32, FString>> Script;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Script.Emplace(200, Body);
		}
		return Script;
	}

	TSharedPtr<FCrowdyCppClient> MakeClient(int32& OutSends)
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = BusyRetryTestEndpoint;
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
		if (Client.IsValid())
		{
			Client->SetTestOnRequest([&OutSends](const FString&) { ++OutSends; });
		}
		return Client;
	}

	struct FOpOutcome
	{
		int32 Delivered = 0;
		FCrowdyCppJsonResult Last;
	};

	FCrowdyCppRequestHandle IssueOp(const TSharedPtr<FCrowdyCppClient>& Client, const TCHAR* Operation, FOpOutcome& Out)
	{
		return Client->RunOp(ECrowdyCppApiDomain::GameModel, Operation, MakeShared<FJsonObject>(),
			[&Out](FCrowdyCppJsonResult Result)
			{
				++Out.Delivered;
				Out.Last = MoveTemp(Result);
			});
	}

	void PollTimes(const TSharedPtr<FCrowdyCppClient>& Client, int32 Times)
	{
		for (int32 Index = 0; Index < Times; ++Index)
		{
			Client->Poll();
		}
	}

	// Polls until Done or TimeoutSeconds pass; a retry waits at least 100 ms.
	template <typename FDonePredicate>
	bool PollUntil(const TSharedPtr<FCrowdyCppClient>& Client, FDonePredicate Done, double TimeoutSeconds = 3.0)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		Client->Poll();
		while (!Done() && FPlatformTime::Seconds() - StartSeconds < TimeoutSeconds)
		{
			FPlatformProcess::Sleep(0.005f);
			Client->Poll();
		}
		return Done();
	}

	FCrowdyCppOpStats FindOpStats(const TSharedPtr<FCrowdyCppClient>& Client, const FString& Operation)
	{
		TArray<FCrowdyCppOpStats> Ops;
		FCrowdyCppTransportStats Transport;
		Client->GetStats(Ops, Transport);
		const FCrowdyCppOpStats* Found =
			Ops.FindByPredicate([&Operation](const FCrowdyCppOpStats& Op) { return Op.Operation == Operation; });
		return Found ? *Found : FCrowdyCppOpStats();
	}

	int32 CountReason(const FCrowdyCppOpStats& Op, const TCHAR* Reason)
	{
		const TPair<FString, int32>* Found = Op.FailureReasons.FindByPredicate(
			[Reason](const TPair<FString, int32>& Pair) { return Pair.Key == Reason; });
		return Found ? Found->Value : 0;
	}

	// Sets crowdy.net.retry.busy for the life of the scope.
	struct FBusyRetrySwitchScope
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FBusyRetrySwitchScope(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.net.retry.busy"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByCode);
			}
		}

		~FBusyRetrySwitchScope()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByCode);
			}
		}
	};

	// A real world, so the subsystem has a timer manager to arm an invoke retry on.
	struct FBusyRetryTestWorld
	{
		UWorld* World = nullptr;

		FBusyRetryTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FBusyRetryTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		// The first tick activates a timer armed this frame, the second crosses it.
		void FireTimers() const
		{
			World->GetTimerManager().Tick(6.0f);
			++GFrameCounter;
			World->GetTimerManager().Tick(6.0f);
		}
	};

	const TCHAR* const InvokeSuccessBody =
		TEXT("{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"1\",\"errorMessage\":\"\",\"mutationsApplied\":[]}}}");

	const TCHAR* const RateLimitedBody =
		TEXT("{\"errors\":[{\"message\":\"Too many calls\",\"extensions\":{\"code\":\"RATE_LIMITED\",\"blame\":\"BUDGET\",\"retryable\":true,\"retryAfterMs\":0}}]}");

	FCrowdyInvokeResult ThrownBusy()
	{
		FCrowdyInvokeResult Result;
		Result.bTransportOk = false;
		Result.bSuccess = false;
		Result.bRetryable = true;
		Result.Blame = ECrowdyPlayerFaultBlame::Platform;
		Result.FaultCode = TEXT("PLATFORM_BUSY");
		return Result;
	}

	struct FInvokeOutcome
	{
		int32 Delivered = 0;
		FCrowdyInvokeResult Last;
	};

	void InvokeCounting(UCrowdyGameModelSubsystem* Model, FInvokeOutcome& Out)
	{
		FCrowdyInvokeRequest Request;
		Request.FunctionName = TEXT("busy_retry_fn");
		Request.SelfContainerId = TEXT("c1");
		Model->Invoke(Request, [&Out](FCrowdyInvokeResult Result)
		{
			++Out.Delivered;
			Out.Last = MoveTemp(Result);
		});
	}

	// Answers and retries rounds until the invoke is delivered.
	void DriveInvoke(const FBusyRetryTestWorld& Env, FCrowdyGameModelTestClientHost& ClientHost, const FInvokeOutcome& Out)
	{
		for (int32 Round = 0; Round < 12 && Out.Delivered == 0; ++Round)
		{
			ClientHost.Client->Poll();
			Env.FireTimers();
		}
	}

	UCrowdyGameModelSubsystem* MakeLiveModel(UWorld* World)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(World);
		Model->BeginWorldSessionForTest();
		Model->SetApiContextForTest(BusyRetryTestEndpoint, TEXT("test-token"), 42);
		return Model;
	}
}

// A busy refusal and then a PLATFORM_ERROR on a query are both retried: a repeated read is harmless.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryRecoversTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.RecoversAfterBusy", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryRecoversTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()),
		TPair<int32, FString>(200, RefusalBody(TEXT("PLATFORM_ERROR"))) });

	FOpOutcome Outcome;
	IssueOp(Client, TEXT("GameModelSession"), Outcome);
	PollUntil(Client, [&Outcome] { return Outcome.Delivered > 0; });

	TestEqual(TEXT("the caller hears once"), Outcome.Delivered, 1);
	TestTrue(TEXT("and hears the success"), Outcome.Last.bTransportOk);
	TestEqual(TEXT("the query was sent three times"), Sends, 3);
	const FCrowdyCppOpStats Op = FindOpStats(Client, TEXT("GameModelSession"));
	TestEqual(TEXT("one call"), Op.Calls, 1);
	TestEqual(TEXT("no failure"), Op.Failures, 0);
	TestEqual(TEXT("two re-sends"), Op.Retries, 2);
	TestEqual(TEXT("one request recovered"), Op.RecoveredByRetry, 1);

	Client->ResetStats();
	const FCrowdyCppOpStats Reset = FindOpStats(Client, TEXT("GameModelSession"));
	TestEqual(TEXT("a reset clears the re-sends"), Reset.Retries, 0);
	TestEqual(TEXT("and the recoveries"), Reset.RecoveredByRetry, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryGivesUpTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.GivesUpAfterThree", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryGivesUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestResponseScript(Repeat(BusyBody(), 5));

	FOpOutcome Outcome;
	IssueOp(Client, TEXT("GameModelContainerStates"), Outcome);
	PollUntil(Client, [&Outcome] { return Outcome.Delivered > 0; });

	TestEqual(TEXT("the caller hears once"), Outcome.Delivered, 1);
	TestFalse(TEXT("and hears the refusal"), Outcome.Last.bTransportOk);
	TestEqual(TEXT("the first send and three retries"), Sends, 1 + CrowdyCppMaxBusyRetries);
	TestEqual(TEXT("nothing is left waiting"), Client->NumTestWaitingRetries(), 0);
	TestEqual(TEXT("the refusal's code"), Outcome.Last.ErrorCode, FString(TEXT("PLATFORM_BUSY")));
	TestEqual(TEXT("the refusal's blame"), Outcome.Last.Blame, FString(TEXT("PLATFORM")));
	TestTrue(TEXT("the refusal says it may be retried"), Outcome.Last.bRetryable);
	TestTrue(TEXT("with the server's wait"), Outcome.Last.RetryAfterMs.IsSet() && Outcome.Last.RetryAfterMs.GetValue() == 0);

	const FCrowdyCppOpStats Op = FindOpStats(Client, TEXT("GameModelContainerStates"));
	TestEqual(TEXT("one call"), Op.Calls, 1);
	TestEqual(TEXT("one failure"), Op.Failures, 1);
	TestEqual(TEXT("three re-sends"), Op.Retries, CrowdyCppMaxBusyRetries);
	TestEqual(TEXT("nothing recovered"), Op.RecoveredByRetry, 0);
	TestEqual(TEXT("the busy reason counted once"), CountReason(Op, TEXT("PLATFORM_BUSY")), 1);
	return true;
}

// Only a refusal the platform blames on itself and says to retry, and never a redirect or an unservable app.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryOnlyPlatformRetryableTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.OnlyPlatformRetryable", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryOnlyPlatformRetryableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	const FString NoBlame = TEXT(
		"{\"errors\":[{\"message\":\"busy\",\"extensions\":{\"code\":\"PLATFORM_BUSY\"}}],\"data\":null}");
	const TPair<const TCHAR*, FString> Cases[] = {
		{ TEXT("retryable false"), BusyBody(TEXT("PLATFORM"), false) },
		{ TEXT("blame AUTHOR"), BusyBody(TEXT("AUTHOR"), true) },
		{ TEXT("no blame"), NoBlame },
		{ TEXT("WRONG_DATACENTER"), RefusalBody(TEXT("WRONG_DATACENTER")) },
		{ TEXT("APP_UNAVAILABLE"), RefusalBody(TEXT("APP_UNAVAILABLE")) },
	};
	for (const TPair<const TCHAR*, FString>& Case : Cases)
	{
		int32 Sends = 0;
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
		if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
		{
			return false;
		}
		Client->SetTestResponseScript({ TPair<int32, FString>(200, Case.Value) });
		FOpOutcome Outcome;
		IssueOp(Client, TEXT("GameModelSession"), Outcome);
		Client->Poll();
		TestEqual(FString::Printf(TEXT("%s: delivered on the first poll"), Case.Key), Outcome.Delivered, 1);
		TestFalse(FString::Printf(TEXT("%s: as the refusal"), Case.Key), Outcome.Last.bTransportOk);
		TestEqual(FString::Printf(TEXT("%s: nothing waits to retry"), Case.Key), Client->NumTestWaitingRetries(), 0);
		TestEqual(FString::Printf(TEXT("%s: sent once"), Case.Key), Sends, 1);
		TestEqual(FString::Printf(TEXT("%s: no re-send counted"), Case.Key),
			FindOpStats(Client, TEXT("GameModelSession")).Retries, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryMutationsNotRetriedTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.MutationsNotRetried", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryMutationsNotRetriedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->SetTestResponseScript(Repeat(BusyBody(), 1));
	FOpOutcome SetProperty;
	IssueOp(Client, TEXT("GameModelSetProperty"), SetProperty);
	Client->Poll();
	TestEqual(TEXT("a mutation's refusal reaches the caller at once"), SetProperty.Delivered, 1);
	TestEqual(TEXT("and it is sent once"), Sends, 1);

	Sends = 0;
	Client->SetTestResponseScript(Repeat(BusyBody(), 1));
	int32 InvokeDelivered = 0;
	Client->InvokeFunction(1, TEXT("fn"), TEXT("c-1"), FString(), TEXT("{}"),
		[&InvokeDelivered](FCrowdyCppInvokeResult) { ++InvokeDelivered; });
	Client->Poll();
	TestEqual(TEXT("an invoke's refusal reaches the caller at once"), InvokeDelivered, 1);
	TestEqual(TEXT("and the bridge sends it once"), Sends, 1);

	// An ensure creates a row, so only a refusal that says the work never started may be repeated.
	Sends = 0;
	Client->SetTestResponseScript(Repeat(RefusalBody(TEXT("PLATFORM_ERROR")), 1));
	FOpOutcome EnsureError;
	IssueOp(Client, TEXT("GameModelEnsureContainer"), EnsureError);
	Client->Poll();
	TestEqual(TEXT("an ensure's PLATFORM_ERROR reaches the caller at once"), EnsureError.Delivered, 1);
	TestEqual(TEXT("and it is sent once"), Sends, 1);

	Sends = 0;
	Client->SetTestResponseScript(Repeat(BusyBody(), 1));
	FOpOutcome Ensure;
	IssueOp(Client, TEXT("GameModelEnsureContainer"), Ensure);
	PollUntil(Client, [&Ensure] { return Ensure.Delivered > 0; });
	TestTrue(TEXT("an ensure's PLATFORM_BUSY recovers"), Ensure.Delivered == 1 && Ensure.Last.bTransportOk);
	TestEqual(TEXT("after one re-send"), Sends, 2);

	Sends = 0;
	Client->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()),
		TPair<int32, FString>(200, TEXT("{\"data\":{\"gameModelContainerState\":{\"containerId\":\"c-1\",\"propertiesJson\":\"{\\\"hp\\\":1}\"}}}")) });
	int32 ReadDelivered = 0;
	bool bReadOk = false;
	Client->ReadContainerState(1, TEXT("c-1"), [&ReadDelivered, &bReadOk](FCrowdyCppContainerStateResult Result)
	{
		++ReadDelivered;
		bReadOk = Result.bOk;
	});
	PollUntil(Client, [&ReadDelivered] { return ReadDelivered > 0; });
	TestTrue(TEXT("a container read recovers"), bReadOk);
	TestEqual(TEXT("after one re-send"), Sends, 2);

	Sends = 0;
	Client->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()),
		TPair<int32, FString>(200, TEXT("{\"data\":{\"gameModelContainers\":[]}}")) });
	int32 ListDelivered = 0;
	bool bListOk = false;
	Client->ListContainers(1, TEXT("T"), FString(), [&ListDelivered, &bListOk](bool bOk, TArray<TSharedPtr<FJsonObject>>)
	{
		++ListDelivered;
		bListOk = bOk;
	});
	PollUntil(Client, [&ListDelivered] { return ListDelivered > 0; });
	TestTrue(TEXT("a container list recovers"), bListOk);
	TestEqual(TEXT("after one re-send"), Sends, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryCancelWhileWaitingTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.CancelWhileWaiting", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryCancelWhileWaitingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	Client->SetTestResponseScript(Repeat(BusyBody(), 3));
	FOpOutcome Canceled;
	const FCrowdyCppRequestHandle Handle = IssueOp(Client, TEXT("GameModelSession"), Canceled);
	Client->Poll();
	TestEqual(TEXT("a busy refusal is held, not delivered"), Canceled.Delivered, 0);
	TestEqual(TEXT("it waits to retry"), Client->NumTestWaitingRetries(), 1);
	TestEqual(TEXT("and the request is still pending"), Client->NumPendingRequests(), 1);

	TestTrue(TEXT("cancelling it finds it"), Client->Cancel(Handle));
	TestEqual(TEXT("the cancel delivers once"), Canceled.Delivered, 1);
	TestEqual(TEXT("as canceled"), Canceled.Last.ErrorMessage, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("and stops its wait"), Client->NumTestWaitingRetries(), 0);

	FOpOutcome Drained;
	IssueOp(Client, TEXT("GameModelSession"), Drained);
	Client->Poll();
	TestEqual(TEXT("CancelAll reaches a request waiting to retry"), Client->CancelAll(), 1);
	TestEqual(TEXT("and delivers it once"), Drained.Delivered, 1);
	TestEqual(TEXT("and stops its wait"), Client->NumTestWaitingRetries(), 0);

	FOpOutcome Closed;
	IssueOp(Client, TEXT("GameModelSession"), Closed);
	Client->Poll();
	Client->Close();
	TestEqual(TEXT("Close delivers a request waiting to retry once"), Closed.Delivered, 1);
	TestEqual(TEXT("as canceled"), Closed.Last.ErrorMessage, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("and stops its wait"), Client->NumTestWaitingRetries(), 0);
	FPlatformProcess::Sleep(0.3f);
	PollTimes(Client, 3);
	TestEqual(TEXT("none of them was sent again"), Sends, 3);
	TestEqual(TEXT("nor delivered again"), Canceled.Delivered + Drained.Delivered + Closed.Delivered, 3);
	return true;
}

// A re-sent attempt whose answer has not been delivered yet is canceled like any other pending request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryCancelInFlightRetryTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.CancelInFlightRetry", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryCancelInFlightRetryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	for (const bool bClose : { false, true })
	{
		const TCHAR* Name = bClose ? TEXT("Close") : TEXT("CancelAll");
		int32 Sends = 0;
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
		if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
		{
			return false;
		}
		Client->SetTestResponseScript(Repeat(BusyBody(), 1));
		FOpOutcome Outcome;
		IssueOp(Client, TEXT("GameModelSession"), Outcome);
		if (!TestTrue(FString::Printf(TEXT("%s: the retry went out"), Name), PollUntil(Client, [&Sends] { return Sends == 2; })))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s: its answer is not delivered yet"), Name), Outcome.Delivered, 0);

		if (bClose)
		{
			Client->Close();
		}
		else
		{
			Client->CancelAll();
		}
		TestEqual(FString::Printf(TEXT("%s: delivered once"), Name), Outcome.Delivered, 1);
		TestEqual(FString::Printf(TEXT("%s: as canceled"), Name), Outcome.Last.ErrorMessage,
			FCrowdyCppClient::CanceledErrorMessage());
		PollTimes(Client, 3);
		TestEqual(FString::Printf(TEXT("%s: the late answer is dropped"), Name), Outcome.Delivered, 1);
		TestEqual(FString::Printf(TEXT("%s: nothing more is sent"), Name), Sends, 2);
	}
	return true;
}

// The due time is read back rather than timed, so a slow machine cannot turn this red or green.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryHonoursWaitTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.HonoursWait", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryHonoursWaitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	struct FCase
	{
		const TCHAR* Name;
		int32 RetryAfterMs;
		double MinSeconds;
		double MaxSeconds;
	};
	const FCase Cases[] = {
		{ TEXT("a named wait, plus up to 20%"), 3000, 3.0, 3.6 },
		{ TEXT("a zero wait, raised to the floor"), 0, 0.1, 0.12 },
		{ TEXT("no named wait, the local backoff"), -1, 0.1, 0.2 },
	};
	for (const FCase& Case : Cases)
	{
		int32 Sends = 0;
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
		if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
		{
			return false;
		}
		Client->SetTestResponseScript(Repeat(BusyBody(TEXT("PLATFORM"), true, Case.RetryAfterMs), 1));
		FOpOutcome Outcome;
		IssueOp(Client, TEXT("GameModelSession"), Outcome);
		const double BeforeSeconds = FPlatformTime::Seconds();
		Client->Poll();
		const double AfterSeconds = FPlatformTime::Seconds();
		const double DueSeconds = Client->GetTestNextRetryDueSeconds();
		TestTrue(FString::Printf(TEXT("%s: not due before its wait"), Case.Name), DueSeconds >= BeforeSeconds + Case.MinSeconds);
		TestTrue(FString::Printf(TEXT("%s: nor after it"), Case.Name), DueSeconds <= AfterSeconds + Case.MaxSeconds);
		Client->CancelAll();
	}

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestResponseScript(Repeat(BusyBody(TEXT("PLATFORM"), true, 3000), 1));
	FOpOutcome Early;
	IssueOp(Client, TEXT("GameModelSession"), Early);
	PollTimes(Client, 3);
	TestEqual(TEXT("nothing is sent before a 3 s wait"), Sends, 1);
	Client->CancelAll();

	Sends = 0;
	Client->SetTestResponseScript(Repeat(BusyBody(TEXT("PLATFORM"), true, 6000), 1));
	FOpOutcome TooLong;
	IssueOp(Client, TEXT("GameModelSession"), TooLong);
	Client->Poll();
	TestEqual(TEXT("a wait past 5 s goes to the caller at once"), TooLong.Delivered, 1);
	TestEqual(TEXT("with the server's wait on it"), TooLong.Last.RetryAfterMs.Get(0), static_cast<int64>(6000));
	TestEqual(TEXT("and is not retried"), Client->NumTestWaitingRetries(), 0);
	TestEqual(TEXT("sent once"), Sends, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryParkedBoundTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.ParkedRetriesBounded", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryParkedBoundTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	constexpr int32 Requests = FCrowdyCppClient::MaxParkedRetries + 1;
	Client->SetTestResponseScript(Repeat(BusyBody(TEXT("PLATFORM"), true, 3000), Requests));
	int32 Delivered = 0;
	for (int32 Index = 0; Index < Requests; ++Index)
	{
		Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
			[&Delivered](FCrowdyCppJsonResult) { ++Delivered; });
	}
	Client->Poll();
	TestEqual(TEXT("the cap's worth wait to retry"), Client->NumTestWaitingRetries(), FCrowdyCppClient::MaxParkedRetries);
	TestEqual(TEXT("the one past it goes to its caller"), Delivered, 1);
	TestEqual(TEXT("every other one is delivered by a cancel"), Client->CancelAll(), FCrowdyCppClient::MaxParkedRetries);
	TestEqual(TEXT("each exactly once"), Delivered, Requests);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetrySwitchOffTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.SwitchOff", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetrySwitchOffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	const FBusyRetrySwitchScope Off(0);
	if (!TestNotNull(TEXT("the switch exists"), Off.Var))
	{
		return false;
	}
	TestFalse(TEXT("the switch reads off"), CrowdyCppIsBusyRetryEnabled());

	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestResponseScript(Repeat(BusyBody(), 1));
	FOpOutcome Outcome;
	IssueOp(Client, TEXT("GameModelSession"), Outcome);
	Client->Poll();
	TestEqual(TEXT("the refusal reaches the caller at once"), Outcome.Delivered, 1);
	TestFalse(TEXT("as a failure"), Outcome.Last.bTransportOk);
	TestEqual(TEXT("nothing waits to retry"), Client->NumTestWaitingRetries(), 0);
	TestEqual(TEXT("sent once"), Sends, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBusyRetryDelayTest,
	"CrowdySDK.CrowdyCpp.BusyRetry.Delay", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyCppBusyRetryDelayTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("three retries"), CrowdyCppMaxBusyRetries, 3);
	TestFalse(TEXT("a named wait past 5 s is not waited out"),
		CrowdyCppBusyRetryDelaySeconds(0, TOptional<int64>(5001)).IsSet());

	auto InRange = [](const TOptional<double>& Delay, double Min, double Max)
	{
		return Delay.IsSet() && Delay.GetValue() >= Min && Delay.GetValue() <= Max;
	};
	for (int32 Sample = 0; Sample < 64; ++Sample)
	{
		if (!TestTrue(TEXT("a named wait gets 0-20% more"), InRange(CrowdyCppBusyRetryDelaySeconds(2, TOptional<int64>(250)), 0.25, 0.3))
			|| !TestTrue(TEXT("a zero wait is raised to 100 ms"), InRange(CrowdyCppBusyRetryDelaySeconds(0, TOptional<int64>(0)), 0.1, 0.12))
			|| !TestTrue(TEXT("a 5 s wait is still waited out"), InRange(CrowdyCppBusyRetryDelaySeconds(0, TOptional<int64>(5000)), 5.0, 6.0))
			|| !TestTrue(TEXT("the first local wait is 100-200 ms"), InRange(CrowdyCppBusyRetryDelaySeconds(0, TOptional<int64>()), 0.1, 0.2))
			|| !TestTrue(TEXT("the second doubles it"), InRange(CrowdyCppBusyRetryDelaySeconds(1, TOptional<int64>()), 0.2, 0.4))
			|| !TestTrue(TEXT("the third doubles again"), InRange(CrowdyCppBusyRetryDelaySeconds(2, TOptional<int64>()), 0.4, 0.8))
			|| !TestTrue(TEXT("a negative wait is no wait named"), InRange(CrowdyCppBusyRetryDelaySeconds(0, TOptional<int64>(-5)), 0.1, 0.2)))
		{
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyRefusalGateTest,
	"CrowdySDK.GameModel.BusyRetry.RefusalGate", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyRefusalGateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	TestTrue(TEXT("a thrown busy refusal"), UCrowdyGameModelSubsystem::IsBusyRefusal(ThrownBusy()));

	FCrowdyInvokeResult LowerCase = ThrownBusy();
	LowerCase.FaultCode = TEXT("platform_busy");
	TestTrue(TEXT("the code is matched without case"), UCrowdyGameModelSubsystem::IsBusyRefusal(LowerCase));

	FCrowdyInvokeResult InBand = ThrownBusy();
	InBand.bTransportOk = true;
	TestFalse(TEXT("in band, the function ran"), UCrowdyGameModelSubsystem::IsBusyRefusal(InBand));

	FCrowdyInvokeResult Author = ThrownBusy();
	Author.Blame = ECrowdyPlayerFaultBlame::Author;
	TestFalse(TEXT("blame AUTHOR"), UCrowdyGameModelSubsystem::IsBusyRefusal(Author));

	FCrowdyInvokeResult NotRetryable = ThrownBusy();
	NotRetryable.bRetryable = false;
	TestFalse(TEXT("not retryable"), UCrowdyGameModelSubsystem::IsBusyRefusal(NotRetryable));

	FCrowdyInvokeResult OtherCode = ThrownBusy();
	OtherCode.FaultCode = TEXT("PLATFORM_ERROR");
	TestFalse(TEXT("a platform error may have run the function"), UCrowdyGameModelSubsystem::IsBusyRefusal(OtherCode));

	FCrowdyInvokeResult Succeeded = ThrownBusy();
	Succeeded.bSuccess = true;
	TestFalse(TEXT("a success"), UCrowdyGameModelSubsystem::IsBusyRefusal(Succeeded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyInvokeRecoversTest,
	"CrowdySDK.GameModel.BusyRetry.InvokeRecovers", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyInvokeRecoversTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	FBusyRetryTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, BusyRetryTestEndpoint, InvokeSuccessBody);
	ClientHost.Client->SetTestResponseScript(Repeat(BusyBody(), 1));
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	FInvokeOutcome Outcome;
	InvokeCounting(Model, Outcome);
	ClientHost.Client->Poll();
	TestEqual(TEXT("the busy refusal is held"), Outcome.Delivered, 0);
	TestEqual(TEXT("a retry is armed"), Stats.InvokeBusyRetries, 1);

	Env.FireTimers();
	ClientHost.Client->Poll();
	TestEqual(TEXT("the caller hears once"), Outcome.Delivered, 1);
	TestTrue(TEXT("and hears the success"), Outcome.Last.bTransportOk && Outcome.Last.bSuccess);
	TestEqual(TEXT("both attempts were dispatched"), Stats.InvokesDispatched, 2);
	TestEqual(TEXT("and both spend the allowance"), Model->GetRecentInvokeCount(), 2);
	TestEqual(TEXT("one recovered"), Stats.InvokeBusyRecovered, 1);
	TestEqual(TEXT("none given up"), Stats.InvokeBusyGaveUp, 0);

	// A query retried by the bridge shows on its op line.
	ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()),
		TPair<int32, FString>(200, TEXT("{\"data\":{}}")) });
	int32 QueryDelivered = 0;
	ClientHost.Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
		[&QueryDelivered](FCrowdyCppJsonResult) { ++QueryDelivered; });
	PollUntil(ClientHost.Client, [&QueryDelivered] { return QueryDelivered > 0; });

	TArray<FString> Lines;
	Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the busy line is printed"), Lines.Contains(TEXT("[GameModel] invoke busy retries 1 recovered 1 gave up 0")));
	TestTrue(TEXT("the op line carries its retries"), Lines.ContainsByPredicate([](const FString& Line)
	{
		return Line.StartsWith(TEXT("[GameModel] op GameModelSession ")) && Line.Contains(TEXT(" retries 1 recovered 1"));
	}));

	Model->ResetNetStats();
	TestTrue(TEXT("a reset clears the busy counters"),
		Stats.InvokeBusyRetries == 0 && Stats.InvokeBusyRecovered == 0 && Stats.InvokeBusyGaveUp == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyInvokeGivesUpTest,
	"CrowdySDK.GameModel.BusyRetry.InvokeGivesUp", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyInvokeGivesUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	FBusyRetryTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, BusyRetryTestEndpoint, InvokeSuccessBody);
	ClientHost.Client->SetTestResponseScript(Repeat(BusyBody(), 5));
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	FInvokeOutcome Outcome;
	InvokeCounting(Model, Outcome);
	DriveInvoke(Env, ClientHost, Outcome);
	TestEqual(TEXT("the caller hears once"), Outcome.Delivered, 1);
	TestTrue(TEXT("the busy refusal"), UCrowdyGameModelSubsystem::IsBusyRefusal(Outcome.Last));
	TestEqual(TEXT("the first attempt and three retries"), Stats.InvokesDispatched, 1 + CrowdyCppMaxBusyRetries);
	TestEqual(TEXT("three retries counted"), Stats.InvokeBusyRetries, CrowdyCppMaxBusyRetries);
	TestEqual(TEXT("one given up"), Stats.InvokeBusyGaveUp, 1);
	TestEqual(TEXT("none recovered"), Stats.InvokeBusyRecovered, 0);
	return true;
}

// Busy and budget retries are capped apart: two busy retries leave both budget retries, and one more busy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyThenBudgetTest,
	"CrowdySDK.GameModel.BusyRetry.BusyThenBudget", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyThenBudgetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	FBusyRetryTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, BusyRetryTestEndpoint, InvokeSuccessBody);
	ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()),
		TPair<int32, FString>(200, BusyBody()), TPair<int32, FString>(200, RateLimitedBody),
		TPair<int32, FString>(200, RateLimitedBody), TPair<int32, FString>(200, BusyBody()) });
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	FInvokeOutcome Outcome;
	InvokeCounting(Model, Outcome);
	DriveInvoke(Env, ClientHost, Outcome);
	TestEqual(TEXT("the caller hears once"), Outcome.Delivered, 1);
	TestTrue(TEXT("and hears the success"), Outcome.Last.bTransportOk && Outcome.Last.bSuccess);
	TestEqual(TEXT("six attempts"), Stats.InvokesDispatched, 6);
	TestEqual(TEXT("three of the retries were busy ones"), Stats.InvokeBusyRetries, 3);
	TestEqual(TEXT("one recovered"), Stats.InvokeBusyRecovered, 1);
	TestEqual(TEXT("none given up"), Stats.InvokeBusyGaveUp, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyTeardownTest,
	"CrowdySDK.GameModel.BusyRetry.TeardownGivesUp", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyTeardownTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	FBusyRetryTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, BusyRetryTestEndpoint, InvokeSuccessBody);
	ClientHost.Client->SetTestResponseScript(Repeat(BusyBody(), 1));

	FInvokeOutcome Outcome;
	InvokeCounting(Model, Outcome);
	ClientHost.Client->Poll();
	TestEqual(TEXT("a retry is armed"), Model->GetNetStats().InvokeBusyRetries, 1);
	Model->FailPendingWorkForTest();
	TestEqual(TEXT("teardown tells the caller once"), Outcome.Delivered, 1);
	TestEqual(TEXT("and counts the abandoned retry as given up"), Model->GetNetStats().InvokeBusyGaveUp, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBusyInvokeNotRetriedTest,
	"CrowdySDK.GameModel.BusyRetry.InvokeNotRetried", CrowdyBusyRetryTestSupport::BusyRetryTestFlags)
bool FCrowdyGameModelBusyInvokeNotRetriedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyBusyRetryTestSupport;

	// In band the function ran; the others are not the busy refusal, not wanted, or name a wait too long to hold.
	const FString InBand = TEXT(
		"{\"data\":{\"gameModelInvoke\":{\"success\":false,\"returnValueJson\":null,\"errorMessage\":\"busy\",\"mutationsApplied\":[],"
		"\"fault\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":true}}}}");
	struct FCase
	{
		const TCHAR* Name;
		FString Body;
		int32 Switch;
		int32 GaveUp;
	};
	const FCase Cases[] = {
		{ TEXT("in band"), InBand, 1, 0 },
		{ TEXT("blame AUTHOR"), BusyBody(TEXT("AUTHOR")), 1, 0 },
		{ TEXT("PLATFORM_ERROR"), RefusalBody(TEXT("PLATFORM_ERROR")), 1, 0 },
		{ TEXT("switch off"), BusyBody(), 0, 1 },
		{ TEXT("wait past 5 s"), BusyBody(TEXT("PLATFORM"), true, 6000), 1, 1 },
	};
	for (const FCase& Case : Cases)
	{
		const FBusyRetrySwitchScope Switch(Case.Switch);
		FBusyRetryTestWorld Env;
		UCrowdyGameModelSubsystem* Model = MakeLiveModel(Env.World);
		FCrowdyGameModelTestClientHost ClientHost(Model, BusyRetryTestEndpoint, InvokeSuccessBody);
		ClientHost.Client->SetTestResponseScript(Repeat(Case.Body, 1));

		FInvokeOutcome Outcome;
		InvokeCounting(Model, Outcome);
		ClientHost.Client->Poll();
		TestEqual(FString::Printf(TEXT("%s: delivered at once"), Case.Name), Outcome.Delivered, 1);
		TestFalse(FString::Printf(TEXT("%s: as the refusal"), Case.Name), Outcome.Last.bSuccess);
		Env.FireTimers();
		ClientHost.Client->Poll();
		TestEqual(FString::Printf(TEXT("%s: dispatched once"), Case.Name), Model->GetNetStats().InvokesDispatched, 1);
		TestEqual(FString::Printf(TEXT("%s: no retry"), Case.Name), Model->GetNetStats().InvokeBusyRetries, 0);
		TestEqual(FString::Printf(TEXT("%s: gave up"), Case.Name), Model->GetNetStats().InvokeBusyGaveUp, Case.GaveUp);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
