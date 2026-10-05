#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"

// Every Game Model entry point answers inline, exactly once, with a refusal nothing will retry, and sends nothing.
namespace CrowdyCppGameModelDeprecatedTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FString Code = CrowdyCppGameModelDeprecatedCode;
	const FString Message = CrowdyCppGameModelDeprecatedMessage;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppGameModelDeprecatedAnswersAtOnceTest,
	"CrowdySDK.CrowdyCpp.GameModelDeprecatedAnswersAtOnce", CrowdyCppGameModelDeprecatedTests::TestFlags)
bool FCrowdyCppGameModelDeprecatedAnswersAtOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppGameModelDeprecatedTests;

	TestFalse(TEXT("the code is not empty, which a caller would read as retryable"), Code.IsEmpty());
	for (const TCHAR* Retried : { TEXT("RATE_LIMITED"), TEXT("PLATFORM_BUSY"), TEXT("CONTAINER_TYPE_UNDEFINED"),
		TEXT("OBJECT_QUARANTINED"), TEXT("CONTAINER_TYPE_APP_SCOPED") })
	{
		TestFalse(FString::Printf(TEXT("the code is not %s"), Retried), Code.Equals(Retried, ESearchCase::CaseSensitive));
	}
	TestFalse(TEXT("the code is not a model refusal"), CrowdyCppIsModelRefusalCode(Code));

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	int32 InvokeCalls = 0;
	FCrowdyCppInvokeResult Invoke;
	const FCrowdyCppRequestHandle InvokeHandle = Client->InvokeFunction(1, TEXT("fn"), TEXT("c-1"), TEXT("s-1"),
		TEXT("{}"),
		[&InvokeCalls, &Invoke](FCrowdyCppInvokeResult Result)
		{
			++InvokeCalls;
			Invoke = MoveTemp(Result);
		});

	int32 ReadCalls = 0;
	FCrowdyCppContainerStateResult Read;
	Client->ReadContainerState(1, TEXT("c-1"),
		[&ReadCalls, &Read](FCrowdyCppContainerStateResult Result)
		{
			++ReadCalls;
			Read = MoveTemp(Result);
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

	int32 RunOpCalls = 0;
	FCrowdyCppJsonResult GameModelOp;
	Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
		[&RunOpCalls, &GameModelOp](FCrowdyCppJsonResult Result)
		{
			++RunOpCalls;
			GameModelOp = MoveTemp(Result);
		});

	int32 ComputeCalls = 0;
	FCrowdyCppJsonResult Compute;
	Client->RunOp(ECrowdyCppApiDomain::Compute, TEXT("AnyOperation"), MakeShared<FJsonObject>(),
		[&ComputeCalls, &Compute](FCrowdyCppJsonResult Result)
		{
			++ComputeCalls;
			Compute = MoveTemp(Result);
		});

	int32 SeedCalls = 0;
	FCrowdyCppStudioOpResult Seed;
	Client->SeedSchema(TEXT("{}"),
		[&SeedCalls, &Seed](FCrowdyCppStudioOpResult Result)
		{
			++SeedCalls;
			Seed = MoveTemp(Result);
		});

	int32 AutomationCalls = 0;
	FCrowdyCppStudioOpResult Automation;
	Client->UpsertAutomation(TEXT("{}"),
		[&AutomationCalls, &Automation](FCrowdyCppStudioOpResult Result)
		{
			++AutomationCalls;
			Automation = MoveTemp(Result);
		});

	int32 TriggerCalls = 0;
	FCrowdyCppStudioOpResult Trigger;
	Client->UpsertAutomationTrigger(TEXT("{}"),
		[&TriggerCalls, &Trigger](FCrowdyCppStudioOpResult Result)
		{
			++TriggerCalls;
			Trigger = MoveTemp(Result);
		});

	int32 RuntimeCalls = 0;
	FCrowdyCppJsonResult Runtime;
	Client->RunRuntimeOp(TEXT("GameModelEnsureContainer"), MakeShared<FJsonObject>(),
		[&RuntimeCalls, &Runtime](FCrowdyCppJsonResult Result)
		{
			++RuntimeCalls;
			Runtime = MoveTemp(Result);
		});

	// Naming a plane does not route around the refusal.
	int32 NamedPlaneCalls = 0;
	FCrowdyCppJsonResult NamedPlane;
	const FCrowdyCppRequestHandle NamedPlaneHandle = Client->RunOp(ECrowdyCppApiDomain::GameModel,
		TEXT("GameModelSchema"), MakeShared<FJsonObject>(),
		[&NamedPlaneCalls, &NamedPlane](FCrowdyCppJsonResult Result)
		{
			++NamedPlaneCalls;
			NamedPlane = MoveTemp(Result);
		},
		ECrowdyCppTokenPlane::Management);

	TArray<FString> SubscribeErrors;
	TArray<bool> SubscribeTerminal;
	int32 SubscribeOther = 0;
	FCrowdyCppSubscriptionCallbacks Callbacks;
	Callbacks.OnError = [&SubscribeErrors, &SubscribeTerminal](const FString& Error, bool bTerminal)
	{
		SubscribeErrors.Add(Error);
		SubscribeTerminal.Add(bTerminal);
	};
	Callbacks.OnNext = [&SubscribeOther](TSharedPtr<FJsonObject>) { ++SubscribeOther; };
	Callbacks.OnComplete = [&SubscribeOther]() { ++SubscribeOther; };
	const FCrowdyCppSubscriptionHandle Subscription = Client->SubscribeOperation(ECrowdyCppApiDomain::GameModel,
		TEXT("GameModelContainerChanged"), nullptr, MoveTemp(Callbacks));

	// Everything has answered before any Poll.
	TestEqual(TEXT("the invoke answered inline"), InvokeCalls, 1);
	TestEqual(TEXT("the container read answered inline"), ReadCalls, 1);
	TestEqual(TEXT("the container list answered inline"), ListCalls, 1);
	TestEqual(TEXT("the GameModel op answered inline"), RunOpCalls, 1);
	TestEqual(TEXT("the Compute op answered inline"), ComputeCalls, 1);
	TestEqual(TEXT("the seed answered inline"), SeedCalls, 1);
	TestEqual(TEXT("the automation upsert answered inline"), AutomationCalls, 1);
	TestEqual(TEXT("the trigger upsert answered inline"), TriggerCalls, 1);
	TestEqual(TEXT("the runtime op answered inline"), RuntimeCalls, 1);
	TestEqual(TEXT("the op with a named plane answered inline"), NamedPlaneCalls, 1);
	TestEqual(TEXT("the subscription refused inline"), SubscribeErrors.Num(), 1);

	TestFalse(TEXT("invoke: no transport"), Invoke.bTransportOk);
	TestFalse(TEXT("invoke: no success"), Invoke.bSuccess);
	TestFalse(TEXT("invoke: not retryable"), Invoke.bRetryable);
	TestTrue(TEXT("invoke: no blame"), Invoke.Blame.IsEmpty());
	TestFalse(TEXT("invoke: no wait named"), Invoke.RetryAfterMs.IsSet());
	TestEqual(TEXT("invoke: the deprecated code"), Invoke.FaultCode, Code);
	TestEqual(TEXT("invoke: the deprecated message"), Invoke.ErrorMessage, Message);
	TestTrue(TEXT("invoke: no quarantine"),
		Invoke.QuarantinedKind.IsEmpty() && Invoke.QuarantinedName.IsEmpty() && Invoke.QuarantineReason.IsEmpty());
	TestEqual(TEXT("invoke: no mutations"), Invoke.Mutations.Num(), 0);

	TestFalse(TEXT("read: failed"), Read.bOk);
	TestFalse(TEXT("read: no state"), Read.State.IsValid());
	TestEqual(TEXT("read: the deprecated message"), Read.ErrorMessage, Message);

	TestFalse(TEXT("list: failed"), bListOk);
	TestEqual(TEXT("list: no rows"), ListRows, 0);

	for (const FCrowdyCppJsonResult* Op : { &GameModelOp, &Compute, &Runtime, &NamedPlane })
	{
		TestFalse(TEXT("op: no transport"), Op->bTransportOk);
		TestFalse(TEXT("op: no data"), Op->Data.IsValid());
		TestFalse(TEXT("op: not retryable"), Op->bRetryable);
		TestTrue(TEXT("op: no blame"), Op->Blame.IsEmpty());
		TestFalse(TEXT("op: no wait named"), Op->RetryAfterMs.IsSet());
		TestEqual(TEXT("op: the deprecated code"), Op->ErrorCode, Code);
		TestEqual(TEXT("op: the deprecated message"), Op->ErrorMessage, Message);
	}

	for (const FCrowdyCppStudioOpResult* Op : { &Seed, &Automation, &Trigger })
	{
		TestFalse(TEXT("studio op: failed"), Op->bOk);
		TestEqual(TEXT("studio op: the deprecated message"), Op->ErrorMessage, Message);
	}

	TestFalse(TEXT("subscription: no handle"), Subscription.IsValid());
	if (SubscribeErrors.Num() == 1)
	{
		TestEqual(TEXT("subscription: the deprecated message"), SubscribeErrors[0], Message);
		TestTrue(TEXT("subscription: terminal"), SubscribeTerminal[0]);
	}

	// Nothing was sent, queued, parked for a retry or opened.
	FString Url;
	FString Authorization;
	TestFalse(TEXT("no request reached the transport"), Client->GetLastTestRequest(Url, Authorization));
	TestEqual(TEXT("nothing is pending"), Client->NumPendingRequests(), 0);
	TestEqual(TEXT("nothing waits to retry"), Client->NumTestWaitingRetries(), 0);
	TestEqual(TEXT("no socket was opened"), Client->NumTestWebSocketConnections(), 0);
	TestEqual(TEXT("no subscription is active"), Client->NumActiveSubscriptions(), 0);

	// An answered handle refers to nothing, so cancelling it neither succeeds nor delivers again.
	TestTrue(TEXT("the invoke was given a handle"), InvokeHandle.IsValid());
	TestTrue(TEXT("the op with a named plane was given a handle"), NamedPlaneHandle.IsValid());
	TestFalse(TEXT("cancelling the answered invoke does nothing"), Client->Cancel(InvokeHandle));
	TestFalse(TEXT("cancelling the answered op does nothing"), Client->Cancel(NamedPlaneHandle));
	TestEqual(TEXT("the cancel did not answer the invoke again"), InvokeCalls, 1);
	TestEqual(TEXT("the cancel did not answer the op again"), NamedPlaneCalls, 1);

	// Each refusal is counted as one failed call under its operation, for the deprecated reason.
	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	for (const TCHAR* Operation : { TEXT("GameModelInvoke"), TEXT("GameModelContainerState"),
		TEXT("GameModelContainers"), TEXT("GameModelSession"), TEXT("AnyOperation"), TEXT("GameModelSeed"),
		TEXT("GameModelUpsertAutomation"), TEXT("GameModelUpsertAutomationTrigger"), TEXT("GameModelEnsureContainer"),
		TEXT("GameModelSchema") })
	{
		const FCrowdyCppOpStats* Op = Ops.FindByPredicate(
			[Operation](const FCrowdyCppOpStats& Stats) { return Stats.Operation == Operation; });
		if (!TestNotNull(FString::Printf(TEXT("%s: recorded"), Operation), Op))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: one call"), Operation), Op->Calls, 1);
		TestEqual(FString::Printf(TEXT("%s: one failure"), Operation), Op->Failures, 1);
		TestEqual(FString::Printf(TEXT("%s: no retries"), Operation), Op->Retries, 0);
		if (TestEqual(FString::Printf(TEXT("%s: one failure reason"), Operation), Op->FailureReasons.Num(), 1))
		{
			TestTrue(FString::Printf(TEXT("%s: the reason is the deprecated code"), Operation),
				Op->FailureReasons[0].Key.Equals(Code, ESearchCase::CaseSensitive));
			TestEqual(FString::Printf(TEXT("%s: counted once"), Operation), Op->FailureReasons[0].Value, 1);
		}
	}
	TestEqual(TEXT("only the refused calls are recorded"), Ops.Num(), 10);
	TestEqual(TEXT("the transport saw no response"), Transport.Responses, 0);
	TestEqual(TEXT("the transport never held a request"), Transport.PeakInFlight, 0);

	// And none of them is ever answered again.
	for (int32 Pump = 0; Pump < 3; ++Pump)
	{
		Client->Poll();
	}
	TestEqual(TEXT("the invoke answered once"), InvokeCalls, 1);
	TestEqual(TEXT("the container read answered once"), ReadCalls, 1);
	TestEqual(TEXT("the container list answered once"), ListCalls, 1);
	TestEqual(TEXT("the GameModel op answered once"), RunOpCalls, 1);
	TestEqual(TEXT("the Compute op answered once"), ComputeCalls, 1);
	TestEqual(TEXT("the seed answered once"), SeedCalls, 1);
	TestEqual(TEXT("the automation upsert answered once"), AutomationCalls, 1);
	TestEqual(TEXT("the trigger upsert answered once"), TriggerCalls, 1);
	TestEqual(TEXT("the runtime op answered once"), RuntimeCalls, 1);
	TestEqual(TEXT("the op with a named plane answered once"), NamedPlaneCalls, 1);
	TestEqual(TEXT("the subscription refused once"), SubscribeErrors.Num(), 1);
	TestEqual(TEXT("the subscription delivered nothing else"), SubscribeOther, 0);
	TestFalse(TEXT("still nothing reached the transport"), Client->GetLastTestRequest(Url, Authorization));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
