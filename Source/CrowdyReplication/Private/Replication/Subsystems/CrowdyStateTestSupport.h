#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"

// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package
// NewObject trips a ClassWithin ensure, so outer it to a bare GameInstance.
inline UCrowdyAutoRegistry* MakeStateRegistry()
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	return NewObject<UCrowdyAutoRegistry>(GameInstance);
}

// A bare entity subsystem (UWorldSubsystem, no ClassWithin) with a local player id set, so
// RegisterParticipant / FindRecord / GetLocalPlayerID resolve headlessly.
inline UCrowdyEntitySubsystem* MakeEntitySubsystem(const FGuid& LocalPlayer)
{
	UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	ES->SetLocalPlayerID(LocalPlayer);
	return ES;
}

// The router and entity subsystem are UWorldSubsystems (no ClassWithin); a plain transient-package
// NewObject is fine because the tests never drive Initialize/Tick (which would need a world), instead
// injecting collaborators via the test seams and driving DispatchEvent / RetryDeferredForTest directly.
inline UCrowdyEventRouter* MakeRouter(UCrowdyAutoRegistry* Registry, UCrowdyEntitySubsystem* Entities)
{
	UCrowdyEventRouter* Router = NewObject<UCrowdyEventRouter>(GetTransientPackage());
	Router->SetAutoRegistryForTest(Registry);
	Router->SetEntitySubsystemForTest(Entities);
	return Router;
}

// The replicator is a UWorldSubsystem; a plain transient-package NewObject is fine on the hook path
// (Initialize/Tick are never driven, so no world/GameInstance is touched).
inline UCrowdyStateReplicator* MakeReplicator(UCrowdyAutoRegistry* Registry, const FGuid& LocalPlayer)
{
	UCrowdyStateReplicator* Rep = NewObject<UCrowdyStateReplicator>(GetTransientPackage());
	Rep->SetRegistryForTest(Registry);
	Rep->SetLocalPlayerIDForTest(LocalPlayer);
	return Rep;
}

// A real game session carrying an elected host, mirroring the persisted host that survives level travel.
// UCrowdyGameSession is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so it needs a GameInstance outer.
inline UCrowdyGameSession* MakeGameSession(const FGuid& HostID)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(GameInstance);
	Session->SetHostID(HostID);
	return Session;
}

/**
 * An inbound event together with the payload it borrows, for a test that has no wire message to borrow one
 * from.
 *
 * It exists so a test reproduces the PRODUCTION lifetime rather than a convenient one. Handed to a dispatch
 * as a temporary, the payload dies at the end of that statement, exactly as the wire message's does when the
 * delivery returns. A test that then ticks a retry is reading whatever the wait queue copied, which is the
 * thing worth asserting; a helper that kept the payload alive for the whole test could not fail.
 */
struct FCrowdyScopedInboundEvent
{
	FInstancedStruct OwnedPayload;
	FCrowdyInboundEvent Event;

	// Repointed on every read, so moving this wrapper cannot leave the event addressing a stale member.
	operator const FCrowdyInboundEvent&()
	{
		Event.Payload = &OwnedPayload;
		return Event;
	}
};

// Wraps a state delta in an inbound broadcast event, stamping a FOREIGN sender so echo-drop never fires.
inline FCrowdyScopedInboundEvent MakeInboundStateEvent(const FCrowdyStateDelta& Delta)
{
	FCrowdyScopedInboundEvent Scoped;
	Scoped.OwnedPayload = FInstancedStruct::Make(Delta);
	Scoped.Event.bTargetedDelivery = false;
	Scoped.Event.SenderID = Delta.SenderID;
	Scoped.Event.Target = ECrowdyTarget::Everyone;
	return Scoped;
}

// Layout index of the property with the given name, or INDEX_NONE.
inline int32 IndexOfPropertyName(const FCrowdyRepLayout& Layout, const TCHAR* Name)
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

// The layout entry for the property with the given name, or null.
inline const FCrowdyRepProperty* FindByName(const FCrowdyRepLayout& Layout, const TCHAR* Name)
{
	const FName Wanted(Name);
	for (const FCrowdyRepProperty& Prop : Layout.Properties)
	{
		if (Prop.Property && Prop.Property->GetFName() == Wanted)
		{
			return &Prop;
		}
	}
	return nullptr;
}

// Registers any UObject as a resolvable RemoteProxy participant so FindParticipant(EntityId) returns it
// (its OwnerID is a fresh non-local guid, so IsLocallyOwned is false and the owned-entity gate is bypassed).
inline void RegisterProxyParticipant(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, UObject* Participant)
{
	FCrowdyEntityRecord Rec;
	Rec.NetID = EntityId;
	Rec.OwnerID = FGuid::NewGuid();
	Rec.Role = ECrowdyRole::RemoteProxy;
	Rec.Participant = Participant;
	Entities->RegisterEntity(Rec);
}

// Registers any UObject as a participant under a CHOSEN role and owner id, which the helper above cannot
// express: it always stamps RemoteProxy with a foreign owner, the one combination none of the receive path's
// authority gates apply to. Pass ECrowdyRole::HostOwned with an invalid OwnerID for a world entity (the
// world-static convention: no owner), or ECrowdyRole::Owner with the subsystem's local player id for an entity
// this client owns. Those two fields are exactly what the gates read, so a test that needs one of them to fire
// registers through here.
inline void RegisterParticipantAs(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, UObject* Participant,
	ECrowdyRole Role, const FGuid& OwnerId)
{
	FCrowdyEntityRecord Rec;
	Rec.NetID = EntityId;
	Rec.OwnerID = OwnerId;
	Rec.Role = Role;
	Rec.Participant = Participant;
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

#endif
