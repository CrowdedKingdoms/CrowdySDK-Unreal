#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/RPC/CrowdyEventSeamTestTypes.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyRpcTestTarget.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"

// Covers the event router's spawn-wait defer queue caps and ICrowdyEntitySubscriber's event plane, without
// pulling in a Mass entity: every case here uses only the router, the entity subsystem, and a plain RPC
// call carrying an id nothing has claimed. The queue bounds untrusted input regardless of what kind of
// entity eventually claims it, so exercising them against the caps directly is a truer test than routing
// through a Mass avatar. The same interface's state plane is covered in CrowdyStateFragmentProviderTests.cpp.
namespace
{
	constexpr EAutomationTestFlags CrowdyEventSeamTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An ordinary (never action-scoped) RPC call for an entity id nothing has claimed: no participant is
	// registered for it and no subscriber holds it, so dispatching it always lands on the defer queue.
	// ClassID/FunctionID are left at zero deliberately - resolving to a real function is not needed to
	// exercise the queue's own bookkeeping, only a SenderID foreign enough that the broadcast echo-drop
	// never fires.
	FCrowdyScopedInboundEvent MakeUnresolvedRpcEvent(const FGuid& EntityID)
	{
		FCrowdyRpcCall Call;
		Call.EntityID = EntityID;
		Call.SenderID = FGuid::NewGuid();

		FCrowdyScopedInboundEvent Scoped;
		Scoped.OwnedPayload = FInstancedStruct::Make(Call);
		Scoped.Event.bTargetedDelivery = false;
		Scoped.Event.Target = ECrowdyTarget::Everyone;
		return Scoped;
	}
}

// Mutating CrowdyDeferQueue::MaxEvents (or the eviction loop in EnforceDeferredEventCap) turns this red:
// the queue would either grow past the cap or trim the wrong number of events.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventSeamDeferQueueTotalCapEvictsOldestTest,
	"CrowdySDK.EventSeam.DeferQueueTotalCapEvictsOldest", CrowdyEventSeamTestFlags)
bool FCrowdyEventSeamDeferQueueTotalCapEvictsOldestTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(nullptr, Entities);

	// Nine distinct ids, far under the 256-entity cap, so only the 1024-event total cap can trim this
	// queue. Bunched insertion (every event for one id before moving to the next) puts the oldest events
	// entirely on the first id, so its surviving count pins the exact eviction boundary.
	constexpr int32 NumIds = 9;
	constexpr int32 EventsPerId = 120;
	TArray<FGuid> Ids;
	for (int32 IdIndex = 0; IdIndex < NumIds; ++IdIndex)
	{
		Ids.Add(FGuid::NewGuid());
	}

	for (const FGuid& Id : Ids)
	{
		for (int32 EventIndex = 0; EventIndex < EventsPerId; ++EventIndex)
		{
			Router->DispatchEvent(MakeUnresolvedRpcEvent(Id));
		}
	}

	constexpr int32 TotalInserted = NumIds * EventsPerId; // 1080
	constexpr int32 ExpectedEvictions = TotalInserted - CrowdyDeferQueue::MaxEvents; // 56

	TestEqual(TEXT("the queue never grows past the total cap"),
		Router->NumDeferredForTest(), CrowdyDeferQueue::MaxEvents);
	TestEqual(TEXT("every id is still represented; only the total cap tripped, not the distinct-id cap"),
		Router->NumDeferredEntitiesForTest(), NumIds);
	TestEqual(TEXT("exactly the excess over the cap was evicted"),
		Router->NumDeferredEvictionsForTest(), ExpectedEvictions);

	// The oldest id (inserted first) is the one eviction ate into: it should have exactly
	// EventsPerId - ExpectedEvictions left. Registering it and retrying resolves precisely that many and
	// no more, which is the operational meaning of "oldest evicted first".
	RegisterProxyParticipant(Entities, Ids[0], NewObject<UCrowdyRpcSubsystemTestTarget>());
	Router->RetryDeferredForTest();

	constexpr int32 ExpectedRemainingForOldestId = EventsPerId - ExpectedEvictions; // 64
	TestEqual(TEXT("resolving the oldest id drains exactly its surviving events, not more and not fewer"),
		Router->NumDeferredForTest(), CrowdyDeferQueue::MaxEvents - ExpectedRemainingForOldestId);
	TestEqual(TEXT("the oldest id is fully cleared from the wait set once resolved"),
		Router->NumDeferredEntitiesForTest(), NumIds - 1);
	TestEqual(TEXT("re-filing the untouched ids during the retry needed no further eviction"),
		Router->NumDeferredEvictionsForTest(), ExpectedEvictions);

	return true;
}

// Mutating CrowdyDeferQueue::MaxDistinctEntities (or the eviction loop in EnforceDeferredEntityCap) turns
// this red: either the distinct-id count would exceed the cap, or the oldest/newest id checks below
// would flip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventSeamDeferQueueDistinctIdCapEvictsIndependentlyTest,
	"CrowdySDK.EventSeam.DeferQueueDistinctIdCapEvictsIndependently", CrowdyEventSeamTestFlags)
bool FCrowdyEventSeamDeferQueueDistinctIdCapEvictsIndependentlyTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(nullptr, Entities);

	// One event per invented id: a flood of 300 distinct ids is far more than the 256-entity cap, but
	// the 300 total events are nowhere near the 1024-event cap. If only the total cap were enforced,
	// nothing here would be evicted at all, so any eviction below is proof the distinct-id cap acts on
	// its own.
	constexpr int32 NumIds = 300;
	TArray<FGuid> Ids;
	for (int32 IdIndex = 0; IdIndex < NumIds; ++IdIndex)
	{
		Ids.Add(FGuid::NewGuid());
		Router->DispatchEvent(MakeUnresolvedRpcEvent(Ids.Last()));
	}

	constexpr int32 ExpectedEvictions = NumIds - CrowdyDeferQueue::MaxDistinctEntities; // 44

	TestTrue(TEXT("the total event cap is nowhere near tripping"), NumIds < CrowdyDeferQueue::MaxEvents);
	TestEqual(TEXT("distinct ids are capped even though the total-event cap never engaged"),
		Router->NumDeferredEntitiesForTest(), CrowdyDeferQueue::MaxDistinctEntities);
	TestEqual(TEXT("one event per id, so the event count matches the distinct-id cap too"),
		Router->NumDeferredForTest(), CrowdyDeferQueue::MaxDistinctEntities);
	TestEqual(TEXT("exactly the excess over the distinct-id cap was evicted"),
		Router->NumDeferredEvictionsForTest(), ExpectedEvictions);

	// The oldest id was evicted to make room for later ones: registering it and retrying finds nothing
	// waiting, so the queue is unchanged.
	RegisterProxyParticipant(Entities, Ids[0], NewObject<UCrowdyRpcSubsystemTestTarget>());
	Router->RetryDeferredForTest();
	TestEqual(TEXT("the oldest id left nothing behind to resolve"),
		Router->NumDeferredForTest(), CrowdyDeferQueue::MaxDistinctEntities);

	// The newest id was kept: registering it and retrying resolves its one event and removes it.
	RegisterProxyParticipant(Entities, Ids.Last(), NewObject<UCrowdyRpcSubsystemTestTarget>());
	Router->RetryDeferredForTest();
	TestEqual(TEXT("the newest id's single event was still waiting and is now resolved"),
		Router->NumDeferredForTest(), CrowdyDeferQueue::MaxDistinctEntities - 1);

	return true;
}

// The existing actor/participant RPC path is untouched by the subscriber seam. Stated as a test: an entity
// already known to the ordinary entity registry must dispatch exactly as it always has, and the subscriber
// - registered here specifically so it has something to wrongly answer - must never be consulted at all.
// Mutating the `if (!TargetParticipant)` guard in UCrowdyEventRouter::DispatchRpcCall (CrowdyEventRouter.cpp)
// to consult the subscriber before or regardless of the registry lookup turns this red: the spy's call
// counts stop being zero.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventSeamActorSourcedRpcNeverConsultsExternalProviderTest,
	"CrowdySDK.EventSeam.ActorSourcedRpcNeverConsultsExternalProvider", CrowdyEventSeamTestFlags)
bool FCrowdyEventSeamActorSourcedRpcNeverConsultsExternalProviderTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	Registry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	// Always answers "unknown, nothing to receive on" - the one pair of answers that makes a wrongly
	// consulted subscriber visible: the call below would be dropped instead of delivered.
	UCrowdySpyEntitySubscriber* SpySubscriber = NewObject<UCrowdySpyEntitySubscriber>();
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(SpySubscriber));

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Target);

	UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(), TEXT("SubMulticast_Implementation"));
	TestNotNull(TEXT("SubMulticast resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
	FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 42);
	Call.EntityID = EntityID;
	Call.SenderID = FGuid::NewGuid();

	Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("the call still runs, on the entity found in the ordinary registry"), Target->CallCount, 1);
	TestEqual(TEXT("with its argument intact"), Target->GotValue, 42);
	TestEqual(TEXT("the subscriber was never asked whether the id is known"), SpySubscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nor asked to resolve a receiver"), SpySubscriber->ResolveReceiverCallCount, 0);

	// The other planes are on the same registration now, so a delivery leaking onto one of them would be a
	// call arriving twice by two roads rather than a miss, which nothing above would notice.
	TestEqual(TEXT("and nothing reached it on the state plane"), SpySubscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("or the model plane"), SpySubscriber->ApplyModelChangesCallCount, 0);

	return true;
}

/**
 * The subscriber slot must survive level travel: two worlds are alive at once, the arriving world registers
 * first, and the departing world finishes tearing down afterwards. So a late registration from the departing
 * world must not take the slot back, and its release must not clear the arriving world's claim.
 *
 * There is one slot now, and the sibling test in CrowdyStateFragmentProviderTests.cpp reaches the same code
 * through the state plane. This one asserts it through the event plane, because a router that later grew a
 * second registration path, or resolved the slot differently per plane, would leave one of the two reading
 * green while the other silently lost the live world's payloads after a level change.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventSeamReceiverProviderIsWorldScopedTest,
	"CrowdySDK.EventSeam.ReceiverProviderIsWorldScoped", CrowdyEventSeamTestFlags)
bool FCrowdyEventSeamReceiverProviderIsWorldScopedTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	Registry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdySpyEntitySubscriber* DepartingWorldSubscriber = NewObject<UCrowdySpyEntitySubscriber>();
	UCrowdySpyEntitySubscriber* ArrivingWorldSubscriber = NewObject<UCrowdySpyEntitySubscriber>();

	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(ArrivingWorldSubscriber));

	UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(), TEXT("SubMulticast_Implementation"));
	if (!TestNotNull(TEXT("SubMulticast resolved"), Fn))
	{
		return false;
	}

	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);

	// No participant is registered for this id, so the router falls through to whichever subscriber holds the
	// slot. The spy answers "unknown", so the call is left on the wait path; the consultation counts are what
	// this test reads, not the delivery.
	FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 42);
	Call.EntityID = FGuid::NewGuid();
	Call.SenderID = FGuid::NewGuid();

	Router->ReceiveChannelRpcCall(Call);

	TestTrue(TEXT("the arriving world's subscriber was consulted"),
		ArrivingWorldSubscriber->ResolveReceiverCallCount > 0 || ArrivingWorldSubscriber->IsEntityKnownCallCount > 0);
	TestEqual(TEXT("the departing world's subscriber was never asked whether it holds the id once superseded"),
		DepartingWorldSubscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nor asked to resolve a receiver"),
		DepartingWorldSubscriber->ResolveReceiverCallCount, 0);

	// The departing world tears down AFTER the arriving world registered: the exact ordering this seam exists
	// to survive. Neither its late re-registration nor its release may touch the live claim.
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));
	Router->UnregisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));

	ArrivingWorldSubscriber->ResolveReceiverCallCount = 0;
	ArrivingWorldSubscriber->IsEntityKnownCallCount = 0;

	FCrowdyRpcCall SecondCall = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 43);
	SecondCall.EntityID = FGuid::NewGuid();
	SecondCall.SenderID = FGuid::NewGuid();

	Router->ReceiveChannelRpcCall(SecondCall);

	TestTrue(TEXT("the arriving world's subscriber still holds the slot after both attempts"),
		ArrivingWorldSubscriber->ResolveReceiverCallCount > 0 || ArrivingWorldSubscriber->IsEntityKnownCallCount > 0);
	TestEqual(TEXT("the departing world's late re-registration never took the slot back"),
		DepartingWorldSubscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("and it was never asked to resolve a receiver either"),
		DepartingWorldSubscriber->ResolveReceiverCallCount, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
