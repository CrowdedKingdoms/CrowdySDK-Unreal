#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The Exec domain: a developer's server-code operations resolve to their generated documents and carry the session bearer.
namespace CrowdyNativeExecDeveloperTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const GameBearer = TEXT("game-bearer-token");
	const TCHAR* const SessionBearer = TEXT("session-bearer-token");

	const TCHAR* const DeveloperOperations[] =
	{
		TEXT("ExecStarters"),
		TEXT("ExecBuild"),
		TEXT("ExecBuildStatus"),
		TEXT("ExecDeploy"),
		TEXT("ExecVersions"),
		TEXT("ExecActivateVersion"),
		TEXT("ExecSetEnabled"),
		TEXT("ExecAppStatus"),
		TEXT("ExecInstances"),
		TEXT("ExecLogs"),
		TEXT("ExecEndpointStats"),
	};

	FString BusyBody()
	{
		return TEXT("{\"errors\":[{\"message\":\"The platform is busy.\",\"extensions\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":true,\"retryAfterMs\":0}}],\"data\":null}");
	}

	TSharedPtr<FCrowdyCppClient> MakeClient(int32& OutSends)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
		if (!Client.IsValid())
		{
			return nullptr;
		}
		Client->SetGameToken(GameBearer);
		Client->SetManagementToken(SessionBearer);
		Client->SetTestOnRequest([&OutSends](const FString&) { ++OutSends; });
		return Client;
	}

	struct FOutcome
	{
		bool bDone = false;
		bool bDoneBeforePoll = false;
		FCrowdyCppJsonResult Result;
	};

	// Issues Operation in the Exec domain without naming a plane, so the domain's own plane is the one used.
	void Issue(FCrowdyCppClient& Client, const TCHAR* Operation, FOutcome& Out)
	{
		const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), TEXT("7"));
		Client.RunOp(ECrowdyCppApiDomain::Exec, Operation, Variables, [&Out](FCrowdyCppJsonResult Result)
		{
			Out.Result = MoveTemp(Result);
			Out.bDone = true;
		});
		Out.bDoneBeforePoll = Out.bDone;
	}

	bool PollUntilDone(FCrowdyCppClient& Client, const FOutcome& Out)
	{
		const double Until = FPlatformTime::Seconds() + 3.0;
		while (!Out.bDone)
		{
			if (FPlatformTime::Seconds() > Until)
			{
				return false;
			}
			Client.Poll();
			FPlatformProcess::Sleep(0.002f);
		}
		return true;
	}

	FString LastOperationName(const FCrowdyCppClient& Client)
	{
		FString Body;
		Client.GetLastTestRequestBody(Body);
		TSharedPtr<FJsonObject> Request;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request);
		FString Name;
		if (Request.IsValid())
		{
			Request->TryGetStringField(TEXT("operationName"), Name);
		}
		return Name;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecDeveloperOperationsTest,
	"CrowdySDK.CrowdyCppBridge.ExecDeveloperOperationsCarryTheSessionBearer", CrowdyNativeExecDeveloperTests::TestFlags)
bool FCrowdyNativeExecDeveloperOperationsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNativeExecDeveloperTests;
	for (const TCHAR* Operation : DeveloperOperations)
	{
		int32 Sends = 0;
		FOutcome Out;
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
		if (!TestTrue(FString::Printf(TEXT("a test client is made for %s"), Operation), Client.IsValid()))
		{
			return false;
		}

		Issue(*Client, Operation, Out);
		TestFalse(FString::Printf(TEXT("%s resolves to a document"), Operation), Out.bDoneBeforePoll);
		if (!TestTrue(FString::Printf(TEXT("%s is answered"), Operation), PollUntilDone(*Client, Out)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s reaches the transport (%s)"), Operation, *Out.Result.ErrorMessage), Out.Result.bTransportOk);

		FString Url;
		FString Authorization;
		if (!TestTrue(FString::Printf(TEXT("%s was sent"), Operation), Client->GetLastTestRequest(Url, Authorization)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s carries the session bearer"), Operation), Authorization.Contains(SessionBearer));
		TestFalse(FString::Printf(TEXT("%s does not carry the game bearer"), Operation), Authorization.Contains(GameBearer));
		TestEqual(FString::Printf(TEXT("%s is sent under its own name"), Operation), LastOperationName(*Client), FString(Operation));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecDeveloperUnknownOperationTest,
	"CrowdySDK.CrowdyCppBridge.ExecDeveloperUnknownOperationFailsWithoutARoundTrip", CrowdyNativeExecDeveloperTests::TestFlags)
bool FCrowdyNativeExecDeveloperUnknownOperationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNativeExecDeveloperTests;
	int32 Sends = 0;
	FOutcome Out;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Sends);
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}

	Issue(*Client, TEXT("ExecNoSuchOperation"), Out);
	TestTrue(TEXT("the answer comes before any poll"), Out.bDoneBeforePoll);
	TestFalse(TEXT("it failed"), Out.Result.bTransportOk);
	TestTrue(TEXT("the failure names the operation"), Out.Result.ErrorMessage.Contains(TEXT("ExecNoSuchOperation")));
	TestEqual(TEXT("nothing was sent"), Sends, 0);
	return true;
}

// The bridge resends a busy query but hands a busy mutation straight back, which is why the developer client retries its changes itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeExecDeveloperBusyTest,
	"CrowdySDK.CrowdyCppBridge.ExecDeveloperBusyResendsQueriesOnly", CrowdyNativeExecDeveloperTests::TestFlags)
bool FCrowdyNativeExecDeveloperBusyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNativeExecDeveloperTests;
	if (!CrowdyCppIsBusyRetryEnabled())
	{
		AddInfo(TEXT("crowdy.net.retry.busy is 0, so the bridge resends nothing; skipped"));
		return true;
	}

	int32 MutationSends = 0;
	int32 QuerySends = 0;
	FOutcome Mutation;
	FOutcome Query;
	const TSharedPtr<FCrowdyCppClient> MutationClient = MakeClient(MutationSends);
	const TSharedPtr<FCrowdyCppClient> QueryClient = MakeClient(QuerySends);
	if (!TestTrue(TEXT("test clients are made"), MutationClient.IsValid() && QueryClient.IsValid()))
	{
		return false;
	}

	MutationClient->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()), TPair<int32, FString>(200, TEXT("{\"data\":{\"execDeploy\":{\"version\":2}}}")) });
	Issue(*MutationClient, TEXT("ExecDeploy"), Mutation);
	TestTrue(TEXT("the busy deploy is answered"), PollUntilDone(*MutationClient, Mutation));
	TestEqual(TEXT("the busy deploy was sent once"), MutationSends, 1);
	TestEqual(TEXT("its refusal reaches the caller"), Mutation.Result.ErrorCode, FString(TEXT("PLATFORM_BUSY")));

	QueryClient->SetTestResponseScript({ TPair<int32, FString>(200, BusyBody()), TPair<int32, FString>(200, TEXT("{\"data\":{\"execVersions\":[]}}")) });
	Issue(*QueryClient, TEXT("ExecVersions"), Query);
	TestTrue(TEXT("the busy query is answered"), PollUntilDone(*QueryClient, Query));
	TestEqual(TEXT("the busy query was sent again"), QuerySends, 2);
	TestTrue(TEXT("the second answer reaches the caller"), Query.Result.bTransportOk);
	return true;
}

#endif
