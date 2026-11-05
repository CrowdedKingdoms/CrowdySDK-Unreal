#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "crowdy/client.hpp"
#include "crowdy/core/result.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/replication/connection.hpp"

/// Start a game session from an app token the game already holds.
///
/// Every way a player gets an app token ends here: portal().mintAppToken() on the identity
/// session, the portal's exchangeCode(), an organization's mintManagedPlayerToken handed to the
/// device, exchangeManagedPlayerAssertion(), and signInWithMinecraft(). startGameSession() does
/// what a game does after its own mint: it builds the per-game client on the app's datacenter
/// (the token's gameApiUrl, gameApiWsUrl and discoveryUrl), stores the token there, and opens the
/// native replication connection signed with it. The token then refreshes on that client as any
/// app token does (portal().refresh(), refreshGameplayToken(), the connection's proactive
/// refresh) until its sign-in ends.
///
///   auto token = identity.portal().signInWithMinecraft(input);
///   crowdy::GameSessionOptions options;
///   options.fallbackOrigin = identityOrigin;
///   crowdy::GameSession session = crowdy::startGameSession(token, std::move(options));
///   if (!session.ok()) { /* session.status says why */ }
///   crowdy::session::WorldSession world(session.connection, session.client.get(), config);
namespace crowdy {

/// The native-UDP token material in an app token: the 64 octets (the HMAC key), the
/// gameTokenId and the expiry in epoch milliseconds (0 when the token names none). `token` is
/// empty and `gameTokenId` 0 when the response is not a usable app token: empty, an org token,
/// the wrong length, or an id that is not a 64-bit integer.
replication::TokenInfo replicationTokenOf(const domains::AppTokenResponse& token);

/// The per-game client configuration for an app token: `base` (its transport, crypto, token
/// store, timeouts) with its origins replaced by the token's. httpUrl is gameApiUrl, wsUrl is
/// gameApiWsUrl (wsEndpoint is cleared), discoveryUrl is discoveryUrl. An origin the token leaves
/// null falls back to `fallbackOrigin` (httpUrl and discoveryUrl), then to what `base` had.
ClientConfig gameClientConfig(const domains::AppTokenResponse& token, ClientConfig base = {},
                              std::string_view fallbackOrigin = {});

/// How startGameSession() builds the session.
struct GameSessionOptions {
  /// The per-game client's base configuration; see gameClientConfig().
  ClientConfig client;
  /// Where to go when the token names no gameApiUrl or discoveryUrl (an older API, a local
  /// stack): usually the origin the token was obtained from. Empty leaves `client`'s.
  std::string fallbackOrigin;
  /// The replication connection's settings. appId and token are set from the app token.
  replication::Config replication;
  replication::Handlers handlers;
  /// Assignment attempts. Small deployments can answer "no available servers" for a moment
  /// while server heartbeats lapse; a value above 1 retries after connectRetryDelayMs.
  int connectAttempts = 1;
  int connectRetryDelayMs = 2000;
  /// Wait up to this long for the connection to report Connected (it does once the
  /// session-ready wait has passed). 0 returns once the server is assigned, while the state is
  /// still Connecting. With replication.manualPump the wait calls pump(); it never calls
  /// poll(), so no handler runs before startGameSession() returns.
  int connectTimeoutMs = 5000;
};

/// A started game session: the per-game client holding the app token, and the replication
/// connection signed with it. The connection uses the client to refresh its token and to move,
/// so keep the client alive as long as anything uses the connection. Destroying or reassigning
/// a session releases its connection before its client.
struct GameSession {
  GameSession() = default;
  GameSession(const GameSession&) = default;
  GameSession(GameSession&&) noexcept = default;
  GameSession& operator=(const GameSession& other) {
    if (this != &other) {
      connection.reset();
      client = other.client;
      connection = other.connection;
      token = other.token;
      status = other.status;
    }
    return *this;
  }
  GameSession& operator=(GameSession&& other) noexcept {
    if (this != &other) {
      connection.reset();
      client = std::move(other.client);
      connection = std::move(other.connection);
      token = std::move(other.token);
      status = other.status;
    }
    return *this;
  }
  ~GameSession() { connection.reset(); }

  std::shared_ptr<CrowdyClient> client;
  std::shared_ptr<replication::Connection> connection;
  /// The token the session started with. Its own refreshes rotate the client's (getToken()).
  domains::AppTokenResponse token;
  /// Ok once connected. InvalidArgument for a token that is not a usable app token (nothing is
  /// built); Timeout when the connection did not report Connected within connectTimeoutMs (the
  /// client and the connection are kept, and the connection may still get there); otherwise
  /// the assignment's or the socket's failure, with the client and the Failed connection kept
  /// so the caller can retry connection->connect().
  Status status = Errc::Rejected;

  bool ok() const { return status.ok() && client != nullptr && connection != nullptr; }
  /// The app was paused when the token was minted: the session stays, and the replication
  /// server refuses its sends (ErrorCode::AppPaused) until it runs again.
  bool paused() const { return domains::isAppPaused(token.runtimeGate); }
  /// The app id as the wire carries it; 0 when the token named none.
  std::int64_t appId() const;
};

/// Build the per-game client for `token` and open its replication connection. Blocking: the
/// server assignment is a GraphQL call, and by default it waits for the session to be ready.
/// Refuses (InvalidArgument, nothing built) an empty token, an org token (`cko_...`, an
/// organization's server credential, which a device must never hold), a token that is not 64
/// characters, and an appId or gameTokenId that is not a 64-bit integer.
GameSession startGameSession(const domains::AppTokenResponse& token,
                             GameSessionOptions options = {});

}  // namespace crowdy
