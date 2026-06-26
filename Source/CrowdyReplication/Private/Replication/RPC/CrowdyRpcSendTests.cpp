#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/RPC/CrowdyRpcTestTarget.h"
#include "Core/FCrowdyTypeID.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyRpcSendTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// A CrowdyEvent receiver is an ordinary reflected UFUNCTION carrying its routing in
// metadata; CROWDY_EVENT only adds the call-site thunk. This confirms the receiver
// reflects and its routing resolves to the values declared on the UFUNCTION — the
// front door cannot resolve or dispatch a call otherwise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcEventReceiverReflectedTest,
	"CrowdySDK.RPC.EventReceiverReflected", CrowdyRpcSendTestFlags)
bool FCrowdyRpcEventReceiverReflectedTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("MacroEvent_Implementation"));
	TestNotNull(TEXT("event receiver is a registered UFUNCTION"), Fn);
	if (!Fn)
	{
		return false;
	}

#if WITH_METADATA
	// The routing values declared on the UFUNCTION must round-trip through BuildFnInfo.
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
	TestTrue(TEXT("recipient from meta"), Info.Recipient == ECrowdyEventRecipient::OwningClient);
	TestTrue(TEXT("decay from meta"), Info.DecayRate == ECrowdyDecayRate::Exponential_Decay);
	TestTrue(TEXT("distance from meta"), Info.Distance == ECrowdyReplicationDistance::Four_Chunks);
#endif

	return true;
}

// Calling the macro thunk drives SendChecked, which marshals the arguments into a
// wire-ready call. Here we drive the same MarshalCall path the thunk uses and confirm
// the packed call carries the right identity and round-trips its arguments.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcMacroMarshalsCallTest,
	"CrowdySDK.RPC.MacroMarshalsCall", CrowdyRpcSendTestFlags)
bool FCrowdyRpcMacroMarshalsCallTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("MacroEvent_Implementation"));
	TestNotNull(TEXT("MacroEvent resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const FVector Dir(0.0, 0.0, 1.0);
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::MacroEvent_Implementation, 5, Dir);

	TestEqual(TEXT("call carries the function id"), Call.FunctionID, Info.FunctionID);
	TestNotEqual(TEXT("function id is set"), Call.FunctionID, static_cast<int64>(0));
	TestTrue(TEXT("parameters serialized past the version byte"), Call.ParamBlob.Num() > 1);

	// The bytes the wire would carry deserialize and invoke with the original values.
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);
	TestEqual(TEXT("invoked exactly once"), Target->CallCount, 1);
	TestEqual(TEXT("int32 arg"), Target->GotI32, 5);
	TestTrue(TEXT("FVector arg"), Target->GotVector.Equals(Dir));
	return true;
}

// Phase 3 changes SendChecked from a local replay into a transport send. With no
// world (and so no entity subsystem) the send must log-and-drop, never crash and
// never invoke the receiver locally — that is the receive side's job from Phase 4 on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSendWithoutWorldIsSafeTest,
	"CrowdySDK.RPC.SendWithoutWorldIsSafe", CrowdyRpcSendTestFlags)
bool FCrowdyRpcSendWithoutWorldIsSafeTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	// Drive the real macro thunk: it compiles only if the static type checks pass,
	// and it routes the call instead of running it here.
	Target->MacroEvent(9, FVector(1.0, 2.0, 3.0));

	TestEqual(TEXT("routed, not invoked locally"), Target->CallCount, 0);
	return true;
}

// The dispatch path resolves the wire EventType for FCrowdyRpcCall through the event
// payload registry; if the struct is not registered the send is dropped. The id must
// be stable (path-hashed) so both ends agree.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcPayloadTypeRegisteredTest,
	"CrowdySDK.RPC.PayloadTypeRegistered", CrowdyRpcSendTestFlags)
bool FCrowdyRpcPayloadTypeRegisteredTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Registry = UEventPayloadRegistry::Get();
	TestNotNull(TEXT("registry available"), Registry);
	if (!Registry)
	{
		return false;
	}

	Registry->RegisterStructAuto(FCrowdyRpcCall::StaticStruct());

	FCrowdyTypeID ID = 0;
	TestTrue(TEXT("FCrowdyRpcCall has a wire type id"), Registry->GetID(FCrowdyRpcCall::StaticStruct(), ID));
	TestNotEqual(TEXT("id is non-zero"), static_cast<int32>(ID), 0);

	// Re-resolving yields the same id: registration is idempotent and deterministic.
	FCrowdyTypeID Again = 0;
	Registry->GetID(FCrowdyRpcCall::StaticStruct(), Again);
	TestEqual(TEXT("id is stable"), static_cast<int32>(Again), static_cast<int32>(ID));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
