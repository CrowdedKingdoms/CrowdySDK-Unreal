#include "Subsystem/CrowdyMods.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyServiceApiTestSupport.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyStudioProjects.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

namespace CrowdyModsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 TestApp = 31337;
	const TCHAR* const ModName = TEXT("construct-spinning-child");

	// Every request of the test client answers with this gateway.
	const TCHAR* const ConnectAnswer =
		TEXT("{\"data\":{\"execConnect\":{\"gatewayUrl\":\"wss://gateway.example.test\",\"token\":\"connect-1\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}");

	FString ErrorAnswer(const TCHAR* Code, const TCHAR* Reason = nullptr, const TCHAR* Message = TEXT("No."))
	{
		const FString ReasonField = Reason ? FString::Printf(TEXT(",\"reason\":\"%s\""), Reason) : FString();
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"%s\",\"extensions\":{\"code\":\"%s\"%s}}],\"data\":null}"), Message, Code, *ReasonField);
	}

	FString BuildJson(const TCHAR* Status, const TCHAR* Log = nullptr)
	{
		const FString LogField = Log ? FString::Printf(TEXT("\"%s\""), Log) : FString(TEXT("null"));
		return FString::Printf(
			TEXT("{\"buildId\":\"b-1F\",\"status\":\"%s\",\"kind\":\"exec\",\"log\":%s,\"sdkVersion\":\"0.13.0\",\"createdAt\":\"2026-11-11T10:00:00Z\",")
			TEXT("\"startedAt\":null,\"finishedAt\":null,\"artifacts\":[]}"), Status, *LogField);
	}

	FString BuildAnswer(const TCHAR* Field, const TCHAR* Status, const TCHAR* Log = nullptr)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":%s}}"), Field, *BuildJson(Status, Log));
	}

	FString ModJson(const TCHAR* GridId, const TCHAR* Name, bool bEnabled, bool bRunning, const TCHAR* Blocked = nullptr)
	{
		const FString BlockedField = Blocked ? FString::Printf(TEXT("\"%s\""), Blocked) : FString(TEXT("null"));
		return FString::Printf(
			TEXT("{\"modId\":\"m-7\",\"gridId\":\"%s\",\"name\":\"%s\",\"ownerId\":\"9007199254740995\",\"version\":3,\"digest\":\"f46465af3d3f\",")
			TEXT("\"enabled\":%s,\"listingId\":null,\"blocked\":%s,\"running\":%s,\"updatedAt\":\"2026-11-11T10:00:00.250Z\"}"),
			GridId, Name, bEnabled ? TEXT("true") : TEXT("false"), *BlockedField, bRunning ? TEXT("true") : TEXT("false"));
	}

	FString DataAnswer(const TCHAR* Field, const FString& Value)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":%s}}"), Field, *Value);
	}

	FCrowdyModFile ModFile(const FString& Path, const FString& Content)
	{
		FCrowdyModFile File;
		File.Path = Path;
		File.Content = Content;
		return File;
	}

	FString LogLine(int32 Level, const TCHAR* At, const TCHAR* Text)
	{
		return FString::Printf(TEXT("{\"id\":\"l\",\"nodeType\":\"mod:x\",\"key\":\"901\",\"level\":%d,\"host\":\"h\",\"at\":\"%s\",\"text\":\"%s\",\"flow\":null}"),
			Level, At, Text);
	}

	TArray<FCrowdyModFile> Crate()
	{
		return {ModFile(TEXT("Cargo.toml"), TEXT("[package]\nname = \"x\"\n")), ModFile(TEXT("README.md"), TEXT("hi")), ModFile(TEXT("src/lib.rs"), TEXT("// lib\n")),
			ModFile(TEXT("src/a/b.rs"), TEXT("// nested\n"))};
	}

	struct FSentCall
	{
		FString NodeType;
		FString Key;
		FString Method;
		TArray<uint8> Payload;
		uint32 Rid = 0;
		bool bValid = false;
	};

	// A call frame: tag, rid, node type (str8), key (str16), method (str8), then the payload.
	FSentCall ParseCall(const TArray<uint8>& Frame)
	{
		FSentCall Out;
		int32 At = 5;
		auto Take = [&Frame, &At](int32 Length, FString& Into)
		{
			if (Length < 0 || At + Length > Frame.Num())
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Frame.GetData() + At), Length);
			Into = FString(Converted.Length(), Converted.Get());
			At += Length;
			return true;
		};
		if (Frame.Num() < 6 || Frame[0] != 0x01 || !Take(Frame[At++], Out.NodeType) || At + 2 > Frame.Num())
		{
			return Out;
		}
		const int32 KeyLength = Frame[At] | (Frame[At + 1] << 8);
		At += 2;
		if (!Take(KeyLength, Out.Key) || At >= Frame.Num() || !Take(Frame[At++], Out.Method))
		{
			return Out;
		}
		Out.Payload = TArray<uint8>(Frame.GetData() + At, Frame.Num() - At);
		Out.Rid = Frame[1] | (Frame[2] << 8) | (Frame[3] << 16) | (static_cast<uint32>(Frame[4]) << 24);
		Out.bValid = true;
		return Out;
	}

	TArray<uint8> Reply(uint32 Rid, uint8 Status, const TArray<uint8>& Payload)
	{
		TArray<uint8> Frame = {0x81, static_cast<uint8>(Rid), static_cast<uint8>(Rid >> 8), static_cast<uint8>(Rid >> 16), static_cast<uint8>(Rid >> 24), Status};
		Frame.Append(Payload);
		return Frame;
	}

	struct FRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyMods> Mods;
		TSharedPtr<FCrowdyCppClient> Client;
		int32 Answers = 0;
		TArray<ECrowdyModBuildStatus> Progress;
		FCrowdyModBuild Build;
		FCrowdyModInfo Info;
		FCrowdyModList List;
		FCrowdyModLogs Logs;
		FCrowdyModDelete Delete;
		FCrowdyModStarter Starter;
		FCrowdyModCall Call;

		explicit FRig(const FString& Body, int32 Status = 200, bool bSignedIn = true, int64 AppId = TestApp)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Mods(NewObject<UCrowdyMods>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Body, Status))
		{
			// Signed out, the subsystem has no stand-in client, so it asks its game instance, which has no session.
			if (Client.IsValid() && bSignedIn)
			{
				Client->SetGameToken(TEXT("app-token"));
				Client->SetManagementToken(TEXT("session-token"));
			}
			Mods->InitializeForTest(bSignedIn ? Client : nullptr, AppId, TEXT("example.test"));
		}

		void Script(TArray<TPair<int32, FString>> Responses)
		{
			Client->SetTestResponseScript(MoveTemp(Responses));
		}

		void BuildMod(const TArray<FCrowdyModFile>& Files)
		{
			Mods->BuildMod(ModName, Files, [this](const FCrowdyModBuild& Answer) { Progress.Add(Answer.Status); },
				[this](const FCrowdyModBuild& Answer) { Build = Answer; ++Answers; });
		}

		void Deploy(int64 GridId = 901)
		{
			Mods->DeployMod(GridId, ModName, TEXT("b-1F"), [this](const FCrowdyModInfo& Answer) { Info = Answer; ++Answers; });
		}

		void CallMod(const FString& ParamsJson)
		{
			Mods->CallMod(901, ModName, TEXT("state"), ParamsJson, [this](const FCrowdyModCall& Answer) { Call = Answer; ++Answers; });
		}

		/** Pumps the client and the ticker, Delta seconds a step, until Done or the steps run out. */
		bool PumpUntil(TFunctionRef<bool()> Done, float Delta = 0.f, int32 Steps = 400)
		{
			for (int32 Step = 0; Step < Steps && !Done(); ++Step)
			{
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(Delta);
			}
			return Done();
		}

		void Pump(float Delta = 0.f)
		{
			PumpUntil([this]() { return Answers > 0; }, Delta);
		}

		/** Pumps in real time, as the gateway's socket answers on the task graph. */
		bool PumpLive(TFunctionRef<bool()> Done, double Seconds = 3.0)
		{
			const double Until = FPlatformTime::Seconds() + Seconds;
			while (!Done() && FPlatformTime::Seconds() < Until)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(0.f);
				FPlatformProcess::Sleep(0.005f);
			}
			return Done();
		}

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}

		bool SentAnything() const
		{
			FString Url;
			FString Authorization;
			return Client->GetLastTestRequest(Url, Authorization);
		}

		FString SentAuthorization() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}

		/** Opens the gateway for the Call Mod just made and reads the call it sent. */
		bool OpenAndReadCall(FAutomationTestBase& Test, FSentCall& Out)
		{
			if (!Test.TestTrue(TEXT("the call dials the gateway"), PumpLive([this]() { return Client->NumTestExecConnections() >= 1; })))
			{
				return false;
			}
			Client->TestExecOpen();
			TArray<TArray<uint8>> Sent;
			PumpLive([this, &Sent]()
			{
				Sent.Append(Client->TakeTestExecSentFrames());
				return Sent.Num() > 0;
			});
			if (!Test.TestEqual(TEXT("one frame was sent"), Sent.Num(), 1))
			{
				return false;
			}
			Out = ParseCall(Sent[0]);
			return Test.TestTrue(TEXT("it is a call"), Out.bValid);
		}
	};

	const TCHAR* const ModsRunProjectId = TEXT("p-1");
	constexpr int64 ModsRunPlayer = 9007199254740995LL;
	constexpr int64 ModsRunOtherPlayer = 77;

	// A PAIRED project at revision 7. Each half's content is marked, so a crate holding the other half's files shows.
	const TCHAR* const ModsRunPairedFiles =
		TEXT("[{\"target\":\"SERVER\",\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"spinner\\\"\\n\",\"revision\":\"3\"},")
		TEXT("{\"target\":\"SERVER\",\"path\":\"src/lib.rs\",\"content\":\"// server at seven\\n\",\"revision\":\"7\"},")
		TEXT("{\"target\":\"SERVER\",\"path\":\"README.md\",\"content\":\"server readme\",\"revision\":\"2\"},")
		TEXT("{\"target\":\"CLIENT\",\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"spinner-client\\\"\\n\",\"revision\":\"4\"},")
		TEXT("{\"target\":\"CLIENT\",\"path\":\"src/lib.rs\",\"content\":\"// client at seven\\n\",\"revision\":\"6\"}]");

	const TCHAR* const ModsRunClientFiles =
		TEXT("[{\"target\":\"CLIENT\",\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"glow\\\"\\n\",\"revision\":\"2\"},")
		TEXT("{\"target\":\"CLIENT\",\"path\":\"src/lib.rs\",\"content\":\"// client at seven\\n\",\"revision\":\"7\"}]");

	const TCHAR* const ModsRunStarterAnswer =
		TEXT("{\"data\":{\"execModStarter\":{\"crate\":\"grid-mod\",\"nodeType\":\"mod:grid-mod\",\"description\":\"Greets visitors.\",")
		TEXT("\"files\":[{\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"grid-mod\\\"\\n\"},{\"path\":\"src/lib.rs\",\"content\":\"// visit\\n\"}]}}}");

	const TCHAR* const ModsRunConsentAnswer = TEXT("{\"data\":{\"execConsentClientMod\":true}}");

	// A crate that does not compile, as the platform's build log keeps rustc's output.
	const TCHAR* const ModsRunRustcLog = TEXT("error[E0425]: cannot find value `x` in this scope\\n  --> src/lib.rs:12:5\\n");

	FString ModsRunProject(const TCHAR* Pairing, const TCHAR* ServerModule, const TCHAR* ClientModule, const TCHAR* Revision, const TCHAR* Files)
	{
		const FString Server = ServerModule ? FString::Printf(TEXT("\"%s\""), ServerModule) : FString(TEXT("null"));
		const FString Client = ClientModule ? FString::Printf(TEXT("\"%s\""), ClientModule) : FString(TEXT("null"));
		return FString::Printf(
			TEXT("{\"data\":{\"crowdyStudioProject\":{\"projectId\":\"p-1\",\"gridId\":\"901\",\"name\":\"spinner\",\"serverModuleName\":%s,\"clientModuleName\":%s,")
			TEXT("\"pairingPreference\":\"%s\",\"revision\":\"%s\",\"archived\":false,\"updatedAt\":\"2026-11-11T10:00:00.250Z\",\"source\":\"STUDIO\",")
			TEXT("\"githubOwner\":null,\"githubRepo\":null,\"githubBranch\":null,\"githubSha\":null,\"files\":%s}}}"),
			*Server, *Client, Pairing, Revision, Files);
	}

	FString ModsRunBuild(const TCHAR* Field, const TCHAR* Status, const TCHAR* BuildId, const TCHAR* Log = nullptr)
	{
		return BuildAnswer(Field, Status, Log).Replace(TEXT("b-1F"), BuildId, ESearchCase::CaseSensitive);
	}

	FString ModsRunClient(const TCHAR* Name, const TCHAR* Digest, const TCHAR* Hash)
	{
		return FString::Printf(
			TEXT("{\"modId\":\"m-7\",\"gridId\":\"901\",\"name\":\"%s\",\"ownerId\":\"9007199254740995\",\"clientVersion\":2,\"digest\":\"%s\",\"sizeBytes\":4096,")
			TEXT("\"capabilitySummaryJson\":\"{\\\"hostFunctions\\\":[]}\",\"capabilityHash\":\"%s\",\"tickIntervalMs\":50,\"updatedAt\":\"2026-11-11T10:00:00.250Z\"}"),
			Name, Digest, Hash);
	}

	FString ModsRunServed(const TCHAR* Name, const TCHAR* Digest)
	{
		return FString::Printf(
			TEXT("{\"modId\":\"m-7\",\"name\":\"%s\",\"gridId\":\"901\",\"authorId\":\"9007199254740995\",\"listingId\":null,\"clientVersion\":2,\"digest\":\"%s\",")
			TEXT("\"capabilitySummaryJson\":\"{}\",\"capabilityHash\":\"h-1\",\"tickIntervalMs\":50,\"callerConsented\":true,\"authorCapabilitySummaryJson\":\"{}\",")
			TEXT("\"authorCapabilityHash\":\"h-1\",\"callerTrustsAuthor\":false,\"updatedAt\":\"2026-11-11T10:00:00.250Z\"}"),
			Name, Digest);
	}

	int32 ModsRunCountOf(const FString& Text, const TCHAR* Needle)
	{
		int32 Count = 0;
		for (int32 At = Text.Find(Needle, ESearchCase::CaseSensitive); At != INDEX_NONE; At = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			++Count;
		}
		return Count;
	}

	FString ModsRunList(const TCHAR* Field, const TArray<FString>& Rows)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":[%s]}}"), Field, *FString::Join(Rows, TEXT(",")));
	}

	// Every answer a PAIRED run at revision 7 asks for, in the order it asks.
	TArray<TPair<int32, FString>> ModsRunPairedScript(const TCHAR* Hash = TEXT("h-1"))
	{
		return {
			{200, ModsRunProject(TEXT("PAIRED"), TEXT("spinner"), TEXT("spinner-client"), TEXT("7"), ModsRunPairedFiles)},
			{200, ModsRunBuild(TEXT("execModClientBuild"), TEXT("succeeded"), TEXT("c-2"))},
			{200, ModsRunBuild(TEXT("execModBuild"), TEXT("succeeded"), TEXT("s-1"))},
			{200, DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), TEXT("spinner"), false, false))},
			{200, DataAnswer(TEXT("execModSetEnabled"), ModJson(TEXT("901"), TEXT("spinner"), true, false))},
			{200, DataAnswer(TEXT("execModClientDeploy"), ModsRunClient(TEXT("spinner"), TEXT("d-client-1"), Hash))},
			{200, FString(ModsRunConsentAnswer)},
		};
	}

	TArray<TPair<int32, FString>> ModsRunServerScript()
	{
		return {
			{200, ModsRunProject(TEXT("SERVER_ONLY"), TEXT("spinner"), nullptr, TEXT("7"), ModsRunPairedFiles)},
			{200, ModsRunBuild(TEXT("execModBuild"), TEXT("succeeded"), TEXT("s-1"))},
			{200, DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), TEXT("spinner"), false, false))},
			{200, DataAnswer(TEXT("execModSetEnabled"), ModJson(TEXT("901"), TEXT("spinner"), true, true))},
		};
	}

	// A project saved past the revision a run asks for.
	TArray<TPair<int32, FString>> ModsRunMovedOnScript()
	{
		return {{200, ModsRunProject(TEXT("SERVER_ONLY"), TEXT("spinner"), nullptr, TEXT("8"), ModsRunPairedFiles)}};
	}

	/** A rig whose Studio projects read through the same client, which records every request in the order it was sent. */
	struct FRunRig : FRig
	{
		TStrongObjectPtr<UCrowdyStudioProjects> Projects;
		TArray<FString> Sent;
		int32 Runs = 0;
		FCrowdyModRun Run;
		int32 Statuses = 0;
		FCrowdyModProjectStatus Status;
		/** Runs as each request goes out, with how many have gone, before its answer arrives. */
		TFunction<void(int32)> OnSent;

		explicit FRunRig(int64 AppId, bool bSignedIn = true)
			: FRig(TEXT("{}"), 200, bSignedIn, AppId)
			, Projects(NewObject<UCrowdyStudioProjects>(Instance.Get()))
		{
			Projects->InitializeForTest(bSignedIn ? Client : nullptr, AppId, Mods.Get());
			Mods->SetStudioProjectsForTest(Projects.Get());
			Mods->SetUserIdForTest(ModsRunPlayer);
			FCrowdyCppClient* Raw = Client.Get();
			Client->SetTestOnRequest([this, Raw](const FString&)
			{
				FString Body;
				Raw->GetLastTestRequestBody(Body);
				Sent.Add(Body);
				if (OnSent)
				{
					OnSent(Sent.Num());
				}
			});
		}

		// Ends the subsystems as a game instance's end does, so no run holds its app past the test.
		~FRunRig()
		{
			Client->SetTestOnRequest(nullptr);
			Mods->Deinitialize();
			Projects->Deinitialize();
		}

		void Start(const TCHAR* Revision, int64 GridId = 901, ECrowdyStudioRunMode Mode = ECrowdyStudioRunMode::DraftTest)
		{
			Mods->RunProject(GridId, ModsRunProjectId, Revision, Mode, [this](const FCrowdyModRun& Answer) { Run = Answer; ++Runs; });
		}

		bool PumpRun()
		{
			return PumpUntil([this]() { return Runs > 0; });
		}

		void ReadStatus()
		{
			Statuses = 0;
			Mods->GetProjectStatus(901, ModsRunProjectId, [this](const FCrowdyModProjectStatus& Answer) { Status = Answer; ++Statuses; });
			PumpUntil([this]() { return Statuses > 0; });
		}

		TArray<FString> Operations() const
		{
			TArray<FString> Names;
			for (const FString& Body : Sent)
			{
				TSharedPtr<FJsonObject> Request;
				FString Name;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request) && Request.IsValid())
				{
					Request->TryGetStringField(TEXT("operationName"), Name);
				}
				Names.Add(Name);
			}
			return Names;
		}

		/** The body of the first request of Operation, or empty when none was sent. */
		FString SentFor(const TCHAR* Operation) const
		{
			const int32 Index = Operations().IndexOfByKey(FString(Operation));
			return Sent.IsValidIndex(Index) ? Sent[Index] : FString();
		}
	};

	bool ModsRunExpectOps(FAutomationTestBase& Test, const TCHAR* What, const FRunRig& Rig, const TArray<FString>& Expected)
	{
		const TArray<FString> Names = Rig.Operations();
		return Test.TestTrue(FString::Printf(TEXT("%s (sent: %s)"), What, *FString::Join(Names, TEXT(", "))), Names == Expected);
	}
}

using namespace CrowdyModsTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildPollsTest, "CrowdySDK.Mods.BuildPollsUntilItEnds", Flags)
bool FCrowdyModsBuildPollsTest::RunTest(const FString& Parameters)
{
	FRig Rig(TEXT("{}"));
	Rig.Script({
		{200, BuildAnswer(TEXT("execModBuild"), TEXT("queued"))},
		{200, BuildAnswer(TEXT("execModBuildStatus"), TEXT("building"))},
		{200, BuildAnswer(TEXT("execModBuildStatus"), TEXT("building"))},
		{200, BuildAnswer(TEXT("execModBuildStatus"), TEXT("succeeded"), TEXT("toolchain 1.90\\nFinished"))},
	});
	Rig.BuildMod(Crate());
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers + Rig.Progress.Num(), 0);
	Rig.PumpUntil([&Rig]() { return Rig.Progress.Num() > 0; });
	TestTrue(TEXT("the first answer is reported as progress"), Rig.Progress == TArray<ECrowdyModBuildStatus>{ECrowdyModBuildStatus::Queued});
	Rig.PumpUntil([&Rig]() { return Rig.Answers > 0; }, 0.5f, 3);
	TestEqual(TEXT("no status is read before 2 seconds pass"), Rig.Progress.Num(), 1);

	Rig.Pump(UCrowdyMods::BuildPollSeconds);
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestTrue(TEXT("one progress per status change"),
		Rig.Progress == TArray<ECrowdyModBuildStatus>{ECrowdyModBuildStatus::Queued, ECrowdyModBuildStatus::Building, ECrowdyModBuildStatus::Succeeded});
	TestTrue(TEXT("the build succeeded"), Rig.Build.Result == ECrowdyModResult::Success && Rig.Build.Status == ECrowdyModBuildStatus::Succeeded);
	TestEqual(TEXT("the build id is kept exactly as given"), Rig.Build.BuildId, FString(TEXT("b-1F")));
	TestTrue(TEXT("with the compiler's log"), Rig.Build.Log.Contains(TEXT("Finished")));
	TestEqual(TEXT("and the SDK version"), Rig.Build.SdkVersion, FString(TEXT("0.13.0")));
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the status read names the build"), Body.Contains(TEXT("execModBuildStatus")) && Body.Contains(TEXT("\"buildId\":\"b-1F\"")));
	TestTrue(TEXT("the app travels as a BigInt string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	TestEqual(TEXT("a mod call carries the app token, not the session's"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildCompileFailureTest, "CrowdySDK.Mods.FailedCompileIsASuccessfulCall", Flags)
bool FCrowdyModsBuildCompileFailureTest::RunTest(const FString& Parameters)
{
	FRig Rig(TEXT("{}"));
	Rig.Script({
		{200, BuildAnswer(TEXT("execModBuild"), TEXT("queued"))},
		{200, BuildAnswer(TEXT("execModBuildStatus"), TEXT("failed"), TEXT("error[E0308]: mismatched types"))},
	});
	Rig.BuildMod(Crate());
	Rig.Pump(UCrowdyMods::BuildPollSeconds);
	TestTrue(TEXT("a crate that does not compile is a successful call"), Rig.Build.Result == ECrowdyModResult::Success);
	TestTrue(TEXT("whose status is Failed"), Rig.Build.Status == ECrowdyModBuildStatus::Failed);
	TestTrue(TEXT("and whose log says why"), Rig.Build.Log.Contains(TEXT("E0308")));
	TestTrue(TEXT("the failure is reported as progress too"), Rig.Progress.Num() == 2 && Rig.Progress.Last() == ECrowdyModBuildStatus::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildTimeoutTest, "CrowdySDK.Mods.BuildGivesUpAfterElevenMinutes", Flags)
bool FCrowdyModsBuildTimeoutTest::RunTest(const FString& Parameters)
{
	FRig Rig(BuildAnswer(TEXT("execModBuildStatus"), TEXT("building")));
	Rig.Script({{200, BuildAnswer(TEXT("execModBuild"), TEXT("queued"))}});
	Rig.Mods->SetNowForTest(0.0);
	Rig.BuildMod(Crate());
	Rig.PumpUntil([&Rig]() { return Rig.Progress.Num() > 0; });
	Rig.PumpUntil([&Rig]() { return Rig.Progress.Num() > 1; }, UCrowdyMods::BuildPollSeconds);
	Rig.Mods->SetNowForTest(UCrowdyMods::BuildLimitSeconds - 1.0);
	Rig.PumpUntil([&Rig]() { return Rig.Answers > 0; }, UCrowdyMods::BuildPollSeconds, 2);
	TestEqual(TEXT("a build still running before the limit is waited for"), Rig.Answers, 0);

	Rig.Mods->SetNowForTest(UCrowdyMods::BuildLimitSeconds);
	Rig.Pump(UCrowdyMods::BuildPollSeconds);
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestTrue(TEXT("a build past the limit fails"), Rig.Build.Result == ECrowdyModResult::Failed);
	TestTrue(TEXT("saying it did not finish"), Rig.Build.Message.Contains(TEXT("did not finish")));
	TestTrue(TEXT("with what the platform said so far"), Rig.Build.BuildId == TEXT("b-1F") && Rig.Build.Status == ECrowdyModBuildStatus::Building);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildPollRetryTest, "CrowdySDK.Mods.BuildOutlastsADroppedStatusRead", Flags)
bool FCrowdyModsBuildPollRetryTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig(TEXT("{}"));
		Rig.Script({
			{200, BuildAnswer(TEXT("execModBuild"), TEXT("queued"))},
			{503, FString()},
			{200, BuildAnswer(TEXT("execModBuildStatus"), TEXT("succeeded"))},
		});
		Rig.BuildMod(Crate());
		Rig.Pump(UCrowdyMods::BuildPollSeconds);
		TestEqual(TEXT("one answer"), Rig.Answers, 1);
		TestTrue(TEXT("a status read that did not complete is read again"),
			Rig.Build.Result == ECrowdyModResult::Success && Rig.Build.Status == ECrowdyModBuildStatus::Succeeded);
	}
	{
		FRig Rig(TEXT("{}"));
		Rig.Script({
			{200, BuildAnswer(TEXT("execModBuild"), TEXT("queued"))},
			{200, ErrorAnswer(TEXT("NOT_FOUND"))},
		});
		Rig.BuildMod(Crate());
		Rig.Pump(UCrowdyMods::BuildPollSeconds);
		TestTrue(TEXT("a refused status read ends the build"), Rig.Answers == 1 && Rig.Build.Result == ECrowdyModResult::NotFound);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildRefusesSourceTest, "CrowdySDK.Mods.BuildRefusesBadSourceWithoutARequest", Flags)
bool FCrowdyModsBuildRefusesSourceTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyModFile> TooMany;
	for (int32 Index = 0; Index <= UCrowdyMods::MaxFiles; ++Index)
	{
		TooMany.Add(ModFile(FString::Printf(TEXT("src/f%d.rs"), Index), FString()));
	}
	TArray<FCrowdyModFile> TooBig = Crate();
	TooBig[2].Content = FString::ChrN(static_cast<int32>(UCrowdyMods::MaxSourceBytes) + 1, TEXT('a'));
	TArray<FCrowdyModFile> Wide = Crate();
	// Each of these characters is three bytes of UTF-8, so the crate is over the limit in bytes though not in characters.
	Wide[2].Content = FString::ChrN(static_cast<int32>(UCrowdyMods::MaxSourceBytes / 3) + 1, TEXT('\x20AC'));
	struct FCase
	{
		TArray<FCrowdyModFile> Files;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{TooMany, TEXT("65 files")},
		{TooBig, TEXT("more than 2 MB of source")},
		{Wide, TEXT("more than 2 MB of UTF-8")},
		{{ModFile(TEXT("Makefile"), FString())}, TEXT("a file other than the three kinds")},
		{{ModFile(TEXT("src/lib.txt"), FString())}, TEXT("a file under src/ that is not .rs")},
		{{ModFile(TEXT("lib.rs"), FString())}, TEXT("a .rs file outside src/")},
		{{ModFile(TEXT("src/../lib.rs"), FString())}, TEXT("a path that climbs out of src/")},
		{{ModFile(TEXT("src\\lib.rs"), FString())}, TEXT("a path with a backslash")},
		{{ModFile(TEXT("cargo.toml"), FString())}, TEXT("a name in the wrong case")},
	};
	for (const FCase& Case : Cases)
	{
		FRig Rig(BuildAnswer(TEXT("execModBuild"), TEXT("queued")));
		Rig.BuildMod(Case.Files);
		TestEqual(FString::Printf(TEXT("%s: nothing answers inside the call"), Case.What), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("%s is refused"), Case.What), Rig.Build.Result == ECrowdyModResult::Refused);
		TestFalse(FString::Printf(TEXT("%s: without a request"), Case.What), Rig.SentAnything());
		TestEqual(FString::Printf(TEXT("%s: no progress"), Case.What), Rig.Progress.Num(), 0);
	}
	FRig Rig(BuildAnswer(TEXT("execModBuild"), TEXT("failed")));
	Rig.BuildMod(Crate());
	Rig.Pump();
	TestTrue(TEXT("Cargo.toml, README.md and .rs files in folders under src/ are sent"), Rig.SentBody().Contains(TEXT("src/a/b.rs")));
	TestTrue(TEXT("with the crate's name"), Rig.SentBody().Contains(ModName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsResultsTest, "CrowdySDK.Mods.ResultsMapThePlatformsAnswers", Flags)
bool FCrowdyModsResultsTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FString Body;
		int32 Status;
		ECrowdyModResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), ModName, false, false)), 200, ECrowdyModResult::Success, TEXT("a deployed mod")},
		{ErrorAnswer(TEXT("FORBIDDEN")), 200, ECrowdyModResult::Denied, TEXT("a missing permission")},
		{ErrorAnswer(TEXT("FORBIDDEN"), TEXT("PLAYER_WALLET_EMPTY")), 200, ECrowdyModResult::Denied, TEXT("a billing switch in the reason")},
		{ErrorAnswer(TEXT("CONFLICT"), nullptr, TEXT("PLAYER_SPEND_CAP: over the cap")), 200, ECrowdyModResult::Denied, TEXT("a billing switch at the start of the message")},
		{ErrorAnswer(TEXT("NOT_FOUND")), 200, ECrowdyModResult::NotFound, TEXT("no such build")},
		{ErrorAnswer(TEXT("GRID_NOT_FOUND")), 200, ECrowdyModResult::NotFound, TEXT("no such grid")},
		{ErrorAnswer(TEXT("BAD_USER_INPUT")), 200, ECrowdyModResult::Refused, TEXT("a bad name")},
		{ErrorAnswer(TEXT("LIMIT_EXCEEDED")), 200, ECrowdyModResult::Refused, TEXT("a limit")},
		{FString(), 503, ECrowdyModResult::Failed, TEXT("a request that did not complete")},
		{TEXT("{\"data\":{\"execModDeploy\":{\"modId\":\"m-7\"}}}"), 200, ECrowdyModResult::BadReply, TEXT("a malformed mod")},
		{DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("902"), ModName, false, false)), 200, ECrowdyModResult::BadReply, TEXT("a mod of another grid")},
		{DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), TEXT("other"), false, false)), 200, ECrowdyModResult::BadReply, TEXT("a mod of another name")},
		{DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901abc"), ModName, false, false)), 200, ECrowdyModResult::BadReply, TEXT("a grid id with trailing text")},
	};
	for (const FCase& Case : Cases)
	{
		FRig Rig(Case.Body, Case.Status);
		Rig.Deploy();
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.What, static_cast<int32>(Case.Expected)), Rig.Info.Result == Case.Expected);
		if (Case.Expected != ECrowdyModResult::Success)
		{
			TestTrue(FString::Printf(TEXT("%s carries no mod"), Case.What), Rig.Info.Mod.ModId.IsEmpty() && Rig.Info.Mod.GridId == 0);
		}
	}
	FRig Billing(ErrorAnswer(TEXT("FORBIDDEN"), TEXT("PLAYER_WALLET_EMPTY"), TEXT("the owner's wallet is empty")));
	Billing.Deploy();
	Billing.Pump();
	TestEqual(TEXT("a refusal's message is the platform's code and text"), Billing.Info.Message,
		FString(TEXT("FORBIDDEN PLAYER_WALLET_EMPTY: the owner's wallet is empty")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsDeployTest, "CrowdySDK.Mods.DeployReadsWholeIds", Flags)
bool FCrowdyModsDeployTest::RunTest(const FString& Parameters)
{
	FRig Rig(DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("9007199254740993"), ModName, false, false)));
	Rig.Deploy(9007199254740993LL);
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	Rig.Pump();
	TestTrue(TEXT("the deploy succeeded"), Rig.Info.Result == ECrowdyModResult::Success);
	const FCrowdyMod& Mod = Rig.Info.Mod;
	TestEqual(TEXT("a grid id past a double's precision stays whole"), Mod.GridId, static_cast<int64>(9007199254740993LL));
	TestEqual(TEXT("so does the owner's"), Mod.OwnerUserId, static_cast<int64>(9007199254740995LL));
	TestEqual(TEXT("the mod id is kept as given"), Mod.ModId, FString(TEXT("m-7")));
	TestEqual(TEXT("the version"), Mod.Version, static_cast<int64>(3));
	TestTrue(TEXT("a new mod is off and not running"), !Mod.bEnabled && !Mod.bRunning && !Mod.bBlocked && Mod.BlockedReason.IsEmpty());
	const int64 Expected = FDateTime(2026, 11, 11, 10, 0, 0).ToUnixTimestamp() * 1000 + 250;
	TestEqual(TEXT("the update time in milliseconds"), Mod.UpdatedAtMs, Expected);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the deploy names the grid as a BigInt string"), Body.Contains(TEXT("\"gridId\":\"9007199254740993\"")));
	TestTrue(TEXT("and the build as given"), Body.Contains(TEXT("\"buildId\":\"b-1F\"")) && Body.Contains(TEXT("execModDeploy")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsSwitchTest, "CrowdySDK.Mods.SwitchReadsBlockAndRefusal", Flags)
bool FCrowdyModsSwitchTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig(DataAnswer(TEXT("execModSetEnabled"), ModJson(TEXT("901"), ModName, true, false, TEXT("PLAYER_WALLET_EMPTY"))));
		Rig.Mods->SetModEnabled(901, ModName, true, [&Rig](const FCrowdyModInfo& Answer) { Rig.Info = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("the switch succeeded"), Rig.Info.Result == ECrowdyModResult::Success);
		TestTrue(TEXT("on but not running"), Rig.Info.Mod.bEnabled && !Rig.Info.Mod.bRunning);
		TestTrue(TEXT("blocked by billing"), Rig.Info.Mod.bBlocked);
		TestEqual(TEXT("with the reason"), Rig.Info.Mod.BlockedReason, FString(TEXT("PLAYER_WALLET_EMPTY")));
		TestTrue(TEXT("the switch is sent"), Rig.SentBody().Contains(TEXT("\"enabled\":true")));
	}
	{
		FRig Rig(ErrorAnswer(TEXT("FORBIDDEN"), nullptr, TEXT("run_server_code is missing")));
		Rig.Mods->SetModEnabled(901, ModName, true, [&Rig](const FCrowdyModInfo& Answer) { Rig.Info = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a switch without the permission is denied"), Rig.Info.Result == ECrowdyModResult::Denied);
		TestTrue(TEXT("and the message says which"), Rig.Info.Message.Contains(TEXT("run_server_code")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsListTest, "CrowdySDK.Mods.ListsReadEveryRow", Flags)
bool FCrowdyModsListTest::RunTest(const FString& Parameters)
{
	const auto List = [](const TCHAR* Field, const TArray<FString>& Rows)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":[%s]}}"), Field, *FString::Join(Rows, TEXT(",")));
	};
	{
		FRig Rig(List(TEXT("execMods"), {ModJson(TEXT("901"), ModName, true, true), ModJson(TEXT("901"), TEXT("greeter"), false, false)}));
		Rig.Mods->ListMods(901, [&Rig](const FCrowdyModList& Answer) { Rig.List = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a grid's mods are listed"), Rig.List.Result == ECrowdyModResult::Success && Rig.List.Mods.Num() == 2);
		TestTrue(TEXT("in the platform's order"), Rig.List.Mods.Num() == 2 && Rig.List.Mods[1].Name == TEXT("greeter"));
	}
	{
		FRig Rig(List(TEXT("execMods"), {}));
		Rig.Mods->ListMods(901, [&Rig](const FCrowdyModList& Answer) { Rig.List = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a grid with no mods is an empty success"), Rig.List.Result == ECrowdyModResult::Success && Rig.List.Mods.Num() == 0);
	}
	{
		FRig Rig(List(TEXT("execMods"), {ModJson(TEXT("901"), ModName, true, true), ModJson(TEXT("902"), TEXT("greeter"), false, false)}));
		Rig.Mods->ListMods(901, [&Rig](const FCrowdyModList& Answer) { Rig.List = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a row of another grid is a bad reply"), Rig.List.Result == ECrowdyModResult::BadReply && Rig.List.Mods.Num() == 0);
	}
	{
		FRig Rig(List(TEXT("execMods"), {ModJson(TEXT("901"), ModName, true, true).Replace(TEXT("\"running\":true"), TEXT("\"running\":null"))}));
		Rig.Mods->ListMods(901, [&Rig](const FCrowdyModList& Answer) { Rig.List = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a malformed row fails the list, never a shorter one"), Rig.List.Result == ECrowdyModResult::BadReply);
	}
	{
		FRig Rig(List(TEXT("execMyMods"), {ModJson(TEXT("901"), ModName, true, true), ModJson(TEXT("77"), ModName, false, false)}));
		Rig.Mods->ListMyMods([&Rig](const FCrowdyModList& Answer) { Rig.List = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("the player's mods on every grid are listed"), Rig.List.Result == ECrowdyModResult::Success && Rig.List.Mods.Num() == 2);
		TestTrue(TEXT("with their grids"), Rig.List.Mods.Num() == 2 && Rig.List.Mods[1].GridId == 77);
		TestTrue(TEXT("the read names the app"), Rig.SentBody().Contains(TEXT("execMyMods")) && Rig.SentBody().Contains(TEXT("\"appId\":\"31337\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsLogsTest, "CrowdySDK.Mods.LogsReadOldestFirst", Flags)
bool FCrowdyModsLogsTest::RunTest(const FString& Parameters)
{
	const FString Lines = FString::Printf(TEXT("{\"data\":{\"execModLogs\":[%s,%s,%s]}}"), *LogLine(3, TEXT("2026-11-11T10:00:03Z"), TEXT("third")),
		*LogLine(1, TEXT("2026-11-11T10:00:02Z"), TEXT("second")), *LogLine(0, TEXT("2026-11-11T10:00:01Z"), TEXT("first")));
	{
		FRig Rig(Lines);
		Rig.Mods->GetModLogs(901, ModName, 0, [&Rig](const FCrowdyModLogs& Answer) { Rig.Logs = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("the logs are read"), Rig.Logs.Result == ECrowdyModResult::Success);
		if (TestEqual(TEXT("every line"), Rig.Logs.Lines.Num(), 3))
		{
			TestEqual(TEXT("oldest first"), Rig.Logs.Lines[0].Text, FString(TEXT("first")));
			TestEqual(TEXT("newest last"), Rig.Logs.Lines[2].Text, FString(TEXT("third")));
			TestEqual(TEXT("the level as given"), Rig.Logs.Lines[2].Level, 3);
			TestEqual(TEXT("the time in milliseconds"), Rig.Logs.Lines[0].AtMs, FDateTime(2026, 11, 11, 10, 0, 1).ToUnixTimestamp() * 1000);
		}
		TestTrue(TEXT("a limit of 0 asks for 100"), Rig.SentBody().Contains(TEXT("\"limit\":100")));
	}
	for (const int32 Limit : {-1, UCrowdyMods::MaxLogLines + 1})
	{
		FRig Rig(Lines);
		Rig.Mods->GetModLogs(901, ModName, Limit, [&Rig](const FCrowdyModLogs& Answer) { Rig.Logs = Answer; ++Rig.Answers; });
		TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("a limit of %d is refused"), Limit), Rig.Logs.Result == ECrowdyModResult::Refused);
		TestFalse(TEXT("without a request"), Rig.SentAnything());
	}
	{
		FRig Rig(Lines.Replace(TEXT("\"level\":1"), TEXT("\"level\":\"warn\"")));
		Rig.Mods->GetModLogs(901, ModName, 500, [&Rig](const FCrowdyModLogs& Answer) { Rig.Logs = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a level that is not an integer is a bad reply"), Rig.Logs.Result == ECrowdyModResult::BadReply && Rig.Logs.Lines.Num() == 0);
		TestTrue(TEXT("500 lines may be asked for"), Rig.SentBody().Contains(TEXT("\"limit\":500")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsDeleteTest, "CrowdySDK.Mods.DeleteAnswersTheDeletion", Flags)
bool FCrowdyModsDeleteTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig(TEXT("{\"data\":{\"execModDelete\":true}}"));
		Rig.Mods->DeleteMod(901, ModName, [&Rig](const FCrowdyModDelete& Answer) { Rig.Delete = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a deletion succeeds"), Rig.Delete.Result == ECrowdyModResult::Success && Rig.Delete.bDeleted);
		TestTrue(TEXT("naming the grid and the mod"), Rig.SentBody().Contains(TEXT("\"gridId\":\"901\"")) && Rig.SentBody().Contains(ModName));
	}
	{
		FRig Rig(TEXT("{\"data\":{\"execModDelete\":\"yes\"}}"));
		Rig.Mods->DeleteMod(901, ModName, [&Rig](const FCrowdyModDelete& Answer) { Rig.Delete = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("an answer that is not a boolean is a bad reply"), Rig.Delete.Result == ECrowdyModResult::BadReply && !Rig.Delete.bDeleted);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsStarterTest, "CrowdySDK.Mods.StarterReadsThePlatformsCrate", Flags)
bool FCrowdyModsStarterTest::RunTest(const FString& Parameters)
{
	const FString Starter = TEXT("{\"data\":{\"execModStarter\":{\"crate\":\"grid-mod\",\"nodeType\":\"mod:grid-mod\",\"description\":\"Greets visitors.\",")
		TEXT("\"files\":[{\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"grid-mod\\\"\\n\"},{\"path\":\"src/lib.rs\",\"content\":\"// visit\\n\"}]}}}");
	{
		FRig Rig(Starter);
		Rig.Mods->GetStarter([&Rig](const FCrowdyModStarter& Answer) { Rig.Starter = Answer; ++Rig.Answers; });
		TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(TEXT("the starter is read"), Rig.Starter.Result == ECrowdyModResult::Success);
		TestEqual(TEXT("its name is the crate's"), Rig.Starter.Name, FString(TEXT("grid-mod")));
		if (TestEqual(TEXT("with both files"), Rig.Starter.Files.Num(), 2))
		{
			TestEqual(TEXT("each with its path"), Rig.Starter.Files[1].Path, FString(TEXT("src/lib.rs")));
			TestEqual(TEXT("and its content, newlines kept"), Rig.Starter.Files[0].Content, FString(TEXT("[package]\nname = \"grid-mod\"\n")));
		}
		TestTrue(TEXT("the read names the app"), Rig.SentBody().Contains(TEXT("execModStarter")) && Rig.SentBody().Contains(TEXT("\"appId\":\"31337\"")));
		TestEqual(TEXT("on the app token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	}
	{
		FRig Rig(ErrorAnswer(TEXT("FORBIDDEN")));
		Rig.Mods->GetStarter([&Rig](const FCrowdyModStarter& Answer) { Rig.Starter = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a refused starter carries no crate"), Rig.Starter.Result == ECrowdyModResult::Denied && Rig.Starter.Files.Num() == 0 && Rig.Starter.Name.IsEmpty());
	}
	{
		FRig Rig(Starter.Replace(TEXT("\"files\":["), TEXT("\"filez\":[")));
		Rig.Mods->GetStarter([&Rig](const FCrowdyModStarter& Answer) { Rig.Starter = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a starter with no files is a bad reply"), Rig.Starter.Result == ECrowdyModResult::BadReply);
	}
	{
		FRig Rig(Starter.Replace(TEXT("\"path\":\"src/lib.rs\""), TEXT("\"path\":\"src/../../Config/x.rs\"")));
		Rig.Mods->GetStarter([&Rig](const FCrowdyModStarter& Answer) { Rig.Starter = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a starter path that leaves its crate is a bad reply"), Rig.Starter.Result == ECrowdyModResult::BadReply && Rig.Starter.Files.Num() == 0);
	}
	{
		FRig Rig(FString(), 503);
		Rig.Mods->GetStarter([&Rig](const FCrowdyModStarter& Answer) { Rig.Starter = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a starter read that did not complete fails"), Rig.Starter.Result == ECrowdyModResult::Failed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsNotSignedInTest, "CrowdySDK.Mods.NoTokenIsNotSignedInLater", Flags)
bool FCrowdyModsNotSignedInTest::RunTest(const FString& Parameters)
{
	FRig Rig(DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), ModName, false, false)), 200, false);
	Rig.Deploy();
	Rig.BuildMod(Crate());
	Rig.CallMod(FString());
	TestEqual(TEXT("nothing answers inside the calls"), Rig.Answers, 0);
	Rig.PumpUntil([&Rig]() { return Rig.Answers == 3; });
	TestTrue(TEXT("a deploy without an app token is not signed in"), Rig.Info.Result == ECrowdyModResult::NotSignedIn);
	TestTrue(TEXT("a build is not signed in"), Rig.Build.Result == ECrowdyModResult::NotSignedIn);
	TestTrue(TEXT("a call is not signed in"), Rig.Call.Result == ECrowdyModResult::NotSignedIn);
	TestFalse(TEXT("nothing was sent"), Rig.SentAnything());
	TestEqual(TEXT("and no gateway was dialled"), Rig.Client->NumTestExecConnections(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsCallTest, "CrowdySDK.Mods.CallReachesTheModOnItsGrid", Flags)
bool FCrowdyModsCallTest::RunTest(const FString& Parameters)
{
	FRig Rig(ConnectAnswer);
	Rig.CallMod(TEXT("{\"n\":1}"));
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	FSentCall Sent;
	if (!Rig.OpenAndReadCall(*this, Sent))
	{
		return true;
	}
	TestEqual(TEXT("the dial carries the app token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	TestEqual(TEXT("the node type is the mod's"), Sent.NodeType, FString::Printf(TEXT("mod:%s"), ModName));
	TestEqual(TEXT("the key is the grid id"), Sent.Key, FString(TEXT("901")));
	TestEqual(TEXT("the method as given"), Sent.Method, FString(TEXT("state")));
	TestTrue(TEXT("the params travel as MessagePack"), Sent.Payload == TArray<uint8>({0x81, 0xa1, 'n', 0x01}));

	Rig.Client->TestExecReceiveBinary(Reply(Sent.Rid, 0, {0x81, 0xa2, 'o', 'k', 0xc3}));
	TestTrue(TEXT("the reply arrives"), Rig.PumpLive([&Rig]() { return Rig.Answers > 0; }));
	TestEqual(TEXT("once"), Rig.Answers, 1);
	TestTrue(TEXT("the call succeeded"), Rig.Call.Result == ECrowdyModResult::Success);
	TestEqual(TEXT("with the mod's answer as JSON"), Rig.Call.ReplyJson, FString(TEXT("{\"ok\":true}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsCallWakeTest, "CrowdySDK.Mods.CallWakeSendsAnEmptyMap", Flags)
bool FCrowdyModsCallWakeTest::RunTest(const FString& Parameters)
{
	FRig Rig(ConnectAnswer);
	Rig.CallMod(TEXT("{}"));
	FSentCall Sent;
	if (!Rig.OpenAndReadCall(*this, Sent))
	{
		return true;
	}
	TestEqual(TEXT("the wake calls state"), Sent.Method, FString(TEXT("state")));
	TestTrue(TEXT("with {} as an empty MessagePack map"), Sent.Payload == TArray<uint8>({0x80}));
	Rig.Client->TestExecReceiveBinary(Reply(Sent.Rid, 0, {0x80}));
	Rig.PumpLive([&Rig]() { return Rig.Answers > 0; });
	TestTrue(TEXT("an empty map answers as {}"), Rig.Call.Result == ECrowdyModResult::Success && Rig.Call.ReplyJson == TEXT("{}"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsCallDialRefusedTest, "CrowdySDK.Mods.CallKeepsTheDialsRefusal", Flags)
bool FCrowdyModsCallDialRefusedTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Code;
		ECrowdyModResult Expected;
	};
	const FCase Cases[] = {
		{TEXT("FORBIDDEN"), ECrowdyModResult::Denied},
		{TEXT("APP_PAUSED"), ECrowdyModResult::Failed},
	};
	for (const FCase& Case : Cases)
	{
		FRig Rig(ErrorAnswer(Case.Code, nullptr, TEXT("refused by the platform")));
		Rig.CallMod(FString());
		Rig.PumpLive([&Rig]() { return Rig.Answers > 0; }, 10.0);
		TestEqual(FString::Printf(TEXT("%s: one answer"), Case.Code), Rig.Answers, 1);
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.Code, static_cast<int32>(Case.Expected)), Rig.Call.Result == Case.Expected);
		TestTrue(FString::Printf(TEXT("%s: the message is the platform's (%s)"), Case.Code, *Rig.Call.Message),
			Rig.Call.Message.Contains(TEXT("refused by the platform")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsCallRefusedTest, "CrowdySDK.Mods.CallOfAnOffOrDeletedModIsRefused", Flags)
bool FCrowdyModsCallRefusedTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		uint8 Status;
		ECrowdyModResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{6, ECrowdyModResult::Denied, TEXT("a switched-off mod")},
		{4, ECrowdyModResult::NotFound, TEXT("a deleted mod")},
		{11, ECrowdyModResult::Refused, TEXT("a bad request")},
		{7, ECrowdyModResult::Failed, TEXT("a rate-limited call, worth trying again")},
	};
	for (const FCase& Case : Cases)
	{
		FRig Rig(ConnectAnswer);
		Rig.CallMod(FString());
		FSentCall Sent;
		if (!Rig.OpenAndReadCall(*this, Sent))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: no params travel as an empty map"), Case.What), Sent.Payload == TArray<uint8>({0x80}));
		const FTCHARToUTF8 Words(TEXT("mod is off"));
		Rig.Client->TestExecReceiveBinary(Reply(Sent.Rid, Case.Status, TArray<uint8>(reinterpret_cast<const uint8*>(Words.Get()), Words.Length())));
		Rig.PumpLive([&Rig]() { return Rig.Answers > 0; });
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.What, static_cast<int32>(Case.Expected)), Rig.Call.Result == Case.Expected);
		TestTrue(FString::Printf(TEXT("%s carries no reply"), Case.What), Rig.Call.ReplyJson.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: the message is the status and its words"), Case.What), Rig.Call.Message.EndsWith(TEXT(": mod is off")));
	}
	FRig Bad(ConnectAnswer);
	Bad.CallMod(TEXT("{not json"));
	Bad.Pump();
	TestTrue(TEXT("params that are not JSON are refused"), Bad.Call.Result == ECrowdyModResult::Refused);
	TestEqual(TEXT("without dialling the gateway"), Bad.Client->NumTestExecConnections(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsAfterEndTest, "CrowdySDK.Mods.AnswerAfterTheEndIsDropped", Flags)
bool FCrowdyModsAfterEndTest::RunTest(const FString& Parameters)
{
	{
		FRig Rig(DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), ModName, false, false)));
		Rig.Deploy();
		Rig.Mods->Deinitialize();
		Rig.Pump();
		TestEqual(TEXT("an answer arriving after the subsystem ended runs nothing"), Rig.Answers, 0);
	}
	{
		FRig Rig(ConnectAnswer);
		Rig.CallMod(FString());
		Rig.PumpLive([&Rig]() { return Rig.Client->NumTestExecConnections() >= 1; });
		Rig.Mods->Deinitialize();
		Rig.PumpLive([&Rig]() { return Rig.Answers > 0; }, 0.2);
		TestEqual(TEXT("a call cut off by the end runs nothing"), Rig.Answers, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsClientBuildTest, "CrowdySDK.Mods.ClientBuildSharesTheBuildPoll", Flags)
bool FCrowdyModsClientBuildTest::RunTest(const FString& Parameters)
{
	{
		FRunRig Rig(41001);
		Rig.Script({
			{200, ModsRunBuild(TEXT("execModClientBuild"), TEXT("queued"), TEXT("c-2"))},
			{200, ModsRunBuild(TEXT("execModBuildStatus"), TEXT("succeeded"), TEXT("c-2"), TEXT("Finished"))},
		});
		Rig.Mods->BuildClientMod(TEXT("glow"), Crate(), [&Rig](const FCrowdyModBuild& Answer) { Rig.Progress.Add(Answer.Status); },
			[&Rig](const FCrowdyModBuild& Answer) { Rig.Build = Answer; ++Rig.Answers; });
		TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
		Rig.Pump(UCrowdyMods::BuildPollSeconds);
		TestTrue(TEXT("the CLIENT build succeeded"), Rig.Build.Result == ECrowdyModResult::Success && Rig.Build.Status == ECrowdyModBuildStatus::Succeeded);
		TestEqual(TEXT("with its id as given"), Rig.Build.BuildId, FString(TEXT("c-2")));
		TestTrue(TEXT("one progress per status change"), Rig.Progress == TArray<ECrowdyModBuildStatus>{ECrowdyModBuildStatus::Queued, ECrowdyModBuildStatus::Succeeded});
		ModsRunExpectOps(*this, TEXT("it queues a CLIENT build, then reads the status every kind shares"), Rig, {TEXT("ExecModClientBuild"), TEXT("ExecModBuildStatus")});
		TestTrue(TEXT("naming the crate"), Rig.SentFor(TEXT("ExecModClientBuild")).Contains(TEXT("\"name\":\"glow\"")));
	}
	{
		FRunRig Rig(41002);
		Rig.Mods->BuildClientMod(TEXT("glow"), {ModFile(TEXT("Makefile"), FString())}, nullptr, [&Rig](const FCrowdyModBuild& Answer) { Rig.Build = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a CLIENT crate with a file of another kind is refused"), Rig.Build.Result == ECrowdyModResult::Refused);
		TestEqual(TEXT("without a request"), Rig.Sent.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsClientDeployTest, "CrowdySDK.Mods.ClientDeployReadsTheAttachedHalf", Flags)
bool FCrowdyModsClientDeployTest::RunTest(const FString& Parameters)
{
	const FString Attached = ModsRunClient(TEXT("spinner"), TEXT("d-1"), TEXT("h-1")).Replace(TEXT("\"gridId\":\"901\""), TEXT("\"gridId\":\"9007199254740993\""));
	struct FCase
	{
		FString Body;
		ECrowdyModResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{DataAnswer(TEXT("execModClientDeploy"), Attached), ECrowdyModResult::Success, TEXT("an attached CLIENT half")},
		{DataAnswer(TEXT("execModClientDeploy"), Attached.Replace(TEXT("9007199254740993"), TEXT("902"))), ECrowdyModResult::BadReply, TEXT("a half of another grid")},
		{DataAnswer(TEXT("execModClientDeploy"), Attached.Replace(TEXT("\"capabilityHash\":\"h-1\","), TEXT(""))), ECrowdyModResult::BadReply, TEXT("a half with no hash to consent to")},
		{ErrorAnswer(TEXT("NOT_FOUND")), ECrowdyModResult::NotFound, TEXT("a half for a mod that does not exist")},
	};
	for (const FCase& Case : Cases)
	{
		FRig Rig(Case.Body);
		FCrowdyModClientInfo Info;
		Rig.Mods->DeployClientMod(9007199254740993LL, TEXT("spinner"), TEXT("c-2"), [&Rig, &Info](const FCrowdyModClientInfo& Answer) { Info = Answer; ++Rig.Answers; });
		TestEqual(FString::Printf(TEXT("%s: nothing answers inside the call"), Case.What), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.What, static_cast<int32>(Case.Expected)), Info.Result == Case.Expected);
		if (Case.Expected != ECrowdyModResult::Success)
		{
			TestTrue(FString::Printf(TEXT("%s carries no half"), Case.What), Info.Client.ModId.IsEmpty() && Info.Client.GridId == 0);
			continue;
		}
		TestEqual(TEXT("a grid id past a double's precision stays whole"), Info.Client.GridId, static_cast<int64>(9007199254740993LL));
		TestEqual(TEXT("the author"), Info.Client.OwnerUserId, static_cast<int64>(9007199254740995LL));
		TestEqual(TEXT("the CLIENT version"), Info.Client.ClientVersion, static_cast<int64>(2));
		TestEqual(TEXT("the digest"), Info.Client.Digest, FString(TEXT("d-1")));
		TestEqual(TEXT("the hash a consent names"), Info.Client.CapabilityHash, FString(TEXT("h-1")));
		TestEqual(TEXT("the size"), Info.Client.SizeBytes, static_cast<int64>(4096));
		TestEqual(TEXT("the tick"), Info.Client.TickIntervalMs, 50);
		const FString Body = Rig.SentBody();
		TestTrue(TEXT("the attach names the grid, the mod and the build"), Body.Contains(TEXT("execModClientDeploy")) && Body.Contains(TEXT("\"gridId\":\"9007199254740993\""))
			&& Body.Contains(TEXT("\"name\":\"spinner\"")) && Body.Contains(TEXT("\"buildId\":\"c-2\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunRefusesTest, "CrowdySDK.Mods.RunRefusesBeforeSending", Flags)
bool FCrowdyModsRunRefusesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Revision;
		int64 GridId;
		bool bSignedIn;
		ECrowdyModResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{TEXT(""), 901, true, ECrowdyModResult::Refused, TEXT("an empty revision")},
		{TEXT("7"), 0, true, ECrowdyModResult::Refused, TEXT("grid 0")},
		{TEXT("7"), -4, true, ECrowdyModResult::Refused, TEXT("a negative grid")},
		{TEXT("7"), 901, false, ECrowdyModResult::NotSignedIn, TEXT("a signed-out player")},
	};
	int64 AppId = 41010;
	for (const FCase& Case : Cases)
	{
		FRunRig Rig(++AppId, Case.bSignedIn);
		Rig.Script(ModsRunServerScript());
		Rig.Start(Case.Revision, Case.GridId, ECrowdyStudioRunMode::Live);
		TestEqual(FString::Printf(TEXT("%s: nothing answers inside the call"), Case.What), Rig.Runs, 0);
		Rig.PumpRun();
		TestEqual(FString::Printf(TEXT("%s: one answer"), Case.What), Rig.Runs, 1);
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.What, static_cast<int32>(Case.Expected)), Rig.Run.Result == Case.Expected);
		TestFalse(FString::Printf(TEXT("%s: nothing was sent"), Case.What), Rig.SentAnything());
		TestTrue(FString::Printf(TEXT("%s: nothing was built"), Case.What), Rig.Run.Targets.IsEmpty() && Rig.Run.Revision.IsEmpty() && Rig.Run.BuildLog.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: the answer names its grid, project and mode"), Case.What),
			Rig.Run.GridId == Case.GridId && Rig.Run.ProjectId == ModsRunProjectId && Rig.Run.Mode == ECrowdyStudioRunMode::Live);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunRevisionTest, "CrowdySDK.Mods.RunRefusesAChangedRevisionBeforeBuilding", Flags)
bool FCrowdyModsRunRevisionTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41020);
	Rig.Script(ModsRunMovedOnScript());
	Rig.Start(TEXT("7"));
	Rig.PumpRun();
	TestTrue(TEXT("a project saved past the asked revision is refused"), Rig.Run.Result == ECrowdyModResult::RevisionChanged);
	TestTrue(TEXT("saying which revision it holds"), Rig.Run.Message.Contains(TEXT("8")));
	ModsRunExpectOps(*this, TEXT("only the project was read"), Rig, {TEXT("CrowdyStudioProject")});
	TestTrue(TEXT("nothing was built"), Rig.Run.Targets.IsEmpty() && Rig.Run.Revision.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunClientFailsTest, "CrowdySDK.Mods.RunPublishesNothingWhenTheClientDoesNotCompile", Flags)
bool FCrowdyModsRunClientFailsTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41030);
	TArray<TPair<int32, FString>> Script = ModsRunPairedScript();
	Script[1].Value = ModsRunBuild(TEXT("execModClientBuild"), TEXT("failed"), TEXT("c-2"), ModsRunRustcLog);
	Rig.Script(MoveTemp(Script));
	Rig.Start(TEXT("7"));
	Rig.PumpRun();
	TestTrue(TEXT("a CLIENT half that does not compile is a run that went to its end"), Rig.Run.Result == ECrowdyModResult::Success);
	ModsRunExpectOps(*this, TEXT("nothing was built or deployed after the CLIENT build"), Rig, {TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild")});
	if (TestEqual(TEXT("both halves are answered"), Rig.Run.Targets.Num(), 2))
	{
		TestTrue(TEXT("SERVER first, not started"), Rig.Run.Targets[0].Target == ECrowdyStudioTarget::Server && Rig.Run.Targets[0].Phase == ECrowdyModRunPhase::NotStarted);
		TestTrue(TEXT("the CLIENT half failed to build"), Rig.Run.Targets[1].Target == ECrowdyStudioTarget::Client && Rig.Run.Targets[1].Phase == ECrowdyModRunPhase::BuildFailed);
		TestTrue(TEXT("and runs nothing"), Rig.Run.Targets[1].RunningRevision.IsEmpty() && Rig.Run.Targets[1].Digest.IsEmpty());
	}
	TestTrue(TEXT("the log is the compiler's"), Rig.Run.BuildLog.Contains(TEXT("E0425")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunDiagnosticsTest, "CrowdySDK.Mods.RunConvertsTheBuildDiagnostics", Flags)
bool FCrowdyModsRunDiagnosticsTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41040);
	TArray<TPair<int32, FString>> Script = ModsRunPairedScript();
	Script[1].Value = ModsRunBuild(TEXT("execModClientBuild"), TEXT("failed"), TEXT("c-2"), ModsRunRustcLog);
	Rig.Script(MoveTemp(Script));
	Rig.Start(TEXT("7"));
	Rig.PumpRun();
	if (!TestEqual(TEXT("one diagnostic"), Rig.Run.Diagnostics.Num(), 1))
	{
		return true;
	}
	const FCrowdyModDiagnostic& Diagnostic = Rig.Run.Diagnostics[0];
	TestTrue(TEXT("a path with no half in it is the built half's"), Diagnostic.Target == ECrowdyStudioTarget::Client);
	TestTrue(TEXT("from the compiler"), Diagnostic.Source == ECrowdyModDiagnosticSource::Rustc);
	TestTrue(TEXT("an error"), Diagnostic.Severity == ECrowdyModDiagnosticSeverity::Error);
	TestEqual(TEXT("the file"), Diagnostic.Path, FString(TEXT("src/lib.rs")));
	TestEqual(TEXT("the line"), Diagnostic.Line, 12);
	TestEqual(TEXT("the column"), Diagnostic.Column, 5);
	TestEqual(TEXT("no end"), Diagnostic.EndLine, 0);
	TestEqual(TEXT("the code"), Diagnostic.Code, FString(TEXT("E0425")));
	TestEqual(TEXT("the message"), Diagnostic.Message, FString(TEXT("cannot find value `x` in this scope")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunPairedTest, "CrowdySDK.Mods.RunSendsThePairedSequenceInOrder", Flags)
bool FCrowdyModsRunPairedTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41050);
	Rig.Script(ModsRunPairedScript());
	Rig.Start(TEXT("7"), 901, ECrowdyStudioRunMode::Live);
	TestEqual(TEXT("nothing answers inside the call"), Rig.Runs, 0);
	Rig.PumpRun();
	TestTrue(FString::Printf(TEXT("the run went to its end (%s)"), *Rig.Run.Message), Rig.Run.Result == ECrowdyModResult::Success);
	ModsRunExpectOps(*this, TEXT("CLIENT built first, then SERVER built, deployed and switched on, then CLIENT attached and consented to"), Rig,
		{TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild"), TEXT("ExecModBuild"), TEXT("ExecModDeploy"), TEXT("ExecModSetEnabled"), TEXT("ExecModClientDeploy"),
			TEXT("ExecConsentClientMod")});

	const FString ClientBuild = Rig.SentFor(TEXT("ExecModClientBuild"));
	TestTrue(TEXT("the CLIENT crate is the read's CLIENT files"), ClientBuild.Contains(TEXT("// client at seven")) && ClientBuild.Contains(TEXT("\"name\":\"spinner-client\"")));
	TestFalse(TEXT("and none of its SERVER files"), ClientBuild.Contains(TEXT("// server at seven")));
	const FString ServerBuild = Rig.SentFor(TEXT("ExecModBuild"));
	TestTrue(TEXT("the SERVER crate is the read's SERVER files"), ServerBuild.Contains(TEXT("// server at seven")) && ServerBuild.Contains(TEXT("\"name\":\"spinner\"")));
	TestFalse(TEXT("and none of its CLIENT files"), ServerBuild.Contains(TEXT("// client at seven")));
	TestTrue(TEXT("the SERVER crate holds its README"), ServerBuild.Contains(TEXT("server readme")) && ServerBuild.Contains(TEXT("\"path\":\"README.md\"")));
	TestFalse(TEXT("and the CLIENT crate does not"), ClientBuild.Contains(TEXT("server readme")) || ClientBuild.Contains(TEXT("\"path\":\"README.md\"")));
	TestTrue(TEXT("each crate holds exactly its half's files"), ModsRunCountOf(ServerBuild, TEXT("\"path\":")) == 3 && ModsRunCountOf(ClientBuild, TEXT("\"path\":")) == 2);
	TestTrue(TEXT("the SERVER build is deployed"), Rig.SentFor(TEXT("ExecModDeploy")).Contains(TEXT("\"buildId\":\"s-1\"")));
	TestTrue(TEXT("and switched on"), Rig.SentFor(TEXT("ExecModSetEnabled")).Contains(TEXT("\"enabled\":true")));
	const FString Attach = Rig.SentFor(TEXT("ExecModClientDeploy"));
	TestTrue(TEXT("the CLIENT build is attached to the SERVER mod on the run's grid"), Attach.Contains(TEXT("\"buildId\":\"c-2\"")) && Attach.Contains(TEXT("\"name\":\"spinner\""))
		&& Attach.Contains(TEXT("\"gridId\":\"901\"")));

	TestTrue(TEXT("the mode is carried"), Rig.Run.Mode == ECrowdyStudioRunMode::Live);
	TestEqual(TEXT("the revision built is the one asked for"), Rig.Run.Revision, FString(TEXT("7")));
	if (!TestEqual(TEXT("both halves are answered"), Rig.Run.Targets.Num(), 2))
	{
		return true;
	}
	const FCrowdyModRunTarget& Server = Rig.Run.Targets[0];
	TestTrue(TEXT("SERVER first, switched on and waiting for its first call"), Server.Target == ECrowdyStudioTarget::Server && Server.Phase == ECrowdyModRunPhase::Deployed);
	TestTrue(TEXT("its module, mod and digest"), Server.Module == TEXT("spinner") && Server.ModId == TEXT("m-7") && Server.Digest == TEXT("f46465af3d3f"));
	TestEqual(TEXT("running revision 7"), Server.RunningRevision, FString(TEXT("7")));
	const FCrowdyModRunTarget& Client = Rig.Run.Targets[1];
	TestTrue(TEXT("the CLIENT half is attached"), Client.Target == ECrowdyStudioTarget::Client && Client.Phase == ECrowdyModRunPhase::Deployed);
	TestTrue(TEXT("its module and digest"), Client.Module == TEXT("spinner-client") && Client.Digest == TEXT("d-client-1"));
	TestTrue(TEXT("both at revision 7"), Client.RunningRevision == TEXT("7") && Client.SavedRevision == TEXT("7"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunClientOnlyTest, "CrowdySDK.Mods.RunRidesTheStarterForAClientOnlyProject", Flags)
bool FCrowdyModsRunClientOnlyTest::RunTest(const FString& Parameters)
{
	const auto Head = []()
	{
		return TArray<TPair<int32, FString>>{
			{200, ModsRunProject(TEXT("CLIENT_ONLY"), nullptr, TEXT("glow"), TEXT("7"), ModsRunClientFiles)},
			{200, ModsRunBuild(TEXT("execModClientBuild"), TEXT("succeeded"), TEXT("c-2"))},
		};
	};
	const TPair<int32, FString> Attach(200, DataAnswer(TEXT("execModClientDeploy"), ModsRunClient(TEXT("glow"), TEXT("d-glow"), TEXT("h-glow"))));
	const TPair<int32, FString> Consent(200, FString(ModsRunConsentAnswer));
	{
		FRunRig Rig(41060);
		TArray<TPair<int32, FString>> Script = Head();
		Script.Add({200, ModsRunList(TEXT("execMyMods"), {ModJson(TEXT("902"), TEXT("glow"), true, true), ModJson(TEXT("901"), TEXT("other"), true, true)})});
		Script.Add({200, FString(ModsRunStarterAnswer)});
		Script.Add({200, ModsRunBuild(TEXT("execModBuild"), TEXT("succeeded"), TEXT("s-starter"))});
		Script.Add({200, DataAnswer(TEXT("execModDeploy"), ModJson(TEXT("901"), TEXT("glow"), false, false))});
		Script.Add({200, DataAnswer(TEXT("execModSetEnabled"), ModJson(TEXT("901"), TEXT("glow"), true, false))});
		Script.Add(Attach);
		Script.Add(Consent);
		Rig.Script(MoveTemp(Script));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(FString::Printf(TEXT("a CLIENT-only run with no mod to ride went to its end (%s)"), *Rig.Run.Message), Rig.Run.Result == ECrowdyModResult::Success);
		ModsRunExpectOps(*this, TEXT("with no glow of the player's on this grid, the starter is built, deployed as glow and switched on"), Rig,
			{TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild"), TEXT("ExecMyMods"), TEXT("ExecModStarter"), TEXT("ExecModBuild"), TEXT("ExecModDeploy"),
				TEXT("ExecModSetEnabled"), TEXT("ExecModClientDeploy"), TEXT("ExecConsentClientMod")});
		TestTrue(TEXT("the starter's files are built under the CLIENT module's name"), Rig.SentFor(TEXT("ExecModBuild")).Contains(TEXT("// visit"))
			&& Rig.SentFor(TEXT("ExecModBuild")).Contains(TEXT("\"name\":\"glow\"")));
		TestTrue(TEXT("and deployed as glow"), Rig.SentFor(TEXT("ExecModDeploy")).Contains(TEXT("\"buildId\":\"s-starter\"")) && Rig.SentFor(TEXT("ExecModDeploy")).Contains(TEXT("\"name\":\"glow\"")));
		TestTrue(TEXT("the CLIENT build rides glow"), Rig.SentFor(TEXT("ExecModClientDeploy")).Contains(TEXT("\"buildId\":\"c-2\""))
			&& Rig.SentFor(TEXT("ExecModClientDeploy")).Contains(TEXT("\"name\":\"glow\"")));
		TestTrue(TEXT("only the CLIENT half is answered"), Rig.Run.Targets.Num() == 1 && Rig.Run.Targets[0].Target == ECrowdyStudioTarget::Client
			&& Rig.Run.Targets[0].Phase == ECrowdyModRunPhase::Deployed);
	}
	{
		FRunRig Rig(41061);
		TArray<TPair<int32, FString>> Script = Head();
		Script.Add({200, ModsRunList(TEXT("execMyMods"), {ModJson(TEXT("901"), TEXT("glow"), true, true)})});
		Script.Add(Attach);
		Script.Add(Consent);
		Rig.Script(MoveTemp(Script));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(TEXT("a CLIENT-only run whose mod runs went to its end"), Rig.Run.Result == ECrowdyModResult::Success);
		ModsRunExpectOps(*this, TEXT("a switched-on glow is ridden as it is"), Rig,
			{TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild"), TEXT("ExecMyMods"), TEXT("ExecModClientDeploy"), TEXT("ExecConsentClientMod")});
	}
	{
		FRunRig Rig(41062);
		TArray<TPair<int32, FString>> Script = Head();
		Script.Add({200, ModsRunList(TEXT("execMyMods"), {ModJson(TEXT("901"), TEXT("glow"), false, false)})});
		Script.Add({200, DataAnswer(TEXT("execModSetEnabled"), ModJson(TEXT("901"), TEXT("glow"), true, false))});
		Script.Add(Attach);
		Script.Add(Consent);
		Rig.Script(MoveTemp(Script));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(TEXT("a CLIENT-only run whose mod its owner switched off is refused"), Rig.Run.Result == ECrowdyModResult::Refused);
		TestTrue(TEXT("saying to switch it on first"), Rig.Run.Message.Contains(TEXT("glow is switched off")));
		ModsRunExpectOps(*this, TEXT("a switched-off glow is neither switched on, redeployed nor ridden"), Rig,
			{TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild"), TEXT("ExecMyMods")});
		TestTrue(TEXT("the CLIENT half is not published"), Rig.Run.Targets.Num() == 1 && Rig.Run.Targets[0].Phase == ECrowdyModRunPhase::DeployFailed
			&& Rig.Run.Targets[0].RunningRevision.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunConsentTest, "CrowdySDK.Mods.RunConsentsToTheAttachedHalfByItsHash", Flags)
bool FCrowdyModsRunConsentTest::RunTest(const FString& Parameters)
{
	{
		FRunRig Rig(41070);
		Rig.Script(ModsRunPairedScript(TEXT("h-attach-9")));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		const TArray<FString> Names = Rig.Operations();
		const int32 Attach = Names.IndexOfByKey(FString(TEXT("ExecModClientDeploy")));
		TestTrue(TEXT("the consent follows the attach"), Attach != INDEX_NONE && Names.IsValidIndex(Attach + 1) && Names[Attach + 1] == TEXT("ExecConsentClientMod"));
		const FString Consent = Rig.SentFor(TEXT("ExecConsentClientMod"));
		TestTrue(TEXT("naming the attached half and the hash the attach answered"), Consent.Contains(TEXT("\"modId\":\"m-7\"")) && Consent.Contains(TEXT("\"capabilityHash\":\"h-attach-9\"")));
	}
	{
		FRunRig Rig(41071);
		TArray<TPair<int32, FString>> Script = ModsRunPairedScript();
		Script.Last().Value = ErrorAnswer(TEXT("FORBIDDEN"), nullptr, TEXT("run_client_code is missing"));
		Rig.Script(MoveTemp(Script));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(TEXT("a refused consent fails the run"), Rig.Run.Result == ECrowdyModResult::Denied);
		TestTrue(TEXT("the CLIENT half is attached but not served"), Rig.Run.Targets.Num() == 2 && Rig.Run.Targets[1].Phase == ECrowdyModRunPhase::DeployFailed);
		TestTrue(TEXT("and the message says why"), Rig.Run.Message.Contains(TEXT("run_client_code")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunBusyTest, "CrowdySDK.Mods.RunMapsRateLimitedToBusy", Flags)
bool FCrowdyModsRunBusyTest::RunTest(const FString& Parameters)
{
	{
		FRunRig Rig(41080);
		TArray<TPair<int32, FString>> Script = ModsRunPairedScript();
		Script[1].Value = ErrorAnswer(TEXT("RATE_LIMITED"), nullptr, TEXT("one build at a time"));
		Rig.Script(MoveTemp(Script));
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(TEXT("a build refused while another builds is Busy"), Rig.Run.Result == ECrowdyModResult::Busy);
		TestTrue(TEXT("the CLIENT half did not build"), Rig.Run.Targets.Num() == 2 && Rig.Run.Targets[1].Phase == ECrowdyModRunPhase::BuildFailed);
		ModsRunExpectOps(*this, TEXT("and nothing more was sent"), Rig, {TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild")});
	}
	{
		FRig Rig(ErrorAnswer(TEXT("RATE_LIMITED")));
		Rig.Mods->BuildClientMod(TEXT("glow"), Crate(), nullptr, [&Rig](const FCrowdyModBuild& Answer) { Rig.Build = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("the primitive maps it the same way"), Rig.Build.Result == ECrowdyModResult::Busy);
	}
	{
		FRig Rig(ErrorAnswer(TEXT("INTERNAL_SERVER_ERROR"), nullptr, TEXT("build queue is full")));
		Rig.BuildMod(Crate());
		Rig.Pump();
		TestTrue(TEXT("the platform's own failure, such as a full queue, is worth trying again"), Rig.Build.Result == ECrowdyModResult::Failed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunLockTest, "CrowdySDK.Mods.RunIsOneAtATimePerAppInTheProcess", Flags)
bool FCrowdyModsRunLockTest::RunTest(const FString& Parameters)
{
	constexpr int64 App = 41085;
	{
		FRunRig First(App);
		First.Script(ModsRunServerScript());
		First.Start(TEXT("7"));

		FCrowdyModRun Second;
		int32 SecondAnswers = 0;
		First.Mods->RunProject(901, ModsRunProjectId, TEXT("7"), ECrowdyStudioRunMode::DraftTest, [&Second, &SecondAnswers](const FCrowdyModRun& Answer) { Second = Answer; ++SecondAnswers; });
		FRunRig Other(App);
		Other.Start(TEXT("7"));
		First.PumpRun();
		Other.PumpRun();
		First.PumpUntil([&SecondAnswers]() { return SecondAnswers > 0; });
		TestTrue(TEXT("a second run of the app is Busy"), SecondAnswers == 1 && Second.Result == ECrowdyModResult::Busy);
		TestTrue(TEXT("so is one from another game instance of the app"), Other.Runs == 1 && Other.Run.Result == ECrowdyModResult::Busy);
		TestEqual(TEXT("which sent nothing"), Other.Sent.Num(), 0);
		TestTrue(FString::Printf(TEXT("the first ran to its end (%s)"), *First.Run.Message), First.Run.Result == ECrowdyModResult::Success);
		ModsRunExpectOps(*this, TEXT("the refused runs sent nothing"), First, {TEXT("CrowdyStudioProject"), TEXT("ExecModBuild"), TEXT("ExecModDeploy"), TEXT("ExecModSetEnabled")});

		First.Runs = 0;
		First.Script(ModsRunMovedOnScript());
		First.Start(TEXT("7"));
		First.PumpRun();
		TestTrue(TEXT("after a success the app may run again"), First.Run.Result == ECrowdyModResult::RevisionChanged);

		First.Runs = 0;
		First.Script(ModsRunMovedOnScript());
		First.Start(TEXT("7"));
		First.PumpRun();
		TestTrue(TEXT("after a failure too"), First.Run.Result == ECrowdyModResult::RevisionChanged);
	}
	{
		FRunRig Ending(App);
		Ending.Script(ModsRunServerScript());
		Ending.Start(TEXT("7"));
		Ending.Mods->Deinitialize();
		Ending.PumpUntil([&Ending]() { return Ending.Runs > 0; }, 0.f, 20);
		TestEqual(TEXT("a run whose game instance ended answers nobody"), Ending.Runs, 0);

		FRunRig Next(App);
		Next.Script(ModsRunMovedOnScript());
		Next.Start(TEXT("7"));
		Next.PumpRun();
		TestTrue(TEXT("and its app may run again"), Next.Runs == 1 && Next.Run.Result == ECrowdyModResult::RevisionChanged);
		TestEqual(TEXT("the ended run sent nothing past the project read"), Ending.Operations().Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsStatusTest, "CrowdySDK.Mods.StatusReportsARunningRevisionOnlyForTheServedDigest", Flags)
bool FCrowdyModsStatusTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41090);
	Rig.Script(ModsRunPairedScript());
	Rig.Start(TEXT("7"));
	Rig.PumpRun();
	if (!TestTrue(TEXT("revision 7 was deployed"), Rig.Run.Result == ECrowdyModResult::Success))
	{
		return true;
	}
	const auto StatusScript = [](const FString& ServerRows, const TCHAR* ServedDigest)
	{
		return TArray<TPair<int32, FString>>{
			{200, ModsRunProject(TEXT("PAIRED"), TEXT("spinner"), TEXT("spinner-client"), TEXT("8"), ModsRunPairedFiles)},
			{200, FString::Printf(TEXT("{\"data\":{\"execMods\":[%s]}}"), *ServerRows)},
			{200, ModsRunList(TEXT("execGridClientMods"), {ModsRunServed(TEXT("spinner"), ServedDigest)})},
		};
	};
	const FString Running = ModJson(TEXT("901"), TEXT("spinner"), true, true);

	Rig.Sent.Reset();
	Rig.Script(StatusScript(Running, TEXT("d-other")));
	Rig.ReadStatus();
	TestTrue(TEXT("the status is read"), Rig.Status.Result == ECrowdyModResult::Success && !Rig.Status.bRunInFlight);
	ModsRunExpectOps(*this, TEXT("from the platform every time"), Rig, {TEXT("CrowdyStudioProject"), TEXT("ExecMods"), TEXT("ExecGridClientMods")});
	if (TestEqual(TEXT("both halves"), Rig.Status.Targets.Num(), 2))
	{
		const FCrowdyModRunTarget& Server = Rig.Status.Targets[0];
		TestTrue(TEXT("the SERVER mod runs"), Server.Phase == ECrowdyModRunPhase::Running && Server.Version == 3 && Server.Digest == TEXT("f46465af3d3f"));
		TestEqual(TEXT("with the revision deployed under its digest"), Server.RunningRevision, FString(TEXT("7")));
		TestEqual(TEXT("and the project's saved revision"), Server.SavedRevision, FString(TEXT("8")));
		const FCrowdyModRunTarget& Client = Rig.Status.Targets[1];
		TestTrue(TEXT("the CLIENT half served is another"), Client.Phase == ECrowdyModRunPhase::Deployed && Client.Digest == TEXT("d-other"));
		TestTrue(TEXT("so no revision is claimed for it"), Client.RunningRevision.IsEmpty());
	}

	Rig.Script(StatusScript(Running, TEXT("d-client-1")));
	Rig.ReadStatus();
	TestTrue(TEXT("a served CLIENT digest that matches gives its revision"), Rig.Status.Targets.Num() == 2 && Rig.Status.Targets[1].RunningRevision == TEXT("7"));

	Rig.Script(StatusScript(Running.Replace(TEXT("f46465af3d3f"), TEXT("0123abcd")), TEXT("d-client-1")));
	Rig.ReadStatus();
	TestTrue(TEXT("a SERVER mod redeployed elsewhere claims no revision"), Rig.Status.Targets.Num() == 2 && Rig.Status.Targets[0].RunningRevision.IsEmpty()
		&& Rig.Status.Targets[0].Digest == TEXT("0123abcd"));

	Rig.Script(StatusScript(FString(), TEXT("d-client-1")));
	Rig.ReadStatus();
	TestTrue(TEXT("a SERVER mod gone from the grid has not started and claims nothing"), Rig.Status.Targets.Num() == 2
		&& Rig.Status.Targets[0].Phase == ECrowdyModRunPhase::NotStarted && Rig.Status.Targets[0].RunningRevision.IsEmpty() && Rig.Status.Targets[0].Digest.IsEmpty());

	FRunRig Refused(41091);
	Refused.Mods->GetProjectStatus(0, ModsRunProjectId, [&Refused](const FCrowdyModProjectStatus& Answer) { Refused.Status = Answer; ++Refused.Statuses; });
	Refused.PumpUntil([&Refused]() { return Refused.Statuses > 0; });
	TestTrue(TEXT("a status of grid 0 is refused without a request"), Refused.Status.Result == ECrowdyModResult::Refused && Refused.Sent.Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunPlayerChangeTest, "CrowdySDK.Mods.RunEndsWhenThePlayerOrAppChanges", Flags)
bool FCrowdyModsRunPlayerChangeTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Paired = {TEXT("CrowdyStudioProject"), TEXT("ExecModClientBuild"), TEXT("ExecModBuild"), TEXT("ExecModDeploy"), TEXT("ExecModSetEnabled"),
		TEXT("ExecModClientDeploy")};
	struct FCase
	{
		int32 ChangeAfter;
		bool bApp;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{1, false, TEXT("another player signed in while the project was read")},
		{4, false, TEXT("another player signed in while the SERVER half deployed")},
		{6, false, TEXT("another player signed in while the CLIENT half was attached")},
		{1, true, TEXT("the app changed while the project was read")},
		{6, true, TEXT("the app changed while the CLIENT half was attached")},
	};
	int64 AppId = 41100;
	for (const FCase& Case : Cases)
	{
		const int64 RunApp = ++AppId;
		FRunRig Rig(RunApp);
		Rig.Script(ModsRunPairedScript());
		Rig.OnSent = [&Rig, &Case, RunApp](int32 Count)
		{
			if (Count != Case.ChangeAfter)
			{
				return;
			}
			if (Case.bApp)
			{
				Rig.Mods->SetAppIdForTest(RunApp + 1000);
				return;
			}
			Rig.Mods->SetUserIdForTest(ModsRunOtherPlayer);
		};
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(FString::Printf(TEXT("%s: the run answers once, Failed"), Case.What), Rig.Runs == 1 && Rig.Run.Result == ECrowdyModResult::Failed);
		TestTrue(FString::Printf(TEXT("%s: saying why"), Case.What), Rig.Run.Message.Contains(TEXT("changed")));
		TArray<FString> Expected = Paired;
		Expected.SetNum(Case.ChangeAfter);
		ModsRunExpectOps(*this, *FString::Printf(TEXT("%s: nothing more was sent"), Case.What), Rig, Expected);
		TestTrue(FString::Printf(TEXT("%s: no consent was given"), Case.What), Rig.SentFor(TEXT("ExecConsentClientMod")).IsEmpty());

		Rig.OnSent = nullptr;
		Rig.Mods->SetAppIdForTest(RunApp);
		Rig.Mods->SetUserIdForTest(ModsRunPlayer);
		Rig.Runs = 0;
		Rig.Script(ModsRunMovedOnScript());
		Rig.Start(TEXT("7"));
		Rig.PumpRun();
		TestTrue(FString::Printf(TEXT("%s: the app is free to run again"), Case.What), Rig.Run.Result == ECrowdyModResult::RevisionChanged);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsBuildPlayerChangeTest, "CrowdySDK.Mods.BuildStopsWhenThePlayerChanges", Flags)
bool FCrowdyModsBuildPlayerChangeTest::RunTest(const FString& Parameters)
{
	{
		FRunRig Rig(41120);
		Rig.Script({
			{200, ModsRunBuild(TEXT("execModBuild"), TEXT("queued"), TEXT("s-1"))},
			{200, ModsRunBuild(TEXT("execModBuildStatus"), TEXT("building"), TEXT("s-1"))},
		});
		Rig.OnSent = [&Rig](int32 Count)
		{
			if (Count == 2)
			{
				Rig.Mods->SetUserIdForTest(ModsRunOtherPlayer);
			}
		};
		Rig.BuildMod(Crate());
		Rig.PumpUntil([&Rig]() { return Rig.Sent.Num() >= 2; }, UCrowdyMods::BuildPollSeconds);
		Rig.PumpUntil([&Rig]() { return Rig.Answers > 0; }, 0.f, 20);
		TestTrue(TEXT("a status read answered after another player signed in ends the build at once, not as a dropped read"),
			Rig.Answers == 1 && Rig.Build.Result == ECrowdyModResult::Failed && Rig.Build.Message.Contains(TEXT("changed")));
		Rig.PumpUntil([]() { return false; }, UCrowdyMods::BuildPollSeconds, 3);
		TestEqual(TEXT("and no status is read again"), Rig.Sent.Num(), 2);
	}
	{
		FRunRig Rig(41121);
		Rig.Script({{200, ModsRunBuild(TEXT("execModBuild"), TEXT("queued"), TEXT("s-1"))}});
		Rig.BuildMod(Crate());
		Rig.PumpUntil([&Rig]() { return Rig.Progress.Num() > 0; });
		Rig.Mods->SetUserIdForTest(ModsRunOtherPlayer);
		Rig.Pump(UCrowdyMods::BuildPollSeconds);
		TestTrue(TEXT("a build whose player changed between status reads ends Failed"), Rig.Answers == 1 && Rig.Build.Result == ECrowdyModResult::Failed);
		TestEqual(TEXT("without reading its status as the other player"), Rig.Sent.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsStatusPlayerChangeTest, "CrowdySDK.Mods.StatusStopsWhenThePlayerChanges", Flags)
bool FCrowdyModsStatusPlayerChangeTest::RunTest(const FString& Parameters)
{
	FRunRig Rig(41130);
	Rig.Script({
		{200, ModsRunProject(TEXT("PAIRED"), TEXT("spinner"), TEXT("spinner-client"), TEXT("7"), ModsRunPairedFiles)},
		{200, ModsRunList(TEXT("execMods"), {ModJson(TEXT("901"), TEXT("spinner"), true, true)})},
		{200, ModsRunList(TEXT("execGridClientMods"), {ModsRunServed(TEXT("spinner"), TEXT("d-1"))})},
	});
	Rig.OnSent = [&Rig](int32 Count)
	{
		if (Count == 1)
		{
			Rig.Mods->SetUserIdForTest(ModsRunOtherPlayer);
		}
	};
	Rig.ReadStatus();
	TestTrue(TEXT("a status read whose player changed answers once, Failed"), Rig.Statuses == 1 && Rig.Status.Result == ECrowdyModResult::Failed);
	TestTrue(TEXT("claiming nothing"), Rig.Status.Targets.IsEmpty());
	ModsRunExpectOps(*this, TEXT("and reads nothing as the other player"), Rig, {TEXT("CrowdyStudioProject")});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunModulesTest, "CrowdySDK.Mods.RunRefusesModulesOtherThanTheOnesAsked", Flags)
bool FCrowdyModsRunModulesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Server;
		const TCHAR* Client;
		ECrowdyModResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{TEXT("spinner"), TEXT("glow-client"), ECrowdyModResult::RevisionChanged, TEXT("a CLIENT module renamed since it was asked")},
		{TEXT("renamed"), TEXT(""), ECrowdyModResult::RevisionChanged, TEXT("a SERVER module renamed since it was asked")},
		{TEXT("spinner"), TEXT("spinner-client"), ECrowdyModResult::Success, TEXT("the modules asked for")},
		{TEXT(""), TEXT(""), ECrowdyModResult::Success, TEXT("no modules named, so none checked")},
	};
	int64 AppId = 41140;
	for (const FCase& Case : Cases)
	{
		FRunRig Rig(++AppId);
		Rig.Script(ModsRunPairedScript());
		Rig.Mods->RunProject(901, ModsRunProjectId, TEXT("7"), ECrowdyStudioRunMode::DraftTest, Case.Server, Case.Client,
			[&Rig](const FCrowdyModRun& Answer) { Rig.Run = Answer; ++Rig.Runs; });
		Rig.PumpRun();
		TestTrue(FString::Printf(TEXT("%s ends %d (%s)"), Case.What, static_cast<int32>(Case.Expected), *Rig.Run.Message), Rig.Run.Result == Case.Expected);
		if (Case.Expected != ECrowdyModResult::Success)
		{
			ModsRunExpectOps(*this, *FString::Printf(TEXT("%s: nothing was built"), Case.What), Rig, {TEXT("CrowdyStudioProject")});
			TestTrue(FString::Printf(TEXT("%s: the message names the module"), Case.What), Rig.Run.Message.Contains(TEXT("module")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsRunCommitTest, "CrowdySDK.Mods.RunRefusesARevisionWhoseCommitDiffers", Flags)
bool FCrowdyModsRunCommitTest::RunTest(const FString& Parameters)
{
	const FString GitHubProject = ModsRunProject(TEXT("SERVER_ONLY"), TEXT("spinner"), nullptr, TEXT("7"), ModsRunPairedFiles)
		.Replace(TEXT("\"source\":\"STUDIO\""), TEXT("\"source\":\"GITHUB\""))
		.Replace(TEXT("\"githubOwner\":null"), TEXT("\"githubOwner\":\"acme\""))
		.Replace(TEXT("\"githubRepo\":null"), TEXT("\"githubRepo\":\"spinner\""))
		.Replace(TEXT("\"githubBranch\":null"), TEXT("\"githubBranch\":\"main\""))
		.Replace(TEXT("\"githubSha\":null"), TEXT("\"githubSha\":\"abc123\""));
	FRunRig Rig(41150);
	Rig.Script({{200, GitHubProject}});
	Rig.Start(TEXT("7@def456"));
	Rig.PumpRun();
	TestTrue(TEXT("a revision whose commit is another is refused"), Rig.Run.Result == ECrowdyModResult::RevisionChanged);
	ModsRunExpectOps(*this, TEXT("before anything is built"), Rig, {TEXT("CrowdyStudioProject")});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModsStudioDefaultsTest, "CrowdySDK.Mods.StudioAnswersDefaultToNothing", Flags)
bool FCrowdyModsStudioDefaultsTest::RunTest(const FString& Parameters)
{
	static_assert(std::is_same_v<decltype(FCrowdyModClient::GridId), int64>, "a grid id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyModClient::OwnerUserId), int64>, "a user id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyModRun::GridId), int64>, "a grid id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyModProjectStatus::GridId), int64>, "a grid id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyModRunTarget::Version), int64>, "a mod's version is a BigInt");
	const FCrowdyModClientInfo Info;
	TestTrue(TEXT("a default deploy is not a success"), Info.Result != ECrowdyModResult::Success && Info.Client.ModId.IsEmpty());
	const FCrowdyModRun Run;
	TestTrue(TEXT("a default run is not a success"), Run.Result != ECrowdyModResult::Success && Run.Targets.IsEmpty() && Run.Revision.IsEmpty());
	const FCrowdyModProjectStatus Status;
	TestTrue(TEXT("a default status is not a success"), Status.Result != ECrowdyModResult::Success && !Status.bRunInFlight);
	const FCrowdyModRunTarget Target;
	TestTrue(TEXT("a default target has not started"), Target.Phase == ECrowdyModRunPhase::NotStarted && Target.RunningRevision.IsEmpty() && Target.Digest.IsEmpty());
	const FCrowdyModDiagnostic Diagnostic;
	TestTrue(TEXT("a default diagnostic is the compiler's"), Diagnostic.Source == ECrowdyModDiagnosticSource::Rustc);
	TestEqual(TEXT("the sources are rustc, local, runtime"), static_cast<int32>(ECrowdyModDiagnosticSource::Runtime), 2);
	return true;
}

#endif
