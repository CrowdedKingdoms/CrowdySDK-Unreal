#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyExecBuildAndDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CrowdyExecDeveloperApiTests
{
	using namespace CrowdyExecDeveloper;

	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* const GameBearer = TEXT("game-bearer-token");
	const TCHAR* const SessionBearer = TEXT("session-bearer-token");

	// Past 2^53, so an id sent as a JSON number would arrive changed.
	constexpr int64 TestAppId = 9007199254740993;
	const TCHAR* const TestAppIdText = TEXT("9007199254740993");

	const TCHAR* const BusyBody =
		TEXT("{\"errors\":[{\"message\":\"The platform is busy.\",\"extensions\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":true}}],\"data\":null}");

	struct FRig
	{
		explicit FRig(const FString& ApiUrl = FString())
		{
			FCrowdyCppClientConfig Config;
			if (!ApiUrl.IsEmpty())
			{
				Config.ApiUrl = ApiUrl;
				Config.DiscoveryUrl = ApiUrl;
			}
			Bridge = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
			if (!Bridge.IsValid())
			{
				return;
			}
			Bridge->SetGameToken(GameBearer);
			Bridge->SetManagementToken(SessionBearer);
			Bridge->SetTestOnRequest([Counter = Sends, Sent = SentBodies, Raw = Bridge.Get()](const FString&)
			{
				++*Counter;
				FString Body;
				Raw->GetLastTestRequestBody(Body);
				Sent->Add(MoveTemp(Body));
			});
			Client = FClient::CreateForClient(Bridge.ToSharedRef(), TestAppId);
			Client->SetBusyWaitsForTest({0.f, 0.f});
		}

		FRig(const FRig&) = delete;
		FRig& operator=(const FRig&) = delete;

		bool IsValid() const { return Client.IsValid(); }

		void Script(std::initializer_list<const TCHAR*> Bodies)
		{
			TArray<TPair<int32, FString>> Responses;
			for (const TCHAR* Body : Bodies)
			{
				Responses.Emplace(200, Body);
			}
			Bridge->SetTestResponseScript(MoveTemp(Responses));
		}

		void ScriptWithStatus(TArray<TPair<int32, FString>> Responses)
		{
			Bridge->SetTestResponseScript(MoveTemp(Responses));
		}

		FString LastUrl() const
		{
			FString Url;
			FString Authorization;
			Bridge->GetLastTestRequest(Url, Authorization);
			return Url;
		}

		bool PumpUntil(TFunctionRef<bool()> Done)
		{
			const double Until = FPlatformTime::Seconds() + 5.0;
			while (!Done())
			{
				if (FPlatformTime::Seconds() > Until)
				{
					return false;
				}
				Bridge->Poll();
				FTSTicker::GetCoreTicker().Tick(0.01f);
				FPlatformProcess::Sleep(0.002f);
			}
			return true;
		}

		TSharedPtr<FJsonObject> LastRequest() const
		{
			FString Body;
			Bridge->GetLastTestRequestBody(Body);
			TSharedPtr<FJsonObject> Request;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request);
			return Request;
		}

		FString LastAuthorization() const
		{
			FString Url;
			FString Authorization;
			Bridge->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}

		TSharedRef<int32> Sends = MakeShared<int32>(0);
		/** Every request body, in the order sent. */
		TSharedRef<TArray<FString>> SentBodies = MakeShared<TArray<FString>>();
		TSharedPtr<FCrowdyCppClient> Bridge;
		TSharedPtr<FClient> Client;
	};

	template <typename T>
	struct TCapture
	{
		bool bDone = false;
		TResult<T> Result;
	};

	template <typename T>
	TFunction<void(const TResult<T>&)> Into(const TSharedRef<TCapture<T>>& Capture)
	{
		return [Capture](const TResult<T>& Result)
		{
			Capture->Result = Result;
			Capture->bDone = true;
		};
	}

	template <typename T>
	bool Await(FRig& Rig, const TSharedRef<TCapture<T>>& Capture)
	{
		return Rig.PumpUntil([Capture] { return Capture->bDone; });
	}

	// Waits for the call Issue makes to be answered, then hands back the variables it sent (null if it never was).
	template <typename FIssue>
	TSharedPtr<FJsonObject> SentVariables(FRig& Rig, const FIssue& Issue)
	{
		const TSharedRef<bool> bDone = MakeShared<bool>(false);
		Issue([bDone](const auto&) { *bDone = true; });
		if (!Rig.PumpUntil([bDone] { return *bDone; }))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Request = Rig.LastRequest();
		const TSharedPtr<FJsonObject>* Variables = nullptr;
		return Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Variables) ? *Variables : TSharedPtr<FJsonObject>();
	}

	FString OperationOf(const FRig& Rig)
	{
		const TSharedPtr<FJsonObject> Request = Rig.LastRequest();
		FString Name;
		if (Request.IsValid())
		{
			Request->TryGetStringField(TEXT("operationName"), Name);
		}
		return Name;
	}

	TSharedPtr<FJsonObject> InputOf(const TSharedPtr<FJsonObject>& Variables)
	{
		const TSharedPtr<FJsonObject>* Input = nullptr;
		return Variables.IsValid() && Variables->TryGetObjectField(TEXT("input"), Input) ? *Input : TSharedPtr<FJsonObject>();
	}
}

// Every call names its operation, sends the app id as a string and carries the developer's session bearer, never the game one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperRequestsTest, "CrowdySDK.CrowdyExecEditor.DeveloperRequestsNameOperationAndVariables",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperRequestsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}

	auto CheckCommon = [this, &Rig](const TCHAR* Operation, const TSharedPtr<FJsonObject>& AppIdHolder)
	{
		TestEqual(FString::Printf(TEXT("%s is sent under its name"), Operation), OperationOf(Rig), FString(Operation));
		TestTrue(FString::Printf(TEXT("%s carries the session bearer"), Operation), Rig.LastAuthorization().Contains(SessionBearer));
		TestFalse(FString::Printf(TEXT("%s does not carry the game bearer"), Operation), Rig.LastAuthorization().Contains(GameBearer));
		if (!TestTrue(FString::Printf(TEXT("%s sends appId as a string"), Operation),
			AppIdHolder.IsValid() && AppIdHolder->HasTypedField<EJson::String>(TEXT("appId"))))
		{
			return;
		}
		TestEqual(FString::Printf(TEXT("%s sends the app id exactly"), Operation), AppIdHolder->GetStringField(TEXT("appId")), FString(TestAppIdText));
	};

	TSharedPtr<FJsonObject> Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Starters(Done); });
	CheckCommon(TEXT("ExecStarters"), Vars);

	FBuildCrate Crate;
	Crate.Name = TEXT("sbx_counter");
	Crate.Files.Add(FBuildFile{TEXT("Cargo.toml"), TEXT("[package]")});
	Vars = SentVariables(Rig, [&Rig, &Crate](auto Done) { Rig.Client->Build({Crate}, Done); });
	const TSharedPtr<FJsonObject> BuildInput = InputOf(Vars);
	CheckCommon(TEXT("ExecBuild"), BuildInput);
	const TArray<TSharedPtr<FJsonValue>>* Crates = nullptr;
	if (TestTrue(TEXT("the build sends one crate"), BuildInput.IsValid() && BuildInput->TryGetArrayField(TEXT("crates"), Crates) && Crates->Num() == 1))
	{
		const TSharedPtr<FJsonObject> SentCrate = (*Crates)[0]->AsObject();
		TestEqual(TEXT("the crate keeps its name"), SentCrate->GetStringField(TEXT("name")), FString(TEXT("sbx_counter")));
		const TArray<TSharedPtr<FJsonValue>>& Files = SentCrate->GetArrayField(TEXT("files"));
		if (TestEqual(TEXT("the crate sends its file"), Files.Num(), 1))
		{
			TestEqual(TEXT("the file keeps its path"), Files[0]->AsObject()->GetStringField(TEXT("path")), FString(TEXT("Cargo.toml")));
			TestEqual(TEXT("the file keeps its content"), Files[0]->AsObject()->GetStringField(TEXT("content")), FString(TEXT("[package]")));
		}
	}

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->BuildStatus(TEXT("b-1"), Done); });
	CheckCommon(TEXT("ExecBuildStatus"), Vars);
	TestEqual(TEXT("the status read names the build"), Vars.IsValid() ? Vars->GetStringField(TEXT("buildId")) : FString(), FString(TEXT("b-1")));

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Deploy(TEXT("{\"root\":\"root\"}"), TEXT("b-1"), Done); });
	const TSharedPtr<FJsonObject> DeployInput = InputOf(Vars);
	CheckCommon(TEXT("ExecDeploy"), DeployInput);
	if (DeployInput.IsValid())
	{
		TestEqual(TEXT("the deploy sends the manifest"), DeployInput->GetStringField(TEXT("manifestJson")), FString(TEXT("{\"root\":\"root\"}")));
		TestEqual(TEXT("the deploy names the build"), DeployInput->GetStringField(TEXT("buildId")), FString(TEXT("b-1")));
		const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
		TestTrue(TEXT("the deploy uploads no modules"), DeployInput->TryGetArrayField(TEXT("artifacts"), Artifacts) && Artifacts->Num() == 0);
	}

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Versions(Done); });
	CheckCommon(TEXT("ExecVersions"), Vars);

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->ActivateVersion(3, Done); });
	CheckCommon(TEXT("ExecActivateVersion"), Vars);
	TestEqual(TEXT("the activation names the version"), Vars.IsValid() ? static_cast<int32>(Vars->GetNumberField(TEXT("version"))) : 0, 3);

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->SetEnabled(false, TEXT("sbx_counter"), Done); });
	CheckCommon(TEXT("ExecSetEnabled"), Vars);
	if (Vars.IsValid())
	{
		TestTrue(TEXT("switching off sends enabled false"), Vars->HasTypedField<EJson::Boolean>(TEXT("enabled")) && !Vars->GetBoolField(TEXT("enabled")));
		TestEqual(TEXT("switching one type off names it"), Vars->GetStringField(TEXT("nodeType")), FString(TEXT("sbx_counter")));
	}
	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->SetEnabled(true, FString(), Done); });
	TestTrue(TEXT("switching the whole app names no type"), Vars.IsValid() && !Vars->HasField(TEXT("nodeType")));

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Status(Done); });
	CheckCommon(TEXT("ExecAppStatus"), Vars);

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Instances(Done); });
	CheckCommon(TEXT("ExecInstances"), Vars);

	FLogQuery Query;
	Query.NodeType = TEXT("sbx_counter");
	Query.Flow = TEXT("0123456789abcdef0123456789abcdef");
	Query.MaxLevel = 2;
	Query.Limit = 50;
	Vars = SentVariables(Rig, [&Rig, &Query](auto Done) { Rig.Client->Logs(Query, Done); });
	CheckCommon(TEXT("ExecLogs"), Vars);
	if (Vars.IsValid())
	{
		TestEqual(TEXT("the logs filter by type"), Vars->GetStringField(TEXT("nodeType")), FString(TEXT("sbx_counter")));
		TestEqual(TEXT("the logs filter by flow"), Vars->GetStringField(TEXT("flow")), Query.Flow);
		TestEqual(TEXT("the logs send the level"), static_cast<int32>(Vars->GetNumberField(TEXT("maxLevel"))), 2);
		TestEqual(TEXT("the logs send the limit"), static_cast<int32>(Vars->GetNumberField(TEXT("limit"))), 50);
		TestFalse(TEXT("an unset key is left out"), Vars->HasField(TEXT("key")));
		TestFalse(TEXT("an unset before is left out"), Vars->HasField(TEXT("before")));
	}
	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->Logs(FLogQuery(), Done); });
	TestTrue(TEXT("an empty log query sends only the app"), Vars.IsValid() && Vars->Values.Num() == 1);

	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->EndpointStats(TEXT("sbx_counter"), 60, Done); });
	CheckCommon(TEXT("ExecEndpointStats"), Vars);
	if (Vars.IsValid())
	{
		TestEqual(TEXT("the stats filter by type"), Vars->GetStringField(TEXT("nodeType")), FString(TEXT("sbx_counter")));
		TestEqual(TEXT("the stats send the window"), static_cast<int32>(Vars->GetNumberField(TEXT("sinceMinutes"))), 60);
	}
	Vars = SentVariables(Rig, [&Rig](auto Done) { Rig.Client->EndpointStats(FString(), 0, Done); });
	TestTrue(TEXT("stats with no filter send only the app"), Vars.IsValid() && Vars->Values.Num() == 1);
	return true;
}

// Each answer is read into its struct, nullable fields included.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperAnswersTest, "CrowdySDK.CrowdyExecEditor.DeveloperAnswersParse",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperAnswersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"data\":{\"execBuild\":{\"buildId\":\"b-9\",\"status\":\"succeeded\",\"kind\":\"exec\",\"log\":\"Compiling sbx_counter\",\"createdAt\":\"2026-09-29T10:00:00Z\",\"startedAt\":null,\"finishedAt\":null,\"artifacts\":[{\"crate\":\"sbx_counter\",\"digest\":\"ab12\",\"sizeBytes\":4096,\"capabilitySummaryJson\":null,\"capabilityHash\":null,\"tickIntervalMs\":null}]}}}"),
		TEXT("{\"data\":{\"execVersions\":[{\"version\":4,\"createdBy\":\"user:1\",\"createdAt\":\"2026-09-29T10:01:02.500Z\",\"types\":3,\"active\":true,\"manifestJson\":\"{}\"},{\"version\":3,\"createdBy\":null,\"createdAt\":\"2026-09-28T09:00:00Z\",\"types\":4,\"active\":false,\"manifestJson\":null}]}}"),
		TEXT("{\"data\":{\"execAppStatus\":{\"activeVersion\":null,\"disabled\":false,\"disabledTypes\":[\"sbx_locker\"],\"budgetPaused\":true}}}"),
		TEXT("{\"data\":{\"execLogs\":[{\"id\":\"l-2\",\"nodeType\":\"sbx_counter\",\"key\":\"village\",\"level\":2,\"host\":\"h1\",\"at\":\"2026-09-29T10:02:00Z\",\"text\":\"add 1 by Ada\",\"flow\":\"0123456789abcdef0123456789abcdef\"},{\"id\":\"l-1\",\"nodeType\":\"sbx_counter\",\"key\":\"village\",\"level\":3,\"host\":\"h1\",\"at\":\"2026-09-29T10:01:00Z\",\"text\":\"started\",\"flow\":null}]}}"),
		TEXT("{\"data\":{\"execEndpointStats\":[{\"nodeType\":\"sbx_counter\",\"method\":\"add\",\"calls\":10,\"appErrors\":1,\"busy\":2,\"denied\":0,\"deadlineExceeded\":0,\"otherErrors\":0,\"timedCalls\":8,\"latencyMsAvg\":1.5,\"latencyMsMax\":4,\"firstMinute\":\"2026-09-29T10:00:00Z\",\"lastMinute\":\"2026-09-29T10:05:00Z\"},{\"nodeType\":\"sbx_counter\",\"method\":\"$spawn\",\"calls\":1,\"appErrors\":0,\"busy\":0,\"denied\":1,\"deadlineExceeded\":0,\"otherErrors\":0,\"timedCalls\":0,\"latencyMsAvg\":null,\"latencyMsMax\":null,\"firstMinute\":\"2026-09-29T10:00:00Z\",\"lastMinute\":\"2026-09-29T10:00:00Z\"}]}}"),
		TEXT("{\"data\":{\"execInstances\":[{\"instanceId\":\"i-1\",\"nodeType\":\"sbx_counter\",\"key\":\"village\",\"kind\":\"hub\",\"phase\":\"idle\",\"host\":null,\"epoch\":5,\"sinceMs\":1200,\"heldBack\":null}]}}"),
		TEXT("{\"data\":{\"execStarters\":{\"manifestJson\":\"{\\\"root\\\":\\\"world\\\"}\",\"starters\":[{\"crate\":\"world\",\"nodeType\":\"world\",\"description\":\"The root.\",\"files\":[{\"path\":\"src/lib.rs\",\"content\":\"// world\"}]}]}}}"),
	});

	const TSharedRef<TCapture<FBuild>> Built = MakeShared<TCapture<FBuild>>();
	Rig.Client->Build({}, Into(Built));
	if (TestTrue(TEXT("the build is answered"), Await(Rig, Built)) && TestTrue(Built->Result.Error.Message, Built->Result.bOk))
	{
		const FBuild& Build = Built->Result.Value;
		TestEqual(TEXT("build id"), Build.BuildId, FString(TEXT("b-9")));
		TestTrue(TEXT("the build succeeded"), Build.IsSucceeded());
		TestEqual(TEXT("build log"), Build.Log, FString(TEXT("Compiling sbx_counter")));
		if (TestEqual(TEXT("one artifact"), Build.Artifacts.Num(), 1))
		{
			TestEqual(TEXT("artifact crate"), Build.Artifacts[0].Crate, FString(TEXT("sbx_counter")));
			TestEqual(TEXT("artifact size"), Build.Artifacts[0].SizeBytes, static_cast<int64>(4096));
		}
	}

	const TSharedRef<TCapture<TArray<FVersion>>> Versions = MakeShared<TCapture<TArray<FVersion>>>();
	Rig.Client->Versions(Into(Versions));
	if (TestTrue(TEXT("the versions are answered"), Await(Rig, Versions)) && TestTrue(Versions->Result.Error.Message, Versions->Result.bOk)
		&& TestEqual(TEXT("two versions"), Versions->Result.Value.Num(), 2))
	{
		const FVersion& Newest = Versions->Result.Value[0];
		TestEqual(TEXT("newest version"), Newest.Version, 4);
		TestTrue(TEXT("newest is active"), Newest.bActive);
		TestEqual(TEXT("newest types"), Newest.Types, 3);
		TestEqual(TEXT("newest by"), Newest.CreatedBy, FString(TEXT("user:1")));
		TestEqual(TEXT("newest when"), Newest.CreatedAt, FDateTime(2026, 9, 29, 10, 1, 2, 500));
		TestTrue(TEXT("a null author and manifest read as empty"), Versions->Result.Value[1].CreatedBy.IsEmpty() && Versions->Result.Value[1].ManifestJson.IsEmpty());
	}

	const TSharedRef<TCapture<FAppStatus>> Status = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Status));
	if (TestTrue(TEXT("the status is answered"), Await(Rig, Status)) && TestTrue(Status->Result.Error.Message, Status->Result.bOk))
	{
		TestFalse(TEXT("no active version before the first deploy"), Status->Result.Value.ActiveVersion.IsSet());
		TestEqual(TEXT("disabled types"), Status->Result.Value.DisabledTypes, TArray<FString>({TEXT("sbx_locker")}));
		TestTrue(TEXT("budget paused"), Status->Result.Value.bBudgetPaused);
		TestFalse(TEXT("the app is on"), Status->Result.Value.bDisabled);
	}

	const TSharedRef<TCapture<TArray<FLogLine>>> Logs = MakeShared<TCapture<TArray<FLogLine>>>();
	Rig.Client->Logs(FLogQuery(), Into(Logs));
	if (TestTrue(TEXT("the logs are answered"), Await(Rig, Logs)) && TestTrue(Logs->Result.Error.Message, Logs->Result.bOk)
		&& TestEqual(TEXT("two lines"), Logs->Result.Value.Num(), 2))
	{
		TestEqual(TEXT("a call's line keeps its flow"), Logs->Result.Value[0].Flow, FString(TEXT("0123456789abcdef0123456789abcdef")));
		TestEqual(TEXT("line text"), Logs->Result.Value[0].Text, FString(TEXT("add 1 by Ada")));
		TestEqual(TEXT("line level"), Logs->Result.Value[0].Level, 2);
		TestTrue(TEXT("a line outside a call has no flow"), Logs->Result.Value[1].Flow.IsEmpty());
	}

	const TSharedRef<TCapture<TArray<FEndpointStat>>> Stats = MakeShared<TCapture<TArray<FEndpointStat>>>();
	Rig.Client->EndpointStats(FString(), 0, Into(Stats));
	if (TestTrue(TEXT("the stats are answered"), Await(Rig, Stats)) && TestTrue(Stats->Result.Error.Message, Stats->Result.bOk)
		&& TestEqual(TEXT("two endpoints"), Stats->Result.Value.Num(), 2))
	{
		const FEndpointStat& Timed = Stats->Result.Value[0];
		TestEqual(TEXT("method"), Timed.Method, FString(TEXT("add")));
		TestEqual(TEXT("calls"), Timed.Calls, 10.0);
		TestEqual(TEXT("busy"), Timed.Busy, 2.0);
		TestTrue(TEXT("a timed endpoint has its average"), Timed.LatencyMsAvg.IsSet() && Timed.LatencyMsAvg.GetValue() == 1.5);
		TestFalse(TEXT("an untimed endpoint has no average"), Stats->Result.Value[1].LatencyMsAvg.IsSet());
		TestFalse(TEXT("an untimed endpoint has no max"), Stats->Result.Value[1].LatencyMsMax.IsSet());
	}

	const TSharedRef<TCapture<TArray<FInstance>>> Instances = MakeShared<TCapture<TArray<FInstance>>>();
	Rig.Client->Instances(Into(Instances));
	if (TestTrue(TEXT("the instances are answered"), Await(Rig, Instances)) && TestTrue(Instances->Result.Error.Message, Instances->Result.bOk)
		&& TestEqual(TEXT("one instance"), Instances->Result.Value.Num(), 1))
	{
		TestTrue(TEXT("an idle instance has no host"), Instances->Result.Value[0].Host.IsEmpty());
		TestEqual(TEXT("epoch"), Instances->Result.Value[0].Epoch, static_cast<int64>(5));
	}

	const TSharedRef<TCapture<FStarterPack>> Pack = MakeShared<TCapture<FStarterPack>>();
	Rig.Client->Starters(Into(Pack));
	if (TestTrue(TEXT("the starters are answered"), Await(Rig, Pack)) && TestTrue(Pack->Result.Error.Message, Pack->Result.bOk)
		&& TestEqual(TEXT("one starter"), Pack->Result.Value.Starters.Num(), 1))
	{
		const FStarter& Starter = Pack->Result.Value.Starters[0];
		TestEqual(TEXT("the pack's manifest"), Pack->Result.Value.ManifestJson, FString(TEXT("{\"root\":\"world\"}")));
		TestEqual(TEXT("the starter's crate"), Starter.Crate.Name, FString(TEXT("world")));
		TestTrue(TEXT("the starter's file"), Starter.Crate.Files.Num() == 1 && Starter.Crate.Files[0].Path == TEXT("src/lib.rs"));
	}
	return true;
}

// A deploy the platform refuses as busy is sent again after the first wait and succeeds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperBusyDeployTest, "CrowdySDK.CrowdyExecEditor.DeveloperBusyDeployRetriesOnce",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperBusyDeployTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({BusyBody, TEXT("{\"data\":{\"execDeploy\":{\"version\":7}}}")});

	const TSharedRef<TCapture<int32>> Deployed = MakeShared<TCapture<int32>>();
	Rig.Client->Deploy(TEXT("{}"), TEXT("b-1"), Into(Deployed));
	if (!TestTrue(TEXT("the deploy is answered"), Await(Rig, Deployed)))
	{
		return false;
	}
	TestTrue(Deployed->Result.Error.Message, Deployed->Result.bOk);
	TestEqual(TEXT("the new version"), Deployed->Result.Value, 7);
	TestEqual(TEXT("sent twice"), *Rig.Sends, 2);
	return true;
}

// A change refused as busy every time ends in the refusal after the last wait.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperBusyToTheEndTest, "CrowdySDK.CrowdyExecEditor.DeveloperBusyEveryTryEndsInError",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperBusyToTheEndTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({BusyBody, BusyBody, BusyBody, BusyBody, BusyBody});

	const TSharedRef<TCapture<FAppStatus>> Switched = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->SetEnabled(false, TEXT("sbx_counter"), Into(Switched));
	if (!TestTrue(TEXT("the switch is answered"), Await(Rig, Switched)))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), Switched->Result.bOk);
	TestEqual(TEXT("with the busy code"), Switched->Result.Error.Code, FString(TEXT("PLATFORM_BUSY")));
	TestEqual(TEXT("sent once and once per wait"), *Rig.Sends, 3);
	return true;
}

// A null build status, a missing field and a refused call each end in an error, never a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperErrorsTest, "CrowdySDK.CrowdyExecEditor.DeveloperBadAnswersAreErrors",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperErrorsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"data\":{\"execBuildStatus\":null}}"),
		TEXT("{\"data\":{\"execDeploy\":{}}}"),
		TEXT("{\"data\":{\"execVersions\":[{\"version\":\"four\"}]}}"),
		TEXT("{\"errors\":[{\"message\":\"Missing permission manage_compute.\",\"extensions\":{\"code\":\"FORBIDDEN\"}}],\"data\":null}"),
	});

	const TSharedRef<TCapture<FBuild>> Missing = MakeShared<TCapture<FBuild>>();
	Rig.Client->BuildStatus(TEXT("b-404"), Into(Missing));
	if (TestTrue(TEXT("the status read is answered"), Await(Rig, Missing)))
	{
		TestFalse(TEXT("an unknown build is an error"), Missing->Result.bOk);
		TestTrue(TEXT("naming the build"), Missing->Result.Error.Message.Contains(TEXT("b-404")));
	}

	const TSharedRef<TCapture<int32>> Deployed = MakeShared<TCapture<int32>>();
	Rig.Client->Deploy(TEXT("{}"), TEXT("b-1"), Into(Deployed));
	if (TestTrue(TEXT("the deploy is answered"), Await(Rig, Deployed)))
	{
		TestFalse(TEXT("a deploy with no version is an error"), Deployed->Result.bOk);
		TestTrue(TEXT("naming the operation"), Deployed->Result.Error.Message.Contains(TEXT("ExecDeploy")));
	}

	const TSharedRef<TCapture<TArray<FVersion>>> Versions = MakeShared<TCapture<TArray<FVersion>>>();
	Rig.Client->Versions(Into(Versions));
	if (TestTrue(TEXT("the versions are answered"), Await(Rig, Versions)))
	{
		TestFalse(TEXT("a mistyped field is an error"), Versions->Result.bOk);
		TestTrue(TEXT("naming the operation"), Versions->Result.Error.Message.Contains(TEXT("ExecVersions")));
	}

	const TSharedRef<TCapture<FAppStatus>> Status = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Status));
	if (TestTrue(TEXT("the status is answered"), Await(Rig, Status)))
	{
		TestFalse(TEXT("a refused call is an error"), Status->Result.bOk);
		TestEqual(TEXT("carrying the platform's code"), Status->Result.Error.Code, FString(TEXT("FORBIDDEN")));
		TestTrue(TEXT("and its message"), Status->Result.Error.Message.Contains(TEXT("manage_compute")));
	}
	return true;
}

// Waiting on a build reads its status until it finishes, showing each answer on the way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperWaitForBuildTest, "CrowdySDK.CrowdyExecEditor.DeveloperWaitForBuildFinishes",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperWaitForBuildTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"data\":{\"execBuildStatus\":{\"buildId\":\"b-1\",\"status\":\"building\",\"kind\":\"exec\",\"log\":null,\"createdAt\":\"2026-09-29T10:00:00Z\",\"artifacts\":[]}}}"),
		TEXT("{\"data\":{\"execBuildStatus\":{\"buildId\":\"b-1\",\"status\":\"succeeded\",\"kind\":\"exec\",\"log\":\"Finished\",\"createdAt\":\"2026-09-29T10:00:00Z\",\"artifacts\":[]}}}"),
	});

	const TSharedRef<TCapture<FBuild>> Finished = MakeShared<TCapture<FBuild>>();
	const TSharedRef<TArray<FString>> Seen = MakeShared<TArray<FString>>();
	Rig.Client->WaitForBuild(TEXT("b-1"), Into(Finished), [Seen](const FBuild& Build) { Seen->Add(Build.Status); }, 0.f, 60.f);
	if (!TestTrue(TEXT("the wait ends"), Await(Rig, Finished)))
	{
		return false;
	}
	TestTrue(Finished->Result.Error.Message, Finished->Result.bOk);
	TestTrue(TEXT("it ends on success"), Finished->Result.Value.IsSucceeded());
	TestEqual(TEXT("with the log"), Finished->Result.Value.Log, FString(TEXT("Finished")));
	TestEqual(TEXT("each answer was shown"), *Seen, TArray<FString>({TEXT("building"), TEXT("succeeded")}));
	TestEqual(TEXT("the status was read twice"), *Rig.Sends, 2);

	const TSharedRef<TCapture<FBuild>> TimedOut = MakeShared<TCapture<FBuild>>();
	Rig.Client->WaitForBuild(TEXT("b-2"), Into(TimedOut), nullptr, 0.f, 0.f);
	if (TestTrue(TEXT("a wait with no time left ends"), Await(Rig, TimedOut)))
	{
		TestFalse(TEXT("as an error"), TimedOut->Result.bOk);
		TestTrue(TEXT("saying the build did not finish"), TimedOut->Result.Error.Message.Contains(TEXT("did not finish")));
		TestEqual(TEXT("without reading the status"), *Rig.Sends, 2);
	}
	return true;
}

// A status read refused as busy, or lost on the way, is asked again at the next interval rather than ending the wait.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperWaitSurvivesBusyTest, "CrowdySDK.CrowdyExecEditor.DeveloperWaitForBuildSurvivesBusyAndLostReads",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperWaitSurvivesBusyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	// Not retryable, so the connection hands the busy answer straight back rather than retrying it itself.
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, TEXT("{\"errors\":[{\"message\":\"The platform is busy.\",\"extensions\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":false}}],\"data\":null}"));
	Responses.Emplace(504, TEXT("gateway timeout"));
	Responses.Emplace(200, TEXT("{\"data\":{\"execBuildStatus\":{\"buildId\":\"b-1\",\"status\":\"succeeded\",\"kind\":\"exec\",\"log\":\"Finished\",\"createdAt\":\"2026-09-29T10:00:00Z\",\"artifacts\":[]}}}"));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	const TSharedRef<TCapture<FBuild>> Finished = MakeShared<TCapture<FBuild>>();
	Rig.Client->WaitForBuild(TEXT("b-1"), Into(Finished), nullptr, 0.f, 60.f);
	if (!TestTrue(TEXT("the wait ends"), Await(Rig, Finished)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("it ends on the build's answer, not the busy or lost read (%s)"), *Finished->Result.Error.Message), Finished->Result.bOk);
	TestTrue(TEXT("the build succeeded"), Finished->Result.Value.IsSucceeded());
	TestEqual(TEXT("the status was read three times"), *Rig.Sends, 3);
	return true;
}

// A wrong-datacenter refusal the connection could not follow itself is followed once, to the crowdedkingdoms.com host it names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperFollowsDatacenterTest, "CrowdySDK.CrowdyExecEditor.DeveloperFollowsNamedDatacenter",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperFollowsDatacenterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig(TEXT("https://ck.dev.crowdedkingdoms.com/graphql"));
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"errors\":[{\"message\":\"App 7 is served from another datacenter. Reconnect to https://ck-eu.dev.crowdedkingdoms.com.\",\"extensions\":{\"code\":\"WRONG_DATACENTER\"}}],\"data\":null}"),
		TEXT("{\"data\":{\"execAppStatus\":{\"activeVersion\":4,\"disabled\":false,\"disabledTypes\":[],\"budgetPaused\":false}}}"),
		TEXT("{\"errors\":[{\"message\":\"Reconnect to http://ck-us.dev.crowdedkingdoms.com\",\"extensions\":{\"code\":\"WRONG_DATACENTER\"}}],\"data\":null}"),
	});

	const TSharedRef<TCapture<FAppStatus>> Moved = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Moved));
	if (!TestTrue(TEXT("the status is answered"), Await(Rig, Moved)))
	{
		return false;
	}
	TestTrue(Moved->Result.Error.Message, Moved->Result.bOk);
	TestTrue(TEXT("with the moved datacenter's answer"), Moved->Result.Value.ActiveVersion.IsSet() && Moved->Result.Value.ActiveVersion.GetValue() == 4);
	TestEqual(TEXT("sent twice"), *Rig.Sends, 2);
	TestEqual(TEXT("the second request went to the named datacenter"), Rig.LastUrl(), FString(TEXT("https://ck-eu.dev.crowdedkingdoms.com/graphql")));
	TestTrue(TEXT("still under the session bearer"), Rig.LastAuthorization().Contains(SessionBearer));

	const TSharedRef<TCapture<FAppStatus>> Plain = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Plain));
	if (TestTrue(TEXT("the second status is answered"), Await(Rig, Plain)))
	{
		TestFalse(TEXT("a datacenter named over plain http is not followed"), Plain->Result.bOk);
		TestEqual(TEXT("the refusal is answered"), Plain->Result.Error.Code, FString(TEXT("WRONG_DATACENTER")));
		TestEqual(TEXT("and not sent again"), *Rig.Sends, 3);
		TestEqual(TEXT("the client stays where it was"), Rig.LastUrl(), FString(TEXT("https://ck-eu.dev.crowdedkingdoms.com/graphql")));
	}
	return true;
}

// A wrong-datacenter refusal naming a host outside crowdedkingdoms.com is answered, never followed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperRefusesOffEstateTest, "CrowdySDK.CrowdyExecEditor.DeveloperRefusesUnknownDatacenter",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperRefusesOffEstateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	// The connection itself would accept a sibling in its own estate, so only the crowdedkingdoms.com rule refuses this one.
	FRig Rig(TEXT("https://ck-api-1.prod.cp.cks-env.com/graphql"));
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"errors\":[{\"message\":\"Reconnect to https://ck-api-4.prod.cp.cks-env.com\",\"extensions\":{\"code\":\"WRONG_DATACENTER\"}}],\"data\":null}"),
	});

	const TSharedRef<TCapture<FAppStatus>> Status = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Status));
	if (!TestTrue(TEXT("the status is answered"), Await(Rig, Status)))
	{
		return false;
	}
	TestFalse(TEXT("it failed"), Status->Result.bOk);
	TestEqual(TEXT("with the refusal"), Status->Result.Error.Code, FString(TEXT("WRONG_DATACENTER")));
	TestEqual(TEXT("sent once"), *Rig.Sends, 1);
	TestEqual(TEXT("the client did not move"), Rig.Bridge->GetApiEndpoint(), FString(TEXT("https://ck-api-1.prod.cp.cks-env.com/graphql")));
	return true;
}

// Ids and counts are read strictly: a BigInt must be a number or a string of digits, an Int must fit 32 bits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperStrictNumbersTest, "CrowdySDK.CrowdyExecEditor.DeveloperNumbersAreReadStrictly",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperStrictNumbersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"data\":{\"execInstances\":[{\"instanceId\":\"i-1\",\"nodeType\":\"sbx_counter\",\"key\":\"village\",\"kind\":\"hub\",\"phase\":\"idle\",\"host\":null,\"epoch\":\"9007199254740993\",\"sinceMs\":1,\"heldBack\":null}]}}"),
		TEXT("{\"data\":{\"execInstances\":[{\"instanceId\":\"i-1\",\"nodeType\":\"sbx_counter\",\"key\":\"village\",\"kind\":\"hub\",\"phase\":\"idle\",\"host\":null,\"epoch\":\"12abc\",\"sinceMs\":1,\"heldBack\":null}]}}"),
		TEXT("{\"data\":{\"execDeploy\":{\"version\":3000000000}}}"),
	});

	const TSharedRef<TCapture<TArray<FInstance>>> Digits = MakeShared<TCapture<TArray<FInstance>>>();
	Rig.Client->Instances(Into(Digits));
	if (TestTrue(TEXT("the first read is answered"), Await(Rig, Digits)) && TestTrue(Digits->Result.Error.Message, Digits->Result.bOk)
		&& TestEqual(TEXT("one instance"), Digits->Result.Value.Num(), 1))
	{
		TestEqual(TEXT("a BigInt sent as digits keeps every digit"), Digits->Result.Value[0].Epoch, static_cast<int64>(9007199254740993));
	}

	const TSharedRef<TCapture<TArray<FInstance>>> Words = MakeShared<TCapture<TArray<FInstance>>>();
	Rig.Client->Instances(Into(Words));
	if (TestTrue(TEXT("the second read is answered"), Await(Rig, Words)))
	{
		// Unreal's own integer parse reads "12abc" as 12, so only the whole-number check refuses it.
		TestFalse(TEXT("a BigInt with trailing letters is an error"), Words->Result.bOk);
		TestTrue(FString::Printf(TEXT("naming the field (%s)"), *Words->Result.Error.Message), Words->Result.Error.Message.Contains(TEXT("answered without a valid execInstances.epoch")));
	}

	const TSharedRef<TCapture<int32>> Deployed = MakeShared<TCapture<int32>>();
	Rig.Client->Deploy(TEXT("{}"), TEXT("b-1"), Into(Deployed));
	if (TestTrue(TEXT("the deploy is answered"), Await(Rig, Deployed)))
	{
		TestFalse(TEXT("a version past 32 bits is an error"), Deployed->Result.bOk);
		TestTrue(FString::Printf(TEXT("naming the field (%s)"), *Deployed->Result.Error.Message), Deployed->Result.Error.Message.Contains(TEXT("answered without a valid execDeploy.version")));
	}
	return true;
}

// A restart names its type and answers how many instances it stopped; the app's use and limits, the pause reason and the build's SDK version are read, and an older platform that leaves them out still parses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperRestartAndLimitsTest, "CrowdySDK.CrowdyExecEditor.DeveloperRestartAndAppLimits",
	CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeveloperRestartAndLimitsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	Rig.Script({
		TEXT("{\"data\":{\"execRestartType\":{\"nodeType\":\"sbx_counter\",\"stopped\":3}}}"),
		TEXT("{\"data\":{\"execAppStatus\":{\"activeVersion\":4,\"disabled\":false,\"disabledTypes\":[],\"budgetPaused\":true,\"budgetPauseReason\":\"spend_cap\",\"maxInstances\":null,\"maxReservedMb\":1024,\"instanceLimit\":16,\"instances\":5,\"reservedMb\":320}}}"),
		TEXT("{\"data\":{\"execAppStatus\":{\"activeVersion\":4,\"disabled\":false,\"disabledTypes\":[],\"budgetPaused\":false}}}"),
		TEXT("{\"data\":{\"execBuild\":{\"buildId\":\"b-9\",\"status\":\"succeeded\",\"kind\":\"exec\",\"log\":\"\",\"sdkVersion\":\"0.8.0\",\"artifacts\":[]}}}"),
	});

	const TSharedRef<TCapture<FRestart>> Restarted = MakeShared<TCapture<FRestart>>();
	Rig.Client->RestartType(TEXT("sbx_counter"), Into(Restarted));
	if (TestTrue(TEXT("the restart is answered"), Await(Rig, Restarted)) && TestTrue(Restarted->Result.Error.Message, Restarted->Result.bOk))
	{
		TestEqual(TEXT("the restart is sent under its name"), OperationOf(Rig), FString(TEXT("ExecRestartType")));
		const TSharedPtr<FJsonObject> Request = Rig.LastRequest();
		const TSharedPtr<FJsonObject>* Variables = nullptr;
		TestTrue(TEXT("naming the type"), Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Variables)
			&& (*Variables)->GetStringField(TEXT("nodeType")) == TEXT("sbx_counter") && (*Variables)->GetStringField(TEXT("appId")) == TestAppIdText);
		TestEqual(TEXT("the instances it stopped"), Restarted->Result.Value.Stopped, 3);
		TestEqual(TEXT("the type it restarted"), Restarted->Result.Value.NodeType, FString(TEXT("sbx_counter")));
	}

	const TSharedRef<TCapture<FAppStatus>> Limits = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Limits));
	if (TestTrue(TEXT("the status is answered"), Await(Rig, Limits)) && TestTrue(Limits->Result.Error.Message, Limits->Result.bOk))
	{
		const FAppStatus& Status = Limits->Result.Value;
		TestEqual(TEXT("the pause reason"), Status.BudgetPauseReason, FString(TEXT("spend_cap")));
		TestEqual(TEXT("instances placed"), Status.Instances.Get(-1), 5);
		TestEqual(TEXT("the instance limit in force"), Status.InstanceLimit.Get(-1), 16);
		TestEqual(TEXT("memory reserved"), Status.ReservedMb.Get(-1), 320);
		TestEqual(TEXT("the memory limit"), Status.MaxReservedMb.Get(-1), 1024);
	}

	const TSharedRef<TCapture<FAppStatus>> Older = MakeShared<TCapture<FAppStatus>>();
	Rig.Client->Status(Into(Older));
	if (TestTrue(TEXT("the older status is answered"), Await(Rig, Older)) && TestTrue(Older->Result.Error.Message, Older->Result.bOk))
	{
		const FAppStatus& Status = Older->Result.Value;
		TestFalse(TEXT("an older platform says nothing of the use or limits"), Status.Instances.IsSet() || Status.InstanceLimit.IsSet() || Status.ReservedMb.IsSet() || Status.MaxReservedMb.IsSet());
		TestTrue(TEXT("nor of a pause reason"), Status.BudgetPauseReason.IsEmpty());
	}

	const TSharedRef<TCapture<FBuild>> Built = MakeShared<TCapture<FBuild>>();
	Rig.Client->Build({}, Into(Built));
	if (TestTrue(TEXT("the build is answered"), Await(Rig, Built)) && TestTrue(Built->Result.Error.Message, Built->Result.bOk))
	{
		TestEqual(TEXT("the SDK the platform compiled against"), Built->Result.Value.SdkVersion, FString(TEXT("0.8.0")));
	}
	return true;
}

namespace CrowdyExecDeveloperApiTests
{
	const TCHAR* const SplitManifest = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"crate\":\"root\",\"client\":true},")
		TEXT("\"alpha\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"alpha\",\"client\":true},\"beta\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"beta\",\"client\":true},")
		TEXT("\"gamma\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"gamma\",\"client\":true}}}");
	const TCHAR* const SplitLive = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"digest\":\"dr\",\"client\":true}}}");

	FString BuildStarted(const TCHAR* BuildId)
	{
		return FString::Printf(TEXT("{\"data\":{\"execBuild\":{\"buildId\":\"%s\",\"status\":\"queued\",\"kind\":\"exec\",\"log\":null,\"createdAt\":\"2026-10-18T10:00:00Z\",\"artifacts\":[]}}}"), BuildId);
	}

	FString BuildEnded(const TCHAR* BuildId, const TCHAR* Status, const TCHAR* Crate, const TCHAR* Digest)
	{
		const FString Artifacts = Crate ? FString::Printf(TEXT("[{\"crate\":\"%s\",\"digest\":\"%s\",\"sizeBytes\":10}]"), Crate, Digest) : FString(TEXT("[]"));
		return FString::Printf(TEXT("{\"data\":{\"execBuildStatus\":{\"buildId\":\"%s\",\"status\":\"%s\",\"kind\":\"exec\",\"log\":\"log of %s\",\"createdAt\":\"2026-10-18T10:00:00Z\",\"artifacts\":%s}}}"),
			BuildId, Status, BuildId, *Artifacts);
	}

	FString DeployAnswer(int32 Version)
	{
		return FString::Printf(TEXT("{\"data\":{\"execDeploy\":{\"version\":%d}}}"), Version);
	}

	FString ActiveAnswer(int32 Version, const TCHAR* ManifestJson)
	{
		return FString::Printf(TEXT("{\"data\":{\"execVersions\":[{\"version\":%d,\"createdBy\":\"user:1\",\"createdAt\":\"2026-10-18T10:00:00Z\",\"types\":1,\"active\":true,\"manifestJson\":\"%s\"}]}}"),
			Version, *FString(ManifestJson).Replace(TEXT("\""), TEXT("\\\"")));
	}

	FString ActivatedAnswer(int32 Version)
	{
		return FString::Printf(TEXT("{\"data\":{\"execActivateVersion\":{\"activeVersion\":%d,\"disabled\":false,\"disabledTypes\":[],\"budgetPaused\":false}}}"), Version);
	}

	CrowdyExecBuildAndDeploy::FLive LiveVersion(int32 Version)
	{
		CrowdyExecBuildAndDeploy::FLive Live;
		Live.Version = Version;
		Live.ManifestJson = SplitLive;
		return Live;
	}

	FBuildCrate NamedCrate(const TCHAR* Name)
	{
		FBuildCrate Crate;
		Crate.Name = Name;
		Crate.Files.Add(FBuildFile{TEXT("src/lib.rs"), FString::Printf(TEXT("// %s"), Name)});
		return Crate;
	}

	struct FSent
	{
		FString Operation;
		TSharedPtr<FJsonObject> Variables;
		TSharedPtr<FJsonObject> Input;
	};

	FSent ReadSent(const FString& Body)
	{
		FSent Sent;
		TSharedPtr<FJsonObject> Request;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request) || !Request.IsValid())
		{
			return Sent;
		}
		Request->TryGetStringField(TEXT("operationName"), Sent.Operation);
		const TSharedPtr<FJsonObject>* Variables = nullptr;
		if (Request->TryGetObjectField(TEXT("variables"), Variables))
		{
			Sent.Variables = *Variables;
			Sent.Input = InputOf(*Variables);
		}
		return Sent;
	}

	/** The type entry Name of the manifest a deploy sent, null when it has none. */
	TSharedPtr<FJsonObject> SentType(const FSent& Sent, const TCHAR* Name)
	{
		FString ManifestJson;
		TSharedPtr<FJsonObject> Manifest;
		const TSharedPtr<FJsonObject>* Types = nullptr;
		const TSharedPtr<FJsonObject>* Type = nullptr;
		if (!Sent.Input.IsValid() || !Sent.Input->TryGetStringField(TEXT("manifestJson"), ManifestJson)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), Manifest) || !Manifest.IsValid()
			|| !Manifest->TryGetObjectField(TEXT("types"), Types) || !(*Types)->TryGetObjectField(Name, Type))
		{
			return nullptr;
		}
		return *Type;
	}

	FString FieldOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	struct FRunCapture
	{
		bool bDone = false;
		int32 Done = 0;
		int32 Deploying = 0;
		int32 SentAtDeploying = -1;
		int32 SentAtFinal = -1;
		CrowdyExecBuildAndDeploy::FOutcome Outcome;
	};

	CrowdyExecBuildAndDeploy::FCallbacks CaptureRun(const FRig& Rig, const TSharedRef<FRunCapture>& Capture)
	{
		CrowdyExecBuildAndDeploy::FCallbacks Callbacks;
		Callbacks.OnDeploying = [Capture, Sends = Rig.Sends]()
		{
			++Capture->Deploying;
			Capture->SentAtDeploying = *Sends;
		};
		Callbacks.OnFinalDeploy = [Capture, Sends = Rig.Sends]() { Capture->SentAtFinal = *Sends; };
		Callbacks.OnDone = [Capture](const CrowdyExecBuildAndDeploy::FOutcome& Outcome)
		{
			++Capture->Done;
			Capture->Outcome = Outcome;
			Capture->bDone = true;
		};
		return Callbacks;
	}

	const FBuildArtifact* FindArtifact(const CrowdyExecBuildAndDeploy::FOutcome& Outcome, const TCHAR* Crate)
	{
		return Outcome.Artifacts.FindByPredicate([Crate](const FBuildArtifact& Artifact) { return Artifact.Crate == Crate; });
	}
}

// Two builds run before anything deploys; the first is stored by a staging deploy of what is live, then one deploy names it and the reused crate by digest and the last build's crate by name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecSplitDeployTest, "CrowdySDK.CrowdyExecEditor.DeploySplitStagesEachBuildButTheLast", CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecSplitDeployTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, BuildStarted(TEXT("b-1")));
	Responses.Emplace(200, BuildEnded(TEXT("b-1"), TEXT("succeeded"), TEXT("alpha"), TEXT("da")));
	Responses.Emplace(200, BuildStarted(TEXT("b-2")));
	Responses.Emplace(200, BuildEnded(TEXT("b-2"), TEXT("succeeded"), TEXT("beta"), TEXT("db")));
	Responses.Emplace(200, ActiveAnswer(9, SplitLive));
	Responses.Emplace(200, DeployAnswer(10));
	Responses.Emplace(200, ActivatedAnswer(9));
	Responses.Emplace(200, DeployAnswer(11));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	CrowdyExecBuildAndDeploy::FPlan Plan;
	Plan.Builds.Add({NamedCrate(TEXT("alpha"))});
	Plan.Builds.Add({NamedCrate(TEXT("beta"))});
	Plan.Reused.Add(TEXT("gamma"), TEXT("dg"));
	const TSharedRef<FRunCapture> Capture = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Plan, SplitManifest, LiveVersion(9), CaptureRun(Rig, Capture), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy ends"), Rig.PumpUntil([Capture] { return Capture->bDone; })))
	{
		return false;
	}
	const CrowdyExecBuildAndDeploy::FOutcome& Outcome = Capture->Outcome;
	TestTrue(FString::Printf(TEXT("it deployed (%s)"), *Outcome.Error.Message), Outcome.Step == CrowdyExecBuildAndDeploy::EStep::Deployed);
	TestEqual(TEXT("as the last deploy's version"), Outcome.Version, 11);
	TestEqual(TEXT("the staging deploy's version is kept"), Outcome.StagedVersions, TArray<int32>({10}));
	TestEqual(TEXT("it ends once"), Capture->Done, 1);
	TestEqual(TEXT("the deploys start once, after both builds"), Capture->Deploying, 1);
	TestEqual(TEXT("after the builds' four requests"), Capture->SentAtDeploying, 4);
	TestEqual(TEXT("the instances are read just before the deploy itself, after staging"), Capture->SentAtFinal, 7);
	TestEqual(TEXT("the live version is active again after staging"), Outcome.RestoredVersion, 9);
	const FBuildArtifact* Alpha = FindArtifact(Outcome, TEXT("alpha"));
	const FBuildArtifact* Gamma = FindArtifact(Outcome, TEXT("gamma"));
	TestTrue(TEXT("every crate's module is reported: built"), Alpha && Alpha->Digest == TEXT("da") && FindArtifact(Outcome, TEXT("beta")));
	TestTrue(TEXT("and reused"), Gamma && Gamma->Digest == TEXT("dg"));

	TArray<FSent> Sent;
	for (const FString& Body : *Rig.SentBodies)
	{
		Sent.Add(ReadSent(Body));
	}
	TArray<FString> Operations;
	for (const FSent& Each : Sent)
	{
		Operations.Add(Each.Operation);
	}
	if (!TestEqual(TEXT("both builds; the active version read again; staging, the live version back, then the deploy"), Operations,
		TArray<FString>({TEXT("ExecBuild"), TEXT("ExecBuildStatus"), TEXT("ExecBuild"), TEXT("ExecBuildStatus"), TEXT("ExecVersions"), TEXT("ExecDeploy"),
			TEXT("ExecActivateVersion"), TEXT("ExecDeploy")})))
	{
		return false;
	}
	TestTrue(TEXT("the live version is the one made active again"), Sent[6].Variables.IsValid() && static_cast<int32>(Sent[6].Variables->GetNumberField(TEXT("version"))) == 9);
	const FSent& Staging = Sent[5];
	TestEqual(TEXT("the staging deploy carries the first build"), FieldOf(Staging.Input, TEXT("buildId")), FString(TEXT("b-1")));
	const TSharedPtr<FJsonObject> StandIn = SentType(Staging, TEXT("ck_staged_alpha"));
	TestEqual(TEXT("its stand-in names the first build's crate"), FieldOf(StandIn, TEXT("crate")), FString(TEXT("alpha")));
	TestEqual(TEXT("under the live root"), FieldOf(StandIn, TEXT("parent")), FString(TEXT("root")));
	TestTrue(TEXT("out of players' reach"), StandIn.IsValid() && StandIn->HasTypedField<EJson::Boolean>(TEXT("client")) && !StandIn->GetBoolField(TEXT("client")));
	TestEqual(TEXT("the live root runs as it did"), FieldOf(SentType(Staging, TEXT("root")), TEXT("digest")), FString(TEXT("dr")));
	TestFalse(TEXT("the project's types are not in it"), SentType(Staging, TEXT("alpha")).IsValid() || SentType(Staging, TEXT("beta")).IsValid());

	const FSent& Final = Sent[7];
	TestEqual(TEXT("the deploy carries the last build"), FieldOf(Final.Input, TEXT("buildId")), FString(TEXT("b-2")));
	TestEqual(TEXT("the stored build's crate is named by its digest"), FieldOf(SentType(Final, TEXT("alpha")), TEXT("digest")), FString(TEXT("da")));
	TestTrue(TEXT("and not by its crate"), FieldOf(SentType(Final, TEXT("alpha")), TEXT("crate")).IsEmpty());
	TestEqual(TEXT("the reused crate is named by its digest"), FieldOf(SentType(Final, TEXT("gamma")), TEXT("digest")), FString(TEXT("dg")));
	TestEqual(TEXT("the last build's crate is named by its crate"), FieldOf(SentType(Final, TEXT("beta")), TEXT("crate")), FString(TEXT("beta")));
	TestEqual(TEXT("a type keeps its other fields"), FieldOf(SentType(Final, TEXT("alpha")), TEXT("parent")), FString(TEXT("root")));
	TestFalse(TEXT("no stand-in is deployed"), SentType(Final, TEXT("ck_staged_alpha")).IsValid());
	return true;
}

// The platform leaves a removed type's Server Objects running, and a restart cannot name the type once it is gone, so the deploy stops them first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployStopsRemovedTest, "CrowdySDK.CrowdyExecEditor.DeployStopsTheTypesItRemovesFirst", CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecDeployStopsRemovedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	const FString LiveWithOld = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"digest\":\"dr\",\"client\":true},\"old\":{\"kind\":\"hub\",\"parent\":\"root\",\"digest\":\"do\",\"client\":true}}}");
	TestEqual(TEXT("a type the live version runs and the deploy leaves out is removed"), CrowdyExecBuildAndDeploy::FindRemovedTypes(LiveWithOld, SplitManifest), TArray<FString>({TEXT("old")}));
	TestTrue(TEXT("a type both run is not"), CrowdyExecBuildAndDeploy::FindRemovedTypes(SplitLive, SplitManifest).IsEmpty());
	const FString LiveWithAlphaCapital = FString(SplitManifest).Replace(TEXT("\"alpha\":{"), TEXT("\"Alpha\":{"), ESearchCase::CaseSensitive);
	TestEqual(TEXT("a Type Name differing only in case is another type, and is removed"), CrowdyExecBuildAndDeploy::FindRemovedTypes(LiveWithAlphaCapital, SplitManifest), TArray<FString>({TEXT("Alpha")}));
	TestTrue(TEXT("with nothing live, nothing is removed"), CrowdyExecBuildAndDeploy::FindRemovedTypes(FString(), SplitManifest).IsEmpty());
	TestTrue(TEXT("nor when the deploy's manifest cannot be read"), CrowdyExecBuildAndDeploy::FindRemovedTypes(LiveWithOld, TEXT("not json")).IsEmpty());

	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, ActiveAnswer(10, *LiveWithOld));
	Responses.Emplace(200, TEXT("{\"data\":{\"execRestartType\":{\"nodeType\":\"old\",\"stopped\":2}}}"));
	Responses.Emplace(200, DeployAnswer(12));
	Responses.Emplace(200, ActiveAnswer(12, *LiveWithOld));
	Responses.Emplace(200, TEXT("{\"errors\":[{\"message\":\"app 1 has no node type `old`\",\"extensions\":{\"code\":\"NOT_FOUND\"}}]}"));
	Responses.Emplace(200, DeployAnswer(13));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	// The caller's copy of what is live predates another deploy, which added old; the deploy goes by the version active now.
	CrowdyExecBuildAndDeploy::FPlan Plan;
	Plan.Reused.Add(TEXT("alpha"), TEXT("da"));
	const CrowdyExecBuildAndDeploy::FLive Live = LiveVersion(9);
	const TSharedRef<FRunCapture> Stopped = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Plan, SplitManifest, Live, CaptureRun(Rig, Stopped), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy ends"), Rig.PumpUntil([Stopped] { return Stopped->bDone; })))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("it deployed (%s)"), *Stopped->Outcome.Error.Message), Stopped->Outcome.Step == CrowdyExecBuildAndDeploy::EStep::Deployed);
	TestEqual(TEXT("the removed type's Server Objects were stopped"), Stopped->Outcome.StoppedRemoved, TArray<FString>({TEXT("old")}));
	TestTrue(TEXT("with nothing refused"), Stopped->Outcome.StopRemovedFailed.IsEmpty());
	if (TestEqual(TEXT("the active version read, one restart, then the deploy"), Rig.SentBodies->Num(), 3))
	{
		TestEqual(TEXT("the active version is read first"), ReadSent((*Rig.SentBodies)[0]).Operation, FString(TEXT("ExecVersions")));
		const FSent Restart = ReadSent((*Rig.SentBodies)[1]);
		TestEqual(TEXT("the restart goes before the deploy"), Restart.Operation, FString(TEXT("ExecRestartType")));
		TestTrue(TEXT("naming the removed type"), Restart.Variables.IsValid() && Restart.Variables->GetStringField(TEXT("nodeType")) == TEXT("old"));
		TestEqual(TEXT("the deploy follows"), ReadSent((*Rig.SentBodies)[2]).Operation, FString(TEXT("ExecDeploy")));
	}

	const TSharedRef<FRunCapture> Refused = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Plan, SplitManifest, Live, CaptureRun(Rig, Refused), 60.f, 0.f);
	if (!TestTrue(TEXT("the second deploy ends"), Rig.PumpUntil([Refused] { return Refused->bDone; })))
	{
		return false;
	}
	TestEqual(TEXT("a refused stop does not hold the deploy back"), Refused->Outcome.Version, 13);
	TestTrue(TEXT("nothing is said to be stopped"), Refused->Outcome.StoppedRemoved.IsEmpty());
	TestTrue(FString::Printf(TEXT("the refusal names the type and why (%s)"), *FString::Join(Refused->Outcome.StopRemovedFailed, TEXT("; "))),
		Refused->Outcome.StopRemovedFailed.Num() == 1 && Refused->Outcome.StopRemovedFailed[0].StartsWith(TEXT("old: ")) && Refused->Outcome.StopRemovedFailed[0].Contains(TEXT("no node type")));
	return true;
}

// A failed build deploys nothing, even when the build before it succeeded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecSplitDeployFailedBuildTest, "CrowdySDK.CrowdyExecEditor.DeploySplitFailedBuildDeploysNothing", CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecSplitDeployFailedBuildTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, BuildStarted(TEXT("b-1")));
	Responses.Emplace(200, BuildEnded(TEXT("b-1"), TEXT("succeeded"), TEXT("alpha"), TEXT("da")));
	Responses.Emplace(200, BuildStarted(TEXT("b-2")));
	Responses.Emplace(200, BuildEnded(TEXT("b-2"), TEXT("failed"), nullptr, nullptr));
	Responses.Emplace(200, DeployAnswer(10));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	CrowdyExecBuildAndDeploy::FPlan Plan;
	Plan.Builds.Add({NamedCrate(TEXT("alpha"))});
	Plan.Builds.Add({NamedCrate(TEXT("beta"))});
	const TSharedRef<FRunCapture> Capture = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Plan, SplitManifest, LiveVersion(9), CaptureRun(Rig, Capture), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy ends"), Rig.PumpUntil([Capture] { return Capture->bDone; })))
	{
		return false;
	}
	TestTrue(TEXT("it ends on the failed build"), Capture->Outcome.Step == CrowdyExecBuildAndDeploy::EStep::BuildFailed);
	TestEqual(TEXT("both builds are reported"), Capture->Outcome.Builds.Num(), 2);
	TestTrue(TEXT("the failed one last, with its log"), !Capture->Outcome.Builds.IsEmpty() && Capture->Outcome.Builds.Last().Log == TEXT("log of b-2"));
	TestEqual(TEXT("the deploys never start"), Capture->Deploying, 0);
	TestEqual(TEXT("nothing is sent after the failed build"), *Rig.Sends, 4);
	TestFalse(TEXT("no deploy is sent"), Rig.SentBodies->ContainsByPredicate([](const FString& Body) { return ReadSent(Body).Operation == TEXT("ExecDeploy"); }));
	TestEqual(TEXT("no version"), Capture->Outcome.Version, 0);
	return true;
}

// Nothing to build deploys once with no build; one build and nothing reused sends the manifest exactly as it was made.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecSplitDeployOneOrNoBuildTest, "CrowdySDK.CrowdyExecEditor.DeploySplitOneOrNoBuild", CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecSplitDeployOneOrNoBuildTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, ActiveAnswer(9, SplitLive));
	Responses.Emplace(200, DeployAnswer(12));
	Responses.Emplace(200, BuildStarted(TEXT("b-3")));
	Responses.Emplace(200, BuildEnded(TEXT("b-3"), TEXT("succeeded"), TEXT("alpha"), TEXT("da")));
	Responses.Emplace(200, ActiveAnswer(9, SplitLive));
	Responses.Emplace(200, DeployAnswer(13));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	CrowdyExecBuildAndDeploy::FPlan Reused;
	Reused.Reused.Add(TEXT("alpha"), TEXT("da"));
	const TSharedRef<FRunCapture> First = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Reused, SplitManifest, LiveVersion(9), CaptureRun(Rig, First), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy with nothing to build ends"), Rig.PumpUntil([First] { return First->bDone; })))
	{
		return false;
	}
	TestEqual(TEXT("it deployed"), First->Outcome.Version, 12);
	if (TestEqual(TEXT("with the active version read, then the deploy"), Rig.SentBodies->Num(), 2))
	{
		TestEqual(TEXT("the active version is read first"), ReadSent((*Rig.SentBodies)[0]).Operation, FString(TEXT("ExecVersions")));
		const FSent Sent = ReadSent((*Rig.SentBodies)[1]);
		TestEqual(TEXT("a deploy"), Sent.Operation, FString(TEXT("ExecDeploy")));
		TestTrue(TEXT("naming no build"), Sent.Input.IsValid() && !Sent.Input->HasField(TEXT("buildId")));
		TestEqual(TEXT("the reused crate by its digest"), FieldOf(SentType(Sent, TEXT("alpha")), TEXT("digest")), FString(TEXT("da")));
	}

	CrowdyExecBuildAndDeploy::FPlan OneBuild;
	OneBuild.Builds.Add({NamedCrate(TEXT("alpha"))});
	const TSharedRef<FRunCapture> Second = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), OneBuild, SplitManifest, LiveVersion(9), CaptureRun(Rig, Second), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy of one build ends"), Rig.PumpUntil([Second] { return Second->bDone; })))
	{
		return false;
	}
	TestEqual(TEXT("it deployed"), Second->Outcome.Version, 13);
	TestTrue(TEXT("with no staging deploy"), Second->Outcome.StagedVersions.IsEmpty());
	if (TestEqual(TEXT("with four more requests"), Rig.SentBodies->Num(), 6))
	{
		const FSent Sent = ReadSent(Rig.SentBodies->Last());
		TestEqual(TEXT("the manifest is sent exactly as made"), FieldOf(Sent.Input, TEXT("manifestJson")), FString(SplitManifest));
		TestEqual(TEXT("with its build"), FieldOf(Sent.Input, TEXT("buildId")), FString(TEXT("b-3")));
	}
	return true;
}

// Staging runs what is live now: when another deploy made a newer version active since the deploy started, nothing is deployed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecSplitDeployLiveMovedTest, "CrowdySDK.CrowdyExecEditor.DeploySplitStopsWhenTheLiveVersionMoved", CrowdyExecDeveloperApiTests::TestFlags)
bool FCrowdyExecSplitDeployLiveMovedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperApiTests;
	FRig Rig;
	if (!TestTrue(TEXT("the rig is made"), Rig.IsValid()))
	{
		return false;
	}
	TArray<TPair<int32, FString>> Responses;
	Responses.Emplace(200, BuildStarted(TEXT("b-1")));
	Responses.Emplace(200, BuildEnded(TEXT("b-1"), TEXT("succeeded"), TEXT("alpha"), TEXT("da")));
	Responses.Emplace(200, BuildStarted(TEXT("b-2")));
	Responses.Emplace(200, BuildEnded(TEXT("b-2"), TEXT("succeeded"), TEXT("beta"), TEXT("db")));
	Responses.Emplace(200, ActiveAnswer(10, SplitLive));
	Responses.Emplace(200, DeployAnswer(11));
	Rig.ScriptWithStatus(MoveTemp(Responses));

	CrowdyExecBuildAndDeploy::FPlan Plan;
	Plan.Builds.Add({NamedCrate(TEXT("alpha"))});
	Plan.Builds.Add({NamedCrate(TEXT("beta"))});
	const TSharedRef<FRunCapture> Capture = MakeShared<FRunCapture>();
	CrowdyExecBuildAndDeploy::Run(Rig.Client.ToSharedRef(), Plan, SplitManifest, LiveVersion(9), CaptureRun(Rig, Capture), 60.f, 0.f);
	if (!TestTrue(TEXT("the deploy ends"), Rig.PumpUntil([Capture] { return Capture->bDone; })))
	{
		return false;
	}
	TestTrue(TEXT("it stops before storing anything"), Capture->Outcome.Step == CrowdyExecBuildAndDeploy::EStep::StagingFailed);
	TestTrue(FString::Printf(TEXT("naming the version now active (%s)"), *Capture->Outcome.Error.Message), Capture->Outcome.Error.Message.Contains(TEXT("version 10")));
	TestFalse(TEXT("no deploy is sent"), Rig.SentBodies->ContainsByPredicate([](const FString& Body) { return ReadSent(Body).Operation == TEXT("ExecDeploy"); }));
	TestTrue(TEXT("no staging version"), Capture->Outcome.StagedVersions.IsEmpty());

	FVersion Active;
	Active.Version = 4;
	Active.bActive = true;
	TestFalse(TEXT("an active version whose manifest is not given leaves what runs unknown"), CrowdyExecBuildAndDeploy::FindLive({Active}).IsSet());
	Active.bActive = false;
	const TOptional<CrowdyExecBuildAndDeploy::FLive> Nothing = CrowdyExecBuildAndDeploy::FindLive({Active});
	TestTrue(TEXT("no active version is nothing live"), Nothing.IsSet() && Nothing->Version == 0);
	TestFalse(TEXT("a deploy in parts over an unknown live version is refused before building"), CrowdyExecBuildAndDeploy::FindPlanProblem(Plan, SplitManifest, {}).IsEmpty());
	TestTrue(TEXT("one over a known one is not"), CrowdyExecBuildAndDeploy::FindPlanProblem(Plan, SplitManifest, LiveVersion(9)).IsEmpty());
	CrowdyExecBuildAndDeploy::FPlan OneBuild;
	OneBuild.Builds.Add({NamedCrate(TEXT("alpha"))});
	TestTrue(TEXT("nor is one build over an unknown one"), CrowdyExecBuildAndDeploy::FindPlanProblem(OneBuild, SplitManifest, {}).IsEmpty());
	return true;
}

#endif
