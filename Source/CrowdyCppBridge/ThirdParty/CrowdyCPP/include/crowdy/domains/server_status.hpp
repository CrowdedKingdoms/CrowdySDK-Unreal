#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// client.serverStatus() — server discovery + capability bootstrap. Targets
/// the Game API with the APP-SCOPED token.
namespace crowdy::domains {

class ServerStatusAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Assign a replication server. NOT a passive lookup: as a side effect the
  /// Game API registers this client's UDP session with the chosen server. If
  /// this query errors, no session exists and every datagram is silently
  /// dropped. Wait ~1.5 s after this call before the first UDP send. See
  /// https://docs.crowdedkingdoms.com/replication-api/authenticate-and-assign.
  ServerAssignment serverWithLeastClients() const {
    return ServerAssignment::fromJson(
        execUnwrap(gen::serverStatus::kServerWithLeastClientsDocument));
  }

  /// Async twin of serverWithLeastClients(). On success `assignment` is parsed;
  /// on failure inspect `outcome`. Mirrors the pattern every domain method
  /// follows to gain an async overload over the shared exec helpers.
  void serverWithLeastClientsAsync(
      std::function<void(graphql::GraphQLOutcome outcome, ServerAssignment assignment)> cb) const {
    execUnwrapAsync(gen::serverStatus::kServerWithLeastClientsDocument, graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      ServerAssignment assignment;
                      if (out.ok()) assignment = ServerAssignment::fromJson(out.data);
                      cb(std::move(out), assignment);
                    });
  }

  graphql::Json listAll() const {
    return execUnwrap(gen::serverStatus::kGraphqlServersDocument);
  }

  void listAllAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::serverStatus::kGraphqlServersDocument, graphql::JVal(), {}, std::move(cb));
  }

  graphql::Json listActiveGraphqlServers() const {
    return execUnwrap(gen::serverStatus::kActiveGraphQLServersDocument);
  }

  void listActiveGraphqlServersAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::serverStatus::kActiveGraphQLServersDocument, graphql::JVal(), {},
                    std::move(cb));
  }

  graphql::Json versionInfo() const {
    return execUnwrap(gen::serverStatus::kVersionInfoDocument);
  }

  void versionInfoAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::serverStatus::kVersionInfoDocument, graphql::JVal(), {}, std::move(cb));
  }

  /// Per-app bootstrap: version info, spatial limits (max distance/decay,
  /// sequence modulo), and profile — call once when entering an app.
  graphql::Json gameClientBootstrap(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::serverStatus::kGameClientBootstrapDocument, vars);
  }

  void gameClientBootstrapAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::serverStatus::kGameClientBootstrapDocument, vars, {}, std::move(cb));
  }
};

}  // namespace crowdy::domains
