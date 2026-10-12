#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// Structured error classes for the GraphQL layer, mirroring CrowdyJS
/// (https://github.com/CrowdedKingdoms/CrowdyJS): branch on `code()` /
/// `extensions.code`, never on message text.
///
/// The replication (UDP) layer never throws — see crowdy/core/result.hpp.
namespace crowdy::graphql {

/// Base class for every SDK error.
class CrowdyError : public std::runtime_error {
 public:
  CrowdyError(std::string code, const std::string& message)
      : std::runtime_error(message), code_(std::move(code)) {}
  /// Stable machine-readable code (e.g. "UNAUTHENTICATED", "HTTP_ERROR").
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

/// Non-2xx HTTP response from a GraphQL endpoint.
class CrowdyHttpError : public CrowdyError {
 public:
  CrowdyHttpError(int status, std::string body)
      : CrowdyError("HTTP_ERROR", "GraphQL endpoint returned HTTP " + std::to_string(status)),
        status_(status),
        body_(std::move(body)) {}
  int status() const { return status_; }
  const std::string& body() const { return body_; }

 private:
  int status_;
  std::string body_;
};

/// extensions.code for "this app lives in another datacenter, go there".
inline constexpr std::string_view kWrongDatacenterCode = "WRONG_DATACENTER";
/// extensions.code for "this app's datacenter cannot serve it right now".
/// Carries NO endpoint, deliberately: there is nowhere to move to.
inline constexpr std::string_view kAppUnavailableCode = "APP_UNAVAILABLE";

/// One entry of a GraphQL `errors` array.
struct GraphQLErrorDetail {
  std::string message;
  std::string code;         ///< extensions.code (stable, enumerated)
  std::string remediation;  ///< extensions.remediation when provided
  std::string path;         ///< dotted path, e.g. "mintAppToken"

  // Datacenter-routing extensions. Present on WRONG_DATACENTER and
  // APP_UNAVAILABLE; empty elsewhere.
  std::string appId;
  std::string appDatacenter;  ///< where the app IS served
  std::string servedBy;       ///< the datacenter that refused
  std::string gameApiUrl;     ///< move target; only on WRONG_DATACENTER
  std::string gameApiWsUrl;
  /// extensions.retryable. Defaults TRUE to match the server contract, where
  /// only an explicit `false` means "do not bother trying again".
  bool retryable = true;
  /// extensions.blame: "PLATFORM" (ours, a retry is reasonable), "AUTHOR" (the
  /// app's own code, an identical call fails identically) or "BUDGET" (a spent
  /// allowance). Empty when the error carried no attribution, which is NOT the
  /// same as PLATFORM: pair it with `retryable` rather than reading either
  /// alone, since `retryable` defaults true and an unattributed transport
  /// failure must not read as a licensed retry.
  std::string blame;
  /// extensions.retryAfterMs: how long to wait before trying again, in
  /// milliseconds measured when the server built the refusal.
  ///
  /// OPTIONAL rather than an int defaulting to 0, and the `retryable` default
  /// above must NOT be copied here: "retry immediately" and "the server named
  /// no wait" are different instructions, and a zero standing for both would
  /// turn silence into a busy loop. Absent whenever the key is missing or is
  /// not a JSON number.
  ///
  /// On the invoke rate limit this is what REMAINS of a fixed window, not a
  /// fixed backoff, so a later refusal in the same window carries a smaller
  /// number. Treat it as a deadline from receipt, not as an interval to
  /// reuse.
  std::optional<std::int64_t> retryAfterMs;
  /// extensions.httpStatus: the HTTP status the server maps this code to, which a
  /// GraphQL error carries even when the response itself was a 200. Absent when the
  /// key is missing or not a JSON number.
  std::optional<int> httpStatus;
  /// extensions.cause: why an open circuit opened, when the server knows.
  /// `watchdog_timeout` means the failures were watchdog kills. Empty when
  /// the key is absent.
  std::string cause;
  /// extensions.ownedByCaller on ACTOR_EXISTS: true when the taken uuid is already the
  /// caller's actor (a retried create), false when another player holds it. Absent
  /// when the server did not say.
  std::optional<bool> ownedByCaller;
  /// extensions.suspendedUntil (ISO-8601) on ACCESS_SUSPENDED; empty elsewhere.
  std::string suspendedUntil;
  /// extensions.reason on APP_PAUSED (why the app's runtime gate is not ACTIVE);
  /// empty elsewhere.
  std::string reason;
};

/// extensions.code for creating an actor whose uuid is taken (see actorExistsOf()).
inline constexpr std::string_view kActorExistsCode = "ACTOR_EXISTS";
/// extensions.code for a player whose access to the app was revoked (HTTP 403).
inline constexpr std::string_view kAccessRevokedCode = "ACCESS_REVOKED";
/// extensions.code for a player suspended from the app until `suspendedUntil` (HTTP 403).
inline constexpr std::string_view kAccessSuspendedCode = "ACCESS_SUSPENDED";
/// extensions.code for a player with no access to a paid or invite-only app (HTTP 403).
inline constexpr std::string_view kAccessNotGrantedCode = "ACCESS_NOT_GRANTED";
/// extensions.code for an app whose runtime gate is not ACTIVE (HTTP 403).
inline constexpr std::string_view kAppPausedCode = "APP_PAUSED";

/// The first error with `code`, or nullptr.
inline const GraphQLErrorDetail* errorWithCode(const std::vector<GraphQLErrorDetail>& errors,
                                               std::string_view code) {
  for (const auto& error : errors) {
    if (error.code == code) return &error;
  }
  return nullptr;
}

/// An ACTOR_EXISTS refusal: `ownedByCaller` true means the actor is already yours (a
/// retried create; read it rather than failing), false that another player holds the
/// uuid, absent when the server did not say.
struct ActorExists {
  std::optional<bool> ownedByCaller;
};
inline std::optional<ActorExists> actorExistsOf(const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kActorExistsCode);
  if (!e) return std::nullopt;
  return ActorExists{e->ownedByCaller};
}

/// A refusal of the player's own access to an app: ACCESS_REVOKED, ACCESS_SUSPENDED
/// (with `suspendedUntil`, after which access returns by itself) or ACCESS_NOT_GRANTED.
/// None is fixed by retrying; tell the player.
struct AccessRefusal {
  std::string code;
  std::string suspendedUntil;
};
inline std::optional<AccessRefusal> accessRefusalOf(const std::vector<GraphQLErrorDetail>& errors) {
  for (std::string_view code : {kAccessSuspendedCode, kAccessRevokedCode, kAccessNotGrantedCode}) {
    if (const GraphQLErrorDetail* e = errorWithCode(errors, code)) {
      return AccessRefusal{std::string(code), e->suspendedUntil};
    }
  }
  return std::nullopt;
}

/// An APP_PAUSED refusal; `reason` is extensions.reason (empty when none). Show the
/// player that the world is paused instead of retrying in a loop.
struct AppPaused {
  std::string reason;
};
inline std::optional<AppPaused> appPausedOf(const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kAppPausedCode);
  if (!e) return std::nullopt;
  return AppPaused{e->reason};
}

/// The server returned GraphQL errors. Preserves every error including
/// extensions.
class CrowdyGraphQLError : public CrowdyError {
 public:
  explicit CrowdyGraphQLError(std::vector<GraphQLErrorDetail> errors)
      : CrowdyError(errors.empty() ? "GRAPHQL_ERROR" : firstCode(errors),
                    errors.empty() ? "GraphQL error" : errors.front().message),
        errors_(std::move(errors)) {}
  const std::vector<GraphQLErrorDetail>& errors() const { return errors_; }

 private:
  static std::string firstCode(const std::vector<GraphQLErrorDetail>& errors) {
    return errors.front().code.empty() ? "GRAPHQL_ERROR" : errors.front().code;
  }
  std::vector<GraphQLErrorDetail> errors_;
};

/// The app's own datacenter cannot serve it right now, and there is nowhere to
/// move to — so this is a typed error rather than a redirect. Distinct from
/// WRONG_DATACENTER, which the client handles silently by moving.
///
/// `message` is written for a player to read; show it rather than substituting
/// generic text, because the server knows why and the client does not.
class CrowdyAppUnavailableError : public CrowdyGraphQLError {
 public:
  explicit CrowdyAppUnavailableError(std::vector<GraphQLErrorDetail> errors)
      : CrowdyGraphQLError(std::move(errors)) {}

  const std::string& appId() const { return detail().appId; }
  const std::string& appDatacenter() const { return detail().appDatacenter; }
  const std::string& servedBy() const { return detail().servedBy; }
  /// False only when the server said so explicitly.
  bool retryable() const { return detail().retryable; }

 private:
  const GraphQLErrorDetail& detail() const {
    for (const auto& error : errors()) {
      if (error.code == kAppUnavailableCode) return error;
    }
    return errors().front();
  }
};

/// Network-level failure (DNS, TLS, connection refused).
class CrowdyNetworkError : public CrowdyError {
 public:
  explicit CrowdyNetworkError(const std::string& message)
      : CrowdyError("NETWORK_ERROR", message) {}
};

/// Request timed out.
class CrowdyTimeoutError : public CrowdyError {
 public:
  explicit CrowdyTimeoutError(const std::string& message)
      : CrowdyError("TIMEOUT", message) {}
};

/// Server response failed validation (not JSON, missing data, wrong shape).
class CrowdyProtocolError : public CrowdyError {
 public:
  explicit CrowdyProtocolError(const std::string& message)
      : CrowdyError("PROTOCOL_ERROR", message) {}
};

}  // namespace crowdy::graphql
