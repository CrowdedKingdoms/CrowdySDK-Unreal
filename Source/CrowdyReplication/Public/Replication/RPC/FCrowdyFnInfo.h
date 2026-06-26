#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"                 // FCrowdyClassID, CROWDY_INVALID_CLASS_ID
#include "Core/UDP/Enums/ECrowdyMessageType.h" // ECrowdyEventRecipient, ECrowdyDecayRate, ECrowdyReplicationDistance

/**
 * Per-function routing and serialization metadata, resolved once per receiver
 * UFunction and cached by declaring class.
 *
 * Phase 1 populates FunctionID and bParamsPOD. The routing fields default to the
 * same values the SendCrowdyEvent API defaults to and are filled from baked
 * metadata in phase 2.
 */
struct FCrowdyFnInfo
{
	int64 FunctionID = 0;
	ECrowdyEventRecipient Recipient = ECrowdyEventRecipient::SpatialMulticast;
	ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay;
	ECrowdyReplicationDistance Distance = ECrowdyReplicationDistance::Eight_Chunks;

	// For a Multicast: the channel it routes over, by name. Empty = the app-wide default session
	// channel. The runtime resolves the name to a joined channel id at send time.
	FString ChannelName;

	// True when every input parameter is plain-old-data, so the parameter frame can
	// skip InitializeStruct/DestroyStruct.
	bool bParamsPOD = false;

	// Declaring class's Crowdy id (UCrowdyClassRegistry::GetID). Bookkeeping for the editor's
	// incremental rescan: a recompiled class's stale entries are evicted from the registry by id,
	// which never dereferences the now-invalid UFunction key. Set during the scan; unread at runtime.
	FCrowdyClassID OwnerClassID = CROWDY_INVALID_CLASS_ID;
};
