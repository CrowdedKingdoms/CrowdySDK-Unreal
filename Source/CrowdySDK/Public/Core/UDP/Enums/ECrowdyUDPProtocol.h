#pragma once
#include "CoreMinimal.h"
#include "ECrowdyUDPProtocol.generated.h"

/**
 * Controls which IP protocol stack the UDP subsystem will attempt when
 * connecting to the game server.
 *
 *  Auto — tries IPv6 first; if the socket initialisation fails it falls back
 *         to IPv4 automatically. Use this unless you have a specific reason to
 *         force one protocol.
 *  IPv4 — always connects over IPv4. Useful on networks or platforms that do
 *         not support IPv6.
 *  IPv6 — always connects over IPv6. Fails hard if the address is not reachable
 *         over IPv6 (no fallback).
 *
 * Set this in Project Settings → Plugins → Crowdy SDK → Network.
 */
UENUM(BlueprintType)
enum class ECrowdyUDPProtocol : uint8
{
	Auto UMETA(DisplayName = "Auto (IPv6 → IPv4 fallback)"),
	IPv4 UMETA(DisplayName = "Force IPv4"),
	IPv6 UMETA(DisplayName = "Force IPv6"),
};
