#include "Replication/RPC/CrowdyRpcTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Replication/Subsystems/CrowdyActorDepartureTestSupport.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Serialization/MemoryWriter.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/StrongObjectPtr.h"

// Owning Client is the one targeted recipient: it reaches whichever client writes the entity, in every ownership mode.
namespace
{
	constexpr EAutomationTestFlags CrowdyOwningClientTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	void Hear(UCrowdyActorTracker* Tracker, const FGuid& NetID, const FCrowdyActorId& WireId)
	{
		FCrowdyActorUpdate Update;
		Update.UUID = NetID;
		Tracker->NoteHeardForTest({ Update }, 0.0);

		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->GUID = NetID;
		Message->UUID = WireId;
		const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> Shared = Message;
		Tracker->HandleActorUpdateDeliveryForTest(FCrowdyDelivery(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION,
			Message->GetPayloadKey(), Message->GetPayload(), Message->GetPayloadBytes(), Shared));
	}

	FGuid RegisterRemote(UCrowdyEntitySubsystem* Entities, const bool bLeaderOwned, const FGuid& OwnerID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.OwnerID = OwnerID;
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.bLeaderOwned = bLeaderOwned;
		Record.Participant = NewObject<UCrowdyRpcSubsystemTestTarget>(GetTransientPackage());
		Entities->RegisterEntity(Record);
		return Record.NetID;
	}

	UCrowdyAutoRegistry* MakeOwnerCallRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
		Registry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());
		Registry->UpdateClassRpcFunctions(ACrowdyRpcActorTestTarget::StaticClass());
		return Registry;
	}

	FCrowdyRpcCall MakeOwnerCall(const FGuid& EntityID)
	{
		FCrowdyRpcCall Call;
		if (UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(), TEXT("SubOwnerOnly_Implementation")))
		{
			Call = FCrowdyRPC::MarshalCall(Fn, FCrowdyRPC::GetFnInfo(Fn), &UCrowdyRpcSubsystemTestTarget::SubOwnerOnly_Implementation, 7);
		}
		Call.EntityID = EntityID;
		Call.SenderID = FGuid::NewGuid();
		return Call;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnerRouteTableTest,
	"CrowdySDK.RPC.OwnerRouteTable", CrowdyOwningClientTestFlags)
bool FCrowdyOwnerRouteTableTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* What;
		bool bWorldEntity;
		bool bWorldOwnerKnown;
		bool bLeaderWithoutOwner;
		bool bNoRegionToReach;
		bool bCarriesText;
		ECrowdyRpcRoute Route;
	};
	const FCase Cases[] =
	{
		{ TEXT("a player's entity, heard"), false, false, false, false, false, ECrowdyRpcRoute::SpatialToOwner },
		{ TEXT("a player's entity, heard, with a host about"), false, true, false, false, false, ECrowdyRpcRoute::SpatialToOwner },
		{ TEXT("no region to reach the owner in"), false, false, false, true, false, ECrowdyRpcRoute::OwnerOverSession },
		{ TEXT("a World entity with an elected host"), true, true, false, false, false, ECrowdyRpcRoute::OwnerOverSession },
		{ TEXT("a World entity with none"), true, false, false, false, false, ECrowdyRpcRoute::NoOwner },
		{ TEXT("a Leader entity between Leaders"), false, false, true, true, false, ECrowdyRpcRoute::NoOwner },
		{ TEXT("text to a heard writer stays a direct message"), false, false, false, false, true, ECrowdyRpcRoute::SingleActorToOwner },
		{ TEXT("text never falls back to the session channel"), false, false, false, true, true, ECrowdyRpcRoute::TextHasNoRoute },
		{ TEXT("text to the elected host stays a direct message"), true, true, false, false, true, ECrowdyRpcRoute::SingleActorToHost },
		{ TEXT("text with no writer is dropped as any call is"), true, false, false, false, true, ECrowdyRpcRoute::NoOwner },
	};

	for (const FCase& Case : Cases)
	{
		TestTrue(Case.What, FCrowdyRPC::DecideOwnerRoute(Case.bWorldEntity, Case.bWorldOwnerKnown,
			Case.bLeaderWithoutOwner, Case.bNoRegionToReach, Case.bCarriesText) == Case.Route);
	}
	return true;
}

// An owner-only event that can hold typed text must stay under the rules for private messages between players.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCallCarriesTextTest,
	"CrowdySDK.RPC.CallCarriesText", CrowdyOwningClientTestFlags)
bool FCrowdyCallCarriesTextTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Function; bool bCarriesText; };
	const FCase Cases[] =
	{
		{ TEXT("WithNumber"), false },
		{ TEXT("WithName"), false },
		{ TEXT("WithString"), true },
		{ TEXT("WithText"), true },
		{ TEXT("WithList"), true },
		{ TEXT("WithSet"), true },
		{ TEXT("WithMapValue"), true },
		{ TEXT("WithNestedStruct"), true },
	};

	for (const FCase& Case : Cases)
	{
		UFunction* Fn = UCrowdyRpcTextTestTarget::StaticClass()->FindFunctionByName(Case.Function);
		if (!TestNotNull(Case.Function, Fn))
		{
			return false;
		}
		TestEqual(Case.Function, FCrowdyRPC::BuildFnInfo(Fn).bCarriesText, Case.bCarriesText);
	}
	return true;
}

// A client can hold an owner another client has moved on from, while it hears the entity under the new one's id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventNamesNobodyOnAStaleOwnerTest,
	"CrowdySDK.RPC.EventNamesNobodyOnAStaleOwner", CrowdyOwningClientTestFlags)
bool FCrowdyEventNamesNobodyOnAStaleOwnerTest::RunTest(const FString& Parameters)
{
	const FGuid OldOwner = FGuid::NewGuid();
	const FGuid NewOwner = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->SetupQueuesForTest();

	const FGuid NetID = RegisterRemote(Entities, false, OldOwner);
	TestEqual(TEXT("with nothing heard the record decides"), Entities->ResolveEventAddresseeForTest(NetID, OldOwner, Tracker.Get()), OldOwner);

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Appeared = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
	Appeared->UUID = FCrowdyActorId::FromGuid(NetID);
	FCrowdyActorUpdate Appearance;
	Appearance.UUID = NetID;
	Appearance.Message = Appeared;
	Tracker->RecordAppearancesForTest({ Appearance });
	FCrowdyActorId Stored;
	TestTrue(TEXT("the plain id is on record"), Tracker->FindWireId(NetID, Stored));
	TestEqual(TEXT("an id with no owner in it says nothing"), Entities->ResolveEventAddresseeForTest(NetID, OldOwner, Tracker.Get()), OldOwner);

	Tracker->HandleEntityOwnerChangedForTest(NetID, OldOwner);
	TestEqual(TEXT("an id that agrees keeps the name"), Entities->ResolveEventAddresseeForTest(NetID, OldOwner, Tracker.Get()), OldOwner);

	Tracker->HandleEntityOwnerChangedForTest(NetID, NewOwner);
	TestFalse(TEXT("an id naming another owner names nobody"), Entities->ResolveEventAddresseeForTest(NetID, OldOwner, Tracker.Get()).IsValid());

	Tracker->ReleaseQueuesForTest();
	return true;
}

// The sender did not know who owns it, so the event reached everyone in range: only the writer runs it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUnnamedCallRunsOnTheWriterTest,
	"CrowdySDK.RPC.UnnamedCallRunsOnTheWriter", CrowdyOwningClientTestFlags)
bool FCrowdyUnnamedCallRunsOnTheWriterTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	UCrowdyEventRouter* Router = MakeRouter(MakeOwnerCallRegistry(), Entities);

	UCrowdyRpcSubsystemTestTarget* Drawn = NewObject<UCrowdyRpcSubsystemTestTarget>();
	UCrowdyRpcSubsystemTestTarget* Written = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid DrawnID = FGuid::NewGuid();
	const FGuid WrittenID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, DrawnID, Drawn);
	RegisterParticipantAs(Entities, WrittenID, Written, ECrowdyRole::Owner, Local);

	const auto Deliver = [Router](const FGuid& EntityID)
	{
		const FInstancedStruct Payload = FInstancedStruct::Make(MakeOwnerCall(EntityID));
		FCrowdyInboundEvent Event;
		Event.Payload = &Payload;
		Event.Target = ECrowdyTarget::Owner;
		Router->DispatchEvent(Event);
	};

	Deliver(DrawnID);
	TestEqual(TEXT("a client that only draws the entity ignores it"), Drawn->CallCount, 0);

	Deliver(FGuid::NewGuid());
	TestEqual(TEXT("a client without the entity does not wait for it"), Router->NumDeferredForTest(), 0);

	Deliver(WrittenID);
	TestEqual(TEXT("the writer runs it"), Written->CallCount, 1);
	return true;
}

// A targeted message reaches whoever sent the id, so one to an id nobody has sent reaches nobody.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdUnheardTest,
	"CrowdySDK.RPC.WireIdUnheard", CrowdyOwningClientTestFlags)
bool FCrowdyWireIdUnheardTest::RunTest(const FString& Parameters)
{
	const FGuid Owner = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->SetupQueuesForTest();

	const FGuid Avatar = FGuid::NewGuid();
	RegisterParticipantAs(Entities, Avatar, NewObject<UCrowdyRpcSubsystemTestTarget>(GetTransientPackage()), ECrowdyRole::RemoteProxy, Avatar);
	TestFalse(TEXT("an avatar keeps its route, heard or not"), Entities->IsWireIdUnheardForTest(Avatar, Tracker.Get()));

	const FGuid Still = RegisterRemote(Entities, false, Owner);
	TestTrue(TEXT("an entity that sends no movement has no id to reach"), Entities->IsWireIdUnheardForTest(Still, Tracker.Get()));

	const FGuid Leader = RegisterRemote(Entities, true, Owner);
	TestTrue(TEXT("nor has a Leader's entity not heard yet"), Entities->IsWireIdUnheardForTest(Leader, Tracker.Get()));
	Hear(Tracker.Get(), Leader, FCrowdyActorId::ForOwner(Leader, Owner));
	TestFalse(TEXT("which is reachable once heard"), Entities->IsWireIdUnheardForTest(Leader, Tracker.Get()));

	const FGuid NewOwner = FGuid::NewGuid();
	const FGuid Moved = RegisterRemote(Entities, false, Owner);
	const FCrowdyActorId OldId = FCrowdyActorId::FromGuid(Moved);
	Hear(Tracker.Get(), Moved, OldId);
	TestFalse(TEXT("an entity heard under its first id is reachable"), Entities->IsWireIdUnheardForTest(Moved, Tracker.Get()));

	Tracker->HandleEntityOwnerChangedForTest(Moved, NewOwner);
	TestTrue(TEXT("until it changes hands"), Entities->IsWireIdUnheardForTest(Moved, Tracker.Get()));
	Hear(Tracker.Get(), Moved, OldId);
	TestTrue(TEXT("an update under the old id, however late it is stamped, does not count"), Entities->IsWireIdUnheardForTest(Moved, Tracker.Get()));
	Hear(Tracker.Get(), Moved, FCrowdyActorId::ForOwner(Moved, NewOwner));
	TestFalse(TEXT("one under the new id does"), Entities->IsWireIdUnheardForTest(Moved, Tracker.Get()));

	Tracker->HandleEntityOwnerChangedForTest(Moved, Owner);
	TestTrue(TEXT("and the next move starts over"), Entities->IsWireIdUnheardForTest(Moved, Tracker.Get()));

	TestFalse(TEXT("no tracker means no opinion"), Entities->IsWireIdUnheardForTest(Leader, nullptr));
	Tracker->ReleaseQueuesForTest();
	return true;
}

// The addressee travels last, so an older peer reads the call and a forged length is refused before it allocates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelCallCodecTest,
	"CrowdySDK.RPC.ChannelCallCodec", CrowdyOwningClientTestFlags)
bool FCrowdyChannelCallCodecTest::RunTest(const FString& Parameters)
{
	FCrowdyRpcCall Call;
	Call.ClassID = 11;
	Call.FunctionID = 22;
	Call.EntityID = FGuid::NewGuid();
	Call.SenderID = FGuid::NewGuid();
	Call.ParamBlob = { 1, 2, 3 };
	const FGuid Addressee = FGuid::NewGuid();

	TArray<uint8> Payload;
	FCrowdyRPC::EncodeChannelRpc(Call, 0, Payload, Addressee);

	FCrowdyRpcCall Decoded;
	uint8 Flags = 0;
	FGuid Read;
	TestTrue(TEXT("an addressed call decodes"), FCrowdyRPC::DecodeChannelRpc(Payload, Decoded, Flags, &Read));
	TestEqual(TEXT("with its addressee"), Read, Addressee);
	TestEqual(TEXT("and its parameters"), Decoded.ParamBlob, Call.ParamBlob);

	FCrowdyRpcCall ForAnOlderPeer;
	TestTrue(TEXT("a reader that does not ask still gets the call"), FCrowdyRPC::DecodeChannelRpc(Payload, ForAnOlderPeer, Flags));
	TestEqual(TEXT("whole"), ForAnOlderPeer.ParamBlob, Call.ParamBlob);

	FCrowdyRPC::EncodeChannelRpc(Call, 0, Payload);
	FGuid Unset;
	TestTrue(TEXT("a call for everyone decodes"), FCrowdyRPC::DecodeChannelRpc(Payload, Decoded, Flags, &Unset));
	TestFalse(TEXT("and names nobody"), Unset.IsValid());

	FCrowdyRPC::EncodeChannelRpc(Call, CrowdyChannelRpcFlagAddressed, Payload);
	AddExpectedMessage(TEXT("ran out of bytes"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	TestFalse(TEXT("a flag with no addressee behind it is refused"), FCrowdyRPC::DecodeChannelRpc(Payload, Decoded, Flags, &Unset));

	// The blob's count sits just before its bytes. A count that large would ask for 2 GB before reading anything.
	FCrowdyRPC::EncodeChannelRpc(Call, 0, Payload);
	FMemory::Memset(Payload.GetData() + Payload.Num() - Call.ParamBlob.Num() - sizeof(int32), 0x7F, sizeof(int32));
	FCrowdyRpcCall Forged;
	TestFalse(TEXT("a forged blob length is refused"), FCrowdyRPC::DecodeChannelRpc(Payload, Forged, Flags));
	TestEqual(TEXT("before anything is allocated"), Forged.ParamBlob.Max(), 0);

	return true;
}

// A channel call names the client it is for, so a client that wrongly believes it owns the entity leaves it alone,
// and the client it names waits for the change of owner it has not heard of yet.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAddressedCallTest,
	"CrowdySDK.RPC.AddressedCallRunsOnTheClientItNames", CrowdyOwningClientTestFlags)
bool FCrowdyAddressedCallTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	UCrowdyEventRouter* Router = MakeRouter(MakeOwnerCallRegistry(), Entities);

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterParticipantAs(Entities, EntityID, Target, ECrowdyRole::Owner, Local);

	const FCrowdyRpcCall Call = MakeOwnerCall(EntityID);
	Router->ReceiveChannelRpcCall(Call, Other);
	TestEqual(TEXT("named for someone else, it does not run here, whoever this client thinks owns it"), Target->CallCount, 0);
	TestEqual(TEXT("and does not wait"), Router->NumDeferredForTest(), 0);

	Router->ReceiveChannelRpcCall(Call, Local);
	TestEqual(TEXT("named for this client, it runs"), Target->CallCount, 1);

	Entities->ReassignOwnership(EntityID, Other, Local);
	Router->ReceiveChannelRpcCall(Call, Local);
	TestEqual(TEXT("not while this client does not write it"), Target->CallCount, 1);
	TestEqual(TEXT("it waits"), Router->NumDeferredForTest(), 1);

	Entities->ReassignOwnership(EntityID, Local, Other);
	Router->RetryDeferredForTest();
	TestEqual(TEXT("and runs once the change of owner lands"), Target->CallCount, 2);
	TestEqual(TEXT("and the wait is over"), Router->NumDeferredForTest(), 0);

	UFunction* ActorFn = ACrowdyRpcActorTestTarget::StaticClass()->FindFunctionByName(TEXT("ActorOwnerOnly_Implementation"));
	if (!TestNotNull(TEXT("the actor's event resolved"), ActorFn))
	{
		return false;
	}
	FCrowdyRpcCall ForAbsentEntity = FCrowdyRPC::MarshalCall(ActorFn, FCrowdyRPC::GetFnInfo(ActorFn), &ACrowdyRpcActorTestTarget::ActorOwnerOnly_Implementation, 7);
	ForAbsentEntity.EntityID = FGuid::NewGuid();
	ForAbsentEntity.SenderID = FGuid::NewGuid();

	Router->ReceiveChannelRpcCall(ForAbsentEntity);
	TestEqual(TEXT("an unnamed call for an entity not here is for someone else"), Router->NumDeferredForTest(), 0);
	Router->ReceiveChannelRpcCall(ForAbsentEntity, Local);
	TestEqual(TEXT("one named for this client waits for the entity"), Router->NumDeferredForTest(), 1);

	return true;
}

// Loopback is a debug replay of what the sender just sent, so it runs whoever owns the entity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackRunsAnywhereTest,
	"CrowdySDK.RPC.LoopbackRunsWhereverItWasSent", CrowdyOwningClientTestFlags)
bool FCrowdyLoopbackRunsAnywhereTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(MakeOwnerCallRegistry(), Entities);

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Target);

	Router->ReceiveLoopbackCall(MakeOwnerCall(EntityID));
	TestEqual(TEXT("it ran on a client that only draws the entity"), Target->CallCount, 1);
	return true;
}

// A send aimed at an entity this client holds as a target goes the way one from its actor would.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTargetSendFindsNoWriterTest,
	"CrowdySDK.RPC.TargetSendFindsNoWriter", CrowdyOwningClientTestFlags)
bool FCrowdyTargetSendFindsNoWriterTest::RunTest(const FString& Parameters)
{
	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ServerObject);
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(), TEXT("SubOwnerOnly_Implementation"));
	if (!TestNotNull(TEXT("the event resolved"), Fn))
	{
		return false;
	}

	FCrowdyRpcTarget Target;
	Target.NetID = FGuid::NewGuid();
	Target.Role = ECrowdyRole::HostOwned;

	AddExpectedMessage(TEXT("and nobody does"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FCrowdyRPC::RouteToTargetForTest(Entities, Target, Fn, FCrowdyRPC::GetFnInfo(Fn), MakeOwnerCall(Target.NetID));
	return true;
}

// A source's identity is a copy, so a change of owner reaches the registry first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEventSourceAsksTheRegistryTest,
	"CrowdySDK.RPC.EventSourceAsksTheRegistry", CrowdyOwningClientTestFlags)
bool FCrowdyEventSourceAsksTheRegistryTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);

	UCrowdyRpcEventSourceTestTarget* Source = NewObject<UCrowdyRpcEventSourceTestTarget>();
	Source->EntityID = FGuid::NewGuid();
	Source->OwnerID = Local;
	Source->Role = ECrowdyRole::Owner;

	UFunction* Fn = UCrowdyRpcEventSourceTestTarget::StaticClass()->FindFunctionByName(TEXT("SourceMulticast_Implementation"));
	if (!TestNotNull(TEXT("the event resolved"), Fn))
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcEventSourceTestTarget::SourceMulticast_Implementation, 1);

	FCrowdyRPC::RouteFromEventSourceForTest(Source, Source, Fn, Info, Call, Entities);
	TestEqual(TEXT("with no record the source's own copy decides"), Source->CallCount, 1);

	RegisterParticipantAs(Entities, Source->EntityID, Source, ECrowdyRole::RemoteProxy, FGuid::NewGuid());
	AddExpectedMessage(TEXT("does not write entity"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FCrowdyRPC::RouteFromEventSourceForTest(Source, Source, Fn, Info, Call, Entities);
	TestEqual(TEXT("once the registry names another owner, the stale copy no longer speaks"), Source->CallCount, 1);

	return true;
}

// A crowd draws other clients' entities, so its row goes when the entity becomes this client's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRowReleasedWhenTheEntityBecomesLocalTest,
	"CrowdySDK.RPC.RowReleasedWhenTheEntityBecomesLocal", CrowdyOwningClientTestFlags)
bool FCrowdyRowReleasedWhenTheEntityBecomesLocalTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	TStrongObjectPtr<UCrowdyCrowdRowRecordingBackend> Backend(NewObject<UCrowdyCrowdRowRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());
	Manager->SetEntitySubsystemForTest(Entities);
	Manager->BindToEntitySubsystemForTest(Entities);

	const FGuid Other = FGuid::NewGuid();
	const FGuid NetID = RegisterRemote(Entities, false, Other);
	const int32 Slot = Manager->AllocateSlotForTest(NetID);

	Entities->ReassignOwnership(NetID, FGuid::NewGuid(), Other);
	TestEqual(TEXT("a move between two other clients keeps the row"), Backend->DeactivatedSlots.Num(), 0);

	const FGuid Current = Entities->FindRecord(NetID)->OwnerID;
	Entities->ReassignOwnership(NetID, Local, Current);
	TestEqual(TEXT("a move to this client releases it"), Backend->DeactivatedSlots.Num(), 1);
	TestTrue(TEXT("the right one"), Backend->DeactivatedSlots.Contains(Slot));

	return true;
}

// With no position there is no region to send to, so the session channel carries it. The spatial route reads the
// position unchecked, so this crashes if a target with none ever reaches it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTargetWithNoPositionUsesTheSessionTest,
	"CrowdySDK.RPC.TargetWithNoPositionUsesTheSession", CrowdyOwningClientTestFlags)
bool FCrowdyTargetWithNoPositionUsesTheSessionTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(), TEXT("SubOwnerOnly_Implementation"));
	if (!TestNotNull(TEXT("the event resolved"), Fn))
	{
		return false;
	}

	FCrowdyRpcTarget Target;
	Target.NetID = FGuid::NewGuid();
	Target.OwnerID = FGuid::NewGuid();
	Target.Role = ECrowdyRole::RemoteProxy;

	TestTrue(TEXT("the call was routed"), FCrowdyRPC::RouteToTargetForTest(Entities, Target, Fn, FCrowdyRPC::GetFnInfo(Fn), MakeOwnerCall(Target.NetID)));
	return true;
}

// Text never takes the session channel: with no way to send it as a direct message, it is dropped and says so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTextWithNoRouteIsDroppedTest,
	"CrowdySDK.RPC.TextWithNoRouteIsDropped", CrowdyOwningClientTestFlags)
bool FCrowdyTextWithNoRouteIsDroppedTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UFunction* Fn = FCrowdyRPC::ResolveFunction(ACrowdyRpcActorTestTarget::StaticClass(), TEXT("ActorOwnerOnly_Implementation"));
	if (!TestNotNull(TEXT("the event resolved"), Fn))
	{
		return false;
	}

	FCrowdyRpcTarget Target;
	Target.NetID = FGuid::NewGuid();
	Target.OwnerID = FGuid::NewGuid();
	Target.Role = ECrowdyRole::RemoteProxy;

	FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
	Info.bCarriesText = true;

	AddExpectedMessage(TEXT("it carries text"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FCrowdyRPC::RouteToTargetForTest(Entities, Target, Fn, Info, MakeOwnerCall(Target.NetID));
	return true;
}

#if WITH_METADATA
// Host leaves the pickers, and everything that names it keeps working until it goes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHostRecipientDeprecatedTest,
	"CrowdySDK.RPC.HostRecipientDeprecated", CrowdyOwningClientTestFlags)
bool FCrowdyHostRecipientDeprecatedTest::RunTest(const FString& Parameters)
{
	const UEnum* Enum = StaticEnum<ECrowdyEventRecipient>();
	const int32 Index = Enum->GetIndexByValue(static_cast<int64>(CrowdyDeprecatedHostRecipient));
	TestTrue(TEXT("Host is hidden from pickers"), Enum->HasMetaData(TEXT("Hidden"), Index));
	TestEqual(TEXT("a saved value still reads"), Enum->GetValueByNameString(TEXT("Host")), static_cast<int64>(CrowdyDeprecatedHostRecipient));

	const int32 Owning = Enum->GetIndexByValue(static_cast<int64>(ECrowdyEventRecipient::OwningClient));
	TestFalse(TEXT("Owning Client stays"), Enum->HasMetaData(TEXT("Hidden"), Owning));

	UFunction* Fixture = UCrowdyRpcSubsystemTestTarget::StaticClass()->FindFunctionByName(TEXT("SubHostOnly_Implementation"));
	UFunction* Authored = UCrowdyRpcEventSourceTestTarget::StaticClass()->FindFunctionByName(TEXT("SourceMulticast_Implementation"));
	if (!TestNotNull(TEXT("a marked fixture's Host event resolved"), Fixture) || !TestNotNull(TEXT("an event to mark Host resolved"), Authored))
	{
		return false;
	}

	TestTrue(TEXT("the SDK's own fixture routes as Host"), FCrowdyRPC::BuildFnInfo(Fixture).Recipient == CrowdyDeprecatedHostRecipient);

	const FString Saved = Authored->GetMetaData(CrowdyRpcMetaKeys::Recipient);
	Authored->SetMetaData(CrowdyRpcMetaKeys::Recipient, TEXT("Host"));
	AddExpectedMessage(TEXT("which is deprecated"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("and so does an author's, with one warning"), FCrowdyRPC::BuildFnInfo(Authored).Recipient == CrowdyDeprecatedHostRecipient);
	Authored->SetMetaData(CrowdyRpcMetaKeys::Recipient, *Saved);
	return true;
}
#endif

#endif // WITH_DEV_AUTOMATION_TESTS
