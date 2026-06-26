#pragma once

#include "CoreMinimal.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/RPC/FCrowdyFnInfo.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "UObject/Class.h"        // UFunction
#include "UObject/UnrealType.h"   // FProperty, TFieldIterator, property flags
#include "Templates/Tuple.h"
#include "Templates/UnrealTemplate.h"
#include <type_traits>           // std::is_convertible_v
#include <utility>               // std::index_sequence, std::index_sequence_for

class AActor;
class UCrowdyEntitySubsystem;
class UWorld;
struct FOutParmRec;

DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyRPC, Log, All);

// Format version prefixed to every FCrowdyRpcCall::ParamBlob. Bump whenever the
// on-wire parameter encoding changes so stale peers drop instead of misparsing.
inline constexpr uint8 CrowdyRpcParamBlobVersion = 1;

// A reliable RPC rides a channel message whose payload is a small reliability header
// followed by the serialized FCrowdyRpcCall. The header is a format version plus a flags
// byte; the flags are reserved for a later guaranteed-delivery layer (per-message id,
// dedup, ack) and are zero for coverage-only sends. Bump the version if the layout changes.
inline constexpr uint8 CrowdyChannelRpcVersion = 1;

// Channel payloads are capped at 1024 bytes on the wire. A reliable RPC whose encoded
// payload would exceed this is dropped loudly rather than truncated.
inline constexpr int32 CrowdyChannelPayloadMaxBytes = 1024;

// How one object-reference parameter is encoded so it resolves on a remote client. A raw
// pointer has no cross-process meaning, so each object rides one of these by stable identity.
enum class ECrowdyObjectRefTag : uint8
{
	Null = 0,    // null, or a runtime object with no portable identity
	Entity = 1,  // a tracked entity actor, addressed by its entity FGuid
	Path = 2,    // an asset or a class, addressed by its object path
};

/**
 * UFUNCTION meta keys the CROWDY_EVENT macro stamps to declare per-function
 * routing. BuildFnInfo reads them (where metadata exists) to fill FCrowdyFnInfo;
 * the macro must emit these exact spellings.
 */
namespace CrowdyRpcMetaKeys
{
	inline const TCHAR* Recipient = TEXT("CrowdyRecipient");
	inline const TCHAR* Decay     = TEXT("CrowdyDecay");
	inline const TCHAR* Distance  = TEXT("CrowdyDistance");

	// Name of the channel a Multicast routes over. Empty means the app-wide default session channel.
	// Stored by name (not id) so the same event resolves across environments (dev/prod).
	inline const TCHAR* Channel   = TEXT("CrowdyChannel");

	// Marks a Blueprint event as RPC-style ("Crowdy Replicates"): calling it routes over the
	// transport and runs the body on every client, the same as a C++ CROWDY_EVENT. Distinguishes
	// it from a struct-handler (which carries only CrowdyEvent) so the router does not also bind
	// it as a handler and the compiler injects the dispatch gate. C++ RPCs use the macro instead.
	inline const TCHAR* Replicates = TEXT("CrowdyReplicates");
	inline const TCHAR* LegacyReplicate = TEXT("CrowdyReplicate");
	inline const TCHAR* Replicate = Replicates;

	inline bool HasReplicatesMeta(const UField* Field)
	{
#if WITH_METADATA
		return Field
			&& (Field->HasMetaData(Replicates) || Field->HasMetaData(LegacyReplicate));
#else
		(void)Field;
		return false;
#endif
	}
}

/**
 * Reflection-driven core of the RPC-style CrowdyEvent system. SendChecked builds a
 * reflected parameter frame from typed C++ arguments, serializes it into an
 * FCrowdyRpcCall, and routes it over the Crowdy transport with the function's
 * per-function recipient/decay/distance. ApplyCall is the receive side: it rebuilds
 * the frame from the bytes and invokes the receiver via ProcessEvent. The
 * client-authoritative ownership model is layered onto SerializeAndRoute in a later
 * phase; the marshal/serialize/route and unmarshal/invoke halves live here.
 */
class CROWDYREPLICATION_API FCrowdyRPC
{
public:

	/**
	 * Sets the per-world entity subsystem the object-reference codec resolves against, for the
	 * duration of an encode or decode, and restores the previous value when it goes out of scope.
	 * Object parameters are addressed by entity GUID, asset path, or class path; only the entity
	 * form needs the subsystem. Send and receive entry points install one of these around the
	 * marshal/unmarshal so the codec need not thread the subsystem through every call. Game-thread
	 * only, mirroring the other scoped serialization state on this class.
	 */
	struct CROWDYREPLICATION_API FScopedEntityContext
	{
		explicit FScopedEntityContext(const UObject* ContextObject);
		explicit FScopedEntityContext(UCrowdyEntitySubsystem* Entities);
		~FScopedEntityContext();

		FScopedEntityContext(const FScopedEntityContext&) = delete;
		FScopedEntityContext& operator=(const FScopedEntityContext&) = delete;

	private:
		UCrowdyEntitySubsystem* Previous;
	};

	/**
	 * Compile-time-checked entry point. The unnamed member-function-pointer
	 * parameter is used purely so the compiler deduces the receiver's exact
	 * parameter types (TParams) from &Class::Func_Implementation; the static_asserts
	 * then reject a wrong argument count or a non-convertible argument at compile
	 * time, and each argument is materialized AS its declared type so an int/float
	 * mismatch converts predictably instead of bit-corrupting the frame.
	 */
	template <typename C, typename... TParams, typename... TArgs>
	static void SendChecked(UObject* Obj, const TCHAR* ImplName,
	                        void (C::*MemberFn)(TParams...), TArgs&&... Args)
	{
		if (!Obj)
		{
			return;
		}

		UFunction* Fn = ResolveFunction(C::StaticClass(), ImplName);
		if (!Fn)
		{
			return;
		}

		const FCrowdyFnInfo Info = GetFnInfo(Fn);

		// Object-reference parameters resolve against this object's world while the call is
		// encoded and, for the local run, decoded. Scalar and struct events leave it unused.
		FScopedEntityContext EntityContext(Obj);

		FCrowdyRpcCall Call = MarshalCall(Fn, Info, MemberFn, Forward<TArgs>(Args)...);
		SerializeAndRoute(Obj, Fn, Info, MoveTemp(Call));
	}

	/**
	 * Marshals compile-time-checked arguments into a wire-ready FCrowdyRpcCall: builds
	 * the reflected parameter frame, verifies each argument against the receiver's
	 * declared parameters, serializes the frame, and returns the packed call. The
	 * unnamed member-function-pointer parameter exists only so the compiler deduces
	 * the receiver's exact parameter types; each argument is then materialized AS its
	 * declared type so an int/float mismatch converts predictably instead of
	 * bit-corrupting the frame. SendChecked routes the result over the transport;
	 * automation can hand it to ApplyCall to exercise the receive path in-process.
	 */
	template <typename C, typename... TParams, typename... TArgs>
	static FCrowdyRpcCall MarshalCall(UFunction* Fn, const FCrowdyFnInfo& Info,
	                                  void (C::*)(TParams...), TArgs&&... Args)
	{
		static_assert(sizeof...(TParams) == sizeof...(TArgs),
			"CrowdyEvent: wrong number of arguments.");
		static_assert((std::is_convertible_v<TArgs, TParams> && ...),
			"CrowdyEvent: argument type mismatch.");

		if (!Fn)
		{
			return FCrowdyRpcCall{};
		}

		// ProcessEvent expects a contiguous buffer matching the function's parameter
		// layout; ParmsSize covers every parameter slot. Zero it first so any POD
		// parameter starts clean, then construct non-POD members in place.
		const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
		uint8* Frame = static_cast<uint8*>(FMemory_Alloca(FrameSize));
		FMemory::Memzero(Frame, FrameSize);
		if (!Info.bParamsPOD)
		{
			Fn->InitializeStruct(Frame);
		}

		PlaceAll(Fn, Frame, std::index_sequence_for<TParams...>{},
		         TTuple<TParams...>(Forward<TArgs>(Args)...));

		FCrowdyRpcCall Call = BuildCall(Fn, Info, Frame);

		if (!Info.bParamsPOD)
		{
			Fn->DestroyStruct(Frame);
		}

		return Call;
	}

	// --- Reflection helpers (defined in CrowdyRPC.cpp) ---

	// Finds the receiver UFunction by name on Class (searching base classes too).
	// Logs and returns null when missing.
	static UFunction* ResolveFunction(UClass* Class, const TCHAR* ImplName);

	// Builds the routing/serialization metadata for a receiver function. Phase 1
	// fills FunctionID and bParamsPOD; routing fields keep their defaults.
	static FCrowdyFnInfo BuildFnInfo(UFunction* Fn);

	// Cached BuildFnInfo, keyed by UFunction (rebuilt every call in editor where
	// Live Coding can recycle UFunction addresses).
	static FCrowdyFnInfo GetFnInfo(UFunction* Fn);

	// Stable hash of the function's full signature (declaring class path + name +
	// ordered canonical parameter types).
	static int64 ComputeFunctionID(const UFunction* Fn);

	// Canonical, platform-stable spelling of a parameter type for the signature hash.
	static FString CanonicalParamType(const FProperty* Prop);

	// True when a parameter type can ride the RPC serializer: a primitive (bool,
	// integer, float, byte), an enum, a name/string/text, or a struct. Object and
	// class references, containers, and delegates have no stable wire form and are
	// rejected. Struct members are not inspected — a struct that itself holds an
	// unsupported member still passes.
	static bool IsSupportedParamType(const FProperty* Prop);

	// Describes why a function cannot be an RPC-style CrowdyEvent, or an empty string
	// when its signature is valid. A valid signature is one-way (no return value, no
	// output parameter) and carries only supported parameter types. The text names the
	// offending parameter so it can drop straight into a compile-log message.
	static FString DescribeSignatureProblem(const UFunction* Fn);

	// Conservative lower bound on the encoded channel-payload size for a reliable RPC: the fixed
	// header and identity plus the guaranteed bytes of fixed-width parameters. Variable-length
	// parameters (string, name, text, struct) can be empty, so they contribute nothing and the
	// result never overstates. Registration uses it to reject a reliable RPC that can never fit
	// the channel budget; the actual encoded size is still checked on every send.
	static int32 EstimateMinChannelPayloadBytes(const UFunction* Fn);

	// Serializes the input parameters held in Frame into OutBlob (version byte +
	// each input parameter in declaration order). OutParms, when supplied, is the VM's
	// out-parm record list (from a Blueprint event's live frame): a by-ref/out parameter
	// which includes a const-ref container, lives there, in the caller's storage, not inline
	// in Frame, so its value is read from the record instead of the locals offset. The C++
	// send path passes nullptr and reads every parameter inline.
	static void SerializeParams(const UFunction* Fn, const void* Frame, TArray<uint8>& OutBlob,
		FOutParmRec* OutParms = nullptr);

	// Rebuilds the input parameters from Blob into Frame. Frame must already be
	// alloca'd to ParmsSize and, for non-POD parameters, InitializeStruct'd.
	// Returns false (and dispatches nothing) on a version or size mismatch.
	static bool DeserializeParams(const UFunction* Fn, const TArray<uint8>& Blob, void* Frame);

	// Reconstructs a parameter frame from Call and invokes Fn on Target. This is the
	// receive path, reused by the router and by the in-process round-trip tests.
	static void ApplyCall(UObject* Target, UFunction* Fn, const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call);

	// Encodes a call into a channel payload: a [version][flags] reliability header followed by the
	// serialized FCrowdyRpcCall. Flags is reserved for the guaranteed-delivery layer and is zero
	// for coverage-only sends. Used by the reliable Multicast send path.
	static void EncodeChannelRpc(const FCrowdyRpcCall& Call, uint8 Flags, TArray<uint8>& OutPayload);

	// Reverses EncodeChannelRpc. Returns false on a version mismatch or truncated bytes — the
	// channel payload is untrusted, so a drifted or malformed peer drops cleanly here instead of
	// misparsing. Used by the channel receive path.
	static bool DecodeChannelRpc(const TArray<uint8>& Payload, FCrowdyRpcCall& OutCall, uint8& OutFlags);

	// Packs the routing identity (declaring ClassID + FunctionID) and the serialized
	// parameter bytes of Frame into a wire-ready call, without sending it. MarshalCall
	// builds Frame from typed arguments first; SerializeAndRoute sends the result. OutParms is
	// forwarded to SerializeParams for the Blueprint frame case (nullptr on the C++ path).
	static FCrowdyRpcCall BuildCall(const UFunction* Fn, const FCrowdyFnInfo& Info, const void* Frame,
		FOutParmRec* OutParms = nullptr);

	// Sends an already-marshalled call to the target entity over the Crowdy transport with
	// the given addressing, reusing the serialized parameter bytes (no re-serialization).
	// Shared by the send path and by the owner's re-announce on the receive side.
	static void RouteOverWire(UCrowdyEntitySubsystem* EntitySubsystem, const AActor* ContextActor,
		const FCrowdyRpcCall& Call, const FCrowdyFnInfo& Info, ECrowdyTarget Target);

	// True when the crowdy.rpc.trace console variable is set — gates the per-call
	// send/receive trace logging.
	static bool IsRpcTraceEnabled();

	// True when crowdy.rpc.reliable.trace (or the umbrella crowdy.rpc.trace) is set. Gates the
	// reliable channel-transport send/receive trace logging. Exported so the channel subsystem in
	// CrowdyServices can gate its own transport lines on the same flag.
	static bool IsReliableTraceEnabled();

	// True when the crowdy.rpc.loopback console variable is set. In loopback mode a sent
	// replicated event is also delivered to this client's own receive path, so the full
	// serialize/resolve/dispatch round-trip can be exercised without a second client.
	static bool IsLoopbackEnabled();

	// Send-side entry for a Blueprint replicated event, invoked by the gate the compiler
	// injects at the head of a marked event. Returns true when the call originated here and
	// was routed over the network — the local body must then be skipped — and false when this
	// invocation is the local replay of a received call, so the body must run. ParamFrame is
	// the event's live parameter frame (the executing function's Stack.Locals); the C++ path
	// uses SendChecked instead and never reaches here.
	static bool DispatchOrReplayBlueprintCall(UObject* Self, UFunction* EventFn, void* ParamFrame,
		FOutParmRec* OutParms = nullptr);

	// True when a parameter is a genuine output excluded from the parameter wire: the return
	// value, or a non-const output reference. A const reference is tagged CPF_OutParm by
	// reflection (UHT marks container const-refs CPF_OutParm | CPF_ConstParm) but cannot pass
	// data back to the caller, so it is an input that must be serialized. This is the single
	// predicate the serialize/deserialize walks, PlaceAll, and the signature validator share so
	// they never disagree about which parameters ride the wire.
	static bool IsTrueOutputParam(const FProperty* Prop)
	{
		return Prop->HasAnyPropertyFlags(CPF_ReturnParm)
			|| (Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm));
	}

private:

	// Resolves the target entity, applies the client-authoritative ownership model, and
	// either runs the call locally and announces it or delegates it to the owning client.
	// Returns true when the call was routed (so a Blueprint caller skips its local body) and
	// false when there is no Crowdy world to route through (so the body runs as a local fallback).
	static bool SerializeAndRoute(UObject* Obj, UFunction* Fn, const FCrowdyFnInfo& Info,
		FCrowdyRpcCall Call);

	// Encodes a Multicast call and publishes it over the named channel (empty = default session
	// channel), after checking it fits the channel payload budget (an oversize call is dropped
	// loudly, not truncated).
	static void RouteOverChannel(UCrowdyEntitySubsystem* EntitySubsystem, const FCrowdyRpcCall& Call,
		const UFunction* Fn, const FString& ChannelName);

	// Feeds an already-serialized call back into the local event router's receive path so a
	// single client can test the full round-trip (loopback mode). bLoopbackDelivering is held
	// for the duration so a replicated event called from the replayed body cannot start its own
	// loopback — without that guard the receive path would feed itself endlessly.
	static void DeliverLoopback(UWorld* World, const FCrowdyRpcCall& Call);

	// A received call is replayed by invoking the receiver through ProcessEvent, where the
	// injected gate would otherwise re-dispatch it. ApplyCall arms these for the exact
	// (object, function) it is about to replay, and the first matching gate consumes them and
	// runs the body. Scoped to the pair so a recursive call, or the same event on a different
	// entity from within a replayed body, still originates. Game-thread only.
	static UObject* ReplayObject;
	static UFunction* ReplayFunction;

	// Set while a loopback-delivered call is running through the receive path, so the nested
	// send a replayed body might issue does not loop back again. Game-thread only.
	static bool bLoopbackDelivering;

	// Copies one typed value into its reflected slot in Frame.
	template <typename ValueType>
	static void PlaceOne(FProperty* Prop, uint8* Frame, ValueType&& Value)
	{
		if (Prop)
		{
			Prop->CopyCompleteValue(Prop->ContainerPtrToValuePtr<void>(Frame), &Value);
		}
	}

	// Gathers the input parameter properties in declaration order and copies each
	// tuple element into its slot by reflected offset (never C++ struct packing).
	template <typename TupleType, std::size_t... Indices>
	static void PlaceAll(UFunction* Fn, uint8* Frame, std::index_sequence<Indices...>, TupleType&& Values)
	{
		constexpr int32 NumParams = sizeof...(Indices);
		FProperty* Params[NumParams > 0 ? NumParams : 1] = {};

		int32 Found = 0;
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			if (IsTrueOutputParam(Prop))
			{
				continue;
			}
			if (Found < NumParams)
			{
				Params[Found] = Prop;
			}
			++Found;
		}

		( PlaceOne(Params[Indices], Frame, Values.template Get<Indices>()), ... );
	}
};
