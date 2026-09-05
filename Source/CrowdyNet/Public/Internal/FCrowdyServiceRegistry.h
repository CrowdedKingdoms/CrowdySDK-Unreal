#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Core/UDP/Subscription/FCrowdySubscriptionOptions.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

#include <atomic>

class UScriptStruct;
class FOutputDevice;

struct FCrowdyRoutingTable;
struct FCrowdyRouterState;
struct FCrowdySubscriptionRecord;

/**
 * One key of a subscription. A subscription may carry several: a system that cares about three
 * carriers of the same logical event takes one handle over three keys and gets one callback.
 */
struct CROWDYNET_API FCrowdySubscriptionKey
{
	enum class EKind : uint8
	{
		/** Built from something that could not be resolved. Reported and refused at Subscribe. */
		Invalid,
		/** One payload type number inside one category. */
		Payload,
		/** Every payload in one category, whatever its type number. */
		PayloadWildcard,
		/** One opcode, for the carriers that ship no payload type tag. */
		Opcode,
	};

	EKind Kind = EKind::Invalid;
	FCrowdyPayloadKey PayloadKey;
	ECrowdyMessageType MessageType = ECrowdyMessageType::BAD_MESSAGE;

	/** Kept only so a key built from an unresolvable struct can name it when it is refused. */
	const UScriptStruct* SourceStruct = nullptr;

	static FCrowdySubscriptionKey EventPayload(FCrowdyTypeID TypeID);
	static FCrowdySubscriptionKey EventPayload(const UScriptStruct* PayloadStruct);
	static FCrowdySubscriptionKey ActorUpdatePayload(FCrowdyTypeID TypeID);
	static FCrowdySubscriptionKey ActorUpdatePayload(const UScriptStruct* PayloadStruct);

	/** Every payload in the category, whatever its type. */
	static FCrowdySubscriptionKey AllPayloads(ECrowdyPayloadCategory Category);

	/** The plainly named second flavour, for opcodes that carry no payload type tag. */
	static FCrowdySubscriptionKey Opcode(ECrowdyMessageType MessageType);

	bool operator==(const FCrowdySubscriptionKey& Other) const
	{
		return Kind == Other.Kind && PayloadKey == Other.PayloadKey && MessageType == Other.MessageType;
	}

	bool operator!=(const FCrowdySubscriptionKey& Other) const { return !(*this == Other); }

	FString Describe() const;
};

/** A routing problem worth a developer's attention, kept so a test can assert on it. */
struct CROWDYNET_API FCrowdyRoutingConflict
{
	FName Subscriber;
	FName OtherSubscriber;
	FString Key;
	FString Detail;

	FString Describe() const;
};

struct CROWDYNET_API FCrowdyRoutingStats
{
	uint64 DeliveredCount = 0;
	uint64 UnroutedCount = 0;

	/** One entry per distinct key that arrived with nothing subscribed to it. */
	TArray<FString> UnroutedKeys;
};

/**
 * Internal-only registry. Owns services and routes inbound messages to their subscribers.
 *
 * Routing is by what a message is, not by the opcode that carried it: a message that declares a
 * payload key is matched against that key and its category wildcard, and everything else is
 * matched by opcode. Subscribers hold a handle whose release stops any further invocation; the
 * threading contract a handler runs under is stated on FCrowdyDelivery.
 */
class CROWDYNET_API FCrowdyServiceRegistry
{
public:

	FCrowdyServiceRegistry();
	~FCrowdyServiceRegistry();

	FCrowdyServiceRegistry(const FCrowdyServiceRegistry&) = delete;
	FCrowdyServiceRegistry& operator=(const FCrowdyServiceRegistry&) = delete;

	/** One handle, one callback, however many keys. Releasing it releases all of them. */
	FCrowdySubscription Subscribe(TConstArrayView<FCrowdySubscriptionKey> Keys,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToEventPayload(const UScriptStruct* PayloadStruct,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToEventPayload(FCrowdyTypeID TypeID,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToActorUpdatePayload(const UScriptStruct* PayloadStruct,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToActorUpdatePayload(FCrowdyTypeID TypeID,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToAllPayloads(ECrowdyPayloadCategory Category,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	FCrowdySubscription SubscribeToOpcode(ECrowdyMessageType MessageType,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler);

	/** Binds the cast once here instead of repeating it in every handler. */
	template <typename TMessage>
	FCrowdySubscription SubscribeToOpcode(ECrowdyMessageType MessageType,
		const FCrowdySubscriptionOptions& Options,
		TFunction<void(const TMessage&, const FCrowdyDelivery&)> Handler)
	{
		return SubscribeToOpcode(MessageType, Options,
			[TypedHandler = MoveTemp(Handler)](const FCrowdyDelivery& Delivery)
			{
				TypedHandler(Delivery.GetAs<TMessage>(), Delivery);
			});
	}

	void DispatchMessage(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message);

	/** Per key: the resolved struct where one exists, the numeric id, and every subscriber by role. */
	void DumpRoutes(FOutputDevice& Ar) const;

	TArray<FCrowdyRoutingConflict> GetConflicts() const;
	FCrowdyRoutingStats GetStats() const;

	/** Prints the table of every registry alive in this process. Backs the crowdy.net.routes command. */
	static void DumpAllRoutes(FOutputDevice& Ar);

private:

	friend class FCrowdySubscription;

	using FRecordPtr = TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe>;
	using FTablePtr = TSharedPtr<const FCrowdyRoutingTable, ESPMode::ThreadSafe>;

	/** Called by a handle when it is released. Drops the record and republishes the table. */
	void OnSubscriptionReleased(const FRecordPtr& Record);

	/** Requires RecordsMutex. Builds a whole new table from Records and publishes it. */
	void RebuildTable();

	FTablePtr PinTable() const;

	/** Requires RecordsMutex. Reports anything about this subscription a developer needs to know. */
	bool ValidateSubscription(TConstArrayView<FCrowdySubscriptionKey> Keys,
		const FCrowdySubscriptionOptions& Options, bool bHasHandler);

	static int32 DeliverToList(const TArray<FRecordPtr>& List, const FCrowdyDelivery& Delivery);

	void ReportUnrouted(const FCrowdyDelivery& Delivery);
	void MaybeDumpFirstRoutes();

	/** Whether the message parser can ever produce this opcode, so a dead subscription is reported. */
	static bool IsOpcodeEverParsed(ECrowdyMessageType MessageType);

	/**
	 * Whether this opcode always carries a payload key and is therefore routed by that key, never
	 * by the opcode table, so an Opcode subscription naming it can never be matched.
	 */
	static bool IsOpcodeAlwaysPayloadRouted(ECrowdyMessageType MessageType);

	mutable FCriticalSection RecordsMutex;
	TArray<FRecordPtr> Records;
	TArray<FCrowdyRoutingConflict> Conflicts;
	uint32 NextGeneration = 1;

	mutable FRWLock TableLock;
	FTablePtr CurrentTable;

	TSharedPtr<FCrowdyRouterState, ESPMode::ThreadSafe> RouterState;

	/** How many distinct unrouted keys are ever recorded by name; the rest are still counted. */
	static constexpr int32 MaxTrackedUnroutedKeys = 64;

	std::atomic<uint64> DeliveredCount{ 0 };
	std::atomic<uint64> UnroutedCount{ 0 };
	std::atomic<bool> bDumpedFirstRoutes{ false };

	/** Guards only the two containers below, which a plain counter increment never touches. */
	mutable FCriticalSection StatsMutex;
	TSet<uint32> ReportedUnroutedKeys;
	TArray<FString> UnroutedKeyDescriptions;
	bool bUnroutedCapReported = false;
};
