#pragma once

#include "CoreMinimal.h"

/**
 * The address of the replication server this client should open its UDP socket against, plus the gate-keep flag.
 *
 * Gate-keep is not a failure: the server answered, but the app has no capacity for another player right now, so
 * there is no address to connect to and the caller should surface that to the player rather than retry blindly.
 */
struct FCrowdyUDPEndpoint
{
	bool bGateKeep = false;
	FString IPv6Address;
	FString IPv4Address;
	uint16 Port = 0;
};

class FJsonObject;

namespace CrowdyUDPEndpoint
{
	/**
	 * Read a replication server out of the data an assignment answer carries.
	 *
	 * A missing IPv6 address is survivable and leaves that field empty, since the connection opens over IPv4 unless
	 * IPv6 was chosen deliberately. A missing IPv4 address or client port is not, and is reported as the reason
	 * rather than left as a zero the caller would go on to connect to.
	 */
	CROWDYNET_API bool ParseServerAssignment(const TSharedPtr<FJsonObject>& Data, FCrowdyUDPEndpoint& OutEndpoint,
		FString& OutError);
}
