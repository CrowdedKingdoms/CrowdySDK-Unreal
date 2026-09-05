#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/core/bytes.hpp"

/// Base64 helpers. The GraphQL world APIs carry binary blobs (chunk voxels,
/// actor state, audio) as base64 strings; the native UDP path never uses
/// base64.
namespace crowdy::core {

std::string base64Encode(Bytes data);
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);

/// URL-safe, unpadded variant (RFC 4648 §5) — used by the PKCE portal flow.
std::string base64UrlEncode(Bytes data);

}  // namespace crowdy::core
