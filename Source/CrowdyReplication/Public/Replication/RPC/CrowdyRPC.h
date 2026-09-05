#pragma once

#include "CoreMinimal.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/RPC/FCrowdyFnInfo.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "UObject/Class.h"        // UFunction
#include "UObject/UnrealType.h"   // FProperty, TFieldIterator, property flags
#include "Templates/Function.h"
#include "Templates/Tuple.h"
#include "Templates/UnrealTemplate.h"
#include <type_traits>           // std::is_convertible_v
#include <utility>               // std::index_sequence, std::index_sequence_for

class AActor;
class ICrowdyEventSource;
class UCrowdyEntitySubsystem;
class UWorld;
struct FCrowdyEventParams;
struct FOutParmRec;

DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyRPC, Log, All);

// Format version prefixed to every FCrowdyRpcCall::ParamBlob. Bump whenever the
// on-wire parameter encoding changes so stale peers drop instead of misparsing.
// v2: TSet/TMap parameters now ride an explicit bounded [count][elements] snapshot
// instead of the engine's delta SerializeItem format (a forged mid-stream element
// count in that format drove an unbounded allocation). A mixed-version peer set is
// unsupported since every client runs one build, so any drift drops cleanly at the
// version check below rather than risking a mis-decode of the parameters after a set/map.
inline constexpr uint8 CrowdyRpcParamBlobVersion = 2;

// A reliable RPC rides a channel message whose payload is a small reliability header
// followed by the serialized FCrowdyRpcCall. The header is a format version plus a flag
// byte; the flags are reserved for a later guaranteed-delivery layer (per-message id,
// dedup, ack) and are zero for coverage-only sending. Bump the version if the layout changes.
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

// How an RPC routes after the client-authoritative ownership model is applied, plus whether the body
// also runs locally now. Pure policy output of FCrowdyRPC::DecideRoute.
enum class ECrowdyRpcRoute : uint8
{
	None,               // no network announce (the local authority runs it and does not re-announce)
	SpatialBroadcast,   // announce to everyone in range over the spatial transport (decay-thinned)
	Channel,            // announce over the reliable session channel
	SingleActorToOwner, // targeted single-actor send to the entity's owner
	SingleActorToHost,  // targeted single-actor send to the host's avatar entity
	Reject,             // invalid config (SpatialMulticast on a non-spatial participant): drop with an error
};

struct FCrowdyRpcRouteDecision
{
	bool bRunLocally = false;
	ECrowdyRpcRoute Route = ECrowdyRpcRoute::None;
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

	// Declares that this event's parameters describe a one-shot action: an attack swing, a flinch, an
	// emote. It says nothing about how the event behaves on a receiver holding the entity as a real
	// object, where the author's own body runs as always. It exists for the receivers that hold the
	// entity as DATA and have no body to run: they have no way to know that "some int the game called
	// ActionId" means an animation, and guessing from a parameter name would be this layer deciding what
	// a game's events mean.
	//
	// The SDK only carries the declaration. What is done with it belongs to whatever renders the entity.
	inline const TCHAR* Action    = TEXT("CrowdyAction");

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
 * per-function recipient/decay/distance. ApplyCall is the receiver side: it rebuilds
 * the frame from the bytes and invokes the receiver via ProcessEvent. The
 * client-authoritative ownership model is layered onto SerializeAndRoute in a later
 * phase; the marshal/serialize/route and unmarshal/invoke halves live here.
 */
class CROWDYREPLICATION_API FCrowdyRPC
{
public:

	/**
	 * Sets the per-world entity subsystem the object-reference codec resolves against, for the
	 * duration of an encoding or decode, and restores the previous value when it goes out of scope.
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
	 * Marks one (object, function) invocation as the replay of a RECEIVED call, and restores whatever was
	 * armed before when it goes out of scope.
	 *
	 * A Blueprint replicated event carries a dispatch gate spliced in at the top of its body, and that gate
	 * routes the call over the network unless this scope names the exact object and function it is running
	 * for. So anything that invokes a received call through ProcessEvent must hold one: without it every
	 * client that received the call announces it again, from a receiver rather than from an originator.
	 *
	 * Scoped to the pair rather than to a flag, so a recursive call, or the same event on another entity
	 * from inside a replayed body, still originates as it should. Game-thread only, like the rest of the
	 * scoped serialization state here.
	 */
	struct CROWDYREPLICATION_API FScopedReplay
	{
		FScopedReplay(UObject* Object, UFunction* Function);
		~FScopedReplay();

		FScopedReplay(const FScopedReplay&) = delete;
		FScopedReplay& operator=(const FScopedReplay&) = delete;

	private:
		UObject* PreviousObject;
		UFunction* PreviousFunction;
	};

	/**
	 * Whether this function is one the compiler extension should have spliced a dispatch gate into: a
	 * Blueprint-declared event marked "Crowdy Replicates". A C++ CROWDY_EVENT is NOT one of these, because
	 * its send goes through the macro's thunk and it never carries a gate.
	 */
	static bool IsBlueprintReplicatedEvent(const UFunction* Function);

	/**
	 * Whether Function's compiled bytecode actually calls the dispatch gate.
	 *
	 * A Blueprint replicated event that lost its gate is the worst failure this system has: calling it runs
	 * the body locally and announces NOTHING, with no error anywhere, because the send path is simply never
	 * entered. It looks from the outside exactly like a transport that dropped the call.
	 *
	 * Read from UStruct::ScriptAndPropertyObjectReferences, the list of objects a function's bytecode
	 * references, rather than by disassembling the script: the gate is a function call, so if the gate is in
	 * the body its UFunction is in that list.
	 *
	 * False for a function with no script at all, which is every native one, so ask IsBlueprintReplicatedEvent
	 * first rather than treating this as a verdict on its own.
	 */
	static bool CarriesDispatchGate(const UFunction* Function);

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
	 * automation can hand it to ApplyCall to exercise the receiver path in-process.
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

	/**
	 * Sends a CrowdyEvent to an entity named by Target, with no calling instance involved.
	 *
	 * The ordinary send path needs a live UObject to read the entity identity and the world position
	 * from. An entity this client holds only as rendering data has no such object, so the caller
	 * resolves the identity, the position and the ownership from wherever that entity's state lives and
	 * hands them in as Target. Everything after that is the ordinary send: the call is packed by the
	 * same BuildCall and keyed by the function's DECLARING class, so it resolves against whatever
	 * instance the receiving client holds for that entity, and it routes by the function's own
	 * CrowdyRecipient, all four of them.
	 *
	 * Fn is the receiver function and Frame its parameter frame, laid out exactly as ProcessEvent
	 * expects one: MarshalCall builds a frame from typed arguments, and a Blueprint thunk already has
	 * one. A function that takes no parameters may pass a null frame. OutParms is the VM's out-parm
	 * record list when Frame came from a live Blueprint frame, and null from C++.
	 *
	 * Where this client already holds a registration for the target entity, that record's owner, role
	 * and (when it holds an actor) position are preferred over the matching fields of Target, so the
	 * routing never contradicts what the rest of this client believes about the entity.
	 *
	 * The call goes out as this client: the local player is stamped as the sender and no sending
	 * identity is ever read from Target. Aiming a call at an entity another client owns is what this
	 * entry is for, not an error - the RPC plane is client-authoritative by design and the owner's own
	 * instance is what runs the body. State that must not be forgeable belongs in a Game Model instead.
	 *
	 * Returns false ONLY when there was no transport to route through, so a caller that has a local body
	 * may run it as a fallback. Every other outcome returns true, including every drop: a drop means the
	 * send itself was invalid, and running the body here instead would be the wrong answer. Each drop is
	 * logged with its reason, rate-limited per function where its cause can persist across frames.
	 *
	 * Game thread only.
	 */
	static bool SendToTarget(UWorld* World, const FCrowdyRpcTarget& Target, UFunction* Fn,
		const void* Frame, FOutParmRec* OutParms = nullptr);

	// Reflection helpers (defined in CrowdyRPC.cpp)

	// Finds the receiver UFunction by name on Class (searching base classes too).
	// Logs and returns null when missing.
	static UFunction* ResolveFunction(UClass* Class, const TCHAR* ImplName);

	// Builds the routing/serialization metadata for a receiver function: fills
	// FunctionID and bParamsPOD; routing fields keep their defaults until baked
	// metadata overrides them.
	static FCrowdyFnInfo BuildFnInfo(UFunction* Fn);

	/**
	 * Cached BuildFnInfo. Every send and every receive needs a function's routing info, and building it
	 * walks the parameter list twice before reading the routing metadata, so the result is kept per
	 * function. Entries are keyed by the function's object identity (slot plus serial number), so an
	 * address reused after garbage collection, and the fresh functions a Blueprint recompile produces,
	 * both miss cleanly rather than serving another function's routing.
	 */
	static FCrowdyFnInfo GetFnInfo(UFunction* Fn);

	/**
	 * Drops every cached FCrowdyFnInfo. Needed after reflection data is rebuilt in place, which can change
	 * a function's parameter list (and so its signature hash) without changing the function object.
	 */
	static void InvalidateFnInfoCache();

	// Number of functions with cached routing info. Diagnostics and tests only.
	static int32 NumCachedFnInfo();

	// Binds the reload-complete delegate that flushes the cache. Call once when the module starts.
	static void InstallFnInfoCacheInvalidation();

	// Removes that binding and flushes the cache. Call when the module shuts down.
	static void RemoveFnInfoCacheInvalidation();

	// Stable hash of the function's full signature (declaring class path + name +
	// ordered canonical parameter types).
	static int64 ComputeFunctionID(const UFunction* Fn);

	// Canonical, platform-stable spelling of a parameter type for the signature hash.
	static FString CanonicalParamType(const FProperty* Prop);

	// True when a parameter type can ride the RPC serializer: a primitive (bool,
	// integer, float, byte), an enum, a name/string/text, or a struct. Object and
	// class references, containers, and delegates have no stable wire form and are
	// rejected. Struct members are not inspected a struct that itself holds an
	// unsupported member still passes.
	static bool IsSupportedParamType(const FProperty* Prop);

	// Describes why a function cannot be an RPC-style CrowdyEvent or an empty string
	// when its signature is valid. A valid signature is one-way (no return value, no
	// output parameter) and carries only supported parameter types. The text names the
	// offending parameter so it can drop straight into a compile-log message.
	static FString DescribeSignatureProblem(const UFunction* Fn);

	// True when a UStruct transitively holds a TArray/TSet/TMap member, at any struct-nesting depth.
	// A container reached through a struct is written by the struct's own SerializeItem, which reads an
	// untrusted element count and allocates before the short read is caught (the bounded reader's cap
	// only guards the FString/FName path). The RPC signature validator uses this to reject a container
	// buried inside a struct parameter, and the CrowdyState rep-layout builder shares it for the same
	// reason. Depth-guarded against absurd nesting (a struct cannot contain itself by value).
	static bool StructTransitivelyContainsContainer(const UStruct* Struct, int32 Depth = 0);

	// Conservative lower bound on the encoded channel-payload size for a reliable RPC: the fixed
	// header and identity plus the guaranteed bytes of fixed-width parameters. Variable-length
	// parameters (string, name, text, struct) can be empty, so they contribute nothing and the
	// result never overstates. Registration uses it to reject a reliable RPC that can never fit
	// the channel budget; the actual encoded size is still checked on every send.
	static int32 EstimateMinChannelPayloadBytes(const UFunction* Fn);

	// Pure ownership-model policy. bNonSpatial is true for a subsystem participant (no world location);
	// bEntityValid/bWeOwnEntity/bWeAreHost are the local authority facts. No side effects, so every
	// (recipient x participant-kind x authority) combination is table-testable. The actor rows (bNonSpatial
	// false) reproduce SerializeAndRoute's shipped behavior exactly; a non-spatial SpatialMulticast is rejected.
	static FCrowdyRpcRouteDecision DecideRoute(ECrowdyEventRecipient Recipient, bool bNonSpatial,
		bool bEntityValid, bool bWeOwnEntity, bool bWeAreHost);

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

	/**
	 * Reconstructs the same parameter frame ApplyCall builds and hands it to OnDecoded as values, without
	 * invoking anything. For a recipient that holds the call's target as data rather than as an object,
	 * where there is no instance of the declaring class to run a body on.
	 *
	 * Returns false, having run nothing, when the blob does not decode against Fn's current signature
	 * (a drifted build, or forged bytes). The frame is constructed and destroyed around OnDecoded, so
	 * nothing it was handed may be kept past the call.
	 */
	static bool DecodeCall(UFunction* Fn, const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call,
		TFunctionRef<void(const FCrowdyEventParams& Params)> OnDecoded);

	// Encodes a call into a channel payload: a [version][flags] reliability header followed by the
	// serialized FCrowdyRpcCall. Flags is reserved for the guaranteed-delivery layer and is zero
	// for coverage-only sends. Used by the reliable Multicast send path.
	static void EncodeChannelRpc(const FCrowdyRpcCall& Call, uint8 Flags, TArray<uint8>& OutPayload);

	// Reverses EncodeChannelRpc. Returns false on a version mismatch or truncated bytes the
	// channel payload is untrusted, so a drifted or malformed peer drops cleanly here instead of
	// misparsing. Used by the channel receive path.
	static bool DecodeChannelRpc(const TArray<uint8>& Payload, FCrowdyRpcCall& OutCall, uint8& OutFlags);

	// Packs the routing identity (declaring ClassID + FunctionID) and the serialized
	// parameter bytes of Frame into a wire-ready call, without sending it. MarshalCall
	// builds Frame from typed arguments first; SerializeAndRoute sends the result. OutParms is
	// forwarded to SerializeParams for the Blueprint frame case (nullptr on the C++ path).
	static FCrowdyRpcCall BuildCall(const UFunction* Fn, const FCrowdyFnInfo& Info, const void* Frame,
		FOutParmRec* OutParms = nullptr);

	// Sends an already-marshaled call to the target entity over the Crowdy transport with
	// the given addressing, reusing the serialized parameter bytes (no re-serialization).
	// Shared by the sent path and by the owner's re-announcement on the receiver side.
	static void RouteOverWire(UCrowdyEntitySubsystem* EntitySubsystem, const AActor* ContextActor,
		const FCrowdyRpcCall& Call, const FCrowdyFnInfo& Info, ECrowdyTarget Target);

	// True when the crowdy.rpc.trace console variable is set gates the per-call
	// send/receive trace logging.
	static bool IsRpcTraceEnabled();

	// True when crowdy.rpc.reliable.trace (or the umbrella crowdy.rpc.trace) is set. Gates the
	// reliable channel-transport send/receive trace logging. Exported so the channel subsystem in
	// CrowdyServices can gate its own transport lines on the same flag.
	static bool IsReliableTraceEnabled();

	// True when the crowdy.rpc.loopback console variable is set. In loopback mode a sent
	// replicated event is also delivered to this client's own receiver path, so the full
	// serialize/resolve/dispatch round-trip can be exercised without a second client.
	static bool IsLoopbackEnabled();

	// Send-side entry for a Blueprint replicated event, invoked by the gate the compiler
	// injects at the head of a marked event. Returns true when the call originated here and
	// was routed over the network the local body must then be skipped and false when this
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

	/**
	 * Send path for a sender with no owning actor that implements ICrowdyEventSource: the entity id, the
	 * sending identity and the world position all come from the interface rather than from an actor.
	 * All four recipients route. Owner-only and host-only travel by the single-actor transport, which
	 * addresses a registered id plus a chunk rather than an actor, so a source with no actor can use it.
	 * A position is read from the source where it has one, and only the routes that address a region
	 * need it; one of those with no position to announce from drops, logged with its reason.
	 *
	 * Returns true in every case, including the drops: a caller that ran no local body must not then run
	 * one as a fallback, since the reason for each drop is that the send was invalid, not that there was
	 * no transport.
	 */
	static bool RouteFromEventSource(UObject* Obj, ICrowdyEventSource* Source, UFunction* Fn,
		const FCrowdyFnInfo& Info, FCrowdyRpcCall Call, UCrowdyEntitySubsystem* EntitySubsystem,
		UWorld* World);

	/**
	 * Applies the ownership model to a call already addressed to Target and dispatches it: runs the body
	 * on this client where the model says this client also runs it, and sends over whichever transport
	 * the recipient names. Every address comes from the target or from the host's own registration, so a
	 * caller cannot aim a message at a chunk its destination is not standing in. Where this client holds
	 * a record for the target, that record's owner, role and actor position are preferred over Target's.
	 *
	 * Call must already carry its EntityID and SenderID; this only routes.
	 *
	 * LocalInstance is the object to run the body on when the model says to run it here. A caller that
	 * already holds the instance passes it. Passing null asks for the instance holding Target's identity
	 * on this client to be looked up instead, and the lookup happens only if the body actually runs here.
	 *
	 * Returns false ONLY when there is no transport to route through, matching SerializeAndRoute: every
	 * drop returns true, so a caller never runs a body as a fallback for a send that was refused rather
	 * than unavailable. Game thread only.
	 */
	static bool RouteToTarget(UWorld* World, UCrowdyEntitySubsystem* EntitySubsystem,
		const FCrowdyRpcTarget& Target, UObject* LocalInstance, UFunction* Fn,
		const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call);

	// Finds the object on this client that would receive Fn for the entity NetID names: the registered
	// participant itself, or the component on it that carries the function's declaring class. Null when
	// this client holds no such object, which is the normal case for an entity it only renders.
	static UObject* ResolveLocalTargetReceiver(UCrowdyEntitySubsystem* EntitySubsystem, const FGuid& NetID,
		const UFunction* Fn);

	// Feeds an already-serialized call back into the local event router's receive path so a
	// single client can test the full round-trip (loopback mode). bLoopbackDelivering is held
	// for the duration, so a replicated event called from the replayed body cannot start its own
	// loopback without that guard the receive path would feed itself endlessly.
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
