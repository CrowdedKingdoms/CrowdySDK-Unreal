#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Ownership/CrowdyOwnershipTransfer.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

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

	// An Owner record for Actor owned by WriterID.
	FCrowdyEntityRecord MakeOwnerRecord(AActor* Actor, const FGuid& NetID, const FGuid& WriterID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.WriterID = WriterID;
		Record.Role = ECrowdyRole::Owner;
		Record.Participant = Actor;
		return Record;
	}

	// A peer's entity this client draws: WriterID writes it and OwnerUserID is the user it claimed.
	FCrowdyEntityRecord MakeProxyRecord(AActor* Actor, const FGuid& WriterID, const int64 OwnerUserID)
	{
		FCrowdyEntityRecord Record = MakeOwnerRecord(Actor, FGuid::NewGuid(), WriterID);
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.OwnerUserID = OwnerUserID;
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
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is now the other player"), R->WriterID, OtherPlayer);
		TestTrue(TEXT("role is RemoteProxy after transferring away"), R->Role == ECrowdyRole::RemoteProxy);
	}

	// Transfer back: other -> local. We own it again, so the role becomes Owner.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is the local player again"), R->WriterID, LocalPlayer);
		TestTrue(TEXT("role is Owner after transferring back"), R->Role == ECrowdyRole::Owner);
	}

	// Transfer to host: an invalid new-owner id makes it a host-owned world entity (no per-client owner).
	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestFalse(TEXT("host-owned entity has an invalid owner id"), R->WriterID.IsValid());
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
	ES->ReassignOwnership(NetID, OtherPlayer, WrongExpectation, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("stale grant leaves the owner unchanged"), R->WriterID, LocalPlayer);
		TestTrue(TEXT("stale grant leaves the role unchanged"), R->Role == ECrowdyRole::Owner);
	}

	// Idempotent duplicate: re-applying the current owner is a no-op.
	ES->ReassignOwnership(NetID, LocalPlayer, LocalPlayer, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("duplicate grant leaves the owner unchanged"), R->WriterID, LocalPlayer);
		TestTrue(TEXT("duplicate grant keeps the role"), R->Role == ECrowdyRole::Owner);
	}

	// Matching expectation: the grant applies.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("a matching-expectation grant applies"), R->WriterID, OtherPlayer);
	}
	return true;
}

// ReassignOwnership syncs the target entity component's cached WriterID/Role (the component is befriended and
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
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, 0);
	TestEqual(TEXT("component owner id follows the transfer away"), Actor->Entity->GetWriterID(), OtherPlayer);
	TestTrue(TEXT("component role is RemoteProxy after transferring away"), Actor->Entity->GetRole() == ECrowdyRole::RemoteProxy);

	// Adopt direction (RemoteProxy -> Owner): transferring back syncs the component back onto ownership.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer, 0);
	TestEqual(TEXT("component owner id follows the transfer back"), Actor->Entity->GetWriterID(), LocalPlayer);
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
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, 0);
	Rep->HandleOwnershipChangedForTest(Actor, NetID, OtherPlayer, LocalPlayer);
	TestFalse(TEXT("entity untracked after transferring away"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("nothing owned after transferring away"), Rep->NumOwnedForTest(), 0);

	// Transfer back: the record becomes an Owner again and the retrack picks it back up.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer, 0);
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

	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer, 0);
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
	ES->ReassignOwnership(NetID, LocalPlayer, FGuid(), 0);
	if (const FCrowdyEntityRecord* R = ES->FindRecord(NetID))
	{
		TestEqual(TEXT("owner is now the client"), R->WriterID, LocalPlayer);
		TestTrue(TEXT("role is Owner after the world-to-client transfer"), R->Role == ECrowdyRole::Owner);
	}

	Rep->HandleOwnershipChangedForTest(Actor, NetID, LocalPlayer, FGuid());
	TestTrue(TEXT("entity tracked as owner"), Rep->IsTrackedForTest(NetID));
	TestEqual(TEXT("and never remembered as a subsystem"), Rep->NumKnownSubsystemsForTest(), 0);
	return true;
}

// A grant names the new owner's user, and the record keeps it; a grant that keeps the writer only fills a user the record
// did not know, never replaces a known one, and announces no change of owner, because none happened.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipGrantCarriesOwnerUserTest,
	"CrowdySDK.State.OwnershipGrantCarriesTheNewOwnerUser", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipGrantCarriesOwnerUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 OtherUser = 5150;
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);

	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("actor created"), Actor) || !TestNotNull(TEXT("entity component present"), Actor->Entity.Get()))
	{
		return false;
	}
	Actor->Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));

	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, OtherUser);
	const FCrowdyEntityRecord* Moved = ES->FindRecord(NetID);
	if (!TestNotNull(TEXT("the record is still there"), Moved))
	{
		return false;
	}
	TestEqual(TEXT("the grant moves the writer"), Moved->WriterID, OtherPlayer);
	TestEqual(TEXT("and stores the user it names"), Moved->OwnerUserID, OtherUser);
	TestEqual(TEXT("which is the owner user this client reads"), ES->GetOwnerUserID(*Moved), OtherUser);

	ACrowdyStateHostOverrideActor* Taught = NewObject<ACrowdyStateHostOverrideActor>();
	Taught->Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;
	const FCrowdyEntityRecord Unknown = MakeProxyRecord(Taught, OtherPlayer, 0);
	ES->RegisterEntity(Unknown);
	const int32 Announced = Taught->Entity->OwnershipAnnouncementCount;
	ES->ReassignOwnership(Unknown.NetID, OtherPlayer, OtherPlayer, OtherUser);
	TestEqual(TEXT("a grant that keeps the writer still teaches the user"), ES->FindRecord(Unknown.NetID)->OwnerUserID, OtherUser);
	TestEqual(TEXT("without announcing a change of owner"), Taught->Entity->OwnershipAnnouncementCount, Announced);

	ES->ReassignOwnership(Unknown.NetID, OtherPlayer, OtherPlayer, OtherUser + 1);
	TestEqual(TEXT("but never replaces a user it knows"), ES->FindRecord(Unknown.NetID)->OwnerUserID, OtherUser);
	return true;
}

// A grant with no user (0) is a change of writer to a user nobody knows yet: the previous owner's user is not kept.
// A grant that keeps the writer and names no user leaves the known one alone, and 0 never finds anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipGrantWithNoUserTest,
	"CrowdySDK.State.OwnershipGrantWithNoUserLeavesItUnknown", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipGrantWithNoUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 PreviousUser = 900;
	constexpr int64 KeptUser = 901;
	const FGuid PreviousWriter = FGuid::NewGuid();
	const FGuid NewWriter = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(FGuid::NewGuid());

	ACrowdyStateSendTestActor* MovedActor = NewObject<ACrowdyStateSendTestActor>();
	ACrowdyStateSendTestActor* KeptActor = NewObject<ACrowdyStateSendTestActor>();
	const FCrowdyEntityRecord Moved = MakeProxyRecord(MovedActor, PreviousWriter, PreviousUser);
	const FCrowdyEntityRecord Kept = MakeProxyRecord(KeptActor, NewWriter, KeptUser);
	ES->RegisterEntity(Moved);
	ES->RegisterEntity(Kept);

	ES->ReassignOwnership(Moved.NetID, NewWriter, PreviousWriter, 0);
	TestEqual(TEXT("a grant with no user moves the writer"), ES->FindRecord(Moved.NetID)->WriterID, NewWriter);
	TestEqual(TEXT("and leaves the user unknown rather than the previous owner's"), ES->FindRecord(Moved.NetID)->OwnerUserID, static_cast<int64>(0));
	TestEqual(TEXT("so the previous owner's user no longer finds it"), ES->GetEntitiesByOwnerUser(PreviousUser).Num(), 0);

	ES->ReassignOwnership(Kept.NetID, NewWriter, NewWriter, 0);
	TestEqual(TEXT("a grant that keeps the writer and names no user keeps the known one"), ES->FindRecord(Kept.NetID)->OwnerUserID, KeptUser);

	const TArray<AActor*> Found = ES->GetEntitiesByOwnerUser(KeptUser);
	TestTrue(TEXT("a known user finds its entity"), Found.Num() == 1 && Found[0] == KeptActor);
	TestEqual(TEXT("an unknown user finds nothing, though an entity has no user"), ES->GetEntitiesByOwnerUser(0).Num(), 0);
	return true;
}

// The compare-and-swap reads writer ids only: a stale grant naming the current user is still dropped, and a current
// grant naming another user still applies.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnershipGrantUserNotInSwapTest,
	"CrowdySDK.State.OwnershipGrantUserIsNotPartOfTheSwap", CrowdyStateOwnershipTestFlags)
bool FCrowdyStateOwnershipGrantUserNotInSwapTest::RunTest(const FString& Parameters)
{
	constexpr int64 CurrentUser = 31;
	constexpr int64 NextUser = 32;
	const FGuid CurrentWriter = FGuid::NewGuid();
	const FGuid NextWriter = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(FGuid::NewGuid());

	const FCrowdyEntityRecord Record = MakeProxyRecord(NewObject<ACrowdyStateSendTestActor>(), CurrentWriter, CurrentUser);
	ES->RegisterEntity(Record);

	ES->ReassignOwnership(Record.NetID, NextWriter, FGuid::NewGuid(), CurrentUser);
	TestEqual(TEXT("a stale grant is dropped although it names the current user"), ES->FindRecord(Record.NetID)->WriterID, CurrentWriter);
	TestEqual(TEXT("and the user is untouched"), ES->FindRecord(Record.NetID)->OwnerUserID, CurrentUser);

	ES->ReassignOwnership(Record.NetID, NextWriter, CurrentWriter, NextUser);
	TestEqual(TEXT("a grant from the current writer applies although it names another user"), ES->FindRecord(Record.NetID)->WriterID, NextWriter);
	TestEqual(TEXT("with that user"), ES->FindRecord(Record.NetID)->OwnerUserID, NextUser);
	return true;
}

// A request carries the user the requesting session claims, and the entity's writer hands it to game code with the
// requester's actor id, so a grant can name both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnershipRequestCarriesUserTest,
	"CrowdySDK.Ownership.RequestCarriesTheRequestersUser", CrowdyStateOwnershipTestFlags)
bool FCrowdyOwnershipRequestCarriesUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 RequesterUser = 7070;
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid Requester = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(LocalPlayer);

	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("actor created"), Actor) || !TestNotNull(TEXT("entity component present"), Actor->Entity.Get()))
	{
		return false;
	}
	Actor->Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;
	const FGuid NetID = FGuid::NewGuid();
	ES->RegisterEntity(MakeOwnerRecord(Actor, NetID, LocalPlayer));
	Actor->Entity->SetCollaboratorsForTest(ES, nullptr);
	Actor->Entity->AssignPooledIdentity(NetID, LocalPlayer, 0, ECrowdyRole::Owner, 0);

	const TStrongObjectPtr<UCrowdyOwnershipRequestListener> Listener(NewObject<UCrowdyOwnershipRequestListener>(GetTransientPackage()));
	ES->OnOwnershipRequested.AddDynamic(Listener.Get(), &UCrowdyOwnershipRequestListener::OnRequested);

	Actor->Entity->RequestOwnership_Implementation(Requester, RequesterUser);
	if (!TestEqual(TEXT("the writer hands the request to game code once"), Listener->Calls, 1))
	{
		return false;
	}
	TestTrue(TEXT("for the entity"), Listener->LastTarget.Get() == Actor);
	TestEqual(TEXT("naming the requesting session"), Listener->LastRequesterID, Requester);
	TestEqual(TEXT("and the user it claims"), Listener->LastRequesterUserID, RequesterUser);
	return true;
}

// A peer's avatar sends no spawn event, so this client's record of it names no writer. Granting to it names the
// avatar's own id, which is its session's, and the user this client knows for it; a record that names a writer names
// that writer, and one that names none and is no avatar names nobody.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnershipGrantToRemoteAvatarTest,
	"CrowdySDK.Ownership.GrantToARemoteAvatarNamesItsSession", CrowdyStateOwnershipTestFlags)
bool FCrowdyOwnershipGrantToRemoteAvatarTest::RunTest(const FString& Parameters)
{
	constexpr int64 SpawnerUser = 6060;
	const FGuid Session = FGuid::NewGuid();
	const FGuid Spawner = FGuid::NewGuid();
	UCrowdyEntitySubsystem* ES = MakeOwnershipEntitySubsystem(FGuid::NewGuid());

	FCrowdyEntityRecord Avatar = MakeProxyRecord(NewObject<ACrowdyStateSendTestActor>(), FGuid(), 0);
	Avatar.NetID = Session;
	Avatar.bAvatar = true;
	ES->RegisterEntity(Avatar);
	const FCrowdyEntityRecord* AvatarRecord = ES->FindRecord(Session);
	if (!TestNotNull(TEXT("the avatar is registered"), AvatarRecord))
	{
		return false;
	}
	TestEqual(TEXT("a grant to a remote avatar names its session"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(*AvatarRecord), Session);
	TestEqual(TEXT("with its user unknown here"), ES->GetOwnerUserID(*AvatarRecord), static_cast<int64>(0));

	const FCrowdyEntityRecord Spawned = MakeProxyRecord(NewObject<ACrowdyStateSendTestActor>(), Spawner, SpawnerUser);
	ES->RegisterEntity(Spawned);
	const FCrowdyEntityRecord* SpawnedRecord = ES->FindRecord(Spawned.NetID);
	TestEqual(TEXT("a grant to a player's other entity names its writer"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(*SpawnedRecord), Spawner);
	TestEqual(TEXT("and the user it claimed"), ES->GetOwnerUserID(*SpawnedRecord), SpawnerUser);

	FCrowdyEntityRecord Named = MakeProxyRecord(NewObject<ACrowdyStateSendTestActor>(), Spawner, SpawnerUser);
	Named.bAvatar = true;
	TestEqual(TEXT("a flagged entity whose record names a writer is granted to that writer"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(Named), Spawner);

	const FCrowdyEntityRecord Unowned = MakeProxyRecord(NewObject<ACrowdyStateSendTestActor>(), FGuid(), 0);
	TestFalse(TEXT("an entity that is no avatar and names no writer names nobody"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(Unowned).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
