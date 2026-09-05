#pragma once

#include "crowdy/core/result.hpp"
#include "crowdy/replication/types.hpp"

namespace crowdy::replication {

/// Bridges the replication client to the GraphQL plane without a compile
/// dependency on it. CrowdyClient installs an implementation backed by
/// serverStatus().serverWithLeastClients() and portal().refresh(); tests can
/// stub it.
///
/// Implementations may block (they run HTTP requests) and are called from
/// the replication client's connect path (net thread in owned-thread mode).
class ISessionProvider {
 public:
  virtual ~ISessionProvider() = default;

  /// Assign (or re-assign) a replication server. Side effect on the Game
  /// API: installs this client's UDP session on the chosen server.
  virtual Result<Assignment> assignServer() = 0;

  /// Rotate the app-scoped token (refreshAppToken). Returns the new token
  /// material; the implementation must also update its GraphQL bearer.
  virtual Result<TokenInfo> refreshToken() = 0;
};

}  // namespace crowdy::replication
