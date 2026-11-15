#include "CrowdyModScriptRuntime.h"

#include "Algo/Find.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
THIRD_PARTY_INCLUDES_START
#include <wasmtime.h>
THIRD_PARTY_INCLUDES_END

#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
// OpenSSL declares a type named UI, which collides with Unreal's UI namespace.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY(LogCrowdyModScripts);

namespace
{
	TAutoConsoleVariable<int32> CVarCrowdyModScriptsTrace(TEXT("crowdy.modscripts.trace"), 0,
		TEXT("When non-zero, logs Mod Script activity: engine startup, compiles, instances, dispatch outcomes and guest log lines. ")
		TEXT("Off by default."), ECVF_Default);

	// GSharedPool is guarded by GRuntimeLock too, so it never starts after shutdown.
	FCriticalSection GRuntimeLock;
	TSharedPtr<FCrowdyModScriptRuntime> GRuntime;
	TSharedPtr<FCrowdyModScriptWorkerPool> GSharedPool;

	struct FModuleCacheEntry
	{
		FString Digest;
		const FCrowdyModScriptRuntime* Runtime = nullptr;
		TSharedPtr<FCrowdyModScriptModule> Module;
	};

	// Least recently used first.
	FCriticalSection GModuleCacheLock;
	TArray<FModuleCacheEntry> GModuleCache;

	// Call under GModuleCacheLock. A hit moves to the back.
	TSharedPtr<FCrowdyModScriptModule> FindCached(const FString& Digest, const FCrowdyModScriptRuntime* Runtime)
	{
		const int32 Index = GModuleCache.IndexOfByPredicate([&Digest, Runtime](const FModuleCacheEntry& Entry)
		{
			return Entry.Runtime == Runtime && Entry.Digest.Equals(Digest, ESearchCase::CaseSensitive);
		});
		if (Index == INDEX_NONE)
		{
			return nullptr;
		}
		FModuleCacheEntry Hit = MoveTemp(GModuleCache[Index]);
		GModuleCache.RemoveAt(Index);
		TSharedPtr<FCrowdyModScriptModule> Module = Hit.Module;
		GModuleCache.Add(MoveTemp(Hit));
		return Module;
	}
}

bool CrowdyModScriptTrace::Enabled()
{
	return CVarCrowdyModScriptsTrace.GetValueOnAnyThread() != 0;
}

FString CrowdyModScriptTrace::Printable(FString Text)
{
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 Char = static_cast<uint32>(Text[Index]);
		if (Char < 0x20 || (Char >= 0x7F && Char <= 0x9F) || Char == 0x2028 || Char == 0x2029)
		{
			Text[Index] = TEXT(' ');
		}
	}
	return Text;
}

const TCHAR* LexToString(ECrowdyModScriptOutcome Outcome)
{
	switch (Outcome)
	{
	case ECrowdyModScriptOutcome::Ok: return TEXT("Ok");
	case ECrowdyModScriptOutcome::Fuel: return TEXT("Fuel");
	case ECrowdyModScriptOutcome::Watchdog: return TEXT("Watchdog");
	case ECrowdyModScriptOutcome::Trap: return TEXT("Trap");
	case ECrowdyModScriptOutcome::HostTimeout: return TEXT("HostTimeout");
	}
	return TEXT("Unknown");
}

TSharedPtr<FCrowdyModScriptRuntime> FCrowdyModScriptRuntime::Get()
{
	FScopeLock Lock(&GRuntimeLock);
	return GRuntime;
}

TSharedPtr<FCrowdyModScriptWorkerPool> FCrowdyModScriptWorkerPool::GetShared()
{
	FScopeLock Lock(&GRuntimeLock);
	if (!GRuntime)
	{
		return nullptr;
	}
	if (!GSharedPool)
	{
		GSharedPool = MakeShared<FCrowdyModScriptWorkerPool>();
	}
	return GSharedPool;
}

TSharedPtr<FCrowdyModScriptModule> FCrowdyModScriptModuleCache::Get(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, const FString& DigestHex, TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	const FString Digest = DigestHex.ToLower();
	const FString Actual = Sha256Hex(Bytes);
	if (!Actual.Equals(Digest, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("the module's SHA-256 %s does not match its digest %s"), *Actual, *Digest);
		return nullptr;
	}
	{
		FScopeLock Lock(&GModuleCacheLock);
		if (TSharedPtr<FCrowdyModScriptModule> Hit = FindCached(Digest, &Runtime.Get()))
		{
			return Hit;
		}
	}

	TSharedPtr<FCrowdyModScriptModule> Compiled = FCrowdyModScriptModule::Compile(Runtime, Bytes, OutError);
	if (!Compiled)
	{
		return nullptr;
	}
	FScopeLock Lock(&GModuleCacheLock);
	if (TSharedPtr<FCrowdyModScriptModule> Raced = FindCached(Digest, &Runtime.Get()))
	{
		return Raced;
	}
	GModuleCache.Add({Digest, &Runtime.Get(), Compiled});
	if (GModuleCache.Num() > MaxModules)
	{
		GModuleCache.RemoveAt(0);
	}
	return Compiled;
}

void FCrowdyModScriptModuleCache::GetAsync(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, const FString& DigestHex, TArray<uint8> Bytes,
	TFunction<void(TSharedPtr<FCrowdyModScriptModule> Module, const FString& Error)> OnDone)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Runtime, DigestHex, Owned = MoveTemp(Bytes), Callback = MoveTemp(OnDone)]() mutable
	{
		FString Error;
		TSharedPtr<FCrowdyModScriptModule> Module = FCrowdyModScriptModuleCache::Get(Runtime, DigestHex, Owned, Error);
		AsyncTask(ENamedThreads::GameThread, [Found = MoveTemp(Module), Reason = MoveTemp(Error), Done = MoveTemp(Callback)]()
		{
			Done(Found, Reason);
		});
	});
}

FString FCrowdyModScriptModuleCache::Sha256Hex(TConstArrayView<uint8> Bytes)
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	uint8 Hash[EVP_MAX_MD_SIZE];
	unsigned int HashLength = 0;
	if (!EVP_Digest(Bytes.GetData(), Bytes.Num(), Hash, &HashLength, EVP_sha256(), nullptr) || HashLength != 32)
	{
		return FString();
	}
	return BytesToHexLower(Hash, 32);
#else
	return FString();
#endif
}

int64 FCrowdyModScriptRuntime::UnixNowMs()
{
	return (FDateTime::UtcNow().GetTicks() - FDateTime(1970, 1, 1).GetTicks()) / ETimespan::TicksPerMillisecond;
}

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME

namespace
{
	constexpr double EpochTickSeconds = 0.05;
	constexpr int32 DeadlineTicks = 5;
	constexpr uint32 MaxLogBytes = 4096;
	constexpr uint32 MaxStateBytes = 1024 * 1024;
	constexpr uint32 MaxHostRequestBytes = 256 * 1024 + 1024;
	constexpr int64 MaxTableElements = 10000;
	constexpr int32 WasiErrnoIo = 29;

	const char* const RequestTooLargeAnswer = "{\"ok\":false,\"error\":{\"kind\":\"request_too_large\",\"message\":\"host call request exceeds the sandbox limit\"}}";
	const char* const NoHandlerAnswer = "{\"ok\":false,\"error\":{\"kind\":\"no_handler\",\"message\":\"the host has no handler for host calls\"}}";

	struct FFuncShape
	{
		uint8 NumParams;
		uint8 NumResults;
		wasm_valkind_t Params[3];
		wasm_valkind_t Result;
	};

	constexpr FFuncShape AllocShape = {1, 1, {WASM_I32}, WASM_I32};
	constexpr FFuncShape InitShape = {0, 0, {}, WASM_I32};
	constexpr FFuncShape TickShape = {1, 0, {WASM_I32}, WASM_I32};

	// on_event(ptr, len) and ck_free(ptr, len).
	constexpr FFuncShape PtrLenShape = {2, 0, {WASM_I32, WASM_I32}, WASM_I32};

	FString Utf8ToString(const char* Data, size_t Size)
	{
		while (Size > 0 && Data[Size - 1] == '\0')
		{
			--Size;
		}
		if (Size == 0)
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(Data, static_cast<int32>(FMath::Min<size_t>(Size, MAX_int32)));
		return FString(Converted.Length(), Converted.Get());
	}

	FString NameToString(const wasm_name_t* Name)
	{
		return Name ? Utf8ToString(Name->data, Name->size) : FString();
	}

	bool NameIs(const wasm_name_t* Name, const char* Expected)
	{
		const size_t Length = FCStringAnsi::Strlen(Expected);
		return Name && Name->size == Length && FMemory::Memcmp(Name->data, Expected, Length) == 0;
	}

	FString TakeErrorMessage(wasmtime_error_t* Error)
	{
		wasm_name_t Message;
		wasmtime_error_message(Error, &Message);
		FString Result = Utf8ToString(Message.data, Message.size);
		wasm_byte_vec_delete(&Message);
		wasmtime_error_delete(Error);
		return Result;
	}

	FString TrapMessage(const wasm_trap_t* Trap)
	{
		wasm_message_t Message;
		wasm_trap_message(Trap, &Message);
		FString Result = Utf8ToString(Message.data, Message.size);
		wasm_byte_vec_delete(&Message);
		return Result;
	}

	wasm_trap_t* GuestTrap(const char* Message)
	{
		return wasmtime_trap_new(Message, FCStringAnsi::Strlen(Message));
	}

	TArray<uint8> AnsiBytes(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	bool ShapeMatches(const wasm_functype_t* Type, const FFuncShape& Shape)
	{
		const wasm_valtype_vec_t* Params = wasm_functype_params(Type);
		const wasm_valtype_vec_t* Results = wasm_functype_results(Type);
		if (Params->size != Shape.NumParams || Results->size != Shape.NumResults)
		{
			return false;
		}
		for (size_t Index = 0; Index < Params->size; ++Index)
		{
			if (wasm_valtype_kind(Params->data[Index]) != Shape.Params[Index])
			{
				return false;
			}
		}
		return Shape.NumResults == 0 || wasm_valtype_kind(Results->data[0]) == Shape.Result;
	}

	wasm_functype_t* NewFuncType(const FFuncShape& Shape)
	{
		wasm_valtype_t* ParamTypes[3] = {};
		for (uint8 Index = 0; Index < Shape.NumParams; ++Index)
		{
			ParamTypes[Index] = wasm_valtype_new(Shape.Params[Index]);
		}
		wasm_valtype_t* ResultType = Shape.NumResults ? wasm_valtype_new(Shape.Result) : nullptr;

		wasm_valtype_vec_t Params;
		wasm_valtype_vec_t Results;
		if (Shape.NumParams)
		{
			wasm_valtype_vec_new(&Params, Shape.NumParams, ParamTypes);
		}
		else
		{
			wasm_valtype_vec_new_empty(&Params);
		}
		if (ResultType)
		{
			wasm_valtype_vec_new(&Results, 1, &ResultType);
		}
		else
		{
			wasm_valtype_vec_new_empty(&Results);
		}
		return wasm_functype_new(&Params, &Results);
	}
}

struct FCrowdyModScriptEngineState final : public FRunnable
{
	wasm_engine_t* Engine = nullptr;
	wasmtime_linker_t* Linker = nullptr;
	FString UnavailableReason;

	FEvent* WakeEvent = nullptr;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopping{false};

	// Guarded by TickLock.
	FCriticalSection TickLock;
	int32 ActiveRuns = 0;
	double NextTickSeconds = 0.0;

	// Seconds to the next tick, negative while idle. Ticks stay a full period apart, so a stall never shortens a budget.
	double TickIfDue()
	{
		FScopeLock Lock(&TickLock);
		if (ActiveRuns == 0)
		{
			return -1.0;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextTickSeconds)
		{
			wasmtime_engine_increment_epoch(Engine);
			NextTickSeconds = Now + EpochTickSeconds;
		}
		return NextTickSeconds - Now;
	}

	virtual uint32 Run() override
	{
		while (!bStopping)
		{
			const double WaitSeconds = TickIfDue();
			if (WaitSeconds < 0.0)
			{
				WakeEvent->Wait();
				continue;
			}
			WakeEvent->Wait(static_cast<uint32>(FMath::Max(1, FMath::CeilToInt32(WaitSeconds * 1000.0))));
		}
		return 0;
	}

	virtual void Stop() override
	{
		bStopping = true;
		WakeEvent->Trigger();
	}

	// The first run after an idle spell restarts the tick phase, so a lone run always gets its full DeadlineTicks.
	void BeginRun()
	{
		bool bWake = false;
		{
			FScopeLock Lock(&TickLock);
			if (ActiveRuns++ == 0)
			{
				NextTickSeconds = FPlatformTime::Seconds() + EpochTickSeconds;
				bWake = true;
			}
		}
		if (bWake)
		{
			WakeEvent->Trigger();
		}
	}

	void EndRun()
	{
		FScopeLock Lock(&TickLock);
		--ActiveRuns;
	}
};

struct FCrowdyModScriptModuleState
{
	TSharedPtr<FCrowdyModScriptRuntime> Runtime;
	FCrowdyModScriptEngineState* Engine = nullptr;
	wasmtime_module_t* Module = nullptr;
};

struct FCrowdyModScriptInstanceState
{
	TSharedPtr<FCrowdyModScriptModule> Module;
	FCrowdyModScriptEngineState* Engine = nullptr;
	FCrowdyModScriptHost Host;

	wasmtime_store_t* Store = nullptr;
	wasmtime_instance_t Instance = {};
	wasmtime_memory_t Memory = {};
	wasmtime_func_t AllocFunc = {};
	wasmtime_func_t InitFunc = {};
	wasmtime_func_t TickFunc = {};
	wasmtime_func_t EventFunc = {};
	wasmtime_func_t FreeFunc = {};
	wasmtime_global_t FuelGlobal = {};
	bool bExportsResolved = false;
	bool bHasEvent = false;
	bool bHasFree = false;

	// Touched only by the dispatching thread. Ok means the dispatch's own result stands.
	int32 WorkTicksLeft = 0;
	bool bInAlloc = false;
	ECrowdyModScriptOutcome ForcedOutcome = ECrowdyModScriptOutcome::Ok;

	TArray<uint8> StateBlob;
	std::atomic<uint64> LastMemoryBytes{0};
	std::atomic<bool> bInDispatch{false};
	std::atomic<bool> bStopRequested{false};
};

namespace
{
	// Runs on the dispatching thread at every watchdog tick while guest code runs.
	wasmtime_error_t* OnEpochDeadline(const wasmtime_context_t*, void* Data, uint64_t* NextDelta, wasmtime_update_deadline_kind_t* UpdateKind)
	{
		FCrowdyModScriptInstanceState& State = *static_cast<FCrowdyModScriptInstanceState*>(Data);
		*NextDelta = 1;
		*UpdateKind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
		if (State.bStopRequested)
		{
			State.ForcedOutcome = ECrowdyModScriptOutcome::Trap;
			return wasmtime_error_new("the run was stopped");
		}
		if (--State.WorkTicksLeft > 0)
		{
			return nullptr;
		}
		State.ForcedOutcome = ECrowdyModScriptOutcome::Watchdog;
		return wasmtime_error_new("250 ms of work without a host call");
	}

	void BeginWatchedCall(FCrowdyModScriptInstanceState& State, wasmtime_context_t* Context)
	{
		State.WorkTicksLeft = DeadlineTicks;
		State.ForcedOutcome = ECrowdyModScriptOutcome::Ok;
		wasmtime_context_set_epoch_deadline(Context, 1);
		State.Engine->BeginRun();
	}

	FCrowdyModScriptInstanceState& CallerState(wasmtime_caller_t* Caller, wasmtime_context_t*& OutContext)
	{
		OutContext = wasmtime_caller_context(Caller);
		return *static_cast<FCrowdyModScriptInstanceState*>(wasmtime_context_get_data(OutContext));
	}

	// Null when any byte of [Ptr, Ptr + Len) lies outside guest memory.
	uint8* GuestBytes(const FCrowdyModScriptInstanceState& State, wasmtime_context_t* Context, int32 Ptr, uint32 Len)
	{
		if (!State.bExportsResolved)
		{
			return nullptr;
		}
		const uint64 Start = static_cast<uint32>(Ptr);
		if (Start + Len > wasmtime_memory_data_size(Context, &State.Memory))
		{
			return nullptr;
		}
		uint8* Data = wasmtime_memory_data(Context, &State.Memory);
		return Data ? Data + Start : nullptr;
	}

	void SetI32(wasmtime_val_t& Out, int32 Value)
	{
		Out.kind = WASMTIME_I32;
		Out.of.i32 = Value;
	}

	void SetI64(wasmtime_val_t& Out, int64 Value)
	{
		Out.kind = WASMTIME_I64;
		Out.of.i64 = Value;
	}

	wasm_trap_t* HostLog(void*, wasmtime_caller_t* Caller, const wasmtime_val_t* Args, size_t, wasmtime_val_t*, size_t)
	{
		wasmtime_context_t* Context = nullptr;
		FCrowdyModScriptInstanceState& State = CallerState(Caller, Context);
		const int32 Level = FMath::Clamp(Args[0].of.i32, 0, 3);
		const uint32 Len = FMath::Min(static_cast<uint32>(Args[2].of.i32), MaxLogBytes);
		const uint8* Bytes = GuestBytes(State, Context, Args[1].of.i32, Len);
		if (!Bytes)
		{
			return GuestTrap("ck.log: the message lies outside guest memory");
		}
		const FString Message = CrowdyModScriptTrace::Printable(Utf8ToString(reinterpret_cast<const char*>(Bytes), Len));
		if (State.Host.OnLog)
		{
			State.Host.OnLog(Level, Message);
			return nullptr;
		}
		UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("Guest log %d: %s"), Level, *Message);
		return nullptr;
	}

	wasm_trap_t* HostNowMs(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* Results, size_t)
	{
		SetI64(Results[0], FCrowdyModScriptRuntime::UnixNowMs());
		return nullptr;
	}

	wasm_trap_t* HostStateGet(void*, wasmtime_caller_t* Caller, const wasmtime_val_t* Args, size_t, wasmtime_val_t* Results, size_t)
	{
		wasmtime_context_t* Context = nullptr;
		FCrowdyModScriptInstanceState& State = CallerState(Caller, Context);
		const int32 Dest = Args[0].of.i32;
		const int32 Cap = Args[1].of.i32;
		const int32 Len = State.StateBlob.Num();
		if (Dest != 0 && Cap > 0)
		{
			const uint32 Copy = static_cast<uint32>(FMath::Min(Len, Cap));
			uint8* Target = GuestBytes(State, Context, Dest, Copy);
			if (!Target)
			{
				return GuestTrap("ck.state_get: the destination lies outside guest memory");
			}
			FMemory::Memcpy(Target, State.StateBlob.GetData(), Copy);
		}
		SetI32(Results[0], Len);
		return nullptr;
	}

	wasm_trap_t* HostStateSet(void*, wasmtime_caller_t* Caller, const wasmtime_val_t* Args, size_t, wasmtime_val_t* Results, size_t)
	{
		wasmtime_context_t* Context = nullptr;
		FCrowdyModScriptInstanceState& State = CallerState(Caller, Context);
		const uint32 Len = static_cast<uint32>(Args[1].of.i32);
		if (Len > MaxStateBytes)
		{
			SetI32(Results[0], 1);
			return nullptr;
		}
		const uint8* Source = GuestBytes(State, Context, Args[0].of.i32, Len);
		if (!Source)
		{
			return GuestTrap("ck.state_set: the source lies outside guest memory");
		}
		State.StateBlob.Reset();
		State.StateBlob.Append(Source, static_cast<int32>(Len));
		SetI32(Results[0], 0);
		return nullptr;
	}

	wasm_trap_t* HostRandomGet(void*, wasmtime_caller_t* Caller, const wasmtime_val_t* Args, size_t, wasmtime_val_t* Results, size_t)
	{
		wasmtime_context_t* Context = nullptr;
		FCrowdyModScriptInstanceState& State = CallerState(Caller, Context);
		const uint32 Len = static_cast<uint32>(Args[1].of.i32);
		uint8* Target = GuestBytes(State, Context, Args[0].of.i32, Len);
		if (!Target)
		{
			return GuestTrap("random_get: the buffer lies outside guest memory");
		}
		const bool bFilled = Len == 0 || BCryptGenRandom(nullptr, Target, Len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
		SetI32(Results[0], bFilled ? 0 : WasiErrnoIo);
		return nullptr;
	}

	// Calls the guest's ck_alloc re-entrantly, copies the answer in and packs (ptr << 32) | len.
	wasm_trap_t* CopyAnswerToGuest(FCrowdyModScriptInstanceState& State, wasmtime_context_t* Context, TConstArrayView<uint8> Answer, int64& OutPacked)
	{
		OutPacked = 0;
		if (Answer.IsEmpty())
		{
			return nullptr;
		}
		if (Answer.Num() > FCrowdyModScriptInstance::MemoryLimitBytes)
		{
			return GuestTrap("ck.host_call: the answer is larger than guest memory may grow");
		}
		wasmtime_val_t Arg;
		SetI32(Arg, Answer.Num());
		wasmtime_val_t Returned = {};
		wasm_trap_t* Trap = nullptr;
		State.bInAlloc = true;
		wasmtime_error_t* Error = wasmtime_func_call(Context, &State.AllocFunc, &Arg, 1, &Returned, 1, &Trap);
		State.bInAlloc = false;
		if (Error)
		{
			const FTCHARToUTF8 Message(*FString::Printf(TEXT("ck.host_call: ck_alloc failed: %s"), *TakeErrorMessage(Error)));
			return wasmtime_trap_new(reinterpret_cast<const char*>(Message.Get()), Message.Length());
		}
		if (Trap)
		{
			return Trap;
		}
		const int32 Ptr = Returned.of.i32;
		uint8* Target = GuestBytes(State, Context, Ptr, static_cast<uint32>(Answer.Num()));
		if (!Target)
		{
			return GuestTrap("ck.host_call: ck_alloc returned a block outside guest memory");
		}
		FMemory::Memcpy(Target, Answer.GetData(), Answer.Num());
		OutPacked = static_cast<int64>((static_cast<uint64>(static_cast<uint32>(Ptr)) << 32) | static_cast<uint32>(Answer.Num()));
		return nullptr;
	}

	wasm_trap_t* HostCall(void*, wasmtime_caller_t* Caller, const wasmtime_val_t* Args, size_t, wasmtime_val_t* Results, size_t)
	{
		wasmtime_context_t* Context = nullptr;
		FCrowdyModScriptInstanceState& State = CallerState(Caller, Context);
		if (!State.bExportsResolved)
		{
			return GuestTrap("ck.host_call: called before the instance finished instantiating");
		}
		// Each nested call would recurse on the worker's native stack.
		if (State.bInAlloc)
		{
			return GuestTrap("ck.host_call: host_call inside ck_alloc");
		}
		const uint32 Len = static_cast<uint32>(Args[1].of.i32);
		const bool bTooLarge = Len > MaxHostRequestBytes;
		const uint8* Request = bTooLarge ? nullptr : GuestBytes(State, Context, Args[0].of.i32, Len);
		if (!bTooLarge && !Request)
		{
			return GuestTrap("ck.host_call: the request lies outside guest memory");
		}

		TArray<uint8> Answer;
		bool bAnswered = true;
		if (bTooLarge)
		{
			Answer = AnsiBytes(RequestTooLargeAnswer);
		}
		else if (State.Host.OnHostCallOrTimeout)
		{
			bAnswered = State.Host.OnHostCallOrTimeout(TConstArrayView<uint8>(Request, static_cast<int32>(Len)), Answer);
		}
		else
		{
			Answer = State.Host.OnHostCall ? State.Host.OnHostCall(TConstArrayView<uint8>(Request, static_cast<int32>(Len))) : AnsiBytes(NoHandlerAnswer);
		}
		// Each host call starts a fresh 250 ms work budget, and the wait for the answer spent none of it.
		State.WorkTicksLeft = DeadlineTicks;
		wasmtime_context_set_epoch_deadline(Context, 1);
		if (!bAnswered && State.bStopRequested)
		{
			State.ForcedOutcome = ECrowdyModScriptOutcome::Trap;
			return GuestTrap("ck.host_call: the run was stopped");
		}
		if (!bAnswered)
		{
			State.ForcedOutcome = ECrowdyModScriptOutcome::HostTimeout;
			return GuestTrap("ck.host_call: the host gave no answer in time");
		}

		int64 Packed = 0;
		if (wasm_trap_t* Trap = CopyAnswerToGuest(State, Context, Answer, Packed))
		{
			return Trap;
		}
		SetI64(Results[0], Packed);
		return nullptr;
	}

	struct FImportSpec
	{
		const char* Module;
		const char* Name;
		FFuncShape Shape;
		wasmtime_func_callback_t Callback;
	};

	const FImportSpec ImportSpecs[] = {
		{"ck", "log", {3, 0, {WASM_I32, WASM_I32, WASM_I32}, WASM_I32}, &HostLog},
		{"ck", "now_ms", {0, 1, {}, WASM_I64}, &HostNowMs},
		{"ck", "state_get", {2, 1, {WASM_I32, WASM_I32}, WASM_I32}, &HostStateGet},
		{"ck", "state_set", {2, 1, {WASM_I32, WASM_I32}, WASM_I32}, &HostStateSet},
		{"ck", "host_call", {2, 1, {WASM_I32, WASM_I32}, WASM_I64}, &HostCall},
		{"wasi_snapshot_preview1", "random_get", {2, 1, {WASM_I32, WASM_I32}, WASM_I32}, &HostRandomGet},
	};

	bool ReadVarU32(TConstArrayView<uint8> Bytes, int64& Offset, uint32& Out)
	{
		Out = 0;
		for (int32 Shift = 0; Shift < 35; Shift += 7)
		{
			if (Offset >= Bytes.Num())
			{
				return false;
			}
			const uint8 Byte = Bytes[static_cast<int32>(Offset++)];
			Out |= static_cast<uint32>(Byte & 0x7F) << Shift;
			if ((Byte & 0x80) == 0)
			{
				return true;
			}
		}
		return false;
	}

	// Walks the section headers of bytes that already compiled.
	bool HasStartSection(TConstArrayView<uint8> Bytes)
	{
		constexpr uint8 StartSectionId = 8;
		int64 Offset = 8;
		while (Offset < Bytes.Num())
		{
			const uint8 Id = Bytes[static_cast<int32>(Offset++)];
			uint32 Size = 0;
			if (!ReadVarU32(Bytes, Offset, Size))
			{
				return false;
			}
			if (Id == StartSectionId)
			{
				return true;
			}
			Offset += Size;
		}
		return false;
	}

	bool ValidateImports(const wasmtime_module_t* Module, FString& OutError)
	{
		wasm_importtype_vec_t Imports;
		wasmtime_module_imports(Module, &Imports);
		ON_SCOPE_EXIT { wasm_importtype_vec_delete(&Imports); };

		for (size_t Index = 0; Index < Imports.size; ++Index)
		{
			const wasm_importtype_t* Import = Imports.data[Index];
			const wasm_name_t* ModuleName = wasm_importtype_module(Import);
			const wasm_name_t* Name = wasm_importtype_name(Import);
			const FImportSpec* Spec = Algo::FindByPredicate(ImportSpecs, [ModuleName, Name](const FImportSpec& Candidate)
			{
				return NameIs(ModuleName, Candidate.Module) && NameIs(Name, Candidate.Name);
			});
			if (!Spec)
			{
				OutError = FString::Printf(TEXT("import %s.%s is not allowed"), *NameToString(ModuleName), *NameToString(Name));
				return false;
			}
			const wasm_externtype_t* Type = wasm_importtype_type(Import);
			if (wasm_externtype_kind(Type) != WASM_EXTERN_FUNC || !ShapeMatches(wasm_externtype_as_functype_const(Type), Spec->Shape))
			{
				OutError = FString::Printf(TEXT("import %hs.%hs has the wrong signature"), Spec->Module, Spec->Name);
				return false;
			}
		}
		return true;
	}

	const wasm_externtype_t* FindExport(const wasm_exporttype_vec_t& Exports, const char* Name, wasm_externkind_t Kind)
	{
		for (size_t Index = 0; Index < Exports.size; ++Index)
		{
			const wasm_exporttype_t* Export = Exports.data[Index];
			if (!NameIs(wasm_exporttype_name(Export), Name))
			{
				continue;
			}
			const wasm_externtype_t* Type = wasm_exporttype_type(Export);
			return wasm_externtype_kind(Type) == Kind ? Type : nullptr;
		}
		return nullptr;
	}

	bool ValidateExports(const wasmtime_module_t* Module, FString& OutError)
	{
		wasm_exporttype_vec_t Exports;
		wasmtime_module_exports(Module, &Exports);
		ON_SCOPE_EXIT { wasm_exporttype_vec_delete(&Exports); };

		const wasm_externtype_t* Memory = FindExport(Exports, "memory", WASM_EXTERN_MEMORY);
		const wasm_memorytype_t* MemoryType = Memory ? wasm_externtype_as_memorytype_const(Memory) : nullptr;
		if (!MemoryType || wasmtime_memorytype_is64(MemoryType) || wasmtime_memorytype_isshared(MemoryType))
		{
			OutError = TEXT("export memory is missing or is not a private 32-bit memory");
			return false;
		}
		const wasm_externtype_t* Fuel = FindExport(Exports, "ck_fuel", WASM_EXTERN_GLOBAL);
		const wasm_globaltype_t* FuelType = Fuel ? wasm_externtype_as_globaltype_const(Fuel) : nullptr;
		if (!FuelType || wasm_valtype_kind(wasm_globaltype_content(FuelType)) != WASM_I64 || wasm_globaltype_mutability(FuelType) != WASM_VAR)
		{
			OutError = TEXT("export ck_fuel is missing or is not a mutable i64 global");
			return false;
		}

		struct FFuncExport
		{
			const char* Name;
			const FFuncShape* Shape;
		};
		const FFuncExport Funcs[] = {{"ck_alloc", &AllocShape}, {"init", &InitShape}, {"tick", &TickShape}};
		for (const FFuncExport& Func : Funcs)
		{
			const wasm_externtype_t* Type = FindExport(Exports, Func.Name, WASM_EXTERN_FUNC);
			if (!Type || !ShapeMatches(wasm_externtype_as_functype_const(Type), *Func.Shape))
			{
				OutError = FString::Printf(TEXT("export %hs is missing or has the wrong signature"), Func.Name);
				return false;
			}
		}
		return true;
	}

	bool GetExport(wasmtime_context_t* Context, const wasmtime_instance_t& Instance, const char* Name, wasmtime_extern_kind_t Kind, wasmtime_extern_t& Out)
	{
		return wasmtime_instance_export_get(Context, &Instance, Name, FCStringAnsi::Strlen(Name), &Out) && Out.kind == Kind;
	}

	bool TakesPtrLen(wasmtime_context_t* Context, const wasmtime_func_t& Func)
	{
		wasm_functype_t* Type = wasmtime_func_type(Context, &Func);
		const bool bMatches = Type && ShapeMatches(Type, PtrLenShape);
		if (Type)
		{
			wasm_functype_delete(Type);
		}
		return bMatches;
	}

	bool ResolveExports(FCrowdyModScriptInstanceState& State, wasmtime_context_t* Context)
	{
		wasmtime_extern_t Memory;
		wasmtime_extern_t Alloc;
		wasmtime_extern_t Init;
		wasmtime_extern_t Tick;
		wasmtime_extern_t Fuel;
		if (!GetExport(Context, State.Instance, "memory", WASMTIME_EXTERN_MEMORY, Memory)
			|| !GetExport(Context, State.Instance, "ck_alloc", WASMTIME_EXTERN_FUNC, Alloc)
			|| !GetExport(Context, State.Instance, "init", WASMTIME_EXTERN_FUNC, Init)
			|| !GetExport(Context, State.Instance, "tick", WASMTIME_EXTERN_FUNC, Tick)
			|| !GetExport(Context, State.Instance, "ck_fuel", WASMTIME_EXTERN_GLOBAL, Fuel))
		{
			return false;
		}
		State.Memory = Memory.of.memory;
		State.AllocFunc = Alloc.of.func;
		State.InitFunc = Init.of.func;
		State.TickFunc = Tick.of.func;
		State.FuelGlobal = Fuel.of.global;
		// Optional, and called with (ptr, len): one of another shape counts as absent, as it would not take that call.
		wasmtime_extern_t Event;
		wasmtime_extern_t Free;
		State.bHasEvent = GetExport(Context, State.Instance, "on_event", WASMTIME_EXTERN_FUNC, Event) && TakesPtrLen(Context, Event.of.func);
		State.bHasFree = GetExport(Context, State.Instance, "ck_free", WASMTIME_EXTERN_FUNC, Free) && TakesPtrLen(Context, Free.of.func);
		State.EventFunc = State.bHasEvent ? Event.of.func : wasmtime_func_t{};
		State.FreeFunc = State.bHasFree ? Free.of.func : wasmtime_func_t{};
		State.bExportsResolved = true;
		return true;
	}

	// Copies Payload into memory from ck_alloc, calls on_event with it, then hands it back to ck_free.
	wasmtime_error_t* CallOnEvent(FCrowdyModScriptInstanceState& State, wasmtime_context_t* Context, TConstArrayView<uint8> Payload, wasm_trap_t*& OutTrap)
	{
		int64 Packed = 0;
		OutTrap = CopyAnswerToGuest(State, Context, Payload, Packed);
		if (OutTrap)
		{
			return nullptr;
		}
		wasmtime_val_t Args[2];
		SetI32(Args[0], static_cast<int32>(static_cast<uint64>(Packed) >> 32));
		SetI32(Args[1], Payload.Num());
		if (wasmtime_error_t* Error = wasmtime_func_call(Context, &State.EventFunc, Args, 2, nullptr, 0, &OutTrap))
		{
			return Error;
		}
		if (OutTrap || !State.bHasFree || Packed == 0)
		{
			return nullptr;
		}
		return wasmtime_func_call(Context, &State.FreeFunc, Args, 2, nullptr, 0, &OutTrap);
	}

	const TCHAR* ExportName(ECrowdyModScriptExport Export)
	{
		switch (Export)
		{
		case ECrowdyModScriptExport::Init:  return TEXT("init");
		case ECrowdyModScriptExport::Tick:  return TEXT("tick");
		default:                            return TEXT("on_event");
		}
	}

	int64 ReadFuel(wasmtime_context_t* Context, const FCrowdyModScriptInstanceState& State)
	{
		wasmtime_val_t Fuel = {};
		wasmtime_global_get(Context, &State.FuelGlobal, &Fuel);
		return Fuel.kind == WASMTIME_I64 ? Fuel.of.i64 : 0;
	}
}

FCrowdyModScriptRuntime::FCrowdyModScriptRuntime()
	: State(MakeUnique<FCrowdyModScriptEngineState>())
{
	FCrowdyModScriptEngineState& Engine = *State;
	wasm_config_t* Config = wasm_config_new();
	if (wasmtime_error_t* Error = wasmtime_config_target_set(Config, "pulley64"))
	{
		Engine.UnavailableReason = FString::Printf(TEXT("the Pulley interpreter target is unavailable: %s"), *TakeErrorMessage(Error));
		wasm_config_delete(Config);
		UE_LOG(LogCrowdyModScripts, Warning, TEXT("Mod Scripts are unavailable: %s"), *Engine.UnavailableReason);
		return;
	}
	wasmtime_config_epoch_interruption_set(Config, true);
	wasmtime_config_consume_fuel_set(Config, false);
	// The interpreter checks bounds itself, so no process-wide fault handlers are installed beside the engine's own.
	wasmtime_config_signals_based_traps_set(Config, false);

	Engine.Engine = wasm_engine_new_with_config(Config);
	if (!Engine.Engine || !wasmtime_engine_is_pulley(Engine.Engine))
	{
		Engine.UnavailableReason = TEXT("the engine did not start on the Pulley interpreter");
		UE_LOG(LogCrowdyModScripts, Warning, TEXT("Mod Scripts are unavailable: %s"), *Engine.UnavailableReason);
		return;
	}

	Engine.Linker = wasmtime_linker_new(Engine.Engine);
	for (const FImportSpec& Spec : ImportSpecs)
	{
		wasm_functype_t* Type = NewFuncType(Spec.Shape);
		wasmtime_error_t* Error = wasmtime_linker_define_func(Engine.Linker, Spec.Module, FCStringAnsi::Strlen(Spec.Module), Spec.Name,
			FCStringAnsi::Strlen(Spec.Name), Type, Spec.Callback, nullptr, nullptr);
		wasm_functype_delete(Type);
		if (Error)
		{
			Engine.UnavailableReason = FString::Printf(TEXT("defining import %hs.%hs failed: %s"), Spec.Module, Spec.Name, *TakeErrorMessage(Error));
			UE_LOG(LogCrowdyModScripts, Warning, TEXT("Mod Scripts are unavailable: %s"), *Engine.UnavailableReason);
			return;
		}
	}

	Engine.WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	Engine.Thread = FRunnableThread::Create(&Engine, TEXT("CrowdyModScriptWatchdog"), 0, TPri_AboveNormal);
	if (!Engine.Thread)
	{
		Engine.UnavailableReason = TEXT("the watchdog thread could not start");
		UE_LOG(LogCrowdyModScripts, Warning, TEXT("Mod Scripts are unavailable: %s"), *Engine.UnavailableReason);
		return;
	}
	UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("Mod Script engine started on Pulley (Wasmtime %hs)"), WASMTIME_VERSION);
}

FCrowdyModScriptRuntime::~FCrowdyModScriptRuntime()
{
	FCrowdyModScriptEngineState& Engine = *State;
	if (Engine.Thread)
	{
		Engine.Thread->Kill(true);
		delete Engine.Thread;
	}
	if (Engine.WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(Engine.WakeEvent);
	}
	if (Engine.Linker)
	{
		wasmtime_linker_delete(Engine.Linker);
	}
	if (Engine.Engine)
	{
		wasm_engine_delete(Engine.Engine);
	}
}

bool FCrowdyModScriptRuntime::IsAvailable() const
{
	return State->Thread != nullptr;
}

const FString& FCrowdyModScriptRuntime::GetUnavailableReason() const
{
	return State->UnavailableReason;
}

FCrowdyModScriptModule::FCrowdyModScriptModule()
	: State(MakeUnique<FCrowdyModScriptModuleState>())
{
}

FCrowdyModScriptModule::~FCrowdyModScriptModule()
{
	if (State->Module)
	{
		wasmtime_module_delete(State->Module);
	}
}

TSharedPtr<FCrowdyModScriptModule> FCrowdyModScriptModule::Compile(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, TConstArrayView<uint8> Bytes, FString& OutError)
{
	if (!Runtime->IsAvailable())
	{
		OutError = FString::Printf(TEXT("Mod Scripts are unavailable: %s"), *Runtime->GetUnavailableReason());
		return nullptr;
	}
	if (Bytes.Num() > MaxModuleBytes)
	{
		OutError = TEXT("the module is larger than 512 KiB");
		return nullptr;
	}
	wasmtime_module_t* Compiled = nullptr;
	if (wasmtime_error_t* Error = wasmtime_module_new(Runtime->State->Engine, Bytes.GetData(), Bytes.Num(), &Compiled))
	{
		OutError = FString::Printf(TEXT("the module does not compile: %s"), *TakeErrorMessage(Error));
		return nullptr;
	}
	// A start function would run inside Create, on the thread that starts the script.
	if (HasStartSection(Bytes))
	{
		OutError = TEXT("a start function is not allowed");
		wasmtime_module_delete(Compiled);
		return nullptr;
	}
	if (!ValidateImports(Compiled, OutError) || !ValidateExports(Compiled, OutError))
	{
		wasmtime_module_delete(Compiled);
		return nullptr;
	}

	TSharedPtr<FCrowdyModScriptModule> Module = MakeShareable(new FCrowdyModScriptModule());
	Module->State->Runtime = Runtime;
	Module->State->Engine = Runtime->State.Get();
	Module->State->Module = Compiled;
	UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("Compiled a Mod Script module of %d bytes"), Bytes.Num());
	return Module;
}

FCrowdyModScriptInstance::FCrowdyModScriptInstance()
	: State(MakeUnique<FCrowdyModScriptInstanceState>())
{
}

FCrowdyModScriptInstance::~FCrowdyModScriptInstance()
{
	if (State->Store)
	{
		wasmtime_store_delete(State->Store);
	}
}

TSharedPtr<FCrowdyModScriptInstance> FCrowdyModScriptInstance::Create(const TSharedRef<FCrowdyModScriptModule>& Module, FCrowdyModScriptHost Host, FString& OutError)
{
	TSharedPtr<FCrowdyModScriptInstance> Instance = MakeShareable(new FCrowdyModScriptInstance());
	FCrowdyModScriptInstanceState& S = *Instance->State;
	S.Module = Module;
	S.Engine = Module->State->Engine;
	S.Host = MoveTemp(Host);
	S.Store = wasmtime_store_new(S.Engine->Engine, &S, nullptr);
	wasmtime_store_limiter(S.Store, MemoryLimitBytes, MaxTableElements, 1, 1, 1);
	wasmtime_store_epoch_deadline_callback(S.Store, &OnEpochDeadline, &S, nullptr);

	// Instantiation runs any start function, so it is watched like a dispatch.
	wasmtime_context_t* Context = wasmtime_store_context(S.Store);
	wasm_trap_t* Trap = nullptr;
	BeginWatchedCall(S, Context);
	wasmtime_error_t* Error = wasmtime_linker_instantiate(S.Engine->Linker, Context, Module->State->Module, &S.Instance, &Trap);
	S.Engine->EndRun();
	if (Error)
	{
		OutError = FString::Printf(TEXT("the module does not instantiate: %s"), *TakeErrorMessage(Error));
		return nullptr;
	}
	if (Trap)
	{
		OutError = FString::Printf(TEXT("the module trapped while instantiating: %s"), *TrapMessage(Trap));
		wasm_trap_delete(Trap);
		return nullptr;
	}
	if (!ResolveExports(S, Context))
	{
		OutError = TEXT("the instance is missing a required export");
		return nullptr;
	}
	S.LastMemoryBytes = wasmtime_memory_data_size(Context, &S.Memory);
	return Instance;
}

FCrowdyModScriptResult FCrowdyModScriptInstance::Dispatch(ECrowdyModScriptExport Export, int64 FuelPerDispatch, int32 Arg, TConstArrayView<uint8> Payload)
{
	FCrowdyModScriptResult Result;
	FCrowdyModScriptInstanceState& S = *State;
	if (S.bStopRequested)
	{
		Result.Outcome = ECrowdyModScriptOutcome::Trap;
		Result.Message = TEXT("the instance was stopped");
		return Result;
	}
	if (S.bInDispatch.exchange(true))
	{
		Result.Outcome = ECrowdyModScriptOutcome::Trap;
		Result.Message = TEXT("a dispatch is already running on this instance");
		return Result;
	}
	ON_SCOPE_EXIT { S.bInDispatch = false; };

	wasmtime_context_t* Context = wasmtime_store_context(S.Store);
	wasmtime_val_t Fuel;
	SetI64(Fuel, FuelPerDispatch);
	if (wasmtime_error_t* Error = wasmtime_global_set(Context, &S.FuelGlobal, &Fuel))
	{
		Result.Outcome = ECrowdyModScriptOutcome::Trap;
		Result.Message = TakeErrorMessage(Error);
		return Result;
	}

	const bool bTick = Export == ECrowdyModScriptExport::Tick;
	const bool bEvent = Export == ECrowdyModScriptExport::Event;
	if (bEvent && !S.bHasEvent)
	{
		return Result;
	}
	wasmtime_val_t TickArg;
	SetI32(TickArg, Arg);
	wasm_trap_t* Trap = nullptr;
	const double StartSeconds = FPlatformTime::Seconds();
	BeginWatchedCall(S, Context);
	wasmtime_error_t* Error = bEvent ? CallOnEvent(S, Context, Payload, Trap)
		: wasmtime_func_call(Context, bTick ? &S.TickFunc : &S.InitFunc, bTick ? &TickArg : nullptr, bTick ? 1 : 0, nullptr, 0, &Trap);
	S.Engine->EndRun();
	Result.ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	S.LastMemoryBytes = wasmtime_memory_data_size(Context, &S.Memory);

	if (Error)
	{
		Result.Outcome = ECrowdyModScriptOutcome::Trap;
		Result.Message = TakeErrorMessage(Error);
	}
	else if (Trap)
	{
		Result.Outcome = ReadFuel(Context, S) <= 0 ? ECrowdyModScriptOutcome::Fuel : ECrowdyModScriptOutcome::Trap;
		Result.Message = TrapMessage(Trap);
		wasm_trap_delete(Trap);
	}
	if (Result.Outcome != ECrowdyModScriptOutcome::Ok && S.ForcedOutcome != ECrowdyModScriptOutcome::Ok)
	{
		Result.Outcome = S.ForcedOutcome;
	}
	UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("Dispatch %s ended %s after %.1f ms %s"),
		ExportName(Export), LexToString(Result.Outcome), Result.ElapsedMs, *Result.Message);
	return Result;
}

uint64 FCrowdyModScriptInstance::GetMemoryBytes() const
{
	return State->LastMemoryBytes.load();
}

bool FCrowdyModScriptInstance::HasOnEvent() const
{
	return State->bHasEvent;
}

void FCrowdyModScriptInstance::RequestStop()
{
	State->bStopRequested = true;
}

#else

struct FCrowdyModScriptEngineState
{
};

struct FCrowdyModScriptModuleState
{
};

struct FCrowdyModScriptInstanceState
{
};

FCrowdyModScriptRuntime::FCrowdyModScriptRuntime() = default;
FCrowdyModScriptRuntime::~FCrowdyModScriptRuntime() = default;

bool FCrowdyModScriptRuntime::IsAvailable() const
{
	return false;
}

const FString& FCrowdyModScriptRuntime::GetUnavailableReason() const
{
	static const FString Reason = TEXT("Mod Scripts are not supported on this platform");
	return Reason;
}

FCrowdyModScriptModule::FCrowdyModScriptModule() = default;
FCrowdyModScriptModule::~FCrowdyModScriptModule() = default;

TSharedPtr<FCrowdyModScriptModule> FCrowdyModScriptModule::Compile(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, TConstArrayView<uint8>, FString& OutError)
{
	OutError = Runtime->GetUnavailableReason();
	return nullptr;
}

FCrowdyModScriptInstance::FCrowdyModScriptInstance() = default;
FCrowdyModScriptInstance::~FCrowdyModScriptInstance() = default;

TSharedPtr<FCrowdyModScriptInstance> FCrowdyModScriptInstance::Create(const TSharedRef<FCrowdyModScriptModule>&, FCrowdyModScriptHost, FString& OutError)
{
	OutError = TEXT("Mod Scripts are not supported on this platform");
	return nullptr;
}

FCrowdyModScriptResult FCrowdyModScriptInstance::Dispatch(ECrowdyModScriptExport, int64, int32, TConstArrayView<uint8>)
{
	FCrowdyModScriptResult Result;
	Result.Outcome = ECrowdyModScriptOutcome::Trap;
	Result.Message = TEXT("Mod Scripts are not supported on this platform");
	return Result;
}

uint64 FCrowdyModScriptInstance::GetMemoryBytes() const
{
	return 0;
}

bool FCrowdyModScriptInstance::HasOnEvent() const
{
	return false;
}

void FCrowdyModScriptInstance::RequestStop()
{
}

#endif

class FCrowdyModScriptRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime = MakeShared<FCrowdyModScriptRuntime>();
		FScopeLock Lock(&GRuntimeLock);
		GRuntime = Runtime;
	}

	// The pool joins its workers before the runtime they dispatch on goes.
	virtual void ShutdownModule() override
	{
		TSharedPtr<FCrowdyModScriptRuntime> Runtime;
		TSharedPtr<FCrowdyModScriptWorkerPool> Pool;
		{
			FScopeLock Lock(&GRuntimeLock);
			Runtime = MoveTemp(GRuntime);
			Pool = MoveTemp(GSharedPool);
		}
		TArray<FModuleCacheEntry> Cached;
		{
			FScopeLock Lock(&GModuleCacheLock);
			Cached = MoveTemp(GModuleCache);
		}
	}
};

IMPLEMENT_MODULE(FCrowdyModScriptRuntimeModule, CrowdyModScriptRuntime);
