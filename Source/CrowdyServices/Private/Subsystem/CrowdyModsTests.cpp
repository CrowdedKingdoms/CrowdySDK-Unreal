#include "Subsystem/CrowdyMods.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

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

		explicit FRig(const FString& Body, int32 Status = 200, bool bSignedIn = true)
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
			Mods->InitializeForTest(bSignedIn ? Client : nullptr, TestApp, TEXT("example.test"));
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

#endif
