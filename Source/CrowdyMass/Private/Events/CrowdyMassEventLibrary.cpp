#include "Events/CrowdyMassEventLibrary.h"

#include "CrowdyMassLog.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Mass/CrowdyMassEntity.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "UObject/ObjectKey.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	// A refusal repeats for as long as its cause lasts (an entity whose class has not arrived yet, one
	// whose class genuinely differs, or a stale event reference), so each function gets at most one line
	// per interval rather than one per call. Mirrors FCrowdyRPC's own send-side drop throttle
	// (CrowdyRPC.cpp, ShouldLogRpcDrop) at the same granularity and the same interval.
	constexpr double CrowdyMassEventRefuseWarnIntervalSeconds = 5.0;

	TMap<int64, double>& RefusalLogTimes()
	{
		static TMap<int64, double> LastLoggedSeconds;
		return LastLoggedSeconds;
	}

	/**
	 * Whether this machine has to hold a stand-in for the target before the event is sent.
	 *
	 * A sender never receives its own broadcast, so a body that runs on a crowd stand-in has to be run
	 * here as well or the one machine that does not see the action is the one that asked for it. Running
	 * it locally resolves a participant registered under the entity's id, and for an entity held as a row
	 * asking for its stand-in is exactly what creates one.
	 *
	 * Two conditions, and each excludes a case the other does not. The body must be one a stand-in can
	 * host, which means declared on the stand-in base or below: an event declared on the entity's own
	 * Actor class runs on that Actor and nothing here has to be minted for it. And only a broadcast is
	 * enrolled, because those are the recipients that always run on the sender; an owner-only or host-only
	 * call this machine will not run locally needs no receiver here.
	 *
	 * It lives on the shared send path rather than at one call site so that both ways of sending an event
	 * to an entity, the node and the library entry, behave the same. Two enrolment rules is how one
	 * feature ends up with two entry points and different visible behaviour.
	 */
	bool NeedsLocalStandIn(UFunction* Fn)
	{
		const UClass* DeclaringClass = Fn ? Fn->GetOwnerClass() : nullptr;
		if (!DeclaringClass || !DeclaringClass->IsChildOf(UCrowdyMassEntity::StaticClass()))
		{
			return false;
		}

		const ECrowdyEventRecipient Recipient = FCrowdyRPC::GetFnInfo(Fn).Recipient;
		return Recipient == ECrowdyEventRecipient::Multicast
			|| Recipient == ECrowdyEventRecipient::SpatialMulticast;
	}

	bool ShouldLogEventRefusal(int64 FunctionID)
	{
		TMap<int64, double>& LastLoggedSeconds = RefusalLogTimes();

		const double Now = FPlatformTime::Seconds();
		if (const double* Last = LastLoggedSeconds.Find(FunctionID))
		{
			if (Now - *Last < CrowdyMassEventRefuseWarnIntervalSeconds)
			{
				return false;
			}
		}

		LastLoggedSeconds.Add(FunctionID, Now);
		return true;
	}

	void LogRefusal(const UFunction* Fn, const FString& Reason)
	{
		if (!ShouldLogEventRefusal(FCrowdyRPC::ComputeFunctionID(Fn)))
		{
			return;
		}
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEventLibrary] '%s' refused: %s"), *Fn->GetName(), *Reason);
	}

	// The Mass subsystem of the world an object belongs to, or null with a line saying which half was
	// missing. A binding is world-scoped, and an object outside a level has no world to scope it to.
	// bReport is false for an unbind: a binding cannot exist without the subsystem, so its absence is not worth a line.
	UCrowdyMassEntitySubsystem* FindMassSubsystemForBinding(const UObject* HandlerObject, const bool bReport)
	{
		UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(HandlerObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		if (!World)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEventLibrary] '%s' belongs to no world, so there is nothing to bind a crowd event on."),
				*GetNameSafe(HandlerObject));
			return nullptr;
		}

		UCrowdyMassEntitySubsystem* MassSubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
		if (MassSubsystem || !bReport)
		{
			return MassSubsystem;
		}

		static TSet<FObjectKey> WorldsToldNoCrowdRows;
		bool bAlreadyTold = false;
		WorldsToldNoCrowdRows.Add(FObjectKey(World), &bAlreadyTold);
		UE_CLOG(!bAlreadyTold, LogCrowdyMass, Log,
			TEXT("[CrowdyMassEventLibrary] World '%s' has no Crowdy Mass entity subsystem, which exists only on maps whose profile selects the Mass backend, so crowd events never arrive here."),
			*World->GetName());

		return nullptr;
	}

	FString DescribeResolveFailure(CrowdyMassTarget::EResolveResult Result)
	{
		switch (Result)
		{
		case CrowdyMassTarget::EResolveResult::NoWorld:
			return TEXT("the target's world has gone, or this world holds no Mass entity manager.");
		case CrowdyMassTarget::EResolveResult::InvalidEntity:
			return TEXT("the entity handle does not name a live entity right now (despawned, or stale).");
		case CrowdyMassTarget::EResolveResult::MissingIdentity:
			return TEXT("the entity carries no identity fragment yet.");
		case CrowdyMassTarget::EResolveResult::Unroutable:
			return TEXT("the entity's identity fragment carries no valid id.");
		default:
			return TEXT("the target could not be resolved.");
		}
	}

	// Cleans up every CPF_Parm input property the thunk constructed into Frame, on every exit path,
	// including the ones a refused send takes. Frame's own storage is alloca'd by the thunk and needs no
	// freeing; only the properties constructed into it do.
	struct FScopedEventParamFrame
	{
		const UFunction* Fn = nullptr;
		void* Frame = nullptr;

		~FScopedEventParamFrame()
		{
			if (!Fn || !Frame)
			{
				return;
			}
			for (TFieldIterator<FProperty> It(Fn); It; ++It)
			{
				FProperty* Prop = *It;
				if (Prop->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(Prop))
				{
					Prop->DestroyValue_InContainer(Frame);
				}
			}
		}
	};
}

void UCrowdyMassEventLibrary::ResetRefusalLogThrottleForTest()
{
	RefusalLogTimes().Reset();
}

bool UCrowdyMassEventLibrary::SendEventToMassTarget(const FCrowdyMassEntityTarget& Target, UFunction* Fn, const void* Frame)
{
	// Resolving the target reads the Mass entity manager and the class-check reads a world subsystem,
	// both game-thread only, mirroring the same guard at the top of FCrowdyRPC::SendToTarget below.
	if (!ensure(IsInGameThread()))
	{
		return true;
	}

	if (!Fn)
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEventLibrary] SendEventToMassTarget: no function to call."));
		return true;
	}

	if (!UCrowdyBakedRegistry::IsEventHandlerFunction(Fn))
	{
		LogRefusal(Fn, TEXT("it is not a CrowdyEvent (missing meta=(CrowdyEvent)), so there is no receiver to run it."));
		return true;
	}

	FCrowdyRpcTarget RpcTarget;
	const CrowdyMassTarget::EResolveResult ResolveResult = CrowdyMassTarget::ResolveRpcTarget(Target, RpcTarget);
	if (ResolveResult == CrowdyMassTarget::EResolveResult::NoWorld)
	{
		// Mirrors FCrowdyRPC::SendToTarget's own "!World" case: there is nothing to route through at
		// all, not even a refusal to route. A caller with a local body would fall back to running it
		// here; this send has no such body, but the same "nothing to route through" meaning is what
		// this false carries back to a Blueprint caller checking the return value.
		return false;
	}
	if (ResolveResult != CrowdyMassTarget::EResolveResult::Resolved)
	{
		// Every other resolve failure names a target the transport genuinely exists to route through,
		// just not one addressable right now. SendToTarget treats its own equivalent case (a target
		// with no routable NetID) as a drop rather than a missing transport, so this does too.
		LogRefusal(Fn, DescribeResolveFailure(ResolveResult));
		return true;
	}

	UWorld* World = Target.World.Get();

	// An AUTHORING check, not a security boundary: the recorded class traces back to the ClassID the
	// entity's own sender put on the update that announced it, so a peer chooses what its entity claims to
	// be. It catches a graph aiming an event at the wrong kind of entity. Nothing mechanical may key off
	// it; anything that must not be forgeable has to be decided by the server.
	UCrowdyMassEntitySubsystem* MassSubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	const UClass* RecordedClass = MassSubsystem ? MassSubsystem->GetEntityClass(RpcTarget.NetID) : nullptr;

	const CrowdyMassTarget::EClassCheckResult ClassCheck = CrowdyMassTarget::CheckEventClass(Fn, RecordedClass);
	if (ClassCheck == CrowdyMassTarget::EClassCheckResult::ClassUnknown)
	{
		LogRefusal(Fn, TEXT("this client has not yet learned the entity's class; not sending until it does."));
		return true;
	}
	if (ClassCheck == CrowdyMassTarget::EClassCheckResult::ClassMismatch)
	{
		LogRefusal(Fn, FString::Printf(
			TEXT("its declaring class '%s' is not '%s' or a base of it."),
			*GetNameSafe(Fn->GetOwnerClass()), *GetNameSafe(RecordedClass)));
		return true;
	}

	// Cached per entity, so this is one mint and a map lookup after. Null means either that the tracker
	// does not hold the id, which the resolve above has already ruled out, or that the project's avatar
	// class cannot host this body, which GetOrCreateAvatar reports once by itself; neither is a reason to
	// abandon a send every other machine will apply.
	if (MassSubsystem && NeedsLocalStandIn(Fn))
	{
		MassSubsystem->GetOrCreateAvatar(RpcTarget.NetID, Fn->GetOwnerClass());
	}

	return FCrowdyRPC::SendToTarget(World, RpcTarget, Fn, Frame);
}

UFunction* UCrowdyMassEventLibrary::ResolveBoundEvent(UClass* EventClass, const FName EventName,
	const int64 ExpectedSignature)
{
	// ResolveFunction already logs when it cannot find one.
	UFunction* Fn = FCrowdyRPC::ResolveFunction(EventClass, *EventName.ToString());
	if (!Fn)
	{
		return nullptr;
	}

	if (!UCrowdyBakedRegistry::IsEventHandlerFunction(Fn))
	{
		LogRefusal(Fn, TEXT("it is not a CrowdyEvent (missing meta=(CrowdyEvent)), so nothing will ever be delivered for it."));
		return nullptr;
	}

	if (FCrowdyRPC::ComputeFunctionID(Fn) != ExpectedSignature)
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassEventLibrary] '%s::%s' does not match the signature this graph was compiled against. ")
			TEXT("Recompile the binding Blueprint. Not binding, rather than delivering to pins grown for a different signature. ")
			TEXT("(graph expected %lld, resolved %lld from owner '%s')"),
			*GetNameSafe(EventClass), *EventName.ToString(),
			ExpectedSignature, FCrowdyRPC::ComputeFunctionID(Fn), *GetPathNameSafe(Fn->GetOwnerClass()));
		return nullptr;
	}

	return Fn;
}

bool UCrowdyMassEventLibrary::CrowdyMassBindEventOnEntity(UObject* HandlerObject, const FName HandlerFunctionName,
	UClass* EventClass, const FName EventName, const int64 ExpectedSignature)
{
	// Registering reads a world subsystem, exactly like the send path above.
	if (!ensure(IsInGameThread()))
	{
		return false;
	}

	if (!::IsValid(HandlerObject))
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEventLibrary] Bind refused: there is no object to run the handler on."));
		return false;
	}

	UFunction* Fn = ResolveBoundEvent(EventClass, EventName, ExpectedSignature);
	if (!Fn)
	{
		return false;
	}

	UCrowdyMassEntitySubsystem* MassSubsystem = FindMassSubsystemForBinding(HandlerObject, true);
	if (!MassSubsystem)
	{
		return false;
	}

	return MassSubsystem->RegisterObjectEventHandler(Fn, HandlerObject, HandlerFunctionName).IsValid();
}

bool UCrowdyMassEventLibrary::CrowdyMassUnbindEventOnEntity(UObject* HandlerObject, const FName HandlerFunctionName,
	UClass* EventClass, const FName EventName)
{
	if (!ensure(IsInGameThread()))
	{
		return false;
	}

	if (!HandlerObject)
	{
		return false;
	}

	// Resolved WITHOUT the signature check the bind makes. An unbind names a registration that already
	// exists, and refusing it because the event has been reshaped since would leave exactly the stale
	// binding an author is trying to remove in place.
	UFunction* Fn = FCrowdyRPC::ResolveFunction(EventClass, *EventName.ToString());
	if (!Fn)
	{
		return false;
	}

	UCrowdyMassEntitySubsystem* MassSubsystem = FindMassSubsystemForBinding(HandlerObject, false);
	if (!MassSubsystem)
	{
		return false;
	}

	return MassSubsystem->UnregisterObjectEventHandler(Fn, HandlerObject, HandlerFunctionName);
}

DEFINE_FUNCTION(UCrowdyMassEventLibrary::execCrowdyMassCallEventOnEntity)
{
	P_GET_STRUCT_REF(FCrowdyMassEntityTarget, Z_Target);
	P_GET_OBJECT(UClass, Z_EventClass);
	P_GET_PROPERTY(FNameProperty, Z_EventName);
	P_GET_PROPERTY(FInt64Property, Z_ExpectedSignature);

	// The picked function decides both what follows on the stack (one term per input parameter it
	// declares, in declaration order, appended by the node's own handling functor) and how big a frame
	// to build for it, so it has to be resolved before either the stack or a frame can be touched.
	// ResolveFunction already logs when it cannot find one.
	UFunction* Fn = FCrowdyRPC::ResolveFunction(Z_EventClass, *Z_EventName.ToString());

	// The terms on the stack were written for the signature the graph compiled against. If the function has
	// been reshaped since, or is gone, there is no term count that is right and reading the wrong one
	// corrupts this frame.
	const bool bSignatureMatches = Fn && FCrowdyRPC::ComputeFunctionID(Fn) == Z_ExpectedSignature;
	if (!bSignatureMatches)
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassEventLibrary] '%s::%s' does not match the signature this graph was compiled against. ")
			TEXT("Recompile the calling Blueprint. Aborting this call rather than reading its parameters. ")
			TEXT("(graph expected %lld, resolved %lld from owner '%s'%s)"),
			*GetNameSafe(Z_EventClass), *Z_EventName.ToString(),
			Z_ExpectedSignature,
			Fn ? FCrowdyRPC::ComputeFunctionID(Fn) : 0,
			Fn ? *GetPathNameSafe(Fn->GetOwnerClass()) : TEXT("<unresolved>"),
			Fn ? TEXT("") : TEXT(", function not found"));

		// Returning here would leave Stack.Code on a term rather than an instruction, and the interpreter
		// cannot recover: with no parameters the next byte is EX_EndFunctionParms, whose handler steps Code
		// backwards, so the enclosing loop spins forever. Aborting is the only safe exit.
		Stack.bAbortingExecution = true;
		*static_cast<bool*>(RESULT_PARAM) = true;
		return;
	}

	const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
	void* Frame = FMemory_Alloca(FrameSize);
	FMemory::Memzero(Frame, FrameSize);

	FScopedEventParamFrame ScopedFrame;
	ScopedFrame.Fn = Fn;
	ScopedFrame.Frame = Frame;

	// Every parameter the node exposed as a pin arrives here as one term, in the same declaration order
	// FCrowdyRPC::PlaceAll and the serializer already use elsewhere on this event: CPF_Parm properties
	// that are not a true output (FCrowdyRPC::IsTrueOutputParam). For a genuine CrowdyEvent that is every
	// parameter, since its signature is required to carry no return value and no output parameter.
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm) || FCrowdyRPC::IsTrueOutputParam(Prop))
		{
			continue;
		}

		void* Dest = Prop->ContainerPtrToValuePtr<void>(Frame);
		Prop->InitializeValue(Dest);
		Stack.StepCompiledIn<FProperty>(Dest);
	}

	P_FINISH;

	bool bResult = false;
	P_NATIVE_BEGIN;
	bResult = UCrowdyMassEventLibrary::SendEventToMassTarget(Z_Target, Fn, Frame);
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bResult;
}
