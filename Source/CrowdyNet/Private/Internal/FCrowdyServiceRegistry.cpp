#include "Internal/FCrowdyServiceRegistry.h"

#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "HAL/IConsoleManager.h"
#include "Internal/FCrowdyRoutingTable.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Misc/ScopeLock.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/Class.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

/**
 * Every registry alive in this process, so the route dump can reach all of them. A registry adds
 * itself last in its constructor and removes itself first in its destructor.
 */
static FCriticalSection GCrowdyLiveRegistriesMutex;
static TArray<FCrowdyServiceRegistry*> GCrowdyLiveRegistries;

static FAutoConsoleCommandWithOutputDevice GCrowdyNetRoutesCommand(
	TEXT("crowdy.net.routes"),
	TEXT("Prints the network router's subscription table: every payload key and opcode, with the ")
	TEXT("struct behind it where one is registered, and every subscriber by role."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&FCrowdyServiceRegistry::DumpAllRoutes));

FCrowdySubscriptionKey FCrowdySubscriptionKey::EventPayload(const FCrowdyTypeID TypeID)
{
	FCrowdySubscriptionKey Key;
	Key.Kind = EKind::Payload;
	Key.PayloadKey = FCrowdyPayloadKey::Event(TypeID);
	return Key;
}

FCrowdySubscriptionKey FCrowdySubscriptionKey::EventPayload(const UScriptStruct* PayloadStruct)
{
	FCrowdySubscriptionKey Key;
	Key.SourceStruct = PayloadStruct;

	if (!PayloadStruct)
	{
		return Key;
	}

	// Naming a struct here is what registers it for the wire, so an author never has to do it twice.
	UEventPayloadRegistry::Get()->RegisterStructAuto(const_cast<UScriptStruct*>(PayloadStruct));

	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	if (!UEventPayloadRegistry::Get()->GetID(PayloadStruct, TypeID))
	{
		return Key;
	}

	Key.Kind = EKind::Payload;
	Key.PayloadKey = FCrowdyPayloadKey::Event(TypeID);
	return Key;
}

FCrowdySubscriptionKey FCrowdySubscriptionKey::ActorUpdatePayload(const FCrowdyTypeID TypeID)
{
	FCrowdySubscriptionKey Key;
	Key.Kind = EKind::Payload;
	Key.PayloadKey = FCrowdyPayloadKey::ActorUpdate(TypeID);
	return Key;
}

FCrowdySubscriptionKey FCrowdySubscriptionKey::ActorUpdatePayload(const UScriptStruct* PayloadStruct)
{
	FCrowdySubscriptionKey Key;
	Key.SourceStruct = PayloadStruct;

	if (!PayloadStruct)
	{
		return Key;
	}

	UActorUpdatePayloadRegistry::Get()->RegisterStructAuto(const_cast<UScriptStruct*>(PayloadStruct));

	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	if (!UActorUpdatePayloadRegistry::Get()->GetID(PayloadStruct, TypeID))
	{
		return Key;
	}

	Key.Kind = EKind::Payload;
	Key.PayloadKey = FCrowdyPayloadKey::ActorUpdate(TypeID);
	return Key;
}

FCrowdySubscriptionKey FCrowdySubscriptionKey::AllPayloads(const ECrowdyPayloadCategory Category)
{
	FCrowdySubscriptionKey Key;
	Key.PayloadKey.Category = Category;

	if (Category != ECrowdyPayloadCategory::None)
	{
		Key.Kind = EKind::PayloadWildcard;
	}

	return Key;
}

FCrowdySubscriptionKey FCrowdySubscriptionKey::Opcode(const ECrowdyMessageType MessageType)
{
	FCrowdySubscriptionKey Key;
	Key.Kind = EKind::Opcode;
	Key.MessageType = MessageType;
	return Key;
}

FString FCrowdySubscriptionKey::Describe() const
{
	switch (Kind)
	{
	case EKind::Payload:
		return PayloadKey.Describe();

	case EKind::PayloadWildcard:
		return FString::Printf(TEXT("%s/*"), FCrowdyPayloadKey::CategoryName(PayloadKey.Category));

	case EKind::Opcode:
		return FString::Printf(TEXT("opcode %u"), static_cast<uint32>(static_cast<uint8>(MessageType)));

	default:
		return SourceStruct
			? FString::Printf(TEXT("unresolved struct '%s'"), *SourceStruct->GetPathName())
			: FString(TEXT("unset key"));
	}
}

FString FCrowdyRoutingConflict::Describe() const
{
	const FString Names = OtherSubscriber.IsNone()
		? Subscriber.ToString()
		: FString::Printf(TEXT("%s and %s"), *Subscriber.ToString(), *OtherSubscriber.ToString());

	return Key.IsEmpty()
		? FString::Printf(TEXT("%s: %s"), *Names, *Detail)
		: FString::Printf(TEXT("%s on %s: %s"), *Names, *Key, *Detail);
}

FCrowdyServiceRegistry::FCrowdyServiceRegistry()
{
	RouterState = MakeShared<FCrowdyRouterState, ESPMode::ThreadSafe>();
	RouterState->Owner = this;

	{
		FScopeLock Lock(&RecordsMutex);
		RebuildTable();
	}

	FScopeLock Lock(&GCrowdyLiveRegistriesMutex);
	GCrowdyLiveRegistries.Add(this);
}

FCrowdyServiceRegistry::~FCrowdyServiceRegistry()
{
	{
		FScopeLock Lock(&GCrowdyLiveRegistriesMutex);
		GCrowdyLiveRegistries.Remove(this);
	}

	{
		FScopeLock Lock(&RouterState->Mutex);
		RouterState->Owner = nullptr;
	}

	{
		FScopeLock Lock(&RecordsMutex);
		for (const FRecordPtr& Record : Records)
		{
			if (Record.IsValid())
			{
				Record->bActive.store(false, std::memory_order_release);
			}
		}
		Records.Empty();
	}

	FRWScopeLock WriteLock(TableLock, SLT_Write);
	CurrentTable.Reset();
}

FCrowdySubscription FCrowdyServiceRegistry::Subscribe(const TConstArrayView<FCrowdySubscriptionKey> Keys,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	FCrowdySubscription Subscription;

	FScopeLock Lock(&RecordsMutex);

	if (!ValidateSubscription(Keys, Options, static_cast<bool>(Handler)))
	{
		return Subscription;
	}

	FRecordPtr Record = MakeShared<FCrowdySubscriptionRecord, ESPMode::ThreadSafe>();
	Record->Handler = MoveTemp(Handler);
	Record->Options = Options;
	Record->Keys.Append(Keys.GetData(), Keys.Num());
	Record->Router = RouterState;

	Records.Add(Record);
	RebuildTable();

	Subscription.Record = MoveTemp(Record);
	return Subscription;
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToEventPayload(const UScriptStruct* PayloadStruct,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::EventPayload(PayloadStruct);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToEventPayload(const FCrowdyTypeID TypeID,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::EventPayload(TypeID);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToActorUpdatePayload(const UScriptStruct* PayloadStruct,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::ActorUpdatePayload(PayloadStruct);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToActorUpdatePayload(const FCrowdyTypeID TypeID,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::ActorUpdatePayload(TypeID);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToAllPayloads(const ECrowdyPayloadCategory Category,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::AllPayloads(Category);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

FCrowdySubscription FCrowdyServiceRegistry::SubscribeToOpcode(const ECrowdyMessageType MessageType,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler)
{
	const FCrowdySubscriptionKey Key = FCrowdySubscriptionKey::Opcode(MessageType);
	return Subscribe(MakeArrayView(&Key, 1), Options, MoveTemp(Handler));
}

bool FCrowdyServiceRegistry::ValidateSubscription(const TConstArrayView<FCrowdySubscriptionKey> Keys,
	const FCrowdySubscriptionOptions& Options, const bool bHasHandler)
{
	const FString Who = Options.SubscriberName.IsNone()
		? FString(TEXT("<unnamed>"))
		: Options.SubscriberName.ToString();

	if (!bHasHandler)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[Router] '%s' subscribed without a handler; nothing was registered."), *Who);
		Conflicts.Add({ Options.SubscriberName, NAME_None, FString(), TEXT("subscribed without a handler") });
		return false;
	}

	if (Keys.IsEmpty())
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[Router] '%s' subscribed without a key; nothing was registered."), *Who);
		Conflicts.Add({ Options.SubscriberName, NAME_None, FString(), TEXT("subscribed without a key") });
		return false;
	}

	for (const FCrowdySubscriptionKey& Key : Keys)
	{
		const bool bIsPayloadKey = Key.Kind == FCrowdySubscriptionKey::EKind::Payload
			|| Key.Kind == FCrowdySubscriptionKey::EKind::PayloadWildcard;

		if (Key.Kind == FCrowdySubscriptionKey::EKind::Invalid
			|| (bIsPayloadKey && Key.PayloadKey.Category == ECrowdyPayloadCategory::None))
		{
			UE_LOG(LogCrowdyNet, Error,
				TEXT("[Router] '%s' subscribed to %s, which resolves to no payload type; nothing was registered."),
				*Who, *Key.Describe());
			Conflicts.Add({ Options.SubscriberName, NAME_None, Key.Describe(), TEXT("key resolves to no payload type") });
			return false;
		}
	}

	if (Options.bRequiresExclusiveHandling && Options.Role != ECrowdySubscriptionRole::Handle)
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[Router] '%s' asked for exclusive handling without the Handle role; the request is ignored."), *Who);
	}

	for (const FCrowdySubscriptionKey& Key : Keys)
	{
		if (Key.Kind == FCrowdySubscriptionKey::EKind::PayloadWildcard
			&& Options.Role == ECrowdySubscriptionRole::Handle)
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("[Router] '%s' handles %s, which counts every payload in that category as handled and ")
				TEXT("suppresses every fallback in it."), *Who, *Key.Describe());
		}

		if (Key.Kind == FCrowdySubscriptionKey::EKind::Opcode && !IsOpcodeEverParsed(Key.MessageType))
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("[Router] '%s' subscribed to %s, which the message parser never produces, so it can never fire."),
				*Who, *Key.Describe());
		}

		if (Key.Kind == FCrowdySubscriptionKey::EKind::Opcode && IsOpcodeAlwaysPayloadRouted(Key.MessageType))
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("[Router] '%s' subscribed to %s by opcode, but that opcode always carries a payload key and ")
				TEXT("is routed by it, never by the opcode table, so this subscription can never fire. Subscribe ")
				TEXT("to the payload instead."),
				*Who, *Key.Describe());
		}

		if (Options.Role != ECrowdySubscriptionRole::Handle)
		{
			continue;
		}

		for (const FRecordPtr& Existing : Records)
		{
			if (!Existing.IsValid() || !Existing->bActive.load(std::memory_order_acquire))
			{
				continue;
			}

			if (Existing->Options.Role != ECrowdySubscriptionRole::Handle)
			{
				continue;
			}

			if (!Existing->Options.bRequiresExclusiveHandling && !Options.bRequiresExclusiveHandling)
			{
				continue;
			}

			if (!Existing->Keys.Contains(Key))
			{
				continue;
			}

			// Both subscriptions stay: refusing the second would change delivery in the same edit that
			// reports the problem, and the point of the report is that the developer decides.
			UE_LOG(LogCrowdyNet, Error,
				TEXT("[Router] '%s' and '%s' both handle %s, which was declared as needing a single handler. ")
				TEXT("Both are still delivered to."),
				*Existing->Options.SubscriberName.ToString(), *Who, *Key.Describe());

			Conflicts.Add({ Existing->Options.SubscriberName, Options.SubscriberName, Key.Describe(),
				TEXT("two handlers where one declared it needs to be the only handler") });
		}
	}

	return true;
}

void FCrowdyServiceRegistry::OnSubscriptionReleased(const FRecordPtr& Record)
{
	FScopeLock Lock(&RecordsMutex);
	Records.Remove(Record);
	RebuildTable();
}

void FCrowdyServiceRegistry::RebuildTable()
{
	TSharedRef<FCrowdyRoutingTable, ESPMode::ThreadSafe> NewTable =
		MakeShared<FCrowdyRoutingTable, ESPMode::ThreadSafe>();

	for (const FRecordPtr& Record : Records)
	{
		if (!Record.IsValid() || !Record->bActive.load(std::memory_order_acquire))
		{
			continue;
		}

		for (const FCrowdySubscriptionKey& Key : Record->Keys)
		{
			FCrowdyRouteSlot* Slot = nullptr;

			switch (Key.Kind)
			{
			case FCrowdySubscriptionKey::EKind::Payload:
				Slot = &NewTable->RoutesFor(Key.PayloadKey.Category).FindOrAdd(Key.PayloadKey.TypeID);
				break;

			case FCrowdySubscriptionKey::EKind::PayloadWildcard:
				Slot = &NewTable->RoutesFor(Key.PayloadKey.Category).Wildcard;
				break;

			case FCrowdySubscriptionKey::EKind::Opcode:
				Slot = &NewTable->OpcodeRoutes[static_cast<uint8>(Key.MessageType)];
				break;

			default:
				break;
			}

			if (!Slot)
			{
				continue;
			}

			switch (Record->Options.Role)
			{
			case ECrowdySubscriptionRole::Handle:   Slot->Handlers.Add(Record);  break;
			case ECrowdySubscriptionRole::Fallback: Slot->Fallbacks.Add(Record); break;
			default:                                Slot->Observers.Add(Record); break;
			}
		}
	}

	NewTable->Generation = NextGeneration++;

	// A reader that is already walking the table this replaces keeps its own reference to it, and the
	// last owner frees it. No free list, no deferred reclamation.
	const FTablePtr Published(NewTable);

	FRWScopeLock WriteLock(TableLock, SLT_Write);
	CurrentTable = Published;
}

FCrowdyServiceRegistry::FTablePtr FCrowdyServiceRegistry::PinTable() const
{
	FTablePtr Table;
	{
		FRWScopeLock ReadLock(TableLock, SLT_ReadOnly);
		Table = CurrentTable;
	}
	return Table;
}

int32 FCrowdyServiceRegistry::DeliverToList(const TArray<FRecordPtr>& List, const FCrowdyDelivery& Delivery)
{
	int32 Called = 0;

	// No early exit of any kind: one subscriber must not be able to stop the rest from being told.
	for (const FRecordPtr& Record : List)
	{
		if (!Record.IsValid() || !Record->bActive.load(std::memory_order_acquire) || !Record->Handler)
		{
			continue;
		}

		// Wraps the subscriber call and nothing else, so what is left as DispatchMessage's own time is
		// the routing proper. Placed here rather than around DeliverToList because that runs up to four
		// times a message over lists that are usually empty, and scoping an empty list would spend
		// tracing overhead measuring nothing.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_SubscriberHandler);

#if !PLATFORM_EXCEPTIONS_DISABLED && (defined(__cpp_exceptions) || defined(_CPPUNWIND))
			try
			{
				Record->Handler(Delivery);
			}
			catch (...)
			{
				UE_LOG(LogCrowdyNet, Error,
					TEXT("[Router] subscriber '%s' threw while handling %s; the remaining subscribers still ran."),
					*Record->Options.SubscriberName.ToString(), *Delivery.PayloadKey.Describe());
			}
#else
			Record->Handler(Delivery);
#endif
		}

		++Called;
	}

	return Called;
}

void FCrowdyServiceRegistry::DispatchMessage(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{
	// The routing half of per-message delivery. Expected to be small: the table is pinned once and the
	// lookup is a direct index, never a scan over subscribers. Scoped anyway, because "expected to be
	// small" is what the payload-decode share was assumed to be before it was measured.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DispatchMessage);

	const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> ConstMessage = Message;

	const FCrowdyDelivery Delivery
	{
		Message->GetType(),
		Message->GetPayloadKey(),
		Message->GetPayload(),
		Message->GetPayloadBytes(),
		ConstMessage
	};

	// Pinned once for the whole delivery. Anything that subscribes while this runs is only in the
	// table that replaces this one, so it takes effect on the next message.
	const FTablePtr Table = PinTable();
	if (!Table.IsValid())
	{
		ReportUnrouted(Delivery);
		return;
	}

	const FCrowdyRouteSlot* Slot = nullptr;
	const FCrowdyRouteSlot* Wildcard = nullptr;

	if (Delivery.PayloadKey.IsSet())
	{
		const FCrowdyPayloadRoutes& Routes = Table->RoutesFor(Delivery.PayloadKey.Category);
		Slot = Routes.Find(Delivery.PayloadKey.TypeID);
		Wildcard = &Routes.Wildcard;
	}
	else
	{
		Slot = &Table->OpcodeRoutes[static_cast<uint8>(Delivery.Opcode)];
	}

	int32 Called = 0;

	if (Slot)     { Called += DeliverToList(Slot->Observers, Delivery); }
	if (Wildcard) { Called += DeliverToList(Wildcard->Observers, Delivery); }

	int32 Handled = 0;
	if (Slot)     { Handled += DeliverToList(Slot->Handlers, Delivery); }
	if (Wildcard) { Handled += DeliverToList(Wildcard->Handlers, Delivery); }

	Called += Handled;

	if (Handled == 0)
	{
		if (Slot)     { Called += DeliverToList(Slot->Fallbacks, Delivery); }
		if (Wildcard) { Called += DeliverToList(Wildcard->Fallbacks, Delivery); }
	}

	if (Called == 0)
	{
		ReportUnrouted(Delivery);
		return;
	}

	DeliveredCount.fetch_add(1, std::memory_order_relaxed);

	// Flips exactly once across every calling thread; only that one thread dumps the table.
	if (!bDumpedFirstRoutes.exchange(true, std::memory_order_relaxed))
	{
		MaybeDumpFirstRoutes();
	}
}

void FCrowdyServiceRegistry::ReportUnrouted(const FCrowdyDelivery& Delivery)
{
	UnroutedCount.fetch_add(1, std::memory_order_relaxed);

	// Hashed first so the common case (a key already seen and suppressed) never builds a string or
	// touches the payload registry; only a first sighting pays for Describe().
	const uint32 KeyHash = HashCombine(::GetTypeHash(Delivery.PayloadKey),
		::GetTypeHash(static_cast<uint8>(Delivery.Opcode)));

	bool bFirstSighting = false;
	bool bJustHitCap = false;
	{
		FScopeLock Lock(&StatsMutex);

		if (!ReportedUnroutedKeys.Contains(KeyHash))
		{
			if (ReportedUnroutedKeys.Num() < MaxTrackedUnroutedKeys)
			{
				ReportedUnroutedKeys.Add(KeyHash);
				bFirstSighting = true;
			}
			else if (!bUnroutedCapReported)
			{
				bUnroutedCapReported = true;
				bJustHitCap = true;
			}
		}
	}

	if (bJustHitCap)
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[Router] more than %d distinct unrouted keys have arrived; no further ones will be named, ")
			TEXT("though the total count keeps being tracked."),
			MaxTrackedUnroutedKeys);
	}

	if (!bFirstSighting)
	{
		return;
	}

	// Throttled to the first sighting of each key: an unsubscribed payload usually arrives at the
	// same rate as a subscribed one.
	const FString KeyText = Delivery.PayloadKey.IsSet()
		? Delivery.PayloadKey.Describe()
		: FString::Printf(TEXT("opcode %u"), static_cast<uint32>(static_cast<uint8>(Delivery.Opcode)));

	{
		FScopeLock Lock(&StatsMutex);
		UnroutedKeyDescriptions.Add(KeyText);
	}

	UE_LOG(LogCrowdyNet, Warning, TEXT("[Router] nothing subscribes to %s arriving on opcode %u."),
		*KeyText, static_cast<uint32>(static_cast<uint8>(Delivery.Opcode)));
}

void FCrowdyServiceRegistry::MaybeDumpFirstRoutes()
{
	if (!UE_LOG_ACTIVE(LogCrowdyNet, Verbose))
	{
		return;
	}

	FStringOutputDevice Out;
	DumpRoutes(Out);
	UE_LOG(LogCrowdyNet, Verbose, TEXT("%s"), *Out);
}

bool FCrowdyServiceRegistry::IsOpcodeEverParsed(const ECrowdyMessageType MessageType)
{
	switch (MessageType)
	{
	case ECrowdyMessageType::BAD_MESSAGE:                  // the parser's stand-in for anything it drops
	case ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION:
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
	case ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
	case ECrowdyMessageType::SERVER_EVENT_NOTIFICATION:
	case ECrowdyMessageType::GENERIC_SPATIAL_1:
	case ECrowdyMessageType::SINGLE_ACTOR_MESSAGE:
	case ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION:
		return true;

	default:
		return false;
	}
}

bool FCrowdyServiceRegistry::IsOpcodeAlwaysPayloadRouted(const ECrowdyMessageType MessageType)
{
	// These carriers always set a payload key (FGameEventNotification and FActorUpdateNotificationMessage
	// both override GetPayloadKey unconditionally, and every opcode below derives from one of the two), so
	// DispatchMessage never consults the opcode table for them.
	switch (MessageType)
	{
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
	case ECrowdyMessageType::SERVER_EVENT_NOTIFICATION:
	case ECrowdyMessageType::SINGLE_ACTOR_MESSAGE:
		return true;

	default:
		return false;
	}
}

TArray<FCrowdyRoutingConflict> FCrowdyServiceRegistry::GetConflicts() const
{
	FScopeLock Lock(&RecordsMutex);
	return Conflicts;
}

FCrowdyRoutingStats FCrowdyServiceRegistry::GetStats() const
{
	FCrowdyRoutingStats Stats;
	Stats.DeliveredCount = DeliveredCount.load(std::memory_order_relaxed);
	Stats.UnroutedCount = UnroutedCount.load(std::memory_order_relaxed);

	FScopeLock Lock(&StatsMutex);
	Stats.UnroutedKeys = UnroutedKeyDescriptions;
	return Stats;
}

namespace CrowdyRouteDump
{
	static void AppendRole(FOutputDevice& Ar, const TCHAR* RoleName,
		const TArray<TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe>>& Records)
	{
		if (Records.IsEmpty())
		{
			return;
		}

		FString Names;
		for (const TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			if (!Names.IsEmpty())
			{
				Names += TEXT(", ");
			}

			Names += Record->Options.SubscriberName.IsNone()
				? FString(TEXT("<unnamed>"))
				: Record->Options.SubscriberName.ToString();

			if (!Record->bActive.load(std::memory_order_acquire))
			{
				Names += TEXT(" (released)");
			}
		}

		if (!Names.IsEmpty())
		{
			Ar.Logf(TEXT("      %s: %s"), RoleName, *Names);
		}
	}

	static void AppendSlot(FOutputDevice& Ar, const FString& Label, const FCrowdyRouteSlot& Slot)
	{
		if (Slot.IsEmpty())
		{
			return;
		}

		Ar.Logf(TEXT("    %s"), *Label);
		AppendRole(Ar, TEXT("observe"), Slot.Observers);
		AppendRole(Ar, TEXT("handle"), Slot.Handlers);
		AppendRole(Ar, TEXT("fallback"), Slot.Fallbacks);
	}

	static void AppendCategory(FOutputDevice& Ar, const ECrowdyPayloadCategory Category,
		const FCrowdyPayloadRoutes& Routes)
	{
		Ar.Logf(TEXT("  %s payloads"), FCrowdyPayloadKey::CategoryName(Category));

		for (int32 Index = 0; Index < Routes.Keys.Num(); ++Index)
		{
			FCrowdyPayloadKey Key;
			Key.Category = Category;
			Key.TypeID = Routes.Keys[Index];
			AppendSlot(Ar, Key.Describe(), Routes.Slots[Index]);
		}

		AppendSlot(Ar, FString::Printf(TEXT("%s/* (every payload in the category)"),
			FCrowdyPayloadKey::CategoryName(Category)), Routes.Wildcard);
	}
}

void FCrowdyServiceRegistry::DumpRoutes(FOutputDevice& Ar) const
{
	const FTablePtr Table = PinTable();
	if (!Table.IsValid())
	{
		Ar.Logf(TEXT("[Router] no routing table has been built yet."));
		return;
	}

	const FCrowdyRoutingStats Stats = GetStats();

	Ar.Logf(TEXT("[Router] table generation %u, %llu delivered, %llu unrouted."),
		Table->Generation, Stats.DeliveredCount, Stats.UnroutedCount);

	CrowdyRouteDump::AppendCategory(Ar, ECrowdyPayloadCategory::Event, Table->EventRoutes);
	CrowdyRouteDump::AppendCategory(Ar, ECrowdyPayloadCategory::ActorUpdate, Table->ActorUpdateRoutes);

	Ar.Logf(TEXT("  opcodes"));
	for (int32 Opcode = 0; Opcode < 256; ++Opcode)
	{
		CrowdyRouteDump::AppendSlot(Ar, FString::Printf(TEXT("opcode %d"), Opcode), Table->OpcodeRoutes[Opcode]);
	}

	for (const FString& Unrouted : Stats.UnroutedKeys)
	{
		Ar.Logf(TEXT("  nothing subscribes to %s"), *Unrouted);
	}

	for (const FCrowdyRoutingConflict& Conflict : GetConflicts())
	{
		Ar.Logf(TEXT("  conflict: %s"), *Conflict.Describe());
	}
}

void FCrowdyServiceRegistry::DumpAllRoutes(FOutputDevice& Ar)
{
	FScopeLock Lock(&GCrowdyLiveRegistriesMutex);

	if (GCrowdyLiveRegistries.IsEmpty())
	{
		Ar.Logf(TEXT("[Router] no registry is alive in this process."));
		return;
	}

	for (const FCrowdyServiceRegistry* Registry : GCrowdyLiveRegistries)
	{
		Registry->DumpRoutes(Ar);
	}
}

