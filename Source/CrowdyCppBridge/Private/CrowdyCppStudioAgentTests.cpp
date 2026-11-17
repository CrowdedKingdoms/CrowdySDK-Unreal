#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyCppStudio.h"
#include "CrowdyCppStudioStopgaps.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The Studio agent surface: its operations resolve and carry the app token, the model list is read as untrusted, and
// compiler output becomes diagnostics.
namespace CrowdyCppStudioAgentTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const AppBearer = TEXT("app-bearer-token");
	const TCHAR* const SessionBearer = TEXT("session-bearer-token");

	FCrowdyCppClientConfig TestConfig()
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = TEXT("https://game.test");
		return Config;
	}

	TSharedPtr<FCrowdyCppClient> MakeClient(const FString& Body = TEXT("{\"data\":{}}"), int32 Status = 200, int32* OutSends = nullptr)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(Body, Status, TestConfig());
		if (!Client.IsValid())
		{
			return nullptr;
		}
		Client->SetGameToken(AppBearer);
		Client->SetManagementToken(SessionBearer);
		if (OutSends)
		{
			Client->SetTestOnRequest([OutSends](const FString&) { ++*OutSends; });
		}
		return Client;
	}

	template <typename ResultType>
	struct TOutcome
	{
		int32 Deliveries = 0;
		bool bDoneBeforePoll = false;
		ResultType Result;
	};

	template <typename ResultType>
	bool PollUntilDone(FCrowdyCppClient& Client, const TOutcome<ResultType>& Out)
	{
		const double Until = FPlatformTime::Seconds() + 3.0;
		while (Out.Deliveries == 0)
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

	void IssueOp(FCrowdyCppClient& Client, ECrowdyCppApiDomain Domain, const TCHAR* Operation,
		const TSharedRef<FJsonObject>& Variables, TOutcome<FCrowdyCppJsonResult>& Out)
	{
		Client.RunOp(Domain, Operation, Variables, [&Out](FCrowdyCppJsonResult Result)
		{
			Out.Result = MoveTemp(Result);
			++Out.Deliveries;
		});
		Out.bDoneBeforePoll = Out.Deliveries > 0;
	}

	void IssueModels(FCrowdyCppClient& Client, int64 AppId, TOutcome<FCrowdyCppStudioModelsResult>& Out)
	{
		Client.FetchStudioModels(AppId, [&Out](FCrowdyCppStudioModelsResult Result)
		{
			Out.Result = MoveTemp(Result);
			++Out.Deliveries;
		});
		Out.bDoneBeforePoll = Out.Deliveries > 0;
	}

	TSharedRef<FJsonObject> AppIdVariables()
	{
		const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), TEXT("42"));
		return Variables;
	}

	TSharedPtr<FJsonObject> LastRequestBody(const FCrowdyCppClient& Client)
	{
		FString Body;
		TSharedPtr<FJsonObject> Request;
		if (Client.GetLastTestRequestBody(Body))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request);
		}
		return Request;
	}

	FString LastOperationName(const FCrowdyCppClient& Client)
	{
		const TSharedPtr<FJsonObject> Request = LastRequestBody(Client);
		FString Name;
		if (Request.IsValid())
		{
			Request->TryGetStringField(TEXT("operationName"), Name);
		}
		return Name;
	}

	// Issues Operation without naming a plane and checks it went out under the app token.
	void ExpectAppToken(FAutomationTestBase& Test, ECrowdyCppApiDomain Domain, const TCHAR* Operation)
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient();
		if (!Test.TestTrue(FString::Printf(TEXT("a test client is made for %s"), Operation), Client.IsValid()))
		{
			return;
		}
		TOutcome<FCrowdyCppJsonResult> Out;
		IssueOp(*Client, Domain, Operation, AppIdVariables(), Out);
		if (!Test.TestTrue(FString::Printf(TEXT("%s is answered"), Operation), PollUntilDone(*Client, Out)))
		{
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s resolves and is sent (%s)"), Operation, *Out.Result.ErrorMessage), Out.Result.bTransportOk);

		FString Url;
		FString Authorization;
		if (!Test.TestTrue(FString::Printf(TEXT("%s reached the transport"), Operation), Client->GetLastTestRequest(Url, Authorization)))
		{
			return;
		}
		Test.TestEqual(FString::Printf(TEXT("%s carries the app token"), Operation), Authorization, FString(TEXT("Bearer ")) + AppBearer);
		Test.TestEqual(FString::Printf(TEXT("%s is sent under its own name"), Operation), LastOperationName(*Client), FString(Operation));
	}

	const TCHAR* const GoodModelList = TEXT("{\"object\":\"list\",\"data\":["
		"{\"id\":\"openai/gpt-oss-120b\",\"object\":\"model\",\"owned_by\":\"crowded-kingdoms\",\"name\":\"GPT OSS 120B\",\"context_window\":null,"
		"\"input_modalities\":[\"text\"],\"pricing_microusd_per_million\":{\"input\":195000,\"output\":780000}},"
		"{\"id\":\"vision/model\",\"name\":\"Vision\",\"context_window\":131072,\"input_modalities\":[\"text\",\"image\",7]},"
		"{\"name\":\"no id\"},"
		"\"not an object\"]}");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioAgentOperationsTest,
	"CrowdySDK.CrowdyCppBridge.StudioAgent.OperationsCarryTheAppToken", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioAgentOperationsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	ExpectAppToken(*this, ECrowdyCppApiDomain::CrowdyStudioAgent, TEXT("CrowdyStudioProviderConsent"));
	ExpectAppToken(*this, ECrowdyCppApiDomain::CrowdyStudioAgent, TEXT("CrowdyStudioSetProviderConsent"));
	ExpectAppToken(*this, ECrowdyCppApiDomain::CrowdyStudioAgent, TEXT("CrowdyStudioModelUsage"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioAgentExecClientModTest,
	"CrowdySDK.CrowdyCppBridge.StudioAgent.ExecClientModOperationsCarryTheAppToken", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioAgentExecClientModTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	ExpectAppToken(*this, ECrowdyCppApiDomain::Exec, TEXT("ExecModClientBuild"));
	ExpectAppToken(*this, ECrowdyCppApiDomain::Exec, TEXT("ExecModClientDeploy"));
	ExpectAppToken(*this, ECrowdyCppApiDomain::Exec, TEXT("ExecModClientDelete"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioAgentLiveModdingTest,
	"CrowdySDK.CrowdyCppBridge.StudioAgent.LiveModdingResolvesAndAnswersABoolean", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioAgentLiveModdingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"data\":{\"crowdyStudioLiveModding\":true}}"));
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Variables = AppIdVariables();
	Variables->SetStringField(TEXT("action"), TEXT("draft_test"));
	TOutcome<FCrowdyCppJsonResult> Out;
	IssueOp(*Client, ECrowdyCppApiDomain::CrowdyStudioAgent, TEXT("CrowdyStudioLiveModding"), Variables, Out);
	TestFalse(TEXT("the operation resolves to a document"), Out.bDoneBeforePoll);
	if (!TestTrue(TEXT("it is answered"), PollUntilDone(*Client, Out)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("it was sent (%s)"), *Out.Result.ErrorMessage), Out.Result.bTransportOk);
	bool bAllowed = false;
	TestTrue(TEXT("the answer is a bare boolean"), Out.Result.Data.IsValid()
		&& Out.Result.Data->TryGetBoolField(TEXT("crowdyStudioLiveModding"), bAllowed));
	TestTrue(TEXT("the boolean reads true"), bAllowed);

	FString Url;
	FString Authorization;
	if (!TestTrue(TEXT("it reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		return false;
	}
	TestEqual(TEXT("it carries the app token"), Authorization, FString(TEXT("Bearer ")) + AppBearer);

	const TSharedPtr<FJsonObject> Request = LastRequestBody(*Client);
	const TSharedPtr<FJsonObject>* Sent = nullptr;
	if (!TestTrue(TEXT("the request carries variables"), Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Sent) && Sent))
	{
		return false;
	}
	FString Query;
	Request->TryGetStringField(TEXT("query"), Query);
	TestTrue(TEXT("the document selects crowdyStudioLiveModding"),
		Query.Contains(TEXT("crowdyStudioLiveModding(appId: $appId, action: $action)")));
	TestEqual(TEXT("it is sent under its own name"), LastOperationName(*Client), FString(TEXT("CrowdyStudioLiveModding")));

	const TSharedPtr<FJsonValue> SentAppId = (*Sent)->TryGetField(TEXT("appId"));
	TestTrue(TEXT("appId travels as a JSON string"), SentAppId.IsValid() && SentAppId->Type == EJson::String && SentAppId->AsString() == TEXT("42"));
	FString SentAction;
	(*Sent)->TryGetStringField(TEXT("action"), SentAction);
	TestEqual(TEXT("action is sent"), SentAction, FString(TEXT("draft_test")));
	return true;
}

// A busy refusal of the hand-written query is sent again, which a mutation's would not be.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioAgentLiveModdingBusyTest,
	"CrowdySDK.CrowdyCppBridge.StudioAgent.LiveModdingIsRetriedAsAQuery", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioAgentLiveModdingBusyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	if (!CrowdyCppIsBusyRetryEnabled())
	{
		AddInfo(TEXT("crowdy.net.retry.busy is 0, so the bridge resends nothing; skipped"));
		return true;
	}
	int32 Sends = 0;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"data\":{}}"), 200, &Sends);
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}
	const FString Busy = TEXT("{\"errors\":[{\"message\":\"The platform is busy.\",\"extensions\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":true,\"retryAfterMs\":0}}],\"data\":null}");
	Client->SetTestResponseScript({ TPair<int32, FString>(200, Busy), TPair<int32, FString>(200, TEXT("{\"data\":{\"crowdyStudioLiveModding\":false}}")) });

	const TSharedRef<FJsonObject> Variables = AppIdVariables();
	Variables->SetStringField(TEXT("action"), TEXT("deploy_live"));
	TOutcome<FCrowdyCppJsonResult> Out;
	IssueOp(*Client, ECrowdyCppApiDomain::CrowdyStudioAgent, TEXT("CrowdyStudioLiveModding"), Variables, Out);
	if (!TestTrue(TEXT("it is answered"), PollUntilDone(*Client, Out)))
	{
		return false;
	}
	TestEqual(TEXT("the busy query was sent again"), Sends, 2);
	TestTrue(TEXT("the second answer reaches the caller"), Out.Result.bTransportOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsRequestTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.RequestUsesTheOriginAndTheAppToken", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsRequestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	TestEqual(TEXT("the path, query and fragment are stripped"),
		CrowdyCppStudioStopgaps::UrlOrigin(TEXT("https://game.test:8443/api/graphql?x=1#y")), FString(TEXT("https://game.test:8443")));
	TestEqual(TEXT("a bare origin is kept"), CrowdyCppStudioStopgaps::UrlOrigin(TEXT("https://game.test")), FString(TEXT("https://game.test")));
	TestTrue(TEXT("a URL with no scheme has no origin"), CrowdyCppStudioStopgaps::UrlOrigin(TEXT("game.test/graphql")).IsEmpty());
	TestTrue(TEXT("a URL with no host has no origin"), CrowdyCppStudioStopgaps::UrlOrigin(TEXT("https:///graphql")).IsEmpty());

	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"object\":\"list\",\"data\":[]}"));
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the client issues against a graphql path"), Client->GetApiEndpoint().EndsWith(TEXT("/graphql")));

	TOutcome<FCrowdyCppStudioModelsResult> Out;
	IssueModels(*Client, 42, Out);
	TestFalse(TEXT("the answer waits for Poll"), Out.bDoneBeforePoll);
	if (!TestTrue(TEXT("it is answered"), PollUntilDone(*Client, Out)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("an empty list is a good answer (%s)"), *Out.Result.ErrorMessage), Out.Result.bOk);

	FString Url;
	FString Authorization;
	if (!TestTrue(TEXT("it reached the transport"), Client->GetLastTestRequest(Url, Authorization)))
	{
		return false;
	}
	TestEqual(TEXT("the URL is the endpoint's origin plus the model route"), Url, FString(TEXT("https://game.test/v1/model/models?appId=42")));
	TestEqual(TEXT("it carries the app token"), Authorization, FString(TEXT("Bearer ")) + AppBearer);
	FString Body;
	Client->GetLastTestRequestBody(Body);
	TestTrue(TEXT("a GET sends no body"), Body.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsParseTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.ParsesAGoodList", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsParseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(GoodModelList);
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}
	TOutcome<FCrowdyCppStudioModelsResult> Out;
	IssueModels(*Client, 42, Out);
	if (!TestTrue(TEXT("it is answered"), PollUntilDone(*Client, Out)))
	{
		return false;
	}
	const FCrowdyCppStudioModelsResult& Result = Out.Result;
	TestTrue(FString::Printf(TEXT("the list reads (%s)"), *Result.ErrorMessage), Result.bOk);
	TestEqual(TEXT("the status is kept"), Result.HttpStatus, 200);
	TestEqual(TEXT("exactly one delivery"), Out.Deliveries, 1);
	if (!TestEqual(TEXT("entries with no id or no object are skipped"), Result.Models.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("first id"), Result.Models[0].Id, FString(TEXT("openai/gpt-oss-120b")));
	TestEqual(TEXT("first name"), Result.Models[0].Name, FString(TEXT("GPT OSS 120B")));
	TestEqual(TEXT("a null context window reads as 0"), Result.Models[0].ContextWindow, 0);
	TestTrue(TEXT("first modalities"), Result.Models[0].InputModalities == TArray<FString>({ TEXT("text") }));
	TestEqual(TEXT("a stated context window is kept"), Result.Models[1].ContextWindow, 131072);
	TestTrue(TEXT("a modality that is not a string is skipped"),
		Result.Models[1].InputModalities == TArray<FString>({ TEXT("text"), TEXT("image") }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsErrorTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.MapsAnErrorBody", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsErrorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient();
	if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestResponseScript({
		TPair<int32, FString>(402, TEXT("{\"error\":{\"message\":\"Add funds to continue.\",\"type\":\"crowded_kingdoms_error\",\"code\":\"AGENT_FUNDS_NEEDED\",\"retryable\":false}}")),
		TPair<int32, FString>(503, TEXT("{\"error\":{\"message\":\"Provider unavailable.\",\"code\":\"AGENT_PROVIDER_UNAVAILABLE\",\"retryable\":true}}")),
		TPair<int32, FString>(500, TEXT("<html>gateway</html>")) });

	TOutcome<FCrowdyCppStudioModelsResult> Funds;
	IssueModels(*Client, 42, Funds);
	TestTrue(TEXT("the 402 is answered"), PollUntilDone(*Client, Funds));
	TestFalse(TEXT("the 402 fails"), Funds.Result.bOk);
	TestEqual(TEXT("the 402 status is kept"), Funds.Result.HttpStatus, 402);
	TestEqual(TEXT("the 402 code is read"), Funds.Result.ErrorCode, FString(TEXT("AGENT_FUNDS_NEEDED")));
	TestEqual(TEXT("the 402 message is read"), Funds.Result.ErrorMessage, FString(TEXT("Add funds to continue.")));
	TestFalse(TEXT("the 402 is not retryable"), Funds.Result.bRetryable);

	TOutcome<FCrowdyCppStudioModelsResult> Provider;
	IssueModels(*Client, 42, Provider);
	TestTrue(TEXT("the 503 is answered"), PollUntilDone(*Client, Provider));
	TestEqual(TEXT("the 503 status is kept"), Provider.Result.HttpStatus, 503);
	TestEqual(TEXT("the 503 code is read"), Provider.Result.ErrorCode, FString(TEXT("AGENT_PROVIDER_UNAVAILABLE")));
	TestTrue(TEXT("the 503 is retryable"), Provider.Result.bRetryable);

	TOutcome<FCrowdyCppStudioModelsResult> Gateway;
	IssueModels(*Client, 42, Gateway);
	TestTrue(TEXT("the 500 is answered"), PollUntilDone(*Client, Gateway));
	TestFalse(TEXT("the 500 fails"), Gateway.Result.bOk);
	TestTrue(TEXT("a body that is not an error object leaves no code"), Gateway.Result.ErrorCode.IsEmpty());
	TestEqual(TEXT("the status names the failure"), Gateway.Result.ErrorMessage, FString(TEXT("Server returned HTTP 500")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsForgedTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.RefusesOversizedAndMalformedBodies", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsForgedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	const FString Oversized = TEXT("{\"object\":\"list\",\"data\":[],\"pad\":\"")
		+ FString::ChrN(CrowdyCppStudioStopgaps::MaxModelsBodyBytes, TEXT('x')) + TEXT("\"}");
	const FString Deep = TEXT("{\"data\":") + FString::ChrN(512, TEXT('[')) + FString::ChrN(512, TEXT(']')) + TEXT("}");
	const TArray<FString> Bodies = { Oversized, TEXT("not json"), TEXT("{\"data\":{}}"), TEXT("[]"), Deep };

	for (int32 Index = 0; Index < Bodies.Num(); ++Index)
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Bodies[Index]);
		if (!TestTrue(FString::Printf(TEXT("body %d: a test client is made"), Index), Client.IsValid()))
		{
			continue;
		}
		TOutcome<FCrowdyCppStudioModelsResult> Out;
		IssueModels(*Client, 42, Out);
		TestTrue(FString::Printf(TEXT("body %d is answered"), Index), PollUntilDone(*Client, Out));
		TestFalse(FString::Printf(TEXT("body %d is refused"), Index), Out.Result.bOk);
		TestEqual(FString::Printf(TEXT("body %d keeps its status"), Index), Out.Result.HttpStatus, 200);
		TestEqual(FString::Printf(TEXT("body %d yields no models"), Index), Out.Result.Models.Num(), 0);
	}

	FString Many = TEXT("{\"data\":[{\"id\":\"") + FString::ChrN(257, TEXT('a')) + TEXT("\"}");
	for (int32 Index = 0; Index < FCrowdyCppClient::MaxStudioModels + 10; ++Index)
	{
		Many += FString::Printf(TEXT(",{\"id\":\"m%d\"}"), Index);
	}
	Many += TEXT("]}");
	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(Many);
	if (!TestTrue(TEXT("a long-list client is made"), Client.IsValid()))
	{
		return false;
	}
	TOutcome<FCrowdyCppStudioModelsResult> Out;
	IssueModels(*Client, 42, Out);
	TestTrue(TEXT("the long list is answered"), PollUntilDone(*Client, Out));
	TestTrue(TEXT("the long list reads"), Out.Result.bOk);
	TestEqual(TEXT("at most MaxStudioModels are kept"), Out.Result.Models.Num(), FCrowdyCppClient::MaxStudioModels);
	TestTrue(TEXT("an over-long id is skipped"), Out.Result.Models.Num() > 0 && Out.Result.Models[0].Id == TEXT("m0"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsRefusalTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.RefusesWithoutATokenAndCancelsAtClose", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsRefusalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	{
		const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"object\":\"list\",\"data\":[]}"));
		if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
		{
			return false;
		}
		Client->SetGameToken(FString());
		TOutcome<FCrowdyCppStudioModelsResult> Out;
		IssueModels(*Client, 42, Out);
		TestTrue(TEXT("with no app token the answer comes at once"), Out.bDoneBeforePoll);
		TestFalse(TEXT("with no app token it fails"), Out.Result.bOk);
		FString Url;
		FString Authorization;
		TestFalse(TEXT("with no app token nothing is sent"), Client->GetLastTestRequest(Url, Authorization));

		TOutcome<FCrowdyCppStudioModelsResult> NoApp;
		Client->SetGameToken(AppBearer);
		IssueModels(*Client, 0, NoApp);
		TestTrue(TEXT("with no app id the answer comes at once"), NoApp.bDoneBeforePoll);
		TestFalse(TEXT("with no app id nothing is sent"), Client->GetLastTestRequest(Url, Authorization));
	}

	const TSharedPtr<FCrowdyCppClient> Client = MakeClient(TEXT("{\"object\":\"list\",\"data\":[]}"));
	if (!TestTrue(TEXT("a second test client is made"), Client.IsValid()))
	{
		return false;
	}
	TOutcome<FCrowdyCppStudioModelsResult> Out;
	IssueModels(*Client, 42, Out);
	TestEqual(TEXT("one request is pending"), Client->NumPendingRequests(), 1);
	Client->Close();
	TestEqual(TEXT("closing delivers it"), Out.Deliveries, 1);
	TestEqual(TEXT("as canceled"), Out.Result.ErrorMessage, FCrowdyCppClient::CanceledErrorMessage());
	Client->Poll();
	TestEqual(TEXT("the answer that was already queued is dropped"), Out.Deliveries, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioDiagnosticsHumanTest,
	"CrowdySDK.CrowdyCppBridge.StudioDiagnostics.ReadsRustcHumanOutput", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioDiagnosticsHumanTest::RunTest(const FString& Parameters)
{
	const FString Output = TEXT("   Compiling app v0.1.0\r\n")
		TEXT("error[E0425]: cannot find value `x` in this scope\n")
		TEXT("  --> server/src/logic.rs:12:5\n")
		TEXT("   |\n")
		TEXT("warning: unused import\n")
		TEXT("  --> /tmp/build/src/lib.rs:1:5\n");
	const TArray<FCrowdyCppStudioDiagnostic> Diagnostics =
		CrowdyCppStudio::ParseRustcDiagnostics(Output, ECrowdyCppStudioTarget::Client);
	if (!TestEqual(TEXT("two diagnostics"), Diagnostics.Num(), 2))
	{
		return false;
	}
	const FCrowdyCppStudioDiagnostic& Error = Diagnostics[0];
	TestTrue(TEXT("a server/ path names its own target"), Error.Target == ECrowdyCppStudioTarget::Server);
	TestEqual(TEXT("the path is crate-relative"), Error.Path, FString(TEXT("src/logic.rs")));
	TestEqual(TEXT("line"), Error.Line, 12);
	TestEqual(TEXT("column"), Error.Column, 5);
	TestEqual(TEXT("no end line"), Error.EndLine, 0);
	TestTrue(TEXT("severity error"), Error.Severity == ECrowdyCppStudioDiagnosticSeverity::Error);
	TestEqual(TEXT("message"), Error.Message, FString(TEXT("cannot find value `x` in this scope")));
	TestEqual(TEXT("code"), Error.Code, FString(TEXT("E0425")));
	TestTrue(TEXT("source rustc"), Error.Source == ECrowdyCppStudioDiagnosticSource::Rustc);

	const FCrowdyCppStudioDiagnostic& Warning = Diagnostics[1];
	TestTrue(TEXT("any other path gets the default target"), Warning.Target == ECrowdyCppStudioTarget::Client);
	TestEqual(TEXT("a build path is cut to src/"), Warning.Path, FString(TEXT("src/lib.rs")));
	TestTrue(TEXT("severity warning"), Warning.Severity == ECrowdyCppStudioDiagnosticSeverity::Warning);
	TestTrue(TEXT("no code"), Warning.Code.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioDiagnosticsJsonTest,
	"CrowdySDK.CrowdyCppBridge.StudioDiagnostics.ReadsRustcJsonLines", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioDiagnosticsJsonTest::RunTest(const FString& Parameters)
{
	const FString Output = TEXT("{\"$message_type\":\"diagnostic\",\"message\":\"unused variable: `y`\",\"code\":{\"code\":\"unused_variables\",\"explanation\":null},")
		TEXT("\"level\":\"warning\",\"spans\":[{\"file_name\":\"client/src/lib.rs\",\"line_start\":3,\"line_end\":4,\"column_start\":9,\"column_end\":10,\"is_primary\":true},")
		TEXT("{\"file_name\":\"client/src/other.rs\",\"line_start\":1,\"column_start\":1,\"is_primary\":false}]}\n")
		TEXT("{not json\n");
	const TArray<FCrowdyCppStudioDiagnostic> Diagnostics =
		CrowdyCppStudio::ParseRustcDiagnostics(Output, ECrowdyCppStudioTarget::Server);
	if (!TestEqual(TEXT("one diagnostic from the primary span; the broken line is skipped"), Diagnostics.Num(), 1))
	{
		return false;
	}
	const FCrowdyCppStudioDiagnostic& Diagnostic = Diagnostics[0];
	TestTrue(TEXT("a client/ path names its own target"), Diagnostic.Target == ECrowdyCppStudioTarget::Client);
	TestEqual(TEXT("path"), Diagnostic.Path, FString(TEXT("src/lib.rs")));
	TestEqual(TEXT("line"), Diagnostic.Line, 3);
	TestEqual(TEXT("column"), Diagnostic.Column, 9);
	TestEqual(TEXT("end line"), Diagnostic.EndLine, 4);
	TestEqual(TEXT("end column"), Diagnostic.EndColumn, 10);
	TestTrue(TEXT("severity warning"), Diagnostic.Severity == ECrowdyCppStudioDiagnosticSeverity::Warning);
	TestEqual(TEXT("message"), Diagnostic.Message, FString(TEXT("unused variable: `y`")));
	TestEqual(TEXT("code"), Diagnostic.Code, FString(TEXT("unused_variables")));
	TestTrue(TEXT("source rustc"), Diagnostic.Source == ECrowdyCppStudioDiagnosticSource::Rustc);

	TestEqual(TEXT("empty output yields nothing"), CrowdyCppStudio::ParseRustcDiagnostics(FString(), ECrowdyCppStudioTarget::Server).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppStudioModelsHttpsTest,
	"CrowdySDK.CrowdyCppBridge.StudioModels.TheTokenGoesOnlyOverHttps", CrowdyCppStudioAgentTests::TestFlags)
bool FCrowdyCppStudioModelsHttpsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppStudioAgentTests;
	AddExpectedErrorPlain(TEXT("The Game API endpoint is not encrypted"), EAutomationExpectedErrorFlags::Contains, 0);
	const auto Ask = [this](const TCHAR* ApiUrl, bool& bOutSent) -> FCrowdyCppStudioModelsResult
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = ApiUrl;
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"object\":\"list\",\"data\":[]}"), 200, Config);
		TOutcome<FCrowdyCppStudioModelsResult> Out;
		if (!TestTrue(TEXT("a test client is made"), Client.IsValid()))
		{
			return Out.Result;
		}
		Client->SetGameToken(AppBearer);
		IssueModels(*Client, 42, Out);
		PollUntilDone(*Client, Out);
		FString Url;
		FString Authorization;
		bOutSent = Client->GetLastTestRequest(Url, Authorization);
		return Out.Result;
	};

	bool bSent = false;
	const FCrowdyCppStudioModelsResult Plain = Ask(TEXT("http://game.test"), bSent);
	TestFalse(TEXT("plain http to another host is refused"), Plain.bOk);
	TestFalse(TEXT("and nothing was sent"), bSent);
	TestTrue(TEXT("with a message that says why"), Plain.ErrorMessage.Contains(TEXT("https")));

	const FCrowdyCppStudioModelsResult Secure = Ask(TEXT("https://game.test"), bSent);
	TestTrue(TEXT("https is sent"), Secure.bOk && bSent);

	const FCrowdyCppStudioModelsResult Loopback = Ask(TEXT("http://localhost:8080"), bSent);
#if UE_BUILD_SHIPPING
	TestFalse(TEXT("loopback http is refused in Shipping"), Loopback.bOk);
#else
	TestTrue(TEXT("loopback http is allowed outside Shipping"), Loopback.bOk && bSent);
	const FCrowdyCppStudioModelsResult Lookalike = Ask(TEXT("http://localhost.evil.test"), bSent);
	TestFalse(TEXT("a host that only starts with localhost is refused"), Lookalike.bOk);
#endif
	return true;
}

#endif
