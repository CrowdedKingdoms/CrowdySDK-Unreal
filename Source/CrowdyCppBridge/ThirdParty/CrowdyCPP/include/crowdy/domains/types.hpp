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

/// An app's runtime gate (ck-api AppRuntimeGateInfo): `status` is "ACTIVE" when the
/// app runs; anything else ("GRACE", "DENIED", "SUSPENDED", or a later value) means it
/// is paused, and `reason` says why (null when the server gives none).
struct AppRuntimeGate {
  std::string status;
  NullableString reason;

  static AppRuntimeGate fromJson(const graphql::Json& j) {
    AppRuntimeGate g;
    g.status = j["status"].asString();
    g.reason = NullableString::fromJson(j["reason"]);
    return g;
  }
};

/// Whether a runtime gate (AppTokenResponse::runtimeGate, gameClientBootstrap's
/// `runtimeGate`) says the app is paused: any status but ACTIVE. No gate reads as not
/// paused.
inline bool isAppPaused(const std::optional<AppRuntimeGate>& gate) {
  return gate.has_value() && gate->status != "ACTIVE";
}

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
  /// The app's runtime gate when the token was minted (ck-api v2.39.0+). A paused app
  /// still mints, so check isAppPaused(runtimeGate) before entering the world; the
  /// replication server refuses a paused app's sends with ErrorCode::AppPaused (33).
  std::optional<AppRuntimeGate> runtimeGate;
  /// Set only by `refreshAppToken(currentServer)`: the replication server the NEW
  /// token was just authorized on -- the one the client is already connected to.
  /// Empty ip4 / 0 port when the server did not (or could not) authorize there,
  /// in which case the client must re-assign with serverWithLeastClients.
  std::string authorizedServerIp4;
  int authorizedServerClientPort = 0;
  bool hasAuthorizedServer() const {
    return !authorizedServerIp4.empty() && authorizedServerClientPort > 0;
  }

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
    if (j["runtimeGate"].isObject()) r.runtimeGate = AppRuntimeGate::fromJson(j["runtimeGate"]);
    const auto& srv = j["authorizedServer"];
    if (srv.isObject()) {
      r.authorizedServerIp4 = srv["ip4"].asString();
      r.authorizedServerClientPort = static_cast<int>(srv["clientPort"].asInt64());
    }
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

/// A document players (or developers) accept: playerLegalDocuments, appRequiredDocuments,
/// developerLegalDocuments. `appId` is set for an app's own document and null for the
/// platform's. Show the title and link, then record the player's acceptance of exactly these
/// versions (AuthAPI::recordPlayerConsents, PortalAPI::signInWithMinecraft).
struct LegalDocument {
  std::string documentId;
  std::string version;
  std::string title;
  std::string url;
  NullableString appId;

  static LegalDocument fromJson(const graphql::Json& j) {
    LegalDocument d;
    d.documentId = j["documentId"].asString();
    d.version = j["version"].asString();
    d.title = j["title"].asString();
    d.url = j["url"].asString();
    if (j["appId"].ok() && !j["appId"].isNull()) d.appId = j["appId"].asBigIntString();
    return d;
  }

  static std::vector<LegalDocument> listFromJson(const graphql::Json& j) {
    std::vector<LegalDocument> out;
    j.forEach([&](graphql::Json d) { out.push_back(fromJson(d)); });
    return out;
  }
};

/// A country, or a region of one, that Crowded Kingdoms lists (territoryPolicies): `status`
/// "blocked" refuses its players with TERRITORY_NOT_SUPPORTED, and `minAge` raises the
/// youngest age admitted there. A country not listed is open at the platform's minimum. A
/// country with a region listed is checked by region (ck-api v2.61.0): a connection from it
/// whose region cannot be told is refused, and so is an organization's player there sent
/// without a region.
struct TerritoryPolicy {
  std::string countryCode;    ///< ISO 3166-1 alpha-2
  NullableString regionCode;  ///< ISO 3166-2 (such as UA-43); null on the country's own row
  std::string status;         ///< "open" or "blocked"
  int minAge = 13;

  bool blocked() const { return status == "blocked"; }

  static TerritoryPolicy fromJson(const graphql::Json& j) {
    TerritoryPolicy p;
    p.countryCode = j["countryCode"].asString();
    p.regionCode = NullableString::fromJson(j["regionCode"]);
    p.status = j["status"].asString();
    p.minAge = static_cast<int>(j["minAge"].asInt64(13));
    return p;
  }
};

/// A one-time server id for PortalAPI::signInWithMinecraft (minecraftChallenge): 40 hex
/// characters, good once, for one app, for two minutes.
struct MinecraftChallenge {
  std::string serverId;
  std::string expiresAt;  ///< ISO-8601

  static MinecraftChallenge fromJson(const graphql::Json& j) {
    MinecraftChallenge c;
    c.serverId = j["serverId"].asString();
    c.expiresAt = j["expiresAt"].asString();
    return c;
  }
};

/// What an app's safety log records now (App.recording, ck-api v2.60.0): `enabled` (the log is
/// on: text chat and direct messages are recorded), `voice` (voice and channel audio) and
/// `video` (webcam video). While `voice` or `video` is true, show players a notice before they
/// first speak or appear on camera, and an indicator while it stays true.
struct AppRecording {
  bool enabled = false;
  bool voice = false;
  bool video = false;

  static AppRecording fromJson(const graphql::Json& j) {
    AppRecording r;
    r.enabled = j["enabled"].asBool();
    r.voice = j["voice"].asBool();
    r.video = j["video"].asBool();
    return r;
  }
};

/// The account after AuthAPI::recordPlayerAge. `ageBracket` is one of "unknown", "under_13",
/// "13_15", "16_17" and "18_plus"; use it for the game's logic, never as text a player reads.
struct PlayerAge {
  std::string userId;
  NullableString ageBracket;
  /// The declared country (ISO 3166-1 alpha-2).
  NullableString country;

  static PlayerAge fromJson(const graphql::Json& j) {
    PlayerAge a;
    a.userId = j["userId"].asBigIntString();
    a.ageBracket = NullableString::fromJson(j["ageBracket"]);
    a.country = NullableString::fromJson(j["country"]);
    return a;
  }
};

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
