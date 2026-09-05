#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// Where each app lives — the call a client makes BEFORE it authenticates.
///
/// WHY IT COMES FIRST. `ck.<tier>.crowdedkingdoms.com` is a multivalue DNS record
/// over every datacenter's load balancer, so a cold client's first request lands
/// wherever DNS pointed it — and roughly half the time that is not the
/// datacenter hosting the app. Authenticating there writes the session in the
/// wrong place and then mints the app token across a WAN.
///
/// A client already knows its app id before it holds any credential: it is a
/// build-time constant. So it can ask this, move to the returned origin, and log
/// in locally. That is the whole trick, and it is why this query takes no token.
///
/// PLURAL. A launcher or in-game switcher resolves every game it might offer in
/// one call and caches the answer, so switching games is instant rather than a
/// round trip at click time. Placement changes rarely and only an operator can
/// change it.
///
/// An app with no placement comes back with empty endpoints, which means "stay
/// on the shared origin" — not "this app is broken".
namespace crowdy::domains {

/// One app's placement.
struct AppEndpoint {
  std::string appId;          ///< decimal string
  std::string datacenterCode; ///< e.g. "or" / "va"; empty when unplaced
  std::string gameApiUrl;     ///< empty when unplaced
  std::string gameApiWsUrl;

  /// True when this entry names somewhere to go.
  bool placed() const { return !gameApiUrl.empty(); }
};

class DiscoveryAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Resolve where one or more apps are placed. No authentication required.
  ///
  /// Call this against the SHARED origin — the name every datacenter answers,
  /// and the only one a client can rely on before it knows where it belongs.
  std::vector<AppEndpoint> apps(const std::vector<std::string>& appIds) const {
    graphql::JVal vars;
    vars["appIds"] = idsArray(appIds);
    return parse(execUnwrap(gen::apps::kAppDiscoveryIsolatedDocument, vars,
                            gen::apps::kAppDiscoveryOperationName));
  }

  void appsAsync(const std::vector<std::string>& appIds,
                 std::function<void(graphql::GraphQLOutcome,
                                    std::vector<AppEndpoint>)> cb) const {
    graphql::JVal vars;
    vars["appIds"] = idsArray(appIds);
    execUnwrapAsync(gen::apps::kAppDiscoveryIsolatedDocument, vars,
                    gen::apps::kAppDiscoveryOperationName,
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      std::vector<AppEndpoint> endpoints;
                      if (out.ok()) endpoints = parse(out.data);
                      cb(std::move(out), std::move(endpoints));
                    });
  }

  /// Resolve one app. `placed()` is false when it has no placement, which is a
  /// legitimate answer rather than an error.
  ///
  /// Prefer apps() when you have more than one: it is one round trip, not N.
  AppEndpoint app(const std::string& appId) const {
    auto entries = apps({appId});
    return entries.empty() ? AppEndpoint{} : entries.front();
  }

 private:
  static graphql::JVal idsArray(const std::vector<std::string>& appIds) {
    graphql::JArray ids;
    ids.reserve(appIds.size());
    for (const auto& id : appIds) ids.emplace_back(id);
    return graphql::JVal(std::move(ids));
  }

  static std::vector<AppEndpoint> parse(const graphql::Json& value) {
    std::vector<AppEndpoint> endpoints;
    value.forEach([&endpoints](graphql::Json entry) {
      AppEndpoint endpoint;
      endpoint.appId = entry["appId"].asString();
      endpoint.datacenterCode = entry["datacenterCode"].asString();
      endpoint.gameApiUrl = entry["gameApiUrl"].asString();
      endpoint.gameApiWsUrl = entry["gameApiWsUrl"].asString();
      endpoints.push_back(std::move(endpoint));
    });
    return endpoints;
  }
};

}  // namespace crowdy::domains
