#include "Replication/RPC/CrowdyRowEventTestTypes.h"

void ACrowdyRowEventTestActor::RowMulticast_Implementation(int32 ActionId, float PlayRate)
{
	GotActionId = ActionId;
	++CallCount;
}

void ACrowdyRowEventTestActor::RowOwnerOnly_Implementation(int32 ActionId)
{
	GotActionId = ActionId;
	++CallCount;
}

void UCrowdyRowEventTestRowOnly::RowOnlyMulticast_Implementation(int32 ActionId)
{
	GotActionId = ActionId;
	++CallCount;
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"

// Covers the route an event declared on a game's own ACTOR class takes to a client that holds the target
// as a row in a table rather than as an actor: the router finds no participant, the subscriber says the id
// is its own, no object here declares the function, and the call is handed over as decoded values to
// whatever is registered for it. Everything here is driven through the router's real receive entry, so the
// order the guards run in is under test rather than assumed.
namespace
{
	constexpr EAutomationTestFlags CrowdyRowEventTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The router, an entity subsystem with no participants at all, and a registry that knows the test
	// actor's events, which is the whole world an inbound call for a row-held entity needs.
	struct FCrowdyRowEventFixture
	{
		UCrowdyAutoRegistry* Registry = nullptr;
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyEventRouter* Router = nullptr;
		UCrowdyRowEventSpySubscriber* Spy = nullptr;

		FCrowdyRowEventFixture()
		{
			Registry = MakeStateRegistry();
			Registry->UpdateClassRpcFunctions(ACrowdyRowEventTestActor::StaticClass());
			Registry->UpdateClassRpcFunctions(UCrowdyRowEventTestRowOnly::StaticClass());

			Entities = MakeEntitySubsystem(FGuid::NewGuid());
			Router = MakeRouter(Registry, Entities);

			Spy = NewObject<UCrowdyRowEventSpySubscriber>();
			Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Spy));
		}

		static UFunction* Multicast()
		{
			return FCrowdyRPC::ResolveFunction(ACrowdyRowEventTestActor::StaticClass(), TEXT("RowMulticast_Implementation"));
		}

		static UFunction* OwnerOnly()
		{
			return FCrowdyRPC::ResolveFunction(ACrowdyRowEventTestActor::StaticClass(), TEXT("RowOwnerOnly_Implementation"));
		}

		// Declared on a plain UObject, so no actor could ever run it however long a call for it waits.
		static UFunction* RowOnly()
		{
			return FCrowdyRPC::ResolveFunction(UCrowdyRowEventTestRowOnly::StaticClass(), TEXT("RowOnlyMulticast_Implementation"));
		}

		// A call for the row-only event, addressed like the actor multicast above.
		static FCrowdyRpcCall MakeRowOnlyCall(UFunction* Fn, int32 ActionId, const FGuid& EntityID)
		{
			const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
			FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
				&UCrowdyRowEventTestRowOnly::RowOnlyMulticast_Implementation, ActionId);
			Call.EntityID = EntityID;
			Call.SenderID = FGuid::NewGuid();
			return Call;
		}

		// A stale inbound broadcast for Call, stamped as though it arrived a full second ago: well past the
		// short action budget and nowhere near the ordinary 600-tick spawn wait, so one retry pass tells the
		// two budgets apart.
		static FCrowdyScopedInboundEvent MakeStaleEvent(const FCrowdyRpcCall& Call)
		{
			FCrowdyScopedInboundEvent Scoped;
			Scoped.OwnedPayload = FInstancedStruct::Make(Call);
			Scoped.Event.bTargetedDelivery = false;
			Scoped.Event.Target = ECrowdyTarget::Everyone;
			Scoped.Event.ReceivedAtSeconds = FPlatformTime::Seconds() - 1.0;
			return Scoped;
		}

		// A multicast call carrying the given parameters, addressed to a fresh entity id nothing has
		// registered as a participant, with a foreign sender so the broadcast echo-drop never fires.
		static FCrowdyRpcCall MakeMulticastCall(UFunction* Fn, int32 ActionId, float PlayRate, const FGuid& EntityID)
		{
			const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
			FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
				&ACrowdyRowEventTestActor::RowMulticast_Implementation, ActionId, PlayRate);
			Call.EntityID = EntityID;
			Call.SenderID = FGuid::NewGuid();
			return Call;
		}
	};
}

// The gap this seam closes, stated end to end: an event declared on an actor class, multicast, reaching a
// client that holds its target as a row. Mutating the `if (!Receiver)` branch in
// UCrowdyEventRouter::DispatchCallToSubscriber back to a bare drop turns this red, and so does removing
// either parameter from FCrowdyRPC::DecodeCall's frame construction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventActorMulticastReachesHandlerTest,
	"CrowdySDK.RowEvent.ActorMulticastReachesHandler", CrowdyRowEventTestFlags)
bool FCrowdyRowEventActorMulticastReachesHandlerTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), Fn))
	{
		return false;
	}

	// The entity is held here, nothing declares the function here, and a handler is registered for it:
	// exactly what an observer of a remote player rendered as a Mass row looks like.
	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	const FGuid EntityID = FGuid::NewGuid();
	const FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeMulticastCall(Fn, 7, 1.5f, EntityID);

	Fixture.Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("the call reached the registered handler exactly once"),
		Fixture.Spy->HandleEntityEventCallCount, 1);
	TestEqual(TEXT("the object route was tried first, and answered nothing"),
		Fixture.Spy->ResolveReceiverCallCount, 1);
	TestTrue(TEXT("the handler was told which entity"), Fixture.Spy->LastEntityUUID == EntityID);
	TestTrue(TEXT("and which sender claimed it"), Fixture.Spy->LastSenderID == Call.SenderID);
	TestTrue(TEXT("and which function"), Fixture.Spy->LastFunction == Fn);

	TestTrue(TEXT("the int parameter read back by name"), Fixture.Spy->bReadActionId);
	TestEqual(TEXT("with the value the sender marshalled"), Fixture.Spy->LastActionId, 7);
	TestTrue(TEXT("the float parameter read back by name"), Fixture.Spy->bReadPlayRate);
	TestEqual(TEXT("with the value the sender marshalled"), Fixture.Spy->LastPlayRate, 1.5f);

	// A name the event does not declare must read as absent rather than as some other parameter's bytes,
	// which is the whole reason the parameters are addressed by name and not by position.
	TestFalse(TEXT("a parameter the event does not declare reads as absent"), Fixture.Spy->bReadAbsentParam);
	TestEqual(TEXT("and leaves the caller's value untouched"), Fixture.Spy->LastAbsentParam, -1);

	// The other planes ride the same registration, so a delivery leaking onto one of them would be the call
	// arriving twice by two roads, which nothing above would notice.
	TestEqual(TEXT("nothing reached the state plane"), Fixture.Spy->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("nor the model plane"), Fixture.Spy->ApplyModelChangesCallCount, 0);

	TestEqual(TEXT("nothing was counted as unhandled"),
		Fixture.Router->NumUnhandledSubscriberEventsForTest(), 0);

	return true;
}

/**
 * An observer with no handler registered must say so once and count every drop, because the two failures
 * this sits between are opposite: a line per event hands a crowd an amplifier, and a silent drop is
 * indistinguishable from the event never arriving.
 *
 * Mutating ReportUnhandledSubscriberEvent's already-reported early return away turns this red on the report
 * count; mutating the count itself away turns it red on the drop count. Neither mutation alone reddens the
 * other assertion, which is what makes them two claims rather than one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventUnhandledIsReportedOnceAndCountedAlwaysTest,
	"CrowdySDK.RowEvent.UnhandledIsReportedOnceAndCountedAlways", CrowdyRowEventTestFlags)
bool FCrowdyRowEventUnhandledIsReportedOnceAndCountedAlwaysTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), Fn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = nullptr;

	// One event each for several entities, which is the shape a crowd produces: one missing registration
	// hit once per entity.
	constexpr int32 NumEvents = 12;
	for (int32 Index = 0; Index < NumEvents; ++Index)
	{
		Fixture.Router->ReceiveChannelRpcCall(
			FCrowdyRowEventFixture::MakeMulticastCall(Fn, Index + 1, 1.0f, FGuid::NewGuid()));
	}

	TestEqual(TEXT("every dropped call is counted"),
		Fixture.Router->NumUnhandledSubscriberEventsForTest(), NumEvents);
	TestEqual(TEXT("and the whole run is reported once, not once per event"),
		Fixture.Router->NumUnhandledSubscriberEventReportsForTest(), 1);
	TestEqual(TEXT("nothing was delivered to a handler that does not exist"),
		Fixture.Spy->HandleEntityEventCallCount, 0);

	// Nothing waits: the subscriber said the id is its own, so the calls were refused rather than queued.
	TestEqual(TEXT("a refusal is final, so no call went on the spawn-wait queue"),
		Fixture.Router->NumDeferredForTest(), 0);

	return true;
}

// An owner-only call has no legitimate way to reach an entity with no actor, so it must be refused before
// anything about this client's storage is consulted. Mutating the recipient guard in
// DispatchCallToSubscriber to run after ResolveReceiver, or removing it, turns this red: the handler would
// run a call that only one client was ever meant to run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventOwnerOnlyNeverReachesHandlerTest,
	"CrowdySDK.RowEvent.OwnerOnlyNeverReachesHandler", CrowdyRowEventTestFlags)
bool FCrowdyRowEventOwnerOnlyNeverReachesHandlerTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::OwnerOnly();
	if (!TestNotNull(TEXT("the actor's owner-only event resolved"), Fn))
	{
		return false;
	}

	// Registered for exactly this function, so a handler that runs cannot be explained by anything else.
	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
	FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&ACrowdyRowEventTestActor::RowOwnerOnly_Implementation, 9);
	Call.EntityID = FGuid::NewGuid();
	Call.SenderID = FGuid::NewGuid();

	Fixture.Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("an owner-only call never reaches the handler"),
		Fixture.Spy->HandleEntityEventCallCount, 0);
	TestEqual(TEXT("and is refused before this client's storage is even asked for a receiver"),
		Fixture.Spy->ResolveReceiverCallCount, 0);
	TestEqual(TEXT("it is a refusal, not a missing registration, so it is not counted as unhandled"),
		Fixture.Router->NumUnhandledSubscriberEventsForTest(), 0);
	TestEqual(TEXT("and it does not go on waiting for an entity the subscriber already claimed"),
		Fixture.Router->NumDeferredForTest(), 0);

	return true;
}

/**
 * A call whose routing identity this build cannot resolve to a function must not reach the handler, and
 * must not be counted as a missing registration either: nothing here knows yet what it would have been a
 * registration FOR. The declaring class may simply not be loaded, so the call keeps waiting exactly as it
 * did before a handler route existed.
 *
 * It says nothing about the ClassID guard further down, which no wire input can reach: the resolver is
 * keyed on the same declaring class id the guard compares, so an identity that survives resolution already
 * agrees with it. The guard stays as the invariant it is, and this covers what the wire can actually do.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventUnresolvableIdentityNeverReachesHandlerTest,
	"CrowdySDK.RowEvent.UnresolvableIdentityNeverReachesHandler", CrowdyRowEventTestFlags)
bool FCrowdyRowEventUnresolvableIdentityNeverReachesHandlerTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), Fn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeMulticastCall(Fn, 7, 1.5f, FGuid::NewGuid());

	// Control first: the same call, unaltered, does reach the handler. Without it, a route that refused
	// everything would read exactly like a route that refuses only the drifted identity.
	Fixture.Router->ReceiveChannelRpcCall(Call);
	TestEqual(TEXT("the unaltered call reaches the handler"), Fixture.Spy->HandleEntityEventCallCount, 1);

	// The identity a peer on a drifted build would stamp: nothing in this build answers to it.
	Call.ClassID = Call.ClassID + 1;
	Fixture.Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("a call naming an identity this build cannot resolve never reaches the handler"),
		Fixture.Spy->HandleEntityEventCallCount, 1);
	TestEqual(TEXT("and is not blamed on a missing registration"),
		Fixture.Router->NumUnhandledSubscriberEventsForTest(), 0);
	TestEqual(TEXT("it waits for the declaring class instead, exactly as it did before this route existed"),
		Fixture.Router->NumDeferredForTest(), 1);

	return true;
}

/**
 * A call whose target has not arrived yet is held on the shared wait queue, and which budget it waits on
 * turns on whether an ACTOR could still run it.
 *
 * A call declared where no actor can reach it changes what an entity looks like, and a change to a
 * representation is only worth making while it is still current, so it takes the short action budget: the
 * entity a late one names may have died and respawned under the same id while it waited. A call declared
 * on an actor class is a different case entirely. Its target may be a moment away from spawning as a real
 * actor that would run the body, and judging it on a fraction of a second throws that call away.
 *
 * Three cases, because any two of them read the same as a single blanket rule. Case one is the defect:
 * mutating the actor-reach test out of the wait budget evicts it and turns this red. Case two is what
 * stops the fix from closing the gate entirely, and case three says the short wait comes from the
 * registered handler rather than from the declaring class alone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventHandlerShortensTheSpawnWaitTest,
	"CrowdySDK.RowEvent.HandlerShortensTheSpawnWait", CrowdyRowEventTestFlags)
bool FCrowdyRowEventHandlerShortensTheSpawnWaitTest::RunTest(const FString& Parameters)
{
	UFunction* ActorFn = FCrowdyRowEventFixture::Multicast();
	UFunction* RowFn = FCrowdyRowEventFixture::RowOnly();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), ActorFn)
		|| !TestNotNull(TEXT("the row-only multicast event resolved"), RowFn))
	{
		return false;
	}

	// A call an arriving actor could run keeps the ordinary spawn wait, even with a handler registered for
	// it. Its entity has not arrived yet, and the thing it is waiting for is the actor.
	{
		FCrowdyRowEventFixture Fixture;

		// The id is NOT the subscriber's: the entity has not arrived, which is the only state in which the
		// wait budget is chosen at all.
		Fixture.Spy->bKnowsEntity = false;
		Fixture.Spy->HandledFunction = ActorFn;

		const FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeMulticastCall(ActorFn, 7, 1.0f, FGuid::NewGuid());
		Fixture.Router->DispatchEvent(FCrowdyRowEventFixture::MakeStaleEvent(Call));
		TestEqual(TEXT("the call is queued while its entity is missing"),
			Fixture.Router->NumDeferredForTest(), 1);

		Fixture.Router->RetryDeferredForTest();
		TestEqual(TEXT("a call an arriving actor could run keeps the ordinary spawn wait"),
			Fixture.Router->NumDeferredForTest(), 1);
		TestEqual(TEXT("so nothing was evicted"),
			Fixture.Router->NumDeferredEvictionsForTest(), 0);
	}

	// A call no actor could ever run, with a handler registered for it, takes the short budget. Without
	// this the fix above would read the same as switching the short budget off altogether.
	{
		FCrowdyRowEventFixture Fixture;

		Fixture.Spy->bKnowsEntity = false;
		Fixture.Spy->HandledFunction = RowFn;

		const FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeRowOnlyCall(RowFn, 7, FGuid::NewGuid());
		Fixture.Router->DispatchEvent(FCrowdyRowEventFixture::MakeStaleEvent(Call));
		TestEqual(TEXT("the call is queued while its entity is missing"),
			Fixture.Router->NumDeferredForTest(), 1);

		Fixture.Router->RetryDeferredForTest();
		TestEqual(TEXT("a call beyond any actor's reach is dropped once its action budget has elapsed"),
			Fixture.Router->NumDeferredForTest(), 0);
		TestEqual(TEXT("and the drop is counted rather than silent"),
			Fixture.Router->NumDeferredEvictionsForTest(), 1);
	}

	// Control: the identical row-only call with nothing registered for its function keeps the ordinary
	// wait, so the short budget is the handler's doing and not the declaring class's.
	{
		FCrowdyRowEventFixture Fixture;

		Fixture.Spy->bKnowsEntity = false;
		Fixture.Spy->HandledFunction = nullptr;

		const FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeRowOnlyCall(RowFn, 7, FGuid::NewGuid());
		Fixture.Router->DispatchEvent(FCrowdyRowEventFixture::MakeStaleEvent(Call));
		Fixture.Router->RetryDeferredForTest();

		TestEqual(TEXT("the same call with no handler registered is still waiting on the ordinary budget"),
			Fixture.Router->NumDeferredForTest(), 1);
		TestEqual(TEXT("so nothing was evicted"),
			Fixture.Router->NumDeferredEvictionsForTest(), 0);
	}

	return true;
}

/**
 * The precedence defect, stated end to end: an entity that HAS a participant, where that participant
 * declares nothing able to receive the call. Presence is not capability, so the participant is not an
 * answer and the call belongs to whoever holds the entity as data.
 *
 * This is what a crowd entity looks like the moment anything mints its avatar: the avatar is enrolled as
 * the entity's participant, and an actor-class multicast has nothing on it to run. Mutating the
 * fall-through in UCrowdyEventRouter::DispatchRpcCall back to a bare drop turns this red on the first
 * assertion.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventIncapableParticipantFallsThroughTest,
	"CrowdySDK.RowEvent.IncapableParticipantFallsThrough", CrowdyRowEventTestFlags)
bool FCrowdyRowEventIncapableParticipantFallsThroughTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), Fn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	// A participant that is neither the actor class nor an actor at all, so nothing on it declares the
	// call. Registered as a RemoteProxy owned by someone else, which is the one combination none of the
	// receive path's ownership gates apply to.
	const FGuid EntityID = FGuid::NewGuid();
	UCrowdyRowEventTestRowOnly* Participant = NewObject<UCrowdyRowEventTestRowOnly>();
	RegisterProxyParticipant(Fixture.Entities, EntityID, Participant);

	const FCrowdyRpcCall Call = FCrowdyRowEventFixture::MakeMulticastCall(Fn, 7, 1.5f, EntityID);
	Fixture.Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("a participant that cannot receive the call does not displace the subscriber that can"),
		Fixture.Spy->HandleEntityEventCallCount, 1);
	TestTrue(TEXT("the handler was told which entity"), Fixture.Spy->LastEntityUUID == EntityID);
	TestTrue(TEXT("the int parameter survived the route"), Fixture.Spy->bReadActionId);
	TestEqual(TEXT("with the value the sender marshalled"), Fixture.Spy->LastActionId, 7);
	TestEqual(TEXT("nothing ran on the participant itself"), Participant->CallCount, 0);
	TestEqual(TEXT("and nothing was counted as undeliverable"),
		Fixture.Router->NumUndeliverableCallsForTest(), 0);

	// Waiting is not what happened either: the entity is registered, so the call was routed, not queued.
	TestEqual(TEXT("nothing went on the spawn-wait queue"), Fixture.Router->NumDeferredForTest(), 0);

	return true;
}

/**
 * The fall-through above must not become a way around the owner/host guards it now sits under. An
 * owner/host-only call arriving as a BROADCAST for an actor participant is refused by those guards, and
 * moving the fall-through above them would hand exactly those calls to the subscriber instead.
 *
 * Hoisting the fall-through over the owner/host guard block turns this red: the handler would run a call
 * that only one client was ever meant to run.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventFallThroughStaysUnderTheOwnerGuardTest,
	"CrowdySDK.RowEvent.FallThroughStaysUnderTheOwnerGuard", CrowdyRowEventTestFlags)
bool FCrowdyRowEventFallThroughStaysUnderTheOwnerGuardTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::OwnerOnly();
	if (!TestNotNull(TEXT("the actor's owner-only event resolved"), Fn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	// An ACTOR participant, so the guard that applies is the one demanding a targeted single-actor
	// delivery. A test actor needs no world here: nothing is ever invoked on it.
	const FGuid EntityID = FGuid::NewGuid();
	ACrowdyRowEventTestActor* Participant = NewObject<ACrowdyRowEventTestActor>();
	RegisterProxyParticipant(Fixture.Entities, EntityID, Participant);

	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
	FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&ACrowdyRowEventTestActor::RowOwnerOnly_Implementation, 9);
	Call.EntityID = EntityID;
	Call.SenderID = FGuid::NewGuid();

	// ReceiveChannelRpcCall delivers as a broadcast, which is exactly what an owner-only call must never
	// arrive as.
	Fixture.Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("an owner-only broadcast never reaches the handler"),
		Fixture.Spy->HandleEntityEventCallCount, 0);
	TestEqual(TEXT("and the subscriber is not even asked for a receiver"),
		Fixture.Spy->ResolveReceiverCallCount, 0);
	TestEqual(TEXT("nor is it asked whether it holds the id"),
		Fixture.Spy->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nothing ran on the participant either"), Participant->CallCount, 0);

	return true;
}

/**
 * An inbound call names an entity chosen by whoever sent it, so no drop reason on this route may turn one
 * arriving call into one log line. Every drop is counted and the run is reported once, which is the pair
 * that tells a throttled report apart from no traffic at all.
 *
 * Removing the throttle turns this red on the report count; removing the counting turns it red on the drop
 * count. Two claims, not one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventUndeliverableIsThrottledTest,
	"CrowdySDK.RowEvent.UndeliverableIsThrottled", CrowdyRowEventTestFlags)
bool FCrowdyRowEventUndeliverableIsThrottledTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* OwnerFn = FCrowdyRowEventFixture::OwnerOnly();
	UFunction* ActorFn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's owner-only event resolved"), OwnerFn)
		|| !TestNotNull(TEXT("the actor's multicast event resolved"), ActorFn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = OwnerFn;

	const FCrowdyFnInfo OwnerInfo = FCrowdyRPC::GetFnInfo(OwnerFn);

	// One owner-only broadcast each for a run of entities, which is the shape a crowd produces: one
	// refusal reason hit once per entity.
	constexpr int32 NumRefused = 12;
	for (int32 Index = 0; Index < NumRefused; ++Index)
	{
		FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(OwnerFn, OwnerInfo,
			&ACrowdyRowEventTestActor::RowOwnerOnly_Implementation, Index);
		Call.EntityID = FGuid::NewGuid();
		Call.SenderID = FGuid::NewGuid();
		Fixture.Router->ReceiveChannelRpcCall(Call);
	}

	TestEqual(TEXT("every refused call is counted"),
		Fixture.Router->NumUndeliverableCallsForTest(), NumRefused);
	TestEqual(TEXT("and the whole run is reported once, not once per call"),
		Fixture.Router->NumUndeliverableCallReportsForTest(), 1);

	// The drop taken when a participant cannot receive the call and no subscriber holds the entity either
	// shares that throttle, so a flood cannot reopen it by arriving through the other reason.
	Fixture.Spy->bKnowsEntity = false;

	constexpr int32 NumStranded = 8;
	for (int32 Index = 0; Index < NumStranded; ++Index)
	{
		const FGuid EntityID = FGuid::NewGuid();
		RegisterProxyParticipant(Fixture.Entities, EntityID, NewObject<UCrowdyRowEventTestRowOnly>());
		Fixture.Router->ReceiveChannelRpcCall(
			FCrowdyRowEventFixture::MakeMulticastCall(ActorFn, Index, 1.0f, EntityID));
	}

	TestEqual(TEXT("a participant that cannot receive and no subscriber to fall through to is counted too"),
		Fixture.Router->NumUndeliverableCallsForTest(), NumRefused + NumStranded);
	TestEqual(TEXT("and reports through the same throttle rather than reopening it"),
		Fixture.Router->NumUndeliverableCallReportsForTest(), 1);

	// Those calls were refused, not queued: their entities are registered, so waiting could not help.
	TestEqual(TEXT("nothing went on the spawn-wait queue"), Fixture.Router->NumDeferredForTest(), 0);

	return true;
}

/**
 * The entity's inbound allowance is spent before the call's parameters are decoded, never after.
 * Decoding is the expensive half of receiving a call, and both the entity id and the number of calls
 * naming it are chosen by whoever sends them, so an allowance charged after the frame exists bounds work
 * that has already been done.
 *
 * Two claims. The refusal half reddens if the charge is removed: nothing is asked and nothing is counted.
 * The order half reddens if the charge is moved below the decode, because the decode carries the handler
 * call inside it, so a charge that runs after the decode also runs after the handler.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventSpentAllowanceStopsTheDecodeTest,
	"CrowdySDK.RowEvent.SpentAllowanceStopsTheDecode", CrowdyRowEventTestFlags)
bool FCrowdyRowEventSpentAllowanceStopsTheDecodeTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* Fn = FCrowdyRowEventFixture::Multicast();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), Fn))
	{
		return false;
	}

	Fixture.Spy->bKnowsEntity = true;
	Fixture.Spy->ReceiverToOffer = nullptr;
	Fixture.Spy->HandledFunction = Fn;

	// Control first: with an allowance to spend, the call lands and the charge came before it. Without
	// this, a route that refused everything would read exactly like a route that meters correctly.
	Fixture.Spy->bBudgetAvailable = true;
	Fixture.Router->ReceiveChannelRpcCall(
		FCrowdyRowEventFixture::MakeMulticastCall(Fn, 7, 1.5f, FGuid::NewGuid()));

	TestEqual(TEXT("the receiver was asked to spend one event"),
		Fixture.Spy->ChargeEntityEventBudgetCallCount, 1);
	TestEqual(TEXT("and the call was delivered"), Fixture.Spy->HandleEntityEventCallCount, 1);
	TestTrue(TEXT("the charge came before the delivery it bounds, not after it"),
		Fixture.Spy->ChargeStep > 0 && Fixture.Spy->ChargeStep < Fixture.Spy->HandleStep);
	TestEqual(TEXT("nothing was refused for a spent allowance"),
		Fixture.Router->NumBudgetRefusedCallsForTest(), 0);

	// Now the allowance is spent. The call is dropped on the spot and nothing is handed over.
	Fixture.Spy->bBudgetAvailable = false;
	Fixture.Router->ReceiveChannelRpcCall(
		FCrowdyRowEventFixture::MakeMulticastCall(Fn, 9, 2.5f, FGuid::NewGuid()));

	TestEqual(TEXT("the receiver was asked again"),
		Fixture.Spy->ChargeEntityEventBudgetCallCount, 2);
	TestEqual(TEXT("a refused call is never delivered"), Fixture.Spy->HandleEntityEventCallCount, 1);
	TestEqual(TEXT("and the drop is counted rather than silent"),
		Fixture.Router->NumBudgetRefusedCallsForTest(), 1);

	// A spent allowance is a refusal, not a reason to wait: the subscriber already said it holds the id.
	TestEqual(TEXT("nothing went on the spawn-wait queue"), Fixture.Router->NumDeferredForTest(), 0);

	// It is also not a missing registration, which is a different fault with a different fix.
	TestEqual(TEXT("nor is it blamed on a missing registration"),
		Fixture.Router->NumUnhandledSubscriberEventsForTest(), 0);

	return true;
}

/**
 * What the deleted ClassID comparison on the subscriber route rested on, pinned so its deletion is not
 * taken on trust: a CrowdyEvent function is resolved from (ClassID, FunctionID) against a resolver keyed
 * by the class that DECLARES it, and the sender stamps the call with that same declaring class's id. The
 * two are one value taken twice, which is why comparing them could never be unequal.
 *
 * This test cannot fail from removing dead code, and does not claim to. It reddens the day the resolver
 * stops being keyed by the declaring class, which is the only world in which that comparison would have
 * had two sources to compare and the deletion would have to be revisited.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowEventFunctionIdentityIsItsDeclaringClassTest,
	"CrowdySDK.RowEvent.FunctionIdentityIsItsDeclaringClass", CrowdyRowEventTestFlags)
bool FCrowdyRowEventFunctionIdentityIsItsDeclaringClassTest::RunTest(const FString& Parameters)
{
	FCrowdyRowEventFixture Fixture;

	UFunction* ActorFn = FCrowdyRowEventFixture::Multicast();
	UFunction* RowFn = FCrowdyRowEventFixture::RowOnly();
	if (!TestNotNull(TEXT("the actor's multicast event resolved"), ActorFn)
		|| !TestNotNull(TEXT("the row-only multicast event resolved"), RowFn))
	{
		return false;
	}

	const FCrowdyClassID ActorClassID = UCrowdyClassRegistry::Get()->GetID(ACrowdyRowEventTestActor::StaticClass());
	const FCrowdyClassID RowClassID = UCrowdyClassRegistry::Get()->GetID(UCrowdyRowEventTestRowOnly::StaticClass());
	TestTrue(TEXT("the two declaring classes have distinct ids"), ActorClassID != RowClassID);

	// The id a sender stamps on the call is the declaring class's, taken from the function itself.
	const FCrowdyRpcCall ActorCall = FCrowdyRowEventFixture::MakeMulticastCall(ActorFn, 1, 1.0f, FGuid::NewGuid());
	TestTrue(TEXT("a marshalled call carries its function's declaring class id"),
		static_cast<FCrowdyClassID>(ActorCall.ClassID) == ActorClassID);

	// And the id the resolver is keyed by is that same one, so resolution and the stamp agree by
	// construction rather than by agreement between two parties.
	const int64 ActorFunctionID = static_cast<int64>(FCrowdyRPC::GetFnInfo(ActorFn).FunctionID);
	UFunction* Resolved = Fixture.Registry->ResolveFunction(static_cast<int64>(ActorClassID), ActorFunctionID);
	TestTrue(TEXT("the declaring class id resolves the function the call named"), Resolved == ActorFn);
	if (Resolved)
	{
		TestTrue(TEXT("and what comes back declares itself on that very class"),
			UCrowdyClassRegistry::Get()->GetID(Resolved->GetOwnerClass()) == ActorClassID);
	}

	// The key is the pair, not the function id alone: another class's id does not reach this function.
	TestNull(TEXT("a different class's id resolves nothing for this function id"),
		Fixture.Registry->ResolveFunction(static_cast<int64>(RowClassID), ActorFunctionID));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
