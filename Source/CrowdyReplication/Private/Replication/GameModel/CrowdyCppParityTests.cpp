#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppBridge.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Parity gate for the CrowdyCPP client: the same canned gameModelContainerState
// envelope is routed through the CrowdyCPP async client and through
// FCrowdyGameApiCodec, and their decoded results are compared. This proves both
// paths extract identical wire fields, one call end to end, with no network.
namespace
{
	constexpr EAutomationTestFlags CrowdyCppParityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Drives the CrowdyCPP path synchronously: the canned transport queues the
	// outcome, Poll() drains it, so OnDone has run by the time this returns.
	FCrowdyCppContainerStateResult ReadStateViaCrowdyCpp(const FString& Body, int32 HttpStatus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, HttpStatus);
		FCrowdyCppContainerStateResult Captured;
		bool bCalled = false;
		if (Client.IsValid())
		{
			Client->ReadContainerState(1, TEXT("c-1"),
				[&Captured, &bCalled](FCrowdyCppContainerStateResult Result)
				{
					Captured = MoveTemp(Result);
					bCalled = true;
				});
			Client->Poll();
		}
		check(bCalled);
		return Captured;
	}

	// Drives the CrowdyCPP gameModelInvoke path synchronously (canned transport + Poll()).
	FCrowdyCppInvokeResult InvokeViaCrowdyCpp(const FString& Body, int32 HttpStatus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, HttpStatus);
		FCrowdyCppInvokeResult Captured;
		bool bCalled = false;
		if (Client.IsValid())
		{
			Client->InvokeFunction(1, TEXT("add_score"), TEXT("c-1"), FString(), TEXT("{}"),
				[&Captured, &bCalled](FCrowdyCppInvokeResult Result)
				{
					Captured = MoveTemp(Result);
					bCalled = true;
				});
			Client->Poll();
		}
		check(bCalled);
		return Captured;
	}

	struct FContainersCppResult
	{
		bool bOk = false;
		TArray<TSharedPtr<FJsonObject>> Containers;
	};

	// Drives the CrowdyCPP gameModelContainers path synchronously (canned transport + Poll()).
	FContainersCppResult ListContainersViaCrowdyCpp(const FString& Body, int32 HttpStatus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, HttpStatus);
		FContainersCppResult Captured;
		bool bCalled = false;
		if (Client.IsValid())
		{
			Client->ListContainers(1, FString(), FString(),
				[&Captured, &bCalled](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
				{
					Captured.bOk = bOk;
					Captured.Containers = MoveTemp(Containers);
					bCalled = true;
				});
			Client->Poll();
		}
		check(bCalled);
		return Captured;
	}

	// Drives the generic CrowdyCPP runtime-op path synchronously (canned transport + Poll()). The variables are
	// irrelevant to the canned transport (it returns Body for any request), so an empty object is passed.
	FCrowdyCppJsonResult RunRuntimeOpViaCrowdyCpp(const FString& OperationName, const FString& Body, int32 HttpStatus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, HttpStatus);
		FCrowdyCppJsonResult Captured;
		bool bCalled = false;
		if (Client.IsValid())
		{
			Client->RunRuntimeOp(OperationName, MakeShared<FJsonObject>(),
				[&Captured, &bCalled](FCrowdyCppJsonResult Result)
				{
					Captured = MoveTemp(Result);
					bCalled = true;
				});
			Client->Poll();
		}
		check(bCalled);
		return Captured;
	}

	// Wraps the bridge's raw `data` object back into a { "data": ... } envelope, mirroring the subsystem's
	// WrapCppDataEnvelope, so the same FCrowdyGameApiCodec ParseXEnvelope decodes both paths.
	TSharedPtr<FJsonObject> WrapData(const TSharedPtr<FJsonObject>& DataObject)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		if (DataObject.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), DataObject);
		}
		return Envelope;
	}
}

// Success path: a committed container-state envelope decodes to the same
// properties through both clients.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppContainerStateParityTest,
	"CrowdySDK.CrowdyCpp.ContainerStateParity", CrowdyCppParityTestFlags)
bool FCrowdyCppContainerStateParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelContainerState\":{"
		"\"containerId\":\"c-1\",\"appId\":\"1\",\"sessionId\":null,"
		"\"typeName\":\"Player\",\"displayName\":\"Aria\",\"ownerUserId\":\"42\","
		"\"propertiesJson\":\"{\\\"hp\\\":100,\\\"name\\\":\\\"Aria\\\"}\"}}}");

	// Path A: the CrowdyCPP async client.
	const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 200);
	if (!TestTrue(TEXT("CrowdyCPP read ok"), CppResult.bOk))
	{
		return false;
	}
	if (!TestNotNull(TEXT("CrowdyCPP decoded state"), CppResult.State.Get()))
	{
		return false;
	}

	// Path B: FCrowdyGameApiCodec, fed the same envelope.
	TSharedPtr<FJsonObject> HandState;
	const bool bHandOk = FCrowdyGameApiCodec::ParseContainerStateEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandState);
	if (!TestTrue(TEXT("hand-rolled transport ok"), bHandOk))
	{
		return false;
	}
	if (!TestNotNull(TEXT("hand-rolled decoded state"), HandState.Get()))
	{
		return false;
	}

	// Field-for-field parity of the decoded state.
	double CppHp = 0.0;
	double HandHp = 0.0;
	TestTrue(TEXT("CrowdyCPP hp present"), CppResult.State->TryGetNumberField(TEXT("hp"), CppHp));
	TestTrue(TEXT("hand-rolled hp present"), HandState->TryGetNumberField(TEXT("hp"), HandHp));
	TestEqual(TEXT("hp matches across clients"), CppHp, HandHp);
	TestEqual(TEXT("hp is 100"), CppHp, 100.0);

	FString CppName;
	FString HandName;
	TestTrue(TEXT("CrowdyCPP name present"), CppResult.State->TryGetStringField(TEXT("name"), CppName));
	TestTrue(TEXT("hand-rolled name present"), HandState->TryGetStringField(TEXT("name"), HandName));
	TestEqual(TEXT("name matches across clients"), CppName, HandName);
	TestEqual(TEXT("name is Aria"), CppName, FString(TEXT("Aria")));

	return true;
}

// A reachable-but-absent container is where the two clients could most easily
// diverge: the CrowdyCPP path sees a successful GraphQL response, FCrowdyGameApiCodec
// sees a null field. Both must report "no state" (bOk == false, State null) for
// a null container and for a propertiesJson that is valid JSON but not an object.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppContainerStateAbsentParityTest,
	"CrowdySDK.CrowdyCpp.ContainerStateAbsentParity", CrowdyCppParityTestFlags)
bool FCrowdyCppContainerStateAbsentParityTest::RunTest(const FString& Parameters)
{
	// Case 1: the server returns a null container.
	{
		const FString Body = TEXT("{\"data\":{\"gameModelContainerState\":null}}");

		const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 200);
		TestFalse(TEXT("CrowdyCPP: null container is not ok"), CppResult.bOk);
		TestNull(TEXT("CrowdyCPP: null container has no state"), CppResult.State.Get());

		TSharedPtr<FJsonObject> HandState;
		const bool bHandOk = FCrowdyGameApiCodec::ParseContainerStateEnvelope(
			ParseObject(Body), true, TArray<FString>(), HandState);
		TestFalse(TEXT("hand-rolled: null container is not ok"), bHandOk);
		TestEqual(TEXT("null-container ok flag matches"), CppResult.bOk, bHandOk);
	}

	// Case 2: propertiesJson is valid JSON but not an object.
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelContainerState\":{\"containerId\":\"c-1\","
			"\"propertiesJson\":\"42\"}}}");

		const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 200);
		TestFalse(TEXT("CrowdyCPP: non-object state is not ok"), CppResult.bOk);

		TSharedPtr<FJsonObject> HandState;
		const bool bHandOk = FCrowdyGameApiCodec::ParseContainerStateEnvelope(
			ParseObject(Body), true, TArray<FString>(), HandState);
		TestFalse(TEXT("hand-rolled: non-object state is not ok"), bHandOk);
		TestEqual(TEXT("non-object ok flag matches"), CppResult.bOk, bHandOk);
	}

	return true;
}

// GraphQL errors[] must read as a failure, not an empty success, so a rejected
// read is never mistaken for "the container has no state". This drives the real
// CrowdyCPP interpret() path; the FCrowdyGameApiCodec side is a parser-level comparison
// (its GraphQL client extracts errors[] into the TransportErrors passed here).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppContainerStateGraphQLErrorTest,
	"CrowdySDK.CrowdyCpp.ContainerStateGraphQLError", CrowdyCppParityTestFlags)
bool FCrowdyCppContainerStateGraphQLErrorTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("{\"errors\":[{\"message\":\"not authorized\"}]}");

	const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 200);
	TestFalse(TEXT("CrowdyCPP rejects a GraphQL-error envelope"), CppResult.bOk);
	TestTrue(TEXT("CrowdyCPP surfaces the error message"), CppResult.ErrorMessage.Contains(TEXT("not authorized")));

	// FCrowdyGameApiCodec rejects the same case (its GraphQL client would have
	// pulled errors[] out into TransportErrors).
	TSharedPtr<FJsonObject> HandState;
	const bool bHandOk = FCrowdyGameApiCodec::ParseContainerStateEnvelope(
		ParseObject(Body), true, TArray<FString>{TEXT("not authorized")}, HandState);
	TestFalse(TEXT("hand-rolled rejects a GraphQL-error envelope"), bHandOk);

	return true;
}

// A non-2xx HTTP response is a transport failure through the async path too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppContainerStateHttpErrorTest,
	"CrowdySDK.CrowdyCpp.ContainerStateHttpError", CrowdyCppParityTestFlags)
bool FCrowdyCppContainerStateHttpErrorTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("{\"message\":\"internal error\"}");

	const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 500);
	TestFalse(TEXT("CrowdyCPP rejects a 5xx response"), CppResult.bOk);

	return true;
}

// The untrusted propertiesJson is bounded before the recursive UE deserializer
// touches it, so a forged deeply-nested payload is rejected rather than parsed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppContainerStateNestingGuardTest,
	"CrowdySDK.CrowdyCpp.ContainerStateNestingGuard", CrowdyCppParityTestFlags)
bool FCrowdyCppContainerStateNestingGuardTest::RunTest(const FString& Parameters)
{
	// A nested array 200 deep, carried as the propertiesJson string value. The
	// brackets sit inside a JSON string so the envelope itself parses; the guard
	// rejects the value before it is deserialized into a DOM.
	FString Deep;
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("[");
	}
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("]");
	}
	const FString Body = FString::Printf(TEXT(
		"{\"data\":{\"gameModelContainerState\":{\"containerId\":\"c-1\","
		"\"propertiesJson\":\"%s\"}}}"), *Deep);

	const FCrowdyCppContainerStateResult CppResult = ReadStateViaCrowdyCpp(Body, 200);
	TestFalse(TEXT("over-deep propertiesJson is rejected"), CppResult.bOk);

	return true;
}

// gameModelInvoke parity: a committed invoke decodes to the same result (success, returnValue, mutations)
// through both clients, and the transport-vs-logic discrimination is preserved.
//
// It compares the DECODED RESULT ONLY, and deliberately not the fault attribution. FaultCode, Blame, bRetryable and
// RetryAfterMs all come from errors[].extensions, which ParseInvokeEnvelope is never given: it receives the thrown
// channel pre-flattened into message strings. Comparing those fields here would assert that both parsers agree on
// leaving them empty, which is true and worthless. They are covered against the bridge alone, on canned refusals, in
// CrowdySDK.CrowdyCpp.InvokeRateLimitRefusal - so a regression that stopped the bridge reading extensions is caught
// there and NOT here. Case 3 below is a thrown error and checks only what both sides genuinely see.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppInvokeParityTest,
	"CrowdySDK.CrowdyCpp.InvokeParity", CrowdyCppParityTestFlags)
bool FCrowdyCppInvokeParityTest::RunTest(const FString& Parameters)
{
	// Case 1: a committed invoke with an applied mutation.
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelInvoke\":{"
			"\"eventId\":\"e-1\",\"functionName\":\"add_score\",\"success\":true,"
			"\"returnValueJson\":\"42\",\"errorMessage\":\"\","
			"\"mutationsApplied\":[{\"key\":\"score\",\"oldValueJson\":\"10\",\"newValueJson\":\"42\"}]}}}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		const FCrowdyInvokeResult HandResult =
			FCrowdyGameApiCodec::ParseInvokeEnvelope(ParseObject(Body), true, TArray<FString>());

		TestTrue(TEXT("CrowdyCPP invoke transport-ok"), CppResult.bTransportOk);
		TestEqual(TEXT("transport-ok matches"), CppResult.bTransportOk, HandResult.bTransportOk);
		TestEqual(TEXT("success matches"), CppResult.bSuccess, HandResult.bSuccess);
		TestTrue(TEXT("success is true"), CppResult.bSuccess);
		TestEqual(TEXT("returnValueJson matches"), CppResult.ReturnValueJson, HandResult.ReturnValueJson);
		TestEqual(TEXT("returnValueJson is 42"), CppResult.ReturnValueJson, FString(TEXT("42")));
		TestEqual(TEXT("mutation count matches"), CppResult.Mutations.Num(), HandResult.Mutations.Num());
		TestEqual(TEXT("mutation count is 1"), CppResult.Mutations.Num(), 1);
		if (CppResult.Mutations.Num() == 1 && HandResult.Mutations.Num() == 1)
		{
			TestEqual(TEXT("mutation key matches"), CppResult.Mutations[0].Key, HandResult.Mutations[0].Key);
			TestEqual(TEXT("mutation key is score"), CppResult.Mutations[0].Key, FString(TEXT("score")));
			TestEqual(TEXT("mutation oldValueJson matches"),
				CppResult.Mutations[0].OldValueJson, HandResult.Mutations[0].OldValueJson);
			TestEqual(TEXT("mutation oldValueJson is 10"), CppResult.Mutations[0].OldValueJson, FString(TEXT("10")));
			TestEqual(TEXT("mutation newValueJson matches"),
				CppResult.Mutations[0].NewValueJson, HandResult.Mutations[0].NewValueJson);
			TestEqual(TEXT("mutation newValueJson is 42"), CppResult.Mutations[0].NewValueJson, FString(TEXT("42")));
		}
	}

	// Case 2: a rolled-back invoke (the function ran but its authority/logic check failed). This is where the
	// transport-vs-logic split matters: bTransportOk stays true, bSuccess is false, and the reason is carried.
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelInvoke\":{"
			"\"eventId\":\"e-2\",\"functionName\":\"add_score\",\"success\":false,"
			"\"returnValueJson\":\"\",\"errorMessage\":\"insufficient funds\","
			"\"mutationsApplied\":[]}}}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		const FCrowdyInvokeResult HandResult =
			FCrowdyGameApiCodec::ParseInvokeEnvelope(ParseObject(Body), true, TArray<FString>());

		TestTrue(TEXT("rolled-back invoke is still transport-ok"), CppResult.bTransportOk);
		TestEqual(TEXT("rolled-back transport-ok matches"), CppResult.bTransportOk, HandResult.bTransportOk);
		TestFalse(TEXT("rolled-back invoke is not success"), CppResult.bSuccess);
		TestEqual(TEXT("rolled-back success matches"), CppResult.bSuccess, HandResult.bSuccess);
		TestEqual(TEXT("rolled-back error message matches"), CppResult.ErrorMessage, HandResult.ErrorMessage);
		TestEqual(TEXT("rolled-back error message is surfaced"),
			CppResult.ErrorMessage, FString(TEXT("insufficient funds")));
		TestEqual(TEXT("rolled-back has no mutations"), CppResult.Mutations.Num(), 0);
	}

	// Case 3: a GraphQL error (e.g. authorization) is a transport failure, NOT a rolled-back invoke.
	{
		const FString Body = TEXT("{\"errors\":[{\"message\":\"not authorized\"}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		const FCrowdyInvokeResult HandResult = FCrowdyGameApiCodec::ParseInvokeEnvelope(
			ParseObject(Body), true, TArray<FString>{TEXT("not authorized")});

		TestFalse(TEXT("CrowdyCPP: GraphQL error is not transport-ok"), CppResult.bTransportOk);
		TestEqual(TEXT("GraphQL-error transport-ok matches"), CppResult.bTransportOk, HandResult.bTransportOk);
		TestTrue(TEXT("CrowdyCPP surfaces the GraphQL error"),
			CppResult.ErrorMessage.Contains(TEXT("not authorized")));
	}

	// Case 4: a non-2xx HTTP response is a transport failure through the async path too.
	{
		const FString Body = TEXT("{\"message\":\"internal error\"}");
		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 500);
		TestFalse(TEXT("CrowdyCPP: a 5xx invoke is not transport-ok"), CppResult.bTransportOk);
	}

	return true;
}

// The rate-limit refusal, which is the whole reason the bridge reads extensions at all. The server throws it rather
// than returning it in band (PlayerFaultInfo carries no timing field, so an in-band fault could not express the wait),
// which means it arrives on the SAME shape as a network failure: not transport-ok, no gameModelInvoke object. What
// tells the two apart is the attribution, and that is what the retry gate downstream depends on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppInvokeRateLimitRefusalTest,
	"CrowdySDK.CrowdyCpp.InvokeRateLimitRefusal", CrowdyCppParityTestFlags)
bool FCrowdyCppInvokeRateLimitRefusalTest::RunTest(const FString& Parameters)
{
	// A refusal carrying the wait: every extension field reaches the result, including the one that used to have
	// nowhere to land. Dropping the retryAfterMs read turns only the last two assertions red.
	{
		const FString Body = TEXT(
			"{\"errors\":[{\"message\":\"Too many calls\",\"extensions\":"
			"{\"code\":\"RATE_LIMITED\",\"blame\":\"BUDGET\",\"retryable\":true,\"retryAfterMs\":4200}}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);

		TestFalse(TEXT("a thrown refusal is not transport-ok"), CppResult.bTransportOk);
		TestEqual(TEXT("the server's fault code is carried"), CppResult.FaultCode, FString(TEXT("RATE_LIMITED")));
		TestEqual(TEXT("the server's blame is carried"), CppResult.Blame, FString(TEXT("BUDGET")));
		TestTrue(TEXT("an attributed refusal reports retryable"), CppResult.bRetryable);
		TestTrue(TEXT("the wait is read from extensions"), CppResult.RetryAfterMs.IsSet());
		TestEqual(TEXT("the wait is the value the server sent"), CppResult.RetryAfterMs.Get(0), static_cast<int64>(4200));
	}

	// Zero is a real instruction ("the window has already rolled"), not the absence of one. Reading the field into an
	// int defaulting to 0 would make these two cases indistinguishable, which is the single property the optional
	// exists to preserve.
	{
		const FString Body = TEXT(
			"{\"errors\":[{\"message\":\"now\",\"extensions\":"
			"{\"code\":\"RATE_LIMITED\",\"blame\":\"BUDGET\",\"retryAfterMs\":0}}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		TestTrue(TEXT("an explicit zero is a wait the server named"), CppResult.RetryAfterMs.IsSet());
		TestEqual(TEXT("and its value is zero"), CppResult.RetryAfterMs.Get(-1), static_cast<int64>(0));
	}

	// A refusal that names no wait leaves the field unset, so the caller falls back to its own backoff rather than
	// re-sending immediately into the same limiter.
	{
		const FString Body = TEXT(
			"{\"errors\":[{\"message\":\"nope\",\"extensions\":{\"code\":\"FORBIDDEN\",\"blame\":\"AUTHOR\"}}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		TestEqual(TEXT("the fault code is still carried"), CppResult.FaultCode, FString(TEXT("FORBIDDEN")));
		TestFalse(TEXT("an absent retryAfterMs stays unset"), CppResult.RetryAfterMs.IsSet());
	}

	// A network failure is the case the retry gate downstream must never mistake for a refusal. It has the same
	// bTransportOk == false as the refusals above, so the attribution fields staying EMPTY is the only thing that
	// distinguishes them, and it is what makes dropping the transport check in IsBudgetRefusal safe.
	{
		const FString Body = TEXT("{\"message\":\"internal error\"}");
		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 500);

		TestFalse(TEXT("a 5xx is not transport-ok either"), CppResult.bTransportOk);
		TestTrue(TEXT("a failure the server never attributed carries no fault code"), CppResult.FaultCode.IsEmpty());
		TestTrue(TEXT("nor a blame"), CppResult.Blame.IsEmpty());
		TestFalse(TEXT("nor a wait"), CppResult.RetryAfterMs.IsSet());
	}

	return true;
}

// The quarantine refusal, which is the one a shipped client sees instead of a lint report. It is the reason the
// bridge reads these three extensions at all: the message is a whole sentence, but only quarantineReason names the
// finding to fix, and the code on this path is NOT the one you would guess.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppInvokeQuarantineRefusalTest,
	"CrowdySDK.CrowdyCpp.InvokeQuarantineRefusal", CrowdyCppParityTestFlags)
bool FCrowdyCppInvokeQuarantineRefusalTest::RunTest(const FString& Parameters)
{
	// The gameModelInvoke shape, which is the one a player takes. The server rebuilds the error at the user-code
	// boundary, so the code arrives as USER_CODE_ERROR rather than OBJECT_QUARANTINED while the three quarantine
	// fields survive intact. A reader that gated on the code would find nothing here.
	{
		const FString Body = TEXT(
			"{\"errors\":[{\"message\":\"The function 'notify_round_start' is quarantined and will not run.\","
			"\"extensions\":{\"code\":\"USER_CODE_ERROR\",\"blame\":\"AUTHOR\",\"retryable\":false,"
			"\"quarantinedKind\":\"function\",\"quarantinedName\":\"notify_round_start\","
			"\"quarantineReason\":\"notification_channel_foreign: channel 123 belongs to app 456\"}}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);

		TestEqual(TEXT("the rebuilt code is what actually arrives"), CppResult.FaultCode,
			FString(TEXT("USER_CODE_ERROR")));
		TestEqual(TEXT("the quarantined kind is carried"), CppResult.QuarantinedKind, FString(TEXT("function")));
		TestEqual(TEXT("the quarantined object names itself"), CppResult.QuarantinedName,
			FString(TEXT("notify_round_start")));
		TestTrue(TEXT("the reason names the finding to fix"),
			CppResult.QuarantineReason.Contains(TEXT("notification_channel_foreign")));
		TestFalse(TEXT("an author fault is not retryable"), CppResult.bRetryable);
	}

	// A refusal that is nothing to do with the model leaves all three empty, so a non-empty reason is a signal a
	// caller can branch on rather than something every failure carries.
	{
		const FString Body = TEXT(
			"{\"errors\":[{\"message\":\"Too many calls\",\"extensions\":"
			"{\"code\":\"RATE_LIMITED\",\"blame\":\"BUDGET\",\"retryable\":true}}]}");

		const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
		TestTrue(TEXT("a budget refusal names no quarantined kind"), CppResult.QuarantinedKind.IsEmpty());
		TestTrue(TEXT("nor a quarantined object"), CppResult.QuarantinedName.IsEmpty());
		TestTrue(TEXT("nor a reason"), CppResult.QuarantineReason.IsEmpty());
	}

	return true;
}

// Which error codes mean "your game model is wrong". The list is CrowdyCPP's, so this pins that the bridge is
// asking the library rather than carrying a copy that goes stale as the platform adds codes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppModelRefusalCodeTest,
	"CrowdySDK.CrowdyCpp.ModelRefusalCode", CrowdyCppParityTestFlags)
bool FCrowdyCppModelRefusalCodeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a container bound to an undefined type is a model refusal"),
		CrowdyCppIsModelRefusalCode(TEXT("CONTAINER_TYPE_UNDEFINED")));
	TestTrue(TEXT("a quarantined object is a model refusal"),
		CrowdyCppIsModelRefusalCode(TEXT("OBJECT_QUARANTINED")));

	// The distinction the predicate exists to make: neither of these is the developer's model being wrong, and
	// reporting them as such would send someone to lint an app that is fine.
	TestFalse(TEXT("a rate limit is not a model refusal"), CrowdyCppIsModelRefusalCode(TEXT("RATE_LIMITED")));
	TestFalse(TEXT("a permissions failure is not a model refusal"), CrowdyCppIsModelRefusalCode(TEXT("FORBIDDEN")));
	// A failure the server never attributed arrives with an empty code, which must not match anything.
	TestFalse(TEXT("an unattributed failure is not a model refusal"), CrowdyCppIsModelRefusalCode(FString()));
	// Matched exactly, not by prefix: a longer code that merely starts the same is a different code.
	TestFalse(TEXT("a code is matched whole"), CrowdyCppIsModelRefusalCode(TEXT("OBJECT_QUARANTINED_LATER")));

	return true;
}

// gameModelContainers parity: a list decodes to the same raw container objects through both clients, an empty
// list is a success, and a GraphQL error is a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppListContainersParityTest,
	"CrowdySDK.CrowdyCpp.ListContainersParity", CrowdyCppParityTestFlags)
bool FCrowdyCppListContainersParityTest::RunTest(const FString& Parameters)
{
	// Case 1: two containers decode to the same objects (containerId, ownerUserId, and the raw metadataJson).
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelContainers\":["
			"{\"containerId\":\"c-1\",\"appId\":\"1\",\"typeName\":\"Player\",\"ownerUserId\":\"42\","
			"\"metadataJson\":\"{\\\"netid\\\":\\\"abc\\\"}\"},"
			"{\"containerId\":\"c-2\",\"appId\":\"1\",\"typeName\":\"Item\",\"ownerUserId\":\"7\","
			"\"metadataJson\":\"{}\"}]}}");

		const FContainersCppResult CppResult = ListContainersViaCrowdyCpp(Body, 200);

		TArray<TSharedPtr<FJsonObject>> HandContainers;
		const bool bHandOk = FCrowdyGameApiCodec::ParseContainersEnvelope(
			ParseObject(Body), true, TArray<FString>(), HandContainers);

		TestTrue(TEXT("CrowdyCPP list ok"), CppResult.bOk);
		TestEqual(TEXT("list ok matches"), CppResult.bOk, bHandOk);
		if (!TestEqual(TEXT("container count matches"), CppResult.Containers.Num(), HandContainers.Num()))
		{
			return false;
		}
		TestEqual(TEXT("container count is 2"), CppResult.Containers.Num(), 2);
		if (CppResult.Containers.Num() == 2)
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FString CppId;
				FString HandId;
				CppResult.Containers[Index]->TryGetStringField(TEXT("containerId"), CppId);
				HandContainers[Index]->TryGetStringField(TEXT("containerId"), HandId);
				TestEqual(TEXT("containerId matches"), CppId, HandId);
				TestEqual(TEXT("containerId is expected"), CppId,
					Index == 0 ? FString(TEXT("c-1")) : FString(TEXT("c-2")));

				FString CppMeta;
				FString HandMeta;
				CppResult.Containers[Index]->TryGetStringField(TEXT("metadataJson"), CppMeta);
				HandContainers[Index]->TryGetStringField(TEXT("metadataJson"), HandMeta);
				TestEqual(TEXT("metadataJson carried verbatim"), CppMeta, HandMeta);
				TestEqual(TEXT("metadataJson is expected"), CppMeta,
					Index == 0 ? FString(TEXT("{\"netid\":\"abc\"}")) : FString(TEXT("{}")));
			}
			FString FirstId;
			CppResult.Containers[0]->TryGetStringField(TEXT("containerId"), FirstId);
			TestEqual(TEXT("first container is c-1"), FirstId, FString(TEXT("c-1")));
		}
	}

	// Case 2: an empty list is a success with zero containers (not a failure).
	{
		const FString Body = TEXT("{\"data\":{\"gameModelContainers\":[]}}");

		const FContainersCppResult CppResult = ListContainersViaCrowdyCpp(Body, 200);
		TestTrue(TEXT("CrowdyCPP: empty list is ok"), CppResult.bOk);
		TestEqual(TEXT("empty list has no containers"), CppResult.Containers.Num(), 0);

		TArray<TSharedPtr<FJsonObject>> HandContainers;
		const bool bHandOk = FCrowdyGameApiCodec::ParseContainersEnvelope(
			ParseObject(Body), true, TArray<FString>(), HandContainers);
		TestEqual(TEXT("empty-list ok matches"), CppResult.bOk, bHandOk);
	}

	// Case 3: a GraphQL error is a failure, not an empty list.
	{
		const FString Body = TEXT("{\"errors\":[{\"message\":\"nope\"}]}");
		const FContainersCppResult CppResult = ListContainersViaCrowdyCpp(Body, 200);
		TestFalse(TEXT("CrowdyCPP: GraphQL-error list is not ok"), CppResult.bOk);
	}

	return true;
}

// A forged deeply-nested container object is rejected before the recursive UE re-parse: the dumped array is
// nesting-guarded, so an over-deep element fails the whole list closed rather than building an overflowing DOM.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppListContainersNestingGuardTest,
	"CrowdySDK.CrowdyCpp.ListContainersNestingGuard", CrowdyCppParityTestFlags)
bool FCrowdyCppListContainersNestingGuardTest::RunTest(const FString& Parameters)
{
	// A container object with a genuinely nested (200-deep object) field. yyjson parses it iteratively, but the
	// dumped array would build a 200-deep recursive DOM in UE, so the guard rejects the list.
	FString Deep;
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("{\"a\":");
	}
	Deep += TEXT("1");
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("}");
	}
	const FString Body = FString::Printf(TEXT(
		"{\"data\":{\"gameModelContainers\":[{\"containerId\":\"c-1\",\"nested\":%s}]}}"), *Deep);

	const FContainersCppResult CppResult = ListContainersViaCrowdyCpp(Body, 200);
	TestFalse(TEXT("over-deep container list is rejected"), CppResult.bOk);
	TestEqual(TEXT("rejected list has no containers"), CppResult.Containers.Num(), 0);
	return true;
}

// A mutationsApplied array with a non-object element (a schema violation). Both clients now run the same UE
// extraction (dump -> parse -> the same TryGet*/AsObject handling), so they decode it IDENTICALLY. UE's
// AsObject() logs a type error and yields an empty object for the stray number, so both paths carry it as an
// empty mutation - the lock is that the two paths AGREE element-for-element, not that either skips it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppInvokeMutationParityMalformedTest,
	"CrowdySDK.CrowdyCpp.InvokeMutationParityMalformed", CrowdyCppParityTestFlags)
bool FCrowdyCppInvokeMutationParityMalformedTest::RunTest(const FString& Parameters)
{
	// Both extraction paths call FJsonValue::AsObject() on the stray number, which logs this type error; it is
	// expected and must not fail the test.
	AddExpectedError(TEXT("used as a 'Object'"), EAutomationExpectedErrorFlags::Contains, 0);

	const FString Body = TEXT(
		"{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"\",\"errorMessage\":\"\","
		"\"mutationsApplied\":[42,{\"key\":\"score\",\"oldValueJson\":\"10\",\"newValueJson\":\"42\"}]}}}");

	const FCrowdyCppInvokeResult CppResult = InvokeViaCrowdyCpp(Body, 200);
	const FCrowdyInvokeResult HandResult =
		FCrowdyGameApiCodec::ParseInvokeEnvelope(ParseObject(Body), true, TArray<FString>());

	TestTrue(TEXT("malformed-mutation transport-ok"), CppResult.bTransportOk);
	if (!TestEqual(TEXT("mutation count matches hand-rolled"),
		CppResult.Mutations.Num(), HandResult.Mutations.Num()))
	{
		return false;
	}
	TestEqual(TEXT("mutation count is 2"), CppResult.Mutations.Num(), 2);
	for (int32 Index = 0; Index < CppResult.Mutations.Num(); ++Index)
	{
		// Element 0 is the stray number: AsObject() yields an empty object, so every field reads as empty.
		// Element 1 is the well-formed mutation.
		const FString ExpectedKey = Index == 0 ? FString() : FString(TEXT("score"));
		const FString ExpectedOldValueJson = Index == 0 ? FString() : FString(TEXT("10"));
		const FString ExpectedNewValueJson = Index == 0 ? FString() : FString(TEXT("42"));

		TestEqual(TEXT("mutation key matches"), CppResult.Mutations[Index].Key, HandResult.Mutations[Index].Key);
		TestEqual(TEXT("mutation key is expected"), CppResult.Mutations[Index].Key, ExpectedKey);
		TestEqual(TEXT("mutation oldValueJson matches"),
			CppResult.Mutations[Index].OldValueJson, HandResult.Mutations[Index].OldValueJson);
		TestEqual(TEXT("mutation oldValueJson is expected"),
			CppResult.Mutations[Index].OldValueJson, ExpectedOldValueJson);
		TestEqual(TEXT("mutation newValueJson matches"),
			CppResult.Mutations[Index].NewValueJson, HandResult.Mutations[Index].NewValueJson);
		TestEqual(TEXT("mutation newValueJson is expected"),
			CppResult.Mutations[Index].NewValueJson, ExpectedNewValueJson);
	}
	return true;
}

// Session parity: a createSession response (with a turn holder) decodes to the same GmSession fields whether the
// bridge's data object is re-wrapped and parsed, or FCrowdyGameApiCodec reads the full envelope. This is the
// generic RunRuntimeOp seam: both paths run the identical FCrowdyGameApiCodec::ParseSessionEnvelope, so the test
// locks that the CrowdyCPP transport+dump+reparse yields a data object the parser decodes identically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpSessionParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpSessionParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpSessionParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelCreateSession\":{"
		"\"sessionId\":\"s-1\",\"appId\":\"1\",\"name\":\"Match\",\"status\":\"active\","
		"\"createdByUserId\":\"42\",\"currentTurnUserId\":\"7\",\"metadataJson\":\"{}\"}}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelCreateSession"), Body, 200);
	if (!TestTrue(TEXT("CrowdyCPP runtime op transport-ok"), CppResult.bTransportOk))
	{
		return false;
	}

	FCrowdyGameSessionData CppSession;
	const bool bCppOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), TEXT("gameModelCreateSession"), CppSession);

	FCrowdyGameSessionData HandSession;
	const bool bHandOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
		ParseObject(Body), true, TArray<FString>(), TEXT("gameModelCreateSession"), HandSession);

	TestTrue(TEXT("CrowdyCPP session parses"), bCppOk);
	TestEqual(TEXT("session parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("sessionId matches"), CppSession.SessionId, HandSession.SessionId);
	TestEqual(TEXT("sessionId is s-1"), CppSession.SessionId, FString(TEXT("s-1")));
	TestEqual(TEXT("status matches"), CppSession.Status, HandSession.Status);
	TestEqual(TEXT("status is active"), CppSession.Status, FString(TEXT("active")));
	TestEqual(TEXT("createdBy matches"), CppSession.CreatedByUserId, HandSession.CreatedByUserId);
	TestEqual(TEXT("createdBy is 42"), CppSession.CreatedByUserId, static_cast<int64>(42));
	TestEqual(TEXT("turn-holder matches"), CppSession.CurrentTurnUserId, HandSession.CurrentTurnUserId);
	TestEqual(TEXT("has-turn flag matches"), CppSession.bHasCurrentTurn, HandSession.bHasCurrentTurn);
	TestTrue(TEXT("turn holder is present"), CppSession.bHasCurrentTurn);
	TestEqual(TEXT("turn holder is 7"), CppSession.CurrentTurnUserId, static_cast<int64>(7));
	return true;
}

// List parity: a sessions array decodes to the same GmSession list, with a null current-turn read as "no turn".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpListSessionsParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpListSessionsParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpListSessionsParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelSessions\":["
		"{\"sessionId\":\"s-1\",\"status\":\"active\",\"currentTurnUserId\":null},"
		"{\"sessionId\":\"s-2\",\"status\":\"ended\",\"currentTurnUserId\":\"9\"}]}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelSessions"), Body, 200);

	TArray<FCrowdyGameSessionData> CppSessions;
	const bool bCppOk = FCrowdyGameApiCodec::ParseSessionsEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppSessions);

	TArray<FCrowdyGameSessionData> HandSessions;
	const bool bHandOk = FCrowdyGameApiCodec::ParseSessionsEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandSessions);

	TestTrue(TEXT("CrowdyCPP sessions parse"), bCppOk);
	TestEqual(TEXT("sessions parse-ok matches"), bCppOk, bHandOk);
	if (!TestEqual(TEXT("session count matches"), CppSessions.Num(), HandSessions.Num()))
	{
		return false;
	}
	TestEqual(TEXT("session count is 2"), CppSessions.Num(), 2);
	if (CppSessions.Num() == 2)
	{
		TestEqual(TEXT("first sessionId matches"), CppSessions[0].SessionId, HandSessions[0].SessionId);
		TestEqual(TEXT("first sessionId is s-1"), CppSessions[0].SessionId, FString(TEXT("s-1")));
		TestFalse(TEXT("first session has no turn"), CppSessions[0].bHasCurrentTurn);
		TestTrue(TEXT("second session has a turn"), CppSessions[1].bHasCurrentTurn);
		TestEqual(TEXT("second turn holder matches"), CppSessions[1].CurrentTurnUserId, HandSessions[1].CurrentTurnUserId);
		TestEqual(TEXT("second turn holder is 9"), CppSessions[1].CurrentTurnUserId, static_cast<int64>(9));
	}
	return true;
}

// Edge parity: an addEdge response decodes to the same GmEdge, and a real (nullable) weight of 0 is separable from
// an absent one via bHasWeight.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpAddEdgeParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpAddEdgeParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpAddEdgeParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelAddEdge\":{"
		"\"edgeId\":\"e-1\",\"fromContainerId\":\"c-1\",\"toContainerId\":\"c-2\","
		"\"relationshipType\":\"contains\",\"weight\":2.5}}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelAddEdge"), Body, 200);

	FCrowdyEdgeData CppEdge;
	const bool bCppOk = FCrowdyGameApiCodec::ParseAddEdgeEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppEdge);

	FCrowdyEdgeData HandEdge;
	const bool bHandOk = FCrowdyGameApiCodec::ParseAddEdgeEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandEdge);

	TestTrue(TEXT("CrowdyCPP edge parses"), bCppOk);
	TestEqual(TEXT("edge parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("edgeId matches"), CppEdge.EdgeId, HandEdge.EdgeId);
	TestEqual(TEXT("edgeId is e-1"), CppEdge.EdgeId, FString(TEXT("e-1")));
	TestEqual(TEXT("relationship matches"), CppEdge.RelationshipType, HandEdge.RelationshipType);
	TestEqual(TEXT("relationship is contains"), CppEdge.RelationshipType, FString(TEXT("contains")));
	TestEqual(TEXT("has-weight flag matches"), CppEdge.bHasWeight, HandEdge.bHasWeight);
	TestTrue(TEXT("weight present"), CppEdge.bHasWeight);
	TestEqual(TEXT("weight matches"), CppEdge.Weight, HandEdge.Weight);
	TestEqual(TEXT("weight is 2.5"), CppEdge.Weight, 2.5);
	return true;
}

// Traverse parity: the reachable nodes and walked edges decode identically through both paths.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpTraverseParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpTraverseParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpTraverseParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelTraverse\":{\"rootId\":\"c-1\","
		"\"nodes\":[{\"containerId\":\"c-1\"},{\"containerId\":\"c-2\"}],"
		"\"edges\":[{\"edgeId\":\"e-1\",\"fromContainerId\":\"c-1\",\"toContainerId\":\"c-2\","
		"\"relationshipType\":\"contains\"}]}}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelTraverse"), Body, 200);

	FCrowdyTraverseData CppTraverse;
	const bool bCppOk = FCrowdyGameApiCodec::ParseTraverseEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppTraverse);

	FCrowdyTraverseData HandTraverse;
	const bool bHandOk = FCrowdyGameApiCodec::ParseTraverseEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandTraverse);

	TestTrue(TEXT("CrowdyCPP traverse parses"), bCppOk);
	TestEqual(TEXT("traverse parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("rootId matches"), CppTraverse.RootId, HandTraverse.RootId);
	TestEqual(TEXT("rootId is c-1"), CppTraverse.RootId, FString(TEXT("c-1")));
	TestEqual(TEXT("node count matches"), CppTraverse.Nodes.Num(), HandTraverse.Nodes.Num());
	TestEqual(TEXT("nodes are 2"), CppTraverse.Nodes.Num(), 2);
	TestEqual(TEXT("edge count matches"), CppTraverse.Edges.Num(), HandTraverse.Edges.Num());
	TestEqual(TEXT("edges are 1"), CppTraverse.Edges.Num(), 1);
	if (CppTraverse.Edges.Num() == 1 && HandTraverse.Edges.Num() == 1)
	{
		TestEqual(TEXT("edge id matches"), CppTraverse.Edges[0].EdgeId, HandTraverse.Edges[0].EdgeId);
		TestEqual(TEXT("edge id is e-1"), CppTraverse.Edges[0].EdgeId, FString(TEXT("e-1")));
	}
	return true;
}

// SetProperty parity + the transport-vs-nothing contract: a committed write returns the container id; a GraphQL
// error (e.g. writability rejected) is a transport failure, not a silent success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpSetPropertyParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpSetPropertyParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpSetPropertyParityTest::RunTest(const FString& Parameters)
{
	// Case 1: a committed write.
	{
		const FString Body = TEXT("{\"data\":{\"gameModelSetProperty\":{\"containerId\":\"c-1\"}}}");

		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelSetProperty"), Body, 200);
		FString CppContainerId;
		const bool bCppOk = FCrowdyGameApiCodec::ParseSetPropertyEnvelope(
			WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppContainerId);
		FString HandContainerId;
		const bool bHandOk = FCrowdyGameApiCodec::ParseSetPropertyEnvelope(
			ParseObject(Body), true, TArray<FString>(), HandContainerId);

		TestTrue(TEXT("CrowdyCPP set-property ok"), bCppOk);
		TestEqual(TEXT("set-property ok matches"), bCppOk, bHandOk);
		TestEqual(TEXT("containerId matches"), CppContainerId, HandContainerId);
		TestEqual(TEXT("containerId is c-1"), CppContainerId, FString(TEXT("c-1")));
	}

	// Case 2: a GraphQL error is a transport failure through the generic seam.
	{
		const FString Body = TEXT("{\"errors\":[{\"message\":\"not writable\"}]}");
		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelSetProperty"), Body, 200);
		TestFalse(TEXT("CrowdyCPP: GraphQL error is not transport-ok"), CppResult.bTransportOk);
		TestTrue(TEXT("CrowdyCPP surfaces the error"), CppResult.ErrorMessage.Contains(TEXT("not writable")));
	}
	return true;
}

// Delete parity: a Boolean! delete result decodes to the same flag, and a GraphQL authorization error is a
// transport failure (bOk false), never mistaken for "it did not exist".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpDeleteParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpDeleteParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpDeleteParityTest::RunTest(const FString& Parameters)
{
	// Case 1: a real delete returns true.
	{
		const FString Body = TEXT("{\"data\":{\"gameModelDeleteContainer\":true}}");
		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelDeleteContainer"), Body, 200);
		bool bCppDeleted = false;
		const bool bCppOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(
			WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), bCppDeleted);
		bool bHandDeleted = false;
		const bool bHandOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(
			ParseObject(Body), true, TArray<FString>(), bHandDeleted);

		TestTrue(TEXT("CrowdyCPP delete transport-ok"), bCppOk);
		TestEqual(TEXT("delete ok matches"), bCppOk, bHandOk);
		TestEqual(TEXT("deleted flag matches"), bCppDeleted, bHandDeleted);
		TestTrue(TEXT("deleted is true"), bCppDeleted);
	}

	// Case 2: an authorization refusal is a GraphQL error -> not transport-ok.
	{
		const FString Body = TEXT("{\"errors\":[{\"message\":\"forbidden\"}]}");
		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelDeleteContainer"), Body, 200);
		bool bCppDeleted = false;
		const bool bCppOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(
			WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), bCppDeleted);
		TestFalse(TEXT("CrowdyCPP: refused delete is not transport-ok"), CppResult.bTransportOk);
		TestFalse(TEXT("CrowdyCPP: refused delete does not parse"), bCppOk);
	}
	return true;
}

// EnsureContainer parity: an ensured container (existing-or-created) decodes to the same containerId, ownerUserId,
// and created flag through both clients.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpEnsureContainerParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpEnsureContainerParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpEnsureContainerParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelEnsureContainer\":{"
		"\"container\":{\"containerId\":\"c-9\",\"ownerUserId\":\"42\",\"metadataJson\":\"{}\","
		"\"bindingKey\":\"player-42\"},\"created\":true}}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelEnsureContainer"), Body, 200);

	FString CppContainerId;
	int64 CppOwnerUserId = 0;
	bool CppCreated = false;
	const bool bCppOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppContainerId, CppOwnerUserId, CppCreated);

	FString HandContainerId;
	int64 HandOwnerUserId = 0;
	bool HandCreated = false;
	const bool bHandOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandContainerId, HandOwnerUserId, HandCreated);

	TestTrue(TEXT("CrowdyCPP ensure parses"), bCppOk);
	TestEqual(TEXT("ensure parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("containerId matches"), CppContainerId, HandContainerId);
	TestEqual(TEXT("containerId is c-9"), CppContainerId, FString(TEXT("c-9")));
	TestEqual(TEXT("ownerUserId matches"), CppOwnerUserId, HandOwnerUserId);
	TestEqual(TEXT("ownerUserId is 42"), CppOwnerUserId, static_cast<int64>(42));
	TestEqual(TEXT("created flag matches"), CppCreated, HandCreated);
	TestTrue(TEXT("created is true"), CppCreated);
	return true;
}

// ReadContainerByKey parity (happy path): a single row whose bindingKey matches what was asked for decodes to the
// same bFound/containerId/ownerUserId through both clients.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpReadByKeyParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpReadByKeyParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpReadByKeyParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelContainers\":["
		"{\"containerId\":\"c-5\",\"ownerUserId\":\"7\",\"bindingKey\":\"player-7\"}]}}");
	const FString ExpectedBindingKey = TEXT("player-7");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelContainers"), Body, 200);

	bool CppFound = false;
	FString CppContainerId;
	int64 CppOwnerUserId = 0;
	const bool bCppOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), ExpectedBindingKey,
		CppFound, CppContainerId, CppOwnerUserId);

	bool HandFound = false;
	FString HandContainerId;
	int64 HandOwnerUserId = 0;
	const bool bHandOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
		ParseObject(Body), true, TArray<FString>(), ExpectedBindingKey, HandFound, HandContainerId, HandOwnerUserId);

	TestTrue(TEXT("CrowdyCPP read-by-key parses"), bCppOk);
	TestEqual(TEXT("read-by-key parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("found flag matches"), CppFound, HandFound);
	TestTrue(TEXT("matching row is found"), CppFound);
	TestEqual(TEXT("containerId matches"), CppContainerId, HandContainerId);
	TestEqual(TEXT("containerId is c-5"), CppContainerId, FString(TEXT("c-5")));
	TestEqual(TEXT("ownerUserId matches"), CppOwnerUserId, HandOwnerUserId);
	TestEqual(TEXT("ownerUserId is 7"), CppOwnerUserId, static_cast<int64>(7));
	return true;
}

// The drifted-server guard: gameModelContainers is a filterable list, so the parser re-checks each row's bindingKey
// against what was asked for rather than trusting the server to have honored the filter argument. A row whose key
// does not match must never be bound - that would point a remote proxy at another player's container.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpReadByKeyRejectsUnmatchedBindingKeyTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpReadByKeyRejectsUnmatchedBindingKey", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpReadByKeyRejectsUnmatchedBindingKeyTest::RunTest(const FString& Parameters)
{
	const FString ExpectedBindingKey = TEXT("player-42");

	// Case 1: the only row returned has a different bindingKey, simulating a server that ignored the filter. Must
	// not be bound: bFound stays false and no containerId is produced.
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelContainers\":["
			"{\"containerId\":\"c-attacker\",\"ownerUserId\":\"999\",\"bindingKey\":\"player-999\"}]}}");

		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelContainers"), Body, 200);

		bool CppFound = true;
		FString CppContainerId = TEXT("unset");
		int64 CppOwnerUserId = -1;
		const bool bCppOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
			WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), ExpectedBindingKey,
			CppFound, CppContainerId, CppOwnerUserId);

		bool HandFound = true;
		FString HandContainerId = TEXT("unset");
		int64 HandOwnerUserId = -1;
		const bool bHandOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
			ParseObject(Body), true, TArray<FString>(), ExpectedBindingKey, HandFound, HandContainerId, HandOwnerUserId);

		TestTrue(TEXT("CrowdyCPP: an unmatched-key row still parses as a list"), bCppOk);
		TestEqual(TEXT("parse-ok matches"), bCppOk, bHandOk);
		TestFalse(TEXT("CrowdyCPP: no matching row is found"), CppFound);
		TestEqual(TEXT("found flag matches"), CppFound, HandFound);
		TestTrue(TEXT("CrowdyCPP: containerId stays empty"), CppContainerId.IsEmpty());
		TestEqual(TEXT("containerId matches (both empty)"), CppContainerId, HandContainerId);
	}

	// Case 2: a matching row is present alongside a non-matching one; the matching row must be the one selected.
	{
		const FString Body = TEXT(
			"{\"data\":{\"gameModelContainers\":["
			"{\"containerId\":\"c-attacker\",\"ownerUserId\":\"999\",\"bindingKey\":\"player-999\"},"
			"{\"containerId\":\"c-42\",\"ownerUserId\":\"42\",\"bindingKey\":\"player-42\"}]}}");

		const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelContainers"), Body, 200);

		bool CppFound = false;
		FString CppContainerId;
		int64 CppOwnerUserId = 0;
		const bool bCppOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
			WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), ExpectedBindingKey,
			CppFound, CppContainerId, CppOwnerUserId);

		bool HandFound = false;
		FString HandContainerId;
		int64 HandOwnerUserId = 0;
		const bool bHandOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(
			ParseObject(Body), true, TArray<FString>(), ExpectedBindingKey, HandFound, HandContainerId, HandOwnerUserId);

		TestTrue(TEXT("CrowdyCPP: mixed-row list parses"), bCppOk);
		TestEqual(TEXT("parse-ok matches"), bCppOk, bHandOk);
		TestTrue(TEXT("CrowdyCPP: the matching row is found"), CppFound);
		TestEqual(TEXT("found flag matches"), CppFound, HandFound);
		TestEqual(TEXT("containerId matches"), CppContainerId, HandContainerId);
		TestEqual(TEXT("the matching container is selected, not the attacker's"), CppContainerId, FString(TEXT("c-42")));
		TestEqual(TEXT("ownerUserId matches"), CppOwnerUserId, HandOwnerUserId);
		TestEqual(TEXT("ownerUserId is 42"), CppOwnerUserId, static_cast<int64>(42));
	}
	return true;
}

// CreateContainer parity: a created container decodes to the same containerId and ownerUserId through both clients.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpCreateContainerParityTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpCreateContainerParity", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpCreateContainerParityTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"{\"data\":{\"gameModelCreateContainer\":{"
		"\"containerId\":\"c-77\",\"ownerUserId\":\"13\",\"metadataJson\":\"{}\"}}}");

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelCreateContainer"), Body, 200);

	FString CppContainerId;
	int64 CppOwnerUserId = 0;
	const bool bCppOk = FCrowdyGameApiCodec::ParseCreateContainerEnvelope(
		WrapData(CppResult.Data), CppResult.bTransportOk, TArray<FString>(), CppContainerId, CppOwnerUserId);

	FString HandContainerId;
	int64 HandOwnerUserId = 0;
	const bool bHandOk = FCrowdyGameApiCodec::ParseCreateContainerEnvelope(
		ParseObject(Body), true, TArray<FString>(), HandContainerId, HandOwnerUserId);

	TestTrue(TEXT("CrowdyCPP create-container parses"), bCppOk);
	TestEqual(TEXT("create-container parse-ok matches"), bCppOk, bHandOk);
	TestEqual(TEXT("containerId matches"), CppContainerId, HandContainerId);
	TestEqual(TEXT("containerId is c-77"), CppContainerId, FString(TEXT("c-77")));
	TestEqual(TEXT("ownerUserId matches"), CppOwnerUserId, HandOwnerUserId);
	TestEqual(TEXT("ownerUserId is 13"), CppOwnerUserId, static_cast<int64>(13));
	return true;
}

// A forged deeply-nested data object is rejected before the recursive UE re-parse: the dumped data object is
// nesting-guarded, so an over-deep value fails the op closed (bTransportOk false) rather than building an
// overflowing DOM. Here the depth is genuine JSON nesting, not a string, so it survives the yyjson parse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppRuntimeOpNestingGuardTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpNestingGuard", CrowdyCppParityTestFlags)
bool FCrowdyCppRuntimeOpNestingGuardTest::RunTest(const FString& Parameters)
{
	FString Deep;
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("{\"a\":");
	}
	Deep += TEXT("1");
	for (int32 i = 0; i < 200; ++i)
	{
		Deep += TEXT("}");
	}
	const FString Body = FString::Printf(TEXT(
		"{\"data\":{\"gameModelSession\":{\"sessionId\":\"s-1\",\"nested\":%s}}}"), *Deep);

	const FCrowdyCppJsonResult CppResult = RunRuntimeOpViaCrowdyCpp(TEXT("GameModelSession"), Body, 200);
	TestFalse(TEXT("over-deep response data is rejected"), CppResult.bTransportOk);
	TestFalse(TEXT("rejected response has no data"), CppResult.Data.IsValid());
	return true;
}

// The vendored library's linkage and build configuration, checked the same way the
// crowdy.cpp.selftest console command checks it. This runs the checks that a green
// compile does not cover: whether the crypto provider is actually reachable, whether
// RTTI survived the build settings, and whether every operation the plugin sends
// still resolves to a document. Each of those can fail silently, so this belongs in
// the suite rather than only in a command someone has to remember to type after a
// re-vendor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppBridgeSelfTestTest,
	"CrowdySDK.CrowdyCpp.BridgeSelfTest", CrowdyCppParityTestFlags)
bool FCrowdyCppBridgeSelfTestTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the vendored CrowdyCPP build is linked and correctly configured"),
		FCrowdyCppBridge::SelfTest());
	return true;
}

// An operation name the library does not know must fail without a round trip rather
// than reaching the server as an empty document.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppUnknownOperationTest,
	"CrowdySDK.CrowdyCpp.RuntimeOpUnknownOperation", CrowdyCppParityTestFlags)
bool FCrowdyCppUnknownOperationTest::RunTest(const FString& Parameters)
{
	const FCrowdyCppJsonResult CppResult =
		RunRuntimeOpViaCrowdyCpp(TEXT("GameModelNoSuchOperation"), TEXT("{\"data\":{}}"), 200);
	TestFalse(TEXT("an unknown operation fails"), CppResult.bTransportOk);
	TestTrue(TEXT("the failure names the operation that could not be resolved"),
		CppResult.ErrorMessage.Contains(TEXT("GameModelNoSuchOperation")));
	return true;
}

// The vendored CrowdyCPP library is compiled from source, so its behaviour is whatever that snapshot does,
// pinned to the version recorded in VENDOR.txt. Re-vendoring changes that behaviour, so a version bump must
// be a deliberate, reviewed edit to the constant below rather than something that rides along silently in
// a sync script's output.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVendoredVersionTest,
	"CrowdySDK.CrowdyCpp.VendoredVersion", CrowdyCppParityTestFlags)
bool FCrowdyCppVendoredVersionTest::RunTest(const FString& Parameters)
{
	static const FString ExpectedVendoredCrowdyCppVersion = TEXT("0.29.1");

	// The tier decides where a client that names no origin at all dials, and it is generated per branch
	// upstream, so it can change under a version bump without the version saying so. Pin it too.
	static const FString ExpectedVendoredCrowdyCppTier = TEXT("dev");

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
	if (!TestTrue(TEXT("the CrowdySDK plugin is found"), Plugin.IsValid()))
	{
		return false;
	}

	const FString VendorFilePath = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Source"), TEXT("CrowdyCppBridge"), TEXT("ThirdParty"), TEXT("CrowdyCPP"), TEXT("VENDOR.txt"));
	FString VendorFileContents;
	if (!TestTrue(TEXT("VENDOR.txt is present under the vendored CrowdyCPP directory"),
		FFileHelper::LoadFileToString(VendorFileContents, *VendorFilePath)))
	{
		return false;
	}

	TArray<FString> Lines;
	VendorFileContents.ParseIntoArrayLines(Lines);
	FString ParsedVersion;
	FString ParsedTier;
	for (const FString& Line : Lines)
	{
		const FString TrimmedLine = Line.TrimStartAndEnd();
		if (ParsedVersion.IsEmpty() && TrimmedLine.StartsWith(TEXT("version:")))
		{
			ParsedVersion = TrimmedLine.Mid(8).TrimStartAndEnd();
		}
		else if (ParsedTier.IsEmpty() && TrimmedLine.StartsWith(TEXT("tier:")))
		{
			// Recorded as "<name> (<origin>)", and only the name is pinned here.
			ParsedTier = TrimmedLine.Mid(5).TrimStartAndEnd();
			int32 SpaceIndex = INDEX_NONE;
			if (ParsedTier.FindChar(TEXT(' '), SpaceIndex))
			{
				ParsedTier = ParsedTier.Left(SpaceIndex);
			}
		}
	}

	if (!TestTrue(TEXT("VENDOR.txt has a version: line"), !ParsedVersion.IsEmpty()))
	{
		return false;
	}

	TestEqual(TEXT("the vendored CrowdyCPP version matches the pinned expectation"),
		ParsedVersion, ExpectedVendoredCrowdyCppVersion);

	if (TestTrue(TEXT("VENDOR.txt has a tier: line"), !ParsedTier.IsEmpty()))
	{
		TestEqual(TEXT("the vendored CrowdyCPP tier matches the pinned expectation"),
			ParsedTier, ExpectedVendoredCrowdyCppTier);
	}
	return true;
}

// GraphQL returns only the fields a document asks for, so the invoke query is what decides whether this SDK can
// ever see the server's blame attribution. The document is generated upstream and arrives here by re-vendoring,
// which means a sync from a checkout that predates the field would silently take the whole retry surface back to
// inert with nothing failing. Reading the vendored document is the only place that can catch it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppVendoredInvokeSelectsFaultTest,
	"CrowdySDK.CrowdyCpp.VendoredInvokeSelectsFault", CrowdyCppParityTestFlags)
bool FCrowdyCppVendoredInvokeSelectsFaultTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
	if (!TestTrue(TEXT("the CrowdySDK plugin is found"), Plugin.IsValid()))
	{
		return false;
	}

	const FString OperationsPath = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Source"), TEXT("CrowdyCppBridge"), TEXT("ThirdParty"), TEXT("CrowdyCPP"),
		TEXT("include"), TEXT("crowdy"), TEXT("generated"), TEXT("operations.hpp"));
	FString Operations;
	if (!TestTrue(TEXT("the vendored generated operations header is present"),
		FFileHelper::LoadFileToString(Operations, *OperationsPath)))
	{
		return false;
	}

	// Every copy of the fragment has to carry it: the generator emits one inside the isolated single-operation
	// document and one in the shared block, and a caller gets whichever document its operation resolved to.
	int32 FragmentCount = 0;
	int32 SelectingCount = 0;
	int32 SearchFrom = 0;
	const FString FragmentMarker = TEXT("fragment GmInvokeResultFields on GmInvokeResult {");
	while (true)
	{
		const int32 FragmentStart = Operations.Find(FragmentMarker, ESearchCase::CaseSensitive,
			ESearchDir::FromStart, SearchFrom);
		if (FragmentStart == INDEX_NONE)
		{
			break;
		}
		++FragmentCount;
		SearchFrom = FragmentStart + FragmentMarker.Len();

		// Bounded to this fragment's own text, so a fault selection belonging to some later operation cannot make
		// this one read as compliant.
		const int32 FragmentEnd = Operations.Find(TEXT("\n}"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, SearchFrom);
		const FString Body = Operations.Mid(SearchFrom,
			(FragmentEnd == INDEX_NONE ? Operations.Len() : FragmentEnd) - SearchFrom);
		if (Body.Contains(TEXT("fault"), ESearchCase::CaseSensitive)
			&& Body.Contains(TEXT("blame"), ESearchCase::CaseSensitive)
			&& Body.Contains(TEXT("retryable"), ESearchCase::CaseSensitive)
			&& Body.Contains(TEXT("code"), ESearchCase::CaseSensitive))
		{
			++SelectingCount;
		}
	}

	if (!TestTrue(TEXT("the vendored operations declare the invoke result fragment"), FragmentCount > 0))
	{
		return false;
	}
	TestEqual(TEXT("every copy of the invoke fragment selects fault with code, blame and retryable"),
		SelectingCount, FragmentCount);
	return true;
}

// The blame vocabulary is the server's, and anything outside it has to read as Unknown rather than as a licensed
// retry: an unattributed failure is the case where repeating an invoke can apply a write twice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPlayerFaultBlameVocabularyTest,
	"CrowdySDK.CrowdyCpp.PlayerFaultBlameVocabulary", CrowdyCppParityTestFlags)
bool FCrowdyPlayerFaultBlameVocabularyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("PLATFORM maps to Platform"),
		CrowdyPlayerFaultBlameFromWireString(TEXT("PLATFORM")), ECrowdyPlayerFaultBlame::Platform);
	TestEqual(TEXT("AUTHOR maps to Author"),
		CrowdyPlayerFaultBlameFromWireString(TEXT("AUTHOR")), ECrowdyPlayerFaultBlame::Author);
	TestEqual(TEXT("BUDGET maps to Budget"),
		CrowdyPlayerFaultBlameFromWireString(TEXT("BUDGET")), ECrowdyPlayerFaultBlame::Budget);
	TestEqual(TEXT("an empty attribution is Unknown, never Platform"),
		CrowdyPlayerFaultBlameFromWireString(FString()), ECrowdyPlayerFaultBlame::Unknown);
	TestEqual(TEXT("a value the server has not shipped yet is Unknown"),
		CrowdyPlayerFaultBlameFromWireString(TEXT("INFRASTRUCTURE")), ECrowdyPlayerFaultBlame::Unknown);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
