#include "CrowdyModScriptRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_CROWDY_MOD_SCRIPT_RUNTIME

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace CrowdyModScriptRuntimeTests
{
	// Editor only: the test modules are read from Source, which a packaged game does not stage.
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	constexpr int64 Fuel = 100000000;
	constexpr int64 UnlimitedFuel = 1000000000000000;
	constexpr double WatchdogMaxMs = 600.0;

	const TCHAR* const ClockRequest = TEXT("{\"fn\":\"clock\",\"args\":{}}");
	const TCHAR* const ClockAnswer = TEXT("{\"ok\":true,\"data\":{\"ms\":42}}");

	// A minimal module whose only content is an import of env.f, a function the sandbox does not provide.
	const uint8 UnknownImportModule[] = {
		0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
		0x02, 0x09, 0x01, 0x03, 'e', 'n', 'v', 0x01, 'f', 0x00, 0x00,
	};

	FString TextOf(TConstArrayView<uint8> Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	TSharedPtr<FCrowdyModScriptRuntime> AvailableRuntime(FAutomationTestBase& Test)
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime = FCrowdyModScriptRuntime::Get();
		if (!Runtime || !Runtime->IsAvailable())
		{
			Test.AddError(FString::Printf(TEXT("Mod Script runtime unavailable: %s"), Runtime ? *Runtime->GetUnavailableReason() : TEXT("module not started")));
			return nullptr;
		}
		return Runtime;
	}

	bool LoadModuleBytes(FAutomationTestBase& Test, const TCHAR* FileName, TArray<uint8>& OutBytes)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		if (!Plugin)
		{
			Test.AddError(TEXT("CrowdySDK plugin not found"));
			return false;
		}
		const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"), TEXT("CrowdyModScriptRuntime"), TEXT("Tests"), TEXT("Modules"), FileName);
		if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
		{
			Test.AddError(FString::Printf(TEXT("Cannot read %s; build the test modules with Tests/Modules/build.ps1"), *Path));
			return false;
		}
		return true;
	}

	TSharedPtr<FCrowdyModScriptInstance> LoadInstance(FAutomationTestBase& Test, const TCHAR* FileName, FCrowdyModScriptHost Host = FCrowdyModScriptHost())
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(Test);
		TArray<uint8> Bytes;
		if (!Runtime || !LoadModuleBytes(Test, FileName, Bytes))
		{
			return nullptr;
		}
		FString Error;
		TSharedPtr<FCrowdyModScriptModule> Module = FCrowdyModScriptModule::Compile(Runtime.ToSharedRef(), Bytes, Error);
		TSharedPtr<FCrowdyModScriptInstance> Instance = Module ? FCrowdyModScriptInstance::Create(Module.ToSharedRef(), MoveTemp(Host), Error) : nullptr;
		if (!Instance)
		{
			Test.AddError(FString::Printf(TEXT("%s did not load: %s"), FileName, *Error));
		}
		return Instance;
	}

	// Stops the instance from another thread after 2 s, so a broken watchdog fails the test instead of hanging it.
	class FSafetyStop
	{
	public:
		explicit FSafetyStop(const TSharedRef<FCrowdyModScriptInstance>& Instance)
			: Cancelled(FPlatformProcess::GetSynchEventFromPool(true))
		{
			FEvent* Event = Cancelled;
			Done = Async(EAsyncExecution::Thread, [Instance, Event]()
			{
				if (!Event->Wait(2000))
				{
					Instance->RequestStop();
				}
			});
		}

		~FSafetyStop()
		{
			Cancelled->Trigger();
			Done.Wait();
			FPlatformProcess::ReturnSynchEventToPool(Cancelled);
		}

		UE_NONCOPYABLE(FSafetyStop);

	private:
		FEvent* Cancelled;
		TFuture<void> Done;
	};

	FCrowdyModScriptResult DispatchTick(FAutomationTestBase& Test, const TCHAR* FileName, FCrowdyModScriptHost Host = FCrowdyModScriptHost(), int64 TickFuel = Fuel)
	{
		TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(Test, FileName, MoveTemp(Host));
		if (!Instance)
		{
			FCrowdyModScriptResult NotLoaded;
			NotLoaded.Outcome = ECrowdyModScriptOutcome::Trap;
			NotLoaded.Message = TEXT("not loaded");
			return NotLoaded;
		}
		FSafetyStop Safety(Instance.ToSharedRef());
		return Instance->Dispatch(ECrowdyModScriptExport::Tick, TickFuel, 16);
	}

	FString CompileError(FAutomationTestBase& Test, const TCHAR* FileName)
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(Test);
		TArray<uint8> Bytes;
		if (!Runtime || !LoadModuleBytes(Test, FileName, Bytes))
		{
			return TEXT("not loaded");
		}
		FString Error;
		if (FCrowdyModScriptModule::Compile(Runtime.ToSharedRef(), Bytes, Error))
		{
			return FString();
		}
		return Error;
	}

	TArray<uint8> Utf8Bytes(const TCHAR* Text)
	{
		const FTCHARToUTF8 Utf8(Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	TSharedPtr<FCrowdyModScriptModule> LoadCachedModule(FAutomationTestBase& Test, const TCHAR* FileName)
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(Test);
		TArray<uint8> Bytes;
		if (!Runtime || !LoadModuleBytes(Test, FileName, Bytes))
		{
			return nullptr;
		}
		FString Error;
		TSharedPtr<FCrowdyModScriptModule> Module = FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), FCrowdyModScriptModuleCache::Sha256Hex(Bytes), Bytes, Error);
		if (!Module)
		{
			Test.AddError(FString::Printf(TEXT("%s did not load: %s"), FileName, *Error));
		}
		return Module;
	}

	TSharedPtr<FCrowdyModScriptRun> StartRun(FAutomationTestBase& Test, const TCHAR* FileName, FCrowdyModScriptRunHost Host, const TCHAR* Name)
	{
		TSharedPtr<FCrowdyModScriptModule> Module = LoadCachedModule(Test, FileName);
		if (!Module)
		{
			return nullptr;
		}
		FString Error;
		TSharedPtr<FCrowdyModScriptRun> Run = FCrowdyModScriptRun::Start(Module.ToSharedRef(), MoveTemp(Host), Fuel, FCrowdyModScriptRun::MinTickIntervalMs, Name, Error);
		if (!Run)
		{
			Test.AddError(FString::Printf(TEXT("%s did not start: %s"), FileName, *Error));
		}
		return Run;
	}

	// Ticks the run every 5 ms, draining its host calls when asked, until Done holds or TimeoutSeconds pass.
	bool Pump(FCrowdyModScriptRun& Run, double TimeoutSeconds, bool bDrain, TFunctionRef<bool()> Done)
	{
		const double DeadlineSeconds = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > DeadlineSeconds)
			{
				return false;
			}
			Run.Tick(FPlatformTime::Seconds());
			if (bDrain)
			{
				Run.DrainHostCalls(2.0);
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	// Collects LogCrowdyModScripts lines that start with Prefix, from any thread.
	class FLogCapture final : public FOutputDevice
	{
	public:
		explicit FLogCapture(const FString& InPrefix)
			: Prefix(InPrefix)
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FLogCapture() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName& Category) override
		{
			static const FName CategoryName(TEXT("LogCrowdyModScripts"));
			if (Category != CategoryName || FCString::Strncmp(Text, *Prefix, Prefix.Len()) != 0)
			{
				return;
			}
			FScopeLock Guard(&Mutex);
			Lines.Add(Text);
		}

		virtual bool CanBeUsedOnAnyThread() const override
		{
			return true;
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		TArray<FString> GetLines()
		{
			GLog->FlushThreadedLogs();
			FScopeLock Guard(&Mutex);
			return Lines;
		}

	private:
		FString Prefix;
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	// Sets crowdy.modscripts.trace for one test and puts the old value back.
	class FScopedTrace final
	{
	public:
		explicit FScopedTrace(const bool bOn)
			: Var(IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.modscripts.trace")))
			, Old(Var ? Var->GetInt() : 0)
		{
			if (Var)
			{
				Var->Set(bOn ? 1 : 0, ECVF_SetByCode);
			}
		}

		~FScopedTrace()
		{
			if (Var)
			{
				Var->Set(Old, ECVF_SetByCode);
			}
		}

	private:
		IConsoleVariable* Var;
		int32 Old;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptHelloTest, "CrowdySDK.ModScriptRuntime.HelloRunsAndCallsTheHost", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptHelloTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TArray<FString> Lines;
	TArray<FString> Requests;
	FCrowdyModScriptHost Host;
	Host.OnLog = [&Lines](int32, const FString& Message)
	{
		Lines.Add(Message);
	};
	Host.OnHostCall = [&Requests](TConstArrayView<uint8> Request)
	{
		Requests.Add(TextOf(Request));
		const FTCHARToUTF8 Utf8(ClockAnswer);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	};
	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("hello.wasm"), MoveTemp(Host));
	if (!Instance)
	{
		return false;
	}

	const FCrowdyModScriptResult Init = Instance->Dispatch(ECrowdyModScriptExport::Init, Fuel);
	const FCrowdyModScriptResult Tick = Instance->Dispatch(ECrowdyModScriptExport::Tick, Fuel, 16);
	TestEqual(TEXT("init ends Ok"), Init.Outcome, ECrowdyModScriptOutcome::Ok);
	TestEqual(TEXT("tick ends Ok"), Tick.Outcome, ECrowdyModScriptOutcome::Ok);
	if (!TestEqual(TEXT("init and tick each log once"), Lines.Num(), 2) || !TestEqual(TEXT("one host call"), Requests.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("init logs its greeting"), Lines[0], FString(TEXT("hello init")));
	TestEqual(TEXT("the host receives the guest's request"), Requests[0], FString(ClockRequest));
	TestEqual(TEXT("the guest logs the host's answer"), Lines[1], FString(ClockAnswer));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptEventTest, "CrowdySDK.ModScriptRuntime.EventReachesOnEvent", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptEventTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TArray<FString> Requests;
	FCrowdyModScriptHost Host;
	Host.OnHostCall = [&Requests](TConstArrayView<uint8> Request)
	{
		Requests.Add(TextOf(Request));
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("event.wasm"), MoveTemp(Host));
	if (!Instance)
	{
		return false;
	}
	const FString Event = TEXT("{\"kind\":\"grid_event\",\"eventName\":\"ping\"}");
	const FTCHARToUTF8 Payload(*Event);
	TestEqual(TEXT("init ends Ok"), Instance->Dispatch(ECrowdyModScriptExport::Init, Fuel).Outcome, ECrowdyModScriptOutcome::Ok);
	const FCrowdyModScriptResult Result = Instance->Dispatch(ECrowdyModScriptExport::Event, Fuel, 1,
		TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Payload.Get()), Payload.Length()));
	TestEqual(TEXT("the event dispatch ends Ok"), Result.Outcome, ECrowdyModScriptOutcome::Ok);
	TestTrue(TEXT("on_event reads the payload from guest memory"), Requests.Num() == 1 && Requests[0] == Event);

	TSharedPtr<FCrowdyModScriptInstance> Plain = LoadInstance(*this, TEXT("hello.wasm"));
	TestTrue(TEXT("a module without on_event ends an event Ok, calling nothing"), Plain.IsValid() && Plain->Dispatch(ECrowdyModScriptExport::Init, Fuel).Outcome == ECrowdyModScriptOutcome::Ok
		&& Plain->Dispatch(ECrowdyModScriptExport::Event, Fuel, 1, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Payload.Get()), Payload.Length())).Outcome
			== ECrowdyModScriptOutcome::Ok);

	TSharedPtr<FCrowdyModScriptInstance> Odd = LoadInstance(*this, TEXT("badevent.wasm"));
	TestTrue(TEXT("an on_event that does not take (ptr, len) counts as absent"), Odd.IsValid() && !Odd->HasOnEvent() && Instance->HasOnEvent());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunEventQueueTest, "CrowdySDK.ModScriptRuntime.EventQueueKeepsTicking", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunEventQueueTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	// event.wasm's tick asks for the clock; its on_event sends the payload back as a host call.
	TSharedRef<TArray<FString>> Calls = MakeShared<TArray<FString>>();
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [Calls](TConstArrayView<uint8> Request)
	{
		Calls->Add(TextOf(Request));
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("event.wasm"), MoveTemp(Host), TEXT("queue"));
	if (!Run || !TestTrue(TEXT("init ran and the first tick asked"), Pump(*Run, 3.0, true, [&Calls]() { return Calls->Contains(FString(ClockRequest)); })))
	{
		return false;
	}
	const FTCHARToUTF8 Text(TEXT("{\"kind\":\"grid_event\"}"));
	const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe> Small = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
	int32 Queued = 0;
	for (int32 Index = 0; Index < FCrowdyModScriptRun::MaxQueuedEvents + 1; ++Index)
	{
		Queued += Run->QueueEvent(Small, 1) ? 1 : 0;
	}
	TestEqual(TEXT("at most 64 events wait"), Queued, FCrowdyModScriptRun::MaxQueuedEvents);
	const int32 Before = Calls->Num();
	const auto EventsSeen = [&Calls, Before]()
	{
		int32 Seen = 0;
		for (int32 Index = Before; Index < Calls->Num(); ++Index)
		{
			Seen += (*Calls)[Index] != ClockRequest ? 1 : 0;
		}
		return Seen;
	};
	Pump(*Run, 10.0, true, [&EventsSeen]() { return EventsSeen() >= FCrowdyModScriptRun::MaxQueuedEvents; });
	int32 TicksAmid = 0;
	int32 Events = 0;
	for (int32 Index = Before; Index < Calls->Num(); ++Index)
	{
		const bool bEvent = (*Calls)[Index] != ClockRequest;
		Events += bEvent ? 1 : 0;
		TicksAmid += !bEvent && Events > 0 && Events < FCrowdyModScriptRun::MaxQueuedEvents ? 1 : 0;
	}
	TestEqual(TEXT("every queued event reaches on_event"), Events, FCrowdyModScriptRun::MaxQueuedEvents);
	TestTrue(TEXT("and the ticks keep running among them"), TicksAmid > 0);

	TArray<uint8> Big;
	Big.Init('x', FCrowdyModScriptRun::MaxQueuedEventBytes / 2 + 1);
	const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe> Half = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Big));
	TestTrue(TEXT("a large event is queued"), Run->QueueEvent(Half, 1));
	TestFalse(TEXT("but not past 1 MiB waiting"), Run->QueueEvent(Half, 1));
	Run->Stop();
	TestFalse(TEXT("and none once the run stopped"), Run->QueueEvent(Small, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptFuelTest, "CrowdySDK.ModScriptRuntime.FuelLoopTraps", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptFuelTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("spin.wasm"));
	if (!Instance)
	{
		return false;
	}
	const FCrowdyModScriptResult Result = Instance->Dispatch(ECrowdyModScriptExport::Tick, Fuel, 16);
	TestEqual(TEXT("a metered loop ends on fuel"), Result.Outcome, ECrowdyModScriptOutcome::Fuel);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptWatchdogTest, "CrowdySDK.ModScriptRuntime.UnmeteredLoopHitsTheWatchdog", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptWatchdogTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FCrowdyModScriptResult Result = DispatchTick(*this, TEXT("spinfree.wasm"));
	TestEqual(TEXT("an unmetered loop ends on the watchdog"), Result.Outcome, ECrowdyModScriptOutcome::Watchdog);
	TestTrue(FString::Printf(TEXT("the watchdog fired within %.0f ms (took %.1f ms)"), WatchdogMaxMs, Result.ElapsedMs), Result.ElapsedMs <= WatchdogMaxMs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptGrowthTest, "CrowdySDK.ModScriptRuntime.GrowthStopsAt32MiB", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptGrowthTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("grow.wasm"));
	if (!Instance)
	{
		return false;
	}
	const FCrowdyModScriptResult Result = Instance->Dispatch(ECrowdyModScriptExport::Tick, Fuel, 16);
	TestEqual(TEXT("a refused grow returns to the guest instead of trapping"), Result.Outcome, ECrowdyModScriptOutcome::Ok);
	TestEqual(TEXT("memory grew one page at a time up to exactly 32 MiB"), Instance->GetMemoryBytes(), static_cast<uint64>(32 * 1024 * 1024));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptUnknownImportTest, "CrowdySDK.ModScriptRuntime.RefusesAModuleWithAnUnknownImport", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptUnknownImportTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(*this);
	if (!Runtime)
	{
		return false;
	}

	FString Error;
	TSharedPtr<FCrowdyModScriptModule> Module = FCrowdyModScriptModule::Compile(Runtime.ToSharedRef(), MakeArrayView(UnknownImportModule), Error);
	TestFalse(TEXT("a hand-assembled module importing env.f is refused"), Module.IsValid());
	TestTrue(FString::Printf(TEXT("the refusal names the import (%s)"), *Error), Error.Contains(TEXT("env.f")));

	TArray<uint8> Bytes;
	if (!LoadModuleBytes(*this, TEXT("badimport.wasm"), Bytes))
	{
		return false;
	}
	FString BadImportError;
	TestFalse(TEXT("badimport.wasm is refused"), FCrowdyModScriptModule::Compile(Runtime.ToSharedRef(), Bytes, BadImportError).IsValid());
	TestTrue(FString::Printf(TEXT("the refusal names env.system (%s)"), *BadImportError), BadImportError.Contains(TEXT("env.system")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptBadPointerTest, "CrowdySDK.ModScriptRuntime.BadPointerTrapsNotCrashes", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptBadPointerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("badptr.wasm"));
	if (!Instance)
	{
		return false;
	}
	const FCrowdyModScriptResult First = Instance->Dispatch(ECrowdyModScriptExport::Tick, Fuel, 16);
	TestEqual(TEXT("a log past guest memory traps"), First.Outcome, ECrowdyModScriptOutcome::Trap);
	TestTrue(FString::Printf(TEXT("the trap names ck.log (%s)"), *First.Message), First.Message.Contains(TEXT("ck.log")));
	TestTrue(FString::Printf(TEXT("a trap's message is one short line with no backtrace (%s)"), *First.Message),
		!First.Message.Contains(TEXT("\n")) && !First.Message.Contains(TEXT("backtrace")) && First.Message.Len() <= 200);

	const FCrowdyModScriptResult Second = Instance->Dispatch(ECrowdyModScriptExport::Tick, Fuel, 16);
	TestEqual(TEXT("the instance still answers after the trap"), Second.Outcome, ECrowdyModScriptOutcome::Trap);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptCacheDigestTest, "CrowdySDK.ModScriptRuntime.CacheRefusesADigestMismatch", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptCacheDigestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(*this);
	TArray<uint8> Bytes;
	if (!Runtime || !LoadModuleBytes(*this, TEXT("hello.wasm"), Bytes))
	{
		return false;
	}

	const uint8 Abc[] = {'a', 'b', 'c'};
	const FString AbcDigest = FCrowdyModScriptModuleCache::Sha256Hex(MakeArrayView(Abc));
	TestTrue(FString::Printf(TEXT("SHA-256 of abc is the published vector (%s)"), *AbcDigest),
		AbcDigest.Equals(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), ESearchCase::CaseSensitive));

	FString Error;
	TestFalse(TEXT("bytes under another module's digest are refused"), FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), AbcDigest, Bytes, Error).IsValid());
	TestTrue(FString::Printf(TEXT("the refusal names the SHA-256 (%s)"), *Error), Error.Contains(TEXT("SHA-256")));

	const FString Digest = FCrowdyModScriptModuleCache::Sha256Hex(Bytes);
	FString UpperError;
	TSharedPtr<FCrowdyModScriptModule> First = FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), Digest.ToUpper(), Bytes, UpperError);
	TestTrue(FString::Printf(TEXT("the matching digest compiles in either case (%s)"), *UpperError), First.IsValid());
	FString AgainError;
	TestTrue(TEXT("the second Get is the cached module"), First.IsValid() && FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), Digest, Bytes, AgainError) == First);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptCacheSizeTest, "CrowdySDK.ModScriptRuntime.CacheKeepsSixteen", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptCacheSizeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(*this);
	TArray<uint8> Bytes;
	if (!Runtime || !LoadModuleBytes(*this, TEXT("hello.wasm"), Bytes))
	{
		return false;
	}

	// Each variant appends a distinct custom section, so it has its own digest and still compiles.
	TArray<TArray<uint8>> Variants;
	TArray<TSharedPtr<FCrowdyModScriptModule>> Modules;
	for (int32 Index = 0; Index <= FCrowdyModScriptModuleCache::MaxModules; ++Index)
	{
		const uint8 Section[] = {0x00, 0x03, 0x01, 'k', static_cast<uint8>(Index)};
		TArray<uint8>& Variant = Variants.Add_GetRef(Bytes);
		Variant.Append(Section, UE_ARRAY_COUNT(Section));
		FString Error;
		Modules.Add(FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), FCrowdyModScriptModuleCache::Sha256Hex(Variant), Variant, Error));
		if (!TestTrue(FString::Printf(TEXT("variant %d compiles (%s)"), Index, *Error), Modules.Last().IsValid()))
		{
			return false;
		}
	}

	auto GetAgain = [&Runtime, &Variants](int32 Index)
	{
		FString Error;
		return FCrowdyModScriptModuleCache::Get(Runtime.ToSharedRef(), FCrowdyModScriptModuleCache::Sha256Hex(Variants[Index]), Variants[Index], Error);
	};
	const int32 Newest = FCrowdyModScriptModuleCache::MaxModules;
	TestTrue(TEXT("the newest module is cached"), GetAgain(Newest) == Modules[Newest]);
	TestTrue(TEXT("the second oldest module is cached"), GetAgain(1) == Modules[1]);
	TestTrue(TEXT("the oldest module was dropped and compiles anew"), GetAgain(0) != Modules[0]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunHostCallTest, "CrowdySDK.ModScriptRuntime.HostCallIsAnsweredOnTheGameThread", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunHostCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	FScopedTrace Trace(true);
	FLogCapture Capture(TEXT("HostCallTest: "));
	TArray<FString> Requests;
	bool bOffGameThread = false;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Requests, &bOffGameThread](TConstArrayView<uint8> Request)
	{
		bOffGameThread |= !IsInGameThread();
		Requests.Add(TextOf(Request));
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("hello.wasm"), MoveTemp(Host), TEXT("HostCallTest"));
	if (!Run)
	{
		return false;
	}

	const FString Expected = FString(TEXT("HostCallTest: ")) + ClockAnswer;
	const bool bReached = Pump(*Run, 3.0, true, [&Capture, &Expected]()
	{
		return Capture.GetLines().Contains(Expected);
	});
	TestTrue(TEXT("the guest logged the host's answer"), bReached);
	TestFalse(TEXT("every host call was answered on the game thread"), bOffGameThread);
	TestTrue(TEXT("the host received the guest's request"), Requests.Num() > 0 && Requests[0].Equals(ClockRequest, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunHostTimeoutTest, "CrowdySDK.ModScriptRuntime.HostCallTimesOut", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunHostTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	int32 Calls = 0;
	int32 Stops = 0;
	ECrowdyModScriptRunEnd LastEnd = ECrowdyModScriptRunEnd::CircuitOpen;
	FString LastMessage;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	Host.OnStopped = [&Stops, &LastEnd, &LastMessage](ECrowdyModScriptRunEnd End, const FString& Message)
	{
		++Stops;
		LastEnd = End;
		LastMessage = Message;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("callinit.wasm"), MoveTemp(Host), TEXT("TimeoutTest"));
	if (!Run)
	{
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const bool bStopped = Pump(*Run, FCrowdyModScriptRun::HostCallTimeoutSeconds + 2.0, false, [&Stops]()
	{
		return Stops > 0;
	});
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	TestTrue(TEXT("an unanswered host call in init ends the run"), bStopped);
	TestEqual(TEXT("OnStopped fired once"), Stops, 1);
	TestEqual(TEXT("the run ended as InitFailed"), LastEnd, ECrowdyModScriptRunEnd::InitFailed);
	TestTrue(FString::Printf(TEXT("the reason names HostTimeout (%s)"), *LastMessage), LastMessage.Contains(TEXT("HostTimeout")));
	TestTrue(FString::Printf(TEXT("the call waited the full timeout (%.2f s)"), ElapsedSeconds), ElapsedSeconds >= FCrowdyModScriptRun::HostCallTimeoutSeconds - 0.1);
	TestEqual(TEXT("the host was never asked"), Calls, 0);
	TestFalse(TEXT("the run is no longer running"), Run->IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunStopWaitingTest, "CrowdySDK.ModScriptRuntime.StopWakesAWaitingHostCall", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunStopWaitingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	FCrowdyModScriptRun* Running = nullptr;
	int32 Calls = 0;
	int32 Stops = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Running, &Calls](TConstArrayView<uint8>)
	{
		++Calls;
		Running->Stop();
		return Utf8Bytes(ClockAnswer);
	};
	Host.OnStopped = [&Stops](ECrowdyModScriptRunEnd, const FString&)
	{
		++Stops;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("callinit.wasm"), MoveTemp(Host), TEXT("StopWaitingTest"));
	if (!Run)
	{
		return false;
	}
	Running = Run.Get();

	const bool bAsked = Pump(*Run, 3.0, true, [&Calls]()
	{
		return Calls > 0;
	});
	if (!TestTrue(TEXT("init's host call reached the game thread"), bAsked))
	{
		return false;
	}
	const double StopSeconds = FPlatformTime::Seconds();
	const bool bEnded = Pump(*Run, 1.0, true, [&Run]()
	{
		return !Run->IsDispatching();
	});
	const double EndMs = (FPlatformTime::Seconds() - StopSeconds) * 1000.0;
	TestTrue(FString::Printf(TEXT("the waiting dispatch ended well inside the 5 s timeout (%.1f ms)"), EndMs), bEnded);
	TestFalse(TEXT("the run is stopped"), Run->IsRunning());
	TestEqual(TEXT("Stop advanced the generation"), Run->GetGeneration(), 2u);
	TestEqual(TEXT("the host was asked once"), Calls, 1);
	TestEqual(TEXT("no callback ran after Stop"), Stops, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunDeferredAnswerTest, "CrowdySDK.ModScriptRuntime.DeferredAnswerArrivesLater", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunDeferredAnswerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	FScopedTrace Trace(true);
	FLogCapture Capture(TEXT("DeferTest: "));
	FCrowdyModScriptRun* Running = nullptr;
	TArray<FCrowdyModScriptAnswer> Held;
	int32 Calls = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Running, &Held, &Calls](TConstArrayView<uint8>)
	{
		++Calls;
		if (Held.IsEmpty())
		{
			Held.Add(Running->DeferHostCall());
			return Utf8Bytes(TEXT("not read"));
		}
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("hello.wasm"), MoveTemp(Host), TEXT("DeferTest"));
	if (!Run)
	{
		return false;
	}
	Running = Run.Get();
	const bool bAsked = Pump(*Run, 3.0, true, [&Calls]() { return Calls > 0; });
	if (!TestTrue(TEXT("the first host call reached the game thread"), bAsked))
	{
		return false;
	}
	Pump(*Run, 0.3, true, []() { return false; });
	TestEqual(TEXT("while it waits, no other call is taken"), Calls, 1);
	TestTrue(TEXT("and its handle says it is awaited"), Held[0].IsWaiting());
	const FCrowdyModScriptAnswer Copy = Held[0];
	Held[0].Send(Utf8Bytes(ClockAnswer));
	TestFalse(TEXT("an answer sent is awaited no longer"), Copy.IsWaiting());
	FCrowdyModScriptAnswer(Copy).Send(Utf8Bytes(TEXT("{\"ok\":false}")));

	const FString Expected = FString(TEXT("DeferTest: ")) + ClockAnswer;
	const bool bReached = Pump(*Run, 3.0, true, [&Capture, &Expected]() { return Capture.GetLines().Contains(Expected); });
	TestTrue(TEXT("the guest read the answer given later"), bReached);
	TestFalse(TEXT("and never the second one"), Capture.GetLines().ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("\"ok\":false")); }));
	TestTrue(TEXT("then makes its next calls"), Pump(*Run, 3.0, true, [&Calls]() { return Calls > 1; }));
	TestFalse(TEXT("outside OnHostCall nothing is deferred"), Run->DeferHostCall().IsWaiting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunWaitFreesWorkerTest, "CrowdySDK.ModScriptRuntime.WaitingScriptsLeaveWorkersFree", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunWaitFreesWorkerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	// As many scripts as there are running slots each wait on an answer that has not come; a third must still run.
	FScopedTrace Trace(true);
	FLogCapture Capture(TEXT("WaitFreeC: "));
	TArray<TSharedPtr<FCrowdyModScriptRun>> Waiting;
	Waiting.Reserve(FCrowdyModScriptWorkerPool::MaxWorkers);
	TArray<FCrowdyModScriptAnswer> Held;
	for (int32 Index = 0; Index < FCrowdyModScriptWorkerPool::MaxWorkers; ++Index)
	{
		TSharedPtr<FCrowdyModScriptRun>* Self = &Waiting.AddDefaulted_GetRef();
		FCrowdyModScriptRunHost Host;
		Host.OnHostCall = [Self, &Held](TConstArrayView<uint8>)
		{
			Held.Add((*Self)->DeferHostCall());
			return Utf8Bytes(TEXT("not read"));
		};
		*Self = StartRun(*this, TEXT("hello.wasm"), MoveTemp(Host), *FString::Printf(TEXT("WaitFree%d"), Index));
		if (!*Self || !TestTrue(TEXT("each waiting script reached its host call"), Pump(**Self, 3.0, true, [&Held, Index]() { return Held.Num() > Index; })))
		{
			return false;
		}
	}
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [](TConstArrayView<uint8>) { return Utf8Bytes(ClockAnswer); };
	TSharedPtr<FCrowdyModScriptRun> Free = StartRun(*this, TEXT("hello.wasm"), MoveTemp(Host), TEXT("WaitFreeC"));
	if (!Free)
	{
		return false;
	}
	const FString Expected = FString(TEXT("WaitFreeC: ")) + ClockAnswer;
	TestTrue(TEXT("a third script runs while they wait"), Pump(*Free, 3.0, true, [&Capture, &Expected]() { return Capture.GetLines().Contains(Expected); }));
	for (FCrowdyModScriptAnswer& Answer : Held)
	{
		Answer.Send(Utf8Bytes(ClockAnswer));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunStopRunningTest, "CrowdySDK.ModScriptRuntime.StopEndsARunningGuest", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunStopRunningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	// Stop ends the guest within one 50 ms tick; the refilled watchdog budget would take at least 200 ms.
	constexpr double StopWithinMs = 180.0;
	int32 Calls = 0;
	int32 Stops = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	Host.OnStopped = [&Stops](ECrowdyModScriptRunEnd, const FString&)
	{
		++Stops;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("hostspin.wasm"), MoveTemp(Host), TEXT("StopRunningTest"));
	if (!Run)
	{
		return false;
	}

	// The answer sends the guest into its unmetered loop. Its safety is the watchdog, which ends it whatever Stop does.
	const bool bAnswered = Pump(*Run, 3.0, true, [&Calls]()
	{
		return Calls > 0;
	});
	if (!TestTrue(TEXT("tick's host call was answered"), bAnswered))
	{
		return false;
	}
	const double StopSeconds = FPlatformTime::Seconds();
	Run->Stop();
	const double StopCallMs = (FPlatformTime::Seconds() - StopSeconds) * 1000.0;
	Pump(*Run, 1.0, false, [&Run]()
	{
		return !Run->IsDispatching();
	});
	const double EndMs = (FPlatformTime::Seconds() - StopSeconds) * 1000.0;
	TestTrue(FString::Printf(TEXT("Stop returned without waiting on the guest (%.2f ms)"), StopCallMs), StopCallMs < 100.0);
	TestTrue(FString::Printf(TEXT("the guest ended within %.0f ms of Stop (took %.1f ms)"), StopWithinMs, EndMs), EndMs < StopWithinMs);
	TestEqual(TEXT("no callback ran after Stop"), Stops, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunCircuitTest, "CrowdySDK.ModScriptRuntime.CircuitOpensAfterFiveFailures", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunCircuitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	int32 Calls = 0;
	int32 Stops = 0;
	ECrowdyModScriptRunEnd LastEnd = ECrowdyModScriptRunEnd::InitFailed;
	FString LastMessage;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	Host.OnStopped = [&Stops, &LastEnd, &LastMessage](ECrowdyModScriptRunEnd End, const FString& Message)
	{
		++Stops;
		LastEnd = End;
		LastMessage = Message;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("trapcall.wasm"), MoveTemp(Host), TEXT("CircuitTest"));
	if (!Run)
	{
		return false;
	}

	const bool bOpened = Pump(*Run, 3.0, true, [&Stops]()
	{
		return Stops > 0;
	});
	Pump(*Run, 0.2, true, []()
	{
		return false;
	});
	TestTrue(TEXT("traps in a row open the circuit"), bOpened);
	TestEqual(TEXT("exactly five ticks ran"), Calls, FCrowdyModScriptRun::MaxConsecutiveFailures);
	TestEqual(TEXT("OnStopped fired once"), Stops, 1);
	TestEqual(TEXT("the run ended as CircuitOpen"), LastEnd, ECrowdyModScriptRunEnd::CircuitOpen);
	TestTrue(FString::Printf(TEXT("the reason names the last failure (%s)"), *LastMessage), LastMessage.Contains(TEXT("Trap")));
	TestFalse(TEXT("the run is no longer running"), Run->IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunCircuitResetTest, "CrowdySDK.ModScriptRuntime.SuccessResetsTheCircuit", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunCircuitResetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	int32 Calls = 0;
	int32 Stops = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	Host.OnStopped = [&Stops](ECrowdyModScriptRunEnd, const FString&)
	{
		++Stops;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("flaky.wasm"), MoveTemp(Host), TEXT("CircuitResetTest"));
	if (!Run)
	{
		return false;
	}

	// Ticks 1 to 4 fail and 5 succeeds. Without the reset, tick 6 would be the fifth failure and tick 7 would never run.
	const bool bSeventh = Pump(*Run, 3.0, true, [&Calls]()
	{
		return Calls >= 7;
	});
	TestTrue(FString::Printf(TEXT("a seventh tick ran (%d ticks)"), Calls), bSeventh);
	TestTrue(TEXT("the run is still running"), Run->IsRunning());
	TestEqual(TEXT("OnStopped never fired"), Stops, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunLogLimitTest, "CrowdySDK.ModScriptRuntime.LogLinesAreRateLimited", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunLogLimitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FString Prefix = TEXT("Chatty: ");
	const FString ShortLine = Prefix + TEXT("line");
	const FString Notice = Prefix + TEXT("10 log lines dropped");
	FScopedTrace Trace(true);
	FLogCapture Capture(Prefix);
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("chatty.wasm"), FCrowdyModScriptRunHost(), TEXT("Chatty"));
	if (!Run)
	{
		return false;
	}

	const bool bNoticed = Pump(*Run, 3.0, true, [&Capture, &Notice]()
	{
		return Capture.GetLines().Contains(Notice);
	});
	const TArray<FString> Lines = Capture.GetLines();
	const int32 LongLines = Lines.FilterByPredicate([&Prefix](const FString& Line)
	{
		return Line.Len() == Prefix.Len() + FCrowdyModScriptRun::MaxLogLineChars && Line.EndsWith(TEXT("aaaa"));
	}).Num();
	TestTrue(TEXT("a notice counts the 10 lines over the limit"), bNoticed);
	TestEqual(TEXT("the 1,500-byte line was cut to 1,000 characters"), LongLines, 1);
	TestEqual(TEXT("19 short lines fit beside it in the second"), Lines.FilterByPredicate([&ShortLine](const FString& Line) { return Line == ShortLine; }).Num(), 19);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunSkipTest, "CrowdySDK.ModScriptRuntime.TicksAreSkippedNotQueued", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunSkipTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	int32 Calls = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("hostspin.wasm"), MoveTemp(Host), TEXT("SkipTest"));
	if (!Run)
	{
		return false;
	}

	// Each tick spins about 250 ms, while a 16 ms interval comes due every few frames.
	Pump(*Run, 0.8, true, []()
	{
		return false;
	});
	// About 50 ticks came due in 0.8 s; queuing them would run far more than one per 250 ms spin.
	TestTrue(FString::Printf(TEXT("ticks came due and were skipped (%lld)"), Run->GetSkippedTicks()), Run->GetSkippedTicks() >= 5);
	TestTrue(FString::Printf(TEXT("only one tick ran per spin (%d)"), Calls), Calls >= 1 && Calls <= 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptFuelRefillTest, "CrowdySDK.ModScriptRuntime.FuelIsRefilledEveryDispatch", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptFuelRefillTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	// busy spends 2,000,000 fuel a tick, so a second tick on what the first left would trap.
	constexpr int64 TickFuel = 3000000;
	TSharedPtr<FCrowdyModScriptInstance> Instance = LoadInstance(*this, TEXT("busy.wasm"));
	if (!Instance)
	{
		return false;
	}
	FSafetyStop Safety(Instance.ToSharedRef());
	TestEqual(TEXT("the first tick ends Ok"), Instance->Dispatch(ECrowdyModScriptExport::Tick, TickFuel, 16).Outcome, ECrowdyModScriptOutcome::Ok);
	TestEqual(TEXT("the second tick ends Ok on refilled fuel"), Instance->Dispatch(ECrowdyModScriptExport::Tick, TickFuel, 16).Outcome, ECrowdyModScriptOutcome::Ok);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptWatchdogRenewTest, "CrowdySDK.ModScriptRuntime.EachHostCallRenewsTheWatchdog", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptWatchdogRenewTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	FCrowdyModScriptHost Host;
	Host.OnHostCall = [](TConstArrayView<uint8>)
	{
		return Utf8Bytes(ClockAnswer);
	};
	const FCrowdyModScriptResult Result = DispatchTick(*this, TEXT("worksplit.wasm"), MoveTemp(Host), UnlimitedFuel);
	TestEqual(FString::Printf(TEXT("150 ms of work on each side of a host call ends Ok (%s)"), *Result.Message), Result.Outcome, ECrowdyModScriptOutcome::Ok);
	TestTrue(FString::Printf(TEXT("the dispatch did more than 250 ms of work in all (%.1f ms)"), Result.ElapsedMs), Result.ElapsedMs >= 290.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptAllocRecursionTest, "CrowdySDK.ModScriptRuntime.HostCallInsideAllocTraps", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptAllocRecursionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FCrowdyModScriptResult Result = DispatchTick(*this, TEXT("allocrecurse.wasm"));
	TestEqual(TEXT("a host call from ck_alloc traps instead of recursing"), Result.Outcome, ECrowdyModScriptOutcome::Trap);
	TestTrue(FString::Printf(TEXT("the trap names the cause (%s)"), *Result.Message), Result.Message.Contains(TEXT("inside ck_alloc")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptAllocFailureTest, "CrowdySDK.ModScriptRuntime.AllocFailuresTrap", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptAllocFailureTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FCrowdyModScriptResult BadPointer = DispatchTick(*this, TEXT("allocbadptr.wasm"));
	TestEqual(TEXT("ck_alloc returning a block past memory traps"), BadPointer.Outcome, ECrowdyModScriptOutcome::Trap);
	TestTrue(FString::Printf(TEXT("the trap names the bad block (%s)"), *BadPointer.Message), BadPointer.Message.Contains(TEXT("outside guest memory")));

	const FCrowdyModScriptResult Trapped = DispatchTick(*this, TEXT("alloctrap.wasm"));
	TestEqual(TEXT("a trap inside ck_alloc ends the dispatch as Trap"), Trapped.Outcome, ECrowdyModScriptOutcome::Trap);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptTooLargeTest, "CrowdySDK.ModScriptRuntime.OversizedRequestIsRefused", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptTooLargeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TArray<FString> Lines;
	int32 Calls = 0;
	FCrowdyModScriptHost Host;
	Host.OnLog = [&Lines](int32, const FString& Message)
	{
		Lines.Add(Message);
	};
	Host.OnHostCall = [&Calls](TConstArrayView<uint8>)
	{
		++Calls;
		return Utf8Bytes(ClockAnswer);
	};
	const FCrowdyModScriptResult Result = DispatchTick(*this, TEXT("toolarge.wasm"), MoveTemp(Host));
	TestEqual(TEXT("the guest keeps running after the refusal"), Result.Outcome, ECrowdyModScriptOutcome::Ok);
	TestEqual(TEXT("the host never sees the request"), Calls, 0);
	TestTrue(TEXT("the guest is answered request_too_large"), Lines.Num() == 1 && Lines[0].Contains(TEXT("\"kind\":\"request_too_large\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptStateLimitsTest, "CrowdySDK.ModScriptRuntime.StateBlobLimitsHold", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptStateLimitsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FCrowdyModScriptResult Result = DispatchTick(*this, TEXT("state.wasm"));
	TestEqual(FString::Printf(TEXT("every state_get and state_set check in the guest held (%s)"), *Result.Message), Result.Outcome, ECrowdyModScriptOutcome::Ok);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRefusalsTest, "CrowdySDK.ModScriptRuntime.CompileRefusesBadModules", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FString StartError = CompileError(*this, TEXT("startfn.wasm"));
	TestTrue(FString::Printf(TEXT("a start function is refused (%s)"), *StartError), StartError.Contains(TEXT("start function")));
	const FString TickError = CompileError(*this, TEXT("notick.wasm"));
	TestTrue(FString::Printf(TEXT("a missing tick is refused (%s)"), *TickError), TickError.Contains(TEXT("export tick")));
	const FString FuelError = CompileError(*this, TEXT("badfuel.wasm"));
	TestTrue(FString::Printf(TEXT("an i32 ck_fuel is refused (%s)"), *FuelError), FuelError.Contains(TEXT("export ck_fuel")));

	TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(*this);
	TArray<uint8> Bytes;
	if (!Runtime || !LoadModuleBytes(*this, TEXT("hello.wasm"), Bytes))
	{
		return false;
	}
	Bytes.AddZeroed(FCrowdyModScriptModule::MaxModuleBytes);
	FString SizeError;
	TestFalse(TEXT("a module over 512 KiB is refused"), FCrowdyModScriptModule::Compile(Runtime.ToSharedRef(), Bytes, SizeError).IsValid());
	TestTrue(FString::Printf(TEXT("the refusal names the size (%s)"), *SizeError), SizeError.Contains(TEXT("512 KiB")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunRecycleTest, "CrowdySDK.ModScriptRuntime.WatchdogRecyclesTheInstance", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunRecycleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	int32 Inits = 0;
	FCrowdyModScriptRunHost Host;
	Host.OnHostCall = [&Inits](TConstArrayView<uint8>)
	{
		++Inits;
		return Utf8Bytes(ClockAnswer);
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("recycle.wasm"), MoveTemp(Host), TEXT("RecycleTest"));
	if (!Run)
	{
		return false;
	}

	// Only init makes a host call, and every tick ends on the watchdog, so a second call means a fresh instance ran init.
	const bool bRecycled = Pump(*Run, 3.0, true, [&Inits]()
	{
		return Inits >= 2;
	});
	TestTrue(FString::Printf(TEXT("init ran again after the watchdog (%d inits)"), Inits), bRecycled);
	TestTrue(TEXT("the run is still running"), Run->IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunLogControlTest, "CrowdySDK.ModScriptRuntime.LogLinesStayOneLine", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunLogControlTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FString Expected = TEXT("Ctl Name: one two three four");
	FScopedTrace Trace(true);
	FLogCapture Capture(TEXT("Ctl Name: "));
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("logctl.wasm"), FCrowdyModScriptRunHost(), TEXT("Ctl\nName"));
	if (!Run)
	{
		return false;
	}

	const bool bLogged = Pump(*Run, 3.0, true, [&Capture]()
	{
		return Capture.GetLines().Num() > 0;
	});
	const TArray<FString> Lines = Capture.GetLines();
	TestTrue(TEXT("the guest's line was logged under the cleaned name"), bLogged);
	TestTrue(FString::Printf(TEXT("control characters became spaces (%s)"), Lines.IsEmpty() ? TEXT("") : *Lines[0]),
		Lines.Num() > 0 && Lines[0].Equals(Expected, ESearchCase::CaseSensitive));
	return true;
}

// A script's text is its author's, so it stays out of every player's log file unless they turn tracing on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptRunLogTraceTest, "CrowdySDK.ModScriptRuntime.LogLinesNeedTracing", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptRunLogTraceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FString Prefix = TEXT("Quiet: ");
	FScopedTrace Trace(false);
	FLogCapture Capture(Prefix);
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("chatty.wasm"), FCrowdyModScriptRunHost(), TEXT("Quiet"));
	if (!Run)
	{
		return false;
	}

	// Long enough for the chatty guest's 30 lines and the dropped-lines notice after its first second.
	Pump(*Run, 1.5, true, []()
	{
		return false;
	});
	TestEqual(TEXT("with tracing off nothing the script logs, nor the dropped-lines notice, reaches the log"), Capture.GetLines().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptLogRingWrapTest, "CrowdySDK.ModScriptRuntime.LogRingKeepsTheLast200", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptLogRingWrapTest::RunTest(const FString& Parameters)
{
	FCrowdyModScriptLogRing Ring;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Ring.Add(Index, FString::Printf(TEXT("early %d"), Index));
	}
	TArray<FCrowdyModScriptLogEntry> Lines;
	int32 Evicted = -1;
	int32 RateDropped = -1;
	Ring.Read(Lines, Evicted, RateDropped);
	TestEqual(TEXT("a ring under capacity holds what it was given"), Lines.Num(), 3);
	TestTrue(TEXT("oldest first, with each level"), Lines.Num() == 3 && Lines[0].Text == TEXT("early 0") && Lines[2].Text == TEXT("early 2") && Lines[2].Level == 2);
	TestTrue(TEXT("nothing wrapped or was dropped"), Evicted == 0 && RateDropped == 0);

	FCrowdyModScriptLogRing Full;
	for (int32 Index = 0; Index < 250; ++Index)
	{
		Full.Add(Index % 4, FString::Printf(TEXT("line %d"), Index));
	}
	Full.CountRateDropped();
	Full.CountRateDropped();
	Full.Read(Lines, Evicted, RateDropped);
	TestEqual(TEXT("the ring keeps 200 lines"), Lines.Num(), FCrowdyModScriptLogRing::Capacity);
	TestEqual(TEXT("the 50 oldest fell out"), Evicted, 50);
	TestEqual(TEXT("and the rate-limited lines are counted apart"), RateDropped, 2);
	if (TestEqual(TEXT("the ring held 200 entries"), Lines.Num(), 200))
	{
		TestEqual(TEXT("it starts at the oldest line kept"), Lines[0].Text, FString(TEXT("line 50")));
		TestEqual(TEXT("it ends at the newest"), Lines[199].Text, FString(TEXT("line 249")));
		TestEqual(TEXT("each line keeps its own level"), Lines[1].Level, 51 % 4);
		TestTrue(TEXT("and a time"), Lines[0].AtMs > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptLogRingFeedTest, "CrowdySDK.ModScriptRuntime.LogLinesReachTheRingWithTracingOff", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptLogRingFeedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const FString Prefix = TEXT("RingFeed: ");
	FScopedTrace Trace(false);
	FLogCapture Capture(Prefix);
	const TSharedRef<FCrowdyModScriptLogRing, ESPMode::ThreadSafe> Ring = MakeShared<FCrowdyModScriptLogRing, ESPMode::ThreadSafe>();
	FCrowdyModScriptRunHost Host;
	Host.Log = Ring;
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("chatty.wasm"), MoveTemp(Host), TEXT("RingFeed"));
	if (!Run)
	{
		return false;
	}

	TArray<FCrowdyModScriptLogEntry> Lines;
	int32 Evicted = 0;
	int32 RateDropped = 0;
	const bool bLogged = Pump(*Run, 3.0, true, [&Ring, &Lines, &Evicted, &RateDropped]()
	{
		Ring->Read(Lines, Evicted, RateDropped);
		return RateDropped >= 10;
	});
	TestTrue(TEXT("the guest's 30 lines reached the ring"), bLogged);
	TestEqual(TEXT("20 lines fit the rate limit"), Lines.Num(), FCrowdyModScriptRun::MaxLogLinesPerSecond);
	TestEqual(TEXT("the 10 over it are counted as dropped"), RateDropped, 10);
	TestEqual(TEXT("and nothing wrapped"), Evicted, 0);
	if (Lines.Num() > 0)
	{
		TestEqual(TEXT("the 1,500-byte line was cut to 1,000 characters"), Lines[0].Text.Len(), FCrowdyModScriptRun::MaxLogLineChars);
	}
	TestTrue(TEXT("every line carries the level the guest logged"), Algo::AllOf(Lines, [](const FCrowdyModScriptLogEntry& Entry) { return Entry.Level == 1; }));
	TestTrue(TEXT("and a time"), Algo::AllOf(Lines, [](const FCrowdyModScriptLogEntry& Entry) { return Entry.AtMs > 0; }));
	TestEqual(TEXT("with tracing off none of it reached the log"), Capture.GetLines().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptLogRingRecycleTest, "CrowdySDK.ModScriptRuntime.LogRingSurvivesAFreshInstance", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptLogRingRecycleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const TSharedRef<FCrowdyModScriptLogRing, ESPMode::ThreadSafe> Ring = MakeShared<FCrowdyModScriptLogRing, ESPMode::ThreadSafe>();
	FCrowdyModScriptRunHost Host;
	Host.Log = Ring;
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("recyclelog.wasm"), MoveTemp(Host), TEXT("RingRecycle"));
	if (!Run)
	{
		return false;
	}

	// Each init logs one line and every tick ends on the watchdog, so a second line means a fresh instance ran init.
	TArray<FCrowdyModScriptLogEntry> Lines;
	int32 Evicted = 0;
	int32 RateDropped = 0;
	const bool bRecycled = Pump(*Run, 3.0, true, [&Ring, &Lines, &Evicted, &RateDropped]()
	{
		Ring->Read(Lines, Evicted, RateDropped);
		return Lines.Num() >= 2;
	});
	TestTrue(TEXT("the line from the first instance is still there beside the fresh instance's"), bRecycled);
	TestTrue(TEXT("both at the level the guest logged"), Algo::AllOf(Lines, [](const FCrowdyModScriptLogEntry& Entry) { return Entry.Text == TEXT("boot") && Entry.Level == 2; }));
	TestTrue(TEXT("the run kept running"), Run->IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptLogRingStopTest, "CrowdySDK.ModScriptRuntime.LogLinesAfterStopStayOutOfTheRing", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptLogRingStopTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	const TSharedRef<FCrowdyModScriptLogRing, ESPMode::ThreadSafe> Ring = MakeShared<FCrowdyModScriptLogRing, ESPMode::ThreadSafe>();
	FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
	ON_SCOPE_EXIT
	{
		Release->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(Release);
	};
	FCrowdyModScriptRunHost Host;
	Host.Log = Ring;
	Host.OnHostCall = [](TConstArrayView<uint8>) { return TArray<uint8>(); };
	// Holds the guest on the worker until the run is stopped, then lets it carry on to its log call.
	Host.OnHostCallWorker = [Entered, Release](TArray<uint8>&)
	{
		Entered->Trigger();
		Release->Wait(3000);
		return false;
	};
	TSharedPtr<FCrowdyModScriptRun> Run = StartRun(*this, TEXT("logafter.wasm"), MoveTemp(Host), TEXT("RingStop"));
	if (!Run)
	{
		return false;
	}
	if (!TestTrue(TEXT("the guest reached its host call"), Entered->Wait(3000)))
	{
		return false;
	}
	Run->Stop();
	Release->Trigger();
	const bool bEnded = Pump(*Run, 3.0, false, [&Run]() { return !Run->IsDispatching(); });
	TestTrue(TEXT("the dispatch ended"), bEnded);

	TArray<FCrowdyModScriptLogEntry> Lines;
	int32 Evicted = 0;
	int32 RateDropped = 0;
	Ring->Read(Lines, Evicted, RateDropped);
	TestEqual(TEXT("a line logged after Stop never reaches the ring"), Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptCacheAsyncTest,"CrowdySDK.ModScriptRuntime.GetAsyncAnswersOnTheGameThread", CrowdyModScriptRuntimeTests::TestFlags)
bool FCrowdyModScriptCacheAsyncTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyModScriptRuntimeTests;

	TSharedPtr<FCrowdyModScriptRuntime> Runtime = AvailableRuntime(*this);
	TArray<uint8> Bytes;
	if (!Runtime || !LoadModuleBytes(*this, TEXT("hello.wasm"), Bytes))
	{
		return false;
	}

	// Shared so a late answer after a timed-out test writes to live memory.
	struct FAnswer
	{
		TSharedPtr<FCrowdyModScriptModule> Module;
		FString Error;
		bool bDone = false;
		bool bOnGameThread = false;
	};
	TSharedRef<FAnswer> Answer = MakeShared<FAnswer>();
	const FString Digest = FCrowdyModScriptModuleCache::Sha256Hex(Bytes);
	FCrowdyModScriptModuleCache::GetAsync(Runtime.ToSharedRef(), Digest, Bytes, [Answer](TSharedPtr<FCrowdyModScriptModule> Module, const FString& Error)
	{
		Answer->Module = Module;
		Answer->Error = Error;
		Answer->bOnGameThread = IsInGameThread();
		Answer->bDone = true;
	});
	TestFalse(TEXT("GetAsync never answers before it returns"), Answer->bDone);

	const double DeadlineSeconds = FPlatformTime::Seconds() + 3.0;
	while (!Answer->bDone && FPlatformTime::Seconds() < DeadlineSeconds)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("GetAsync answered"), Answer->bDone);
	TestTrue(FString::Printf(TEXT("the module compiled (%s)"), *Answer->Error), Answer->Module.IsValid());
	TestTrue(TEXT("the answer arrived on the game thread"), Answer->bOnGameThread);
	return true;
}

#endif
