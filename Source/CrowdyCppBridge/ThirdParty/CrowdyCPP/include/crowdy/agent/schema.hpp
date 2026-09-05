#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "crowdy/agent/errors.hpp"
#include "crowdy/graphql/json.hpp"

namespace crowdy::agent {

inline constexpr std::array<std::string_view, 24>
    kForbiddenAgentAuthorityFields = {
        "userId",          "ownerUserId", "appId",       "projectId",
        "gridId",          "sessionId",   "runId",       "toolCallId",
        "clientEpoch",     "leaseId",     "lease",       "approval",
        "approvalGrant",   "argumentHash", "descriptorDigest",
        "idempotencyKey",  "deadline",    "token",       "authorization",
        "headers",         "endpoint",    "url",         "permissions",
        "authority",
};

enum class AgentSchemaDirection { Input, Output, Schema };

struct AgentSchemaValidationOptions {
  AgentSchemaDirection direction = AgentSchemaDirection::Input;
  std::size_t maxDepth = 12;
  std::size_t maxNodes = 4'096;
  std::size_t maxBytes = 1'048'576;
};

/// Canonical JSON used by CrowdyJS descriptor hashing. Object keys are sorted,
/// arrays retain order, and only finite JSON values are accepted.
std::string canonicalJson(const graphql::Json& value);
std::string sha256Digest(std::string_view value);
std::string digestCanonicalJson(const graphql::Json& value);
bool isSha256Digest(std::string_view value);

/// Validate the bounded JSON-schema vocabulary used by all 28 canonical Game
/// API tools. Unsupported or unbounded keywords fail closed.
void assertBoundedJsonSchema(const graphql::Json& schema,
                             bool rejectAuthorityFields = true);

/// Strict value validation: unknown object fields, unsafe numbers, oversized
/// arrays/strings, and malformed formats/patterns are rejected.
void validateJsonSchemaValue(
    const graphql::Json& schema, const graphql::Json& value,
    AgentSchemaValidationOptions options = {});

bool isDecimalString(std::string_view value, bool allowNegative = true);

}  // namespace crowdy::agent
