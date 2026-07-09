#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h" // FProperty, FField::HasMetaData

/**
 * UPROPERTY meta-keys that declare a property as CrowdyState-replicated. CrowdyState is the fast,
 * client-authoritative view plane: a marked property's changed value is diffed by its owning client
 * and shipped to peers, where it is written back onto the live actor. The discovery scan (Phase 1)
 * and the Blueprint variable customization must emit these exact spellings; the layout builder reads them.
 *
 * The marker is deliberately "CrowdyState", NOT "CrowdyReplicate". That obvious spelling is already
 * taken: CrowdyRpcMetaKeys::LegacyReplicate == "CrowdyReplicate" is an alias of the RPC/Blueprint
 * replicate marker "CrowdyReplicates" (Replication/RPC/CrowdyRPC.h). Reusing it would make one key
 * mean two different systems and would make RPC discovery mistake a state property's owner for an
 * RPC. CrowdyState is a separate plane, so it uses a distinct key.
 */
namespace CrowdyStateMetaKeys
{
	// The marker: a UPROPERTY carrying meta=(CrowdyState) replicates on the CrowdyState view plane.
	// Distinct from the RPC "CrowdyReplicate"/"CrowdyReplicates" markers by the rationale above.
	inline const TCHAR* Replicate = TEXT("CrowdyState");

	// Delivery scope, not secrecy: the property is delivered only to the owning client (via the
	// targeted SINGLE_ACTOR_MESSAGE path in Phase 5), never on the spatial multicast.
	inline const TCHAR* OwnerOnly = TEXT("CrowdyOwnerOnly");

	// Skip the per-tick Identical diff; the value is pushed only when explicitly marked dirty
	// (Phase 5). Intended for big or expensive properties, the author does not want diffed every tick.
	inline const TCHAR* ManualDirty = TEXT("CrowdyManualDirty");

	// The metadata value is the name of a parameterless notify function invoked on the receiver
	// after the property is written. Analogous to a RepNotify, GAS-style: no previous value.
	inline const TCHAR* OnRep = TEXT("CrowdyOnRep");

	// Opt-in: include this property in the periodic keyframe heartbeat, the redundant full re-send every
	// StateKeyframeIntervalSeconds that lets a late or packet-loss-desynced observer converge without
	// waiting for the next change. WITHOUT this key a property still replicates on change every tick, it
	// just is never re-sent while unchanged. Default off in C++ (add the key to opt a property in); the
	// Blueprint "Crowdy Replication" dropdown defaults its Heartbeat toggle On. Durable/late-join state
	// belongs in Game Models, so most transient view properties leave this off.
	inline const TCHAR* Heartbeat = TEXT("CrowdyHeartbeat");

	/**
	 * True when Property carries the CrowdyState marker. Defined here rather than borrowed from
	 * CrowdyRpcMetaKeys::HasReplicatesMeta, which takes a UField*: an FProperty is an FField, not a
	 * UField, so that helper cannot accept it. Cooked builds strip UPROPERTY metadata
	 * (WITH_METADATA==0) and read the baked rep table instead, so this live-metadata helper
	 * is the editor/discovery path only and returns false without metadata.
	 */
	inline bool HasStateMeta(const FProperty* Property)
	{
#if WITH_METADATA
		return Property && Property->HasMetaData(Replicate);
#else
		(void)Property;
		return false;
#endif
	}
}
