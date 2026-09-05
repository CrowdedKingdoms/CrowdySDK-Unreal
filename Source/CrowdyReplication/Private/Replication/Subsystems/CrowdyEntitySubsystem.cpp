#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "CrowdyReplicationLog.h"

#include "TimerManager.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Engine/AssetManager.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UCrowdyClassRegistry.h"

void UCrowdyEntitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	checkf(IsValid(World), TEXT("World is invalid"));

	// During UGameEngine::Init the initial Game world is created before a
	// UGameInstance owns it, so GetGameInstance() can be null here.
	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
		return;

	// Level transitions bring in new handler/executor classes; re-scan so
	// their payload structs are registered before any of them sends.
	if (UCrowdyAutoRegistry* AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>())
		AutoRegistry->RegisterLoadedPayloadTypes();

	GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
	checkf(IsValid(GameSession), TEXT("GameSession is invalid"));

	// The session may already have a UUID
	// so seed from it and then track future updates.
	LocalPlayerID = GameSession->GetID();
	GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyEntitySubsystem::OnOwnerUUIDUpdated);

	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	if (!Profile || !Profile->bEnableNetworking)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEntitySubsystem]: Networking disabled for this map — entity events inactive."));
		return;
	}

	Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!IsValid(Bridge))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Invalid Bridge subsystem — entity events disabled."));
		return;
	}

	if (Bridge->ServiceRegistry)
	{
		SubscribeToSpawnDestroy(*Bridge->ServiceRegistry);
	}
}

void UCrowdyEntitySubsystem::SubscribeToSpawnDestroy(FCrowdyServiceRegistry& Registry)
{
	const FCrowdySubscriptionKey Keys[] =
	{
		FCrowdySubscriptionKey::EventPayload(FCrowdyEntitySpawnEvent::StaticStruct()),
		FCrowdySubscriptionKey::EventPayload(FCrowdyEntityDestroyEvent::StaticStruct()),
	};
	// Handle claims both keys so the event router's fallback never sees them; exclusive because
	// two systems both spawning an actor for one spawn event would give two actors.
	SpawnDestroySubscription = Registry.Subscribe(Keys,
		{ ECrowdySubscriptionRole::Handle, /*bRequiresExclusiveHandling*/ true, TEXT("CrowdyEntitySubsystem") },
		[this](const FCrowdyDelivery& Delivery) { HandleSpawnDestroyDelivery(Delivery); });
}

void UCrowdyEntitySubsystem::Deinitialize()
{
	if (IsValid(GameSession))
		GameSession->OnOwnerUUIDUpdated.RemoveDynamic(this, &UCrowdyEntitySubsystem::OnOwnerUUIDUpdated);
	GameSession = nullptr;

	for (auto& Pair : PendingRemoteSpawns)
	{
		if (Pair.Value.LoadHandle.IsValid())
			Pair.Value.LoadHandle->CancelHandle();
	}
	PendingRemoteSpawns.Empty();

	Records.Empty();
	ParticipantToID.Empty();

	// Releasing the handle stops any further delivery, so this needs no explicit registry lookup.
	SpawnDestroySubscription.Release();
	Bridge = nullptr;

	Super::Deinitialize();
}

bool UCrowdyEntitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World) return false;

	return World->WorldType == EWorldType::PIE
		|| World->WorldType == EWorldType::Game;
}

//Network reception

void UCrowdyEntitySubsystem::HandleSpawnDestroyDelivery(const FCrowdyDelivery& Delivery)
{
	if (!Delivery.Payload)
	{
		return;
	}

	// Delivery is on the game thread, so the spawn or destroy is applied here rather than being held
	// for a later frame: an entity exists as soon as the message that created it has been read.
	const UScriptStruct* StructType = Delivery.Payload->GetScriptStruct();

	if (StructType == FCrowdyEntitySpawnEvent::StaticStruct())
		HandleRemoteSpawn(Delivery.Payload->Get<FCrowdyEntitySpawnEvent>());
	else if (StructType == FCrowdyEntityDestroyEvent::StaticStruct())
		HandleRemoteDestroy(Delivery.Payload->Get<FCrowdyEntityDestroyEvent>());
}

//Registry
void UCrowdyEntitySubsystem::RegisterEntity(const FCrowdyEntityRecord& Record)
{
	TryRegisterEntity(Record);
}

ECrowdyEntityRegistration UCrowdyEntitySubsystem::TryRegisterEntity(const FCrowdyEntityRecord& Record)
{
	check(IsInGameThread());

	if (!Record.NetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: Rejected registration with invalid NetID (Participant=%s)."),
			*GetNameSafe(Record.GetParticipant()));
		return ECrowdyEntityRegistration::RefusedInvalidNetID;
	}

	// Re-registering an existing NetID replaces the record; drop the old participant mapping first.
	if (const FCrowdyEntityRecord* Existing = Records.Find(Record.NetID))
	{
		UObject* OldParticipant = Existing->GetParticipant();

		// Collision guard: a caller that already tore its own record down first (a respawn's EndPlay, or the
		// actor pool releasing a slot before reassigning it) finds no Existing entry at all, and one that is
		// simply re-announcing the object it already owns has OldParticipant == the incoming participant; both
		// shapes fall through untouched below. What must not fall through is a SECOND, still-alive, DIFFERENT
		// participant claiming an id already held by a live one: NetID has only 32 real bits of entropy (see
		// GetDeterministicID), so two unrelated registrations can land on the same id, and once this id addresses
		// damage a silent takeover means an attack lands on the wrong player. Refuse it and name both.
		if (IsValid(OldParticipant) && OldParticipant != Record.GetParticipant())
		{
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("[CrowdyEntitySubsystem]: NetID collision on %s: '%s' (owner %s) is already registered and '%s' (owner %s) derives the same id - not overwriting."),
				*Record.NetID.ToString(), *GetNameSafe(OldParticipant), *Existing->OwnerID.ToString(),
				*GetNameSafe(Record.GetParticipant()), *Record.OwnerID.ToString());
			return ECrowdyEntityRegistration::RefusedIdHeldByLiveParticipant;
		}

		if (OldParticipant)
		{
			ParticipantToID.Remove(OldParticipant);
		}
		else
		{
			// The old participant was collected before its record was replaced, and a TObjectKey cannot be
			// rebuilt from a dead weak pointer, so its mapping is dropped by value instead (the same reason
			// UnregisterEntity does). Nothing is misrouted by leaving it: a TObjectKey carries the object's
			// serial number, so a new object reusing the address never matches the dead key. What it costs is
			// memory, one entry per replaced-while-dead participant, held for the life of the world.
			for (auto It = ParticipantToID.CreateIterator(); It; ++It)
			{
				if (It.Value() == Record.NetID)
				{
					It.RemoveCurrent();
				}
			}
		}
	}

	Records.Add(Record.NetID, Record);

	if (UObject* Participant = Record.GetParticipant())
		ParticipantToID.Add(Participant, Record.NetID);

	OnEntityRegistered.Broadcast(Record.NetID);

	return ECrowdyEntityRegistration::Registered;
}

void UCrowdyEntitySubsystem::UnregisterEntity(const FGuid& NetID)
{
	check(IsInGameThread());

	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record) return;

	if (UObject* Participant = Record->GetParticipant())
	{
		ParticipantToID.Remove(Participant);
	}
	else
	{
		// The participant died before its record was torn down (a component sub-participant can outlive-then-
		// predecease its anchor's teardown cascade, and a component may already be pending-kill during EndPlay).
		// Its TObjectKey cannot be reconstructed from a stale weak pointer, so drop any mapping to this NetID by
		// value; otherwise the dead-object key leaks in ParticipantToID under component churn on long-lived actors.
		for (auto It = ParticipantToID.CreateIterator(); It; ++It)
		{
			if (It.Value() == NetID)
			{
				It.RemoveCurrent();
			}
		}
	}

	Records.Remove(NetID);

	OnEntityUnregistered.Broadcast(NetID);
}

AActor* UCrowdyEntitySubsystem::FindEntity(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record ? Record->GetActor() : nullptr;
}

UObject* UCrowdyEntitySubsystem::FindParticipant(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record ? Record->GetParticipant() : nullptr;
}

FGuid UCrowdyEntitySubsystem::FindEntityID(const UObject* Participant) const
{
	if (!Participant) return FGuid{};

	// TObjectKey<UObject> constructs from a const UObject*, so no const_cast is needed to look up the key.
	const FGuid* NetID = ParticipantToID.Find(Participant);
	return NetID ? *NetID : FGuid{};
}

FGuid UCrowdyEntitySubsystem::FindEntityID(const AActor* Actor) const
{
	return FindEntityID(static_cast<const UObject*>(Actor));
}

const FCrowdyEntityRecord* UCrowdyEntitySubsystem::FindRecord(const FGuid& NetID) const
{
	return Records.Find(NetID);
}

bool UCrowdyEntitySubsystem::TrySetRecordClassID(const FGuid& NetID, const uint32 ClassID)
{
	if (ClassID == CROWDY_INVALID_CLASS_ID)
	{
		return false;
	}

	FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record || Record->ClassID != CROWDY_INVALID_CLASS_ID)
	{
		return false;
	}

	Record->ClassID = ClassID;
	return true;
}

bool UCrowdyEntitySubsystem::IsLocallyOwned(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record || !Record->OwnerID.IsValid() || Record->OwnerID != LocalPlayerID)
	{
		return false;
	}

	// The owner id alone is not enough. A record whose role says this client only mirrors the entity, or that has
	// no role at all, does not become locally owned because something wrote the local player's id into it: this
	// answer decides whether this client creates and pins the server-side row bound to the entity's id, and an
	// owner id can arrive from a decoded message while the role is only ever set by whatever registered the
	// entity here. Both have to agree.
	return Record->Role == ECrowdyRole::Owner;
}

FGuid UCrowdyEntitySubsystem::GetLocalPlayerID() const
{
	return LocalPlayerID;
}

FGuid UCrowdyEntitySubsystem::GetHostID() const
{
	return IsValid(GameSession) ? GameSession->GetHostID() : FGuid{};
}

void UCrowdyEntitySubsystem::SetLocalPlayerID(const FGuid& InLocalPlayerID)
{
	LocalPlayerID = InLocalPlayerID;
}

void UCrowdyEntitySubsystem::OnOwnerUUIDUpdated(FString NewUUID)
{
	LocalPlayerID = USerializationFunctionLibrary::ToGuid(NewUUID);

	// A LocalClient participant enrolled before the local player id arrived was minted with an empty owner
	// salt; re-derive its owner-salted identity now. Actor entities carry their own identity (set at spawn,
	// GetActor() non-null) and are never re-stamped here, guaranteeing zero behavior change for actors.
	// Collect-then-mutate so the re-registration below does not modify Records mid-iteration.
	TArray<FGuid> StaleParticipantIDs;
	for (const TPair<FGuid, FCrowdyEntityRecord>& Pair : Records)
	{
		const FCrowdyEntityRecord& Record = Pair.Value;
		if (Record.Role == ECrowdyRole::Owner && !Record.OwnerID.IsValid()
			&& Record.GetActor() == nullptr && Record.GetParticipant() != nullptr)
		{
			StaleParticipantIDs.Add(Pair.Key);
		}
	}

	for (const FGuid& OldNetID : StaleParticipantIDs)
		RestampParticipantIdentity(OldNetID);
}

FGuid UCrowdyEntitySubsystem::RegisterParticipant(UObject* Participant, const ECrowdyOwnership Ownership)
{
	check(IsInGameThread());

	if (!IsValid(Participant))
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: RegisterParticipant called with an invalid participant."));
		return FGuid{};
	}

	ECrowdyRole Role;
	FGuid OwnerID;
	UCrowdyEntityComponent::DeriveAuthority(Ownership, GetLocalPlayerID(), Role, OwnerID);

	// Host: world singleton keyed by class path - every client computes the same id, no salt.
	// LocalClient: owner-salted so two clients' same-class participants get distinct ids.
	const FString PathName = Participant->GetClass()->GetPathName();
	const FString Seed = (Ownership == ECrowdyOwnership::Host)
		? PathName
		: PathName + TEXT(":") + OwnerID.ToString();

	FCrowdyEntityRecord Record;
	Record.NetID       = UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));
	Record.OwnerID     = OwnerID;
	Record.Role        = Role;
	Record.ClassID     = UCrowdyClassRegistry::Get()->GetID(Participant->GetClass());
	Record.Participant = Participant;

	// A refused registration means the id resolves to somebody else's live participant, so handing it back would
	// have the caller bind its state under an id that does not answer for it. It gets no id instead, the same
	// answer RegisterSubParticipant gives when its derived id is already taken.
	if (TryRegisterEntity(Record) != ECrowdyEntityRegistration::Registered)
	{
		return FGuid{};
	}

	return Record.NetID;
}

FGuid UCrowdyEntitySubsystem::DeriveSubParticipantID(const FGuid& AnchorNetID, const UClass* SeedClass,
	const FString& InstanceTerm)
{
	if (!AnchorNetID.IsValid() || !SeedClass || InstanceTerm.IsEmpty())
	{
		return FGuid{};
	}

	// The anchor id + the component's class path + a per-instance term. The anchor id is already network-stable (a
	// player id, a spawn-injected id, or the engine's per-placement ActorInstanceGuid), and the class path is
	// identical on every client. The per-instance term is the object name by default - cross-client stable ONLY for
	// an authored component (a Blueprint variable name or a native CreateDefaultSubobject name). A runtime-added
	// component's name is a per-process counter that diverges across clients, so an author supplies a stable binding
	// key to name it instead; when set, it replaces the object name.
	const FString Seed = AnchorNetID.ToString(EGuidFormats::Digits)
		+ TEXT(":") + SeedClass->GetPathName()
		+ TEXT(":") + InstanceTerm;
	return UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));
}

FGuid UCrowdyEntitySubsystem::RegisterSubParticipant(UObject* Participant, const FGuid& AnchorNetID, const FString& KeyOverride)
{
	// The default derivation: the participant IS the component, so its own class and object name are the seed
	// terms. KeyOverride, when set, replaces the object name (see the header).
	if (!IsValid(Participant))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: RegisterSubParticipant called with an invalid participant or anchor."));
		return FGuid{};
	}
	const FString InstanceTerm = KeyOverride.IsEmpty() ? Participant->GetName() : KeyOverride;
	return RegisterSubParticipantAs(Participant, AnchorNetID, Participant->GetClass(), InstanceTerm);
}

FGuid UCrowdyEntitySubsystem::RegisterSubParticipantAs(UObject* Participant, const FGuid& AnchorNetID,
	const UClass* SeedClass, const FString& InstanceTerm)
{
	check(IsInGameThread());

	if (!IsValid(Participant) || !AnchorNetID.IsValid() || !SeedClass || InstanceTerm.IsEmpty())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: RegisterSubParticipant called with an invalid participant or anchor."));
		return FGuid{};
	}

	const FCrowdyEntityRecord* Anchor = Records.Find(AnchorNetID);
	if (!Anchor)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: RegisterSubParticipant for '%s' has no registered anchor entity %s."),
			*GetNameSafe(Participant), *AnchorNetID.ToString());
		return FGuid{};
	}

	const FGuid NetID = DeriveSubParticipantID(AnchorNetID, SeedClass, InstanceTerm);

	// Dup-key guard: a second, DIFFERENT live participant that derives the same id is two indistinguishable
	// concerns (one participant per concern is the rule); refuse the second and name both so the mistake is loud.
	// Re-enrolling the same object is idempotent and falls through to a plain re-registration below.
	if (const FCrowdyEntityRecord* Existing = Records.Find(NetID))
	{
		UObject* Prior = Existing->GetParticipant();
		if (IsValid(Prior) && Prior != Participant)
		{
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("[CrowdyEntitySubsystem]: sub-participant key collision under anchor %s: '%s' and '%s' derive the same id %s - not enrolling the second. Give one a distinct component name or binding key."),
				*AnchorNetID.ToString(), *GetNameSafe(Prior), *GetNameSafe(Participant), *NetID.ToString());
			return FGuid{};
		}
	}

	FCrowdyEntityRecord Record;
	Record.NetID       = NetID;
	Record.OwnerID     = Anchor->OwnerID;
	Record.Role        = Anchor->Role;
	// The COMPONENT's class, not the participant's. For the real component the two are the same; for a stand-in
	// they are not, and it is this that lets the binding read the container declaration off the component class.
	Record.ClassID     = UCrowdyClassRegistry::Get()->GetID(SeedClass);
	Record.AnchorNetID = AnchorNetID;
	Record.Participant = Participant;

	// The dup-key guard above catches the case this can refuse, so a refusal here means something changed the
	// registry underneath us. Report no id rather than one that answers for a different participant.
	if (TryRegisterEntity(Record) != ECrowdyEntityRegistration::Registered)
	{
		return FGuid{};
	}

	return NetID;
}

void UCrowdyEntitySubsystem::UnregisterParticipant(UObject* Participant)
{
	check(IsInGameThread());

	if (!Participant) return;

	const FGuid NetID = FindEntityID(Participant);
	if (NetID.IsValid())
		UnregisterEntity(NetID);
}

void UCrowdyEntitySubsystem::RestampParticipantIdentity(const FGuid& OldNetID)
{
	const FCrowdyEntityRecord* Existing = Records.Find(OldNetID);
	if (!Existing) return;

	FCrowdyEntityRecord Record = *Existing;
	UObject* Participant = Record.GetParticipant();
	if (!IsValid(Participant))
	{
		UnregisterEntity(OldNetID);
		return;
	}

	Record.OwnerID = LocalPlayerID;
	const FString Seed = Participant->GetClass()->GetPathName() + TEXT(":") + Record.OwnerID.ToString();
	const FGuid NewNetID = UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));

	if (NewNetID == OldNetID)
	{
		// Owner resolved to the same salt (already correct) - update the record in place.
		Records.Add(OldNetID, Record);
		return;
	}

	UnregisterEntity(OldNetID);
	Record.NetID = NewNetID;
	RegisterEntity(Record);
}

//Networked entity lifecycle

AActor* UCrowdyEntitySubsystem::SpawnEntity(const TSubclassOf<AActor> EntityClass, const FTransform& SpawnTransform, const FInstancedStruct& InitialState)
{
	if (!EntityClass)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: SpawnEntity called with a null class."));
		return nullptr;
	}

	const uint32 ClassID = UCrowdyClassRegistry::Get()->GetID(EntityClass);

	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(EntityClass, SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Actor)) return nullptr;

	const FGuid EntityID = FGuid::NewGuid();

	// Inject identity before BeginPlay; the component registers the record itself
	UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
		Component->InitIdentity(EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

	Actor->FinishSpawning(SpawnTransform);

	if (!Component)
		Component = AddStaticEntityComponent(Actor, EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

	if (Component)
		Component->OnCrowdySpawned.Broadcast(InitialState, true);

	FCrowdyEntitySpawnEvent SpawnEvent;
	SpawnEvent.EntityID       = EntityID;
	SpawnEvent.OwnerID        = LocalPlayerID;
	SpawnEvent.ClassID        = ClassID;
	SpawnEvent.ClassPath      = EntityClass->GetPathName();
	SpawnEvent.SpawnTransform = SpawnTransform;
	SpawnEvent.InitialState   = InitialState;

	FInstancedStruct Payload;
	Payload.InitializeAs<FCrowdyEntitySpawnEvent>(SpawnEvent);
	DispatchGameEvent(Actor, MoveTemp(Payload));

	return Actor;
}

void UCrowdyEntitySubsystem::DestroyEntity(AActor* TargetEntity)
{
	const FGuid EntityID = FindEntityID(TargetEntity);
	if (!EntityID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DestroyEntity — '%s' is not a registered entity."),
			*GetNameSafe(TargetEntity));
		return;
	}

	FCrowdyEntityDestroyEvent DestroyEvent;
	DestroyEvent.EntityID = EntityID;

	FInstancedStruct Payload;
	Payload.InitializeAs<FCrowdyEntityDestroyEvent>(DestroyEvent);
	DispatchGameEvent(TargetEntity, MoveTemp(Payload));

	float Delay = 0.f;
	if (UCrowdyEntityComponent* Component = TargetEntity->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->OnCrowdyDestroyed.Broadcast(true);
		Delay = Component->DestroyDelay;
	}

	UnregisterEntity(EntityID);
	DestroyAfterDelay(TargetEntity, Delay);
}

void UCrowdyEntitySubsystem::RegisterStaticEntity(AActor* Entity, const bool bUseDeterministicID, const int64 Seed)
{
	if (!IsValid(Entity)) return;

	if (FindEntityID(Entity).IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: RegisterStaticEntity — '%s' is already registered (it likely has a UCrowdyEntityComponent)."),
			*GetNameSafe(Entity));
		return;
	}

	const uint32 ClassID = UCrowdyClassRegistry::Get()->GetID(Entity->GetClass());
	const FGuid EntityID = bUseDeterministicID ? UHelperFunctions::GetDeterministicID(Seed) : FGuid::NewGuid();

	UCrowdyEntityComponent* Component = Entity->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
	{
		// Component exists but never resolved an ID (it would be registered otherwise)
		Component->InitIdentity(EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

		FCrowdyEntityRecord Record;
		Record.NetID   = EntityID;
		Record.OwnerID = LocalPlayerID;
		Record.Role    = ECrowdyRole::Owner;
		Record.ClassID = ClassID;
		Record.Participant = Entity;
		RegisterEntity(Record);
	}
	else
	{
		// RegisterComponent runs the component's BeginPlay, which registers the record
		Component = AddStaticEntityComponent(Entity, EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);
	}

	if (Component)
		Component->OnCrowdySpawned.Broadcast(FInstancedStruct(), true);
}

void UCrowdyEntitySubsystem::DispatchGameEvent(const AActor* Context, FInstancedStruct&& Payload,
	const ECrowdyTarget Target, const AActor* TargetEntity,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance)
{
	if (!Bridge || !IsValid(Context))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchGameEvent — Bridge or context actor is null."));
		return;
	}

	// The actor IS the position: an actor-sent event goes out from where that actor stands.
	DispatchGameEventAt(Context->GetActorLocation(), MoveTemp(Payload), Target, TargetEntity,
		DecayRate, ReplicationDistance);
}

bool UCrowdyEntitySubsystem::ResolveGameEventRouting(const ECrowdyTarget Target, const AActor* TargetEntity,
	const FVector& Location, FGuid& OutTargetID, int64& OutChunkX, int64& OutChunkY, int64& OutChunkZ)
{
	if (Target == ECrowdyTarget::Entity || Target == ECrowdyTarget::Owner)
	{
		const FGuid NetID = FindEntityID(TargetEntity);
		const FCrowdyEntityRecord* Record = FindRecord(NetID);
		if (!Record)
		{
			UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: DispatchGameEvent — target '%s' is not a registered entity, event dropped."),
				*GetNameSafe(TargetEntity));
			return false;
		}

		// Owner routing matches against player IDs, so address the entity's owning client
		OutTargetID = Target == ECrowdyTarget::Owner ? Record->OwnerID : NetID;
	}

	UHelperFunctions::GetChunkCoordinateAtLocation(this, Location, OutChunkX, OutChunkY, OutChunkZ);
	return true;
}

void UCrowdyEntitySubsystem::DispatchGameEventAt(const FVector& Location, FInstancedStruct&& Payload,
	const ECrowdyTarget Target, const AActor* TargetEntity,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance)
{
	if (!Bridge)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchGameEventAt - Bridge is null."));
		return;
	}

	FGuid TargetID;
	int64 ChunkX, ChunkY, ChunkZ;
	if (!ResolveGameEventRouting(Target, TargetEntity, Location, TargetID, ChunkX, ChunkY, ChunkZ))
	{
		return;
	}

	if (Bridge->DispatchGameEventFn)
		Bridge->DispatchGameEventFn(ChunkX, ChunkY, ChunkZ,
			DecayRate, ReplicationDistance,
			LocalPlayerID, MoveTemp(Payload), Target, TargetID, false);
}

void UCrowdyEntitySubsystem::DispatchGameEventView(const AActor* Context, const UScriptStruct* PayloadStruct,
	const void* PayloadMemory, const ECrowdyTarget Target, const AActor* TargetEntity,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance)
{
	if (!Bridge || !IsValid(Context))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchGameEventView — Bridge or context actor is null."));
		return;
	}

	// The actor IS the position, exactly as the owning overload reads it.
	DispatchGameEventViewAt(Context->GetActorLocation(), PayloadStruct, PayloadMemory, Target, TargetEntity,
		DecayRate, ReplicationDistance);
}

void UCrowdyEntitySubsystem::DispatchGameEventViewAt(const FVector& Location, const UScriptStruct* PayloadStruct,
	const void* PayloadMemory, const ECrowdyTarget Target, const AActor* TargetEntity,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance)
{
	if (!Bridge)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchGameEventViewAt - Bridge is null."));
		return;
	}

	FGuid TargetID;
	int64 ChunkX, ChunkY, ChunkZ;
	if (!ResolveGameEventRouting(Target, TargetEntity, Location, TargetID, ChunkX, ChunkY, ChunkZ))
	{
		return;
	}

	if (Bridge->DispatchGameEventViewFn)
		Bridge->DispatchGameEventViewFn(ChunkX, ChunkY, ChunkZ,
			DecayRate, ReplicationDistance,
			LocalPlayerID, PayloadStruct, PayloadMemory, Target, TargetID);
}

void UCrowdyEntitySubsystem::DispatchSingleActorMessage(const AActor* TargetActor, FInstancedStruct Payload)
{
	if (!Bridge || !IsValid(TargetActor))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessage — Bridge or target actor is null."));
		return;
	}

	// The destination actor must be a registered entity - that NetID is the UUID the server routes by.
	const FGuid TargetID = FindEntityID(TargetActor);
	if (!TargetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessage — '%s' is not a registered entity; dropping."),
			*GetNameSafe(TargetActor));
		return;
	}

	// The actor IS the destination: the chunk comes from where it stands.
	DispatchSingleActorMessageTo(TargetID, TargetActor->GetActorLocation(), MoveTemp(Payload));
}

void UCrowdyEntitySubsystem::DispatchSingleActorMessageTo(const FGuid& TargetNetID, const FVector& Location,
	FInstancedStruct Payload)
{
	// The registry this reads and the transport it hands to are both game-thread only.
	check(IsInGameThread());

	if (!Bridge)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessageTo - Bridge is null."));
		return;
	}

	// The NetID is the UUID the server routes by; without one there is no destination.
	if (!TargetNetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessageTo - target NetID is invalid; dropping."));
		return;
	}

	// A chunk coordinate is floor(coord / chunk size) cast to int64, which is undefined for a value that
	// does not fit. A NaN, an infinity or an absurd magnitude would address the message to a region no
	// receiver is in, or to whatever the undefined cast produced, so reject it here where every caller
	// passes through rather than trusting each one to have checked.
	if (Location.ContainsNaN() || Location.GetAbsMax() > CrowdyMaxAddressableWorldCoordinate)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessageTo - location %s is not a position a chunk can be derived from; dropping."),
			*Location.ToString());
		return;
	}

	// GetChunkCoordinateAtLocation leaves its outputs untouched when it cannot resolve a world, so its
	// precondition is checked here instead of addressing a message with whatever was on the stack. The
	// zero seeds keep that true for any future path out of it as well.
	if (!IsValid(GetWorld()))
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessageTo - no world to derive a chunk from; dropping."));
		return;
	}

	int64 ChunkX = 0, ChunkY = 0, ChunkZ = 0;
	UHelperFunctions::GetChunkCoordinateAtLocation(this, Location, ChunkX, ChunkY, ChunkZ);

	if (Bridge->DispatchSingleActorMessageFn)
		Bridge->DispatchSingleActorMessageFn(ChunkX, ChunkY, ChunkZ, TargetNetID, MoveTemp(Payload), false);
}

void UCrowdyEntitySubsystem::PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& ChannelPayload)
{
	if (!Bridge)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: PublishReliableRpc — Bridge is null; dropping."));
		return;
	}

	if (Bridge->PublishReliableRpcFn)
	{
		Bridge->PublishReliableRpcFn(ChannelName, ChannelPayload);
	}
	else
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: PublishReliableRpc — channel transport not wired; a reliable RPC was dropped."));
	}
}

TArray<AActor*> UCrowdyEntitySubsystem::GetEntitiesByOwner(const FGuid& OwnerID) const
{
	TArray<AActor*> Result;
	for (const auto& Pair : Records)
	{
		if (Pair.Value.OwnerID == OwnerID)
		{
			if (AActor* Actor = Pair.Value.GetActor())
				Result.Add(Actor);
		}
	}
	return Result;
}

void UCrowdyEntitySubsystem::ReassignOwnership(const FGuid& NetID, const FGuid& NewOwnerID,
	const FGuid& ExpectedPreviousOwnerID)
{
	check(IsInGameThread());

	FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record)
	{
		// Not present locally (e.g. a proxy that has not spawned yet). The router defers a grant for a not-yet-
		// present entity, so this is a benign miss rather than a lost transfer.
		return;
	}

	const FGuid PreviousOwnerID = Record->OwnerID;

	// Compare-and-swap: a stale or duplicate grant whose expected previous owner no longer matches is dropped.
	// Skipped when the caller passes an invalid expectation (a host-owned source has no per-client owner id).
	if (ExpectedPreviousOwnerID.IsValid() && PreviousOwnerID != ExpectedPreviousOwnerID)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
			TEXT("[CrowdyEntitySubsystem]: ReassignOwnership %s dropped — expected previous owner %s but current is %s."),
			*NetID.ToString(), *ExpectedPreviousOwnerID.ToString(), *PreviousOwnerID.ToString());
		return;
	}

	// Idempotent: re-applying the same owner (a duplicate grant) is a no-op.
	if (PreviousOwnerID == NewOwnerID)
		return;

	// Re-derive the role from the new owner: a valid player id means a client owns it (Owner where we are that
	// client, RemoteProxy elsewhere); an invalid id means it is host-owned (a world entity, no per-client owner).
	ECrowdyRole NewRole;
	if (NewOwnerID.IsValid())
		NewRole = (NewOwnerID == LocalPlayerID) ? ECrowdyRole::Owner : ECrowdyRole::RemoteProxy;
	else
		NewRole = ECrowdyRole::HostOwned;

	Record->OwnerID = NewOwnerID;
	Record->Role    = NewRole;

	// Capture the actor before broadcasting so a re-entrant handler cannot leave us reading a freed record.
	AActor* Actor = Record->GetActor();

	// Keep the entity component's cached identity in sync and let it move any Dynamic-mode continuous channel.
	if (IsValid(Actor))
	{
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
			Component->ApplyOwnershipReassignment(NewOwnerID, NewRole);
	}

	// A handler is free to destroy the actor, so the pointer is re-checked rather than carried across a call.
	if (!IsValid(Actor))
		Actor = nullptr;

	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
		TEXT("[CrowdyEntitySubsystem]: ReassignOwnership %s: %s -> %s (role %d)."),
		*NetID.ToString(), *PreviousOwnerID.ToString(), *NewOwnerID.ToString(), static_cast<int32>(NewRole));

	OnEntityOwnershipChanged.Broadcast(Actor, NetID, NewOwnerID, PreviousOwnerID);

	// The entity's own listeners are told last, after the systems that track it have re-pointed themselves at the
	// new owner. Game code told "you own this now" typically writes and marks replicated state straight away, and
	// that only reaches the wire once the state replicator already tracks the entity under its new owner. The
	// actor is re-resolved because a handler above may have destroyed it.
	if (IsValid(Actor))
	{
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
#if WITH_DEV_AUTOMATION_TESTS
			Component->TrackersRepointedStep = ++UCrowdyEntityComponent::OwnershipStepCounter;
#endif
			Component->BroadcastOwnershipAssigned();
		}
	}
}

void UCrowdyEntitySubsystem::NotifyOwnershipRequested(AActor* TargetEntity, const FGuid& RequesterID)
{
	// A player avatar's NetID equals its player id, so FindEntity resolves the requester's avatar when it exists on
	// this client; otherwise the actor is null and game code falls back to the always-valid RequesterID.
	AActor* RequesterActor = FindEntity(RequesterID);
	OnOwnershipRequested.Broadcast(TargetEntity, RequesterActor, RequesterID);
}

// Remote handlers

void UCrowdyEntitySubsystem::HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event)
{
	if (!Event.EntityID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: Remote spawn carries no entity id; dropping."));
		return;
	}

	// This entity may already be registered: a rendering backend enrols one as soon as its position updates
	// arrive, which is often before the spawn event that names its class. A spawn event is written by an ordinary
	// peer and reaches every client, so what it may do to a record that already exists is limited to filling in
	// what this client does not know yet. It never re-points an answer already settled here, and it never sets the
	// role, which is decided only by whatever registered the entity on this machine.
	if (FCrowdyEntityRecord* Existing = Records.Find(Event.EntityID))
	{
		// The class is a fact about the entity rather than a claim about who may act on it, so an unset one is
		// filled in - this backfill is what the branch exists for. A class already recorded is kept: whatever
		// registered the entity here knows what it actually put in the world.
		if (Existing->ClassID == CROWDY_INVALID_CLASS_ID)
		{
			Existing->ClassID = Event.ClassID;
		}

		// An owner is filled in only when the record has none, and never with the local player's id. "This client
		// owns it" is an answer only this client's own spawn produces; accepted from the network it would make a
		// peer's entity read as locally owned here, and that reading is what decides whether this client creates
		// and pins the server-side container row bound to the entity's id - a row the real owner then cannot bind,
		// because the id is already taken by the wrong user. An owner already recorded is likewise never
		// re-pointed: ownership moves through ReassignOwnership, which compares against the expected previous
		// owner and re-derives the role along with it. A record whose role already says this client owns the entity
		// is excluded even when its owner id is still empty - that is a participant enrolled before the local
		// player id arrived, waiting for its own id, not a slot for a peer to name an owner in.
		const bool bOwnerIsFillable = !Existing->OwnerID.IsValid() && Existing->Role != ECrowdyRole::Owner;
		if (bOwnerIsFillable && Event.OwnerID.IsValid() && Event.OwnerID != LocalPlayerID)
		{
			Existing->OwnerID = Event.OwnerID;
		}

		// Whether an actor still has to be spawned is a separate question from what the record says. An actor
		// already standing for this id is the pooled proxy this branch was written for, and spawning again would
		// give the entity two bodies.
		if (IsValid(Existing->GetActor()))
		{
			return;
		}

		// Anything this client owns or hosts keeps both its record and its representation whatever a peer claims
		// about the id. Only a record that says "another client simulates this" can be displaced by that client.
		if (Existing->Role != ECrowdyRole::RemoteProxy)
		{
			return;
		}

		// Nothing live is behind the record: it is a slot a backend is about to activate an actor into, so the
		// backfill above is the whole of the work here and no second actor is spawned for it.
		if (!IsValid(Existing->GetParticipant()))
		{
			return;
		}

		// What is left is a non-actor stand-in for a remote entity, a crowd avatar enrolled from position updates
		// before this event landed. The entity does have a body and this event names it, so the spawn goes ahead;
		// the stand-in gives up the id in ReleaseRemoteStandIn, once the actor is about to exist.
	}

	// Duplicate spawn event arrived while the class is still loading - the first one is already pending.
	if (PendingRemoteSpawns.Contains(Event.EntityID))
		return;

	FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(Event.ClassID);
	if (!ClassPath.IsValid())
		ClassPath = FSoftClassPath(Event.ClassPath);

	if (!ClassPath.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Unknown ClassID %u and empty class path on remote spawn %s."),
			Event.ClassID, *Event.EntityID.ToString());
		return;
	}

	if (UClass* LoadedClass = ClassPath.ResolveClass())
	{
		FinishRemoteSpawn(Event, LoadedClass);
		return;
	}

	// Class is not in memory; stream it in and hold the spawn until it lands.
	// A destroy event arriving during the load cancels the spawn.
	FPendingRemoteSpawn& Pending = PendingRemoteSpawns.Add(Event.EntityID);
	Pending.SpawnEvent = Event;
	Pending.LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		ClassPath,
		FStreamableDelegate::CreateUObject(this, &UCrowdyEntitySubsystem::OnRemoteSpawnClassLoaded,
			Event.EntityID, ClassPath));
}

void UCrowdyEntitySubsystem::OnRemoteSpawnClassLoaded(const FGuid EntityID, const FSoftClassPath ClassPath)
{
	FPendingRemoteSpawn Pending;
	if (!PendingRemoteSpawns.RemoveAndCopyValue(EntityID, Pending))
		return; // destroyed (or torn down) while the class was loading

	UClass* LoadedClass = ClassPath.ResolveClass();
	if (!LoadedClass)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Failed to load class '%s' for remote spawn %s."),
			*ClassPath.ToString(), *EntityID.ToString());
		return;
	}

	FinishRemoteSpawn(Pending.SpawnEvent, LoadedClass);
}

void UCrowdyEntitySubsystem::ReleaseRemoteStandIn(const FGuid& NetID)
{
	const FCrowdyEntityRecord* Existing = Records.Find(NetID);
	if (!Existing)
	{
		return;
	}

	UObject* Participant = Existing->GetParticipant();
	if (!IsValid(Participant) || Participant->IsA<AActor>() || Existing->Role != ECrowdyRole::RemoteProxy)
	{
		return;
	}

	// Said out loud because it is a swap of what answers for an entity, not a detail: whatever was tracking the
	// stand-in (a Game Model binding, a targeting lookup) is dropped here and rebuilt against the actor.
	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[CrowdyEntitySubsystem]: Entity %s was standing in as '%s' when its spawn event arrived; releasing the id to the spawned actor."),
		*NetID.ToString(), *GetNameSafe(Participant));

	UnregisterEntity(NetID);
}

void UCrowdyEntitySubsystem::FinishRemoteSpawn(const FCrowdyEntitySpawnEvent& Event, UClass* EntityClass)
{
	// A non-actor stand-in may still hold this id (see HandleRemoteSpawn). It lets go here, immediately before the
	// actor exists, rather than when the event arrived: the entity is never left answering to nothing while a
	// class streams in, and the actor's own registration is not refused as a collision with the stand-in.
	ReleaseRemoteStandIn(Event.EntityID);

	// Same class as the owning client identity is injected before BeginPlay,
	// so the component registers as RemoteProxy instead of minting an ID.
	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(EntityClass, Event.SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Actor)) return;

	UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
		Component->InitIdentity(Event.EntityID, Event.OwnerID, ECrowdyRole::RemoteProxy, Event.ClassID);

	Actor->FinishSpawning(Event.SpawnTransform);

	if (!Component)
		Component = AddStaticEntityComponent(Actor, Event.EntityID, Event.OwnerID, ECrowdyRole::RemoteProxy, Event.ClassID);

	if (Component)
		Component->OnCrowdySpawned.Broadcast(Event.InitialState, false);
}

void UCrowdyEntitySubsystem::HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event)
{
	// Destroyed before its class finished loading: the actor never existed
	// here, so cancel the load and drop the pending spawn.
	if (FPendingRemoteSpawn* Pending = PendingRemoteSpawns.Find(Event.EntityID))
	{
		if (Pending->LoadHandle.IsValid())
			Pending->LoadHandle->CancelHandle();
		PendingRemoteSpawns.Remove(Event.EntityID);
		return;
	}

	const FCrowdyEntityRecord* Record = Records.Find(Event.EntityID);
	if (!Record)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Entity %s already destroyed on remote destroy."),
			*Event.EntityID.ToString());
		return;
	}

	AActor* Actor = Record->GetActor();

	float Delay = 0.f;
	if (IsValid(Actor))
	{
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
			Component->OnCrowdyDestroyed.Broadcast(false);
			Delay = Component->DestroyDelay;
		}
	}

	UnregisterEntity(Event.EntityID);
	DestroyAfterDelay(Actor, Delay);
}

//Internal

UCrowdyEntityComponent* UCrowdyEntitySubsystem::AddStaticEntityComponent(AActor* Actor, const FGuid& EntityID,
	const FGuid& EntityOwnerID, const ECrowdyRole Role, const uint32 ClassID) const
{
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor, TEXT("CrowdyEntityComponent"));
	if (!IsValid(Component)) return nullptr;

	Component->Mode = ECrowdyEntityMode::Static;
	Component->InitIdentity(EntityID, EntityOwnerID, Role, ClassID);
	Component->RegisterComponent();

	return Component;
}

void UCrowdyEntitySubsystem::DestroyAfterDelay(AActor* Actor, const float Delay) const
{
	if (!IsValid(Actor)) return;

	if (Delay > 0.f)
	{
		FTimerHandle Handle;
		GetWorld()->GetTimerManager().SetTimer(Handle, [Actor]()
		{
			if (IsValid(Actor)) Actor->Destroy();
		}, Delay, false);
	}
	else
	{
		Actor->Destroy();
	}
}
