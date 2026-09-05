#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

// Covers the delivery contract the latent-node surface rests on: a completion is delivered exactly once and never
// dropped, whether it is the server that answers, the caller that cancels, or the client that goes away first. A
// request whose completion never arrives is indistinguishable from a hang, and it leaves the in-flight flag that
// guards re-entry set for the rest of the session.
namespace CrowdyCppCancelTestSupport
{
	constexpr EAutomationTestFlags CancelTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A client whose canned transport answers every request, so a completion is genuinely waiting to be delivered
	// between issuing the request and the next Poll(). That gap is the window a cancellation has to win.
	TSharedPtr<FCrowdyCppClient> MakeCancelTestClient()
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = TEXT("https://game.test");
		Config.DiscoveryUrl = TEXT("https://api.test");
		return FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
	}

	FCrowdyCppRequestHandle IssueRunOp(const TSharedPtr<FCrowdyCppClient>& Client, int32& OutCallCount,
		FString& OutError)
	{
		return Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
			[&OutCallCount, &OutError](FCrowdyCppJsonResult Result)
			{
				++OutCallCount;
				OutError = Result.ErrorMessage;
			});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppCancelDeliversOnceTest,
	"CrowdySDK.CrowdyCpp.CancelDeliversOnce", CrowdyCppCancelTestSupport::CancelTestFlags)

bool FCrowdyCppCancelDeliversOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppCancelTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeCancelTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 CallCount = 0;
	FString Error;
	const FCrowdyCppRequestHandle Handle = IssueRunOp(Client, CallCount, Error);

	TestTrue(TEXT("issuing returns a usable handle"), Handle.IsValid());
	TestEqual(TEXT("the request is pending before it is drained"), Client->NumPendingRequests(), 1);
	TestEqual(TEXT("nothing has been delivered yet"), CallCount, 0);

	TestTrue(TEXT("cancelling a pending request reports that it did something"), Client->Cancel(Handle));
	TestEqual(TEXT("the completion is delivered by the cancellation"), CallCount, 1);
	TestEqual(TEXT("and it says it was canceled"), Error, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("the request is no longer pending"), Client->NumPendingRequests(), 0);

	// The server's answer was already queued when the cancellation ran. Draining it must not deliver a second
	// completion for the same request: a caller that has moved on would be handed a result for work it abandoned.
	Client->Poll();
	TestEqual(TEXT("the server's late answer is discarded"), CallCount, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppCancelAfterCompleteTest,
	"CrowdySDK.CrowdyCpp.CancelAfterCompleteIsNoOp", CrowdyCppCancelTestSupport::CancelTestFlags)

bool FCrowdyCppCancelAfterCompleteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppCancelTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeCancelTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 CallCount = 0;
	FString Error;
	const FCrowdyCppRequestHandle Handle = IssueRunOp(Client, CallCount, Error);

	Client->Poll();
	TestEqual(TEXT("the request completed normally"), CallCount, 1);
	TestNotEqual(TEXT("a completed request did not report a cancellation"), Error,
		FCrowdyCppClient::CanceledErrorMessage());

	// Cancelling late is the normal shape for a caller that gives up at the same moment the answer arrives, so it
	// has to be harmless rather than a second delivery or a failed check.
	TestFalse(TEXT("cancelling a completed request finds nothing to cancel"), Client->Cancel(Handle));
	TestEqual(TEXT("no second completion is delivered"), CallCount, 1);

	TestFalse(TEXT("a default-constructed handle refers to nothing"), Client->Cancel(FCrowdyCppRequestHandle()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppCancelAllDrainsTest,
	"CrowdySDK.CrowdyCpp.CancelAllDrainsEveryPending", CrowdyCppCancelTestSupport::CancelTestFlags)

bool FCrowdyCppCancelAllDrainsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppCancelTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = MakeCancelTestClient();
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	// One of every call shape, because each one wraps its caller's completion differently and a shape that forgot to
	// register would be silently uncancelable.
	int32 RunOpCalls = 0;
	FString RunOpError;
	IssueRunOp(Client, RunOpCalls, RunOpError);

	int32 ReadCalls = 0;
	FString ReadError;
	Client->ReadContainerState(1, TEXT("c-1"),
		[&ReadCalls, &ReadError](FCrowdyCppContainerStateResult Result)
		{
			++ReadCalls;
			ReadError = Result.ErrorMessage;
		});

	int32 InvokeCalls = 0;
	FString InvokeError;
	Client->InvokeFunction(1, TEXT("fn"), TEXT("c-1"), FString(), TEXT("{}"),
		[&InvokeCalls, &InvokeError](FCrowdyCppInvokeResult Result)
		{
			++InvokeCalls;
			InvokeError = Result.ErrorMessage;
		});

	int32 ListCalls = 0;
	bool bListOk = true;
	int32 ListRows = -1;
	Client->ListContainers(1, TEXT("Type"), FString(),
		[&ListCalls, &bListOk, &ListRows](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
		{
			++ListCalls;
			bListOk = bOk;
			ListRows = Containers.Num();
		});

	int32 SeedCalls = 0;
	FString SeedError;
	Client->SeedSchema(TEXT("{}"),
		[&SeedCalls, &SeedError](FCrowdyCppStudioOpResult Result)
		{
			++SeedCalls;
			SeedError = Result.ErrorMessage;
		});

	TestEqual(TEXT("every call shape registered a pending request"), Client->NumPendingRequests(), 5);

	TestEqual(TEXT("cancelling all delivers every pending completion"), Client->CancelAll(), 5);
	TestEqual(TEXT("nothing is left pending"), Client->NumPendingRequests(), 0);

	TestEqual(TEXT("the run op completed once"), RunOpCalls, 1);
	TestEqual(TEXT("the run op was canceled"), RunOpError, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("the container read completed once"), ReadCalls, 1);
	TestEqual(TEXT("the container read was canceled"), ReadError, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("the invoke completed once"), InvokeCalls, 1);
	TestEqual(TEXT("the invoke was canceled"), InvokeError, FCrowdyCppClient::CanceledErrorMessage());
	TestEqual(TEXT("the list completed once"), ListCalls, 1);
	TestFalse(TEXT("a canceled list reports failure"), bListOk);
	TestEqual(TEXT("a canceled list carries no rows"), ListRows, 0);
	TestEqual(TEXT("the seed completed once"), SeedCalls, 1);
	TestEqual(TEXT("the seed was canceled"), SeedError, FCrowdyCppClient::CanceledErrorMessage());

	// Draining the transport afterwards must not resurrect any of them.
	Client->Poll();
	TestEqual(TEXT("no run op completion arrives late"), RunOpCalls, 1);
	TestEqual(TEXT("no container read completion arrives late"), ReadCalls, 1);
	TestEqual(TEXT("no invoke completion arrives late"), InvokeCalls, 1);
	TestEqual(TEXT("no list completion arrives late"), ListCalls, 1);
	TestEqual(TEXT("no seed completion arrives late"), SeedCalls, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppDestroyCancelsPendingTest,
	"CrowdySDK.CrowdyCpp.DestroyingClientCancelsPending", CrowdyCppCancelTestSupport::CancelTestFlags)

bool FCrowdyCppDestroyCancelsPendingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppCancelTestSupport;

	int32 CallCount = 0;
	FString Error;

	{
		TSharedPtr<FCrowdyCppClient> Client = MakeCancelTestClient();
		if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
		{
			return false;
		}
		IssueRunOp(Client, CallCount, Error);
		TestEqual(TEXT("nothing has been delivered yet"), CallCount, 0);

		// Releasing the last reference is the whole teardown protocol. An owner that simply drops the client, which
		// is what an editor flow being abandoned looks like, must not strand the caller waiting on it.
		Client.Reset();
	}

	TestEqual(TEXT("destroying the client delivers the pending completion"), CallCount, 1);
	TestEqual(TEXT("and it says it was canceled"), Error, FCrowdyCppClient::CanceledErrorMessage());

	// Closing explicitly and then releasing must not deliver twice.
	int32 ClosedCalls = 0;
	FString ClosedError;
	{
		TSharedPtr<FCrowdyCppClient> Client = MakeCancelTestClient();
		IssueRunOp(Client, ClosedCalls, ClosedError);
		Client->Close();
		TestEqual(TEXT("closing delivers the pending completion"), ClosedCalls, 1);
		TestEqual(TEXT("closing reports it as canceled"), ClosedError, FCrowdyCppClient::CanceledErrorMessage());
		Client.Reset();
	}
	TestEqual(TEXT("releasing after a close does not deliver again"), ClosedCalls, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppCancelReissueTest,
	"CrowdySDK.CrowdyCpp.CanceledCompletionMayReissue", CrowdyCppCancelTestSupport::CancelTestFlags)

bool FCrowdyCppCancelReissueTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppCancelTestSupport;

	// The endpoint-switch shape in miniature: the client a request is in flight on is retired, and the caller reacts
	// to the cancellation by reissuing against the client that replaced it. The reissue happens synchronously inside
	// the retiring client's disposal, which is the ordering that has to hold.
	const TSharedPtr<FCrowdyCppClient> Replacement = MakeCancelTestClient();
	TSharedPtr<FCrowdyCppClient> Retiring = MakeCancelTestClient();
	if (!TestTrue(TEXT("both test clients constructed"), Replacement.IsValid() && Retiring.IsValid()))
	{
		return false;
	}

	int32 FirstCalls = 0;
	int32 ReissuedCalls = 0;
	bool bReissuedOk = false;

	Retiring->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
		[&FirstCalls, &ReissuedCalls, &bReissuedOk, Replacement](FCrowdyCppJsonResult Result)
		{
			++FirstCalls;
			if (Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
			{
				Replacement->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"),
					MakeShared<FJsonObject>(),
					[&ReissuedCalls, &bReissuedOk](FCrowdyCppJsonResult Reissued)
					{
						++ReissuedCalls;
						bReissuedOk = Reissued.bTransportOk;
					});
			}
		});

	Retiring->Close();
	TestEqual(TEXT("the retiring client canceled its request"), FirstCalls, 1);
	TestEqual(TEXT("the reissue is pending on the replacement"), Replacement->NumPendingRequests(), 1);

	Replacement->Poll();
	TestEqual(TEXT("the reissued request completed"), ReissuedCalls, 1);
	TestTrue(TEXT("the reissued request reached the replacement's transport"), bReissuedOk);

	// A caller that reissues on the retiring client instead gets an immediate failure rather than a request queued
	// on a disposed client, so the reaction to a cancellation can never hang either way.
	int32 LateCalls = 0;
	bool bLateOk = true;
	Retiring->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
		[&LateCalls, &bLateOk](FCrowdyCppJsonResult Result)
		{
			++LateCalls;
			bLateOk = Result.bTransportOk;
		});
	TestEqual(TEXT("issuing on a disposed client completes immediately"), LateCalls, 1);
	TestFalse(TEXT("and reports failure"), bLateOk);
	TestEqual(TEXT("a disposed client keeps nothing pending"), Retiring->NumPendingRequests(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
