// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#include "Components/ActorComponent.h" // EComponentCreationMethod for the runtime-added component warning
#include "CrowdyCppClient.h" // the async client every Game Model API call runs on
#include "CrowdyGameModelLog.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Core/FCrowdyTypeID.h" // CROWDY_INVALID_CLASS_ID for the recorded-class check
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Misc/App.h"    // FApp::GetCurrentTime for the self-echo window
#include "Misc/Base64.h" // channel-payload decode fallback (docs call payload base64 binary)
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h" // the game-instance owner of the API client
#include "Policies/CondensedJsonPrintPolicy.h" // compact serialize of a collection item's state JSON
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership for auto-registering subsystems
#include "Replication/GameModel/CrowdyAttributeChange.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/GameModel/CrowdyBindingKeyProvider.h"
#include "Replication/GameModel/CrowdyContainerStandIn.h" // what holds a component container's record on a drawn row
#include "Replication/GameModel/CrowdyEntityClassContainer.h" // the container an entity's own recorded class declares
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h" // signal name validation + handler naming, shared with authoring
#include "Replication/GameModel/CrowdyJsonStringSupport.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/GameModel/CrowdyModelChangedPing.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/GameModel/CrowdyModelIdentityProvider.h"
#include "Replication/GameModel/CrowdyModelValueCodec.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h" // coalesce windows and invoke backoffs run on the world's timer manager
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdyBakedRegistry.h" // container pull-on-start setting (live metadata or the bake)
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UEventPayloadRegistry.h"

// Gate for the fallback ping. 1 (default): after a successful invoke the acting client emits an
// FCrowdyModelChangedPing so peers re-pull. 0: rely solely on the server-native model-driven notification,
// so a 2-client PIE test can prove the SERVER_EVENT path in isolation.
static TAutoConsoleVariable<int32> CVarEmitFallbackPing(
	TEXT("crowdy.gamemodel.emitfallbackping"),
	1,
	TEXT("1 (default): acting client emits a fallback model-changed ping after a successful Game Model invoke so peers re-pull. 0: rely solely on the server-native model-driven notification (isolates that path in 2-client PIE)."),
	ECVF_Default);

namespace
{
	// How long (seconds) a container stays "recently self-invoked" for the self-echo drop. Long enough to cover a
	// server commit + model-driven-notification round-trip, short enough that a rapid foreign change is not
	// suppressed - and the drop is consume-once anyway, so only the first echo after a self-invoke is skipped.
	constexpr double SelfEchoWindowSeconds = 3.0;

	// Broadcast each captured attribute change once, after the caller has fully updated the cache (and fired any
	// OnReps). Target is the bound object (null for a free/data container); ModelId is the container id. No-op when
	// nothing changed or nothing is listening, so the common no-observer path costs a single IsBound check.
	void BroadcastAttributeChanges(FCrowdyModelAttributeChanged& Delegate, UObject* Target, const FString& ModelId,
		const TArray<FCrowdyAttributeChange>& Changes)
	{
		if (Changes.Num() == 0 || !Delegate.IsBound())
		{
			return;
		}
		for (const FCrowdyAttributeChange& Change : Changes)
		{
			Delegate.Broadcast(Target, ModelId, Change.Key, Change.OldValueJson, Change.NewValueJson);
		}
	}

	// Past this nesting depth JsonValueToCompactString stops recursing and hands the remaining subtree to the
	// engine's iterative serializer, so a forged deeply-nested server value cannot overflow the C++ stack in this
	// walk. A real Server Owned object is at most a handful deep (the value codec caps a decoded struct at
	// MaxStructDepth) and a free/data container's JSON is shallow, so this bound only bites hostile input - which
	// does not need the sorted-key idempotency the shallow path provides.
	constexpr int32 CanonicalRecursionLimit = 64;

	// Canonical, comparison-stable string for one JSON value. Both apply paths (pulled state and confirmed
	// mutations) run values through THIS function before the cache diff, so equal values always produce equal
	// strings regardless of the server's textual formatting a re-pull that returns unchanged values fires no
	// OnRep. Integers, bools, strings and null are exact; non-integer numbers use the same float print on both
	// sides. An array canonicalizes element-wise in order; an object canonicalizes with SORTED keys (recursively),
	// so a server that returns an object's keys in a different order - or reformats a nested number - is not read
	// as a change and does not spuriously re-fire OnRep. Recursion is bounded by CanonicalRecursionLimit.
	FString JsonValueToCompactString(const TSharedPtr<FJsonValue>& Value, int32 Depth = 0)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			if (FMath::Abs(Number) < 9.2e18 && FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
			{
				return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)));
			}
			return FString::SanitizeFloat(Number);
		}
		case EJson::String:
			return FString::Printf(TEXT("\"%s\""), *EscapeJsonStringBody(Value->AsString()));
		case EJson::Null:
			return TEXT("null");
		default:
		{
			// A forged value nested past the bound would overflow the C++ stack in this recursive walk; hand the
			// deep subtree to the engine's iterative serializer instead. Only hostile input reaches here.
			if (Depth >= CanonicalRecursionLimit)
			{
				FString Out;
				const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
				if (Value->Type == EJson::Object && Value->AsObject().IsValid())
				{
					FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
				}
				else if (Value->Type == EJson::Array)
				{
					FJsonSerializer::Serialize(Value->AsArray(), Writer);
				}
				return Out;
			}
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
				FString Out = TEXT("[");
				for (int32 Index = 0; Index < Array.Num(); ++Index)
				{
					if (Index > 0)
					{
						Out += TEXT(",");
					}
					Out += JsonValueToCompactString(Array[Index], Depth + 1); // ordered; each element canonicalized identically
				}
				return Out + TEXT("]");
			}
			if (Value->Type == EJson::Object && Value->AsObject().IsValid())
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				TArray<TPair<FString, TSharedPtr<FJsonValue>>> Pairs;
				Pairs.Reserve(Object->Values.Num());
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					Pairs.Add(Pair);
				}
				Pairs.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B)
				{
					return A.Key < B.Key;
				});
				FString Out = TEXT("{");
				for (int32 Index = 0; Index < Pairs.Num(); ++Index)
				{
					if (Index > 0)
					{
						Out += TEXT(",");
					}
					Out += FString::Printf(TEXT("\"%s\":%s"), *EscapeJsonStringBody(Pairs[Index].Key),
						*JsonValueToCompactString(Pairs[Index].Value, Depth + 1));
				}
				return Out + TEXT("}");
			}
			return FString();
		}
		}
	}

	// Parses a standalone JSON value string (a mutation's newValueJson: "87", "\"Aria\"", "12.5", "true").
	// The JSON reader does not accept a bare top-level scalar, so wrap it in an object and pull the value out.
	TSharedPtr<FJsonValue> ParseJsonValueString(const FString& Json)
	{
		// The value is a double-encoded server string (a mutation's newValueJson), re-parsed here a layer below the
		// transport guard, so reject a deeply-nested one before it can build a stack-overflowing DOM.
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Json))
		{
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] rejected a JSON value string exceeding the max nesting depth (%d)."),
				CrowdyJsonSafety::MaxNestingDepth);
			return nullptr;
		}
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Json);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Object;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}

	// The CrowdyModel attribute whose lowercased name matches a server property key (from live metadata in the
	// editor; a cooked build reads the baked table). Null when there is no such attribute on Container's class.
	// Served from the per-class attribute table so a pull carrying many keys does not re-walk the class's
	// properties for each one.
	FProperty* FindModelPropertyForKey(const UObject* Container, const FName ServerKey)
	{
		return FCrowdyModelAttributeLookup::FindProperty(Container, ServerKey);
	}

	// The parameterless CrowdyOnRep function name declared on the CrowdyModel attribute for a server key. Comes
	// out of the same table entry the member write resolved, so finding the notify costs no second lookup pass.
	FName FindOnRepForServerKey(const UObject* Container, const FName ServerKey)
	{
		return FCrowdyModelAttributeLookup::FindOnRep(Container, ServerKey);
	}

	// Fire a parameterless CrowdyOnRep, mirroring CrowdyState's receive path exactly: resolve the UFUNCTION,
	// guard NumParms == 0 (defense-in-depth for a drifted binding), call with a null frame. GAS-style the new
	// value is already on the live member and in the cache; there is no previous-value argument.
	void FireParameterlessOnRep(UObject* Container, const FName OnRepName)
	{
		if (!Container || OnRepName == NAME_None)
		{
			return;
		}
		if (UFunction* OnRepFn = Container->FindFunction(OnRepName))
		{
			if (OnRepFn->NumParms == 0)
			{
				Container->ProcessEvent(OnRepFn, nullptr);
			}
		}
	}

	// Writes an authoritative value onto the live CrowdyModel UPROPERTY so OnRep reads it off the member like a
	// normal replicated property (the "feels native" payoff) mirroring how CrowdyState decodes onto the live
	// property before firing OnRep. The value codec owns every supported leaf and aggregate (scalars, scalar
	// arrays, container refs) and bounds a forged array/reference, so a malformed server value is a safe no-op.
	// Returns true only when the value actually applied to the member: false for a non-attribute key or a value
	// the codec rejected, so the caller does not advance its cache or fire a change the member never took.
	bool WriteJsonValueToModelProperty(UObject* Container, const FName ServerKey, const TSharedPtr<FJsonValue>& Value)
	{
		FProperty* Property = FindModelPropertyForKey(Container, ServerKey);
		if (!Property || !Value.IsValid())
		{
			return false;
		}
		void* Addr = Property->ContainerPtrToValuePtr<void>(Container);
		return FCrowdyModelValueCodec::DecodeJsonToProperty(Property, Addr, Value);
	}

	// Wraps the client's raw `data` object back into a { "data": ... } GraphQL envelope so it can be fed to the
	// matching FCrowdyGameApiCodec::ParseXEnvelope, which reads Envelope->data.
	TSharedPtr<FJsonObject> WrapCppDataEnvelope(const TSharedPtr<FJsonObject>& DataObject)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		if (DataObject.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), DataObject);
		}
		return Envelope;
	}

	// Maps the client's plain parsed session data (CrowdyNet) to the Blueprint-facing struct at the façade edge.
	FCrowdyGameModelSession ToBpSession(const FCrowdyGameSessionData& Data)
	{
		FCrowdyGameModelSession Out;
		Out.SessionId = Data.SessionId;
		Out.Name = Data.Name;
		Out.Status = Data.Status;
		Out.CreatedByUserId = Data.CreatedByUserId;
		Out.CurrentTurnUserId = Data.CurrentTurnUserId;
		Out.bHasCurrentTurn = Data.bHasCurrentTurn;
		Out.MetadataJson = Data.MetadataJson;
		return Out;
	}

	FCrowdyContainerEdge ToBpEdge(const FCrowdyEdgeData& Data)
	{
		FCrowdyContainerEdge Out;
		Out.EdgeId = Data.EdgeId;
		Out.FromContainerId = Data.FromContainerId;
		Out.ToContainerId = Data.ToContainerId;
		Out.RelationshipType = Data.RelationshipType;
		Out.Weight = static_cast<float>(Data.Weight);
		return Out;
	}

	// Reads a raw GmContainer JSON object (as ListContainers/Traverse return) into the Blueprint-facing ref.
	FCrowdyContainerRef ToBpContainerRef(const TSharedPtr<FJsonObject>& Obj)
	{
		FCrowdyContainerRef Out;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(TEXT("containerId"), Out.ContainerId);
			Obj->TryGetStringField(TEXT("typeName"), Out.TypeName);
			Obj->TryGetStringField(TEXT("displayName"), Out.DisplayName);
			Obj->TryGetStringField(TEXT("sessionId"), Out.SessionId);
			Obj->TryGetStringField(TEXT("metadataJson"), Out.MetadataJson);
			FString OwnerStr;
			if (Obj->TryGetStringField(TEXT("ownerUserId"), OwnerStr) && !OwnerStr.IsEmpty())
			{
				LexFromString(Out.OwnerUserId, *OwnerStr);
			}
		}
		return Out;
	}
}

void UCrowdyGameModelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Mark this world's Game Model session live. ResolveLiveSelf hands out this subsystem only while the token is
	// held, so a completion that lands after Deinitialize still runs but can no longer write into our state.
	WorldSessionToken = MakeShared<uint8>(0);

	// The model-changed sink: bind the re-pull as the sole OnModelChanged consumer here so it survives even if
	// the reception-layer registration below early-returns (no game instance in the transient world). Every
	// carrier broadcasts via NotifyModelChanged; this is where a normalized hint becomes a re-pull.
	OnModelChangedDelegate.AddUObject(this, &UCrowdyGameModelSubsystem::HandleModelChangeHint);

	// Register the fallback ping payload so the reflection serializer can encode/decode it as a CrowdyEvent.
	// Deterministic path-hash TypeID => sender and receiver agree without a data-asset entry.
	if (UEventPayloadRegistry* Registry = UEventPayloadRegistry::Get())
	{
		Registry->RegisterStructAuto(FCrowdyModelChangedPing::StaticStruct());
	}

	// Subscribe to the model-changed carriers so server-native notifications reach HandleModelChangedDelivery.
	// GetGameInstance() is null during the transient world at engine init guard it (a world-subsystem
	// invariant); ShouldCreateSubsystem already restricts us to PIE/Game worlds.
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		return;
	}
	Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	EnsureReceptionLayerRegistered();

	// Auto-bind: bring the entity subsystem up FIRST (InitializeDependency), else an entity that registers
	// before we subscribe would broadcast OnEntityRegistered into the void (the InitializeDependency footgun
	// from Subsystem Replication). Then subscribe so a CrowdyModel-attributed entity resolves-or-creates its
	// container on registration.
	Collection.InitializeDependency(UCrowdyEntitySubsystem::StaticClass());
	EntitySubsystemForEvents = GetWorld()->GetSubsystem<UCrowdyEntitySubsystem>();
	if (EntitySubsystemForEvents)
	{
		EntitySubsystemForEvents->OnEntityRegistered.AddDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityRegistered);
		EntitySubsystemForEvents->OnEntityUnregistered.AddDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityUnregistered);
	}

	// A pending resolve (an entity that registered before the token was minted, or before its owner created the row)
	// is otherwise re-driven only by an inbound model-changed notification. OnHostIDUpdated fires around connect /
	// host election - near token mint and on host migration - so bind it as a non-notification liveness kick.
	if (UCrowdyGameSession* Session = GetGameSession())
	{
		Session->OnHostIDUpdated.AddDynamic(this, &UCrowdyGameModelSubsystem::HandleHostChanged);
	}
}

void UCrowdyGameModelSubsystem::Deinitialize()
{
	// The teardown latch goes up before anything is drained. A drained caller's completion can re-enter and try to
	// apply again, and a merge window or a backoff armed after this point would sit on a timer manager that is about
	// to be destroyed, so it would never fire and its caller would wait forever. With the latch set, a late apply is
	// sent on its own instead of being queued.
	bShuttingDown = true;
	FailPendingCoalesceWindows();
	FailPendingInvokeRetries();

	// The pending-bind sweep has no caller to notify (it only re-drives binds), but it is armed on this world's timer
	// manager, so it is cleared here with the rest.
	if (UWorld* TimerWorld = GetWorld())
	{
		TimerWorld->GetTimerManager().ClearTimer(PendingSweepTimer);
	}

#if !UE_BUILD_SHIPPING
	// Closed first, so leaving a session shuts the socket rather than leaving it to the client host's own teardown.
	DebugStopWatchingContainerChanges();
#endif

	// Unbind auto-bind delegates before the entity subsystem tears down (same-lifetime world subsystems, but
	// unbind explicitly so a late broadcast never lands on a half-destroyed subsystem).
	if (EntitySubsystemForEvents)
	{
		EntitySubsystemForEvents->OnEntityRegistered.RemoveDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityRegistered);
		EntitySubsystemForEvents->OnEntityUnregistered.RemoveDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityUnregistered);
		EntitySubsystemForEvents = nullptr;
	}

	// Drop the host liveness-kick subscription (the game session is a GameInstance subsystem, so it outlives this
	// world subsystem; unbind explicitly on teardown). GetGameSession resolves it while the game instance is alive.
	if (UCrowdyGameSession* Session = GetGameSession())
	{
		Session->OnHostIDUpdated.RemoveDynamic(this, &UCrowdyGameModelSubsystem::HandleHostChanged);
	}

	// Releasing the handle stops any further delivery to this subscriber, including later in the same fan-out, so
	// this needs no lookup back through the bridge. See FCrowdyDelivery for the threading rules.
	ModelChangedSubscription.Release();
	bReceptionLayerRegistered = false;
	Bridge = nullptr;

	// Clear all per-world caches for hygiene on teardown. Actor-entity traces are normally dropped in
	// HandleEntityUnregistered, but clear them symmetrically here too so a torn-down world leaves nothing stale.
	NetIDToContainerId.Empty();
	ContainerIdToNetIDs.Empty();
	BindEpochByNetID.Empty();
	ContainerCache.Empty();
	ContainerTypeByNetID.Empty();
	NetIDToOwnerUserId.Empty();
	PendingModelEntities.Empty();
	PendingBindBackoff.Empty();
	ResolveInFlight.Empty();
	ClassDerivedBindings.Empty();
	SubParticipantsByAnchor.Empty();
	ContainerStandIns.Empty();
	PendingRefreshPulls.Empty();
	DataContainerCache.Empty();
	WatchedDataContainers.Empty();
	RecentlySelfActed.Empty();

	// The default session context and the last-error cache are per-world; clear them on teardown so a fresh world
	// never inherits a stale session or error string.
	ActiveSessionId.Reset();
	LastModelError.Reset();

	// Drop the sink subscriber (bound to this in Initialize) explicitly on teardown.
	OnModelChangedDelegate.Clear();

	// End this world's Game Model session. The API client outlives us, so a request still in flight will complete
	// later; that completion still runs and still notifies whoever asked for it, but dropping this token stops it
	// from writing into everything cleared above.
	WorldSessionToken.Reset();

	Super::Deinitialize();
}

bool UCrowdyGameModelSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

bool UCrowdyGameModelSubsystem::IsCurrentWorldForGameInstance() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UWorld* InstanceWorld = GameInstance ? GameInstance->GetWorld() : nullptr;

	// Only a POSITIVELY identified mismatch means a departed world. No world, no game instance, or a game instance
	// that cannot name its current world are all "undetermined", and they have to answer true: folding them in with
	// the departed case would drop every delivery instead of only the duplicate ones this exists to drop, which
	// fails far more quietly than the bug it fixes.
	if (!World || !InstanceWorld)
	{
		return true;
	}

	// The game instance's world is whichever one it most recently travelled to, so this is true for exactly one
	// world at a time and false for every world left behind.
	return InstanceWorld == World;
}

void UCrowdyGameModelSubsystem::EnsureReceptionLayerRegistered()
{
	if (bReceptionLayerRegistered)
	{
		return;
	}


	if (!Bridge)
	{
		const UWorld* World = GetWorld();
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		Bridge = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>() : nullptr;
	}

	if (Bridge && Bridge->ServiceRegistry)
	{
		SubscribeToModelChangedCarriers(*Bridge->ServiceRegistry);
		bReceptionLayerRegistered = true;
	}
}

void UCrowdyGameModelSubsystem::ReleaseReceptionLayer()
{
	ModelChangedSubscription.Release();
	bReceptionLayerRegistered = false;
}

void UCrowdyGameModelSubsystem::SubscribeToModelChangedCarriers(FCrowdyServiceRegistry& Registry)
{
	// One subscription per registry, held by the subsystem that subscribed most recently.
	//
	// The registry belongs to the game instance and outlives every world under it, so each level travelled through
	// leaves its subsystem subscribed until that world is collected, and every frame is then handled once per
	// subscription. Comparing each world against the game instance's current world is not enough on its own: a
	// departed world here still reported itself as current, so the duplicates survived that check.
	//
	// Subscription order follows travel order, so the most recent subscriber is the world just entered and every
	// earlier one is a world left behind. Releasing them here is decided by that order alone, with no dependence on
	// what a departed world reports about itself.
	// A world that handed its claim to a newer one never takes it back. The subscribe path is reached from three
	// call sites, including the entity-bind one, so a departed world still running any of them would otherwise
	// re-subscribe and displace the world the player is actually in, silencing it. The latch lives here rather than
	// at a caller so it holds for every path into a subscription.
	if (bSupersededByNewerWorld)
	{
		return;
	}

	static TMap<const FCrowdyServiceRegistry*, TWeakObjectPtr<UCrowdyGameModelSubsystem>> ActiveByRegistry;
	TWeakObjectPtr<UCrowdyGameModelSubsystem>& Active = ActiveByRegistry.FindOrAdd(&Registry);
	if (UCrowdyGameModelSubsystem* Previous = Active.Get())
	{
		if (Previous != this)
		{
			Previous->ReleaseReceptionLayer();
			Previous->bSupersededByNewerWorld = true;
			UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
				TEXT("[GameModel] released the model-changed subscription held by world '%s'; world '%s' now owns it"),
				*GetNameSafe(Previous->GetWorld()), *GetNameSafe(GetWorld()));
		}
	}
	Active = this;

	const FCrowdySubscriptionKey Carriers[] =
	{
		// The server-native SERVER_EVENT (139), keyed on the raw event type id (there is no
		// UScriptStruct for it, the state is raw application bytes).
		FCrowdySubscriptionKey::EventPayload(CrowdyGameModelMetaKeys::ModelChangedEventType),
		// The fallback ping (opcode 138), keyed on its struct.
		FCrowdySubscriptionKey::EventPayload(FCrowdyModelChangedPing::StaticStruct()),
		// The session channel (opcode 18). Also carries chat and reliable-RPC frames, so the
		// cmc: prefix decode inside HandleModelChangedDelivery is what actually recognizes our own traffic.
		FCrowdySubscriptionKey::Opcode(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION),
	};
	ModelChangedSubscription = Registry.Subscribe(Carriers,
		{ ECrowdySubscriptionRole::Handle, /*bRequiresExclusiveHandling*/ false, TEXT("CrowdyGameModelSubsystem") },
		[this](const FCrowdyDelivery& Delivery) { HandleModelChangedDelivery(Delivery); });

	// Name the world and its game instance. The service registry is game-instance scoped while this subsystem is
	// per-world, so two worlds sharing one game instance subscribe twice and every frame is handled twice: the
	// signal fires twice, and every bind, ensure and re-pull it triggers is doubled. Without this identity the log
	// only showed a repeated line with nothing to attribute it to.
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] subscribed to the three model-changed carriers (server-native 139, fallback ping 138, channel 18) for world '%s' (type %d, netmode %d, gameinstance %p, registry %p, instance world '%s', iscurrent %d)"),
		*GetNameSafe(World), World ? static_cast<int32>(World->WorldType.GetValue()) : -1,
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		static_cast<const void*>(GameInstance), static_cast<const void*>(&Registry),
		*GetNameSafe(GameInstance ? GameInstance->GetWorld() : nullptr),
		IsCurrentWorldForGameInstance() ? 1 : 0);
}

void UCrowdyGameModelSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// The service registry may not have existed at Initialize (the SDK subsystem creates it in its post-world-init
	// pass); by BeginPlay it does, so ensure the reception registration actually happened.
	EnsureReceptionLayerRegistered();
	AutoRegisterTaggedSubsystems();
}

void UCrowdyGameModelSubsystem::AutoRegisterTaggedSubsystems()
{
	UWorld* World = GetWorld();
	if (!World || !EntitySubsystemForEvents)
	{
		return;
	}

	// Enroll one tagged subsystem: register it as a Host participant if it is not already one, else just ensure it
	// is bound. RegisterParticipant broadcasts OnEntityRegistered -> HandleEntityRegistered, which resolves and
	// binds; HandleEntityRegistered is idempotent for an already-bound one.
	auto EnrollSubsystem = [this](UObject* Sub)
	{
		if (!Sub || Sub == this)
		{
			return;
		}
		FString TypeName;
		if (!FCrowdyAttributeRegistry::GetContainerTypeName(Sub->GetClass(), TypeName))
		{
			return; // not tagged as a Game Model container
		}
		const FGuid Existing = EntitySubsystemForEvents->FindEntityID(Sub);
		if (Existing.IsValid())
		{
			// Already a participant (e.g. Subsystem Replication enrolled it, possibly before we subscribed);
			// make sure it is bound as a Game Model container too.
			HandleEntityRegistered(Existing);
			return;
		}
		const FGuid NetID = EntitySubsystemForEvents->RegisterParticipant(Sub, ECrowdyOwnership::Host);
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] auto-registered subsystem '%s' as Host participant %s (container '%s')"),
			*Sub->GetClass()->GetName(), *NetID.ToString(), *TypeName);
	};

	for (UWorldSubsystem* Sub : World->GetSubsystemArrayCopy<UWorldSubsystem>())
	{
		EnrollSubsystem(Sub);
	}
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		for (UGameInstanceSubsystem* Sub : GameInstance->GetSubsystemArrayCopy<UGameInstanceSubsystem>())
		{
			EnrollSubsystem(Sub);
		}
	}
}

void UCrowdyGameModelSubsystem::HandleModelChangedDelivery(const FCrowdyDelivery& Delivery)
{
	// This subsystem is per-world, but the service registry it subscribes to belongs to the game instance, which
	// outlives any single world. Travelling from one level to the next leaves the departed world's subsystem
	// subscribed until that world is actually collected, so every carrier frame is handled once per world still
	// holding a subscription: a signal fires N times on N-1 dead worlds, and every re-pull, ensure and bind it
	// drives is multiplied the same way (which saturates the HTTP queue long before anyone notices the duplicate
	// signal). Only the game instance's current world may act on a delivery; a departed one drops it.
	if (!IsCurrentWorldForGameInstance())
	{
		return;
	}

	// Decode the changed container off whichever model-changed carrier this is (server-native spatial opcode
	// 139, the fallback ping on opcode 138, or the channel opcode 18). All three converge on one hint, which
	// the notification sink turns into a re-pull. Routing already matched the 139 and 138 carriers by event
	// id/struct, so the EventType gate that used to sit here for 139 is gone; only the channel carrier still
	// needs its own payload filter, since opcode 18 genuinely multiplexes chat and reliable-RPC frames
	// alongside ours.
	FCrowdyModelChangeHint Hint;

	if (Delivery.Opcode == ECrowdyMessageType::SERVER_EVENT_NOTIFICATION)
	{
		const FServerEventNotification& Event = Delivery.GetAs<FServerEventNotification>();

		// Diagnostic: surface exactly what the server delivered and where, so which wire field carries the
		// identifier is answerable straight from the trace (crowdy.gamemodel.trace 1).
		if (CrowdyGameModelTrace::GameModel())
		{
			FString StateStr;
			StateStr.Reserve(Event.StateView.Num());
			for (const uint8 Byte : Event.StateView)
			{
				StateStr.AppendChar((Byte >= 32 && Byte < 127) ? static_cast<TCHAR>(Byte) : TEXT('.'));
			}
			UE_LOG(LogCrowdyGameModel, Log,
				TEXT("[GameModel] SERVER_EVENT(139) model-changed: state='%s' (%d bytes) uuid='%s' chunk=(%lld,%lld,%lld)"),
				*StateStr, Event.StateView.Num(), *Event.UUID.ToString(), Event.ChunkX, Event.ChunkY, Event.ChunkZ);
		}

		Hint.EventType = Event.EventType;

		// Read here, inside the delivery: the container id becomes an FString that outlives the frame, and
		// nothing past this point touches the octets.
		if (!DecodeContainerIdFromState(Event.StateView, Hint.ContainerId))
		{
			UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Warning,
				TEXT("[GameModel] model-changed SERVER_EVENT with no decodable container id — dropped (state=%d bytes)"),
				Event.StateView.Num());
			return;
		}
	}
	else if (Delivery.Opcode == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
	{
		// The fallback ping. Routing matches this delivery by the wire EventType field, not by the decoded
		// struct, so a forged or drifted sender can still land here with an empty or mismatched payload;
		// GetPtr returns null for either case instead of asserting.
		if (!Delivery.Payload)
		{
			return;
		}
		const FCrowdyModelChangedPing* Ping = Delivery.Payload->GetPtr<FCrowdyModelChangedPing>();
		if (!Ping)
		{
			return;
		}
		Hint = MakeHintFromPing(*Ping);
	}
	else if (Delivery.Opcode == ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION)
	{
		// The channel opcode also carries chat and reliable-RPC frames (UCrowdyChannels subscribes to the same
		// opcode independently). Only a payload that carries our cmc: prefix is a model-changed notification;
		// anything else is silently ignored. The payload is untrusted - DecodeChannelModelChangedId is
		// bounds/prefix-gated.
		const FChannelMessageNotification& Msg = Delivery.GetAs<FChannelMessageNotification>();

		// Diagnostic: log EVERY opcode-18 frame that reaches this layer (before the cmc: filter), so a missing wave
		// notification can be told apart - "no frame arrives at all" (server delivery) vs "a frame arrives but does
		// not decode" (payload/encoding). Channel RPC/chat frames also land here and will show as non-cmc payloads.
		if (CrowdyGameModelTrace::GameModel())
		{
			FString PayloadStr;
			PayloadStr.Reserve(Msg.Payload.Num());
			for (const uint8 Byte : Msg.Payload)
			{
				PayloadStr.AppendChar((Byte >= 32 && Byte < 127) ? static_cast<TCHAR>(Byte) : TEXT('.'));
			}
			UE_LOG(LogCrowdyGameModel, Log,
				TEXT("[GameModel] CHANNEL(18) frame received: payload='%s' (%d bytes, channel %lld)"),
				*PayloadStr, Msg.Payload.Num(), Msg.ChannelId);
		}

		// A signal rides the same channel with its own prefix. It is checked first and returns either way: a signal
		// reports that something happened and changes no state, so it must NOT fall through and trigger a re-pull.
		{
			FString SignalName;
			FString SignalContainerId;
			if (DecodeChannelSignal(Msg.Payload, SignalName, SignalContainerId))
			{
				UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
					TEXT("[GameModel] CHANNEL(18) signal '%s' for container '%s' (channel %lld)"),
					*SignalName, *SignalContainerId, Msg.ChannelId);
				DispatchSignal(SignalName, SignalContainerId);
				return;
			}
		}

		if (!DecodeChannelModelChangedId(Msg.Payload, Hint.ContainerId))
		{
			return;
		}
		Hint.EventType = CrowdyGameModelMetaKeys::ModelChangedEventType;

		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] CHANNEL(18) model-changed: container='%s' (payload %d bytes, channel %lld)"),
			*Hint.ContainerId, Msg.Payload.Num(), Msg.ChannelId);
	}
	else
	{
		return;
	}

	// Every carrier converges on NotifyModelChanged (the sink), which turns the hint into a re-pull. The
	// session token is still checked, because a subsystem whose world has been torn down must not write
	// into state it no longer owns.
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
	{
		Self->NotifyModelChanged(Hint);
	}
}

UCrowdyGameSession* UCrowdyGameModelSubsystem::GetGameSession() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance->GetSubsystem<UCrowdyGameSession>();
		}
	}
	return nullptr;
}

UCrowdyEntitySubsystem* UCrowdyGameModelSubsystem::ResolveEntitySubsystem() const
{
	if (EntitySubsystemForEvents)
	{
		return EntitySubsystemForEvents;
	}
	return GetWorld() ? GetWorld()->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
}

AActor* UCrowdyGameModelSubsystem::ResolveEntityActor(const FGuid& NetID) const
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	return Entities ? Entities->FindEntity(NetID) : nullptr;
}

UObject* UCrowdyGameModelSubsystem::ResolveEntityParticipant(const FGuid& NetID) const
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	return Entities ? Entities->FindParticipant(NetID) : nullptr;
}

bool UCrowdyGameModelSubsystem::ResolveTargetNetID(const UObject* Target, FGuid& OutNetID) const
{
	OutNetID.Invalidate();
	UCrowdyEntitySubsystem* Entities = Target ? ResolveEntitySubsystem() : nullptr;
	if (!Entities)
	{
		return false;
	}
	OutNetID = Entities->FindEntityID(Target);
	return OutNetID.IsValid();
}

bool UCrowdyGameModelSubsystem::IsAuthoritativeToCreate(const FGuid& NetID) const
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities)
	{
		return false;
	}
	// A binding that came from the class the entity records is read-only, and stays read-only. That class is chosen
	// by the entity's own owner: it is sound for deciding what to read, and creating a server row on the strength of
	// it would let an entity's owner mint rows of any type it cares to name.
	if (ClassDerivedBindings.Contains(NetID))
	{
		return false;
	}
	// Locally owns the entity (a per-player container), or the entity is Host-owned (a world/shared container). A
	// shared entity is authoritative on EVERY client: gameModelEnsureContainer is atomic, so concurrent ensures
	// converge on one row and no host election is needed. A remote proxy of a per-player entity is not authoritative.
	return Entities->IsLocallyOwned(NetID) || IsHostOwnedEntity(NetID);
}

bool UCrowdyGameModelSubsystem::IsHostOwnedEntity(const FGuid& NetID) const
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities)
	{
		return false;
	}
	const FCrowdyEntityRecord* Record = Entities->FindRecord(NetID);
	return Record && Record->Role == ECrowdyRole::HostOwned;
}

bool UCrowdyGameModelSubsystem::ResolveApiContext(FString& OutEndpoint, FString& OutToken, int64& OutAppId) const
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	OutEndpoint = Settings ? Settings->GetGameApiHttpUrl() : FString();
	if (OutEndpoint.IsEmpty())
	{
		UE_LOG(LogCrowdyGameModel, Error,
			TEXT("[GameModel] Game API endpoint is empty — pick and sync an app on the CrowdyStudio console (Project page) before invoking Game Models."));
		return false;
	}

	const UCrowdyGameSession* Session = GetGameSession();
	if (!Session)
	{
		UE_LOG(LogCrowdyGameModel, Error, TEXT("[GameModel] No UCrowdyGameSession — cannot resolve the Game API bearer token."));
		return false;
	}

	// The app-scoped GAMEPLAY token is the bearer for the Game API (same source the query subsystem uses).
	OutToken = Session->GetGameToken();
	if (OutToken.IsEmpty())
	{
		UE_LOG(LogCrowdyGameModel, Error,
			TEXT("[GameModel] No app-scoped game token — sign in and mint an app token before invoking Game Models."));
		return false;
	}

	OutAppId = Session->GetAppID();
	return true;
}

void UCrowdyGameModelSubsystem::Invoke(const FCrowdyInvokeRequest& Req, TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	// The session is resolved ONCE, here, rather than per attempt: an empty Session Id falls back to the active
	// (default) session, and a retry has to carry the same session the first attempt did even if the active session
	// changed while the backoff was running. A retried call must be identical to the call it repeats.
	TSharedRef<FCrowdyInvokeRequest> Resolved = MakeShared<FCrowdyInvokeRequest>(Req);
	Resolved->SessionId = ResolveSessionId(Req.SessionId, ActiveSessionId);
	InvokeResolved(MoveTemp(Resolved), MoveTemp(OnDone));
}

bool UCrowdyGameModelSubsystem::IsInvokeBindingStillValid(const FCrowdyInvokeBindingGuard& Guard) const
{
	if (!Guard.bEntityBound)
	{
		return true;
	}
	const uint32* CurrentEpoch = BindEpochByNetID.Find(Guard.SelfNetID);
	return CurrentEpoch && *CurrentEpoch == Guard.BindEpoch;
}

void UCrowdyGameModelSubsystem::InvokeResolved(TSharedRef<FCrowdyInvokeRequest> Resolved,
	TFunction<void(FCrowdyInvokeResult)> OnDone, const FCrowdyInvokeBindingGuard& Guard)
{
	DispatchInvokeAttempt(MoveTemp(Resolved), 0, MoveTemp(OnDone), Guard);
}

void UCrowdyGameModelSubsystem::DispatchInvokeAttempt(TSharedRef<FCrowdyInvokeRequest> Resolved, int32 AttemptIndex,
	TFunction<void(FCrowdyInvokeResult)> OnDone, const FCrowdyInvokeBindingGuard& Guard)
{
	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		FCrowdyInvokeResult Failed;
		Failed.ErrorMessage = TEXT("Game Model API context unavailable (endpoint or token missing)");
		SetLastModelError(Failed.ErrorMessage);
		if (OnDone)
		{
			OnDone(Failed);
		}
		return;
	}

	// appId always comes from the session so it matches the token's app scope; the caller never sets it. It is
	// re-read on a retry along with the token, so a retry that outlives a token refresh still carries a matching pair.
	Resolved->AppId = AppId;

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] Invoke fn=%s self=%s appId=%lld session=%s attempt=%d"),
		*Resolved->FunctionName, *Resolved->SelfContainerId, AppId,
		Resolved->SessionId.IsEmpty() ? TEXT("<app-global>") : *Resolved->SessionId, AttemptIndex);

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		FCrowdyInvokeResult Failed;
		Failed.ErrorMessage = TEXT("Game Model API client unavailable");
		SetLastModelError(Failed.ErrorMessage);
		if (OnDone)
		{
			OnDone(Failed);
		}
		return;
	}

	// paramsJson is compact-serialized here (the client passes it through verbatim), matching
	// BuildInvokeVariables' SerializeParamsCompact: "{}" when there are no params.
	FString ParamsJson;
	if (Resolved->Params.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamsJson);
		FJsonSerializer::Serialize(Resolved->Params.ToSharedRef(), Writer);
	}
	else
	{
		ParamsJson = TEXT("{}");
	}

	// The callback lands from Poll() on the game thread. The mapping below is everything FCrowdyCppInvokeResult
	// carries, the fault attribution included: a caller that repeats a refused call has to be able to tell the
	// platform refusing it (repeat) from its own function failing (do not). The weak self is only consulted to decide
	// whether a retry is possible; a completion that arrives after the world is gone still reports its outcome.
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	Client->InvokeFunction(Resolved->AppId, Resolved->FunctionName, Resolved->SelfContainerId,
		Resolved->SessionId, ParamsJson,
		[WeakThis, Resolved, AttemptIndex, Guard, OnDone = MoveTemp(OnDone)](FCrowdyCppInvokeResult Result)
		{
			// A quarantined function refuses until its definition is written again, and the reason is the only
			// actionable thing in the refusal. Keyed on the non-empty reason rather than on the code, because the
			// player boundary rewrites the code to USER_CODE_ERROR on exactly this path while these fields survive.
			if (!Result.QuarantineReason.IsEmpty())
			{
				if (UCrowdyGameModelSubsystem* Reporter = ResolveLiveSelf(WeakThis))
				{
					const FString Subject = Result.QuarantinedName.IsEmpty()
						? Resolved->FunctionName : Result.QuarantinedName;
					Reporter->ReportModelRefusalOnce(TEXT("OBJECT_QUARANTINED"), Subject, Result.QuarantineReason);
				}
			}

			FCrowdyInvokeResult Mapped;
			Mapped.bTransportOk = Result.bTransportOk;
			Mapped.bSuccess = Result.bSuccess;
			Mapped.ReturnValueJson = MoveTemp(Result.ReturnValueJson);
			Mapped.ErrorMessage = MoveTemp(Result.ErrorMessage);
			Mapped.FaultCode = MoveTemp(Result.FaultCode);
			Mapped.Blame = CrowdyPlayerFaultBlameFromWireString(Result.Blame);
			Mapped.bRetryable = Result.bRetryable;
			Mapped.RetryAfterMs = Result.RetryAfterMs;
			Mapped.Mutations.Reserve(Result.Mutations.Num());
			for (FCrowdyCppMutationApplied& M : Result.Mutations)
			{
				FCrowdyMutationApplied Applied;
				Applied.ContainerId = MoveTemp(M.ContainerId);
				Applied.Key = MoveTemp(M.Key);
				Applied.OldValueJson = MoveTemp(M.OldValueJson);
				Applied.NewValueJson = MoveTemp(M.NewValueJson);
				Mapped.Mutations.Add(MoveTemp(Applied));
			}

			// A retry needs this world's timer manager, so it is only possible while the world session is live. When
			// it is not, the refusal is reported as-is rather than swallowed.
			UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
			if (Self && Self->TryScheduleBudgetRetry(Resolved, AttemptIndex, Mapped, OnDone, Guard))
			{
				return;
			}
			if (OnDone)
			{
				OnDone(MoveTemp(Mapped));
			}
		});
}

int32 UCrowdyGameModelSubsystem::GetRecentInvokeCount() const
{
	const double Cutoff = FApp::GetCurrentTime() - static_cast<double>(InvokeBudgetWindowSeconds);
	int32 Count = 0;
	// The ring holds its entries oldest-first from the head, so counting backwards from the newest stops at the first
	// entry that has aged out.
	for (int32 Offset = InvokeLedgerCount - 1; Offset >= 0; --Offset)
	{
		const int32 Index = (InvokeLedgerHead + Offset) % InvokeLedgerCapacity;
		if (InvokeLedger[Index] < Cutoff)
		{
			break;
		}
		++Count;
	}
	return Count;
}

void UCrowdyGameModelSubsystem::RecordInvokeAttempt(double Now)
{
	// One allocation for the life of the subsystem. Every record after this is a couple of index writes: this runs on
	// the way out of every Game Model call, so it must not touch the heap or move the entries already in it.
	if (InvokeLedger.Num() != InvokeLedgerCapacity)
	{
		InvokeLedger.SetNumZeroed(InvokeLedgerCapacity);
		InvokeLedgerHead = 0;
		InvokeLedgerCount = 0;
	}

	// Anything older than the window can never affect the answer again, so the head is walked past it on the way in
	// and the live span stays the size of one window's traffic.
	const double Cutoff = Now - static_cast<double>(InvokeBudgetWindowSeconds);
	while (InvokeLedgerCount > 0 && InvokeLedger[InvokeLedgerHead] < Cutoff)
	{
		InvokeLedgerHead = (InvokeLedgerHead + 1) % InvokeLedgerCapacity;
		--InvokeLedgerCount;
	}

	// A hard cap on top of the prune, because the prune alone assumes the clock only moves forward. The ledger exists
	// only to answer how much of the allowance is spent, and past the ceiling the answer is already "all of it", so
	// overwriting the oldest entry costs nothing and bounds the memory unconditionally.
	if (InvokeLedgerCount == InvokeLedgerCapacity)
	{
		InvokeLedgerHead = (InvokeLedgerHead + 1) % InvokeLedgerCapacity;
		--InvokeLedgerCount;
	}

	InvokeLedger[(InvokeLedgerHead + InvokeLedgerCount) % InvokeLedgerCapacity] = Now;
	++InvokeLedgerCount;
}

float UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(int32 AttemptIndex)
{
	// A doubling backoff on the entity's own attempt count. An entity stays pending because something outside this
	// client has not happened yet (its owner has not created the row, this client has no token), and asking again at
	// the sweep's own cadence forever spends the allowance on a question whose answer is not changing.
	const int32 Doublings = FMath::Clamp(AttemptIndex, 0, 8);
	const float Delay = MinPendingRetryDelaySeconds * static_cast<float>(1 << Doublings);
	return FMath::Clamp(Delay, MinPendingRetryDelaySeconds, MaxPendingRetryDelaySeconds);
}

float UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(float AuthoredSeconds, int32 RecentInvokes, int32 Limit)
{
	if (AuthoredSeconds <= 0.0f)
	{
		return 0.0f;
	}
	const float Bounded = FMath::Min(AuthoredSeconds, MaxCoalesceWindowSeconds);
	if (Limit <= 0 || RecentInvokes <= 0)
	{
		return Bounded;
	}

	// Below the relief point the authored window is used unchanged, so an effect behaves exactly as its author tuned
	// it whenever the allowance is not actually under pressure. Stretching earlier would add latency to buy back an
	// allowance nothing is competing for.
	constexpr float ReliefPoint = 0.5f;
	const float Pressure = FMath::Clamp(static_cast<float>(RecentInvokes) / static_cast<float>(Limit), 0.0f, 1.0f);
	if (Pressure <= ReliefPoint)
	{
		return Bounded;
	}

	// Above it the window grows linearly to MaxCoalesceStretch at a fully spent allowance: fewer, larger calls buy the
	// allowance back, paid for in how late the effect lands.
	const float Stretch = 1.0f + (MaxCoalesceStretch - 1.0f) * ((Pressure - ReliefPoint) / (1.0f - ReliefPoint));
	return FMath::Clamp(AuthoredSeconds * Stretch, Bounded, MaxCoalesceWindowSeconds);
}

float UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(int32 AttemptIndex, TOptional<int64> ServerSuggestedMs)
{
	// Only the server knows when its own window reopens, so its suggestion always wins over the local guess. It is
	// clamped like any other value that arrives from the network: a hostile or mis-set suggestion must not be able to
	// park a caller for minutes, and a suggestion below the local floor must not turn into a tight retry loop.
	//
	// A suggestion of ZERO is a suggestion, not a missing one, and taking this branch is the point of the distinction:
	// the server is saying its window has already rolled, so the floor is the right wait rather than a backoff sized
	// for a window still closed. Only an unset value falls through. A negative one is treated as unset too, since a
	// duration cannot run backwards and the only honest reading of it is that the server named nothing usable.
	if (ServerSuggestedMs.IsSet() && ServerSuggestedMs.GetValue() >= 0)
	{
		// Divided in double, not float: the value is int64 off the wire and a hostile one near its limit loses whole
		// seconds to float's 24-bit mantissa before the clamp ever sees it.
		const double Seconds = static_cast<double>(ServerSuggestedMs.GetValue()) / 1000.0;
		return FMath::Clamp(static_cast<float>(FMath::Min(Seconds, static_cast<double>(MaxBudgetRetryDelaySeconds))),
			MinBudgetRetryDelaySeconds, MaxBudgetRetryDelaySeconds);
	}

	// The fallback. It is what runs on every refusal today, and it stays even once suggestions arrive, because a
	// refusal that carries no suggestion is always possible and still has to back off somehow.
	const int32 Doublings = FMath::Clamp(AttemptIndex, 0, 4);
	const float Delay = MinBudgetRetryDelaySeconds * static_cast<float>(1 << Doublings);
	return FMath::Clamp(Delay, MinBudgetRetryDelaySeconds, MaxBudgetRetryDelaySeconds);
}

bool UCrowdyGameModelSubsystem::IsBudgetRefusal(const FCrowdyInvokeResult& Result)
{
	if (Result.bSuccess)
	{
		return false;
	}
	// The transport check is INVERTED here rather than absent, and the inversion is the point. A rate-limit refusal is
	// thrown: it arrives as a GraphQL error with no gameModelInvoke object at all, so bTransportOk is false on every
	// genuine one. Requiring a clean transport did not narrow this gate, it closed it, and nothing could ever retry.
	//
	// Requiring a FAILED one keeps the gate exactly as narrow as the contract. The server never reports this refusal
	// in band, so a result that carries the rate-limit attribution AND a live gameModelInvoke object contradicts the
	// contract, and the safe reading of a contradiction is the one that does not repeat a call. It matters because
	// the in-band channel means the function RAN, which is the one situation where repeating could apply a write
	// twice; the argument below holds only for the thrown channel and must not be stretched to cover both.
	if (Result.bTransportOk)
	{
		return false;
	}
	// The attribution is what makes a failed transport safe to act on, and it is stronger evidence than the flag it
	// replaced. Blame and FaultCode are populated only from a GraphQL errors[] entry the server authored: a dropped
	// connection, a timeout, or a non-2xx carrying no error body yields no entry, so both stay empty and no such
	// failure can reach the pair below.
	//
	// That is what keeps the retry safe: this refusal is decided at the server's gate before the function runs, so
	// the refused call committed nothing and repeating it cannot apply a write twice. bRetryable stays unconsulted;
	// it is set for a platform fault, which is a different claim from "your allowance is spent".
	return Result.Blame == ECrowdyPlayerFaultBlame::Budget
		&& Result.FaultCode.Equals(TEXT("RATE_LIMITED"), ESearchCase::IgnoreCase);
}

bool UCrowdyGameModelSubsystem::TryScheduleBudgetRetry(const TSharedRef<FCrowdyInvokeRequest>& Resolved,
	int32 AttemptIndex, const FCrowdyInvokeResult& Result, const TFunction<void(FCrowdyInvokeResult)>& OnDone,
	const FCrowdyInvokeBindingGuard& Guard)
{
	if (bShuttingDown || AttemptIndex >= MaxBudgetRetries || !IsBudgetRefusal(Result))
	{
		return false;
	}

	// Already stale before the backoff even starts, which happens when the entity unregistered while the refused call
	// was in flight. Arming a timer for it would only reach the same answer later, so the refusal is reported now.
	if (!IsInvokeBindingStillValid(Guard))
	{
		return false;
	}

	// Bounds the number of backoffs alive at once. Past it the refusal is simply reported, which is the honest answer
	// and costs the server nothing more.
	constexpr int32 MaxPendingInvokeRetries = 64;
	if (PendingInvokeRetries.Num() >= MaxPendingInvokeRetries)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Read from THIS refusal, never from a remembered one. The server sends what remains of the current window, so a
	// second refusal inside the same window names a shorter wait than the first, and reusing the earlier number would
	// hold the caller past the moment the window actually reopened.
	const float Delay = ResolveBudgetRetryDelaySeconds(AttemptIndex, Result.RetryAfterMs);

	const uint64 RetryId = NextInvokeRetryId++;
	FCrowdyPendingInvokeRetry Waiting;
	Waiting.LastResult = Result;
	Waiting.OnDone = OnDone;
	Waiting.Guard = Guard;
	FCrowdyPendingInvokeRetry& Stored = PendingInvokeRetries.Add(RetryId, MoveTemp(Waiting));

	const int32 NextAttempt = AttemptIndex + 1;
	World->GetTimerManager().SetTimer(Stored.Timer,
		FTimerDelegate::CreateWeakLambda(this, [this, RetryId, Resolved, NextAttempt]()
		{
			// Claim the record before dispatching. Whoever removes it owns the completion, so a teardown racing this
			// timer can never fail a caller the retry is about to serve, or serve one the teardown already failed.
			FCrowdyPendingInvokeRetry Claimed;
			if (!PendingInvokeRetries.RemoveAndCopyValue(RetryId, Claimed))
			{
				return;
			}
			// The binding is re-read HERE, against the moment the call is about to go out, because that is the only
			// reading that can be acted on. The request pins a container id, and an entity that unbound and bound
			// again during the backoff resolves to the same row as a different life, so re-sending would land this
			// call's writes (a whole coalesce window's summed magnitude, when it came from the coalescer) on whoever
			// holds that row now. The refused outcome is reported instead: the call did not happen, which is true.
			if (!IsInvokeBindingStillValid(Claimed.Guard))
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] '%s' was not retried: the target's container binding changed during the backoff."),
					*Resolved->FunctionName);
				if (Claimed.OnDone)
				{
					Claimed.OnDone(MoveTemp(Claimed.LastResult));
				}
				return;
			}
			DispatchInvokeAttempt(Resolved, NextAttempt, MoveTemp(Claimed.OnDone), Claimed.Guard);
		}),
		Delay, false);

	UE_LOG(LogCrowdyGameModel, Warning,
		TEXT("[GameModel] '%s' was refused for exceeding the per-player invoke allowance; retrying in %.1fs (attempt %d of %d). Sustained calls at this rate need coalescing."),
		*Resolved->FunctionName, Delay, NextAttempt, MaxBudgetRetries);
	return true;
}

namespace
{
	// The value of a JSON number field, refusing every other JSON type. A numeric-looking STRING is refused too: the
	// marshaller emits a string only for a parameter typed string or container_ref, neither of which is summable, so
	// accepting one would merge applies the author never made numeric.
	bool CrowdyCoalesceReadNumber(const TSharedPtr<FJsonObject>& Params, const FString& Name, double& OutValue)
	{
		if (!Params.IsValid() || Name.IsEmpty())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(Name);
		if (!Field.IsValid() || Field->Type != EJson::Number)
		{
			return false;
		}
		OutValue = Field->AsNumber();
		return FMath::IsFinite(OutValue);
	}

	// The largest magnitude a summed value may reach. Past 2^53 a double no longer represents whole numbers exactly,
	// so a sum beyond it would arrive as a number nobody authored. Clamping is the safe end of a runaway sum: the
	// server applies a bounded (if enormous) write instead of an undefined one.
	constexpr double CrowdyCoalesceMaxAccumulated = 9007199254740992.0;

	// How many applies one window may hold before it closes early. It bounds the waiter list without ever discarding
	// a merged value, since the apply that hit the bound is merged first and the window closes after.
	constexpr int32 CrowdyCoalesceMaxWaiters = 512;

	// Round a magnitude declared as an int to a whole number, the same way at every point one is handled. Half-up, so
	// it matches the curve sampler in the effect marshaller rather than inventing a second rounding rule.
	double CrowdyRoundIntegerMagnitude(double Value)
	{
		return FMath::FloorToDouble(Value + 0.5);
	}
}

bool UCrowdyGameModelSubsystem::IsCoalescingAvailable() const
{
	return !bShuttingDown && GetWorld() != nullptr;
}

void UCrowdyGameModelSubsystem::EnqueueCoalescedInvoke(const FCrowdyCoalesceRequest& Request,
	TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	// Every path out of here completes OnDone exactly once, eventually. There is no path that queues a caller and
	// forgets it: an apply that cannot be merged is dispatched on the spot, and a window that is open is either
	// flushed by its timer, closed early when it fills, or failed by the teardown drain.
	//
	// The session and the entity's container are resolved HERE and used for both the key and the dispatch. Resolving
	// them later, when the window closes, is what let a whole window's summed magnitude land in whichever session was
	// active (or on whichever container the entity was bound to) by then rather than the one the applies were authored
	// against.
	const FString ResolvedSession = ResolveSessionId(Request.SessionId, ActiveSessionId);

	FString TargetContainerId = Request.ContainerId;
	uint32 BindEpoch = 0;
	if (Request.bEntityBound)
	{
		if (!TryGetContainerId(Request.SelfNetID, TargetContainerId))
		{
			// Nothing is bound, so there is no target to key a window on. The direct route reports that as the failure
			// it is instead of parking the apply in a window that could only ever resolve to some later binding.
			DispatchUncoalescedInvoke(Request, ResolvedSession, MoveTemp(OnDone));
			return;
		}
		if (const uint32* Found = BindEpochByNetID.Find(Request.SelfNetID))
		{
			BindEpoch = *Found;
		}
	}

	double IncomingValue = 0.0;
	const bool bHasNumericMagnitude = CrowdyCoalesceReadNumber(Request.Params, Request.AccumulateParam, IncomingValue);

	// An int magnitude is rounded per APPLY, before it is either summed or sent, and never only on the sum. Rounding
	// only the sum made the delivered total depend on how fast the applies arrived: three applies of 12.5 sent one by
	// one deliver 13 each, but summed and rounded once they deliver 38. Rounding here is what makes a merged window
	// deliver exactly what the same applies would have delivered unmerged.
	if (bHasNumericMagnitude && Request.bAccumulateIsInteger)
	{
		IncomingValue = CrowdyRoundIntegerMagnitude(IncomingValue);
		Request.Params->SetNumberField(Request.AccumulateParam, IncomingValue);
	}

	const bool bMergeable = IsCoalescingAvailable() && Request.WindowSeconds > 0.0f && bHasNumericMagnitude;
	if (!bMergeable)
	{
		DispatchUncoalescedInvoke(Request, ResolvedSession, MoveTemp(OnDone));
		return;
	}

	FCrowdyCoalesceKey Key;
	Key.bEntityBound = Request.bEntityBound;
	Key.SelfNetID = Request.bEntityBound ? Request.SelfNetID : FGuid();
	Key.BindEpoch = BindEpoch;
	Key.ContainerIdHash = CrowdyCoalesce::HashString(TargetContainerId);
	Key.FunctionHash = CrowdyCoalesce::HashString(Request.FunctionName);
	Key.SessionHash = CrowdyCoalesce::HashString(ResolvedSession);
	Key.AccumulateHash = CrowdyCoalesce::HashString(Request.AccumulateParam);
	Key.Discriminator = Request.MergeDiscriminator;

	if (FCrowdyCoalescedInvoke* Open = PendingCoalesced.Find(Key))
	{
		Open->AccumulatedValue += IncomingValue;
		++Open->MergedCount;
		Open->Waiters.Add(MoveTemp(OnDone));

		// The apply that hit the bound is already merged, so closing now keeps the sum exact and only shortens the
		// window. Growing instead would let one runaway caller hold an unbounded list of waiters.
		const bool bFull = Open->Waiters.Num() >= CrowdyCoalesceMaxWaiters;
		if (bFull)
		{
			FlushCoalescedInvoke(Key);
		}
		return;
	}

	// At capacity, close the oldest window early to make room. It is the one closest to flushing anyway, so it loses
	// the least of its remaining merge time, and the slot it frees keeps the new target's repeated applies merging
	// instead of each becoming its own call. See MaxOpenCoalesceWindows for why the bound is also a bound on how many
	// distinct targets one client can be mid-effect on.
	if (PendingCoalesced.Num() >= MaxOpenCoalesceWindows)
	{
		FlushOldestCoalesceWindow();
	}
	if (PendingCoalesced.Num() >= MaxOpenCoalesceWindows)
	{
		// The flush above ran a waiter's completion, which applied again and took the slot back. Send this one on its
		// own rather than recursing: more expensive, never dropped, and it always terminates.
		DispatchUncoalescedInvoke(Request, ResolvedSession, MoveTemp(OnDone));
		return;
	}

	FCrowdyCoalescedInvoke Pending;
	Pending.bEntityBound = Request.bEntityBound;
	Pending.SelfNetID = Request.SelfNetID;
	Pending.ContainerId = TargetContainerId;
	Pending.BindEpoch = BindEpoch;
	Pending.OpenSequence = NextCoalesceOpenSequence++;
	Pending.FunctionName = Request.FunctionName;
	Pending.SessionId = ResolvedSession;
	Pending.AccumulateParam = Request.AccumulateParam;
	Pending.bAccumulateIsInteger = Request.bAccumulateIsInteger;
	Pending.AccumulatedValue = IncomingValue;
	Pending.MergedCount = 1;
	Pending.Waiters.Add(MoveTemp(OnDone));

	// The parameter object is taken over, not copied: the summed value is written into it when the window closes.
	// The marshaller builds a fresh one per apply, so nothing else is looking at it.
	Pending.Params = Request.Params;

	// The window is measured from THIS apply and a later merge never pushes it back. One that slid forward on every
	// merge would never close under sustained fire, which is precisely the case coalescing exists to serve.
	const float Window = StretchCoalesceWindowSeconds(Request.WindowSeconds, GetRecentInvokeCount(),
		InvokeBudgetLimitPerWindow);

	FCrowdyCoalescedInvoke& Stored = PendingCoalesced.Add(Key, MoveTemp(Pending));
	GetWorld()->GetTimerManager().SetTimer(Stored.Timer,
		FTimerDelegate::CreateWeakLambda(this, [this, Key]() { FlushCoalescedInvoke(Key); }),
		Window, false);
}

void UCrowdyGameModelSubsystem::FlushOldestCoalesceWindow()
{
	// The key is copied rather than pointed at, because the flush below removes the very entry it would point into.
	FCrowdyCoalesceKey OldestKey;
	uint64 OldestSequence = 0;
	bool bFound = false;
	for (const TPair<FCrowdyCoalesceKey, FCrowdyCoalescedInvoke>& Entry : PendingCoalesced)
	{
		if (!bFound || Entry.Value.OpenSequence < OldestSequence)
		{
			OldestKey = Entry.Key;
			OldestSequence = Entry.Value.OpenSequence;
			bFound = true;
		}
	}
	if (!bFound)
	{
		return;
	}
	FlushCoalescedInvoke(OldestKey);
}

void UCrowdyGameModelSubsystem::FlushCoalescedInvoke(const FCrowdyCoalesceKey& Key)
{
	FCrowdyCoalescedInvoke* Found = PendingCoalesced.Find(Key);
	if (!Found)
	{
		return;
	}

	// Taken out BEFORE anything is dispatched. An invoke can complete synchronously (a missing API context fails on
	// the spot) and a waiter's own handler may apply the same effect again; with the record already gone, that
	// re-entrant apply opens a fresh window instead of joining one that is mid-flush and about to be discarded.
	FCrowdyCoalescedInvoke Pending = MoveTemp(*Found);
	PendingCoalesced.Remove(Key);

	if (UWorld* World = GetWorld())
	{
		// Redundant when this is the timer's own callback, and required when the window was closed early by a full
		// waiter list, so the timer cannot fire again on a key nothing holds.
		World->GetTimerManager().ClearTimer(Pending.Timer);
	}
	DispatchCoalescedInvoke(MoveTemp(Pending));
}

void UCrowdyGameModelSubsystem::DispatchCoalescedInvoke(FCrowdyCoalescedInvoke&& Pending)
{
	// One invoke, one outcome, one copy each. Every caller merged into this window hears back from the single call
	// below, so a queued latent node always reaches one of its pins and unroots itself. A null entry is a
	// fire-and-forget caller that asked for no outcome, and is skipped rather than counted as a missing waiter.
	auto FanOut = [Waiters = MoveTemp(Pending.Waiters)](FCrowdyInvokeResult Result)
	{
		for (const TFunction<void(FCrowdyInvokeResult)>& Waiter : Waiters)
		{
			if (Waiter)
			{
				Waiter(Result);
			}
		}
	};

	if (!Pending.Params.IsValid())
	{
		FCrowdyInvokeResult Failed;
		Failed.ErrorMessage = TEXT("coalesced effect apply lost its parameters and was not sent");
		SetLastModelError(Failed.ErrorMessage);
		FanOut(Failed);
		return;
	}

	// The binding this window was opened against has to still be the one in place. An entity that unregistered while
	// the window was open no longer has a target at all, and one that unbound and bound again is a different life
	// sharing the same deterministic container row - the summed magnitude of the previous life must not land on it.
	// Either way the whole merged sum is being dropped, so it is reported to every waiter and logged rather than
	// silently discarded: several applies' worth of gameplay effect did not happen.
	if (Pending.bEntityBound)
	{
		const uint32* CurrentEpoch = BindEpochByNetID.Find(Pending.SelfNetID);
		if (!CurrentEpoch || *CurrentEpoch != Pending.BindEpoch)
		{
			FCrowdyInvokeResult Failed;
			Failed.ErrorMessage = FString::Printf(
				TEXT("'%s' was not sent: the target's container binding changed while %d merged applies were waiting"),
				*Pending.FunctionName, Pending.MergedCount);
			SetLastModelError(Failed.ErrorMessage);
			UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] %s"), *Failed.ErrorMessage);
			FanOut(Failed);
			return;
		}
	}

	// Write the summed magnitude over the first apply's value. Everything else in Params is already what the whole
	// window agreed on, because a differing value would have keyed a different window.
	double Summed = Pending.AccumulatedValue;
	if (Pending.bAccumulateIsInteger)
	{
		// Each merged value was already rounded on the way in, so this only absorbs the drift of summing doubles; it
		// is not where an int magnitude becomes whole.
		Summed = CrowdyRoundIntegerMagnitude(Summed);
	}
	Summed = FMath::Clamp(Summed, -CrowdyCoalesceMaxAccumulated, CrowdyCoalesceMaxAccumulated);
	Pending.Params->SetNumberField(Pending.AccumulateParam, Summed);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] merged %d applies of '%s' into one invoke (%s=%g)"),
		Pending.MergedCount, *Pending.FunctionName, *Pending.AccumulateParam, Summed);

	// The pinned container id is dispatched to directly, rather than resolved again from the entity: the epoch check
	// above proved the binding has not moved, and re-resolving would reintroduce the very drift it guards against.
	if (Pending.bEntityBound)
	{
		InvokeAndApplyResolved(Pending.SelfNetID, Pending.FunctionName, Pending.Params, Pending.SessionId,
			MoveTemp(FanOut));
		return;
	}
	InvokeOnContainerResolved(Pending.ContainerId, Pending.FunctionName, Pending.Params, Pending.SessionId,
		MoveTemp(FanOut));
}

void UCrowdyGameModelSubsystem::DispatchUncoalescedInvoke(const FCrowdyCoalesceRequest& Request,
	const FString& ResolvedSessionId, TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	// The already-resolved session rides through, so an apply that could not be merged is sent against exactly the
	// session an apply that could have been merged would have been.
	if (Request.bEntityBound)
	{
		InvokeAndApplyResolved(Request.SelfNetID, Request.FunctionName, Request.Params, ResolvedSessionId,
			MoveTemp(OnDone));
		return;
	}
	InvokeOnContainerResolved(Request.ContainerId, Request.FunctionName, Request.Params, ResolvedSessionId,
		MoveTemp(OnDone));
}

void UCrowdyGameModelSubsystem::FailPendingCoalesceWindows()
{
	if (PendingCoalesced.IsEmpty())
	{
		return;
	}

	// Moved out first, so a waiter whose handler re-enters cannot be drained twice or land in the map this loop is
	// walking. The teardown latch is already set, so any such re-entrant apply is dispatched on its own instead.
	TMap<FCrowdyCoalesceKey, FCrowdyCoalescedInvoke> Draining = MoveTemp(PendingCoalesced);

	UWorld* World = GetWorld();
	for (TPair<FCrowdyCoalesceKey, FCrowdyCoalescedInvoke>& Entry : Draining)
	{
		if (World)
		{
			World->GetTimerManager().ClearTimer(Entry.Value.Timer);
		}

		// A plain failure with no fault attribution, which is exactly what it is: the merged invoke was never sent,
		// so nothing committed and nothing about it is retryable.
		FCrowdyInvokeResult Failed;
		Failed.ErrorMessage = TEXT("the world was torn down before the coalesced effect apply was sent");
		for (const TFunction<void(FCrowdyInvokeResult)>& Waiter : Entry.Value.Waiters)
		{
			if (Waiter)
			{
				Waiter(Failed);
			}
		}
	}
}

void UCrowdyGameModelSubsystem::FailPendingInvokeRetries()
{
	if (PendingInvokeRetries.IsEmpty())
	{
		return;
	}

	TMap<uint64, FCrowdyPendingInvokeRetry> Draining = MoveTemp(PendingInvokeRetries);

	UWorld* World = GetWorld();
	for (TPair<uint64, FCrowdyPendingInvokeRetry>& Entry : Draining)
	{
		if (World)
		{
			World->GetTimerManager().ClearTimer(Entry.Value.Timer);
		}
		// The retry never ran, so the caller is told about the refusal that would have caused it rather than a
		// second, invented failure it could not act on. The TIMING is stripped first: the wait was what remained of a
		// window when the refusal arrived, it has been draining ever since, and handing it to a caller now would
		// invite it to wait out a number that was only ever true seconds ago. The attribution goes with it, because
		// what actually happened here is a teardown, and leaving the rate-limit fault standing would let a caller
		// read this as a refusal worth repeating when there is no longer a world to repeat it in.
		FCrowdyInvokeResult Failed = MoveTemp(Entry.Value.LastResult);
		Failed.RetryAfterMs.Reset();
		Failed.Blame = ECrowdyPlayerFaultBlame::Unknown;
		Failed.FaultCode.Reset();
		Failed.bRetryable = false;
		Failed.ErrorMessage = TEXT("the world was torn down while a refused invoke was waiting to be retried");
		if (Entry.Value.OnDone)
		{
			Entry.Value.OnDone(MoveTemp(Failed));
		}
	}
}

#if !UE_BUILD_SHIPPING
namespace
{
	// Every string logged by the feed below comes from the server. Newlines would let it forge log lines that look
	// like they came from somewhere else, and an unbounded one would let it write the log to disk, so both are cut.
	FString SanitizeForLog(const FString& Value)
	{
		constexpr int32 MaxLoggedChars = 512;

		FString Out;
		Out.Reserve(FMath::Min(Value.Len(), MaxLoggedChars));
		for (const TCHAR Character : Value)
		{
			if (Out.Len() >= MaxLoggedChars)
			{
				Out += TEXT("...");
				break;
			}
			Out.AppendChar(Character < TEXT(' ') || Character == TEXT('\x7f') ? TEXT('?') : Character);
		}
		return Out;
	}
}

void UCrowdyGameModelSubsystem::DebugWatchContainerChanges(const FString& TypeName)
{
	DebugStopWatchingContainerChanges();

	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		return;
	}

	// A WebSocket subscription is not a Game Model call and does not spend the per-player allowance, so it resolves
	// the client without charging one.
	FCrowdyCppClient* Client = EnsureCppClientUnmetered(Endpoint, Token);
	if (!Client)
	{
		return;
	}

	// appId is BigInt, so it rides as a string like every other Game API call.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));
	if (!TypeName.IsEmpty())
	{
		Variables->SetStringField(TEXT("typeName"), TypeName);
	}

	FCrowdyCppSubscriptionCallbacks Callbacks;
	Callbacks.OnNext = [](TSharedPtr<FJsonObject> Data)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (!Data.IsValid() || !Data->TryGetObjectField(TEXT("gameModelContainerChanged"), Row))
		{
			UE_LOG(LogCrowdyGameModel, Log, TEXT("[GameModel] Container-change feed: a notification arrived with no readable row."));
			return;
		}

		FString ContainerId;
		FString ChangedType;
		FString FunctionName;
		(*Row)->TryGetStringField(TEXT("containerId"), ContainerId);
		(*Row)->TryGetStringField(TEXT("typeName"), ChangedType);
		(*Row)->TryGetStringField(TEXT("functionName"), FunctionName);

		UE_LOG(LogCrowdyGameModel, Log,
			TEXT("[GameModel] Container-change feed: container=%s type=%s function=%s"),
			*SanitizeForLog(ContainerId), *SanitizeForLog(ChangedType), *SanitizeForLog(FunctionName));
	};
	Callbacks.OnError = [](const FString& Message, bool bTerminal)
	{
		UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] Container-change feed %s: %s"),
			bTerminal ? TEXT("ended") : TEXT("reported a recoverable failure"), *SanitizeForLog(Message));
	};
	Callbacks.OnComplete = []()
	{
		UE_LOG(LogCrowdyGameModel, Log, TEXT("[GameModel] Container-change feed: the server ended it."));
	};

	const FCrowdyCppSubscriptionHandle Handle = Client->SubscribeOperation(
		ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerChanged"), Variables, MoveTemp(Callbacks));
	DebugContainerWatchId = Handle.Id;

	UE_LOG(LogCrowdyGameModel, Log, TEXT("[GameModel] Container-change feed opened for appId=%lld type=%s"),
		AppId, TypeName.IsEmpty() ? TEXT("<all>") : *TypeName);
}

void UCrowdyGameModelSubsystem::DebugStopWatchingContainerChanges()
{
	if (DebugContainerWatchId == 0)
	{
		return;
	}

	// Resolved through the client host rather than kept as a raw pointer, because the client outlives this world and
	// may have been rebuilt since the feed opened; a rebuild already ended the feed on its own. The id is kept until
	// the close actually goes through, so a context that has stopped resolving (a sign-out) leaves something to
	// retry rather than a feed nobody holds a handle to any more.
	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		return;
	}

	FCrowdyCppClient* Client = EnsureCppClientUnmetered(Endpoint, Token);
	if (!Client)
	{
		return;
	}

	FCrowdyCppSubscriptionHandle Handle;
	Handle.Id = DebugContainerWatchId;
	DebugContainerWatchId = 0;
	Client->Unsubscribe(Handle);
	UE_LOG(LogCrowdyGameModel, Log, TEXT("[GameModel] Container-change feed closed."));
}

namespace
{
	// Resolves the Game Model subsystem for whichever world the console command was issued against.
	UCrowdyGameModelSubsystem* ResolveGameModelSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	}
}

static FAutoConsoleCommandWithWorldAndArgs GCrowdyWatchContainerChanges(
	TEXT("crowdy.gamemodel.watchcontainers"),
	TEXT("Open the server's container-change feed over a WebSocket and log every notification. Optional argument narrows it to one container type. Diagnostic only: nothing is re-pulled from it. Run with no argument again, or crowdy.gamemodel.unwatchcontainers, to close it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyGameModelSubsystem* Subsystem = ResolveGameModelSubsystem(World))
			{
				Subsystem->DebugWatchContainerChanges(Args.Num() > 0 ? Args[0] : FString());
			}
		}));

static FAutoConsoleCommandWithWorld GCrowdyUnwatchContainerChanges(
	TEXT("crowdy.gamemodel.unwatchcontainers"),
	TEXT("Close the container-change feed opened by crowdy.gamemodel.watchcontainers."),
	FConsoleCommandWithWorldDelegate::CreateLambda(
		[](UWorld* World)
		{
			if (UCrowdyGameModelSubsystem* Subsystem = ResolveGameModelSubsystem(World))
			{
				Subsystem->DebugStopWatchingContainerChanges();
			}
		}));
#endif

UCrowdyGameModelSubsystem* UCrowdyGameModelSubsystem::ResolveLive(
	const TWeakObjectPtr<UCrowdyGameModelSubsystem>& Weak)
{
	UCrowdyGameModelSubsystem* Self = Weak.Get();
	// The object can still resolve after Deinitialize has cleared every cache, so the live world session - not object
	// liveness alone - is what decides whether it is safe to write into.
	return (Self && Self->WorldSessionToken.IsValid()) ? Self : nullptr;
}

FCrowdyCppClient* UCrowdyGameModelSubsystem::EnsureCppClient(const FString& Endpoint, const FString& Token)
{
	FCrowdyCppClient* Client = EnsureCppClientUnmetered(Endpoint, Token);
	if (!Client)
	{
		// Nothing will be sent, so nothing is charged.
		return nullptr;
	}

	// Counted here, once, because every Game Model call this subsystem makes resolves its client through this
	// function immediately before sending. The server's allowance covers all of them, not just invokes, so a governor
	// fed only by invokes reads the allowance as far less spent than it is and never widens a merge window under the
	// pressure it exists to relieve. A retried invoke resolves the client again and is counted again, which is
	// correct: the server's gate counted the call it refused too.
	RecordInvokeAttempt(FApp::GetCurrentTime());
	return Client;
}

FCrowdyCppClient* UCrowdyGameModelSubsystem::EnsureCppClientUnmetered(const FString& Endpoint, const FString& Token)
{
	// The client belongs to the game instance, not to this world, so it survives level travel and one completion
	// pump serves every caller in the process. Because ownership lives above the world, a completion can outlive the
	// subsystem that asked for it: it always runs (so every caller is notified and no latent action is left hanging),
	// and anything it wants to write back into this subsystem goes through ResolveLiveSelf first.
	UCrowdyCppClientSubsystem* Host = UCrowdyCppClientSubsystem::Get(this);
	if (!Host)
	{
		UE_LOG(LogCrowdyGameModel, Warning,
			TEXT("[GameModel] No Game API client host is available on this game instance; the operation cannot proceed."));
		return nullptr;
	}

	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();

	FCrowdyCppClientConfig ClientConfig;
	ClientConfig.DiscoveryUrl = Settings ? Settings->GetDiscoveryUrl() : FString();
	// The app's own datacenter endpoint when one has been resolved. Before that the shared origin stands in: it is
	// answered everywhere, so a call made before discovery still reaches a server rather than nothing at all.
	ClientConfig.ApiUrl = Endpoint.IsEmpty() ? ClientConfig.DiscoveryUrl : Endpoint;

	// Refresh the bearer on every call so a re-mint or a sign-in after the client was built is picked up; the token
	// is not part of the client's identity, only the configured endpoints are.
	Host->SetGameToken(Token);

	FCrowdyCppClient* Client = Host->GetClient(ClientConfig);
	if (!Client)
	{
		UE_LOG(LogCrowdyGameModel, Error,
			TEXT("[GameModel] Could not construct the Game API client; the operation cannot proceed."));
	}
	return Client;
}

void UCrowdyGameModelSubsystem::PullContainerState(const FString& ContainerId,
	TFunction<void(bool, TSharedPtr<FJsonObject>)> OnDone)
{
	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone)
		{
			OnDone(false, nullptr);
		}
		return;
	}

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] PullContainerState container=%s appId=%lld"), *ContainerId, AppId);

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		if (OnDone)
		{
			OnDone(false, nullptr);
		}
		return;
	}

	// The callback lands from Poll() on the game thread; it captures only OnDone (never this), so it is safe even
	// if the subsystem outlives fewer ticks than the request.
	Client->ReadContainerState(AppId, ContainerId,
		[OnDone = MoveTemp(OnDone)](FCrowdyCppContainerStateResult Result)
		{
			if (OnDone)
			{
				OnDone(Result.bOk, Result.State);
			}
		});
}

void UCrowdyGameModelSubsystem::ListContainers(const FString& TypeName, const FString& SessionId,
	TFunction<void(bool, TArray<TSharedPtr<FJsonObject>>)> OnDone)
{
	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone)
		{
			OnDone(false, TArray<TSharedPtr<FJsonObject>>());
		}
		return;
	}

	// An empty Session Id falls back to the active (default) session so a list can be scoped without threading it.
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] ListContainers type=%s session=%s appId=%lld"), *TypeName,
		ResolvedSession.IsEmpty() ? TEXT("<app-global>") : *ResolvedSession, AppId);

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		if (OnDone)
		{
			OnDone(false, TArray<TSharedPtr<FJsonObject>>());
		}
		return;
	}

	// The callback lands from Poll() on the game thread; it captures only OnDone (never this).
	Client->ListContainers(AppId, TypeName, ResolvedSession,
		[OnDone = MoveTemp(OnDone)](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
		{
			if (OnDone)
			{
				OnDone(bOk, MoveTemp(Containers));
			}
		});
}

void UCrowdyGameModelSubsystem::BindEntityContainer(const FGuid& NetID, const FString& ContainerId)
{
	AddContainerBinding(NetID, ContainerId);
}

void UCrowdyGameModelSubsystem::AddContainerBinding(const FGuid& NetID, const FString& ContainerId)
{
	// Refused at the writer, so no reader downstream has to answer for an empty container key.
	if (!NetID.IsValid() || ContainerId.IsEmpty())
	{
		return;
	}

	// Re-binding the SAME container keeps the epoch, so an idempotent bind does not invalidate merge windows that are
	// legitimately still open against it. Anything else is a new binding and gets a fresh epoch, which is what makes a
	// window opened against the previous one refuse to flush onto this one.
	if (const FString* Existing = NetIDToContainerId.Find(NetID))
	{
		if (*Existing == ContainerId && BindEpochByNetID.Contains(NetID))
		{
			return;
		}
		RemoveContainerBinding(NetID);
	}

	TArray<FGuid>& Bound = ContainerIdToNetIDs.FindOrAdd(ContainerId);
	if (Bound.Num() > 0)
	{
		// Reported where the ambiguity is created, not where a notification resolves it: that path runs per delivery.
		UE_LOG(LogCrowdyGameModel, Warning,
			TEXT("[GameModel] container %s is now bound by entity %s as well as %s; a container row is meant to have one local holder, so model changes and signals for it reach the newest binding only."),
			*ContainerId, *NetID.ToString(), *Bound.Last().ToString());
	}

	NetIDToContainerId.Add(NetID, ContainerId);
	Bound.AddUnique(NetID);
	BindEpochByNetID.Add(NetID, NextBindEpoch++);
}

void UCrowdyGameModelSubsystem::RemoveContainerBinding(const FGuid& NetID)
{
	FString ContainerId;
	if (!NetIDToContainerId.RemoveAndCopyValue(NetID, ContainerId))
	{
		// Never bound, so there is no epoch either. Dropping the epoch anyway would be harmless but says something
		// untrue about what happened.
		return;
	}

	if (TArray<FGuid>* Bound = ContainerIdToNetIDs.Find(ContainerId))
	{
		// Order-preserving on purpose: FindNetIDForContainer reads this row oldest-first.
		Bound->Remove(NetID);
		if (Bound->Num() == 0)
		{
			ContainerIdToNetIDs.Remove(ContainerId);
		}
	}
	// The epoch is dropped rather than kept, so a window still holding it can never match again. Epochs are never
	// reused, so a later re-bind cannot resurrect this one.
	BindEpochByNetID.Remove(NetID);
}

FGuid UCrowdyGameModelSubsystem::FindNetIDForContainer(const FString& ContainerId) const
{
	const TArray<FGuid>* Bound = ContainerIdToNetIDs.Find(ContainerId);
	if (!Bound || Bound->Num() == 0)
	{
		return FGuid();
	}
	// Newest last, by bind order: the newest holder that still resolves answers, and the oldest when none does.
	for (int32 Index = Bound->Num() - 1; Index > 0; --Index)
	{
		if (ResolveEntityParticipant((*Bound)[Index]))
		{
			return (*Bound)[Index];
		}
	}
	return (*Bound)[0];
}

bool UCrowdyGameModelSubsystem::TryGetContainerId(const FGuid& NetID, FString& OutContainerId) const
{
	if (const FString* Found = NetIDToContainerId.Find(NetID))
	{
		OutContainerId = *Found;
		return true;
	}
	return false;
}

bool UCrowdyGameModelSubsystem::TryGetCachedOwnerUserId(const FGuid& NetID, int64& OutUserId) const
{
	if (const int64* Found = NetIDToOwnerUserId.Find(NetID))
	{
		OutUserId = *Found;
		return true;
	}
	OutUserId = 0;
	return false;
}

bool UCrowdyGameModelSubsystem::TryGetCachedValueJson(const FGuid& NetID, const FName Key, FString& OutJson) const
{
	if (const TMap<FName, FString>* Cache = ContainerCache.Find(NetID))
	{
		if (const FString* Found = Cache->Find(Key))
		{
			OutJson = *Found;
			return true;
		}
	}
	return false;
}

void UCrowdyGameModelSubsystem::ResolveOrCreateContainer(const FGuid& NetID, const FString& TypeName,
	const FString& SessionId, TFunction<void(bool, const FString&)> OnDone)
{
	if (!NetID.IsValid() || TypeName.IsEmpty())
	{
		if (OnDone) { OnDone(false, FString()); }
		return;
	}

	// An empty Session Id falls back to the active (default) session, resolved once here so the ensure and the
	// read-by-key below agree on the same scope (the binding key is unique per app + type + session).
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	// (1) cache hit.
	FString Existing;
	if (TryGetContainerId(NetID, Existing))
	{
		PendingModelEntities.Remove(NetID);
		if (OnDone) { OnDone(true, Existing); }
		return;
	}

	// One resolve per entity at a time a re-entrant registration/notification must not double-ensure.
	if (ResolveInFlight.Contains(NetID))
	{
		if (OnDone) { OnDone(false, FString()); }
		return;
	}

	// Past the two guards that answer without a round trip, so this counts the resolves that go on to spend the
	// allowance rather than every call that asked.
	++ContainerResolveStartCount;

	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		// Not authenticated yet: leave it pending so RetryPendingModelEntities re-drives it once a token exists.
		if (OnDone) { OnDone(false, FString()); }
		return;
	}

	// The binding key is the entity's NetID digest: deterministic and identical on every client for one entity, so
	// concurrent ensures converge on one server row. For a shared entity it is world identity (same on all clients);
	// for a per-player entity the NetID already folds the owner, so it is that player's key.
	const FString BindingKey = FCrowdyModelIdentity::NetIDToContainerKey(NetID);
	const bool bAuthoritative = IsAuthoritativeToCreate(NetID);

	// For a per-player (locally-owned, not shared) entity we should be the owner: the binding key folds our own
	// owner id. Capture our expected owner so the ensure bind can refuse a row the server resolved by key that is
	// owned by a DIFFERENT user - a squatted, client-supplied binding key (defense in depth until the server governs
	// who may ensure a key). A shared/Host-owned entity legitimately has a null or host owner, so it is exempt.
	int64 ExpectedOwnerId = 0;
	if (bAuthoritative && !IsHostOwnedEntity(NetID))
	{
		FCrowdyModelIdentityProvider Identity(GetWorld());
		int64 LocalUserId = 0;
		if (Identity.TryGetLocalUserId(LocalUserId))
		{
			ExpectedOwnerId = LocalUserId;
		}
	}
	ResolveInFlight.Add(NetID);

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);

	// Binds the resolved row (an ensure or a read), guarded against an entity that unregistered during the
	// round-trip, and completes. Shared by both branches so the liveness/owner-cache handling is identical.
	auto BindResolved = [WeakThis, NetID](const FString& ContainerId, int64 OwnerUserId,
		TFunction<void(bool, const FString&)>& Done)
	{
		UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
		if (!S)
		{
			if (Done) { Done(false, FString()); }
			return;
		}
		S->ResolveInFlight.Remove(NetID);
		// The entity may have unregistered during the round-trip; do not resurrect a binding for a destroyed entity
		// (it would leak a map entry and waste every future notification's pull on it).
		if (ContainerId.IsEmpty() || !S->ResolveEntityParticipant(NetID))
		{
			if (Done) { Done(false, FString()); }
			return;
		}
		S->BindEntityContainer(NetID, ContainerId);
		if (OwnerUserId > 0)
		{
			S->NetIDToOwnerUserId.Add(NetID, OwnerUserId);
		}
		S->PendingModelEntities.Remove(NetID);
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] resolved entity %s -> container %s"), *NetID.ToString(), *ContainerId);
		if (Done) { Done(true, ContainerId); }
	};

	if (bAuthoritative)
	{
		// Atomic get-or-create by key: concurrent ensures across clients converge on one row (no host election, no
		// client-side matching). ownerUserId is omitted so the server pins it to the caller (per-player) or null
		// (admin/shared types). A not-exists creation is authorized by the type's instantiableBy, so a client that
		// may not create a shared admin-type row fails here and stays pending until an authority (or a deploy tool)
		// ensures it.
		auto Complete = [WeakThis, NetID, ExpectedOwnerId, BindResolved, OnDone = MoveTemp(OnDone)]
			(bool bOk, FString ContainerId, int64 OwnerUserId, bool /*bCreated*/) mutable
		{
			// The in-flight guard below belongs to this world and is emptied on teardown, so a completion that no
			// longer resolves live has nothing to clear - it only has to tell the caller the resolve did not land.
			UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
			if (!S)
			{
				if (OnDone) { OnDone(false, FString()); }
				return;
			}
			if (!bOk || ContainerId.IsEmpty())
			{
				// Ensure failed (transport, or not authorized to create a not-yet-existing shared row). Leave it
				// pending: a later notification-driven retry rebinds once the row exists.
				S->ResolveInFlight.Remove(NetID);
				if (OnDone) { OnDone(false, FString()); }
				return;
			}
			// The server resolved our per-player key to a row owned by a DIFFERENT user: a squatted binding key.
			// Refuse rather than bind a foreign container (our authoritative writes would be policy-denied anyway).
			if (ExpectedOwnerId > 0 && OwnerUserId > 0 && OwnerUserId != ExpectedOwnerId)
			{
				S->ResolveInFlight.Remove(NetID);
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] entity %s ensured a container owned by a different user (%s, expected %s); refusing to bind a possibly-squatted row."),
					*NetID.ToString(), *LexToString(OwnerUserId), *LexToString(ExpectedOwnerId));
				if (OnDone) { OnDone(false, FString()); }
				return;
			}
			BindResolved(ContainerId, OwnerUserId, OnDone);
		};

		FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
		if (!Client)
		{
			// Report it as a failed ensure rather than returning silently: that path clears the in-flight guard, so
			// the entity stays pending and can resolve again instead of being blocked forever.
			Complete(false, FString(), 0, false);
			return;
		}

		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildEnsureContainerVariables(
			AppId, TypeName, BindingKey, TypeName, ResolvedSession, FString());
		Client->RunRuntimeOp(TEXT("GameModelEnsureContainer"), Vars,
			[WeakThis, TypeName, Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R) mutable
		{
			// An ensure against a type the app never declared used to SUCCEED and bind nothing, so the only trace
			// was the caller's generic "no container bound" line, which names a symptom and nothing about the cause.
			// The server refuses it now and says why; say so here rather than folding it into that same warning.
			if (CrowdyCppIsModelRefusalCode(R.ErrorCode))
			{
				if (UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis))
				{
					S->ReportModelRefusalOnce(R.ErrorCode, TypeName, R.ErrorMessage);
				}
			}

			FString OutContainerId;
			int64 OutOwnerUserId = 0;
			bool OutCreated = false;
			const bool bOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(WrapCppDataEnvelope(R.Data),
				R.bTransportOk, TArray<FString>(), OutContainerId, OutOwnerUserId, OutCreated);
			Complete(bOk, MoveTemp(OutContainerId), OutOwnerUserId, OutCreated);
		});
		return;
	}

	// A remote proxy of a per-player entity: it must NOT create the owner's row (an ensure would pin the owner to
	// this client). Read by key and bind only if the owner has already created it; otherwise leave it pending for a
	// notification-driven retry.
	auto Complete = [WeakThis, NetID, BindResolved, OnDone = MoveTemp(OnDone)]
		(bool bOk, bool bFound, FString ContainerId, int64 OwnerUserId) mutable
	{
		// Same as the ensure branch: the in-flight guard is world-scoped and already emptied on teardown, so a
		// completion that no longer resolves live just reports the failure to its caller.
		UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
		if (!S)
		{
			if (OnDone) { OnDone(false, FString()); }
			return;
		}
		if (!bOk || !bFound || ContainerId.IsEmpty())
		{
			S->ResolveInFlight.Remove(NetID);
			if (OnDone) { OnDone(false, FString()); }
			return;
		}
		BindResolved(ContainerId, OwnerUserId, OnDone);
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		// Report it as a failed read rather than returning silently: that path clears the in-flight guard, so the
		// proxy stays pending and can resolve again instead of being blocked forever.
		Complete(false, false, FString(), 0);
		return;
	}

	const TSharedPtr<FJsonObject> Vars =
		FCrowdyGameApiCodec::BuildReadContainerByKeyVariables(AppId, TypeName, ResolvedSession, BindingKey);
	// gameModelContainers is a filterable list query; the parser re-checks each row's bindingKey against
	// BindingKey so a server that ignored the filter can never bind the wrong player's container.
	Client->RunRuntimeOp(TEXT("GameModelContainers"), Vars,
		[Complete = MoveTemp(Complete), BindingKey](FCrowdyCppJsonResult R) mutable
	{
		bool OutFound = false;
		FString OutContainerId;
		int64 OutOwnerUserId = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(WrapCppDataEnvelope(R.Data),
			R.bTransportOk, TArray<FString>(), BindingKey, OutFound, OutContainerId, OutOwnerUserId);
		Complete(bOk, OutFound, MoveTemp(OutContainerId), OutOwnerUserId);
	});
}

void UCrowdyGameModelSubsystem::HandleEntityRegistered(const FGuid& NetID)
{
	if (!NetID.IsValid())
	{
		return;
	}

	// An entity only binds once the SDK is connected, so the service registry is guaranteed to exist by now: this is
	// the last-resort guarantee that the reception layer is registered before any server-native model-changed
	// notification for this container could arrive (idempotent - a no-op once registered).
	EnsureReceptionLayerRegistered();
	// Resolve the participant behind this NetID - an actor, or a non-actor UObject (a Host-owned subsystem). A
	// free/data container addressed only by id has no participant and re-pulls via HandleModelChangedByContainer.
	UObject* Participant = ResolveEntityParticipant(NetID);
	if (!Participant)
	{
		return;
	}

	// A registered actor may host CrowdyContainer component sub-participants (a reusable attributes component).
	// Enroll them BEFORE binding the actor's own class, so an actor that is not itself a container still gets its
	// components bound. Each enrollment re-enters this handler for the component and binds it (a component is not an
	// actor, so it never re-sweeps).
	if (AActor* Actor = Cast<AActor>(Participant))
	{
		RegisterComponentSubParticipants(Actor, NetID);
	}

	BindParticipantContainer(NetID, Participant);
}

bool UCrowdyGameModelSubsystem::ShouldPullOnBindForClass(const UClass* Class)
{
	FString TypeName;
	if (!FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName))
	{
		// Not a Game Model container type (and a null class lands here too), so there is no container setting to
		// consult and the pull happens, which is what every participant did before the setting existed.
		return true;
	}
	// A container type answers from its own marker. This also covers a Host-owned subsystem container, which used
	// to pull unconditionally because it had no placed instance to read a per-instance flag from; it is a tagged
	// container class like any other, so it now honours the setting.
	return UCrowdyBakedRegistry::ShouldPullModelOnStart(Class);
}

bool UCrowdyGameModelSubsystem::ShouldPullOnBind(const UObject* Participant)
{
	return ShouldPullOnBindForClass(Participant ? Participant->GetClass() : nullptr);
}

bool UCrowdyGameModelSubsystem::ShouldPullOnRetry(const UObject* Participant)
{
	// A null participant here means the entity died between the resolve request and its completion, not "a class
	// with no setting to consult" - skip the pull rather than falling into ShouldPullOnBind's default.
	if (!Participant)
	{
		return false;
	}
	return ShouldPullOnBind(Participant);
}

void UCrowdyGameModelSubsystem::WarnIfRecordedClassUnresolved(const FGuid& NetID, const UObject* Participant)
{
	if (bWarnedRecordedClassUnresolved)
	{
		return;
	}
	uint32 RecordedClassID = CROWDY_INVALID_CLASS_ID;
	if (!CrowdyEntityClassContainer::RecordNamesAnotherClass(ResolveEntitySubsystem(), NetID, Participant, RecordedClassID)
		|| CrowdyEntityClassContainer::ResolveRecordedClass(RecordedClassID))
	{
		return; // the participant is the entity, or the class it records is here and declares no container
	}

	// Said once, at the first entity it happens to. Otherwise the class an entity is drawn as is never loaded here,
	// its container declaration is unreadable, and every effect aimed at it is refused with nothing naming a cause.
	bWarnedRecordedClassUnresolved = true;
	UE_LOG(LogCrowdyGameModel, Warning,
		TEXT("[GameModel] entity %s is represented by '%s', which declares no Game Model container, and the class the entity records (id %u) is not loaded on this client, so its own declaration cannot be read and effects aimed at it are refused. Add that class to Preloaded Entity Classes (Project Settings, Plugins, Crowdy SDK) so it can be resolved here (logged once)."),
		*NetID.ToString(), *GetNameSafe(Participant->GetClass()), RecordedClassID);
}

bool UCrowdyGameModelSubsystem::ShouldPullOnRetryForEntity(const FGuid& NetID) const
{
	UObject* Participant = ResolveEntityParticipant(NetID);
	FString TypeName;
	const UClass* RecordedClass = CrowdyEntityClassContainer::ResolveRecordedContainerClass(
		ResolveEntitySubsystem(), NetID, Participant, TypeName);
	if (!RecordedClass)
	{
		return ShouldPullOnRetry(Participant);
	}
	// The stand-in representing this entity declares nothing, so the setting has to come from the container class
	// the entity records; asking the stand-in would hand every such entity the default whatever its type says.
	return ShouldPullOnBindForClass(RecordedClass);
}

void UCrowdyGameModelSubsystem::BindParticipantContainer(const FGuid& NetID, UObject* Participant)
{
	if (!NetID.IsValid() || !Participant)
	{
		return;
	}
	// What declares the container: the participant's own class when it declares one, otherwise the class the ENTITY
	// records. The second case is an entity represented on this machine by a stand-in rather than by its own actor.
	// Its owner runs the real actor, and that actor's class carries the declaration, so reading it here is what lets
	// the declaration be authored once, on the actor, instead of a second time for the stand-in.
	FString TypeName;
	const UClass* RecordedClass = CrowdyEntityClassContainer::ResolveRecordedContainerClass(
		ResolveEntitySubsystem(), NetID, Participant, TypeName);
	const UClass* Class = RecordedClass ? RecordedClass : Participant->GetClass();
	if (!RecordedClass && !CrowdyEntityClassContainer::TryGetContainerTypeName(Class, TypeName))
	{
		WarnIfRecordedClassUnresolved(NetID, Participant);
		return; // nothing declares a Game Model container (a plain actor that only hosts container components)
	}
	// A container with no Server Owned attributes still binds: signals, timers and automations are authored on
	// effect assets, not on the class, and every one of them needs this container's server id to reach it (an
	// unbound container has no id, so DispatchSignal finds no target and the signal is dropped silently). An
	// individual CrowdyModel property of an unsupported type is reported by DiscoverForClass, per property.
	UE_CLOG(CrowdyGameModelTrace::GameModel() && !FCrowdyAttributeRegistry::ClassHasModelAttributes(Class),
		LogCrowdyGameModel, Log,
		TEXT("[GameModel] container '%s' ('%s') declares no Server Owned attributes; binding it for its functions."),
		*Class->GetName(), *TypeName);

	// Already bound (an explicit BindEntityContainer, or a prior resolve)? Nothing to do.
	FString Bound;
	if (TryGetContainerId(NetID, Bound))
	{
		return;
	}

	if (RecordedClass)
	{
		// Recorded before the resolve and kept for the life of the entity: a class-derived binding reads an existing
		// row by key and must never ensure one, whatever the entity's role says now or comes to say later. The
		// recorded class is chosen by the entity's own owner, so it may name the row to read and nothing more.
		ClassDerivedBindings.Add(NetID);
		const UCrowdyContainerStandIn* StandIn = Cast<UCrowdyContainerStandIn>(Participant);
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] entity %s is represented here by '%s', which declares no container; binding '%s' read-only from %s '%s'%s."),
			*NetID.ToString(), *Participant->GetClass()->GetName(), *TypeName,
			StandIn ? TEXT("the component class") : TEXT("the class the entity records"), *RecordedClass->GetName(),
			StandIn ? *FString::Printf(TEXT(" (instance term '%s')"), *StandIn->InstanceTerm) : TEXT(""));
	}

	// Recorded here because this is the only place the declaring class is in hand. A holder of the entity is handed
	// values with a container id and nothing else, and one entity can bind several containers.
	ContainerTypeByNetID.Add(NetID, TypeName);

	// ResolveOrCreateContainer ensures (locally-owned or shared entity) or reads by key (a remote per-player proxy,
	// or any class-derived binding). Track it as pending so a not-yet-authed ensure or a proxy whose owner has not
	// created the row yet is re-driven by RetryPendingModelEntities on the next model-changed notification.
	PendingModelEntities.Add(NetID, TypeName);

	// Decided before the round-trip, from the class that declares the container, so the completion below does not
	// need to re-resolve it. This only gates the ONE-TIME initial pull below; a later model-changed notification for
	// this container still re-pulls unconditionally once it is bound.
	const bool bShouldPull = RecordedClass ? ShouldPullOnBindForClass(RecordedClass) : ShouldPullOnBind(Participant);

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	ResolveOrCreateContainer(NetID, TypeName, FString(),
		[WeakThis, NetID, bShouldPull](bool bOk, const FString& /*ContainerId*/)
	{
		// On first bind, pull once so the entity's live members reflect server truth and OnRep fires, unless the
		// container type turned the pull off.
		if (bOk && bShouldPull)
		{
			if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
			{
				Self->HandleModelChanged(NetID);
			}
		}
	});
}

void UCrowdyGameModelSubsystem::RegisterComponentSubParticipants(AActor* Anchor, const FGuid& AnchorNetID)
{
	if (!Anchor || !AnchorNetID.IsValid())
	{
		return;
	}

	// Copy into a local array so enrollment's re-entrant OnEntityRegistered can never perturb the iteration.
	TInlineComponentArray<UActorComponent*> Components(Anchor);
	for (UActorComponent* Component : Components)
	{
		EnrollComponentSubParticipant(Component, AnchorNetID);
	}
}

void UCrowdyGameModelSubsystem::EnrollComponentSubParticipant(UActorComponent* Component, const FGuid& AnchorNetID)
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!IsValid(Component) || !AnchorNetID.IsValid() || !Entities)
	{
		return;
	}

	const UClass* CompClass = Component->GetClass();
	FString TypeName;
	// Through the normalizing helper, which is also what the class-only derivation uses. Reading the tag off the
	// exact class here and off the generated class there would give the two a different set of containers for one
	// actor, so an observer would stand in for a container its owner never enrolled.
	if (!CrowdyEntityClassContainer::TryGetContainerTypeName(CompClass, TypeName))
	{
		return; // an ordinary component (the actor's own UCrowdyEntityComponent, movement, mesh, ...)
	}
	// Enrolled on the tag alone, for the same reason BindParticipantContainer binds on it: an attributes component
	// that carries only signals or timers is a real container, and refusing it here left the whole actor/component
	// hierarchy with no server id for those functions to reach.

	// Idempotent: an already-enrolled component (a repeat sweep, or a double EnrollModelComponent) is left as-is.
	if (Entities->FindEntityID(Component).IsValid())
	{
		return;
	}

	// An author-supplied binding key names a component whose object name is not cross-client stable. Empty falls
	// back to the object-name derivation in RegisterSubParticipant.
	FString BindingKey;
	if (CompClass->ImplementsInterface(UCrowdyBindingKeyProvider::StaticClass()))
	{
		BindingKey = ICrowdyBindingKeyProvider::Execute_GetCrowdyBindingKey(Component);
	}

	// A runtime-added component has an engine-generated name that differs per client, so the sub-participant id
	// derived from it diverges: a shared container forks, a remote proxy fails to bind the owner's. A binding key
	// fixes it, so warn only for a runtime component with NO key. Still enroll, so single-client dev is not broken.
	if (Component->CreationMethod == EComponentCreationMethod::Instance && BindingKey.IsEmpty())
	{
		UE_LOG(LogCrowdyGameModel, Warning,
			TEXT("[GameModel] component '%s' on '%s' was added at runtime with no Binding Key; its container id is not stable across clients. Implement ICrowdyBindingKeyProvider (Get Crowdy Binding Key) or place the component in the Blueprint so its Game Model container binds consistently."),
			*Component->GetName(), *GetNameSafe(Component->GetOwner()));
	}

	const FGuid SubNetID = Entities->RegisterSubParticipant(Component, AnchorNetID, BindingKey);
	if (!SubNetID.IsValid())
	{
		return; // invalid anchor or a dup-key collision (already logged by RegisterSubParticipant)
	}
	// RegisterSubParticipant broadcast OnEntityRegistered synchronously, which bound the component's container via
	// this handler already. Record the anchor -> sub link so the anchor's teardown unregisters it.
	SubParticipantsByAnchor.FindOrAdd(AnchorNetID).AddUnique(SubNetID);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] enrolled component '%s' on '%s' as sub-participant %s (container '%s'%s)"),
		*Component->GetName(), *GetNameSafe(Component->GetOwner()), *SubNetID.ToString(), *TypeName,
		BindingKey.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", key '%s'"), *BindingKey));
}

namespace
{
	void SetShouldRetry(bool* bOutShouldRetry)
	{
		if (bOutShouldRetry)
		{
			*bOutShouldRetry = true;
		}
	}
}

bool UCrowdyGameModelSubsystem::IsAnchorFullyStoodIn(const UCrowdyEntitySubsystem& Entities,
	const FGuid& AnchorNetID) const
{
	const TArray<FGuid>* Subs = SubParticipantsByAnchor.Find(AnchorNetID);
	if (!Subs || Subs->IsEmpty())
	{
		return false;
	}

	for (const FGuid& SubNetID : *Subs)
	{
		const FCrowdyEntityRecord* Record = Entities.FindRecord(SubNetID);
		if (!Record || !IsValid(Record->GetParticipant()))
		{
			return false; // one went away, so the derivation has to run and mint its replacement
		}
	}

	return true;
}

int32 UCrowdyGameModelSubsystem::EnrollDerivedComponentContainers(const FGuid& AnchorNetID, UObject* StandInOuter,
	bool* bOutShouldRetry)
{
	++DerivedEnrollmentRequestCount;
	if (bOutShouldRetry)
	{
		*bOutShouldRetry = false;
	}

	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities || !AnchorNetID.IsValid() || !IsValid(StandInOuter))
	{
		return 0;
	}

	const FCrowdyEntityRecord* Anchor = Entities->FindRecord(AnchorNetID);
	if (!Anchor)
	{
		SetShouldRetry(bOutShouldRetry);
		return 0;
	}

	// Answered before the class resolve below, which builds a path string and searches the global object hash.
	// This runs on every effect target read, so an anchor already stood in for must cost lookups and nothing more.
	if (IsAnchorFullyStoodIn(*Entities, AnchorNetID))
	{
		return 0;
	}

	// The class the ENTITY records, which is the actor class its owner runs. Its components are the ones the owner
	// enrolled, so they are the ones that have rows to read.
	const UClass* RecordedClass = CrowdyEntityClassContainer::ResolveRecordedClass(Anchor->ClassID);
	if (!RecordedClass)
	{
		SetShouldRetry(bOutShouldRetry);
		return 0;
	}

	TArray<FCrowdyDerivedComponentContainer> Derived;
	if (!CrowdyEntityClassContainer::GetDerivedComponentContainers(RecordedClass, Derived))
	{
		// The chain was unreadable, so an empty answer here means "not yet", not "declares none". Without this the
		// row that asked during a load is the one row that never gets component containers.
		SetShouldRetry(bOutShouldRetry);
		return 0;
	}

	if (Derived.IsEmpty())
	{
		return 0;
	}

	// The class an entity records is chosen by that entity's own owner, and each container it names costs this
	// client a permanently retrying resolve when no server row answers for it. A class naming an unreasonable
	// number of them is an authoring mistake locally and a cheap multiplier for a modified peer, so the count one
	// entity may spend is bounded here rather than by whatever the class happens to declare.
	if (Derived.Num() > MaxDerivedContainersPerEntity)
	{
		if (!bWarnedDerivedContainerCap)
		{
			bWarnedDerivedContainerCap = true;
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] '%s' declares %d CrowdyContainer components; only the first %d are stood in for on an entity drawn as a row, because each one costs a Game Model binding and a retry against a shared call allowance. Move the extra state onto fewer containers (logged once)."),
				*RecordedClass->GetName(), Derived.Num(), MaxDerivedContainersPerEntity);
		}
		Derived.SetNum(MaxDerivedContainersPerEntity);
	}

	int32 Enrolled = 0;
	for (const FCrowdyDerivedComponentContainer& Container : Derived)
	{
		const UClass* ComponentClass = Container.ComponentClass.Get();
		if (!ComponentClass)
		{
			continue; // the class went away between derivation and here
		}

		// Asked before minting, because a stand-in is a UObject and the registry refuses a second live participant
		// on one id: creating one for an id already held would allocate an object that can never be enrolled.
		const FGuid SubNetID = UCrowdyEntitySubsystem::DeriveSubParticipantID(AnchorNetID, ComponentClass,
			Container.InstanceTerm);
		if (!SubNetID.IsValid())
		{
			continue;
		}

		if (const FCrowdyEntityRecord* Existing = Entities->FindRecord(SubNetID))
		{
			if (IsValid(Existing->GetParticipant()))
			{
				continue; // already stood in for, by this call or an earlier one
			}
			// A record whose participant has gone answers this guard while resolving to nothing, so it would bar
			// its own replacement for the life of the anchor. Drop it and mint again.
			Entities->UnregisterEntity(SubNetID);
		}

		UCrowdyContainerStandIn* StandIn = NewObject<UCrowdyContainerStandIn>(StandInOuter);
		StandIn->RepresentedClass = const_cast<UClass*>(ComponentClass);
		StandIn->InstanceTerm = Container.InstanceTerm;

		// Registering broadcasts, which binds this container through HandleEntityRegistered.
		const FGuid Registered = Entities->RegisterSubParticipantAs(StandIn, AnchorNetID, ComponentClass,
			Container.InstanceTerm);
		if (!Registered.IsValid())
		{
			continue;
		}

		// Held before anything else can collect it. The registry's hold is weak and an Outer keeps nothing alive.
		ContainerStandIns.Add(Registered, StandIn);
		SubParticipantsByAnchor.FindOrAdd(AnchorNetID).AddUnique(Registered);
		++Enrolled;

		// A binding key read off a template answers for the class, and the interface exists so two instances can
		// answer differently. When they do, the owner minted a different id and this stand-in reads a row that does
		// not exist. Said once per world, because a crowd is one class and would otherwise say it per row.
		if (Container.bInstanceTermFromBindingKey && !bWarnedStandInBindingKeyFromTemplate)
		{
			bWarnedStandInBindingKeyFromTemplate = true;
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] '%s' supplies its Crowdy Binding Key through Get Crowdy Binding Key, and an entity drawn as a row has to read that key off the class template rather than off a live component. A key that differs per instance makes the row read a container row that does not exist, and it fails silently. Derive the key from the component's own defaults if crowd rows must bind it (logged once)."),
				*ComponentClass->GetName());
		}

		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] entity %s stands in for component container '%s' ('%s', term '%s') as sub-participant %s"),
			*AnchorNetID.ToString(), *ComponentClass->GetName(), *Container.TypeName, *Container.InstanceTerm,
			*Registered.ToString());
	}

	return Enrolled;
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyGameModelSubsystem::BindEntityLifecycleForTest(UCrowdyEntitySubsystem* Entities)
{
	if (!Entities)
	{
		return;
	}
	EntitySubsystemForEvents = Entities;
	Entities->OnEntityRegistered.AddDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityRegistered);
	Entities->OnEntityUnregistered.AddDynamic(this, &UCrowdyGameModelSubsystem::HandleEntityUnregistered);
}
#endif

void UCrowdyGameModelSubsystem::GetSubParticipants(const FGuid& AnchorNetID, TArray<FGuid>& OutSubNetIDs) const
{
	OutSubNetIDs.Reset();
	if (const TArray<FGuid>* Subs = SubParticipantsByAnchor.Find(AnchorNetID))
	{
		OutSubNetIDs = *Subs;
	}
}

void UCrowdyGameModelSubsystem::ReplayCachedContainerStateToSubscriber(const FGuid& AnchorNetID) const
{
	if (!AnchorNetID.IsValid())
	{
		return;
	}

	auto ReplayOne = [this](const FGuid& NetID)
	{
		const TMap<FName, FString>* Cache = ContainerCache.Find(NetID);
		if (!Cache || Cache->IsEmpty())
		{
			return;
		}

		FString ContainerId;
		TryGetContainerId(NetID, ContainerId);

		// Old repeats New because a replay states what the value IS, not a transition. An empty Old reaches the
		// game's changed handler as a move from nothing to the full value, so a redraw of an untouched row pops a
		// heal number and replays hit reactions every time it crosses the relevance edge.
		TArray<FCrowdyAttributeChange> Changes;
		Changes.Reserve(Cache->Num());
		for (const TPair<FName, FString>& Entry : *Cache)
		{
			FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
			Change.Key = Entry.Key;
			Change.NewValueJson = Entry.Value;
			Change.OldValueJson = Entry.Value;
		}

		RelayChangesToEntitySubscriber(NetID, ContainerId, Changes);
	};

	ReplayOne(AnchorNetID);

	if (const TArray<FGuid>* Subs = SubParticipantsByAnchor.Find(AnchorNetID))
	{
		// Copied, because a relay reaches the holder and a holder may do anything, including releasing the row.
		const TArray<FGuid> SubsCopy = *Subs;
		for (const FGuid& SubNetID : SubsCopy)
		{
			ReplayOne(SubNetID);
		}
	}
}

void UCrowdyGameModelSubsystem::EnrollModelComponent(UActorComponent* Component)
{
	if (!IsValid(Component))
	{
		return;
	}
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	AActor* Owner = Component->GetOwner();
	if (!Entities || !Owner)
	{
		return;
	}
	const FGuid AnchorNetID = Entities->FindEntityID(Owner);
	if (!AnchorNetID.IsValid())
	{
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Warning,
			TEXT("[GameModel] EnrollModelComponent: owner '%s' of component '%s' is not a registered entity yet - add a UCrowdyEntityComponent and enroll after it registers."),
			*GetNameSafe(Owner), *Component->GetName());
		return;
	}
	EnrollComponentSubParticipant(Component, AnchorNetID);
}

void UCrowdyGameModelSubsystem::UnenrollModelComponent(UActorComponent* Component)
{
	if (!IsValid(Component))
	{
		return;
	}
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities)
	{
		return;
	}
	const FGuid SubNetID = Entities->FindEntityID(Component);
	if (!SubNetID.IsValid())
	{
		return; // never enrolled
	}

	// Drop the anchor -> sub tracking link so the anchor's later teardown does not redundantly re-unregister it.
	if (const AActor* Owner = Component->GetOwner())
	{
		const FGuid AnchorNetID = Entities->FindEntityID(Owner);
		if (TArray<FGuid>* Subs = SubParticipantsByAnchor.Find(AnchorNetID))
		{
			Subs->Remove(SubNetID);
			if (Subs->Num() == 0)
			{
				SubParticipantsByAnchor.Remove(AnchorNetID);
			}
		}
	}

	// Unregister the sub-participant: OnEntityUnregistered -> HandleEntityUnregistered(SubNetID) clears its binding
	// and cache. Done here (not left to the anchor cascade) so a component removed while its actor lives on does not
	// leak its record + container binding until the whole actor tears down.
	Entities->UnregisterEntity(SubNetID);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] unenrolled component '%s' (sub-participant %s)"), *Component->GetName(), *SubNetID.ToString());
}

void UCrowdyGameModelSubsystem::HandleEntityUnregistered(const FGuid& NetID)
{
	// If this was an anchor actor, unregister its component sub-participants first. Each UnregisterEntity broadcasts
	// OnEntityUnregistered, re-entering this handler for the sub id, which clears that sub's own binding/cache below.
	if (const TArray<FGuid>* Subs = SubParticipantsByAnchor.Find(NetID))
	{
		const TArray<FGuid> SubsCopy = *Subs; // UnregisterEntity mutates SubParticipantsByAnchor re-entrantly
		SubParticipantsByAnchor.Remove(NetID);
		if (UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem())
		{
			for (const FGuid& SubNetID : SubsCopy)
			{
				Entities->UnregisterEntity(SubNetID);
			}
		}
	}

	RemoveContainerBinding(NetID);
	ContainerStandIns.Remove(NetID);
	ContainerCache.Remove(NetID);
	ContainerTypeByNetID.Remove(NetID);
	NetIDToOwnerUserId.Remove(NetID);
	PendingModelEntities.Remove(NetID);
	PendingBindBackoff.Remove(NetID);
	ResolveInFlight.Remove(NetID);
	ClassDerivedBindings.Remove(NetID);
}

void UCrowdyGameModelSubsystem::RequestPendingModelEntitySweep()
{
	if (bShuttingDown || PendingModelEntities.Num() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		// The sweep runs on a timer, and there is no timer manager without a world. Nothing is lost: an entity can
		// only bind through an API context that a worldless subsystem does not have either.
		return;
	}

	FTimerManager& Timers = World->GetTimerManager();
	if (Timers.IsTimerActive(PendingSweepTimer) || Timers.IsTimerPending(PendingSweepTimer))
	{
		// A sweep is already coming. This is the whole point: the notifications that ask for one arrive at the rate
		// the session changes state, and each sweep is one network round trip per entity it re-drives.
		return;
	}

	Timers.SetTimer(PendingSweepTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]() { RetryPendingModelEntities(); }),
		PendingSweepIntervalSeconds, false);
}

void UCrowdyGameModelSubsystem::RetryPendingModelEntities()
{
	++PendingSweepCount;
	if (PendingModelEntities.Num() == 0)
	{
		PendingBindBackoff.Empty();
		return;
	}

	// Snapshot first: a resolve callback mutates PendingModelEntities (it removes an entry on a successful bind).
	TArray<TPair<FGuid, FString>> Snapshot;
	Snapshot.Reserve(PendingModelEntities.Num());
	for (const TPair<FGuid, FString>& Pair : PendingModelEntities)
	{
		Snapshot.Add(Pair);
	}

	const double Now = FApp::GetCurrentTime();
	int32 Attempted = 0;
	for (const TPair<FGuid, FString>& Pair : Snapshot)
	{
		const FGuid PendingNetID = Pair.Key;
		const FString PendingType = Pair.Value;
		if (NetIDToContainerId.Contains(PendingNetID))
		{
			PendingModelEntities.Remove(PendingNetID);
			PendingBindBackoff.Remove(PendingNetID);
			continue;
		}

		if (Attempted >= MaxPendingResolvesPerSweep)
		{
			// Each resolve below is a network round trip, and a busy world can hold hundreds of pending entities. The
			// rest are picked up by the next sweep, which the re-arm at the bottom guarantees will happen.
			break;
		}

		FCrowdyPendingBindBackoff& Backoff = PendingBindBackoff.FindOrAdd(PendingNetID);
		if (Backoff.Attempts > 0 && Now < Backoff.NextAttemptTime)
		{
			continue;
		}
		Backoff.NextAttemptTime = Now + static_cast<double>(ResolvePendingRetryDelaySeconds(Backoff.Attempts));
		++Backoff.Attempts;
		++Attempted;

		// Re-drive the ensure/read for a still-unbound entity: an authoritative entity that could not ensure yet
		// (no token at world start), or a remote proxy whose owner had not created the row when it first registered.
		const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
		ResolveOrCreateContainer(PendingNetID, PendingType, FString(),
			[WeakThis, PendingNetID](bool bOk, const FString& /*ContainerId*/)
		{
			if (bOk)
			{
				if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
				{
					// Asked by NetID, exactly like BindParticipantContainer's first-bind pull, so a class-derived
					// binding answers from its recorded container class - but a participant that no longer resolves
					// means the entity was unregistered during the round trip and must not pull.
					if (Self->ShouldPullOnRetryForEntity(PendingNetID))
					{
						Self->HandleModelChanged(PendingNetID);
					}
				}
			}
		});
	}

	// Drop backoff records whose entity is no longer pending. Every path that stops an entity being pending would
	// otherwise have to know about this map; instead the sweep reconciles the two, so they cannot drift.
	for (auto It = PendingBindBackoff.CreateIterator(); It; ++It)
	{
		if (!PendingModelEntities.Contains(It->Key))
		{
			It.RemoveCurrent();
		}
	}

	// Anything still pending needs another sweep, whether it was skipped by the per-sweep bound or is waiting out its
	// own backoff. Without this a pending entity would only be re-driven by the next inbound notification, which is
	// exactly the coupling the timer exists to break.
	RequestPendingModelEntitySweep();
}

void UCrowdyGameModelSubsystem::HandleHostChanged(const FGuid& NewHostID, const FGuid& PreviousHostID)
{
	// A liveness kick, not a create gate: re-drive any pending resolves. This covers an entity that could not ensure
	// at world start (no token yet - host election lands near token mint) and one left pending across a host
	// migration (a client that just became authoritative for a Host-owned container). Cheap no-op when nothing is
	// pending. Every client resolves independently now (atomic ensure), so this never decides who may create.
	//
	// The per-entity backoff is cleared first, because a host change is the one event that says the reason those
	// resolves kept failing (no token, a different authority) has actually changed. Leaving it in place would make an
	// entity that has already backed off sit out the very sweep this kick exists to run.
	PendingBindBackoff.Empty();
	RequestPendingModelEntitySweep();
}

void UCrowdyGameModelSubsystem::ApplyStateToContainer(const FGuid& NetID, UObject* Container,
	const TSharedPtr<FJsonObject>& NewState)
{
	if (!Container || !NewState.IsValid())
	{
		return;
	}

	// An entity drawn as a row rather than as an object holds its own storage elsewhere, so the values below are
	// handed to whoever holds it as well as written onto Container. Asked once for the whole apply, and false for
	// every entity represented by an object of its own, which leaves that path untouched.
	const bool bHasSubscriber = FindEntitySubscriberForEntity(ResolveSubscriberEntityID(NetID)) != nullptr;

	TMap<FName, FString>& Cache = ContainerCache.FindOrAdd(NetID);
	TArray<FCrowdyAttributeChange> Changes;

	// What the subscriber is handed: every key whose cached value ADVANCED, which is a superset of Changes.
	// Container is the object this entity is represented by here, not the class that declares its container, so a
	// key the entity genuinely owns can resolve no property on it at all; that key is exactly the one whose only
	// home is the subscriber's storage. Built only when there is a subscriber holding this entity.
	TArray<FCrowdyAttributeChange> Relayed;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : NewState->Values)
	{
		const FName Key(*Pair.Key);
		const FString Canonical = JsonValueToCompactString(Pair.Value);
		const FString* Existing = Cache.Find(Key);
		if (!Existing || *Existing != Canonical)
		{
			const FString OldJson = Existing ? *Existing : FString();
			// Advance the cache + fire a change only for a value that actually applied to the live member. The
			// codec rejects a forged aggregate (an over-length or wrong-typed array, a non-string ref) and leaves
			// the member unchanged; committing the rejected value would poison the cache and fire an OnRep + change
			// that disagrees with the member. A non-attribute server key (e.g. one the server still returns after
			// the class stopped declaring it) resolves no property either: advance the cache so an unchanging extra
			// key is not reprocessed each pull, but never notify.
			if (WriteJsonValueToModelProperty(Container, Key, Pair.Value))
			{
				Cache.Add(Key, Canonical);
				FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
				Change.Key = Key;
				Change.OldValueJson = OldJson;
				Change.NewValueJson = Canonical;
				if (bHasSubscriber)
				{
					Relayed.Add(Change);
				}
			}
			else if (!FindModelPropertyForKey(Container, Key))
			{
				Cache.Add(Key, Canonical);
				if (bHasSubscriber)
				{
					FCrowdyAttributeChange& Change = Relayed.AddDefaulted_GetRef();
					Change.Key = Key;
					Change.OldValueJson = OldJson;
					Change.NewValueJson = Canonical;
				}
				else
				{
					// A write the server CONFIRMED making, on a key this container does not declare and nothing
					// else here holds. Legitimate when a class stopped declaring an attribute the server still
					// stores, but it is also exactly what a misrouted cross-container write looks like, and the
					// silence here is what let one hide: the value was swallowed and the cache advanced so no
					// later pull retried it. Traced with both the container and the key so the two cases can be
					// told apart from a log.
					UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
						TEXT("[GameModel] '%s' declares no Server Owned attribute for confirmed write '%s'; value dropped"),
						*GetNameSafe(Container), *Key.ToString());
				}
			}
		}
	}

	for (const FCrowdyAttributeChange& Change : Changes)
	{
		FireParameterlessOnRep(Container, FindOnRepForServerKey(Container, Change.Key));
	}
	FString ModelId;
	TryGetContainerId(NetID, ModelId);
	BroadcastAttributeChanges(OnModelAttributeChanged, Container, ModelId, Changes);

	// Last, so a subscriber's own reaction runs after this container's cache, members and notifies have settled.
	RelayChangesToEntitySubscriber(NetID, ModelId, Relayed);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] ApplyState net=%s changed=%d relayed=%d"), *NetID.ToString(), Changes.Num(), Relayed.Num());
}

void UCrowdyGameModelSubsystem::ApplyMutationsToContainer(const FGuid& NetID, UObject* Container,
	const TArray<FCrowdyMutationApplied>& Mutations)
{
	if (!Container)
	{
		return;
	}

	// Same reason as ApplyStateToContainer: a confirmed invoke's writes are as authoritative as a pull's, and an
	// entity with no object of its own has to be told about them too or its storage sits on the value the last
	// pull left until the next one happens.
	const bool bHasSubscriber = FindEntitySubscriberForEntity(ResolveSubscriberEntityID(NetID)) != nullptr;

	TMap<FName, FString>& Cache = ContainerCache.FindOrAdd(NetID);
	TArray<FCrowdyAttributeChange> Changes;
	TArray<FCrowdyAttributeChange> Relayed;
	for (const FCrowdyMutationApplied& Mutation : Mutations)
	{
		const FName Key(*Mutation.Key);
		// Canonicalize the server's JSON-encoded value the SAME way ApplyState does so the two paths never
		// disagree; fall back to the raw string only if it does not parse as a standalone JSON value.
		const TSharedPtr<FJsonValue> Value = ParseJsonValueString(Mutation.NewValueJson);
		const FString Canonical = Value.IsValid() ? JsonValueToCompactString(Value) : Mutation.NewValueJson;
		const FString* Existing = Cache.Find(Key);
		if (!Existing || *Existing != Canonical)
		{
			const FString OldJson = Existing ? *Existing : FString();
			// Same rule as ApplyStateToContainer: advance the cache + notify only when the value actually applied.
			// An unparseable mutation value or a codec-rejected forged aggregate leaves the member unchanged and
			// must not poison the cache or fire a change; a non-attribute key advances the cache without notifying.
			const bool bApplied = Value.IsValid() && WriteJsonValueToModelProperty(Container, Key, Value);
			if (bApplied)
			{
				Cache.Add(Key, Canonical);
				FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
				Change.Key = Key;
				Change.OldValueJson = OldJson;
				Change.NewValueJson = Canonical;
				if (bHasSubscriber)
				{
					Relayed.Add(Change);
				}
			}
			else if (!FindModelPropertyForKey(Container, Key))
			{
				Cache.Add(Key, Canonical);
				if (bHasSubscriber)
				{
					FCrowdyAttributeChange& Change = Relayed.AddDefaulted_GetRef();
					Change.Key = Key;
					Change.OldValueJson = OldJson;
					Change.NewValueJson = Canonical;
				}
			}
		}
	}

	for (const FCrowdyAttributeChange& Change : Changes)
	{
		FireParameterlessOnRep(Container, FindOnRepForServerKey(Container, Change.Key));
	}
	FString ModelId;
	TryGetContainerId(NetID, ModelId);
	BroadcastAttributeChanges(OnModelAttributeChanged, Container, ModelId, Changes);

	RelayChangesToEntitySubscriber(NetID, ModelId, Relayed);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] ApplyMutations net=%s applied=%d changed=%d relayed=%d"), *NetID.ToString(), Mutations.Num(),
		Changes.Num(), Relayed.Num());
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyGameModelSubsystem::SetEventRouterForTest(UCrowdyEventRouter* InRouter)
{
	EventRouterForTest = InRouter;
}

UCrowdyEventRouter* UCrowdyGameModelSubsystem::GetEventRouterForTest() const
{
	return EventRouterForTest.Get();
}
#endif

ICrowdyEntitySubscriber* UCrowdyGameModelSubsystem::FindEntitySubscriberForEntity(const FGuid& NetID) const
{
	if (!NetID.IsValid())
	{
		return nullptr;
	}

	// Read from the router's slot rather than holding a second one. A subscriber is registered once, and the
	// world-travel rule that decides which of two live worlds owns it lives with that single registration; a
	// copy here would have to repeat that rule and could disagree with it, leaving server-owned values
	// reaching nothing while calls and view state kept working.
	const UCrowdyEventRouter* Router = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
	Router = EventRouterForTest.Get();
#endif
	if (!Router)
	{
		const UWorld* World = GetWorld();
		Router = World ? World->GetSubsystem<UCrowdyEventRouter>() : nullptr;
	}
	if (!Router)
	{
		return nullptr;
	}

	const TScriptInterface<ICrowdyEntitySubscriber>& Registered = Router->GetEntitySubscriber();
	ICrowdyEntitySubscriber* Subscriber = Registered.GetInterface();
	if (!Subscriber || !IsValid(Registered.GetObject()))
	{
		return nullptr;
	}

	return Subscriber->IsEntityKnown(NetID) ? Subscriber : nullptr;
}

FGuid UCrowdyGameModelSubsystem::ResolveSubscriberEntityID(const FGuid& NetID) const
{
	const UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	const FCrowdyEntityRecord* Record = Entities ? Entities->FindRecord(NetID) : nullptr;
	return (Record && Record->AnchorNetID.IsValid()) ? Record->AnchorNetID : NetID;
}

void UCrowdyGameModelSubsystem::RelayChangesToEntitySubscriber(const FGuid& NetID, const FString& ContainerId,
	const TConstArrayView<FCrowdyAttributeChange> Changes) const
{
	if (Changes.IsEmpty())
	{
		return;
	}

	// Addressed to the entity a holder knows, which for a component container is its anchor. ContainerId still names
	// the container the values came from, so a holder of several can tell them apart.
	const FGuid SubscriberNetID = ResolveSubscriberEntityID(NetID);
	ICrowdyEntitySubscriber* Subscriber = FindEntitySubscriberForEntity(SubscriberNetID);
	if (!Subscriber)
	{
		return;
	}

	Subscriber->ApplyModelChanges(SubscriberNetID, ContainerId, Changes);
}

bool UCrowdyGameModelSubsystem::TryGetContainerTypeForContainerId(const FString& ContainerId, FName& OutTypeName) const
{
	const FGuid NetID = FindNetIDForContainer(ContainerId);
	const FString* Found = NetID.IsValid() ? ContainerTypeByNetID.Find(NetID) : nullptr;
	if (!Found || Found->IsEmpty())
	{
		return false;
	}
	OutTypeName = FName(**Found);
	return true;
}

bool UCrowdyGameModelSubsystem::TryGetNetIDForContainer(const FString& ContainerId, FGuid& OutNetID) const
{
	// The same resolver the model-changed and signal paths use, so no two by-id paths can pick different objects.
	OutNetID = FindNetIDForContainer(ContainerId);
	return OutNetID.IsValid();
}

void UCrowdyGameModelSubsystem::ApplyInvokeMutations(const FGuid& SelfNetID, const FString& SelfContainerId,
	const TArray<FCrowdyMutationApplied>& Mutations)
{
	// Group by destination BEFORE applying any of it, so each destination takes exactly one apply call.
	// ApplyMutationsToContainer diffs against that container's cache and fires one OnRep per changed key, so
	// splitting one container's writes across several calls would fire its notifies in several batches.
	TMap<FString, TArray<FCrowdyMutationApplied>> ByContainer;
	for (const FCrowdyMutationApplied& Mutation : Mutations)
	{
		// A server predating the containerId field sends it empty, so an empty id means "the invoke's own
		// container": exactly the destination every mutation used before routing existed.
		const FString& Destination = Mutation.ContainerId.IsEmpty() ? SelfContainerId : Mutation.ContainerId;
		if (Destination.IsEmpty())
		{
			continue; // no id from the server and no self container to fall back to; nothing addressable
		}
		ByContainer.FindOrAdd(Destination).Add(Mutation);
	}

	for (const TPair<FString, TArray<FCrowdyMutationApplied>>& Pair : ByContainer)
	{
		// Precedence matches HandleModelChangedByContainer: a bound participant first, then a watched free/data
		// container. Never both, so a container that has a participant does not also fire the by-id delegate.
		// By id even for the invoke's own row, so a write and the signal that follows it cannot land on two objects.
		FGuid DestNetID = FindNetIDForContainer(Pair.Key);
		if (!DestNetID.IsValid() && Pair.Key == SelfContainerId)
		{
			DestNetID = SelfNetID;
		}

		if (DestNetID.IsValid())
		{
			if (UObject* Participant = ResolveEntityParticipant(DestNetID))
			{
				ApplyMutationsToContainer(DestNetID, Participant, Pair.Value);
				continue;
			}
		}

		if (WatchedDataContainers.Contains(Pair.Key))
		{
			ApplyDataContainerMutations(Pair.Key, Pair.Value);
			continue;
		}

		// A write to a container this client neither binds nor watches. There is nothing local to update; a
		// client that does hold it refreshes on its own model-changed notification.
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
			TEXT("[GameModel] invoke wrote container %s (%d key(s)) which is not bound or watched locally - skipped"),
			*Pair.Key, Pair.Value.Num());
	}
}

void UCrowdyGameModelSubsystem::HandleModelChanged(const FGuid& NetID)
{
	FString ContainerId;
	if (!TryGetContainerId(NetID, ContainerId))
	{
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
			TEXT("[GameModel] model-changed for unbound entity %s — ignored"), *NetID.ToString());
		return;
	}

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FGuid CapturedNetID = NetID;
	PullContainerState(ContainerId, [WeakThis, CapturedNetID](bool bOk, TSharedPtr<FJsonObject> State)
	{
		UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
		if (!bOk || !Self || !State.IsValid())
		{
			return;
		}
		if (UObject* Participant = Self->ResolveEntityParticipant(CapturedNetID))
		{
			Self->ApplyStateToContainer(CapturedNetID, Participant, State);
		}
	});
}

void UCrowdyGameModelSubsystem::EnqueueRefreshPull(const FString& ContainerId, const FGuid& NetID)
{
	if (bShuttingDown || ContainerId.IsEmpty() || !NetID.IsValid())
	{
		return;
	}

	// Add, not FindOrAdd: the entity holding a container can change between two notifications, and the pull has
	// to address the one holding it now.
	PendingRefreshPulls.Add(ContainerId, NetID);

	UWorld* World = GetWorld();
	if (!World)
	{
		// No world means no timer manager, and no API context either, so nothing could have been pulled anyway.
		return;
	}

	FTimerManager& Timers = World->GetTimerManager();
	if (Timers.IsTimerActive(RefreshPullTimer) || Timers.IsTimerPending(RefreshPullTimer))
	{
		return;
	}

	Timers.SetTimer(RefreshPullTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]() { DrainRefreshPulls(); }),
		ResolveRefreshPullWindowSeconds(), false);
}

float UCrowdyGameModelSubsystem::ResolveRefreshPullWindowSeconds() const
{
	return StretchCoalesceWindowSeconds(RefreshPullCoalesceSeconds, GetRecentInvokeCount(),
		InvokeBudgetLimitPerWindow);
}

void UCrowdyGameModelSubsystem::DrainRefreshPulls()
{
	if (PendingRefreshPulls.IsEmpty())
	{
		return;
	}

	// Moved out first: a pull's completion applies state, which fires notifies and delegates, and any of them may
	// notify again for the same container. Those belong to the next window rather than to this drain.
	TMap<FString, FGuid> Draining = MoveTemp(PendingRefreshPulls);
	PendingRefreshPulls.Reset();

	for (const TPair<FString, FGuid>& Pair : Draining)
	{
		HandleModelChanged(Pair.Value);
	}
}

void UCrowdyGameModelSubsystem::HandleModelChangedByContainer(const FString& ContainerId)
{
	if (ContainerId.IsEmpty())
	{
		return;
	}
	// Each client may bind its OWN entity to a shared container, so resolve the LOCAL entity bound to this
	// container and re-pull that. A container we do not have bound is not ours to update.
	const FGuid BoundNetID = FindNetIDForContainer(ContainerId);
	if (BoundNetID.IsValid())
	{
		EnqueueRefreshPull(ContainerId, BoundNetID);
		return;
	}

	// A watched free/data container (no actor, addressed by id): re-pull it so its by-id cache refreshes and the
	// OnDataContainerChanged delegate fires for any bound UI.
	if (WatchedDataContainers.Contains(ContainerId))
	{
		PullDataContainer(ContainerId, nullptr);
		return;
	}

	// Not bound locally yet. If a model entity registered before its owner created the container (a remote
	// proxy), this notification proves the container now exists retry the pending resolves so the proxy binds
	// and pulls. Self-heals the register-before-create race without polling.
	if (PendingModelEntities.Num() > 0)
	{
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
			TEXT("[GameModel] model-changed for unbound container %s — a sweep of %d pending model entit(ies) is queued"),
			*ContainerId, PendingModelEntities.Num());
		// Asked for, not run: these notifications arrive at the rate the whole session mutates state, and a sweep
		// costs a round trip per entity it re-drives.
		RequestPendingModelEntitySweep();
		return;
	}

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] model-changed for container %s not bound locally — ignored"), *ContainerId);
}

void UCrowdyGameModelSubsystem::InvokeAndApply(const FGuid& SelfNetID, const FString& FunctionName,
	const TSharedPtr<FJsonObject>& Params, const FString& SessionId, TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	InvokeAndApplyResolved(SelfNetID, FunctionName, Params, ResolveSessionId(SessionId, ActiveSessionId),
		MoveTemp(OnDone));
}

void UCrowdyGameModelSubsystem::InvokeAndApplyResolved(const FGuid& SelfNetID, const FString& FunctionName,
	const TSharedPtr<FJsonObject>& Params, const FString& ResolvedSessionId,
	TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	FString ContainerId;
	if (!TryGetContainerId(SelfNetID, ContainerId))
	{
		UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] InvokeAndApply: no container bound for entity %s"), *SelfNetID.ToString());
		FCrowdyInvokeResult Failed;
		Failed.ErrorMessage = TEXT("no container bound for entity");
		SetLastModelError(Failed.ErrorMessage);
		if (OnDone)
		{
			OnDone(Failed);
		}
		return;
	}

	TSharedRef<FCrowdyInvokeRequest> Req = MakeShared<FCrowdyInvokeRequest>();
	Req->FunctionName = FunctionName;
	Req->SelfContainerId = ContainerId;
	Req->SessionId = ResolvedSessionId;
	Req->Params = Params;

	// Stamped from the binding in place right now, so a retry armed off this call can tell whether it is still the
	// same one. A NetID with no epoch recorded is bound but unstamped, which no live binding is: AddContainerBinding
	// writes both together, so the guard reads it as already moved and simply declines to retry.
	FCrowdyInvokeBindingGuard Guard;
	Guard.bEntityBound = true;
	Guard.SelfNetID = SelfNetID;
	if (const uint32* CurrentEpoch = BindEpochByNetID.Find(SelfNetID))
	{
		Guard.BindEpoch = *CurrentEpoch;
	}

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FGuid CapturedNetID = SelfNetID;
	const FString CapturedContainerId = ContainerId;
	InvokeResolved(MoveTemp(Req),
		[WeakThis, CapturedNetID, CapturedContainerId, OnDone = MoveTemp(OnDone)](FCrowdyInvokeResult Result)
	{
		// The apply below writes into this world's cache and its entities, so it only runs while the world session
		// is live; the caller is told the invoke's outcome either way.
		UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
		if (Self && Result.bTransportOk && Result.bSuccess)
		{
			// Echo the CONFIRMED authoritative result into this client's cache + live members (not a prediction),
			// firing OnRep. Routed per mutation, because an effect that writes source.<attr> as well as
			// self.<attr> reports writes to two different containers in one result.
			Self->ApplyInvokeMutations(CapturedNetID, CapturedContainerId, Result.Mutations);
			// Record so this client drops its own model-changed echo (channel/139) for this container rather than
			// re-pulling over the confirmed apply it just did. Only the invoke's own container is marked: the
			// server notifies just that one, so marking a routed destination would arm a drop for a notification
			// that never comes and swallow the next genuine change to it instead.
			Self->MarkSelfActed(CapturedContainerId);
			// Fallback nudge for peers (server-native notification is the primary path; CVar can disable this).
			Self->EmitModelChangedPing(CapturedNetID, CapturedContainerId);
		}
		else if (Self && !Result.ErrorMessage.IsEmpty())
		{
			// Cache the server/transport reason so a UI can read it after a failed apply without a bespoke callback.
			Self->SetLastModelError(Result.ErrorMessage);
		}
		if (OnDone)
		{
			OnDone(Result);
		}
	}, Guard);
}

void UCrowdyGameModelSubsystem::EmitModelChangedPing(const FGuid& NetID, const FString& ContainerId) const
{
	if (CVarEmitFallbackPing.GetValueOnGameThread() == 0)
	{
		return;
	}

	// The fallback ping is a SPATIAL event, so it needs a concrete actor to anchor it. A non-actor participant (a
	// Host-owned subsystem) has no spatial anchor, so the fallback is simply skipped here. Peers bound to its
	// container then refresh only via a server-native/channel notification, which an effect authors when it has a
	// notification carrier; a bare Invoke on a subsystem (no carrier, no ping) will not nudge peers - they refresh
	// on their next pull. Use an effect with a notification carrier for a subsystem/free container peers watch.
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	AActor* Actor = ResolveEntityActor(NetID);
	if (!Entities || !Actor)
	{
		return;
	}

	FCrowdyModelChangedPing Ping;
	Ping.EntityID = NetID;
	Ping.ContainerId = ContainerId;
	Entities->DispatchGameEvent(Actor, FInstancedStruct::Make(Ping), ECrowdyTarget::Everyone);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] emitted model-changed ping for %s (container %s)"), *NetID.ToString(), *ContainerId);
}

bool UCrowdyGameModelSubsystem::DecodeContainerIdFromState(const TConstArrayView<uint8> StateBytes, FString& OutContainerId)
{
	// The authored function writes the changed container's id into the notification's state (an ASCII string).
	if (StateBytes.Num() == 0)
	{
		return false;
	}
	FString Id;
	Id.Reserve(StateBytes.Num());
	for (const uint8 Byte : StateBytes)
	{
		if (Byte == 0)
		{
			break; // stop at an embedded null terminator, if any
		}
		Id.AppendChar(static_cast<TCHAR>(Byte));
	}
	Id.TrimStartAndEndInline();
	OutContainerId = Id;
	return !OutContainerId.IsEmpty();
}

bool UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(const TArray<uint8>& Payload, FString& OutContainerId)
{
	if (Payload.Num() == 0)
	{
		return false;
	}

	const FString Prefix = CrowdyGameModelMetaKeys::ModelChangedChannelPrefix; // "cmc:"

	auto ExtractAfterPrefix = [&Prefix](const FString& Candidate, FString& Out) -> bool
	{
		if (!Candidate.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		Out = Candidate.RightChop(Prefix.Len());
		Out.TrimStartAndEndInline();
		return !Out.IsEmpty();
	};

	// The raw ASCII concat(prefix, id) and, if the payload is base64, its decoding. Sharing the encoding set with
	// the channel subsystem's skip test is what keeps a frame this decoder claims from also reaching the RPC decoder.
	TArray<FString> Forms;
	CrowdyGameModelMetaKeys::GameModelChannelPayloadForms(Payload, Forms);
	for (const FString& Form : Forms)
	{
		if (ExtractAfterPrefix(Form, OutContainerId))
		{
			return true;
		}
	}

	return false;
}

bool UCrowdyGameModelSubsystem::DecodeChannelSignal(const TArray<uint8>& Payload, FString& OutSignalName,
	FString& OutContainerId)
{
	if (Payload.Num() == 0)
	{
		return false;
	}

	const FString Prefix = CrowdyGameModelMetaKeys::SignalChannelPrefix; // "csg:"

	// "csg:<name>:<container id>". The name is split off at the FIRST separator after the prefix, so a container id
	// is free to contain anything. Everything about the name is then validated before it is used, because this
	// payload is attacker-reachable and the name selects a function to call.
	auto SplitAfterPrefix = [&Prefix](const FString& Candidate, FString& OutName, FString& OutId) -> bool
	{
		if (!Candidate.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Body = Candidate.RightChop(Prefix.Len());
		int32 Separator = INDEX_NONE;
		if (!Body.FindChar(TEXT(':'), Separator))
		{
			return false;
		}
		FString Name = Body.Left(Separator);
		FString Id = Body.RightChop(Separator + 1);
		Name.TrimStartAndEndInline();
		Id.TrimStartAndEndInline();
		// Reject rather than sanitize: a name that does not match what the authoring side can produce did not come
		// from an effect this build knows about, and turning it into an FName would let a forged frame probe for
		// arbitrary parameterless UFUNCTIONs on the bound object.
		if (Id.IsEmpty() || !UCrowdyEffect::IsValidSignalName(Name))
		{
			return false;
		}
		OutName = MoveTemp(Name);
		OutId = MoveTemp(Id);
		return true;
	};

	// Raw ASCII and, if the payload is base64, its decoding. Shared with the model-changed decoder and with the
	// channel subsystem's skip test so all three accept exactly the same encodings.
	TArray<FString> Forms;
	CrowdyGameModelMetaKeys::GameModelChannelPayloadForms(Payload, Forms);
	for (const FString& Form : Forms)
	{
		if (SplitAfterPrefix(Form, OutSignalName, OutContainerId))
		{
			return true;
		}
	}

	return false;
}

namespace
{
	// Runs Target's parameterless OnSignal_<Name>. True when a function of that name exists at all; bOutCalled says
	// whether it was actually run.
	bool CallCrowdySignalHandler(UObject* Target, const FName HandlerName, const FString& SignalName, bool& bOutCalled)
	{
		UFunction* Handler = Target->FindFunction(HandlerName);
		if (!Handler)
		{
			return false;
		}

		// Parameterless by contract, exactly like a CrowdyOnRep. The arity check is what makes a mismatch a silent
		// no-op instead of a corrupted call frame, since the handler is found by name and nothing forces its
		// signature at compile time.
		if (Handler->NumParms != 0)
		{
			UE_LOG(LogCrowdyGameModel, Error,
				TEXT("[GameModel] signal '%s' found '%s' on '%s', but it takes %d parameter(s); a signal handler must be parameterless, so it was not called."),
				*SignalName, *HandlerName.ToString(), *Target->GetName(), Handler->NumParms);
			return true;
		}

		Target->ProcessEvent(Handler, nullptr);
		bOutCalled = true;
		return true;
	}
}

void UCrowdyGameModelSubsystem::DispatchSignal(const FString& SignalName, const FString& ContainerId)
{
	if (SignalName.IsEmpty() || ContainerId.IsEmpty())
	{
		return;
	}

	// Resolve the LOCAL object bound to this container, mirroring HandleModelChangedByContainer: each client may
	// bind its own entity to a shared container, and a container nothing here is bound to still broadcasts (with a
	// null target) so non-container listeners such as UI can react.
	const FGuid BoundNetID = FindNetIDForContainer(ContainerId);
	UObject* Resolved = BoundNetID.IsValid() ? ResolveEntityParticipant(BoundNetID) : nullptr;
	if (!Resolved)
	{
		OnCrowdySignal.Broadcast(SignalName, nullptr, ContainerId);
		return;
	}

	const FName HandlerName = UCrowdyEffect::MakeSignalHandlerName(SignalName);
	bool bCalled = false;
	UObject* Handled = CallCrowdySignalHandler(Resolved, HandlerName, SignalName, bCalled) ? Resolved : nullptr;

	// A drawn row holds its component containers in behaviour-less stand-ins, so only a stand-in re-addresses to the
	// entity holding it; a real component that lacks the handler owns that.
	if (!Handled && Cast<UCrowdyContainerStandIn>(Resolved))
	{
		UObject* Anchor = ResolveEntityParticipant(ResolveSubscriberEntityID(BoundNetID));
		if (Anchor && CallCrowdySignalHandler(Anchor, HandlerName, SignalName, bCalled))
		{
			Handled = Anchor;
		}
	}

	UE_CLOG(!Handled && CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] signal '%s' for container %s: '%s' has no '%s' to call"),
		*SignalName, *ContainerId, *Resolved->GetName(), *HandlerName.ToString());

	// Whatever ran the handler, else the object bound to the container so a listener can still tell a row with a
	// local holder from one with none.
	OnCrowdySignal.Broadcast(SignalName, bCalled ? Handled : Resolved, ContainerId);
}

FCrowdyModelChangeHint UCrowdyGameModelSubsystem::MakeHintFromPing(const FCrowdyModelChangedPing& Ping)
{
	FCrowdyModelChangeHint Hint;
	Hint.EntityID = Ping.EntityID;
	Hint.ContainerId = Ping.ContainerId;
	Hint.EventType = CrowdyGameModelMetaKeys::ModelChangedEventType;
	return Hint;
}

void UCrowdyGameModelSubsystem::NotifyModelChanged(const FCrowdyModelChangeHint& Hint)
{
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] model-changed hint container=%s entity=%s eventType=%d"),
		*Hint.ContainerId, *Hint.EntityID.ToString(), Hint.EventType);
	OnModelChangedDelegate.Broadcast(Hint);
}

void UCrowdyGameModelSubsystem::HandleModelChangeHint(const FCrowdyModelChangeHint& Hint)
{
	// Self-echo drop: if THIS client just invoked this container, its own model-changed echo would trigger a
	// redundant re-pull that races the confirmed-mutation apply already done (a free/data pull full-replaces and
	// can transiently evict the just-cached value). Drop exactly one such echo; a later genuine change re-pulls.
	if (!Hint.ContainerId.IsEmpty() && ConsumeSelfEcho(Hint.ContainerId))
	{
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
			TEXT("[GameModel] self-echo for container %s - skipping redundant re-pull"), *Hint.ContainerId);
		return;
	}

	// Prefer the container (each client may bind its own entity to a shared container); fall back to the entity.
	// This is the precedence the ping carrier used before the sink existed, now applied uniformly to every carrier.
	if (!Hint.ContainerId.IsEmpty())
	{
		HandleModelChangedByContainer(Hint.ContainerId);
	}
	else if (Hint.EntityID.IsValid())
	{
		HandleModelChanged(Hint.EntityID);
	}
}

void UCrowdyGameModelSubsystem::PruneExpiredSelfActed(double Now)
{
	// The map holds one entry per recent self-invoke, so this is cheap.
	for (auto It = RecentlySelfActed.CreateIterator(); It; ++It)
	{
		if (It->Value <= Now)
		{
			It.RemoveCurrent();
		}
	}
}

void UCrowdyGameModelSubsystem::MarkSelfActed(const FString& ContainerId)
{
	if (ContainerId.IsEmpty())
	{
		return;
	}
	// Prune here too, not just in ConsumeSelfEcho, so a session that only invokes (and never receives a
	// model-changed notification to drive a consume) keeps the map bounded by the window rather than growing it.
	const double Now = FApp::GetCurrentTime();
	PruneExpiredSelfActed(Now);
	RecentlySelfActed.Add(ContainerId, Now + SelfEchoWindowSeconds);
}

bool UCrowdyGameModelSubsystem::ConsumeSelfEcho(const FString& ContainerId)
{
	const double Now = FApp::GetCurrentTime();
	PruneExpiredSelfActed(Now);

	if (const double* Expiry = RecentlySelfActed.Find(ContainerId))
	{
		if (*Expiry > Now)
		{
			RecentlySelfActed.Remove(ContainerId); // consume-once: only the first echo after a self-invoke is dropped
			return true;
		}
	}
	return false;
}

void UCrowdyGameModelSubsystem::CreateSession(const FString& Name, const TArray<int64>& ParticipantUserIds,
	const FString& MetadataJson, TFunction<void(bool, const FCrowdyGameModelSession&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, FCrowdyGameModelSession()); }
		return;
	}

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] CreateSession name=%s participants=%d appId=%lld"), *Name, ParticipantUserIds.Num(), AppId);

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FCrowdyGameSessionData Data)
	{
		if (OnDone) { OnDone(bOk, ToBpSession(Data)); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FCrowdyGameSessionData());
		return;
	}

	const TSharedPtr<FJsonObject> Vars =
		FCrowdyGameApiCodec::BuildCreateSessionVariables(AppId, Name, ParticipantUserIds, MetadataJson);
	Client->RunRuntimeOp(TEXT("GameModelCreateSession"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), TEXT("gameModelCreateSession"), Session);
		Complete(bOk, MoveTemp(Session));
	});
}

void UCrowdyGameModelSubsystem::JoinSession(const FString& SessionId, const FString& Role, TFunction<void(bool)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false); }
		return;
	}

	// An empty Session Id falls back to the active (default) session, so a caller who set it once can join it here.
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] JoinSession session=%s appId=%lld"), *ResolvedSession, AppId);

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FString /*SessionId*/, int64 /*UserId*/, FString /*Role*/)
	{
		if (OnDone) { OnDone(bOk); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FString(), 0, FString());
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildJoinSessionVariables(AppId, ResolvedSession, Role);
	Client->RunRuntimeOp(TEXT("GameModelJoinSession"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FString OutSessionId;
		int64 OutUserId = 0;
		FString OutRole;
		const bool bOk = FCrowdyGameApiCodec::ParseJoinSessionEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), OutSessionId, OutUserId, OutRole);
		Complete(bOk, MoveTemp(OutSessionId), OutUserId, MoveTemp(OutRole));
	});
}

void UCrowdyGameModelSubsystem::SetSessionTurn(const FString& SessionId, int64 UserId, bool bHasUserId,
	TFunction<void(bool, const FCrowdyGameModelSession&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, FCrowdyGameModelSession()); }
		return;
	}

	// An empty Session Id falls back to the active (default) session.
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] SetSessionTurn session=%s user=%s appId=%lld"), *ResolvedSession,
		bHasUserId ? *LexToString(UserId) : TEXT("<clear>"), AppId);

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FCrowdyGameSessionData Data)
	{
		if (OnDone) { OnDone(bOk, ToBpSession(Data)); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FCrowdyGameSessionData());
		return;
	}

	const TSharedPtr<FJsonObject> Vars =
		FCrowdyGameApiCodec::BuildSetSessionTurnVariables(AppId, ResolvedSession, UserId, bHasUserId);
	Client->RunRuntimeOp(TEXT("GameModelSetSessionTurn"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), TEXT("gameModelSetSessionTurn"), Session);
		Complete(bOk, MoveTemp(Session));
	});
}

void UCrowdyGameModelSubsystem::ListSessions(const FString& Status,
	TFunction<void(bool, const TArray<FCrowdyGameModelSession>&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, TArray<FCrowdyGameModelSession>()); }
		return;
	}

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, TArray<FCrowdyGameSessionData> Data)
	{
		TArray<FCrowdyGameModelSession> Sessions;
		Sessions.Reserve(Data.Num());
		for (const FCrowdyGameSessionData& D : Data)
		{
			Sessions.Add(ToBpSession(D));
		}
		if (OnDone) { OnDone(bOk, Sessions); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, TArray<FCrowdyGameSessionData>());
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildListSessionsVariables(AppId, Status);
	Client->RunRuntimeOp(TEXT("GameModelSessions"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		TArray<FCrowdyGameSessionData> Sessions;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionsEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), Sessions);
		Complete(bOk, MoveTemp(Sessions));
	});
}

void UCrowdyGameModelSubsystem::GetSession(const FString& SessionId,
	TFunction<void(bool, const FCrowdyGameModelSession&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, FCrowdyGameModelSession()); }
		return;
	}

	// An empty Session Id falls back to the active (default) session.
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FCrowdyGameSessionData Data)
	{
		if (OnDone) { OnDone(bOk, ToBpSession(Data)); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FCrowdyGameSessionData());
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildGetSessionVariables(AppId, ResolvedSession);
	Client->RunRuntimeOp(TEXT("GameModelSession"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), TEXT("gameModelSession"), Session);
		Complete(bOk, MoveTemp(Session));
	});
}

int64 UCrowdyGameModelSubsystem::GetLocalUserId() const
{
	const UCrowdyGameSession* Session = GetGameSession();
	return Session ? Session->GetUserID() : 0;
}

bool UCrowdyGameModelSubsystem::IsLocalUsersTurn(const FCrowdyGameModelSession& Session) const
{
	const int64 Local = GetLocalUserId();
	return Session.bHasCurrentTurn && Local != 0 && Session.CurrentTurnUserId == Local;
}

void UCrowdyGameModelSubsystem::SetActiveSession(const FString& SessionId)
{
	ActiveSessionId = SessionId;
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] active session set to %s"), SessionId.IsEmpty() ? TEXT("<app-global>") : *SessionId);
}

FString UCrowdyGameModelSubsystem::GetActiveSession() const
{
	return ActiveSessionId;
}

void UCrowdyGameModelSubsystem::ClearActiveSession()
{
	ActiveSessionId.Reset();
	ReportedModelRefusals.Empty();
}

void UCrowdyGameModelSubsystem::ReportModelRefusalOnce(const FString& Code, const FString& Subject,
	const FString& Detail)
{
	// Keyed on both, because subjects collide across kinds: a function and a container type can share a name, and
	// one row standing for both would say the wrong thing about whichever is not broken.
	const FString Key = Code + TEXT("|") + Subject;
	if (ReportedModelRefusals.Contains(Key))
	{
		return;
	}
	ReportedModelRefusals.Add(Key);

	// Warning rather than Log, and unconditional rather than behind the trace CVar: the developer who needs this
	// has no reason to have turned tracing on, because from where they are standing nothing looks wrong yet.
	UE_LOG(LogCrowdyGameModel, Warning,
		TEXT("[GameModel] the server refused '%s' because the app's game model is wrong (%s): %s Run gameModelLint against this app; quarantine clears itself once the definition is written again."),
		*Subject, *Code, *Detail);
}

FString UCrowdyGameModelSubsystem::ResolveSessionId(const FString& Explicit, const FString& Active)
{
	// Explicit wins, then the active (default) session, then empty (an app-global call). One place so the rule is
	// identical at every call site and can never drift.
	if (!Explicit.IsEmpty())
	{
		return Explicit;
	}
	return Active;
}

void UCrowdyGameModelSubsystem::SetLastModelError(const FString& InError)
{
	LastModelError = InError;
}

FString UCrowdyGameModelSubsystem::GetLastModelError() const
{
	return LastModelError;
}

void UCrowdyGameModelSubsystem::CreateDataContainer(const FString& TypeName, const FString& DisplayName,
	const FString& SessionId, const FString& MetadataJson, TFunction<void(bool, const FString&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, FString()); }
		return;
	}

	// An empty Session Id falls back to the active (default) session so a data container can be scoped without it.
	const FString ResolvedSession = ResolveSessionId(SessionId, ActiveSessionId);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] CreateDataContainer type=%s session=%s appId=%lld"), *TypeName,
		ResolvedSession.IsEmpty() ? TEXT("<app-global>") : *ResolvedSession, AppId);

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	auto Complete = [WeakThis, OnDone = MoveTemp(OnDone)](bool bOk, FString NewId, int64 /*OwnerUserId*/)
	{
		const bool bCreated = bOk && !NewId.IsEmpty();
		// Watch our own new container so a later model-changed notification for it re-pulls automatically. The watch
		// set belongs to this world, so it is only touched while the world session is live; the caller is told the
		// outcome either way.
		if (bCreated)
		{
			if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
			{
				Self->WatchedDataContainers.Add(NewId);
			}
		}
		if (OnDone) { OnDone(bCreated, NewId); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FString(), 0);
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildCreateContainerVariables(
		AppId, TypeName, DisplayName, ResolvedSession, MetadataJson);
	Client->RunRuntimeOp(TEXT("GameModelCreateContainer"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FString OutContainerId;
		int64 OutOwnerUserId = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseCreateContainerEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), OutContainerId, OutOwnerUserId);
		Complete(bOk, MoveTemp(OutContainerId), OutOwnerUserId);
	});
}

void UCrowdyGameModelSubsystem::PullDataContainer(const FString& ContainerId, TFunction<void(bool)> OnDone)
{
	if (ContainerId.IsEmpty())
	{
		if (OnDone) { OnDone(false); }
		return;
	}

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FString CapturedId = ContainerId;
	PullContainerState(ContainerId, [WeakThis, CapturedId, OnDone = MoveTemp(OnDone)](bool bOk, TSharedPtr<FJsonObject> State)
	{
		if (bOk && State.IsValid())
		{
			if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
			{
				// Watch only on a SUCCESSFUL pull, so a bogus/failed id never leaves a permanent watch entry.
				Self->WatchedDataContainers.Add(CapturedId);
				Self->ApplyDataContainerState(CapturedId, State);
			}
		}
		if (OnDone) { OnDone(bOk); }
	});
}

void UCrowdyGameModelSubsystem::InvokeOnContainer(const FString& ContainerId, const FString& FunctionName,
	const TSharedPtr<FJsonObject>& Params, const FString& SessionId, TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	InvokeOnContainerResolved(ContainerId, FunctionName, Params, ResolveSessionId(SessionId, ActiveSessionId),
		MoveTemp(OnDone));
}

void UCrowdyGameModelSubsystem::InvokeOnContainerResolved(const FString& ContainerId, const FString& FunctionName,
	const TSharedPtr<FJsonObject>& Params, const FString& ResolvedSessionId,
	TFunction<void(FCrowdyInvokeResult)> OnDone)
{
	if (ContainerId.IsEmpty())
	{
		if (OnDone)
		{
			FCrowdyInvokeResult Failed;
			Failed.ErrorMessage = TEXT("empty container id");
			OnDone(Failed);
		}
		return;
	}

	TSharedRef<FCrowdyInvokeRequest> Req = MakeShared<FCrowdyInvokeRequest>();
	Req->FunctionName = FunctionName;
	Req->SelfContainerId = ContainerId;
	Req->SessionId = ResolvedSessionId;
	Req->Params = Params;

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FString CapturedId = ContainerId;
	InvokeResolved(MoveTemp(Req), [WeakThis, CapturedId, OnDone = MoveTemp(OnDone)](FCrowdyInvokeResult Result)
	{
		// Optimistic echo of the CONFIRMED authoritative result into this client's by-id cache (fires the change
		// delegate), never a prediction. A free/data container has no actor to anchor the spatial fallback ping,
		// so there is none here: peers who watch this container re-pull on the server-native model-driven
		// notification, which carries the changed container id.
		// The merge below writes into this world's by-id cache, so it only runs while the world session is live; the
		// caller is told the invoke's outcome either way.
		UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
		if (Self && Result.bTransportOk && Result.bSuccess)
		{
			// Watch only on a COMMITTED invoke, so a rejected/failed invoke on a bad id never leaks a watch entry.
			Self->WatchedDataContainers.Add(CapturedId);
			// Merge the confirmed changed-subset onto the by-id cache, never a full replace: mutationsApplied is the
			// delta, so treating an absent key as removed would wrongly evict untouched state and fire a spurious
			// removal change event. Routed per mutation for the same reason the entity path is: a function invoked
			// on this container may also write another one, and those keys do not belong in this container's cache.
			// The watch added just above is what makes this container resolve to the by-id merge.
			Self->ApplyInvokeMutations(FGuid(), CapturedId, Result.Mutations);
			// Record so this client drops its own model-changed echo for this container rather than redundantly
			// re-pulling over the confirmed merge it just applied.
			Self->MarkSelfActed(CapturedId);
		}
		else if (Self && !Result.ErrorMessage.IsEmpty())
		{
			// Cache the server/transport reason so a UI can read it after a failed invoke without a bespoke callback.
			Self->SetLastModelError(Result.ErrorMessage);
		}
		if (OnDone) { OnDone(Result); }
	});
}

void UCrowdyGameModelSubsystem::SetDataProperty(const FString& ContainerId, const FString& Key,
	const FString& ValueType, const FString& ValueJson, TFunction<void(bool)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false); }
		return;
	}

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FString CapturedId = ContainerId;
	auto Complete = [WeakThis, CapturedId, OnDone = MoveTemp(OnDone)](bool bOk, FString /*ContainerId*/)
	{
		// A direct write is NOT recorded in the event log (no model-driven notification), so re-pull to refresh
		// the local cache and fire the change delegate when we already track this container. There is nothing to
		// refresh once the world session is gone, but the caller is still told the write's outcome.
		if (bOk)
		{
			UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
			if (Self && Self->WatchedDataContainers.Contains(CapturedId))
			{
				Self->PullDataContainer(CapturedId, nullptr);
			}
		}
		if (OnDone) { OnDone(bOk); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FString());
		return;
	}

	const TSharedPtr<FJsonObject> Vars =
		FCrowdyGameApiCodec::BuildSetPropertyVariables(AppId, ContainerId, Key, ValueType, ValueJson);
	Client->RunRuntimeOp(TEXT("GameModelSetProperty"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FString OutContainerId;
		const bool bOk = FCrowdyGameApiCodec::ParseSetPropertyEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), OutContainerId);
		Complete(bOk, MoveTemp(OutContainerId));
	});
}

void UCrowdyGameModelSubsystem::DeleteContainer(const FString& ContainerId, TFunction<void(bool)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false); }
		return;
	}

	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	const FString CapturedId = ContainerId;
	auto Complete = [WeakThis, CapturedId, OnDone = MoveTemp(OnDone)](bool bOk, bool /*bDeleted*/)
	{
		// On a clean transport the container no longer exists (whether it was deleted now or was already gone), so
		// drop every local trace of it: the free/data by-id cache + watch set, and - for an actor-bound container -
		// every per-NetID trace of each NetID bound to it (mirroring HandleEntityUnregistered), so a cached BP read
		// or a re-bind never sees the deleted container's stale attribute/owner values. The removal is the change,
		// so broadcast once. Once the world session is gone every one of those caches is already empty, so there is
		// nothing to reconcile - but the caller is still told the delete's outcome.
		if (bOk)
		{
			if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
			{
				Self->WatchedDataContainers.Remove(CapturedId);
				Self->DataContainerCache.Remove(CapturedId);
				// One hash lookup for every NetID bound to the deleted row, rather than a string compare against
				// every binding this client holds. Copied out because RemoveContainerBinding writes to the very
				// array the lookup returns.
				TArray<FGuid> BoundNetIDs;
				if (const TArray<FGuid>* Found = Self->ContainerIdToNetIDs.Find(CapturedId))
				{
					BoundNetIDs = *Found;
				}
				for (const FGuid& BoundNetID : BoundNetIDs)
				{
					Self->RemoveContainerBinding(BoundNetID);
					Self->ContainerCache.Remove(BoundNetID);
					Self->NetIDToOwnerUserId.Remove(BoundNetID);
					Self->PendingModelEntities.Remove(BoundNetID);
					Self->PendingBindBackoff.Remove(BoundNetID);
					Self->ResolveInFlight.Remove(BoundNetID);
				}
				Self->OnDataContainerChanged.Broadcast(CapturedId);
			}
		}
		if (OnDone) { OnDone(bOk); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, false);
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildDeleteContainerVariables(AppId, ContainerId);
	Client->RunRuntimeOp(TEXT("GameModelDeleteContainer"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		bool bDeleted = false;
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), bDeleted);
		Complete(bOk, bDeleted);
	});
}

void UCrowdyGameModelSubsystem::DeleteEdge(const FString& EdgeId, TFunction<void(bool)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false); }
		return;
	}

	// Edges are not cached client-side (Traverse/ListChildren return them fresh), so there is nothing local to
	// reconcile; just report the outcome.
	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, bool /*bDeleted*/)
	{
		if (OnDone) { OnDone(bOk); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, false);
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildDeleteEdgeVariables(AppId, EdgeId);
	Client->RunRuntimeOp(TEXT("GameModelDeleteEdge"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		bool bDeleted = false;
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteEdgeEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), bDeleted);
		Complete(bOk, bDeleted);
	});
}

bool UCrowdyGameModelSubsystem::TryGetContainerValueJson(const FString& ContainerId, const FName Key, FString& OutJson) const
{
	if (const TMap<FName, FString>* Cache = DataContainerCache.Find(ContainerId))
	{
		if (const FString* Found = Cache->Find(Key))
		{
			OutJson = *Found;
			return true;
		}
	}
	return false;
}

void UCrowdyGameModelSubsystem::WatchDataContainer(const FString& ContainerId)
{
	if (!ContainerId.IsEmpty())
	{
		WatchedDataContainers.Add(ContainerId);
	}
}

void UCrowdyGameModelSubsystem::UnwatchDataContainer(const FString& ContainerId)
{
	WatchedDataContainers.Remove(ContainerId);
	DataContainerCache.Remove(ContainerId);
}

void UCrowdyGameModelSubsystem::ApplyDataContainerState(const FString& ContainerId, const TSharedPtr<FJsonObject>& NewState)
{
	if (ContainerId.IsEmpty() || !NewState.IsValid())
	{
		return;
	}

	TMap<FName, FString>& Cache = DataContainerCache.FindOrAdd(ContainerId);
	TArray<FCrowdyAttributeChange> Changes;

	// The pulled state is the container's COMPLETE visible property set, so record which keys it carries: a
	// free/data container's key set is dynamic (an inventory slot cleared, a quest objective dropped), unlike an
	// actor's fixed attribute set, so a cached key absent from the new state was REMOVED, not merely unchanged.
	TSet<FName> PresentKeys;
	PresentKeys.Reserve(NewState->Values.Num());
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : NewState->Values)
	{
		const FName Key(*Pair.Key);
		PresentKeys.Add(Key);
		// Canonicalize the SAME way the actor-bound apply path does so equal values always compare equal.
		const FString Canonical = JsonValueToCompactString(Pair.Value);
		const FString* Existing = Cache.Find(Key);
		if (!Existing || *Existing != Canonical)
		{
			FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
			Change.Key = Key;
			Change.OldValueJson = Existing ? *Existing : FString();
			Change.NewValueJson = Canonical;
			Cache.Add(Key, Canonical);
		}
	}

	// Drop cached keys the new complete state no longer contains (a removed slot/objective); each removal is a
	// change a bound UI must see, or a taken item would linger as a ghost value forever. A removal broadcasts an
	// empty NewValueJson so a per-attribute listener can tell it apart from a value change.
	for (auto It = Cache.CreateIterator(); It; ++It)
	{
		if (!PresentKeys.Contains(It.Key()))
		{
			FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
			Change.Key = It.Key();
			Change.OldValueJson = It.Value();
			It.RemoveCurrent();
		}
	}

	// A free/data container has no actor and no per-attribute OnRep, so the whole-container delegate carries the
	// notification; the bound UI re-reads whatever keys it cares about. Fires once per changed pull, never on an
	// unchanged re-pull. The per-attribute delegate (Target null, ModelId = container id) fires alongside it for a
	// listener that wants the individual key transitions.
	const bool bChanged = Changes.Num() > 0;
	if (bChanged)
	{
		OnDataContainerChanged.Broadcast(ContainerId);
	}
	BroadcastAttributeChanges(OnModelAttributeChanged, nullptr, ContainerId, Changes);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] ApplyDataContainerState container=%s changed=%s keys=%d"),
		*ContainerId, bChanged ? TEXT("yes") : TEXT("no"), NewState->Values.Num());
}

void UCrowdyGameModelSubsystem::ApplyDataContainerMutations(const FString& ContainerId,
	const TArray<FCrowdyMutationApplied>& Mutations)
{
	if (ContainerId.IsEmpty())
	{
		return;
	}

	TMap<FName, FString>& Cache = DataContainerCache.FindOrAdd(ContainerId);
	TArray<FCrowdyAttributeChange> Changes;
	for (const FCrowdyMutationApplied& Mutation : Mutations)
	{
		const FName Key(*Mutation.Key);
		// Canonicalize the SAME way the pull path does so equal values compare equal across both apply paths.
		const TSharedPtr<FJsonValue> Value = ParseJsonValueString(Mutation.NewValueJson);
		const FString Canonical = Value.IsValid() ? JsonValueToCompactString(Value) : Mutation.NewValueJson;
		const FString* Existing = Cache.Find(Key);
		if (!Existing || *Existing != Canonical)
		{
			FCrowdyAttributeChange& Change = Changes.AddDefaulted_GetRef();
			Change.Key = Key;
			Change.OldValueJson = Existing ? *Existing : FString();
			Change.NewValueJson = Canonical;
			Cache.Add(Key, Canonical);
		}
	}

	// A merge never removes, so an untouched cached key stays and never fires a spurious change. Broadcast the
	// whole-container delegate once (if anything changed) plus the per-attribute delegate, mirroring the pull path.
	if (Changes.Num() > 0)
	{
		OnDataContainerChanged.Broadcast(ContainerId);
	}
	BroadcastAttributeChanges(OnModelAttributeChanged, nullptr, ContainerId, Changes);

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] ApplyDataContainerMutations container=%s applied=%d changed=%d"),
		*ContainerId, Mutations.Num(), Changes.Num());
}

void UCrowdyGameModelSubsystem::AddEdge(const FString& FromContainerId, const FString& ToContainerId,
	const FString& RelationshipType, float Weight, bool bHasWeight, const FString& MetadataJson,
	TFunction<void(bool, const FCrowdyContainerEdge&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, FCrowdyContainerEdge()); }
		return;
	}

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FCrowdyEdgeData Edge)
	{
		if (OnDone) { OnDone(bOk, ToBpEdge(Edge)); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FCrowdyEdgeData());
		return;
	}

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildAddEdgeVariables(
		AppId, FromContainerId, ToContainerId, RelationshipType, static_cast<double>(Weight), bHasWeight, MetadataJson);
	Client->RunRuntimeOp(TEXT("GameModelAddEdge"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FCrowdyEdgeData Edge;
		const bool bOk = FCrowdyGameApiCodec::ParseAddEdgeEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), Edge);
		Complete(bOk, MoveTemp(Edge));
	});
}

void UCrowdyGameModelSubsystem::Traverse(const FString& RootId, const FString& RelationshipType, int32 Depth,
	TFunction<void(bool, const TArray<FCrowdyContainerRef>&, const TArray<FCrowdyContainerEdge>&)> OnDone)
{
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		if (OnDone) { OnDone(false, TArray<FCrowdyContainerRef>(), TArray<FCrowdyContainerEdge>()); }
		return;
	}

	auto Complete = [OnDone = MoveTemp(OnDone)](bool bOk, FCrowdyTraverseData Result)
	{
		TArray<FCrowdyContainerRef> Nodes;
		Nodes.Reserve(Result.Nodes.Num());
		for (const TSharedPtr<FJsonObject>& Node : Result.Nodes)
		{
			Nodes.Add(ToBpContainerRef(Node));
		}
		TArray<FCrowdyContainerEdge> Edges;
		Edges.Reserve(Result.Edges.Num());
		for (const FCrowdyEdgeData& Edge : Result.Edges)
		{
			Edges.Add(ToBpEdge(Edge));
		}
		if (OnDone) { OnDone(bOk, Nodes, Edges); }
	};

	FCrowdyCppClient* Client = EnsureCppClient(Endpoint, Token);
	if (!Client)
	{
		Complete(false, FCrowdyTraverseData());
		return;
	}

	const TSharedPtr<FJsonObject> Vars =
		FCrowdyGameApiCodec::BuildTraverseVariables(AppId, RootId, RelationshipType, Depth);
	Client->RunRuntimeOp(TEXT("GameModelTraverse"), Vars,
		[Complete = MoveTemp(Complete)](FCrowdyCppJsonResult R)
	{
		FCrowdyTraverseData Result;
		const bool bOk = FCrowdyGameApiCodec::ParseTraverseEnvelope(
			WrapCppDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), Result);
		Complete(bOk, MoveTemp(Result));
	});
}

void UCrowdyGameModelSubsystem::TouchCollectionParent(const FString& ParentContainerId, const FString& ParentTypeName)
{
	if (ParentContainerId.IsEmpty())
	{
		return;
	}

	// This client's own collection view refreshes off the CONFIRMED edge change (a re-traverse driven by
	// OnDataContainerChanged), broadcast directly here - NOT off the touch's crowdy_rev echo. The touch invokes on
	// a parent this client may never have pulled, so routing it through InvokeOnContainer (which watches + merges
	// the mutation subset onto the by-id cache) would leave a partial {crowdy_rev} cache and fire phantom change
	// events on the first real pull. The touch below is therefore purely server-side: bump crowdy_rev + emit the
	// channel notification so PEERS (who watch the parent) re-pull.
	OnDataContainerChanged.Broadcast(ParentContainerId);

	if (ParentTypeName.IsEmpty())
	{
		// The caller could not name the per-type touch function, so peers get no notification (they refresh on
		// their next pull). Always-on warning: a Verbose+CVar-gated trace would be invisible by default and a
		// designer would never learn the collection change did not propagate.
		UE_LOG(LogCrowdyGameModel, Warning,
			TEXT("[GameModel] collection change on container %s was not broadcast to peers: no parent container type was provided to name its touch function. Pass the parent's Game Model type."),
			*ParentContainerId);
		return;
	}

	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(ParentTypeName);
	// The touch runs against the parent, so the server injects $self_container_id = ParentContainerId; the touch
	// function's notification names the parent from that, so no params are needed.
	Req.SelfContainerId = ParentContainerId;
	Req.Params = MakeShared<FJsonObject>();

	// The raw Invoke does not touch this client's cache, so nothing to poison. On success MarkSelfActed so this
	// client drops its own inbound echo (it already refreshed off the edge change above).
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	Invoke(Req, [WeakThis, ParentContainerId](FCrowdyInvokeResult Result)
	{
		if (!Result.bTransportOk || !Result.bSuccess)
		{
			// Always-on warning, and reported whether or not the world session survived the round-trip: it needs
			// nothing from the subsystem, and a touch that failed late is exactly the case a designer must hear
			// about (the collection change did not propagate to peers).
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] collection touch invoke failed for container=%s (%s); peers refresh on their next pull"),
				*ParentContainerId, *Result.ErrorMessage);
			return;
		}
		// Marking the self-echo touches this world's bookkeeping, so it only runs while the session is live.
		if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
		{
			Self->MarkSelfActed(ParentContainerId);
		}
	});
}

void UCrowdyGameModelSubsystem::AddToCollection(const FString& ParentContainerId, const FString& ParentTypeName,
	const FString& ItemContainerId, const FString& RelationshipType, TFunction<void(bool)> OnDone)
{
	if (ParentContainerId.IsEmpty() || ItemContainerId.IsEmpty())
	{
		if (OnDone) { OnDone(false); }
		return;
	}
	const FString Relationship = RelationshipType.IsEmpty()
		? FString(CrowdyGameModelMetaKeys::DefaultCollectionRelationship) : RelationshipType;

	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	// A containment edge carries no weight (bHasWeight=false). On success, touch the parent so watchers re-pull.
	AddEdge(ParentContainerId, ItemContainerId, Relationship, 0.0f, /*bHasWeight*/ false, FString(),
		[WeakThis, ParentContainerId, ParentTypeName, OnDone = MoveTemp(OnDone)](bool bOk, const FCrowdyContainerEdge&)
	{
		if (bOk)
		{
			if (UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis))
			{
				Self->TouchCollectionParent(ParentContainerId, ParentTypeName);
			}
		}
		if (OnDone) { OnDone(bOk); }
	});
}

void UCrowdyGameModelSubsystem::RemoveFromCollection(const FString& ParentContainerId, const FString& ParentTypeName,
	const FString& ItemContainerId, const FString& RelationshipType, TFunction<void(bool)> OnDone)
{
	if (ParentContainerId.IsEmpty() || ItemContainerId.IsEmpty())
	{
		if (OnDone) { OnDone(false); }
		return;
	}
	const FString Relationship = RelationshipType.IsEmpty()
		? FString(CrowdyGameModelMetaKeys::DefaultCollectionRelationship) : RelationshipType;

	// gameModelDeleteEdge deletes by edge id, so resolve the parent -> item edge(s) of this relationship first (a
	// depth-1 traverse returns the direct edges with their ids). A double-add can leave more than one matching
	// edge, so delete ALL of them - deleting only the first would report success while the item stayed listed.
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	Traverse(ParentContainerId, Relationship, 1,
		[WeakThis, ParentContainerId, ParentTypeName, ItemContainerId, Relationship, OnDone = MoveTemp(OnDone)]
		(bool bOk, const TArray<FCrowdyContainerRef>&, const TArray<FCrowdyContainerEdge>& Edges)
	{
		// Without a live world session there is nothing to delete edges on behalf of, so report the removal as not
		// done rather than fanning out further work; the caller is notified exactly once either way.
		UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
		if (!bOk || !Self)
		{
			if (OnDone) { OnDone(false); }
			return;
		}

		TArray<FString> EdgeIds;
		for (const FCrowdyContainerEdge& E : Edges)
		{
			if (E.FromContainerId == ParentContainerId && E.ToContainerId == ItemContainerId
				&& E.RelationshipType == Relationship && !E.EdgeId.IsEmpty())
			{
				EdgeIds.Add(E.EdgeId);
			}
		}
		if (EdgeIds.Num() == 0)
		{
			// No such edge: the item is not in this collection (already removed, or never added). Nothing changed,
			// so no touch; report false (mirrors the server's delete-edge "returns true only if an edge was deleted").
			UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
				TEXT("[GameModel] RemoveFromCollection: no '%s' edge %s -> %s"),
				*Relationship, *ParentContainerId, *ItemContainerId);
			if (OnDone) { OnDone(false); }
			return;
		}

		// Delete every matching edge, join on a shared counter (all callbacks are game-thread), then touch the
		// parent once after the last delete iff anything was actually removed.
		const TSharedRef<int32> Pending = MakeShared<int32>(EdgeIds.Num());
		const TSharedRef<bool> AnyDeleted = MakeShared<bool>(false);
		for (const FString& EdgeId : EdgeIds)
		{
			Self->DeleteEdge(EdgeId,
				[WeakThis, ParentContainerId, ParentTypeName, Pending, AnyDeleted, OnDone](bool bDeleted)
			{
				if (bDeleted)
				{
					*AnyDeleted = true;
				}
				// Every delete completes, so the counter always reaches zero and OnDone fires exactly once - on the
				// last completion - whether or not the world session survived the round-trip.
				if (--(*Pending) <= 0)
				{
					if (*AnyDeleted)
					{
						if (UCrowdyGameModelSubsystem* Self2 = ResolveLiveSelf(WeakThis))
						{
							Self2->TouchCollectionParent(ParentContainerId, ParentTypeName);
						}
					}
					if (OnDone) { OnDone(*AnyDeleted); }
				}
			});
		}
	});
}

void UCrowdyGameModelSubsystem::GetCollectionWithState(const FString& ParentContainerId, const FString& RelationshipType,
	int32 MaxItems, TFunction<void(bool, const TArray<FCrowdyCollectionItem>&)> OnDone)
{
	if (ParentContainerId.IsEmpty())
	{
		if (OnDone) { OnDone(false, TArray<FCrowdyCollectionItem>()); }
		return;
	}
	const FString Relationship = RelationshipType.IsEmpty()
		? FString(CrowdyGameModelMetaKeys::DefaultCollectionRelationship) : RelationshipType;
	// Bound the per-item state fan-out: default 64, clamp to [1, 256] so a designer-supplied MaxItems can never
	// fan an unbounded burst of simultaneous state reads.
	const int32 Cap = FMath::Clamp(MaxItems > 0 ? MaxItems : 64, 1, 256);

	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	ListChildren(ParentContainerId, Relationship,
		[WeakThis, ParentContainerId, Cap, OnDone = MoveTemp(OnDone)](bool bOk, const TArray<FCrowdyContainerRef>& Children)
	{
		// Without a live world session there is nothing to read state through, so report the get as failed rather
		// than fanning out per-item reads; the caller is notified exactly once either way.
		UCrowdyGameModelSubsystem* Self = ResolveLiveSelf(WeakThis);
		if (!bOk || !Self)
		{
			if (OnDone) { OnDone(false, TArray<FCrowdyCollectionItem>()); }
			return;
		}

		// Bound the per-item state fan-out; a larger collection is truncated, never silently (warn what was dropped).
		TArray<FCrowdyContainerRef> Items = Children;
		if (Items.Num() > Cap)
		{
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] GetCollectionWithState: collection %s has %d items, capped at %d; the rest are omitted"),
				*ParentContainerId, Items.Num(), Cap);
			Items.SetNum(Cap);
		}
		if (Items.Num() == 0)
		{
			if (OnDone) { OnDone(true, TArray<FCrowdyCollectionItem>()); }
			return;
		}

		// Fan one state pull per item and join on a shared counter (every callback is game-thread). Results keep the
		// child order; a per-item read failure yields that item with an empty StateJson rather than failing the set.
		// PullContainerState is the raw read facade, so this neither watches nor broadcasts (a get is not a change).
		const TSharedRef<TArray<FCrowdyCollectionItem>> Results = MakeShared<TArray<FCrowdyCollectionItem>>();
		Results->SetNum(Items.Num());
		const TSharedRef<int32> Pending = MakeShared<int32>(Items.Num());
		// Tracks whether any constituent read actually succeeded, so a total read failure (every item's state
		// pull failing) is reported as a failure rather than as success with an all-blank result set.
		const TSharedRef<bool> AnySucceeded = MakeShared<bool>(false);

		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			(*Results)[Index].ContainerId = Items[Index].ContainerId;
			(*Results)[Index].TypeName = Items[Index].TypeName;

			Self->PullContainerState(Items[Index].ContainerId,
				[Results, Pending, AnySucceeded, Index, OnDone](bool bStateOk, TSharedPtr<FJsonObject> State)
			{
				if (bStateOk && State.IsValid() && Results->IsValidIndex(Index))
				{
					*AnySucceeded = true;
					// Drop the SDK's reserved bookkeeping counter so it never surfaces as if it were a designer
					// field to a UI that iterates the item's state keys (a RemoveField on an absent key is a no-op).
					State->RemoveField(CrowdyGameModelMetaKeys::CollectionRevKey);
					FString Json;
					const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
						TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
					if (FJsonSerializer::Serialize(State.ToSharedRef(), Writer))
					{
						(*Results)[Index].StateJson = Json;
					}
				}
				// Every state read completes, so the counter always reaches zero and OnDone fires exactly once - on
				// the last completion - whether or not the world session survived the fan-out.
				if (--(*Pending) <= 0 && OnDone)
				{
					OnDone(*AnySucceeded, *Results);
				}
			});
		}
	});
}

void UCrowdyGameModelSubsystem::ListChildren(const FString& RootId, const FString& RelationshipType,
	TFunction<void(bool, const TArray<FCrowdyContainerRef>&)> OnDone)
{
	const FString CapturedRoot = RootId;
	Traverse(RootId, RelationshipType, 1,
		[CapturedRoot, OnDone = MoveTemp(OnDone)](bool bOk, const TArray<FCrowdyContainerRef>& Nodes,
			const TArray<FCrowdyContainerEdge>& /*Edges*/)
	{
		// A depth-1 traversal returns the root among its nodes; drop it so callers get only the children.
		TArray<FCrowdyContainerRef> Children;
		Children.Reserve(Nodes.Num());
		for (const FCrowdyContainerRef& Node : Nodes)
		{
			if (Node.ContainerId != CapturedRoot)
			{
				Children.Add(Node);
			}
		}
		if (OnDone) { OnDone(bOk, Children); }
	});
}
