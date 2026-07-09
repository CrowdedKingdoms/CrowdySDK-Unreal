#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h"

// Ownership-transfer coverage: the entity subsystem's ReassignOwnership (record re-point, role derivation,
// compare-and-swap, entity-component cache sync) and the state replicator's HandleOwnershipChanged retrack
// (untrack the old owner, track the new one, maintain KnownHostOwnedIDs across a transfer). The RPC transport
// (RequestOwnership / GrantOwnership) rides the existing, separately-tested RPC channel stack, so these tests
// drive the two substantive halves directly through the same test seams the host-migration tests use.
namespace
{
	constexpr EAutomationTestFlags CrowdyStateOwnershipTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package NewObject
	// trips a ClassWithin ensure, so outer it to a bare GameInstance (the shared Phase 1 idiom).
	UCrowdyAutoRegistry* MakeOwnershipRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UCrowdyAutoRegistry>(GameInstance);
	}

	// The replicator is a UWorldSubsystem, so a plain transient-package NewObject is fine (Initialize/Tick are
	// never driven on the test-seam path; no world/GameInstance is touched).
	UCrowdyStateReplicator* MakeOwnershipReplicator(UCrowdyAutoRegistry* Registry, const FGuid& LocalPlayer)
	{
		UCrowdyStateReplicator* Rep = NewObject<UCrowdyStateReplicator>(GetTransientPackage());
		Rep->SetRegistryForTest(Registry);
		Rep->SetLocalPlayerIDForTest(LocalPlayer);
		return Rep;
	}

	// A real entity subsystem so FindRecord / IsLocallyOwned / GetLocalPlayerID resolve against actual records.
	UCrowdyEntitySubsystem* MakeOwnershipEntitySubsystem(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		ES->SetLocalPlayerID(LocalPlayer);
		return ES;
	}

	// An Owner record for Actor owned by OwnerID.
	FCrowdyEntityRecord MakeOwnerRecord(AActor* Actor, const FGuid& NetID, const FGuid& OwnerID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = OwnerID;
		Record.Role = ECrowdyRole::Owner;
		Record.Participant = Actor;
		return Record;
	}
}

// ReassignOwnership re-points the record's owner and re-derives its role across all three transitions:
// client -> other client (Owner -> RemoteProxy), back to us (RemoteProxy -> Owner), and to host (-> HostOwned
// with an invalid owner id).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipReassignRecordTest,
	"CrowdySDK.State.OwnershipReassignRecord", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipReassignRecordTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);

	// A fixture with no entity component, so ReassignOwnership's component-sync branch is not exercised here.
	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));

	// Transfer away: local -> other. We are no longer the owner, so the role becomes RemoteProxy.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is now the other player"), R->OwnerID, OtherPlayer);
		TestTrue(TEXT("role is RemoteProxy after transferring away"), R->Role == ECrowdyRole::RemoteProxy);
	}

	// Transfer back: other -> local. We own it again, so the role becomes Owner.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is the local player again"), R->OwnerID, LocalPlayer);
		TestTrue(TEXT("role is Owner after transferring back"), R->Role == ECrowdyRole::Owner);
	}

	// Transfer to host: an invalid new-owner id makes it a host-owned world entity (no per-client owner).
	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestFalse(TEXT("host-owned entity has an invalid owner id"), R->OwnerID.IsValid());
		TestTrue(TEXT("role is HostOwned after transferring to host"), R->Role == ECrowdyRole::HostOwned);
	}
	return true;
}

// The compare-and-swap guard: a grant whose expected previous owner no longer matches is dropped (stale), a
// grant re-applying the current owner is a no-op (idempotent duplicate), and a grant with the matching
// expectation applies.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipCompareAndSwapTest,
	"CrowdySDK.State.OwnershipCompareAndSwap", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipCompareAndSwapTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	const FGuid WrongExpectation = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));

	// Stale grant: the expected previous owner does not match the current owner, so it is dropped and the
	// record is unchanged.
	ES->ReassignOwnership(NetID, OtherPlayer, WrongExpectation);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("stale grant leaves the owner unchanged"), R->OwnerID, LocalPlayer);
		TestTrue(TEXT("stale grant leaves the role unchanged"), R->Role == ECrowdyRole::Owner);
	}

	// Idempotent duplicate: re-applying the current owner is a no-op.
	ES->ReassignOwnership(NetID, LocalPlayer, LocalPlayer);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("duplicate grant leaves the owner unchanged"), R->OwnerID, LocalPlayer);
		TestTrue(TEXT("duplicate grant keeps the role"), R->Role == ECrowdyRole::Owner);
	}

	// Matching expectation: the grant applies.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("a matching-expectation grant applies"), R->OwnerID, OtherPlayer);
	}
	return true;
}

// ReassignOwnership syncs the target entity component's cached OwnerID/Role (the component is befriended and
// updated in place), so a reader of the component sees the transfer, not just the subsystem record. Forcing the
// component to Static mode keeps the (world-dependent) continuous-channel move out of this worldless test; that
// path is exercised in PIE.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipComponentSyncTest,
	"CrowdySDK.State.OwnershipComponentSync", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipComponentSyncTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);

	// ACrowdyStateHostOverrideActor carries a UCrowdyEntityComponent default subobject, resolvable via
	// FindComponentByClass even on a NewObject'd actor (as the host-push tests rely on).
	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("actor created"), Actor) || !TestNotNull(TEXT("entity component present"), Actor->Entity.Get()))
	{
		return false;
	}
	// Keep the Dynamic-mode continuous channel (world-dependent) out of this worldless test.
	Actor->Entity->Mode = ECrowdyEntityMode::Static;

	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));

	// Relinquish direction (Owner -> RemoteProxy): transferring away syncs the component off ownership.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer);
	TestEqual(TEXT("component owner id follows the transfer away"), Actor->Entity->GetOwnerID(), OtherPlayer);
	TestTrue(TEXT("component role is RemoteProxy after transferring away"), Actor->Entity->GetRole() == ECrowdyRole::RemoteProxy);

	// Adopt direction (RemoteProxy -> Owner): transferring back syncs the component back onto ownership.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer);
	TestEqual(TEXT("component owner id follows the transfer back"), Actor->Entity->GetOwnerID(), LocalPlayer);
	TestTrue(TEXT("component role is Owner after transferring back"), Actor->Entity->GetRole() == ECrowdyRole::Owner);
	return true;
}

// The replicator retrack: an entity we own is tracked; transferring it away (HandleOwnershipChanged from the
// re-pointed record) drops its tracking; transferring it back re-tracks it. This is the per-entity analogue of
// the host promote/demote path, so a change that broke re-derivation would fail here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipTransferRetrackTest,
	"CrowdySDK.State.OwnershipTransferRetrack", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipTransferRetrackTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeOwnershipRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeOwnershipReplicator(Registry, LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));

	// Registered as an owned entity we drive.
	Rep->HandleEntityRegisteredForTest(NetID);
	TestTrue(TEXT("entity tracked while we own it"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("exactly one owned entry"), Rep->NumOwnedForTest(), 1);

	// Transfer away: the record becomes a RemoteProxy and the retrack drops it.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer);
	Rep->HandleOwnershipChangedForTest(Actor, NetID, OtherPlayer, LocalPlayer);
	TestFalse(TEXT("entity untracked after transferring away"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("nothing owned after transferring away"), Rep->NumOwnedForTest(), 0);

	// Transfer back: the record becomes an Owner again and the retrack picks it back up.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer);
	Rep->HandleOwnershipChangedForTest(Actor, NetID, LocalPlayer, OtherPlayer);
	TestTrue(TEXT("entity re-tracked after transferring back"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("one owned entry again"), Rep->NumOwnedForTest(), 1);
	return true;
}

// Host-scope retrack: transferring an entity to host-owned maintains KnownHostOwnedIDs so a later host
// migration still works. Part 1 (local IS host): the entity stays tracked and is remembered as host-owned; a
// subsequent demotion drops it. Part 2 (local is NOT host): the entity is remembered but not tracked, and a
// later promotion tracks it. A regression that skipped the KnownHostOwnedIDs bookkeeping on transfer would
// break the migration and fail here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipTransferHostScopeTest,
	"CrowdySDK.State.OwnershipTransferHostScope", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipTransferHostScopeTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeOwnershipRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherHost = FGuid::NewGuid();

	// Part 1: local IS host. An entity we own is transferred to host-owned; it stays tracked (we are host) and is
	// now remembered as host-owned, so losing host demotes it.
	{
		UCrowdyStateReplicator* Rep = MakeOwnershipReplicator(Registry, LocalPlayer);
		UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);
		Rep->SetEntitySubsystemForTest(ES);
		Rep->SetHostIDForTest(LocalPlayer); // local IS host

		ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
		const FGuid NetID = FGuid::NewGuid();
		ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));
		Rep->HandleEntityRegisteredForTest(NetID);
		TestTrue(TEXT("owned entity tracked"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("not yet remembered as host-owned"), Rep->NumKnownHostOwnedForTest(), 0);

		// Transfer to host-owned.
		ES->ReassignOwnership(NetID, FGuid(), LocalPlayer);
		Rep->HandleOwnershipChangedForTest(Actor, NetID, FGuid(), LocalPlayer);
		TestTrue(TEXT("host-owned entity still tracked while we are host"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("now remembered as host-owned"), Rep->NumKnownHostOwnedForTest(), 1);

		// Lose host: the (now host-owned) entity is demoted.
		Rep->SetHostIDForTest(OtherHost);
		Rep->HandleHostChangedForTest(OtherHost, LocalPlayer);
		TestFalse(TEXT("demoted host drops the transferred world entity"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("nothing tracked after demotion"), Rep->NumOwnedForTest(), 0);
	}

	// Part 2: local is NOT host. A proxy entity is transferred to host-owned; it is remembered but not tracked,
	// and becoming host promotes it.
	{
		UCrowdyStateReplicator* Rep = MakeOwnershipReplicator(Registry, LocalPlayer);
		UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);
		Rep->SetEntitySubsystemForTest(ES);
		Rep->SetHostIDForTest(OtherHost); // someone else is host

		ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
		const FGuid NetID = FGuid::NewGuid();
		// Owned by another client (a proxy on this client), so it is not tracked to begin with.
		FCrowdyEntityRecord ProxyRecord;
		ProxyRecord.NetID = NetID;
		ProxyRecord.OwnerID = FGuid::NewGuid();
		ProxyRecord.Role = ECrowdyRole::RemoteProxy;
		ProxyRecord.Participant = Actor;
		ES->RegisterEntity(ProxyRecord);
		Rep->HandleEntityRegisteredForTest(NetID);
		TestEqual(TEXT("proxy is not tracked"), Rep->NumOwnedForTest(), 0);

		// Transfer to host-owned: a non-host remembers it but does not track it.
		ES->ReassignOwnership(NetID, FGuid(), ProxyRecord.OwnerID);
		Rep->HandleOwnershipChangedForTest(Actor, NetID, FGuid(), ProxyRecord.OwnerID);
		TestFalse(TEXT("non-host does not track a host-owned entity"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("non-host remembers the host-owned id"), Rep->NumKnownHostOwnedForTest(), 1);

		// Become host: the remembered host-owned entity is promoted to tracked.
		Rep->SetHostIDForTest(LocalPlayer);
		Rep->HandleHostChangedForTest(LocalPlayer, OtherHost);
		TestTrue(TEXT("promotion tracks the transferred world entity"), Rep->IsTrackedForTest(NetID));
		TestEqual(TEXT("exactly one owned entry after promotion"), Rep->NumOwnedForTest(), 1);
	}
	return true;
}

// Host-owned -> client transfer: granting a host-owned (world) entity to a client exercises the compare-and-swap
// SKIP path (a host-owned source has no owner id, so the expected-previous is invalid) and the HostOwned -> Owner
// retrack (KnownHostOwnedIDs must drop the id and the entity re-tracks as a normal owned entity). Local is host.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipTransferHostToClientTest,
	"CrowdySDK.State.OwnershipTransferHostToClient", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipTransferHostToClientTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeOwnershipRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyStateReplicator* Rep = MakeOwnershipReplicator(Registry, LocalPlayer);
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);
	Rep->SetEntitySubsystemForTest(ES);
	Rep->SetHostIDForTest(LocalPlayer); // local IS host

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();

	// A host-owned world entity (no per-client owner), tracked because we are host.
	FCrowdyEntityRecord HostOwnedRecord;
	HostOwnedRecord.NetID = NetID;
	HostOwnedRecord.Role = ECrowdyRole::HostOwned;
	HostOwnedRecord.Participant = Actor;
	ES->RegisterEntity(HostOwnedRecord);
	Rep->HandleEntityRegisteredForTest(NetID);
	TestTrue(TEXT("host-owned entity tracked while host"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("remembered as host-owned"), Rep->NumKnownHostOwnedForTest(), 1);

	// Grant it to a client (us). A host-owned source has an invalid owner id, so the expected-previous is invalid
	// and the compare-and-swap is skipped; the entity becomes client-owned.
	ES->ReassignOwnership(NetID, LocalPlayer, FGuid());
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is now the client"), R->OwnerID, LocalPlayer);
		TestTrue(TEXT("role is Owner after the host-to-client transfer"), R->Role == ECrowdyRole::Owner);
	}

	// The retrack drops it from the host-owned set and tracks it as a normal owned entity.
	Rep->HandleOwnershipChangedForTest(Actor, NetID, LocalPlayer, FGuid());
	TestTrue(TEXT("entity still tracked (now as owner)"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("no longer remembered as host-owned"), Rep->NumKnownHostOwnedForTest(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
