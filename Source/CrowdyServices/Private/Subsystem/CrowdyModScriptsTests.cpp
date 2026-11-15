#include "Subsystem/CrowdyModScripts.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "CrowdyModScriptsPlatform.h"
#include "CrowdyModScriptsTestTypes.h"
#include "Dom/JsonObject.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
#include "CrowdyCppCrypto.h"
#include "CrowdyModScriptRuntime.h"
#include "CrowdyNativeExec.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
THIRD_PARTY_INCLUDES_START
#include "crowdy/core/crypto.hpp"
THIRD_PARTY_INCLUDES_END
#endif

namespace CrowdyModScriptsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString AnswerText(const TArray<uint8>& Answer, FString& OutKind, bool& bOutOk)
	{
		const FString Json = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Answer.GetData()), Answer.Num());
		TSharedPtr<FJsonObject> Object;
		const TSharedPtr<FJsonObject>* Error = nullptr;
		FString Message;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) && Object.IsValid() && Object->TryGetBoolField(TEXT("ok"), bOutOk)
			&& Object->TryGetObjectField(TEXT("error"), Error))
		{
			(*Error)->TryGetStringField(TEXT("kind"), OutKind);
			(*Error)->TryGetStringField(TEXT("message"), Message);
		}
		return Message;
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	// Mod Scripts enter no grid where the runtime is not built in, so the flow's helpers and tests need it.
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	constexpr int64 PlotId = 100;
	constexpr int64 AuthorA = 7;
	constexpr int64 AuthorB = 8;

	using FCall = UCrowdyModScripts::FPlatformCall;
	using FFakeReply = UCrowdyModScripts::FPlatformReply;

	/** Holds every platform call until the test answers it. */
	struct FFakePlatform
	{
		struct FPending
		{
			FCall Call;
			TFunction<void(const FFakeReply&)> OnReply;
		};
		TArray<FPending> Pending;

		int32 Count(ECrowdyModScriptsCall Kind) const
		{
			int32 Found = 0;
			for (const FPending& Entry : Pending)
			{
				Found += Entry.Call.Call == Kind ? 1 : 0;
			}
			return Found;
		}

		const FCall* Find(ECrowdyModScriptsCall Kind, const FString& ModId = FString()) const
		{
			const FPending* Entry = Pending.FindByPredicate([Kind, &ModId](const FPending& Candidate)
			{
				return Candidate.Call.Call == Kind && (ModId.IsEmpty() || Candidate.Call.ModId.Equals(ModId, ESearchCase::CaseSensitive));
			});
			return Entry ? &Entry->Call : nullptr;
		}

		/** Answers the oldest pending call of Kind, for ModId when one is named; false when there is none. */
		bool Answer(ECrowdyModScriptsCall Kind, const FFakeReply& Reply, const FString& ModId = FString())
		{
			const int32 Index = Pending.IndexOfByPredicate([Kind, &ModId](const FPending& Candidate)
			{
				return Candidate.Call.Call == Kind && (ModId.IsEmpty() || Candidate.Call.ModId.Equals(ModId, ESearchCase::CaseSensitive));
			});
			if (Index == INDEX_NONE)
			{
				return false;
			}
			const FPending Taken = MoveTemp(Pending[Index]);
			Pending.RemoveAt(Index);
			Taken.OnReply(Reply);
			return true;
		}
	};

	struct FRowSpec
	{
		FString ModId;
		int64 AuthorId = AuthorA;
		FString Digest = TEXT("d1");
		FString Hash = TEXT("h1");
		FString AuthorHash = TEXT("a1");
		int32 TickIntervalMs = 100;
		bool bConsented = false;
		bool bTrustsAuthor = false;
		TArray<FString> Calls = {TEXT("hud_set")};
	};

	FFakeReply Ok()
	{
		FFakeReply Reply;
		Reply.bOk = true;
		return Reply;
	}

	FFakeReply Refused(const FString& Code)
	{
		FFakeReply Reply;
		Reply.Code = Code;
		Reply.Message = FString::Printf(TEXT("%s: refused"), *Code);
		return Reply;
	}

	FString Summary(const TArray<FString>& Calls, const TArray<FString>& Groups)
	{
		TArray<TSharedPtr<FJsonValue>> CallValues;
		for (const FString& Call : Calls)
		{
			CallValues.Add(MakeShared<FJsonValueString>(Call));
		}
		TArray<TSharedPtr<FJsonValue>> GroupValues;
		for (const FString& Group : Groups)
		{
			GroupValues.Add(MakeShared<FJsonValueString>(Group));
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("version"), 1);
		Object->SetStringField(TEXT("target"), TEXT("client"));
		Object->SetArrayField(TEXT("hostFunctions"), CallValues);
		Object->SetArrayField(TEXT("capabilityGroups"), GroupValues);
		FString Json;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}

	TSharedPtr<FJsonValue> Row(const FRowSpec& Spec, int64 GridId)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("modId"), Spec.ModId);
		Object->SetStringField(TEXT("name"), Spec.ModId + TEXT("-name"));
		Object->SetStringField(TEXT("gridId"), FString::Printf(TEXT("%lld"), GridId));
		Object->SetStringField(TEXT("authorId"), FString::Printf(TEXT("%lld"), Spec.AuthorId));
		Object->SetField(TEXT("listingId"), MakeShared<FJsonValueNull>());
		Object->SetNumberField(TEXT("clientVersion"), 3);
		Object->SetStringField(TEXT("digest"), Spec.Digest);
		Object->SetStringField(TEXT("capabilitySummaryJson"), Summary(Spec.Calls, {TEXT("present")}));
		Object->SetStringField(TEXT("capabilityHash"), Spec.Hash);
		Object->SetNumberField(TEXT("tickIntervalMs"), Spec.TickIntervalMs);
		Object->SetBoolField(TEXT("callerConsented"), Spec.bConsented);
		Object->SetStringField(TEXT("authorCapabilitySummaryJson"),
			Summary({TEXT("hud_set"), TEXT("voxel_set"), TEXT("input_key")}, {TEXT("present"), TEXT("world_write"), TEXT("input")}));
		Object->SetStringField(TEXT("authorCapabilityHash"), Spec.AuthorHash);
		Object->SetBoolField(TEXT("callerTrustsAuthor"), Spec.bTrustsAuthor);
		Object->SetStringField(TEXT("updatedAt"), TEXT("2026-11-15T00:00:00Z"));
		return MakeShared<FJsonValueObject>(Object);
	}

	FFakeReply Listing(const TArray<FRowSpec>& Specs, int64 GridId = PlotId)
	{
		FFakeReply Reply = Ok();
		for (const FRowSpec& Spec : Specs)
		{
			Reply.Rows.Add(Row(Spec, GridId));
		}
		return Reply;
	}

	/** The world grid covers everything; the plot, 10 by 10 by 2 chunks from the origin, decides. */
	FFakeReply Covering(int64 GridId)
	{
		FFakeReply Reply = Ok();
		FCrowdyGridPermissionRow& Plot = Reply.Grids.AddDefaulted_GetRef();
		Plot.GridId = GridId;
		Plot.HighChunkX = 9;
		Plot.HighChunkY = 9;
		Plot.HighChunkZ = 1;
		Plot.bDecidesEdits = true;
		FCrowdyGridPermissionRow& World = Reply.Grids.AddDefaulted_GetRef();
		World.GridId = 1;
		World.LowChunkX = World.LowChunkY = World.LowChunkZ = -1000;
		World.HighChunkX = World.HighChunkY = World.HighChunkZ = 1000;
		return Reply;
	}

	struct FHarness
	{
		TSharedRef<FFakePlatform> Fake = MakeShared<FFakePlatform>();
		TStrongObjectPtr<UCrowdyModScripts> Scripts;
		TStrongObjectPtr<UCrowdyModScriptsTestRecorder> Recorder;

		explicit FHarness(UCrowdyGameSession* Session = nullptr)
			: Scripts(NewObject<UCrowdyModScripts>(GetTransientPackage()))
			, Recorder(NewObject<UCrowdyModScriptsTestRecorder>(GetTransientPackage()))
		{
			Scripts->InitializeForTest([Held = Fake](const FCall& Call, TFunction<void(const FFakeReply&)> OnReply)
			{
				Held->Pending.Add({Call, MoveTemp(OnReply)});
			}, Session);
			Scripts->OnModScriptPrompt.AddDynamic(Recorder.Get(), &UCrowdyModScriptsTestRecorder::HandlePrompt);
			Scripts->OnModScriptPromptWithdrawn.AddDynamic(Recorder.Get(), &UCrowdyModScriptsTestRecorder::HandleWithdrawn);
			Scripts->OnModScriptStarted.AddDynamic(Recorder.Get(), &UCrowdyModScriptsTestRecorder::HandleStarted);
			Scripts->OnModScriptStopped.AddDynamic(Recorder.Get(), &UCrowdyModScriptsTestRecorder::HandleStopped);
			Scripts->OnModScriptError.AddDynamic(Recorder.Get(), &UCrowdyModScriptsTestRecorder::HandleError);
			Scripts->SetHost(TScriptInterface<ICrowdyModScriptHost>(Recorder.Get()));
		}

		~FHarness()
		{
			Scripts->LeaveGrid();
		}

		/** Stands in chunk (5, 5, 0) of the plot. */
		bool Enter(int64 GridId = PlotId)
		{
			Scripts->EnterGrid(5, 5, 0);
			return Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Covering(GridId));
		}

		bool List(const TArray<FRowSpec>& Specs, int64 GridId = PlotId)
		{
			return Fake->Answer(ECrowdyModScriptsCall::ListMods, Listing(Specs, GridId));
		}

		bool IsRunning(const FString& ModId) const
		{
			return Scripts->GetRunning().ContainsByPredicate([&ModId](const FCrowdyModScript& Script) { return Script.ModId == ModId; });
		}
	};

	/** One of the runtime's test modules, served as a checked artifact. */
	bool ModuleArtifact(FAutomationTestBase& Test, FFakeReply& Out, const TCHAR* File)
	{
		const TSharedPtr<FCrowdyModScriptRuntime> Runtime = FCrowdyModScriptRuntime::Get();
		if (!Runtime || !Runtime->IsAvailable())
		{
			Test.AddError(FString::Printf(TEXT("Mod Script runtime unavailable: %s"), Runtime ? *Runtime->GetUnavailableReason() : TEXT("module not started")));
			return false;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		const FString Path = Plugin
			? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"), TEXT("CrowdyModScriptRuntime"), TEXT("Tests"), TEXT("Modules"), File) : FString();
		Out = Ok();
		if (Path.IsEmpty() || !FFileHelper::LoadFileToArray(Out.Bytes, *Path))
		{
			Test.AddError(FString::Printf(TEXT("Cannot read the runtime's Tests/Modules/%s"), File));
			return false;
		}
		uint8 Digest[32];
		if (!GetCrowdyCppCrypto().sha256(crowdy::Bytes(Out.Bytes.GetData(), static_cast<size_t>(Out.Bytes.Num())), Digest))
		{
			Test.AddError(FString::Printf(TEXT("Cannot hash %s"), File));
			return false;
		}
		for (const uint8 Byte : Digest)
		{
			Out.Digest += FString::Printf(TEXT("%02x"), Byte);
		}
		Out.FuelPerDispatch = 100000000;
		Out.TickIntervalMs = 100;
		return true;
	}

	bool HelloArtifact(FAutomationTestBase& Test, FFakeReply& Out)
	{
		return ModuleArtifact(Test, Out, TEXT("hello.wasm"));
	}

	/** Drains the game thread's queue, where a compiled module is handed back, until Done or 3 seconds pass. */
	void PumpUntil(const TFunctionRef<bool()> Done)
	{
		const double DeadlineSeconds = FPlatformTime::Seconds() + 3.0;
		while (!Done() && FPlatformTime::Seconds() < DeadlineSeconds)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
	}

	/** Answers the pending fetch of ModId with the hello module and waits for it to start. */
	bool FetchAndRun(FHarness& Harness, const FString& ModId, const FFakeReply& Hello)
	{
		if (!Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Hello, ModId))
		{
			return false;
		}
		PumpUntil([&Harness, &ModId]() { return Harness.IsRunning(ModId); });
		return Harness.IsRunning(ModId);
	}

	/** Lists Specs, all served the hello module, answers each fetch and waits for the agreed ones to start. */
	bool ListAndRun(FHarness& Harness, TArray<FRowSpec> Specs, const FFakeReply& Hello)
	{
		int32 NumAgreed = 0;
		for (FRowSpec& Spec : Specs)
		{
			Spec.Digest = Hello.Digest;
			NumAgreed += Spec.bConsented || Spec.bTrustsAuthor ? 1 : 0;
		}
		if (!Harness.List(Specs))
		{
			return false;
		}
		while (Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Hello))
		{
		}
		PumpUntil([&Harness, NumAgreed]() { return Harness.Scripts->GetRunning().Num() >= NumAgreed; });
		return true;
	}

	FRowSpec Served(const FString& ModId, int64 AuthorId, const FString& Digest)
	{
		FRowSpec Spec;
		Spec.ModId = ModId;
		Spec.AuthorId = AuthorId;
		Spec.Digest = Digest;
		Spec.Hash = TEXT("h-") + ModId;
		return Spec;
	}

	FRowSpec Unagreed(const FString& ModId, int64 AuthorId = AuthorA, const FString& AuthorHash = TEXT("a1"))
	{
		FRowSpec Spec;
		Spec.ModId = ModId;
		Spec.AuthorId = AuthorId;
		Spec.AuthorHash = AuthorHash;
		Spec.Hash = TEXT("h-") + ModId;
		return Spec;
	}

	FRowSpec Agreed(const FString& ModId, int64 AuthorId = AuthorA)
	{
		FRowSpec Spec = Unagreed(ModId, AuthorId);
		Spec.bConsented = true;
		return Spec;
	}

	/** A listing whose first row is not a Mod Script row. */
	FFakeReply ListingWithJunk(const TArray<FRowSpec>& Specs)
	{
		FFakeReply Reply = Listing(Specs);
		Reply.Rows.Insert(MakeShared<FJsonValueString>(TEXT("junk")), 0);
		return Reply;
	}
#endif
}

using namespace CrowdyModScriptsTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsUnavailableTest, "CrowdySDK.ModScripts.SurfaceRunsNothing", Flags)
bool FCrowdyModScriptsUnavailableTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyModScripts> Scripts(NewObject<UCrowdyModScripts>(GetTransientPackage()));
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	const TSharedPtr<FCrowdyModScriptRuntime> Runtime = FCrowdyModScriptRuntime::Get();
	TestEqual(TEXT("available exactly when the runtime is"), Scripts->IsAvailable(), Runtime.IsValid() && Runtime->IsAvailable());
#else
	TestFalse(TEXT("a build without the runtime cannot run Mod Scripts"), Scripts->IsAvailable());
#endif

	Scripts->EnterGrid(1, 2, 3);
	Scripts->AnswerPrompt(1, true);
	Scripts->Revoke(TEXT("mod"));
	Scripts->ForgetAuthor(7);
	TestEqual(TEXT("with no platform, entering a chunk finds no grid"), Scripts->GetGrid().GridId, static_cast<int64>(0));
	TestEqual(TEXT("and runs nothing"), Scripts->GetRunning().Num(), 0);
	Scripts->LeaveGrid();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsShapeDefaultsTest, "CrowdySDK.ModScripts.ShapeDefaults", Flags)
bool FCrowdyModScriptsShapeDefaultsTest::RunTest(const FString& Parameters)
{
	const FCrowdyModScriptShape Shape;
	TestEqual(TEXT("a shape is a box"), Shape.Kind, ECrowdyModScriptShapeKind::Box);
	TestEqual(TEXT("one voxel on each axis"), Shape.Size, FVector::OneVector);
	TestTrue(TEXT("unrotated"), Shape.Rotation.Equals(FQuat::Identity));
	TestEqual(TEXT("white"), Shape.Color, FLinearColor::White);
	TestEqual(TEXT("opaque"), Shape.Opacity, 1.f);
	TestTrue(TEXT("and visible"), Shape.bVisible && !Shape.bUnlit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHostIsImplementableTest, "CrowdySDK.ModScripts.HostIsImplementable", Flags)
bool FCrowdyModScriptsHostIsImplementableTest::RunTest(const FString& Parameters)
{
	const UClass* Host = UCrowdyModScriptHost::StaticClass();
	TestTrue(TEXT("a Blueprint can implement the host"), Host->GetBoolMetaData(TEXT("IsBlueprintBase")));
	const TCHAR* Methods[] = {
		TEXT("DrainClicks"), TEXT("ReadChunk"), TEXT("ListPlayers"), TEXT("SetVoxel"),
		TEXT("ShowHud"), TEXT("DrawOverlay"), TEXT("ClearScript")};
	for (const TCHAR* Name : Methods)
	{
		const UFunction* Method = Host->FindFunctionByName(Name);
		TestTrue(FString::Printf(TEXT("%s is a Blueprint-implementable host call"), Name),
			Method && Method->HasAnyFunctionFlags(FUNC_BlueprintEvent));
	}
	const UFunction* SetVoxel = Host->FindFunctionByName(TEXT("SetVoxel"));
	TestTrue(TEXT("SetVoxel names the script that wrote"),
		SetVoxel && SetVoxel->FindPropertyByName(TEXT("Script")) && SetVoxel->FindPropertyByName(TEXT("Write"))
		&& SetVoxel->FindPropertyByName(TEXT("Refusal")));
	return true;
}

namespace CrowdyModScriptsTests
{
	const TCHAR* const HudRequest = TEXT("{\"fn\":\"hud_set\",\"args\":{\"payload\":{\"text\":\"hi\"}}}");

	// The refusal's message, empty when the call passed.
	FString CheckAt(const FString& Request, CrowdyModScriptsHostCalls::FBudget& Budget, double NowSeconds,
		const TArray<FString>& Agreed = TArray<FString>{TEXT("hud_set"), TEXT("overlay_draw")})
	{
		CrowdyModScriptsHostCalls::FChecked Call;
		TArray<uint8> Denied;
		if (CrowdyModScriptsHostCalls::Check(Utf8(Request), Agreed, true, Budget, NowSeconds, Call, Denied))
		{
			return FString();
		}
		FString Kind;
		bool bOk = true;
		const FString Message = AnswerText(Denied, Kind, bOk);
		return !bOk && Kind == TEXT("denied") ? Message : FString(TEXT("<not a denial>"));
	}

	FString Check(const FString& Request, const TArray<FString>& Agreed = TArray<FString>{TEXT("hud_set"), TEXT("overlay_draw")})
	{
		CrowdyModScriptsHostCalls::FBudget Budget;
		return CheckAt(Request, Budget, 0.0, Agreed);
	}

	TSharedRef<FJsonObject> Args(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>();
	}

	/** A 2 by 2 by 1 chunk grid from the origin: 0 to 32, 0 to 32 and 0 to 16 voxels. */
	TArray<FCrowdyModScriptShape> Shapes(const FString& Payload)
	{
		FCrowdyModScriptGrid Box;
		Box.GridId = 1;
		Box.HighChunkX = 1;
		Box.HighChunkY = 1;
		TArray<FCrowdyModScriptShape> Out;
		CrowdyModScriptsHostCalls::OverlayShapes(*Args(FString::Printf(TEXT("{\"payload\":%s}"), *Payload)), Box, Out);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHostCallChecksTest, "CrowdySDK.ModScripts.HostCallChecks", Flags)
bool FCrowdyModScriptsHostCallChecksTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	TestEqual(TEXT("an agreed presentation call passes"), Check(HudRequest), FString());
	TestEqual(TEXT("a call without arguments is refused, as in the browser"), Check(TEXT("{\"fn\":\"overlay_draw\"}")),
		FString(TEXT("host call args must be a plain object")));
	TestTrue(TEXT("hud_set is served with no host set"), IsServed(TEXT("hud_set"), false) && IsServed(TEXT("overlay_draw"), false));
	TestTrue(TEXT("the platform's calls are served with no host set"), IsServed(TEXT("user_state_get"), false) && IsServed(TEXT("user_state_set"), false)
		&& IsServed(TEXT("emit_spatial"), false) && IsServed(TEXT("emit_channel"), false));
	const TCHAR* NotOffered[] = {TEXT("avatar_state_get"), TEXT("send_text"), TEXT("input_key"), TEXT("nonsense")};
	for (const TCHAR* Call : NotOffered)
	{
		TestEqual(FString::Printf(TEXT("%s is not offered"), Call), Check(FString::Printf(TEXT("{\"fn\":\"%s\",\"args\":{}}"), Call), {Call}),
			FString::Printf(TEXT("host call '%s' is not offered by this game"), Call));
	}
	const FString Outside = TEXT("host call is outside the capabilities the player consented to");
	TestEqual(TEXT("an unagreed call is refused"), Check(HudRequest, {TEXT("overlay_draw")}), Outside);
	TestEqual(TEXT("the agreed names compare with case"), Check(HudRequest, {TEXT("HUD_SET")}), Outside);

	const FString Plain = TEXT("host call args must be a plain object");
	TestEqual(TEXT("array arguments are refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":[]}")), Plain);
	TestEqual(TEXT("null arguments are refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":null}")), Plain);
	TestEqual(TEXT("string arguments are refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":\"x\"}")), Plain);
	TestEqual(TEXT("authority is refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":{\"authority\":\"server\"}}")), FString(TEXT("host call cannot override 'authority'")));
	TestEqual(TEXT("bindingKind is refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":{\"bindingKind\":1}}")), FString(TEXT("host call cannot override 'bindingKind'")));
	TestEqual(TEXT("binding_kind is refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":{\"binding_kind\":1}}")), FString(TEXT("host call cannot override 'binding_kind'")));
	TestEqual(TEXT("in any case"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":{\"Authority\":1}}")), FString(TEXT("host call cannot override 'authority'")));
	TestEqual(TEXT("an extra envelope field is refused"), Check(TEXT("{\"fn\":\"hud_set\",\"args\":{},\"id\":1}")),
		FString(TEXT("unexpected hostcall envelope field 'id'")));

	const TCHAR* Malformed[] = {
		TEXT("not json"), TEXT("{\"args\":{}}"), TEXT("{\"fn\":7,\"args\":{}}"), TEXT("{\"fn\":\"Bad Name\",\"args\":{}}"),
		TEXT("{\"fn\":\"say\\\"x\",\"args\":{}}"), TEXT("[\"fn\"]")};
	for (const TCHAR* Request : Malformed)
	{
		TestEqual(FString::Printf(TEXT("%s is malformed"), Request), Check(Request), FString(TEXT("malformed host call")));
	}
	const FString Deep = FString::Printf(TEXT("{\"fn\":\"hud_set\",\"args\":{\"a\":%s%s}}"), *FString::ChrN(200, TEXT('[')), *FString::ChrN(200, TEXT(']')));
	TestEqual(TEXT("an over-deep request is not parsed"), Check(Deep), FString(TEXT("malformed host call")));
	const FString Large = FString::Printf(TEXT("{\"fn\":\"hud_set\",\"args\":{\"pad\":\"%s\"}}"), *FString::ChrN(MaxRequestBytes, TEXT('x')));
	TestEqual(TEXT("a request over its limit is refused unread"), Check(Large), FString(TEXT("host call request too large")));
	const FString Over = FString::Printf(TEXT("{\"fn\":\"hud_set\",\"args\":{\"pad\":\"%s\"}}"), *FString::ChrN(MaxArgsBytes, TEXT('x')));
	TestEqual(TEXT("arguments over 256 KiB are refused"), Check(Over), FString(TEXT("host call args exceed the browser sandbox limit")));
	const FString Under = FString::Printf(TEXT("{\"fn\":\"hud_set\",\"args\":{\"pad\":\"%s\"}}"), *FString::ChrN(MaxArgsBytes - 20, TEXT('x')));
	TestEqual(TEXT("and just under them pass"), Check(Under), FString());

	FBudget Present;
	int32 Admitted = 0;
	for (int32 Call = 0; Call < 120; ++Call)
	{
		Admitted += CheckAt(HudRequest, Present, 0.0).IsEmpty() ? 1 : 0;
	}
	TestEqual(TEXT("a group admits its cap in a second"), Admitted, 120);
	TestEqual(TEXT("and refuses the next"), CheckAt(HudRequest, Present, 0.5), FString(TEXT("rate cap exceeded for 'hud_set'")));
	TestEqual(TEXT("for every call of the group"), CheckAt(TEXT("{\"fn\":\"overlay_draw\",\"args\":{}}"), Present, 0.5),
		FString(TEXT("rate cap exceeded for 'overlay_draw'")));
	TestEqual(TEXT("until a second has passed"), CheckAt(HudRequest, Present, 1.0), FString());

	FBudget All;
	for (int32 Call = 0; Call < MaxCallsPerSecond; ++Call)
	{
		CheckAt(TEXT("{\"fn\":\"clock\"}"), All, 0.0);
	}
	TestFalse(TEXT("refused calls count toward the breaker"), All.bTripped);
	TestEqual(TEXT("one more in the second opens it"), CheckAt(HudRequest, All, 0.0), FString(TEXT("global host-call rate exceeded")));
	TestTrue(TEXT("tripped"), All.bTripped);
	TestEqual(TEXT("and it stays open"), CheckAt(HudRequest, All, 10.0), FString(TEXT("global host-call rate exceeded")));

	FString Kind;
	bool bOk = false;
	const TArray<uint8> Shown = Delivered(true);
	const FString Answer = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Shown.GetData()), Shown.Num());
	TestEqual(TEXT("a presentation answers delivered"), Answer, FString(TEXT("{\"ok\":true,\"data\":{\"delivered\":true}}")));
	const TArray<uint8> Unshown = Delivered(false);
	TestEqual(TEXT("or not, with no host"), FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Unshown.GetData()), Unshown.Num()),
		FString(TEXT("{\"ok\":true,\"data\":{\"delivered\":false}}")));
	// 30,000 copies of 1e9 are about 120 KiB as sent and 330 KiB serialized, where 1e9 prints as 1000000000.
	FString Spelled;
	for (int32 Copy = 0; Copy < 30000; ++Copy)
	{
		Spelled += Copy == 0 ? TEXT("1e9") : TEXT(",1e9");
	}
	TestEqual(TEXT("arguments are measured as serialized"), Check(FString::Printf(TEXT("{\"fn\":\"hud_set\",\"args\":{\"n\":[%s]}}"), *Spelled)),
		FString(TEXT("host call args exceed the browser sandbox limit")));
	TestEqual(TEXT("a refusal escapes its message"), AnswerText(Denied(TEXT("a \"quoted\" \\ line")), Kind, bOk), FString(TEXT("a \"quoted\" \\ line")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHudTextTest, "CrowdySDK.ModScripts.HudText", Flags)
bool FCrowdyModScriptsHudTextTest::RunTest(const FString& Parameters)
{
	using CrowdyModScriptsHostCalls::HudText;
	TestEqual(TEXT("the payload's text"), HudText(*Args(TEXT("{\"payload\":{\"text\":\"hi\"}}"))), FString(TEXT("hi")));
	TestEqual(TEXT("a text that is not a string removes the card"), HudText(*Args(TEXT("{\"payload\":{\"text\":7}}"))), FString());
	TestEqual(TEXT("as does a payload that is not an object"), HudText(*Args(TEXT("{\"payload\":\"hi\"}"))), FString());
	TestEqual(TEXT("or none"), HudText(*Args(TEXT("{}"))), FString());
	TestEqual(TEXT("at most 8 lines"), HudText(*Args(TEXT("{\"payload\":{\"text\":\"1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\"}}"))),
		FString(TEXT("1\n2\n3\n4\n5\n6\n7\n8")));
	TestEqual(TEXT("and 500 characters"), HudText(*Args(FString::Printf(TEXT("{\"payload\":{\"text\":\"%s\"}}"), *FString::ChrN(600, TEXT('x'))))).Len(), 500);
	TestEqual(TEXT("a carriage return breaks a line too"), HudText(*Args(TEXT("{\"payload\":{\"text\":\"1\\r2\\r3\\r4\\r5\\r6\\r7\\r8\\r9\"}}"))),
		FString(TEXT("1\r2\r3\r4\r5\r6\r7\r8")));
	TestEqual(TEXT("as does a line separator"), HudText(*Args(TEXT("{\"payload\":{\"text\":\"1\\u20282\\u20283\\u20284\\u20285\\u20286\\u20287\\u20288\\u20289\"}}"))).Len(), 15);
	TestEqual(TEXT("and CR LF is one break"), HudText(*Args(TEXT("{\"payload\":{\"text\":\"1\\r\\n2\\r\\n3\\r\\n4\\r\\n5\\r\\n6\\r\\n7\\r\\n8\\r\\n9\"}}"))),
		FString(TEXT("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsOverlayRowsTest, "CrowdySDK.ModScripts.OverlayRows", Flags)
bool FCrowdyModScriptsOverlayRowsTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyModScriptShape> Read = Shapes(TEXT("{\"objects\":[{\"id\":\"a\",\"kind\":\"sphere\",\"x\":1,\"y\":2,\"z\":3,\"sx\":2,")
		TEXT("\"color\":\"#ff0000\",\"opacity\":0.5,\"unlit\":true}]}"));
	if (TestEqual(TEXT("an objects row is read"), Read.Num(), 1))
	{
		const FCrowdyModScriptShape& Shape = Read[0];
		TestTrue(TEXT("its id and kind"), Shape.Id == TEXT("a") && Shape.Kind == ECrowdyModScriptShapeKind::Sphere);
		TestEqual(TEXT("its place in voxels"), Shape.Location, FVector(1.0, 2.0, 3.0));
		TestEqual(TEXT("its size, 0.2 where unnamed"), Shape.Size, FVector(2.0, 0.2, 0.2));
		TestEqual(TEXT("its colour"), Shape.Color, FLinearColor(FColor(255, 0, 0)));
		TestTrue(TEXT("its opacity and shading"), Shape.Opacity == 0.5f && Shape.bUnlit && Shape.bVisible);
		TestTrue(TEXT("unrotated"), Shape.Rotation.Equals(FQuat::Identity));
	}
	TestEqual(TEXT("nodes rows are read"), Shapes(TEXT("{\"nodes\":[{\"x\":1,\"z\":1}]}")).Num(), 1);
	TestEqual(TEXT("shapes rows are read"), Shapes(TEXT("{\"shapes\":[{\"x\":1,\"z\":1}]}")).Num(), 1);
	TestEqual(TEXT("a bare array is read"), Shapes(TEXT("[{\"x\":1,\"z\":1},{\"x\":2,\"z\":2}]")).Num(), 2);
	Read = Shapes(TEXT("{\"x\":4,\"z\":5}"));
	TestTrue(TEXT("a single row is read, y 0.5 by default"), Read.Num() == 1 && Read[0].Location == FVector(4.0, 0.5, 5.0));
	TestEqual(TEXT("nothing clears"), Shapes(TEXT("null")).Num(), 0);
	TestEqual(TEXT("a row without x is dropped"), Shapes(TEXT("[{\"z\":1}]")).Num(), 0);
	Read = Shapes(TEXT("[{\"p\":{\"x\":3,\"y\":4,\"z\":5}},{\"x\":\"6\",\"z\":\" 7 \"}]"));
	TestTrue(TEXT("a nested position and numeric strings are read"),
		Read.Num() == 2 && Read[0].Location == FVector(3.0, 4.0, 5.0) && Read[1].Location == FVector(6.0, 0.5, 7.0));
	Read = Shapes(TEXT("[{\"kind\":\"mesh\",\"x\":1,\"z\":1},{\"shape\":\"plane\",\"x\":1,\"z\":1}]"));
	TestTrue(TEXT("a mesh row is not drawn but takes its place, and shape names the kind"),
		Read.Num() == 1 && Read[0].Id == TEXT("obj-1") && Read[0].Kind == ECrowdyModScriptShapeKind::Plane);
	TestEqual(TEXT("and its id"), Shapes(TEXT("[{\"kind\":\"mesh\",\"id\":\"a\",\"x\":1,\"z\":1},{\"id\":\"a\",\"x\":1,\"z\":1}]")).Num(), 0);
	Read = Shapes(TEXT("[{\"x\":\"5abc\",\"z\":1},{\"x\":1,\"y\":\"3px\",\"z\":1}]"));
	TestTrue(TEXT("a string that is not wholly a number is not one"), Read.Num() == 1 && Read[0].Location.Y == 0.5);
	FString Dropped;
	for (int32 Row = 0; Row < 300; ++Row)
	{
		Dropped += TEXT("{\"z\":1},");
	}
	TestEqual(TEXT("rows past the first 256 are not read"), Shapes(TEXT("[") + Dropped + TEXT("{\"x\":1,\"z\":1}]")).Num(), 0);
	Read = Shapes(TEXT("[{\"x\":1,\"z\":1,\"qx\":1e200,\"qw\":1e200}]"));
	TestTrue(TEXT("a huge rotation still normalizes"), Read.Num() == 1 && Read[0].Rotation.Equals(FQuat(UE_INV_SQRT_2, 0.0, 0.0, UE_INV_SQRT_2)));
	TestEqual(TEXT("a repeated id is skipped"), Shapes(TEXT("[{\"id\":\"a\",\"x\":1,\"z\":1},{\"id\":\"a\",\"x\":2,\"z\":2}]")).Num(), 1);
	FString Many;
	for (int32 Row = 0; Row < 70; ++Row)
	{
		Many += FString::Printf(TEXT("%s{\"x\":%d,\"z\":1}"), Row == 0 ? TEXT("") : TEXT(","), Row % 30);
	}
	TestEqual(TEXT("at most 64 shapes"), Shapes(TEXT("[") + Many + TEXT("]")).Num(), 64);
	Read = Shapes(TEXT("[{\"x\":-100,\"y\":100,\"z\":50}]"));
	TestTrue(TEXT("a place is clamped into the grid"), Read.Num() == 1 && Read[0].Location == FVector(0.0, 32.0, 16.0));
	Read = Shapes(TEXT("[{\"x\":1,\"z\":1,\"r\":100},{\"x\":1,\"z\":1,\"sx\":0}]"));
	TestTrue(TEXT("sizes are clamped to 0.04 and 8 voxels"),
		Read.Num() == 2 && Read[0].Size == FVector(8.0) && Read[1].Size == FVector(0.04, 0.2, 0.2));
	Read = Shapes(TEXT("[{\"x\":1,\"z\":1,\"qz\":2},{\"x\":1,\"z\":1,\"q\":{\"w\":0}}]"));
	TestTrue(TEXT("a rotation is normalized, a zero one is identity"), Read.Num() == 2
		&& Read[0].Rotation.Equals(FQuat(0.0, 0.0, 2.0 / FMath::Sqrt(5.0), 1.0 / FMath::Sqrt(5.0))) && Read[1].Rotation.Equals(FQuat::Identity));
	Read = Shapes(TEXT("[{\"x\":1,\"z\":1,\"color\":255},{\"x\":1,\"z\":1,\"colour\":\"#12\"}]"));
	TestTrue(TEXT("a numeric colour, and the default for an unreadable one"),
		Read.Num() == 2 && Read[0].Color == FLinearColor(FColor(0, 0, 255)) && Read[1].Color == FLinearColor(FColor(0xf8, 0xfa, 0xfc)));
	Read = Shapes(TEXT("[{\"x\":1,\"z\":1,\"visible\":false},{\"x\":1,\"z\":1,\"hidden\":true},{\"x\":1,\"z\":1,\"shading\":\"basic\"}]"));
	TestTrue(TEXT("visible false and hidden true hide; shading basic is unlit"),
		Read.Num() == 3 && !Read[0].bVisible && !Read[1].bVisible && Read[2].bVisible && Read[2].bUnlit);
	Read = Shapes(FString::Printf(TEXT("[{\"id\":\"  \",\"x\":1,\"z\":1},{\"id\":\"%s\",\"x\":1,\"z\":1}]"), *FString::ChrN(40, TEXT('k'))));
	TestTrue(TEXT("a blank id falls back, a long one is cut to 32"), Read.Num() == 2 && Read[0].Id == TEXT("obj-0") && Read[1].Id.Len() == 32);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsPrepareOnWorkerTest, "CrowdySDK.ModScripts.PrepareOnTheWorker", Flags)
bool FCrowdyModScriptsPrepareOnWorkerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	FGate Gate;
	Gate.Agreed = {TEXT("hud_set"), TEXT("overlay_draw")};
	Gate.Box.HighChunkX = 1;
	TArray<uint8> Request = Utf8(TEXT("{\"fn\":\"overlay_draw\",\"args\":{\"payload\":[{\"id\":\"a\",\"x\":100,\"z\":1}]}}"));
	TestTrue(TEXT("a passed call goes on to the game thread"), Prepare(Gate, Request, 0.0));
	TestEqual(TEXT("as its name alone"), FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Request.GetData()), Request.Num()),
		FString(TEXT("overlay_draw")));
	TestTrue(TEXT("with its shapes read and placed in the grid"), Gate.Prepared.Call == ECall::Overlay && Gate.Prepared.Shapes.Num() == 1
		&& Gate.Prepared.Shapes[0].Location.X == 32.0);
	Request = Utf8(HudRequest);
	TestTrue(TEXT("hud_set's text is read there too"), Prepare(Gate, Request, 0.0) && Gate.Prepared.Call == ECall::Hud && Gate.Prepared.HudText == TEXT("hi")
		&& Gate.Prepared.Shapes.IsEmpty());

	Request = Utf8(TEXT("{\"fn\":\"hud_set\",\"args\":{\"authority\":1}}"));
	TestFalse(TEXT("a refused call is answered on the worker"), Prepare(Gate, Request, 0.0));
	FString Kind;
	bool bOk = true;
	TestEqual(TEXT("with the refusal"), AnswerText(Request, Kind, bOk), FString(TEXT("host call cannot override 'authority'")));
	for (int32 Call = 0; Call <= MaxCallsPerSecond; ++Call)
	{
		Request = Utf8(TEXT("{\"fn\":\"clock\",\"args\":{}}"));
		Prepare(Gate, Request, 0.0);
	}
	TestTrue(TEXT("a flood marks the gate for the game thread"), Gate.bTripped.load());
	return true;
}

namespace CrowdyModScriptsTests
{
	const TArray<FString> WorldCalls = {TEXT("chunk_get"), TEXT("voxels_list"), TEXT("actors_list"), TEXT("actors_list_radius"), TEXT("pointer_clicks"),
		TEXT("voxel_set")};

	/** A gate on the plot, 10 by 10 by 2 chunks from the origin, agreed to every world call. */
	void PlotGate(CrowdyModScriptsHostCalls::FGate& Gate)
	{
		Gate.Agreed = WorldCalls;
		Gate.Box.GridId = 100;
		Gate.Box.HighChunkX = 9;
		Gate.Box.HighChunkY = 9;
		Gate.Box.HighChunkZ = 1;
	}

	// The refusal's message, or empty when the call went on to the game thread.
	FString PrepareWorld(CrowdyModScriptsHostCalls::FGate& Gate, const FString& Name, const FString& Args, FString* OutAnswer = nullptr)
	{
		TArray<uint8> Request = Utf8(FString::Printf(TEXT("{\"fn\":\"%s\",\"args\":%s}"), *Name, *Args));
		if (CrowdyModScriptsHostCalls::Prepare(Gate, Request, 0.0))
		{
			return FString();
		}
		FString Kind;
		bool bOk = false;
		const FString Message = AnswerText(Request, Kind, bOk);
		if (OutAnswer)
		{
			*OutAnswer = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Request.GetData()), Request.Num());
		}
		return bOk ? FString(TEXT("<answered>")) : Message;
	}

	TSharedPtr<FJsonObject> AnswerData(const TArray<uint8>& Answer)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Data = nullptr;
		const FString Json = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Answer.GetData()), Answer.Num());
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid() && Root->TryGetObjectField(TEXT("data"), Data)
			? *Data : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsWorldCallsPreparedTest, "CrowdySDK.ModScripts.WorldCallsPrepared", Flags)
bool FCrowdyModScriptsWorldCallsPreparedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	FGate Gate;
	PlotGate(Gate);
	TestEqual(TEXT("a read inside the grid goes on"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":5,\"y\":5,\"z\":0}")), FString());
	TestTrue(TEXT("naming its chunk"), Gate.Prepared.Call == ECall::ChunkGet && Gate.Prepared.Chunk == FInt64Vector(5, 5, 0));
	TestEqual(TEXT("decimal strings are coordinates too"), PrepareWorld(Gate, TEXT("voxels_list"), TEXT("{\"x\":\"5\",\"y\":\"-0\",\"z\":\"0\"}")), FString());
	TestTrue(TEXT("read as numbers"), Gate.Prepared.Call == ECall::VoxelsList && Gate.Prepared.Chunk == FInt64Vector(5, 0, 0));
	TestEqual(TEXT("a padded string is not"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":\"05\",\"y\":5,\"z\":0}")),
		FString(TEXT("chunk coordinates must be decimal integer strings")));
	TestEqual(TEXT("nor a fraction"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":1.5,\"y\":5,\"z\":0}")),
		FString(TEXT("chunk coordinates must be safe integers or decimal strings")));
	TestEqual(TEXT("nor a missing one"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":5,\"y\":5}")), FString(TEXT("chunk coordinates must be integers")));
	TestEqual(TEXT("a chunk outside the grid is refused"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":10,\"y\":5,\"z\":0}")),
		FString(TEXT("target is outside the player grid")));
	TestEqual(TEXT("as is a far one"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":\"99999999999999999999\",\"y\":5,\"z\":0}")),
		FString(TEXT("target is outside the player grid")));
	TestEqual(TEXT("another spelling of the chunk is refused"), PrepareWorld(Gate, TEXT("actors_list"), TEXT("{\"x\":5,\"y\":5,\"z\":0,\"chunkX\":50}")),
		FString(TEXT("host call cannot carry 'chunkX': the grid is checked on the call's own chunk fields")));

	TestEqual(TEXT("actors_list asks about its chunk"), PrepareWorld(Gate, TEXT("actors_list"), TEXT("{\"x\":5,\"y\":5,\"z\":0,\"radiusXz\":3}")), FString());
	const FCrowdyModScriptGrid& Area = Gate.Prepared.Area;
	TestTrue(TEXT("alone"), Area.GridId == 100 && Area.LowChunkX == 5 && Area.HighChunkX == 5 && Area.LowChunkZ == 0 && Area.HighChunkZ == 0);
	TestEqual(TEXT("actors_list_radius goes on"), PrepareWorld(Gate, TEXT("actors_list_radius"), TEXT("{\"x\":5,\"y\":5,\"z\":0,\"radiusXz\":9,\"radiusY\":5}")), FString());
	TestTrue(TEXT("3 out sideways and 1 up or down at most, clipped to the grid"), Gate.Prepared.Area.LowChunkX == 2 && Gate.Prepared.Area.HighChunkX == 8
		&& Gate.Prepared.Area.LowChunkY == 4 && Gate.Prepared.Area.HighChunkY == 6 && Gate.Prepared.Area.LowChunkZ == 0 && Gate.Prepared.Area.HighChunkZ == 1);
	PrepareWorld(Gate, TEXT("actors_list_radius"), TEXT("{\"x\":0,\"y\":0,\"z\":0,\"radius\":\"2\"}"));
	TestTrue(TEXT("radius stands for radiusXz"), Gate.Prepared.Area.LowChunkX == 0 && Gate.Prepared.Area.HighChunkX == 2 && Gate.Prepared.Area.HighChunkY == 0);
	PrepareWorld(Gate, TEXT("actors_list_radius"), TEXT("{\"x\":5,\"y\":5,\"z\":0,\"radiusXz\":1e300}"));
	TestTrue(TEXT("a huge radius is the largest, not none"), Gate.Prepared.Area.LowChunkX == 2 && Gate.Prepared.Area.HighChunkX == 8);
	TestEqual(TEXT("digits from other scripts are not digits"), PrepareWorld(Gate, TEXT("chunk_get"), TEXT("{\"x\":\"\\uff15\",\"y\":5,\"z\":0}")),
		FString(TEXT("chunk coordinates must be decimal integer strings")));
	FGate Edge;
	PlotGate(Edge);
	Edge.Box.LowChunkX = MIN_int64;
	Edge.Box.HighChunkX = MIN_int64 + 1;
	TestEqual(TEXT("a grid at the edge of int64"), PrepareWorld(Edge, TEXT("actors_list_radius"),
		TEXT("{\"x\":\"-9223372036854775808\",\"y\":5,\"z\":0,\"radiusXz\":3}")), FString());
	TestTrue(TEXT("clips its box without overflow"), Edge.Prepared.Area.LowChunkX == MIN_int64 && Edge.Prepared.Area.HighChunkX == MIN_int64 + 1);

	TestEqual(TEXT("a voxel_set inside the grid goes on"),
		PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"voxelX\":1,\"voxelY\":\"2\",\"voxelZ\":3,\"voxelType\":7,\"stateBase64\":\"AAE=\"}")),
		FString());
	const FCrowdyVoxelWrite& Write = Gate.Prepared.Write;
	TestTrue(TEXT("as the write it names"), Write.ChunkX == 5 && Write.ChunkY == 5 && Write.ChunkZ == 0 && Write.Voxel.Position == FIntVector(1, 2, 3)
		&& Write.Voxel.Type == 7 && Write.Voxel.State == TArray<uint8>({0, 1}));
	TestEqual(TEXT("an absent voxel is 0"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0}")), FString());
	FString Answer;
	TestEqual(TEXT("a voxel_set outside the grid is answered"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":50,\"chunkY\":5,\"chunkZ\":0}"), &Answer),
		FString(TEXT("<answered>")));
	TestEqual(TEXT("out_of_grid"), Answer, FString(TEXT("{\"ok\":true,\"data\":{\"ok\":false,\"error\":\"out_of_grid\"}}")));
	const FString NeedsVoxel = TEXT("host call 'voxel_set' needs a voxel inside its chunk (0-15) and a voxel type 0-255");
	TestEqual(TEXT("a voxel outside its chunk is refused"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"voxelX\":16}")), NeedsVoxel);
	TestEqual(TEXT("as is a type over 255"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"voxelType\":256}")), NeedsVoxel);
	TestEqual(TEXT("or a fraction"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"voxelY\":\"1.5\"}")), NeedsVoxel);
	TestEqual(TEXT("a state that is not base64 is refused"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"stateBase64\":\"!!\"}")),
		FString(TEXT("host call 'voxel_set' needs a base64 state of at most 1024 bytes")));
	TArray<uint8> Long;
	Long.Init(1, 1025);
	TestEqual(TEXT("as is one over 1024 bytes"), PrepareWorld(Gate, TEXT("voxel_set"),
		FString::Printf(TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"stateBase64\":\"%s\"}"), *FBase64::Encode(Long))),
		FString(TEXT("host call 'voxel_set' needs a base64 state of at most 1024 bytes")));
	TestEqual(TEXT("voxel_set's own chunk spellings are checked"), PrepareWorld(Gate, TEXT("voxel_set"), TEXT("{\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"chunk_x\":50}")),
		FString(TEXT("host call cannot carry 'chunk_x': the grid is checked on the call's own chunk fields")));
	TestEqual(TEXT("pointer_clicks goes on"), PrepareWorld(Gate, TEXT("pointer_clicks"), TEXT("{}")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsSdkCallsTest, "CrowdySDK.ModScripts.SdkCallsAnswered", Flags)
bool FCrowdyModScriptsSdkCallsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	FGate Gate;
	PlotGate(Gate);
	Gate.Agreed.Append({TEXT("grid_info"), TEXT("clock"), TEXT("grid_permission_check"), TEXT("emit_event")});
	Gate.ModuleName = TEXT("mine");
	Gate.PlayerId = 42;
	Gate.Keys = {TEXT("run_client_code")};

	FString Answer;
	TestEqual(TEXT("grid_info is answered on the worker"), PrepareWorld(Gate, TEXT("grid_info"), TEXT("{}"), &Answer), FString(TEXT("<answered>")));
	TestEqual(TEXT("with the grid's id and box as decimal strings"), Answer,
		FString(TEXT("{\"ok\":true,\"data\":{\"gridId\":\"100\",\"low\":{\"x\":\"0\",\"y\":\"0\",\"z\":\"0\"},\"high\":{\"x\":\"9\",\"y\":\"9\",\"z\":\"1\"}}}")));
	TestEqual(TEXT("clock is answered on the worker"), PrepareWorld(Gate, TEXT("clock"), TEXT("{}"), &Answer), FString(TEXT("<answered>")));
	const TSharedPtr<FJsonObject> Clock = AnswerData(Utf8(Answer));
	const double NowMs = static_cast<double>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond);
	TestTrue(TEXT("with the wall clock in ms"), Clock.IsValid() && FMath::Abs(Clock->GetNumberField(TEXT("ms")) - NowMs) < 5000.0);

	const auto Check = [](const TCHAR* Key) { return FString::Printf(TEXT("{\"gridId\":\"100\",\"userId\":\"42\",\"permissionKey\":\"%s\"}"), Key); };
	TestEqual(TEXT("a held code key"), PrepareWorld(Gate, TEXT("grid_permission_check"), Check(TEXT("run_client_code")), &Answer), FString(TEXT("<answered>")));
	TestEqual(TEXT("is held"), Answer, FString(TEXT("{\"ok\":true,\"data\":true}")));
	PrepareWorld(Gate, TEXT("grid_permission_check"), Check(TEXT("write_client_code")), &Answer);
	TestEqual(TEXT("one not held is not"), Answer, FString(TEXT("{\"ok\":true,\"data\":false}")));
	TestEqual(TEXT("numbers name the grid and player too"), PrepareWorld(Gate, TEXT("grid_permission_check"),
		TEXT("{\"gridId\":100,\"userId\":42,\"permissionKey\":\"run_client_code\"}")), FString(TEXT("<answered>")));
	TestEqual(TEXT("another grid is refused"), PrepareWorld(Gate, TEXT("grid_permission_check"), TEXT("{\"gridId\":\"101\",\"userId\":\"42\",\"permissionKey\":\"run_client_code\"}")),
		FString(TEXT("target is outside the player grid")));
	TestEqual(TEXT("as is another player"), PrepareWorld(Gate, TEXT("grid_permission_check"), TEXT("{\"gridId\":\"100\",\"userId\":\"43\",\"permissionKey\":\"run_client_code\"}")),
		FString(TEXT("host call 'grid_permission_check' asks about another player")));
	TestEqual(TEXT("a key that is not a key"), PrepareWorld(Gate, TEXT("grid_permission_check"), Check(TEXT("Run"))),
		FString(TEXT("host call 'grid_permission_check' needs a permission key")));
	TestTrue(TEXT("and any key but the four code keys"), PrepareWorld(Gate, TEXT("grid_permission_check"), Check(TEXT("update_voxel_data")))
		.StartsWith(TEXT("host call 'grid_permission_check' cannot answer for 'update_voxel_data'")));

	TestEqual(TEXT("emit_event goes on to the game thread"), PrepareWorld(Gate, TEXT("emit_event"), TEXT("{\"name\":\"ping\",\"payload\":{\"n\":1}}")), FString());
	const TArray<uint8> Prefix = Gate.Prepared.EventPrefix;
	TestEqual(TEXT("as the browser's event, all but its depth"), FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Prefix.GetData()), Prefix.Num()),
		FString(TEXT("{\"kind\":\"grid_event\",\"gridId\":\"100\",\"eventName\":\"ping\",\"payload\":{\"n\":1},\"sourceModule\":\"mine\",\"target\":null")));
	TestTrue(TEXT("for everyone"), Gate.Prepared.Call == ECall::Event && !Gate.Prepared.bEventTargeted);
	PrepareWorld(Gate, TEXT("emit_event"), TEXT("{\"name\":\"ping\",\"target\":\"other\"}"));
	TestTrue(TEXT("or the module it names"), Gate.Prepared.bEventTargeted && Gate.Prepared.EventTarget == TEXT("other"));
	TestEqual(TEXT("a nameless event is refused"), PrepareWorld(Gate, TEXT("emit_event"), TEXT("{\"name\":\"\"}")), FString(TEXT("emit_event name must be a non-empty string")));
	TestEqual(TEXT("as is a name over 128 bytes"), PrepareWorld(Gate, TEXT("emit_event"), FString::Printf(TEXT("{\"name\":\"%s\"}"), *FString::ChrN(129, TEXT('n')))),
		FString(TEXT("emit_event name must be a non-empty string")));
	TestEqual(TEXT("and a target that is not a name"), PrepareWorld(Gate, TEXT("emit_event"), TEXT("{\"name\":\"ping\",\"target\":7}")),
		FString(TEXT("emit_event target must be a module name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsPlatformCallsPreparedTest, "CrowdySDK.ModScripts.PlatformCallsPrepared", Flags)
bool FCrowdyModScriptsPlatformCallsPreparedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	FGate Gate;
	PlotGate(Gate);
	Gate.Agreed.Append({TEXT("user_state_get"), TEXT("user_state_set"), TEXT("emit_spatial"), TEXT("emit_channel")});
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	const auto Utf8Text = [](const TArray<uint8>& Bytes) { return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()); };
#endif

	TestEqual(TEXT("user_state_get goes on to the platform"), PrepareWorld(Gate, TEXT("user_state_get"), TEXT("{}")), FString());
	TestTrue(TEXT("as itself"), Gate.Prepared.Call == ECall::UserStateGet);
	PrepareWorld(Gate, TEXT("user_state_set"), TEXT("{\"stateBase64\":\"AAE=\"}"));
	TestTrue(TEXT("user_state_set carries its state"), Gate.Prepared.Call == ECall::UserStateSet && !Gate.Prepared.bClearState && Gate.Prepared.State == TEXT("AAE="));
	PrepareWorld(Gate, TEXT("user_state_set"), TEXT("{\"stateBase64\":7}"));
	TestTrue(TEXT("and a state that is not a string clears it"), Gate.Prepared.bClearState && Gate.Prepared.State.IsEmpty());

	const FString Spatial = TEXT("{\"kind\":\"client_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwABAg==\",\"distance\":12}");
	TestEqual(TEXT("a client event inside the grid goes on"), PrepareWorld(Gate, TEXT("emit_spatial"), Spatial), FString());
	const UCrowdyModScripts::FRealtimeSend& Send = Gate.Prepared.Send;
	TestTrue(TEXT("from its chunk, at most 8 out, with its payload"), Gate.Prepared.Call == ECall::Spatial && !Send.bChannel && Send.Chunk == FInt64Vector(5, 5, 0)
		&& Send.Distance == MaxSpatialDistance && Send.Payload == TArray<uint8>({7, 0, 1, 2}));
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	TestEqual(TEXT("under the grid's id for the game's actor, which is none"), Utf8Text(Send.Sender), FString(TEXT("30768aa12be36d5e000e86aa384247f8")));
#endif
	PrepareWorld(Gate, TEXT("emit_spatial"), TEXT("{\"kind\":\"server_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwA=\",\"distance\":-3,")
		TEXT("\"uuidHex\":\"E941414141414141414141414141414141414141414141414141414141414141\"}"));
	TestTrue(TEXT("a server event goes too, no distance below 0"), Gate.Prepared.Call == ECall::Spatial && Gate.Prepared.Send.Distance == 0);
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	TestEqual(TEXT("under the id for the actor it names, its octets read as characters"), Utf8Text(Gate.Prepared.Send.Sender),
		FString(TEXT("c7895d6315afab10f3a096a316b8f902")));
	PrepareWorld(Gate, TEXT("emit_spatial"), FString::Printf(TEXT("{\"kind\":\"client_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwA=\",")
		TEXT("\"uuidHex\":\"%s\"}"), *FString::ChrN(64, static_cast<TCHAR>(0xFF10))));
	TestEqual(TEXT("fullwidth digits are not hex, so they name no actor"), Utf8Text(Gate.Prepared.Send.Sender), FString(TEXT("30768aa12be36d5e000e86aa384247f8")));
#endif
	TestEqual(TEXT("a chunk outside the grid is refused"), PrepareWorld(Gate, TEXT("emit_spatial"),
		TEXT("{\"kind\":\"client_event\",\"chunkX\":50,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwA=\"}")), FString(TEXT("target is outside the player grid")));
	TestEqual(TEXT("as is another spelling of it"), PrepareWorld(Gate, TEXT("emit_spatial"), TEXT("{\"kind\":\"client_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"chunk\":1}")),
		FString(TEXT("host call cannot carry 'chunk': the grid is checked on the call's own chunk fields")));
	const FString Inside = TEXT("\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"BwA=\"");
	TestEqual(TEXT("an unknown kind is refused"), PrepareWorld(Gate, TEXT("emit_spatial"), FString::Printf(TEXT("{\"kind\":\"Client_event\",%s}"), *Inside)),
		FString(TEXT("host call 'emit_spatial' has an unknown kind")));
	TestEqual(TEXT("an actor update is refused"), PrepareWorld(Gate, TEXT("emit_spatial"), FString::Printf(TEXT("{\"kind\":\"actor\",%s}"), *Inside)),
		FString(TEXT("host call 'emit_spatial' cannot send kind 'actor' in this game")));
	TestEqual(TEXT("as is text"), PrepareWorld(Gate, TEXT("emit_spatial"), FString::Printf(TEXT("{\"kind\":\"text\",%s}"), *Inside)),
		FString(TEXT("host call 'emit_spatial' cannot send kind 'text' in this game")));
	TestEqual(TEXT("an event needs its type"), PrepareWorld(Gate, TEXT("emit_spatial"),
		TEXT("{\"kind\":\"client_event\",\"chunkX\":5,\"chunkY\":5,\"chunkZ\":0,\"payloadBase64\":\"Bw==\"}")),
		FString(TEXT("host call 'emit_spatial' needs a uint16 eventType prefix")));

	TestEqual(TEXT("a channel message goes on"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":\"9223372036854775807\",\"payloadBase64\":\"aGk=\"}")),
		FString());
	TestTrue(TEXT("to its channel, with its payload"), Gate.Prepared.Call == ECall::Channel && Gate.Prepared.Send.bChannel
		&& Gate.Prepared.Send.ChannelId == MAX_int64 && Gate.Prepared.Send.Payload == TArray<uint8>({'h', 'i'}));
	const FString Outside = TEXT("host call 'emit_channel' targets a channel outside this grid");
	TestEqual(TEXT("an id past int64 names no channel"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":\"9223372036854775808\"}")), Outside);
	TestEqual(TEXT("nor does a padded one"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":\"012\"}")), Outside);
	TArray<uint8> Long;
	Long.Init(1, MaxChannelPayloadBytes + 1);
	TestEqual(TEXT("a payload over 1024 bytes is refused"), PrepareWorld(Gate, TEXT("emit_channel"),
		FString::Printf(TEXT("{\"channelId\":12,\"payloadBase64\":\"%s\"}"), *FBase64::Encode(Long))), FString(TEXT("Channel payload exceeds 1024 bytes: 1025")));
	TestEqual(TEXT("as is one that is not base64"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":12,\"payloadBase64\":\"!!\"}")),
		FString(TEXT("host call 'emit_channel' needs a base64 payload")));
	TestEqual(TEXT("or not text at all"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":12,\"payloadBase64\":{}}")),
		FString(TEXT("host call 'emit_channel' needs a base64 payload")));
	TestEqual(TEXT("while no payload is an empty message"), PrepareWorld(Gate, TEXT("emit_channel"), TEXT("{\"channelId\":12}")), FString());

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	const auto Written = [](ECall Call, const FString& Data)
	{
		FWorldAnswer Answer;
		Answer.Call = Call;
		if (!Data.IsEmpty())
		{
			CrowdyNativeExec::JsonToMsgpack(Data, Answer.Packed);
		}
		const TArray<uint8> Bytes = WorldAnswer(Answer);
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	};
	TestEqual(TEXT("a state row is answered as the platform gave it"), Written(ECall::UserStateGet, TEXT("{\"userAppState\":{\"userId\":\"42\",\"state\":\"AAE=\"}}")),
		FString(TEXT("{\"ok\":true,\"data\":{\"userId\":\"42\",\"state\":\"AAE=\"}}")));
	TestEqual(TEXT("from the set's own field"), Written(ECall::UserStateSet, TEXT("{\"updateUserAppState\":{\"state\":null}}")),
		FString(TEXT("{\"ok\":true,\"data\":{\"state\":null}}")));
	TestEqual(TEXT("no row is null"), Written(ECall::UserStateGet, TEXT("{\"userAppState\":null}")), FString(TEXT("{\"ok\":true,\"data\":null}")));
	TestEqual(TEXT("an answer missing its field is refused"), Written(ECall::UserStateGet, TEXT("{\"updateUserAppState\":null}")),
		FString(TEXT("{\"ok\":false,\"error\":{\"kind\":\"denied\",\"message\":\"host call answer could not be read\"}}")));
	TestEqual(TEXT("as is nothing at all"), Written(ECall::UserStateGet, FString()),
		FString(TEXT("{\"ok\":false,\"error\":{\"kind\":\"denied\",\"message\":\"host call answer could not be read\"}}")));
	TestEqual(TEXT("a state past the browser's reply limit is too large"),
		Written(ECall::UserStateGet, FString::Printf(TEXT("{\"userAppState\":{\"state\":\"%s\"}}"), *FString::ChrN(MaxReplyBytes, TEXT('A')))),
		FString(TEXT("{\"ok\":false,\"error\":{\"kind\":\"response_too_large\",\"message\":\"host response exceeds reply limit\"}}")));
	TestTrue(TEXT("while one past the argument limit is not"), Written(ECall::UserStateGet,
		FString::Printf(TEXT("{\"userAppState\":{\"state\":\"%s\"}}"), *FString::ChrN(MaxArgsBytes + 1, TEXT('A')))).StartsWith(TEXT("{\"ok\":true,")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsWorldAnswersTest, "CrowdySDK.ModScripts.WorldAnswers", Flags)
bool FCrowdyModScriptsWorldAnswersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptsHostCalls;
	const TSharedRef<FCrowdyModScriptChunk, ESPMode::ThreadSafe> Chunk = MakeShared<FCrowdyModScriptChunk, ESPMode::ThreadSafe>();
	Chunk->Types.Init(0, 4096);
	Chunk->Types[0] = 3;
	FCrowdyModScriptCellState& Cell = Chunk->States.AddDefaulted_GetRef();
	Cell.X = 1;
	Cell.Y = 1;
	Cell.State = TEXT("AQ==");

	FWorldAnswer Read;
	Read.Call = ECall::ChunkGet;
	Read.Chunk = Chunk;
	TSharedPtr<FJsonObject> Data = AnswerData(WorldAnswer(Read));
	FString Text;
	TestTrue(TEXT("chunk_get gives the types as base64"), Data.IsValid() && Data->TryGetStringField(TEXT("voxelsBase64"), Text) && Text == FBase64::Encode(Chunk->Types));
	Read.Chunk = nullptr;
	Data = AnswerData(WorldAnswer(Read));
	TestTrue(TEXT("and null for a chunk not loaded"), Data.IsValid() && Data->HasTypedField<EJson::Null>(TEXT("voxelsBase64")));

	Read.Call = ECall::VoxelsList;
	Read.Chunk = Chunk;
	Data = AnswerData(WorldAnswer(Read));
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (TestTrue(TEXT("voxels_list lists the filled cell and the one with a state"), Data.IsValid() && Data->TryGetArrayField(TEXT("voxels"), Rows) && Rows->Num() == 2))
	{
		const TSharedPtr<FJsonObject> Filled = (*Rows)[0]->AsObject();
		const TSharedPtr<FJsonObject> Stated = (*Rows)[1]->AsObject();
		TestTrue(TEXT("the filled one first, with no state"), Filled->GetNumberField(TEXT("voxelType")) == 3 && Filled->HasTypedField<EJson::Null>(TEXT("state")));
		TestTrue(TEXT("then the one with a state, at its cell"), Stated->GetNumberField(TEXT("x")) == 1 && Stated->GetNumberField(TEXT("y")) == 1
			&& Stated->GetNumberField(TEXT("voxelType")) == 0 && Stated->GetStringField(TEXT("state")) == TEXT("AQ=="));
	}
	Read.Chunk = nullptr;
	Data = AnswerData(WorldAnswer(Read));
	TestTrue(TEXT("and nothing for a chunk not loaded"), Data.IsValid() && Data->TryGetArrayField(TEXT("voxels"), Rows) && Rows->IsEmpty());

	FWorldAnswer Actors;
	Actors.Call = ECall::Actors;
	FCrowdyModScriptPlayer& Self = Actors.Players.AddDefaulted_GetRef();
	Self.ActorId = TEXT("aa");
	Self.bSelf = true;
	Self.Location = FVector(1.0, 2.0, 3.0);
	FCrowdyModScriptPlayer& Odd = Actors.Players.AddDefaulted_GetRef();
	Odd.ActorId = TEXT("bb");
	Odd.Yaw = std::numeric_limits<float>::quiet_NaN();
	Data = AnswerData(WorldAnswer(Actors));
	if (TestTrue(TEXT("actors lists each player, a host's NaN written as 0"), Data.IsValid() && Data->TryGetArrayField(TEXT("actors"), Rows) && Rows->Num() == 2
		&& (*Rows)[1]->AsObject()->GetNumberField(TEXT("yaw")) == 0.0))
	{
		bool bSelf = false;
		TestTrue(TEXT("self only on the local player's row"), (*Rows)[0]->AsObject()->TryGetBoolField(TEXT("self"), bSelf) && bSelf
			&& !(*Rows)[1]->AsObject()->HasField(TEXT("self")));
		TestTrue(TEXT("in voxels"), (*Rows)[0]->AsObject()->GetNumberField(TEXT("z")) == 3.0 && (*Rows)[0]->AsObject()->GetStringField(TEXT("uuid")) == TEXT("aa"));
	}

	FWorldAnswer Clicks;
	Clicks.Call = ECall::Clicks;
	Clicks.NowMs = 5;
	Clicks.Clicks.Buttons = 1;
	Clicks.Clicks.MiddleHeldMs = 40;
	FCrowdyModScriptClick& Down = Clicks.Clicks.Clicks.AddDefaulted_GetRef();
	Down.bDown = true;
	FCrowdyModScriptClick& Up = Clicks.Clicks.Clicks.AddDefaulted_GetRef();
	Up.HeldMs = 30;
	Up.Button = 2;
	Data = AnswerData(WorldAnswer(Clicks));
	const TSharedPtr<FJsonObject>* Holding = nullptr;
	if (TestTrue(TEXT("pointer_clicks gives the time, held buttons and clicks"), Data.IsValid() && Data->GetNumberField(TEXT("nowMs")) == 5
		&& Data->TryGetObjectField(TEXT("holdingMs"), Holding) && (*Holding)->GetNumberField(TEXT("1")) == 40 && Data->TryGetArrayField(TEXT("clicks"), Rows)
		&& Rows->Num() == 2))
	{
		TestTrue(TEXT("a press has no heldMs"), (*Rows)[0]->AsObject()->GetStringField(TEXT("t")) == TEXT("down") && !(*Rows)[0]->AsObject()->HasField(TEXT("heldMs")));
		TestTrue(TEXT("a release has"), (*Rows)[1]->AsObject()->GetStringField(TEXT("t")) == TEXT("up") && (*Rows)[1]->AsObject()->GetNumberField(TEXT("heldMs")) == 30
			&& (*Rows)[1]->AsObject()->GetNumberField(TEXT("button")) == 2);
	}

	FWorldAnswer Set;
	Set.Call = ECall::VoxelSet;
	Set.VoxelError = TEXT("busy");
	const TArray<uint8> Busy = WorldAnswer(Set);
	TestEqual(TEXT("voxel_set answers its refusal"), FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Busy.GetData()), Busy.Num()),
		FString(TEXT("{\"ok\":true,\"data\":{\"ok\":false,\"error\":\"busy\"}}")));

	for (int32 Index = 0; Index < 300; ++Index)
	{
		FCrowdyModScriptCellState& Big = Chunk->States.AddDefaulted_GetRef();
		Big.X = Index % 16;
		Big.Y = (Index / 16) % 16;
		Big.Z = 2;
		Big.State = FString::ChrN(1368, TEXT('A'));
	}
	Read.Chunk = Chunk;
	FString Kind;
	bool bOk = true;
	TestEqual(TEXT("an answer over 256 KiB is refused"), AnswerText(WorldAnswer(Read), Kind, bOk), FString(TEXT("host call answer exceeds the browser sandbox limit")));

	FGate Gate;
	{
		FScopeLock Guard(&Gate.Lock);
		Gate.Answer = Set;
	}
	TArray<uint8> Marked = {WorldAnswerMarker};
	Finish(Gate, Marked);
	TestEqual(TEXT("the worker writes the answer the game thread left"), Marked, Busy);
	TArray<uint8> Plain = Utf8(TEXT("{\"ok\":true}"));
	Finish(Gate, Plain);
	TestEqual(TEXT("and passes any other answer through"), Plain, Utf8(TEXT("{\"ok\":true}")));
	return true;
}

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsGridFromClaimsTest, "CrowdySDK.ModScripts.GridFromClaims", Flags)
bool FCrowdyModScriptsGridFromClaimsTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("the runtime is available"), Harness.Scripts->IsAvailable()))
	{
		return false;
	}
	Harness.Scripts->EnterGrid(5, 5, 0);
	const FCall* Asked = Harness.Fake->Find(ECrowdyModScriptsCall::GridPermissions);
	TestTrue(TEXT("the claims are asked about the named chunk"), Asked && Asked->Chunk == FInt64Vector(5, 5, 0));
	Harness.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Covering(PlotId));

	const FCrowdyModScriptGrid Grid = Harness.Scripts->GetGrid();
	TestEqual(TEXT("the deciding grid, not the world grid"), Grid.GridId, PlotId);
	TestTrue(TEXT("with the platform's box"), Grid.LowChunkX == 0 && Grid.LowChunkY == 0 && Grid.LowChunkZ == 0
		&& Grid.HighChunkX == 9 && Grid.HighChunkY == 9 && Grid.HighChunkZ == 1);
	const FCall* Listed = Harness.Fake->Find(ECrowdyModScriptsCall::ListMods);
	TestTrue(TEXT("and its Mod Scripts are listed on entering"), Listed && Listed->GridId == PlotId);

	FHarness Outside;
	Outside.Scripts->EnterGrid(500, 500, 0);
	Outside.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Ok());
	TestEqual(TEXT("no covering grid is no grid"), Outside.Scripts->GetGrid().GridId, static_cast<int64>(0));
	Outside.Scripts->EnterGrid(501, 500, 0);
	Outside.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Refused(TEXT("FORBIDDEN")));
	TestEqual(TEXT("nor is a failure"), Outside.Scripts->GetGrid().GridId, static_cast<int64>(0));
	TestEqual(TEXT("and nothing is listed"), Outside.Fake->Count(ECrowdyModScriptsCall::ListMods), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsChunkInsideBoxTest, "CrowdySDK.ModScripts.ChunkInsideBoxChangesNothing", Flags)
bool FCrowdyModScriptsChunkInsideBoxTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered the plot"), Harness.Enter()))
	{
		return false;
	}
	Harness.Scripts->EnterGrid(9, 0, 1);
	Harness.Scripts->EnterGrid(5, 5, 0);
	TestEqual(TEXT("a chunk inside the box asks nothing"), Harness.Fake->Count(ECrowdyModScriptsCall::GridPermissions), 0);
	TestEqual(TEXT("and stays in the grid"), Harness.Scripts->GetGrid().GridId, PlotId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsStaleGridAnswerTest, "CrowdySDK.ModScripts.StaleGridAnswerIgnored", Flags)
bool FCrowdyModScriptsStaleGridAnswerTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered the plot"), Harness.Enter()))
	{
		return false;
	}
	Harness.Scripts->EnterGrid(50, 0, 0);
	Harness.Scripts->EnterGrid(60, 0, 0);
	Harness.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Covering(200));
	TestEqual(TEXT("the answer for an earlier chunk is dropped"), Harness.Scripts->GetGrid().GridId, PlotId);
	Harness.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Covering(300));
	TestEqual(TEXT("the latest chunk's grid is entered"), Harness.Scripts->GetGrid().GridId, static_cast<int64>(300));

	// The listing asked for the plot answers late, with rows that would read for the current grid.
	Harness.Fake->Answer(ECrowdyModScriptsCall::ListMods, Listing({Unagreed(TEXT("old"))}, 300));
	TestEqual(TEXT("a listing asked for the last grid asks nothing"), Harness.Recorder->Prompts.Num(), 0);
	Harness.Scripts->TickForTest(10.0);
	TestEqual(TEXT("nor ends the current grid's listing in flight"), Harness.Fake->Count(ECrowdyModScriptsCall::ListMods), 1);
	TestTrue(TEXT("which is still awaited"), Harness.List({Unagreed(TEXT("new"))}, 300));
	TestEqual(TEXT("which is read"), Harness.Recorder->Prompts.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsOnePromptPerAuthorTest, "CrowdySDK.ModScripts.OnePromptPerAuthor", Flags)
bool FCrowdyModScriptsOnePromptPerAuthorTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered and listed"), Harness.Enter() && Harness.List({Unagreed(TEXT("a")), Unagreed(TEXT("b")), Unagreed(TEXT("c"), AuthorB)})))
	{
		return false;
	}
	const TArray<FCrowdyModScriptPrompt>& Prompts = Harness.Recorder->Prompts;
	if (!TestEqual(TEXT("one prompt per author"), Prompts.Num(), 2))
	{
		return false;
	}
	const FCrowdyModScriptPrompt* First = Prompts.FindByPredicate([](const FCrowdyModScriptPrompt& Prompt) { return Prompt.AuthorId == AuthorA; });
	if (!TestNotNull(TEXT("the first author is asked"), First))
	{
		return false;
	}
	TestEqual(TEXT("on this grid"), First->GridId, PlotId);
	TestEqual(TEXT("about both of their scripts"), First->Scripts.Num(), 2);
	TestEqual(TEXT("for the author's union of calls"), First->Calls, TArray<FString>({TEXT("hud_set"), TEXT("voxel_set"), TEXT("input_key")}));
	TestEqual(TEXT("in the groups of the calls this build serves"), First->Groups,
		TArray<ECrowdyModScriptGroup>({ECrowdyModScriptGroup::WorldWrite, ECrowdyModScriptGroup::Present}));
	TestEqual(TEXT("naming every call this build refuses"), First->NotOffered, TArray<FString>({TEXT("input_key")}));
	TestEqual(TEXT("asking runs nothing"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);

	Harness.Scripts->TickForTest(10.0);
	const int32 SecondId = Prompts.FindByPredicate([](const FCrowdyModScriptPrompt& Prompt) { return Prompt.AuthorId == AuthorB; })->PromptId;
	Harness.List({Unagreed(TEXT("a")), Unagreed(TEXT("b")), Unagreed(TEXT("c"), AuthorB, TEXT("a2"))});
	TestEqual(TEXT("an open prompt is not asked twice; a changed one is asked again"), Prompts.Num(), 3);
	TestEqual(TEXT("and the stale one is withdrawn"), Harness.Recorder->Withdrawn, TArray<int32>({SecondId}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsDeclineTest, "CrowdySDK.ModScripts.DeclineKeptUntilHashChanges", Flags)
bool FCrowdyModScriptsDeclineTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered, listed and asked"), Harness.Enter() && Harness.List({Unagreed(TEXT("a"))}) && Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Harness.Scripts->AnswerPrompt(Harness.Recorder->Prompts[0].PromptId, false);
	TestEqual(TEXT("not now calls nothing"), Harness.Fake->Pending.Num(), 0);

	Harness.Scripts->TickForTest(10.0);
	Harness.List({Unagreed(TEXT("a"))});
	TestEqual(TEXT("a decline is not asked again"), Harness.Recorder->Prompts.Num(), 1);
	TestEqual(TEXT("and an answered prompt is never withdrawn"), Harness.Recorder->Withdrawn.Num(), 0);

	Harness.Scripts->TickForTest(20.0);
	Harness.List({Unagreed(TEXT("a"), AuthorA, TEXT("a2"))});
	TestEqual(TEXT("until the author's hash changes"), Harness.Recorder->Prompts.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsLeavingTest, "CrowdySDK.ModScripts.LeavingForgetsDeclines", Flags)
bool FCrowdyModScriptsLeavingTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	const TArray<FRowSpec> Rows = {Unagreed(TEXT("a")), Unagreed(TEXT("c"), AuthorB)};
	if (!TestTrue(TEXT("entered, listed and asked"), Harness.Enter() && Harness.List(Rows) && Harness.Recorder->Prompts.Num() == 2))
	{
		return false;
	}
	const FCrowdyModScriptPrompt Declined = Harness.Recorder->Prompts[0];
	const FCrowdyModScriptPrompt Open = Harness.Recorder->Prompts[1];
	Harness.Scripts->AnswerPrompt(Declined.PromptId, false);

	Harness.Scripts->LeaveGrid();
	TestEqual(TEXT("leaving withdraws the open prompt only"), Harness.Recorder->Withdrawn, TArray<int32>({Open.PromptId}));
	TestEqual(TEXT("and leaves the grid"), Harness.Scripts->GetGrid().GridId, static_cast<int64>(0));

	TestTrue(TEXT("entered again"), Harness.Enter() && Harness.List(Rows));
	TestEqual(TEXT("the decline was forgotten"), Harness.Recorder->Prompts.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsRunTrustsThenFetchesTest, "CrowdySDK.ModScripts.RunTrustsThenFetches", Flags)
bool FCrowdyModScriptsRunTrustsThenFetchesTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered, listed and asked"), Harness.Enter() && Harness.List({Unagreed(TEXT("a")), Unagreed(TEXT("b"))})
		&& Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Harness.Scripts->AnswerPrompt(Harness.Recorder->Prompts[0].PromptId, true);
	const FCall* Trust = Harness.Fake->Find(ECrowdyModScriptsCall::TrustAuthor);
	TestTrue(TEXT("Run trusts the author on this grid at the union hash"),
		Trust && Trust->GridId == PlotId && Trust->AuthorId == AuthorA && Trust->Hash == TEXT("a1"));
	TestEqual(TEXT("before anything is fetched"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);

	Harness.Fake->Answer(ECrowdyModScriptsCall::TrustAuthor, Ok());
	TestNotNull(TEXT("then fetches the first script"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	TestNotNull(TEXT("and the second"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));

	FFakeReply Hello;
	if (!HelloArtifact(*this, Hello))
	{
		return false;
	}
	// The listing said d1, so the served module is not the one agreed to.
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Hello, TEXT("a"));
	TestTrue(TEXT("a module whose digest is not the listed one is not run"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Failed));
	TestFalse(TEXT("and stays stopped"), Harness.IsRunning(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHoldbackTest, "CrowdySDK.ModScripts.HoldbackAfterNotFound", Flags)
bool FCrowdyModScriptsHoldbackTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	const TArray<FRowSpec> Rows = {Agreed(TEXT("a")), Agreed(TEXT("b"))};
	if (!TestTrue(TEXT("entered and listed"), Harness.Enter() && Harness.List(Rows)))
	{
		return false;
	}
	TestTrue(TEXT("a consented script is fetched"), Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Refused(TEXT("NOT_FOUND")), TEXT("a")));
	// The live API can name the code only at the start of its message, under a broader one.
	FFakeReply Limited = Refused(TEXT("BAD_REQUEST"));
	Limited.Message = TEXT("RATE_LIMITED: too many fetches");
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Limited, TEXT("b"));
	TestTrue(TEXT("a failed fetch stops it as Failed"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Failed));
	TestEqual(TEXT("and fires no error"), Harness.Recorder->Errors.Num(), 0);

	Harness.Scripts->TickForTest(14.5);
	Harness.List(Rows);
	TestEqual(TEXT("NOT_FOUND holds it back"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
	Harness.Scripts->TickForTest(15.0);
	TestNotNull(TEXT("for 15 seconds"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	TestNull(TEXT("RATE_LIMITED for longer"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Refused(TEXT("NOT_FOUND")), TEXT("a"));
	Harness.Scripts->TickForTest(60.0);
	TestNotNull(TEXT("60 seconds"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsRefusalTest, "CrowdySDK.ModScripts.RefusalFiresError", Flags)
bool FCrowdyModScriptsRefusalTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	const TArray<FRowSpec> Rows = {Unagreed(TEXT("a")), Unagreed(TEXT("b"))};
	if (!TestTrue(TEXT("entered, listed and asked"), Harness.Enter() && Harness.List(Rows) && Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Harness.Scripts->AnswerPrompt(Harness.Recorder->Prompts[0].PromptId, true);
	Harness.Fake->Answer(ECrowdyModScriptsCall::TrustAuthor, Refused(TEXT("FORBIDDEN")));
	const TArray<FCrowdyModScriptError>& Errors = Harness.Recorder->Errors;
	TestEqual(TEXT("a refusal fires an error for each script it covered"), Errors.Num(), 2);
	TestTrue(TEXT("with the platform's code"), Errors.Num() == 2 && Errors[0].Code == TEXT("FORBIDDEN") && Errors[1].Code == TEXT("FORBIDDEN"));
	TestTrue(TEXT("naming the scripts"), Errors.ContainsByPredicate([](const FCrowdyModScriptError& Error) { return Error.ModId == TEXT("a"); }));
	TestEqual(TEXT("and fetches nothing"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);

	Harness.Scripts->TickForTest(10.0);
	Harness.List(Rows);
	TestEqual(TEXT("a refused script is not asked about again in this grid"), Harness.Recorder->Prompts.Num(), 1);

	Harness.Scripts->LeaveGrid();
	TestTrue(TEXT("entered again"), Harness.Enter() && Harness.List(Rows));
	TestEqual(TEXT("until the player enters the grid again"), Harness.Recorder->Prompts.Num(), 2);

	Harness.Scripts->AnswerPrompt(Harness.Recorder->Prompts.Last().PromptId, true);
	Harness.Fake->Answer(ECrowdyModScriptsCall::TrustAuthor, Refused(TEXT("FORBIDDEN")));
	Harness.Scripts->TickForTest(20.0);
	TArray<FRowSpec> Changed = Rows;
	Changed[0].Hash = TEXT("h-a-2");
	Harness.List(Changed);
	TestEqual(TEXT("or until the refused script changes"), Harness.Recorder->Prompts.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsConflictTest, "CrowdySDK.ModScripts.ConflictAsksAgain", Flags)
bool FCrowdyModScriptsConflictTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered, listed and asked"), Harness.Enter() && Harness.List({Unagreed(TEXT("a"))}) && Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Harness.Scripts->AnswerPrompt(Harness.Recorder->Prompts[0].PromptId, true);
	Harness.Fake->Answer(ECrowdyModScriptsCall::TrustAuthor, Refused(TEXT("CONFLICT")));
	TestEqual(TEXT("a stale hash is not a refusal"), Harness.Recorder->Errors.Num(), 0);

	Harness.Scripts->TickForTest(0.5);
	TestEqual(TEXT("the grid is listed again at once"), Harness.Fake->Count(ECrowdyModScriptsCall::ListMods), 1);
	Harness.List({Unagreed(TEXT("a"), AuthorA, TEXT("a2"))});
	TestEqual(TEXT("and the new union asked about"), Harness.Recorder->Prompts.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsListingStopsTest, "CrowdySDK.ModScripts.ListingStopsScripts", Flags)
bool FCrowdyModScriptsListingStopsTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	FFakeReply Hello;
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered"), Harness.Enter()))
	{
		return false;
	}
	TArray<FRowSpec> Rows = {Served(TEXT("gone"), AuthorA, Hello.Digest), Served(TEXT("changed"), AuthorA, Hello.Digest),
		Served(TEXT("revoked"), AuthorA, Hello.Digest), Served(TEXT("kept"), AuthorA, Hello.Digest)};
	for (FRowSpec& Spec : Rows)
	{
		Spec.bConsented = true;
	}
	ListAndRun(Harness, Rows, Hello);
	if (!TestEqual(TEXT("every consented script runs"), Harness.Scripts->GetRunning().Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("each start is told"), Harness.Recorder->Started.Num(), 4);
	TSet<int32> Generations;
	for (const FCrowdyModScript& Script : Harness.Recorder->Started)
	{
		Generations.Add(Script.Generation);
	}
	TestEqual(TEXT("each run has its own generation"), Generations.Num(), 4);

	Harness.Scripts->TickForTest(10.0);
	Rows.RemoveAt(0);
	Rows[0].Hash = TEXT("h-changed-2");
	Rows[1].bConsented = false;
	Harness.List(Rows);
	TestTrue(TEXT("one that left the list is Removed"), Harness.Recorder->WasStopped(TEXT("gone"), ECrowdyModScriptStop::Removed));
	TestTrue(TEXT("one whose hash changed is Changed"), Harness.Recorder->WasStopped(TEXT("changed"), ECrowdyModScriptStop::Changed));
	TestTrue(TEXT("one agreed to by nobody is Unconsented"), Harness.Recorder->WasStopped(TEXT("revoked"), ECrowdyModScriptStop::Unconsented));
	TestTrue(TEXT("the unchanged one keeps running"), Harness.IsRunning(TEXT("kept")));
	TestEqual(TEXT("the host clears each stopped run"), Harness.Recorder->Cleared.Num(), 3);

	TestNotNull(TEXT("the changed version is fetched again"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("changed")));
	TestTrue(TEXT("and runs"), FetchAndRun(Harness, TEXT("changed"), Hello));
	TestTrue(TEXT("as a new generation"), Harness.Recorder->Started.Num() == 5 && !Generations.Contains(Harness.Recorder->Started.Last().Generation));
	TestEqual(TEXT("one taken back elsewhere is not fetched again"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsRevokeTest, "CrowdySDK.ModScripts.RevokeReconsentsOthers", Flags)
bool FCrowdyModScriptsRevokeTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	FFakeReply Hello;
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered"), Harness.Enter()))
	{
		return false;
	}
	TArray<FRowSpec> Rows = {Served(TEXT("a"), AuthorA, Hello.Digest), Served(TEXT("b"), AuthorA, Hello.Digest)};
	for (FRowSpec& Spec : Rows)
	{
		Spec.bConsented = true;
		Spec.bTrustsAuthor = true;
	}
	ListAndRun(Harness, Rows, Hello);
	if (!TestEqual(TEXT("both trusted scripts run"), Harness.Scripts->GetRunning().Num(), 2))
	{
		return false;
	}

	Harness.Scripts->Revoke(TEXT("a"));
	TestTrue(TEXT("Revoke stops it at once"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Revoked));
	TestNotNull(TEXT("takes back the consent"), Harness.Fake->Find(ECrowdyModScriptsCall::RevokeConsent, TEXT("a")));
	const FCall* Untrust = Harness.Fake->Find(ECrowdyModScriptsCall::RevokeTrust);
	TestTrue(TEXT("and, as it ran by trust, the trust"), Untrust && Untrust->GridId == PlotId && Untrust->AuthorId == AuthorA);
	TestEqual(TEXT("re-consenting only once the trust is gone"), Harness.Fake->Count(ECrowdyModScriptsCall::ConsentMod), 0);

	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeTrust, Ok());
	const FCall* Consent = Harness.Fake->Find(ECrowdyModScriptsCall::ConsentMod);
	TestTrue(TEXT("the author's other running script is consented to at its own hash"), Consent && Consent->ModId == TEXT("b") && Consent->Hash == TEXT("h-b"));
	TestTrue(TEXT("and keeps running"), Harness.IsRunning(TEXT("b")));
	Harness.Fake->Answer(ECrowdyModScriptsCall::ConsentMod, Ok());
	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeConsent, Ok());

	Harness.Scripts->TickForTest(10.0);
	Rows[0].bConsented = false;
	Rows[0].bTrustsAuthor = false;
	Rows[1].bTrustsAuthor = false;
	Harness.List(Rows);
	TestTrue(TEXT("the platform's new answer keeps it running"), Harness.IsRunning(TEXT("b")));
	TestEqual(TEXT("what was taken back is not asked about"), Harness.Recorder->Prompts.Num(), 0);
	TestEqual(TEXT("or fetched"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
	TestEqual(TEXT("and no error fired"), Harness.Recorder->Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsForgetAuthorTest, "CrowdySDK.ModScripts.ForgetAuthor", Flags)
bool FCrowdyModScriptsForgetAuthorTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	FFakeReply Hello;
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered"), Harness.Enter()))
	{
		return false;
	}
	TArray<FRowSpec> Rows = {Served(TEXT("a"), AuthorA, Hello.Digest), Served(TEXT("b"), AuthorA, Hello.Digest), Served(TEXT("c"), AuthorB, Hello.Digest)};
	for (FRowSpec& Spec : Rows)
	{
		Spec.bConsented = true;
		Spec.bTrustsAuthor = Spec.AuthorId == AuthorA;
	}
	ListAndRun(Harness, Rows, Hello);
	if (!TestEqual(TEXT("every script runs"), Harness.Scripts->GetRunning().Num(), 3))
	{
		return false;
	}

	Harness.Scripts->ForgetAuthor(AuthorA);
	TestTrue(TEXT("the author's first script stops"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Revoked));
	TestTrue(TEXT("and their second"), Harness.Recorder->WasStopped(TEXT("b"), ECrowdyModScriptStop::Revoked));
	TestTrue(TEXT("another author's keeps running"), Harness.IsRunning(TEXT("c")));
	const FCall* Untrust = Harness.Fake->Find(ECrowdyModScriptsCall::RevokeTrust);
	TestTrue(TEXT("the trust is taken back"), Untrust && Untrust->GridId == PlotId && Untrust->AuthorId == AuthorA);
	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeTrust, Ok());

	Harness.Scripts->TickForTest(10.0);
	for (FRowSpec& Spec : Rows)
	{
		Spec.bConsented = Spec.AuthorId == AuthorB;
		Spec.bTrustsAuthor = false;
	}
	Harness.List(Rows);
	TestEqual(TEXT("the author is not asked about again"), Harness.Recorder->Prompts.Num(), 0);
	TestEqual(TEXT("nor their scripts fetched"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsAgreementAcrossGridsTest, "CrowdySDK.ModScripts.AgreementAcrossGrids", Flags)
bool FCrowdyModScriptsAgreementAcrossGridsTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	Harness.Scripts->Revoke(TEXT("x"));
	TestNotNull(TEXT("a revoke is sent outside any grid"), Harness.Fake->Find(ECrowdyModScriptsCall::RevokeConsent, TEXT("x")));
	if (!TestTrue(TEXT("entered and listed"), Harness.Enter() && Harness.List({Agreed(TEXT("a"))})))
	{
		return false;
	}
	TestNotNull(TEXT("a call still in flight from before does not hold back the new grid's listing"),
		Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));

	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeConsent, Refused(TEXT("FORBIDDEN")), TEXT("x"));
	TestTrue(TEXT("its refusal is still told"), Harness.Recorder->Errors.ContainsByPredicate([](const FCrowdyModScriptError& Error) { return Error.ModId == TEXT("x"); }));

	Harness.Scripts->ForgetAuthor(AuthorA);
	Harness.Scripts->LeaveGrid();
	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeTrust, Refused(TEXT("FORBIDDEN")));
	TestTrue(TEXT("and a trust revoke's refusal arrives after the player left the grid"),
		Harness.Recorder->Errors.ContainsByPredicate([](const FCrowdyModScriptError& Error) { return Error.ModId == TEXT("a"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsFailedLookupTest, "CrowdySDK.ModScripts.FailedLookupAskedAgain", Flags)
bool FCrowdyModScriptsFailedLookupTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	Harness.Scripts->EnterGrid(5, 5, 0);
	Harness.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Refused(TEXT("UNAUTHENTICATED")));
	Harness.Scripts->EnterGrid(5, 5, 0);
	TestEqual(TEXT("a caller naming the chunk every frame does not flood"), Harness.Fake->Count(ECrowdyModScriptsCall::GridPermissions), 0);

	Harness.Scripts->TickForTest(2.0);
	Harness.Scripts->EnterGrid(5, 5, 0);
	TestEqual(TEXT("a failed lookup is asked again after a short hold"), Harness.Fake->Count(ECrowdyModScriptsCall::GridPermissions), 1);
	Harness.Fake->Answer(ECrowdyModScriptsCall::GridPermissions, Ok());

	Harness.Scripts->TickForTest(10.0);
	Harness.Scripts->EnterGrid(5, 5, 0);
	TestEqual(TEXT("an answer that no grid covers the chunk is kept"), Harness.Fake->Count(ECrowdyModScriptsCall::GridPermissions), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsUnreadableRowTest, "CrowdySDK.ModScripts.UnreadableRowSkipped", Flags)
bool FCrowdyModScriptsUnreadableRowTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered"), Harness.Enter()))
	{
		return false;
	}
	Harness.Fake->Answer(ECrowdyModScriptsCall::ListMods, ListingWithJunk({Agreed(TEXT("a"))}));
	TestNotNull(TEXT("a row that does not read leaves the others applied"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsListingFailureTest, "CrowdySDK.ModScripts.ListingFailureStopsScripts", Flags)
bool FCrowdyModScriptsListingFailureTest::RunTest(const FString& Parameters)
{
	FFakeReply Hello;
	if (!HelloArtifact(*this, Hello))
	{
		return false;
	}
	TArray<FRowSpec> Rows = {Agreed(TEXT("a")), Agreed(TEXT("b"))};
	{
		FHarness Harness;
		if (!TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, Rows, Hello) && Harness.Scripts->GetRunning().Num() == 2))
		{
			return false;
		}
		for (FRowSpec& Spec : Rows)
		{
			Spec.Digest = Hello.Digest;
		}
		Harness.Scripts->TickForTest(10.0);
		TArray<FRowSpec> Unreadable = Rows;
		Unreadable[1].TickIntervalMs = 5;
		Harness.List(Unreadable);
		TestTrue(TEXT("a running script whose row no longer reads is Removed"), Harness.Recorder->WasStopped(TEXT("b"), ECrowdyModScriptStop::Removed));
		TestTrue(TEXT("while the others keep running"), Harness.IsRunning(TEXT("a")));

		Harness.Scripts->TickForTest(20.0);
		Harness.Fake->Answer(ECrowdyModScriptsCall::ListMods, Refused(TEXT("FORBIDDEN")));
		TestTrue(TEXT("a refused listing stops every script"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Removed));
		Harness.Scripts->TickForTest(25.0);
		TestEqual(TEXT("and starts none"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
		Harness.Scripts->TickForTest(30.0);
		Harness.List(Rows);
		TestNotNull(TEXT("until a listing reads"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	}

	FHarness Harness;
	if (!TestTrue(TEXT("entered and running again"), Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a"))}, Hello)))
	{
		return false;
	}
	for (int32 Failure = 1; Failure <= 3; ++Failure)
	{
		TestTrue(FString::Printf(TEXT("still running before failure %d"), Failure), Harness.IsRunning(TEXT("a")));
		Harness.Scripts->TickForTest(10.0 * Failure);
		Harness.Fake->Answer(ECrowdyModScriptsCall::ListMods, FFakeReply());
	}
	TestTrue(TEXT("three listings that do not read stop it"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Removed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsTrustCoversTakenBackTest, "CrowdySDK.ModScripts.TrustCoversTakenBack", Flags)
bool FCrowdyModScriptsTrustCoversTakenBackTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered and listed"), Harness.Enter() && Harness.List({Agreed(TEXT("a")), Agreed(TEXT("b"))})))
	{
		return false;
	}
	Harness.Scripts->Revoke(TEXT("a"));
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Refused(TEXT("NOT_FOUND")), TEXT("a"));
	Harness.Fake->Answer(ECrowdyModScriptsCall::RevokeConsent, Ok(), TEXT("a"));

	Harness.Scripts->TickForTest(10.0);
	Harness.List({Unagreed(TEXT("a")), Agreed(TEXT("b")), Unagreed(TEXT("c"))});
	if (!TestEqual(TEXT("the author is asked about the new script"), Harness.Recorder->Prompts.Num(), 1))
	{
		return false;
	}
	const FCrowdyModScriptPrompt& Prompt = Harness.Recorder->Prompts[0];
	TestEqual(TEXT("naming every script the trust covers"), Prompt.Scripts.Num(), 3);
	TestTrue(TEXT("the one taken back included"), Prompt.Scripts.Contains(TEXT("a-name")));

	Harness.Scripts->AnswerPrompt(Prompt.PromptId, true);
	Harness.Fake->Answer(ECrowdyModScriptsCall::TrustAuthor, Ok());
	TestNotNull(TEXT("the new script runs"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("c")));
	TestNull(TEXT("the one taken back this visit does not"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHostCallFloorTest, "CrowdySDK.ModScripts.HostCallsAnsweredEveryRun", Flags)
bool FCrowdyModScriptsHostCallFloorTest::RunTest(const FString& Parameters)
{
	// hud_set passes the worker's checks, so each call reaches the game thread's drain.
	FFakeReply Hud;
	FHarness Harness;
	if (!ModuleArtifact(*this, Hud, TEXT("hud.wasm")) || !TestTrue(TEXT("entered and running"),
		Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a")), Agreed(TEXT("b"))}, Hud) && Harness.Scripts->GetRunning().Num() == 2))
	{
		return false;
	}
	// With the frame's budget already spent, every run still gets an answer each frame.
	Harness.Scripts->SetHostBudgetForTest(0.0);
	double NowSeconds = 0.0;
	for (int32 Frame = 0; Frame < 600 && (Harness.Scripts->GetHostCallsForTest(TEXT("a")) == 0 || Harness.Scripts->GetHostCallsForTest(TEXT("b")) == 0); ++Frame)
	{
		NowSeconds += 0.01;
		Harness.Scripts->TickForTest(NowSeconds);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("the first run's calls are answered"), Harness.Scripts->GetHostCallsForTest(TEXT("a")) > 0);
	TestTrue(TEXT("and the second's"), Harness.Scripts->GetHostCallsForTest(TEXT("b")) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsCheckHoldbackTest, "CrowdySDK.ModScripts.HoldbackAfterCheckRefusal", Flags)
bool FCrowdyModScriptsCheckHoldbackTest::RunTest(const FString& Parameters)
{
	FHarness Harness;
	if (!TestTrue(TEXT("entered and listed"), Harness.Enter() && Harness.List({Agreed(TEXT("a")), Agreed(TEXT("b"))})))
	{
		return false;
	}
	FFakeReply Checked;
	Checked.bCheckRefused = true;
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Checked, TEXT("a"));
	FFakeReply Mismatched = Ok();
	Mismatched.Digest = TEXT("not-d1");
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Mismatched, TEXT("b"));
	TestTrue(TEXT("a module failing its check is Failed"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Failed));
	TestTrue(TEXT("as is one whose digest is not the listed one"), Harness.Recorder->WasStopped(TEXT("b"), ECrowdyModScriptStop::Failed));

	Harness.Scripts->TickForTest(59.0);
	TestEqual(TEXT("both are held back"), Harness.Fake->Count(ECrowdyModScriptsCall::FetchArtifact), 0);
	Harness.Scripts->TickForTest(60.0);
	TestNotNull(TEXT("for 60 seconds"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("a")));
	TestNotNull(TEXT("each"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsSignOutTest, "CrowdySDK.ModScripts.SignOutStopsEverything", Flags)
bool FCrowdyModScriptsSignOutTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	const TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	Session->SetUserID(5);
	FFakeReply Hello;
	FHarness Harness(Session.Get());
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered, running and asking"),
		Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a")), Unagreed(TEXT("c"), AuthorB)}, Hello) && Harness.IsRunning(TEXT("a"))
		&& Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Session->SetUserID(0);
	Harness.Scripts->TickForTest(1.0);
	TestTrue(TEXT("signing out stops every script"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Stopped));
	TestEqual(TEXT("withdraws the open prompt"), Harness.Recorder->Withdrawn, TArray<int32>({Harness.Recorder->Prompts[0].PromptId}));
	TestEqual(TEXT("and leaves the grid"), Harness.Scripts->GetGrid().GridId, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsDeinitializeTest, "CrowdySDK.ModScripts.DeinitializeStopsEverything", Flags)
bool FCrowdyModScriptsDeinitializeTest::RunTest(const FString& Parameters)
{
	FFakeReply Hello;
	FHarness Harness;
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered, running and asking"),
		Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a")), Unagreed(TEXT("c"), AuthorB)}, Hello) && Harness.IsRunning(TEXT("a"))
		&& Harness.Recorder->Prompts.Num() == 1))
	{
		return false;
	}
	Harness.Scripts->Deinitialize();
	TestTrue(TEXT("the world ending stops every script"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::Stopped));
	TestTrue(TEXT("clears it from the host"), Harness.Recorder->Cleared.Contains(TEXT("a")));
	TestEqual(TEXT("withdraws the open prompt"), Harness.Recorder->Withdrawn, TArray<int32>({Harness.Recorder->Prompts[0].PromptId}));
	TestEqual(TEXT("and runs nothing"), Harness.Scripts->GetRunning().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsCompileOffThreadTest, "CrowdySDK.ModScripts.CompileOffTheGameThread", Flags)
bool FCrowdyModScriptsCompileOffThreadTest::RunTest(const FString& Parameters)
{
	FFakeReply Hello;
	FHarness Harness;
	if (!HelloArtifact(*this, Hello) || !TestTrue(TEXT("entered"), Harness.Enter()))
	{
		return false;
	}
	FRowSpec Taken = Agreed(TEXT("a"));
	Taken.Digest = Hello.Digest;
	FFakeReply Junk = Ok();
	Junk.Bytes = {0x00, 0x61, 0x73, 0x6d, 0xff};
	Junk.Digest = FCrowdyModScriptModuleCache::Sha256Hex(Junk.Bytes);
	FRowSpec Broken = Agreed(TEXT("b"));
	Broken.Digest = Junk.Digest;
	Harness.List({Taken, Broken});

	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Hello, TEXT("a"));
	TestFalse(TEXT("a fetched module is not started inside the answer"), Harness.IsRunning(TEXT("a")));
	Harness.Scripts->Revoke(TEXT("a"));
	Harness.Fake->Answer(ECrowdyModScriptsCall::FetchArtifact, Junk, TEXT("b"));
	PumpUntil([&Harness]() { return Harness.Recorder->WasStopped(TEXT("b"), ECrowdyModScriptStop::Failed); });
	FPlatformProcess::Sleep(0.1f);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

	TestFalse(TEXT("one taken back while it compiled is never started"), Harness.IsRunning(TEXT("a")));
	TestEqual(TEXT("nor told as started"), Harness.Recorder->Started.Num(), 0);
	TestTrue(TEXT("a module that does not compile is Failed"), Harness.Recorder->WasStopped(TEXT("b"), ECrowdyModScriptStop::Failed));
	Harness.Scripts->TickForTest(59.0);
	TestNull(TEXT("and held back"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));
	Harness.Scripts->TickForTest(60.0);
	TestNotNull(TEXT("for 60 seconds"), Harness.Fake->Find(ECrowdyModScriptsCall::FetchArtifact, TEXT("b")));
	return true;
}

namespace CrowdyModScriptsTests
{
	/** Ticks one simulated frame of 10 ms at a time until Done or Frames pass. */
	void RunFrames(FHarness& Harness, double& NowSeconds, int32 Frames, const TFunctionRef<bool()> Done)
	{
		for (int32 Frame = 0; Frame < Frames && !Done(); ++Frame)
		{
			NowSeconds += 0.01;
			Harness.Scripts->TickForTest(NowSeconds);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHudReachesHostTest, "CrowdySDK.ModScripts.HudReachesTheHost", Flags)
bool FCrowdyModScriptsHudReachesHostTest::RunTest(const FString& Parameters)
{
	FFakeReply Hud;
	FHarness Harness;
	if (!ModuleArtifact(*this, Hud, TEXT("hud.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a"))}, Hud) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 600, [&Harness]() { return Harness.Recorder->Hud.Num() > 0; });
	TestTrue(TEXT("the script's hud_set reaches the host"),
		Harness.Recorder->Hud.Num() > 0 && Harness.Recorder->Hud[0].Equals(TEXT("hi"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("clicks are drained every frame, though no script listens"), Harness.Recorder->ClickDrains > 0);
	Harness.Scripts->LeaveGrid();
	TestTrue(TEXT("leaving clears it from the host"), Harness.Recorder->Cleared.Contains(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsFloodOpensCircuitTest, "CrowdySDK.ModScripts.FloodOpensTheCircuit", Flags)
bool FCrowdyModScriptsFloodOpensCircuitTest::RunTest(const FString& Parameters)
{
	FFakeReply Spam;
	FHarness Harness;
	if (!ModuleArtifact(*this, Spam, TEXT("spam.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a"))}, Spam) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("made more than 1000 host calls in a second"), EAutomationExpectedErrorFlags::Contains, 1);
	Harness.Scripts->SetHostBudgetForTest(50.0);
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 600, [&Harness]() { return Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::CircuitOpen); });
	TestTrue(TEXT("1,001 calls in a second open the circuit"), Harness.Recorder->WasStopped(TEXT("a"), ECrowdyModScriptStop::CircuitOpen));
	TestEqual(TEXT("and only the group's cap reached the host"), Harness.Recorder->Hud.Num(), 120);
	TestEqual(TEXT("refused calls never reach the game thread"), Harness.Scripts->GetHostCallsForTest(TEXT("a")), 120);
	TestTrue(TEXT("which is cleared"), Harness.Recorder->Cleared.Contains(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsWorldReachesHostTest, "CrowdySDK.ModScripts.WorldCallsReachTheHost", Flags)
bool FCrowdyModScriptsWorldReachesHostTest::RunTest(const FString& Parameters)
{
	FFakeReply World;
	FHarness Harness;
	FRowSpec Spec = Agreed(TEXT("a"));
	Spec.Calls = WorldCalls;
	if (!ModuleArtifact(*this, World, TEXT("world.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Spec}, World) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	UCrowdyModScriptsTestRecorder& Host = *Harness.Recorder;
	Harness.Scripts->SetHostBudgetForTest(50.0);
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 600, [&Host]() { return Host.Writes.Num() >= 3; });
	if (!TestTrue(TEXT("voxel_set reaches the host"), Host.Writes.Num() >= 3))
	{
		return false;
	}
	const FCrowdyVoxelWrite& Write = Host.Writes[0];
	TestTrue(TEXT("as the write the script named"), Write.ChunkX == 5 && Write.ChunkY == 5 && Write.ChunkZ == 0 && Write.Voxel.Position == FIntVector(1, 2, 3)
		&& Write.Voxel.Type == 7);
	const FCrowdyModScriptGrid Area = Host.PlayerAreas.IsEmpty() ? FCrowdyModScriptGrid() : Host.PlayerAreas[0];
	TestTrue(TEXT("actors_list_radius asks about the clipped box"), Area.GridId == PlotId && Area.LowChunkX == 2 && Area.HighChunkX == 8 && Area.LowChunkY == 4
		&& Area.HighChunkY == 6 && Area.LowChunkZ == 0 && Area.HighChunkZ == 1);
	TestTrue(TEXT("clicks are drained once a frame"), Host.ClickDrains > 0 && Host.ClickDrains <= static_cast<int32>(NowSeconds / 0.01) + 1);
	const int32 Answered = Harness.Scripts->GetHostCallsForTest(TEXT("a"));
	AddInfo(FString::Printf(TEXT("writes %d, chunk reads %d, calls answered %d, click drains %d"), Host.Writes.Num(), Host.ChunkReads, Answered, Host.ClickDrains));
	TestTrue(TEXT("chunks are read from the host, and again on later frames"), Host.ChunkReads > 1);

	const int32 Before = Host.ChunkReads;
	const bool bLoaded = Harness.Scripts->ReadChunkForTest(Host, 6, 6, 0) && Harness.Scripts->ReadChunkForTest(Host, 6, 6, 0);
	TestTrue(TEXT("a chunk read twice in a frame is read from the host once"), bLoaded && Host.ChunkReads == Before + 1);
	NowSeconds += 0.01;
	Harness.Scripts->TickForTest(NowSeconds);
	const int32 Ticked = Host.ChunkReads;
	Harness.Scripts->ReadChunkForTest(Host, 6, 6, 0);
	TestEqual(TEXT("and again on the next frame"), Host.ChunkReads, Ticked + 1);

	const int32 Written = Host.Writes.Num();
	Harness.Scripts->SetUnsentEditsForTest(64);
	RunFrames(Harness, NowSeconds, 60, []() { return false; });
	TestEqual(TEXT("while 64 edits wait unsent, voxel_set never reaches the host"), Host.Writes.Num(), Written);

	Harness.Scripts->SetUnsentEditsForTest(0);
	Harness.Scripts->ClearHost(TScriptInterface<ICrowdyModScriptHost>(&Host));
	const int32 Reads = Host.ChunkReads;
	const int32 Areas = Host.PlayerAreas.Num();
	RunFrames(Harness, NowSeconds, 60, []() { return false; });
	TestTrue(TEXT("with no host the world calls reach nothing"), Host.ChunkReads == Reads && Host.PlayerAreas.Num() == Areas && Host.Writes.Num() == Written);
	Harness.Scripts->LeaveGrid();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsEventCascadeTest, "CrowdySDK.ModScripts.EventsCascadeTo8", Flags)
bool FCrowdyModScriptsEventCascadeTest::RunTest(const FString& Parameters)
{
	// echo.wasm sends one event to script a on its first tick, and from each event it receives shows "echo" and sends a another.
	FFakeReply Echo;
	FHarness Harness;
	FRowSpec A = Agreed(TEXT("a"));
	FRowSpec B = Agreed(TEXT("b"));
	A.Calls = B.Calls = {TEXT("emit_event"), TEXT("hud_set")};
	if (!ModuleArtifact(*this, Echo, TEXT("echo.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {A, B}, Echo) && Harness.Scripts->GetRunning().Num() == 2))
	{
		return false;
	}
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 400, []() { return false; });
	TestEqual(TEXT("both first events reach a, each cascading until depth 8"), Harness.Recorder->Hud.Num(), 2 * CrowdyModScriptsHostCalls::MaxEventCascade);
	TestFalse(TEXT("and only a"), Harness.Recorder->HudFrom.Contains(TEXT("b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsEventSenderTest, "CrowdySDK.ModScripts.EventsSkipTheSender", Flags)
bool FCrowdyModScriptsEventSenderTest::RunTest(const FString& Parameters)
{
	// echoall.wasm does as echo.wasm, but sends to every other script: two of them pass each one's first event back and forth.
	FFakeReply Echo;
	FHarness Harness;
	FRowSpec A = Agreed(TEXT("a"));
	FRowSpec B = Agreed(TEXT("b"));
	A.Calls = B.Calls = {TEXT("emit_event"), TEXT("hud_set")};
	if (!ModuleArtifact(*this, Echo, TEXT("echoall.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {A, B}, Echo) && Harness.Scripts->GetRunning().Num() == 2))
	{
		return false;
	}
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 400, []() { return false; });
	TestEqual(TEXT("each script's events reach only the other, 8 deep each"), Harness.Recorder->Hud.Num(), 2 * CrowdyModScriptsHostCalls::MaxEventCascade);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsVoxelSetsHeldTest, "CrowdySDK.ModScripts.VoxelSetsHeldTo100", Flags)
bool FCrowdyModScriptsVoxelSetsHeldTest::RunTest(const FString& Parameters)
{
	FFakeReply Sets;
	FHarness Harness;
	// One tick of 150 calls a second stays under the 1,000-a-second breaker.
	FRowSpec Spec = Agreed(TEXT("a"));
	Spec.Calls = WorldCalls;
	Spec.TickIntervalMs = 1000;
	if (!ModuleArtifact(*this, Sets, TEXT("setspam.wasm")))
	{
		return false;
	}
	Sets.TickIntervalMs = 1000;
	if (!TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Spec}, Sets) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	Harness.Scripts->SetHostBudgetForTest(50.0);
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 300, [&Harness]() { return Harness.Scripts->GetHostCallsForTest(TEXT("a")) >= 150; });
	TestTrue(TEXT("one tick's 150 voxel_set calls were answered"), Harness.Scripts->GetHostCallsForTest(TEXT("a")) >= 150);
	TestEqual(TEXT("of which 100 reached the host, the rest answered busy"), Harness.Recorder->Writes.Num(), 100);
	RunFrames(Harness, NowSeconds, 300, [&Harness]() { return Harness.Recorder->Writes.Num() > 100; });
	TestTrue(TEXT("and more reach it once the second has passed"), Harness.Recorder->Writes.Num() > 100);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHostSwapClearsTest, "CrowdySDK.ModScripts.ReplacedHostClears", Flags)
bool FCrowdyModScriptsHostSwapClearsTest::RunTest(const FString& Parameters)
{
	FFakeReply Hud;
	FHarness Harness;
	if (!ModuleArtifact(*this, Hud, TEXT("hud.wasm"))
		|| !TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Agreed(TEXT("a"))}, Hud) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyModScriptsTestRecorder> Next(NewObject<UCrowdyModScriptsTestRecorder>(GetTransientPackage()));
	Harness.Scripts->SetHost(TScriptInterface<ICrowdyModScriptHost>(Next.Get()));
	TestTrue(TEXT("a replaced host clears every running script"), Harness.Recorder->Cleared.Contains(TEXT("a")));
	Harness.Scripts->SetHost(TScriptInterface<ICrowdyModScriptHost>(Next.Get()));
	TestEqual(TEXT("setting the same host again clears nothing"), Next->Cleared.Num(), 0);
	Harness.Scripts->ClearHost(TScriptInterface<ICrowdyModScriptHost>(Next.Get()));
	TestTrue(TEXT("and so does a host taken away"), Next->Cleared.Contains(TEXT("a")));
	return true;
}

namespace CrowdyModScriptsTests
{
	/** The platform's answer to a state call as the bridge hands it over: its data object, packed. */
	FFakeReply StateAnswer(const TCHAR* Field, const TCHAR* Row = TEXT("{\"userId\":\"42\",\"appId\":\"1\",\"state\":\"AAE=\"}"))
	{
		FFakeReply Reply = Ok();
		CrowdyNativeExec::JsonToMsgpack(FString::Printf(TEXT("{\"%s\":%s}"), Field, Row), Reply.Packed);
		return Reply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsPlatformCallsWaitTest, "CrowdySDK.ModScripts.PlatformCallsWait", Flags)
bool FCrowdyModScriptsPlatformCallsWaitTest::RunTest(const FString& Parameters)
{
	// platform.wasm sets the player's state and reads it back; only when that answered a row does it send a client event and
	// messages to channels 13 and 12.
	TArray<UCrowdyModScripts::FRealtimeSend> Sent;
	FFakeReply Module;
	FHarness Harness;
	FRowSpec Spec = Agreed(TEXT("a"));
	Spec.Calls = {TEXT("user_state_get"), TEXT("user_state_set"), TEXT("emit_spatial"), TEXT("emit_channel")};
	Spec.TickIntervalMs = 1000;
	if (!ModuleArtifact(*this, Module, TEXT("platform.wasm")))
	{
		return false;
	}
	Module.TickIntervalMs = 1000;
	if (!TestTrue(TEXT("entered and running"), Harness.Enter() && ListAndRun(Harness, {Spec}, Module) && Harness.IsRunning(TEXT("a"))))
	{
		return false;
	}
	Harness.Scripts->SetRealtimeForTest([&Sent](const UCrowdyModScripts::FRealtimeSend& Send, FString&)
	{
		Sent.Add(Send);
		return true;
	});
	FFakePlatform& Fake = *Harness.Fake;
	double NowSeconds = 0.0;
	RunFrames(Harness, NowSeconds, 300, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateSet) > 0; });
	const FCall* Set = Fake.Find(ECrowdyModScriptsCall::UserStateSet);
	TestTrue(TEXT("user_state_set asks the platform for the script's state"), Set && !Set->bClearState && Set->State == TEXT("AAE="));
	RunFrames(Harness, NowSeconds, 30, []() { return false; });
	TestTrue(TEXT("and the script waits for its answer, still running"), Fake.Count(ECrowdyModScriptsCall::UserStateGet) == 0 && Harness.IsRunning(TEXT("a")));
	TestTrue(TEXT("which the platform gives frames later"), Fake.Answer(ECrowdyModScriptsCall::UserStateSet, StateAnswer(TEXT("updateUserAppState"))));
	RunFrames(Harness, NowSeconds, 100, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateGet) > 0; });
	TestTrue(TEXT("then it reads the state back"), Fake.Answer(ECrowdyModScriptsCall::UserStateGet, StateAnswer(TEXT("userAppState"))));
	RunFrames(Harness, NowSeconds, 100, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::GridChannels) > 0; });

	if (!TestEqual(TEXT("and with its row answered, sends its client event"), Sent.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("from its chunk, as far as it asked, with its payload"), !Sent[0].bChannel && Sent[0].Chunk == FInt64Vector(5, 5, 0) && Sent[0].Distance == 2
		&& Sent[0].Payload == TArray<uint8>({7, 0}));
	TestEqual(TEXT("under its derived sender id"), FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Sent[0].Sender.GetData()), Sent[0].Sender.Num()),
		FString(TEXT("30768aa12be36d5e000e86aa384247f8")));
	const FCall* Channels = Fake.Find(ECrowdyModScriptsCall::GridChannels);
	TestTrue(TEXT("its first channel message waits for this grid's channels"), Channels && Channels->GridId == PlotId);
	FFakeReply Listed = Ok();
	Listed.ChannelIds = {12};
	Fake.Answer(ECrowdyModScriptsCall::GridChannels, Listed);
	RunFrames(Harness, NowSeconds, 50, []() { return false; });
	TestEqual(TEXT("the waiting message, to another grid's channel, gets nothing; the next goes at once to the grid's own"), Sent.Num(), 2);
	TestTrue(TEXT("channel 12 with its payload"), Sent.Num() == 2 && Sent[1].bChannel && Sent[1].ChannelId == 12 && Sent[1].Payload == TArray<uint8>({'h', 'i'}));
	TestEqual(TEXT("the channels are read once"), Fake.Count(ECrowdyModScriptsCall::GridChannels), 0);

	RunFrames(Harness, NowSeconds, 300, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateSet) > 0; });
	Fake.Answer(ECrowdyModScriptsCall::UserStateSet, Refused(TEXT("FORBIDDEN")));
	RunFrames(Harness, NowSeconds, 100, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateGet) > 0; });
	TestTrue(TEXT("on its next tick a refusal is answered too"), Fake.Answer(ECrowdyModScriptsCall::UserStateGet, StateAnswer(TEXT("userAppState"))));
	RunFrames(Harness, NowSeconds, 50, []() { return false; });
	TestEqual(TEXT("with the channels known, the other grid's channel is refused at once and the grid's own is sent"), Sent.Num(), 4);
	TestTrue(TEXT("to channel 12 only"), Sent.Num() == 4 && Sent[3].bChannel && Sent[3].ChannelId == 12);

	RunFrames(Harness, NowSeconds, 300, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateSet) > 0; });
	Fake.Answer(ECrowdyModScriptsCall::UserStateSet, StateAnswer(TEXT("updateUserAppState")));
	RunFrames(Harness, NowSeconds, 100, [&Fake]() { return Fake.Count(ECrowdyModScriptsCall::UserStateGet) > 0; });
	Fake.Answer(ECrowdyModScriptsCall::UserStateGet, StateAnswer(TEXT("userAppState"), TEXT("null")));
	RunFrames(Harness, NowSeconds, 50, []() { return false; });
	TestEqual(TEXT("and a player with no state reads no row, so nothing more is sent"), Sent.Num(), 4);
	TestTrue(TEXT("and the script still runs"), Harness.IsRunning(TEXT("a")));
	return true;
}

#endif

#endif
