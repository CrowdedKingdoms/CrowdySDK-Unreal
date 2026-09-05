#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/graphql/json.hpp"

/// Typed results for the hot game-client paths. Long-tail / studio-admin
/// results are returned as graphql::Json documents; every field the server
/// sends is reachable.
///
/// GraphQL BigInt scalars (ids, chunk coordinates) cross the wire as decimal
/// strings — mirrored here as std::string to avoid silent precision bugs,
/// with int64 accessors where useful.
namespace crowdy::domains {

/// Nullable GraphQL string that preserves null versus an explicitly empty
/// string while retaining the common std::string conveniences used by older
/// callers (`empty()` and implicit read-only string access).
class NullableString {
 public:
  NullableString() = default;
  NullableString(std::nullopt_t) {}
  NullableString(std::string value) : value_(std::move(value)) {}
  NullableString(const char* value)
      : value_(value ? std::optional<std::string>(value) : std::nullopt) {}

  static NullableString fromJson(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    return NullableString(value.asString());
  }

  bool has_value() const { return value_.has_value(); }
  bool isNull() const { return !value_; }
  bool empty() const { return !value_ || value_->empty(); }
  const std::string* get() const {
    return value_ ? &*value_ : nullptr;
  }
  const std::string& valueOrEmpty() const {
    static const std::string empty;
    return value_ ? *value_ : empty;
  }
  std::string value_or(std::string fallback) const {
    return value_.value_or(std::move(fallback));
  }

  operator const std::string&() const { return valueOrEmpty(); }

  friend bool operator==(const NullableString& lhs,
                         std::string_view rhs) {
    return lhs.valueOrEmpty() == rhs;
  }
  friend bool operator==(std::string_view lhs,
                         const NullableString& rhs) {
    return rhs == lhs;
  }

 private:
  std::optional<std::string> value_;
};

/// Chunk address used by GraphQL world APIs (BigInt strings on the wire).
struct ChunkRef {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  graphql::JVal toInput() const {
    graphql::JVal v;
    v["x"] = std::to_string(x);
    v["y"] = std::to_string(y);
    v["z"] = std::to_string(z);
    return v;
  }
};

/// Result of portal.mintAppToken / exchangeCode / refresh.
struct AppTokenResponse {
  std::string token;        ///< 64-char app-scoped token (also the UDP HMAC key)
  std::string gameTokenId;  ///< decimal String scalar; never silently narrowed
  std::string appId;
  std::string expiresAt;    ///< ISO-8601; empty when non-expiring
  NullableString gameApiUrl;   ///< null when no route has been assigned
  NullableString gameApiWsUrl;
  /// The SHARED origin (multivalue DNS over every datacenter's balancer), which
  /// is why it survives the loss of the single instance gameApiUrl names. Feed
  /// it to ClientConfig::discoveryUrl so a dead endpoint can be re-discovered
  /// rather than retried. Null only when the server has no public URL.
  NullableString discoveryUrl;
  NullableString launchUrl;

  /// Checked migration helper for the native UDP int64 wire tail.
  std::optional<std::int64_t> gameTokenIdInt64() const {
    return graphql::parseBigInt(gameTokenId);
  }

  static AppTokenResponse fromJson(const graphql::Json& j) {
    AppTokenResponse r;
    r.token = j["token"].asString();
    r.gameTokenId = j["gameTokenId"].asBigIntString();
    r.appId = j["appId"].asString();
    r.expiresAt = j["expiresAt"].asString();
    r.gameApiUrl = NullableString::fromJson(j["gameApiUrl"]);
    r.gameApiWsUrl = NullableString::fromJson(j["gameApiWsUrl"]);
    r.discoveryUrl = NullableString::fromJson(j["discoveryUrl"]);
    r.launchUrl = NullableString::fromJson(j["launchUrl"]);
    return r;
  }
};

/// Result of the passwordless sign-in mutations.
struct AuthResponse {
  std::string token;  ///< identity SESSION token, not a gameplay token
  std::string gameTokenId;
  std::string userId;
  NullableString email;
  NullableString gamertag;

  static AuthResponse fromJson(const graphql::Json& j) {
    AuthResponse r;
    r.token = j["token"].asString();
    r.gameTokenId = j["gameTokenId"].asString();
    r.userId = j["user"]["userId"].asBigIntString(
        j["user"]["userId"].asStringView());
    r.email = NullableString::fromJson(j["user"]["email"]);
    r.gamertag = NullableString::fromJson(j["user"]["gamertag"]);
    return r;
  }
};

/// Decoded CLIENT artifact ready for a native sandbox/runtime. GraphQL BigInt
/// fuel remains a decimal string so native engines can choose their own width.
struct ClientArtifactBytes {
  std::vector<std::uint8_t> bytes;
  std::string artifactHash;
  std::string fuelPerDispatch;
  std::optional<std::string> contractJson;
  std::string versionId;
};

/// Decode the common playerCompute/marketplace artifact response shape.
inline std::optional<ClientArtifactBytes> decodeClientArtifactBytes(
    const graphql::Json& artifact) {
  if (!artifact.isObject() || !artifact["artifactBase64"].isString() ||
      !artifact["artifactHash"].isString() ||
      !artifact["clientFuelPerDispatch"].isString() ||
      !artifact["versionId"].isString()) {
    return std::nullopt;
  }
  auto bytes = core::base64Decode(artifact["artifactBase64"].asStringView());
  if (!bytes) return std::nullopt;

  ClientArtifactBytes decoded;
  decoded.bytes = std::move(*bytes);
  decoded.artifactHash = artifact["artifactHash"].asString();
  decoded.fuelPerDispatch =
      artifact["clientFuelPerDispatch"].asString();
  if (artifact["contractJson"].isString()) {
    decoded.contractJson = artifact["contractJson"].asString();
  }
  decoded.versionId = artifact["versionId"].asString();
  return decoded;
}

/// Result of serverStatus.serverWithLeastClients — the Buddy-server
/// assignment for native UDP (calling it also installs the UDP session
/// server-side).
struct ServerAssignment {
  std::string serverId;
  std::string ip4;
  std::string ip6;
  int clientPort = 0;
  std::string status;
  std::int64_t clients = 0;

  static ServerAssignment fromJson(const graphql::Json& j) {
    ServerAssignment a;
    a.serverId = j["serverId"].asString(j["id"].asString());
    a.ip4 = j["ip4"].asString();
    a.ip6 = j["ip6"].asString();
    a.clientPort = static_cast<int>(j["clientPort"].asInt64());
    a.status = j["status"].asString();
    a.clients = j["clients"].asInt64();
    return a;
  }
};

}  // namespace crowdy::domains
