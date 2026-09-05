#pragma once

#include <array>
#include <string>

#include "crowdy/core/crypto.hpp"

/// Actor UUIDs on the Replication API are client-generated 32-byte ASCII
/// strings (no null terminator on the wire) — 32 lowercase hex characters by
/// convention. They are NOT RFC-4122 UUIDs.
namespace crowdy::core {

using ActorUuid = std::array<char, 32>;

/// Generate a random 32-hex-char actor UUID with typed provider failure.
Result<ActorUuid> tryGenerateActorUuid(
    const ICrypto& crypto = defaultCrypto());

/// Migration-compatible convenience helper. Returns an all-zero UUID when the
/// selected provider is unavailable; security-sensitive callers should use
/// tryGenerateActorUuid() and branch on CryptoUnavailable.
ActorUuid generateActorUuid(const ICrypto& crypto = defaultCrypto());

inline std::string toString(const ActorUuid& u) { return std::string(u.data(), u.size()); }

/// Parse from a 32-character string. Returns false if the length is wrong.
bool actorUuidFromString(std::string_view s, ActorUuid& out);

}  // namespace crowdy::core
