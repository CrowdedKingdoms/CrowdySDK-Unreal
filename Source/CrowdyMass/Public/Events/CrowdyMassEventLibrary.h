#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "CrowdyMassEventLibrary.generated.h"

class UFunction;

/** Calls the CROWDY_EVENT Event declared on Class for the crowd row Target names: CROWDY_MASS_SEND_EVENT(Target, ALanternPlayer, WaveTorch, 1, 0, 1.0f). */
#define CROWDY_MASS_SEND_EVENT(Target, Class, Event, ...) \
	UCrowdyMassEventLibrary::SendEventToMassTarget((Target), TEXT(#Event "_Implementation"), &Class::Event##_Implementation, ##__VA_ARGS__)

/**
 * The runtime entries the two Crowdy Mass event nodes compile to: one to send a class-declared CrowdyEvent
 * to whichever real actor owns a Mass-rendered remote player, the same way calling the event on an actor
 * would, and one to receive the same event on a machine that holds that player as a row rather than as an
 * actor. Class and name travel as two literal pins rather than one function reference because both nodes
 * store the picked function as an FMemberReference (so a rename redirects it) and resolve it to this
 * class-plus-name pair only at compile time.
 *
 * The SEND entry has to be a CustomThunk. The stock UK2Node_CallFunction handler errors on an unconnected variadic
 * pin (an authored literal default on one of the event's own parameters is the ordinary case for this
 * node), so the node installs its own handling functor that emits one extra term per parameter of the
 * picked event, and this function reads them directly off the live VM frame rather than through
 * reflected UFUNCTION parameters.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassEventLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Sends EventName (declared on EventClass, or a base of it) to the remote player Target names.
	 * Every parameter of the picked event rides as an extra term after these three declared ones.
	 *
	 * The return value follows FCrowdyRPC::SendToTarget's own contract: false means only that there was
	 * nothing to route through at all (the target's world is gone); every other outcome, including a
	 * target that could not be resolved to a live entity and every refusal this function makes on its
	 * own account (an unresolved function, one that is not a CrowdyEvent, or a class mismatch), returns
	 * true, exactly as SendToTarget's own equivalent cases do. It is not a delivery receipt, the
	 * transport this send rides has none; a Blueprint author who wants to know whether their target is
	 * even a real, addressable remote player should check FCrowdyMassEntityTarget::IsSet first.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="Crowdy Mass|Events",
		meta=(BlueprintInternalUseOnly="true", Variadic))
	static bool CrowdyMassCallEventOnEntity(const FCrowdyMassEntityTarget& Target, UClass* EventClass, FName EventName,
		int64 ExpectedSignature);
	DECLARE_FUNCTION(execCrowdyMassCallEventOnEntity);

	/**
	 * C++ entry point for the same send, for a caller that already has a built parameter frame (tests, or
	 * code that already holds the UFunction it wants to call). Frame must already be laid out exactly as
	 * Fn's own input parameters expect, in declaration order (ProcessEvent order); a function with no
	 * parameters may pass a null frame. This does not take ownership of Frame and does not construct or
	 * destroy anything in it; the caller is responsible for both.
	 *
	 * Refuses (returns true, matching every other drop on this send path) when Fn is null, is not a
	 * CrowdyEvent, the target's entity could not be resolved (a stale handle, a missing identity
	 * fragment, or no routable id), or the target's locally recorded class does not derive Fn's
	 * declaring class. Returns false only when the target's world has gone, since only then is there
	 * nothing at all to route the call through.
	 */
	static bool SendEventToMassTarget(const FCrowdyMassEntityTarget& Target, UFunction* Fn, const void* Frame);

	/**
	 * The same send with typed arguments, checked against the event's parameters at compile time and placed by
	 * their reflected offsets. ImplName must name MemberFn; CROWDY_MASS_SEND_EVENT writes both from one name.
	 */
	template <typename C, typename... TParams, typename... TArgs>
	static bool SendEventToMassTarget(const FCrowdyMassEntityTarget& Target, const TCHAR* ImplName,
		void (C::*MemberFn)(TParams...), TArgs&&... Args)
	{
		// An unresolved name is refused like every other drop on this path; ResolveFunction has already said why.
		UFunction* Fn = FCrowdyRPC::ResolveFunction(C::StaticClass(), ImplName);
		if (!Fn)
		{
			return true;
		}
		return FCrowdyRPC::WithCheckedFrame(Fn, MemberFn,
			[&Target, Fn](const void* Frame) { return SendEventToMassTarget(Target, Fn, Frame); },
			Forward<TArgs>(Args)...);
	}

	/**
	 * Runtime entry the "Receive Crowdy Event on Entity" node's Bind pin compiles to: routes every arriving
	 * EventName (declared on EventClass) for a crowd entity to HandlerFunctionName on HandlerObject.
	 *
	 * HandlerFunctionName is the intermediate event the node compiles the graph's own output pins into, so
	 * this is not a name an author types. It takes the entity the call arrived for as its first parameter,
	 * then whatever of the event's own parameters the graph exposed, matched by name.
	 *
	 * The world the binding is made in is the one HandlerObject belongs to, and the binding is dropped with
	 * it. An object with no world cannot bind, which is what a graph on an object outside a level is.
	 *
	 * False means nothing was bound, and always says why in the log. Binding the same object and function
	 * name again is the same binding rather than a second one, and answers true.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Events", meta=(BlueprintInternalUseOnly="true"))
	static bool CrowdyMassBindEventOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EventClass,
		FName EventName, int64 ExpectedSignature);

	/**
	 * The reverse: stops routing EventName to HandlerFunctionName on HandlerObject. False when there was no
	 * such binding to remove, which includes a graph that unbinds before it ever binds.
	 *
	 * No signature is passed, because removing a binding is a question of identity rather than of shape: a
	 * binding made against a signature that has since changed is exactly the one an author most needs to be
	 * able to take back off.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Events", meta=(BlueprintInternalUseOnly="true"))
	static bool CrowdyMassUnbindEventOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EventClass,
		FName EventName);

	/**
	 * The event a bind names, or null when it cannot be bound: no such function, one that is not a
	 * CrowdyEvent, or one whose signature is no longer the one the binding graph was compiled against.
	 *
	 * That last case is the reason this is a step of its own. A receive node grows one output pin per
	 * parameter of the event as it stood when the graph was compiled, and the binding fills those pins by
	 * name; if the event has been reshaped since, some of them would simply sit at their defaults for the
	 * rest of the session with nothing to show that they had stopped being filled. Refusing the bind
	 * reports it once, at the point the author can act on it, rather than never.
	 *
	 * Every refusal is logged. Nothing here needs a world, so it is also the whole of what a test has to
	 * drive to tell a stale graph from a current one.
	 */
	static UFunction* ResolveBoundEvent(UClass* EventClass, FName EventName, int64 ExpectedSignature);

	/**
	 * Forgets which refusals have already been logged, so the next one is logged again.
	 *
	 * The throttle behind the refusal warnings is process-wide and time-based, which makes any test that
	 * counts those warnings depend on what ran before it and how recently. A test that wants to assert a
	 * refusal was reported calls this first.
	 */
	static void ResetRefusalLogThrottleForTest();
};
