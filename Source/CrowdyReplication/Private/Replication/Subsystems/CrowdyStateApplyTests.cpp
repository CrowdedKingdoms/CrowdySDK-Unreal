#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/Script.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateApplyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package
	// NewObject trips a ClassWithin ensure, so outer it to a bare GameInstance (Phase 1 idiom).
	UCrowdyAutoRegistry* MakeStateRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UCrowdyAutoRegistry>(GameInstance);
	}

	// The router and entity subsystem are UWorldSubsystems (no ClassWithin); a plain transient-package
	// NewObject is fine because the tests never drive Initialize/Tick (which would need a world), instead
	// injecting collaborators via the test seams and driving DispatchEvent / RetryDeferredForTest directly.
	UCrowdyEventRouter* MakeRouter(UCrowdyAutoRegistry* Registry, UCrowdyEntitySubsystem* Entities)
	{
		UCrowdyEventRouter* Router = NewObject<UCrowdyEventRouter>(GetTransientPackage());
		Router->SetAutoRegistryForTest(Registry);
		Router->SetEntitySubsystemForTest(Entities);
		return Router;
	}

	// Layout index of the property with the given name, or INDEX_NONE.
	int32 IndexOfPropertyName(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		for (int32 Index = 0; Index < Layout.Properties.Num(); ++Index)
		{
			const FProperty* Prop = Layout.Properties[Index].Property;
			if (Prop && Prop->GetFName() == Wanted)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	// Wraps a state delta in an inbound broadcast event, stamping a FOREIGN sender so echo-drop never fires.
	FCrowdyInboundEvent MakeInboundStateEvent(const FCrowdyStateDelta& Delta)
	{
		FCrowdyInboundEvent Event;
		Event.Payload = FInstancedStruct::Make(Delta);
		Event.bTargetedDelivery = false;
		Event.SenderID = Delta.SenderID;
		Event.Target = ECrowdyTarget::Everyone;
		return Event;
	}

	// Registers an actor as a resolvable remote-proxy entity so FindEntity(EntityId) returns it.
	void RegisterProxyEntity(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, AActor* Actor)
	{
		FCrowdyEntityRecord Rec;
		Rec.NetID = EntityId;
		Rec.OwnerID = FGuid::NewGuid();
		Rec.Role = ECrowdyRole::RemoteProxy;
		Rec.Participant = Actor;
		Entities->RegisterEntity(Rec);
	}

	// Registers an actor as an entity the LOCAL client owns, so IsLocallyOwned(EntityId) is true (its OwnerID
	// is the entity subsystem's local player id). Used by the host-precedence apply test.
	void RegisterOwnedEntity(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, AActor* Actor)
	{
		FCrowdyEntityRecord Rec;
		Rec.NetID = EntityId;
		Rec.OwnerID = Entities->GetLocalPlayerID();
		Rec.Role = ECrowdyRole::Owner;
		Rec.Participant = Actor;
		Entities->RegisterEntity(Rec);
	}

	// A minimal EDITOR world so actor OnRep notifies actually run. AActor::ProcessEvent (Actor.cpp) skips a
	// function unless the actor has a world AND (its actors are initialized OR GAllowActorScriptExecutionInEditor
	// is set); a worldless NewObject'd actor silently no-ops it, unlike a plain UObject. An editor-type world is
	// deliberate: the project's game world subsystems (CrowdyMass et al.) gate ShouldCreateSubsystem to PIE/Game,
	// so an editor world never spins them up and tears down cleanly, whereas a Game world's
	// CrowdyMassEntitySubsystem::Deinitialize asserts on an EntityManager this bare test world never initialized.
	// The held FEditorScriptExecutionGuard flips GAllowActorScriptExecutionInEditor for the fixture's lifetime so
	// ProcessEvent runs; the receive path fires OnRep via ProcessEvent, so the OnRep tests spawn targets here.
	struct FCrowdyStateTestWorld
	{
		FEditorScriptExecutionGuard ScriptGuard;
		UWorld* World = nullptr;

		FCrowdyStateTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyStateTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		template <typename T>
		T* Spawn()
		{
			return World->SpawnActor<T>();
		}
	};
}

// End-to-end apply: a hot delta changing a subset of an actor's CrowdyState properties (RepInt, RepFloat,
// RepVector, RepHealth NOT RepScore) is routed through DispatchEvent onto a registered target; the target's
// values now equal the source's, and OnRep fires exactly for the changed OnRep'd property (RepHealth) and
// not for the unchanged one (RepScore). A keyframe variant then covers every property.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateApplyRoundTripTest,
	"CrowdySDK.State.ApplyRoundTrip", CrowdyStateApplyTestFlags)
bool FCrowdyStateApplyRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));

	// Hot delta: mutate a subset on the source, mark exactly those bits dirty, encode.
	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 42;
	Source->RepFloat = 3.5f;
	Source->RepVector = FVector(1.0, 2.0, 3.0);
	Source->RepHealth = 88.f;
	// RepScore stays default so its OnRep must never fire.

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepFloat"), TEXT("RepVector"), TEXT("RepHealth") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (TestTrue(*FString::Printf(TEXT("%s present in layout"), Name), Index != INDEX_NONE))
		{
			Dirty[Index] = true;
		}
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	ACrowdyStateApplyTestActor* Target = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	RegisterProxyEntity(Entities, Delta.EntityID, Target);

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("RepInt applied"), Target->RepInt, 42);
	TestEqual(TEXT("RepFloat applied"), Target->RepFloat, 3.5f);
	TestTrue(TEXT("RepVector applied"), Target->RepVector.Equals(FVector(1.0, 2.0, 3.0)));
	TestEqual(TEXT("RepHealth applied"), Target->RepHealth, 88.f);
	TestEqual(TEXT("RepScore unchanged"), Target->RepScore, 0);
	TestEqual(TEXT("OnRep_Health fired exactly once"), Target->HealthOnRepCount, 1);
	TestEqual(TEXT("OnRep_Score never fired"), Target->ScoreOnRepCount, 0);

	// Keyframe variant: every property present, applied onto a fresh target.
	ACrowdyStateApplyTestActor* KeySource = NewObject<ACrowdyStateApplyTestActor>();
	KeySource->RepInt = 7;
	KeySource->RepFloat = -1.25f;
	KeySource->RepVector = FVector(9.0, 8.0, 7.0);
	KeySource->RepHealth = 12.f;
	KeySource->RepScore = 55;

	TBitArray<> AllDirty;
	AllDirty.Init(false, Layout->Properties.Num());
	TArray<uint8> KeyBlob;
	FCrowdyStateCodec::Encode(*Layout, KeySource, AllDirty, /*bKeyframe=*/true, KeyBlob);

	FCrowdyStateDelta KeyDelta;
	KeyDelta.ClassID = ClassID;
	KeyDelta.EntityID = FGuid::NewGuid();
	KeyDelta.SenderID = FGuid::NewGuid();
	KeyDelta.LayoutHash = Layout->LayoutHash;
	KeyDelta.Flags = CrowdyStateDeltaFlags::Keyframe;
	KeyDelta.Blob = KeyBlob;

	ACrowdyStateApplyTestActor* KeyTarget = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	RegisterProxyEntity(Entities, KeyDelta.EntityID, KeyTarget);

	Router->DispatchEvent(MakeInboundStateEvent(KeyDelta));

	TestEqual(TEXT("keyframe RepInt applied"), KeyTarget->RepInt, 7);
	TestEqual(TEXT("keyframe RepFloat applied"), KeyTarget->RepFloat, -1.25f);
	TestTrue(TEXT("keyframe RepVector applied"), KeyTarget->RepVector.Equals(FVector(9.0, 8.0, 7.0)));
	TestEqual(TEXT("keyframe RepHealth applied"), KeyTarget->RepHealth, 12.f);
	TestEqual(TEXT("keyframe RepScore applied"), KeyTarget->RepScore, 55);
	TestEqual(TEXT("keyframe OnRep_Health fired"), KeyTarget->HealthOnRepCount, 1);
	TestEqual(TEXT("keyframe OnRep_Score fired"), KeyTarget->ScoreOnRepCount, 1);

	return true;
}

// ReceiveLoopbackStateDelta (crowdy.state.loopback): retargeting an outgoing delta at a distinct RemoteProxy
// mirror's NetID and clearing SenderID lets it apply through the REAL, unmodified receive path exactly like a
// genuine remote delta. Also demonstrates why a naive replay onto the SOURCE entity's own NetID cannot work:
// with SenderID unchanged (the local player), the self-echo drop fires before the delta ever reaches decode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateReceiveLoopbackStateDeltaTest,
	"CrowdySDK.State.ReceiveLoopbackStateDelta", CrowdyStateApplyTestFlags)
bool FCrowdyStateReceiveLoopbackStateDeltaTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	const FGuid LocalPlayer = FGuid::NewGuid();
	Entities->SetLocalPlayerID(LocalPlayer);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));

	// The entity WE own: UCrowdyStateReplicator would have just sent this delta for it (SenderID = us).
	ACrowdyStateApplyTestActor* Source = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	const FGuid SourceID = FGuid::NewGuid();
	RegisterOwnedEntity(Entities, SourceID, Source);

	// The mirror: a distinct RemoteProxy under its OWN NetID, standing in for what GetOrCreateLoopbackMirror
	// would produce an entity we do NOT own, so none of DispatchStateDelta's owned-entity handling applies.
	ACrowdyStateApplyTestActor* Mirror = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	const FGuid MirrorID = FGuid::NewGuid();
	RegisterProxyEntity(Entities, MirrorID, Mirror);

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepHealth") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (TestTrue(*FString::Printf(TEXT("%s present in layout"), Name), Index != INDEX_NONE))
		{
			Dirty[Index] = true;
		}
	}

	Source->RepInt = 42;
	Source->RepHealth = 88.f;

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.EntityID = SourceID;
	Delta.SenderID = LocalPlayer;
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	// Sanity check for the failure mode this feature fixes: a naive replay onto the source's own NetID, with
	// SenderID untouched, is a no-op (self-echo drop).
	Router->DispatchEvent(MakeInboundStateEvent(Delta));
	TestEqual(TEXT("naive self-replay never fires OnRep (self-echo drop)"), Source->HealthOnRepCount, 0);

	// The loopback path: retarget at the mirror's NetID and clear SenderID, exactly what
	// UCrowdyStateReplicator does before calling ReceiveLoopbackStateDelta.
	FCrowdyStateDelta MirrorDelta = Delta;
	MirrorDelta.EntityID = MirrorID;
	MirrorDelta.SenderID = FGuid();

	Router->ReceiveLoopbackStateDelta(MirrorDelta);

	TestEqual(TEXT("mirror RepInt applied"), Mirror->RepInt, 42);
	TestEqual(TEXT("mirror RepHealth applied"), Mirror->RepHealth, 88.f);
	TestEqual(TEXT("mirror OnRep_Health fired exactly once"), Mirror->HealthOnRepCount, 1);

	// The source actor (the real owned entity) is never touched by the loopback replay.
	TestEqual(TEXT("source OnRep_Health still never fired"), Source->HealthOnRepCount, 0);

	return true;
}

// Container resolution: ResolveStateContainer keys on the layout owner class, returning the actor for an
// actor-owned layout and the RIGHT component (even when several are present) for a component-owned layout.
// Then proves apply-into-a-component works via the resolver + codec directly. The full DispatchStateDelta
// path resolves the layout from the ACTOR class, so a component-owned layout is not routed end to end today;
// exercising the resolver + codec directly is the honest coverage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateApplyToComponentContainerTest,
	"CrowdySDK.State.ApplyToComponentContainer", CrowdyStateApplyTestFlags)
bool FCrowdyStateApplyToComponentContainerTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	ACrowdyStateApplyTestActor* Actor = NewObject<ACrowdyStateApplyTestActor>();
	if (!TestNotNull(TEXT("apply actor created"), Actor)
		|| !TestNotNull(TEXT("apply component present"), Actor->ApplyComp.Get())
		|| !TestNotNull(TEXT("other component present"), Actor->OtherComp.Get()))
	{
		return false;
	}

	// Actor branch: an actor IsA its own class, so it resolves to itself.
	TestTrue(TEXT("actor-owned layout resolves to the actor"),
		UCrowdyEventRouter::ResolveStateContainer(Actor, ACrowdyStateApplyTestActor::StaticClass())
			== static_cast<UObject*>(Actor));

	// Component branch: resolves the matching component, not the unrelated one, even though both are present.
	TestTrue(TEXT("component-owned layout resolves to the right component"),
		UCrowdyEventRouter::ResolveStateContainer(Actor, UCrowdyStateApplyTestComponent::StaticClass())
			== static_cast<UObject*>(Actor->ApplyComp));
	TestTrue(TEXT("other component-owned layout resolves to the other component"),
		UCrowdyEventRouter::ResolveStateContainer(Actor, UCrowdyStateApplyOtherComponent::StaticClass())
			== static_cast<UObject*>(Actor->OtherComp));

	// No-match: an unrelated concrete class resolves to null. (UObject::StaticClass() would match everything.)
	TestNull(TEXT("unrelated class resolves to null"),
		UCrowdyEventRouter::ResolveStateContainer(Actor, UCrowdyStateTestTarget::StaticClass()));

	// Null guards.
	TestNull(TEXT("null actor resolves to null"),
		UCrowdyEventRouter::ResolveStateContainer(static_cast<AActor*>(nullptr), ACrowdyStateApplyTestActor::StaticClass()));
	TestNull(TEXT("null layout class resolves to null"),
		UCrowdyEventRouter::ResolveStateContainer(Actor, nullptr));

	// Apply-into-a-component end to end via the resolver + codec: build a layout for the COMPONENT class,
	// encode from a source component, and decode into the resolved component container.
	UClass* CompClass = UCrowdyStateApplyTestComponent::StaticClass();
	const FCrowdyRepLayout* CompLayout = Registry->FindRepLayout(CompClass);
	if (!TestNotNull(TEXT("component class has a layout"), CompLayout))
	{
		return false;
	}

	UCrowdyStateApplyTestComponent* SourceComp = NewObject<UCrowdyStateApplyTestComponent>();
	SourceComp->CompInt = 314;
	SourceComp->CompFloat = 2.5f;

	TBitArray<> AllDirty;
	AllDirty.Init(false, CompLayout->Properties.Num());
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*CompLayout, SourceComp, AllDirty, /*bKeyframe=*/true, Blob);

	UObject* Container = UCrowdyEventRouter::ResolveStateContainer(Actor, CompClass);
	if (!TestNotNull(TEXT("resolved component container"), Container))
	{
		return false;
	}

	TArray<int32> Changed;
	const bool bOk = FCrowdyStateCodec::Decode(*CompLayout, CompLayout->LayoutHash, Blob, Container, Changed);
	TestTrue(TEXT("component delta decodes"), bOk);
	TestEqual(TEXT("component CompInt applied"), Actor->ApplyComp->CompInt, 314);
	TestEqual(TEXT("component CompFloat applied"), Actor->ApplyComp->CompFloat, 2.5f);

	return true;
}

// A delta whose LayoutHash does not match the receiver's is dropped by Decode before any value is written:
// the target's properties stay unchanged and no OnRep fires. Decode logs a Warning (not an Error) on hash
// mismatch, so no AddExpectedError is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateApplyDropsOnLayoutMismatchTest,
	"CrowdySDK.State.ApplyDropsOnLayoutMismatch", CrowdyStateApplyTestFlags)
bool FCrowdyStateApplyDropsOnLayoutMismatchTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;

	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 99;
	Source->RepHealth = 50.f;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepHealth") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (Index != INDEX_NONE)
		{
			Dirty[Index] = true;
		}
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	// Corrupt the layout hash so Decode drops before writing.
	Delta.LayoutHash = Layout->LayoutHash ^ 1;
	Delta.Blob = Blob;

	ACrowdyStateApplyTestActor* Target = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	RegisterProxyEntity(Entities, Delta.EntityID, Target);

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("RepInt unchanged on mismatch"), Target->RepInt, 0);
	TestEqual(TEXT("RepHealth unchanged on mismatch"), Target->RepHealth, 0.f);
	TestEqual(TEXT("OnRep_Health did not fire on mismatch"), Target->HealthOnRepCount, 0);
	TestEqual(TEXT("OnRep_Score did not fire on mismatch"), Target->ScoreOnRepCount, 0);

	return true;
}

// Discovery clears the OnRep binding of a property whose CrowdyOnRep is not parameterless, leaving a valid
// sibling binding intact. The lazy FindRepLayout build logs the validation error for this class, so whitelist it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOnRepSignatureRejectedTest,
	"CrowdySDK.State.OnRepSignatureRejected", CrowdyStateApplyTestFlags)
bool FCrowdyStateOnRepSignatureRejectedTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("is not a valid CrowdyOnRep notify"), EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(UCrowdyStateBadOnRepTarget::StaticClass());
	if (!TestNotNull(TEXT("bad-onrep class has a layout"), Layout))
	{
		return false;
	}

	const FName BadWanted(TEXT("RepBad"));
	const FName GoodWanted(TEXT("RepGood"));
	bool bCheckedBad = false;
	bool bCheckedGood = false;
	for (const FCrowdyRepProperty& Prop : Layout->Properties)
	{
		if (!Prop.Property)
		{
			continue;
		}
		if (Prop.Property->GetFName() == BadWanted)
		{
			TestTrue(TEXT("bad OnRep binding cleared"), Prop.OnRepFunctionName == NAME_None);
			bCheckedBad = true;
		}
		else if (Prop.Property->GetFName() == GoodWanted)
		{
			TestTrue(TEXT("good OnRep binding intact"), Prop.OnRepFunctionName == FName(TEXT("OnRep_Good")));
			bCheckedGood = true;
		}
	}

	TestTrue(TEXT("RepBad present in layout"), bCheckedBad);
	TestTrue(TEXT("RepGood present in layout"), bCheckedGood);
	return true;
}

// The deferred-retry path is generalized but the two payload types retry independently: a state delta for an
// unregistered entity and an RPC for a different unregistered entity both defer; after registering only the
// state entity and retrying, the state delta applies while the RPC re-defers and stays pending. This proves
// the RPC path is behaviorally unaffected by the generalization.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateDeferredApplyTest,
	"CrowdySDK.State.DeferredApply", CrowdyStateApplyTestFlags)
bool FCrowdyStateDeferredApplyTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	// Build a valid state delta for an entity that is NOT yet registered.
	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 123;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (TestTrue(TEXT("RepInt present"), RepIntIndex != INDEX_NONE))
	{
		Dirty[RepIntIndex] = true;
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	const FGuid StateEntityId = FGuid::NewGuid();
	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));
	Delta.EntityID = StateEntityId;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));
	TestEqual(TEXT("state delta deferred (entity not registered)"), Router->NumDeferredForTest(), 1);

	// An RPC for a DIFFERENT unregistered entity also defers (it never resolves its function, but that is
	// past the defer point an unregistered entity defers first).
	FCrowdyRpcCall Rpc;
	Rpc.EntityID = FGuid::NewGuid();
	FCrowdyInboundEvent RpcEvent;
	RpcEvent.Payload = FInstancedStruct::Make(Rpc);
	RpcEvent.bTargetedDelivery = false;
	RpcEvent.SenderID = Rpc.SenderID;
	RpcEvent.Target = ECrowdyTarget::Everyone;
	Router->DispatchEvent(RpcEvent);
	TestEqual(TEXT("rpc also deferred"), Router->NumDeferredForTest(), 2);

	// Register ONLY the state entity, then retry: the state delta applies, the RPC re-defers.
	ACrowdyStateApplyTestActor* Target = NewObject<ACrowdyStateApplyTestActor>();
	RegisterProxyEntity(Entities, StateEntityId, Target);

	Router->RetryDeferredForTest();

	TestEqual(TEXT("state delta applied after registration"), Target->RepInt, 123);
	TestEqual(TEXT("rpc still pending (its entity stays unregistered)"), Router->NumDeferredForTest(), 1);

	return true;
}

// Host precedence on receive: for an entity WE own, a foreign non-host delta is dropped before it can write,
// while a HostSourced correction is applied (and adopted into the owner's shadow so the owner does not revert).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateOwnerDropsForeignNonHostDeltaTest,
	"CrowdySDK.State.OwnerDropsForeignNonHostDelta", CrowdyStateApplyTestFlags)
bool FCrowdyStateOwnerDropsForeignNonHostDeltaTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(LocalPlayer);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));

	// Build a valid RepInt=77 delta from a foreign sender.
	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 77;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestTrue(TEXT("RepInt present"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}
	Dirty[RepIntIndex] = true;

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	const FGuid EntityId = FGuid::NewGuid();
	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.EntityID = EntityId;
	Delta.SenderID = FGuid::NewGuid(); // foreign
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	// The target is an entity the LOCAL client owns.
	ACrowdyStateApplyTestActor* Target = NewObject<ACrowdyStateApplyTestActor>();
	RegisterOwnedEntity(Entities, EntityId, Target);

	// Foreign non-host delta for our own entity: dropped before decode, target untouched.
	Router->DispatchEvent(MakeInboundStateEvent(Delta));
	TestEqual(TEXT("foreign non-host delta dropped for owned entity"), Target->RepInt, 0);

	// Now flag it HostSourced and inject a replicator tracking this entity so adoption is reachable.
	UCrowdyStateReplicator* Rep = NewObject<UCrowdyStateReplicator>(GetTransientPackage());
	Rep->SetRegistryForTest(Registry);
	Rep->SetLocalPlayerIDForTest(LocalPlayer);
	Rep->SetTimeForTest(0.0);
	TestTrue(TEXT("replicator tracks the owned entity"), Rep->RegisterOwnedEntityForTest(EntityId, Target));
	Router->SetStateReplicatorForTest(Rep);

	Delta.Flags |= CrowdyStateDeltaFlags::HostSourced;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));
	TestEqual(TEXT("host correction applied to owned entity"), Target->RepInt, 77);

	// Adopted: the owner's next diff sees the host value as already-sent, so it emits nothing (no revert).
	int32 EmitCount = 0;
	Rep->DispatchHookForTests = [&EmitCount](const FCrowdyStateDelta& /*D*/, bool /*bT*/) { ++EmitCount; };
	Rep->RunReplicationLoopForTest();
	TestEqual(TEXT("owner does not revert the adopted host value"), EmitCount, 0);

	return true;
}

// Phase C receive backstop: even a HostSourced correction for an entity WE own is dropped if that entity is
// authored OwnerOnly (only the owner may change it); an Allow entity applies it. Both targets receive the same
// HostSourced RepInt=55 blob (one delta per entity id).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHostOverrideBackstopTest,
	"CrowdySDK.State.HostOverrideBackstop", CrowdyStateApplyTestFlags)
bool FCrowdyStateHostOverrideBackstopTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateHostOverrideActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("host-override actor has a layout"), Layout))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(LocalPlayer);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));

	// Build a HostSourced RepInt=55 blob from a foreign sender.
	ACrowdyStateHostOverrideActor* Source = NewObject<ACrowdyStateHostOverrideActor>();
	Source->RepInt = 55;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestTrue(TEXT("RepInt present"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}
	Dirty[RepIntIndex] = true;

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	const FGuid ForeignSender = FGuid::NewGuid(); // not the local player, so the self-echo drop never fires

	// (i) OwnerOnly target: the backstop drops even the HostSourced delta.
	{
		ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
		if (!TestNotNull(TEXT("owner-only target created"), Target)
			|| !TestNotNull(TEXT("owner-only target entity component present"), Target->Entity.Get()))
		{
			return false;
		}
		Target->Entity->HostOverride = ECrowdyHostOverride::OwnerOnly;

		const FGuid EntityId = FGuid::NewGuid();
		RegisterOwnedEntity(Entities, EntityId, Target); // IsLocallyOwned == true

		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = EntityId;
		Delta.SenderID = ForeignSender;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = CrowdyStateDeltaFlags::HostSourced;
		Delta.Blob = Blob;

		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		TestEqual(TEXT("HostSourced delta dropped for OwnerOnly owned entity"), Target->RepInt, 0);
	}

	// (ii) Allow target: the HostSourced correction applies.
	{
		ACrowdyStateHostOverrideActor* Target = NewObject<ACrowdyStateHostOverrideActor>();
		if (!TestNotNull(TEXT("allow target created"), Target)
			|| !TestNotNull(TEXT("allow target entity component present"), Target->Entity.Get()))
		{
			return false;
		}
		Target->Entity->HostOverride = ECrowdyHostOverride::Allow;

		const FGuid EntityId = FGuid::NewGuid();
		RegisterOwnedEntity(Entities, EntityId, Target);

		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = EntityId;
		Delta.SenderID = ForeignSender;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = CrowdyStateDeltaFlags::HostSourced;
		Delta.Blob = Blob;

		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		TestEqual(TEXT("HostSourced delta applied for Allow owned entity"), Target->RepInt, 55);
	}

	return true;
}

// Phase C world-entity gate: a HostOwned (world) entity has no owner, so the bWeOwnTarget host-precedence gate never
// covers it. A non-host (non-HostSourced) delta for it is dropped; a HostSourced delta applies only the host may
// write world state. This locks in the closure of the vector the untracked one-shot push opened (any client could
// otherwise write a shared world entity on every peer).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateWorldEntityHostOnlyTest,
	"CrowdySDK.State.WorldEntityHostOnly", CrowdyStateApplyTestFlags)
bool FCrowdyStateWorldEntityHostOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));
	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestTrue(TEXT("RepInt present"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}

	// Encode a RepInt=55 blob once, reused for both deltas.
	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 55;
	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	// Registers a HostOwned (world) entity: Role=HostOwned with no owner id, so IsLocallyOwned is false everywhere.
	auto RegisterWorldEntity = [Entities](const FGuid& Id, AActor* Actor)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = Id;
		Record.Role = ECrowdyRole::HostOwned;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);
	};

	// A non-host (non-HostSourced) delta for a world entity is dropped before decode.
	{
		ACrowdyStateApplyTestActor* Target = NewObject<ACrowdyStateApplyTestActor>();
		const FGuid EntityId = FGuid::NewGuid();
		RegisterWorldEntity(EntityId, Target);

		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = EntityId;
		Delta.SenderID = FGuid::NewGuid(); // foreign, non-host, non-HostSourced
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Blob = Blob;

		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		TestEqual(TEXT("non-host delta dropped for host-owned world entity"), Target->RepInt, 0);
	}

	// A HostSourced delta for a world entity applies.
	{
		ACrowdyStateApplyTestActor* Target = NewObject<ACrowdyStateApplyTestActor>();
		const FGuid EntityId = FGuid::NewGuid();
		RegisterWorldEntity(EntityId, Target);

		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = EntityId;
		Delta.SenderID = FGuid::NewGuid(); // foreign sender but HostSourced -> the host is the authority for world state
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Flags = CrowdyStateDeltaFlags::HostSourced;
		Delta.Blob = Blob;

		Router->DispatchEvent(MakeInboundStateEvent(Delta));
		TestEqual(TEXT("host-sourced delta applied for host-owned world entity"), Target->RepInt, 55);
	}

	return true;
}

// Regression (2-client PIE showed OnRep re-firing on every keyframe heartbeat): a keyframe re-sending values a
// proxy ALREADY holds is idempotent  no OnRep fires the second time. Phase 5 re-sends every non-owner-only
// property every keyframe interval, so if apply fired OnRep for every PRESENT slot (not just the actually-changed
// ones) every OnRep would re-fire every ~2s even with nothing moving. This locks apply to RepNotify-on-change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateKeyframeNoChangeNoOnRepTest,
	"CrowdySDK.State.KeyframeNoChangeNoOnRep", CrowdyStateApplyTestFlags)
bool FCrowdyStateKeyframeNoChangeNoOnRepTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* ActorClass = ACrowdyStateApplyTestActor::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ActorClass);
	if (!TestNotNull(TEXT("apply actor has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyStateTestWorld TestWorld;
	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ActorClass));

	// A full keyframe of non-default values, including both OnRep'd properties.
	ACrowdyStateApplyTestActor* Source = NewObject<ACrowdyStateApplyTestActor>();
	Source->RepInt = 7;
	Source->RepFloat = -1.25f;
	Source->RepVector = FVector(9.0, 8.0, 7.0);
	Source->RepHealth = 12.f;
	Source->RepScore = 55;

	TBitArray<> AllDirty;
	AllDirty.Init(false, Layout->Properties.Num());
	TArray<uint8> KeyBlob;
	FCrowdyStateCodec::Encode(*Layout, Source, AllDirty, /*bKeyframe=*/true, KeyBlob);

	// Same entity id both times; a fresh foreign sender id each time so the self-echo drop never fires.
	const FGuid EntityId = FGuid::NewGuid();
	const auto MakeKeyframe = [&]() -> FCrowdyStateDelta
	{
		FCrowdyStateDelta D;
		D.ClassID = ClassID;
		D.EntityID = EntityId;
		D.SenderID = FGuid::NewGuid();
		D.LayoutHash = Layout->LayoutHash;
		D.Flags = CrowdyStateDeltaFlags::Keyframe;
		D.Blob = KeyBlob;
		return D;
	};

	ACrowdyStateApplyTestActor* Target = TestWorld.Spawn<ACrowdyStateApplyTestActor>();
	RegisterProxyEntity(Entities, EntityId, Target);

	// First keyframe: the fresh proxy differs from every value, so it applies and both OnReps fire exactly once.
	Router->DispatchEvent(MakeInboundStateEvent(MakeKeyframe()));
	TestEqual(TEXT("first keyframe applied RepScore"), Target->RepScore, 55);
	TestEqual(TEXT("first keyframe fired OnRep_Health once"), Target->HealthOnRepCount, 1);
	TestEqual(TEXT("first keyframe fired OnRep_Score once"), Target->ScoreOnRepCount, 1);

	// Second, identical keyframe: nothing moved, so NEITHER OnRep fires again  the heartbeat is idempotent.
	Router->DispatchEvent(MakeInboundStateEvent(MakeKeyframe()));
	TestEqual(TEXT("repeat keyframe does not re-fire OnRep_Health"), Target->HealthOnRepCount, 1);
	TestEqual(TEXT("repeat keyframe does not re-fire OnRep_Score"), Target->ScoreOnRepCount, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
