#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/Package.h"

// Ownership-transfer coverage: the entity subsystem's ReassignOwnership (record re-point, role derivation,
// compare-and-swap, entity-component cache sync) and the state replicator's HandleOwnershipChanged retrack
// (untrack the old owner, track the new one). The RPC transport (RequestOwnership / GrantOwnership) rides the
// existing, separately-tested RPC channel stack, so these tests drive the two substantive halves directly through
// the same test seams the Leader-change tests use.
namespace
{
	constexpr EAutomationTestFlags CrowdyStateOwnershipTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package NewObject
	// trips a ClassWithin ensure, so outer it to a bare GameInstance.
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
// client -> other client (Owner -> RemoteProxy), back to us (RemoteProxy -> Owner), and to the world (-> HostOwned
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
	// Keep the continuous channel (world-dependent) out of this worldless test.
	Actor->Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;

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
// the Leader promote/demote path, so a change that broke re-derivation would fail here.
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

// Handing an entity to the world stops this client writing it, even on the Leader: a World actor has no writer,
// and it is not a replicated subsystem, so no Leader change brings it back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipTransferToWorldStopsWritingTest,
	"CrowdySDK.State.OwnershipTransferToWorldStopsWriting", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipTransferToWorldStopsWritingTest::RunTest(const FString& Parameters)
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
	Rep->SetLeaderIDForTest(LocalPlayer);

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));
	Rep->HandleEntityRegisteredForTest(NetID);
	TestTrue(TEXT("owned entity tracked"), Rep->IsTrackedForTest(NetID));

	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer);
	Rep->HandleOwnershipChangedForTest(Actor, NetID, FGuid(), LocalPlayer);
	TestFalse(TEXT("a World entity is not tracked, even on the Leader"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("nor remembered for a Leader change"), Rep->NumKnownSubsystemsForTest(), 0);

	Rep->HandleLeaderChangedForTest(LocalPlayer, FGuid());
	TestFalse(TEXT("and a Leader change does not bring it back"), Rep->IsTrackedForTest(NetID));
	return true;
}

// World -> client transfer: granting a World entity to a client exercises the compare-and-swap SKIP path (a World
// source has no owner id, so the expected-previous is invalid) and the HostOwned -> Owner retrack: the entity
// becomes tracked as a normal owned entity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipTransferWorldToClientTest,
	"CrowdySDK.State.OwnershipTransferWorldToClient", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipTransferWorldToClientTest::RunTest(const FString& Parameters)
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

	ACrowdyStateSendTestActor* Actor = NewObject<ACrowdyStateSendTestActor>();
	if (!TestNotNull(TEXT("actor created"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();

	// A World entity (no per-client owner), which no client writes.
	FCrowdyEntityRecord WorldRecord;
	WorldRecord.NetID = NetID;
	WorldRecord.Role = ECrowdyRole::HostOwned;
	WorldRecord.Participant = Actor;
	ES->RegisterEntity(WorldRecord);
	Rep->HandleEntityRegisteredForTest(NetID);
	TestFalse(TEXT("a World entity is not tracked"), Rep->IsTrackedForTest(NetID));

	// Grant it to a client (us). A World source has an invalid owner id, so the expected-previous is invalid and
	// the compare-and-swap is skipped; the entity becomes client-owned.
	ES->ReassignOwnership(NetID, LocalPlayer, FGuid());
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is now the client"), R->OwnerID, LocalPlayer);
		TestTrue(TEXT("role is Owner after the world-to-client transfer"), R->Role == ECrowdyRole::Owner);
	}

	Rep->HandleOwnershipChangedForTest(Actor, NetID, LocalPlayer, FGuid());
	TestTrue(TEXT("entity tracked as owner"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("and never remembered as a subsystem"), Rep->NumKnownSubsystemsForTest(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
