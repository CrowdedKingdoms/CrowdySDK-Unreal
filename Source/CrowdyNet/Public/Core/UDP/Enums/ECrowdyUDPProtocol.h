#pragma once
#include "CoreMinimal.h"
#include "ECrowdyUDPProtocol.generated.h"

/**
 * Controls which IP protocol stack the connection uses to reach the game server.
 *
 * The connection commits to one address family when it opens and does not fall back to the other, so there is no
 * setting that tries both.
 *
 *  Auto - takes the assignment's IPv4 address, which is the more reachable of the two.
 *  IPv4 - same as Auto. Kept distinct so a project can record that IPv4 was chosen deliberately.
 *  IPv6 - takes the assignment's IPv6 address, and does not come up if that address is unreachable.
 *
 * Set this from the CrowdyStudio console (Project page, Connection section).
 */
UENUM(BlueprintType)
enum class ECrowdyUDPProtocol : uint8
{
	Auto UMETA(DisplayName = "Auto (IPv4)"),
	IPv4 UMETA(DisplayName = "Force IPv4"),
	IPv6 UMETA(DisplayName = "Force IPv6"),
};
