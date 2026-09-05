#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/json.hpp"

/// Internal to the GraphQL library; not an installed header.
namespace crowdy::graphql::detail {

/// Read one entry of a GraphQL `errors` array into a GraphQLErrorDetail.
///
/// ONE function because there are two callers — the HTTP client and the
/// subscription client — and they had drifted: the subscription path never read
/// `blame`, so the same refusal arriving over the websocket lost its
/// attribution. Two copies of a parser is two contracts, and the second one is
/// always the one nobody notices going stale.
///
/// `fallbackMessage` is the only thing the two callers legitimately differ on.
inline GraphQLErrorDetail readGraphQLError(Json entry, std::string_view fallbackMessage) {
  GraphQLErrorDetail detail;
  detail.message = entry["message"].asString(fallbackMessage);

  const Json extensions = entry["extensions"];
  detail.code = extensions["code"].asString();
  detail.remediation = extensions["remediation"].asString();
  detail.appId = extensions["appId"].asString();
  detail.appDatacenter = extensions["appDatacenter"].asString();
  detail.servedBy = extensions["servedBy"].asString();
  detail.gameApiUrl = extensions["gameApiUrl"].asString();
  detail.gameApiWsUrl = extensions["gameApiWsUrl"].asString();
  detail.retryable = !extensions["retryable"].isBool() || extensions["retryable"].asBool();
  detail.blame = extensions["blame"].asString();
  // A quarantined function or automation names itself and the finding that stopped it.
  // Read here for the same reason `blame` is: the alternative is every caller digging the
  // fields out of a raw extensions bag, and `quarantineReason` is the ONLY actionable
  // value in the refusal -- without it a developer is told their object is quarantined and
  // left to guess which of their lint errors did it.
  //
  // NOTE the code is NOT always OBJECT_QUARANTINED. On `gameModelInvoke` the server
  // rebuilds the error from a { code, blame, retryable } triple, so the code arrives as
  // USER_CODE_ERROR while these three survive. Keying off the code alone misses the
  // refusal on the one path a player takes.
  detail.quarantinedKind = extensions["quarantinedKind"].asString();
  detail.quarantinedName = extensions["quarantinedName"].asString();
  detail.quarantineReason = extensions["quarantineReason"].asString();
  // Guarded by isNumber() rather than taking asInt64's fallback: the fallback
  // would make an absent key indistinguishable from an explicit 0, which is the
  // one distinction this field exists to preserve.
  const Json retryAfterMs = extensions["retryAfterMs"];
  if (retryAfterMs.isNumber()) detail.retryAfterMs = retryAfterMs.asInt64();

  const Json path = entry["path"];
  if (path.isArray()) {
    path.forEach([&detail](Json segment) {
      if (!detail.path.empty()) detail.path += '.';
      detail.path += segment.isString() ? segment.asString()
                                        : std::to_string(segment.asInt64());
    });
  }
  return detail;
}

}  // namespace crowdy::graphql::detail
