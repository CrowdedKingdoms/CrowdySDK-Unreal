#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

namespace crowdy::domains {

/// Exact GraphQL surface for `crowdy.studio-agent/1` after the orchestrator
/// retirement: app policy, sanitized usage, provider-data consent, the metered
/// model usage read model, and operator controls. All go to the one API
/// origin, gated by permission. The agent's session/run/lease/tool operations
/// are gone from the API (the agent runs in the player's browser against the
/// REST `/v1/model` endpoint), so this class no longer has them. It does not
/// expose an arbitrary document executor, provider client, UDP authority, or
/// tool bridge.
class CrowdyStudioAgentAPI {
 public:
  CrowdyStudioAgentAPI(std::shared_ptr<graphql::GraphQLClient> api,
                       std::shared_ptr<graphql::Dispatcher> dispatcher)
      : api_(std::move(api)), dispatcher_(std::move(dispatcher)) {}

  std::shared_ptr<graphql::Dispatcher> dispatcher() const { return dispatcher_; }

  // Provider-data consent and metered model usage: the GraphQL companions of
  // the REST endpoint the in-browser agent spends tokens through.
  graphql::Json providerConsent(std::string_view appId) const {
    return run("CrowdyStudioProviderConsent", one("appId", appId));
  }
  void providerConsentAsync(std::string_view appId,
                            graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioProviderConsent", one("appId", appId),
             std::move(cb));
  }
  graphql::Json setProviderConsent(const graphql::JVal& input) const {
    return runInput("CrowdyStudioSetProviderConsent", input);
  }
  void setProviderConsentAsync(const graphql::JVal& input,
                               graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioSetProviderConsent", input, std::move(cb));
  }
  graphql::Json modelUsage(std::string_view appId,
                           std::optional<int> limit = std::nullopt) const {
    auto vars = one("appId", appId);
    if (limit) vars["limit"] = std::int64_t{*limit};
    return run("CrowdyStudioModelUsage", vars);
  }
  void modelUsageAsync(std::string_view appId, std::optional<int> limit,
                       graphql::GraphQLCallback cb) const {
    auto vars = one("appId", appId);
    if (limit) vars["limit"] = std::int64_t{*limit};
    runAsync("CrowdyStudioModelUsage", vars, std::move(cb));
  }

  // App policy and sanitized usage.
  graphql::Json policy(std::string_view appId) const {
    return run("CrowdyStudioAgentPolicy", one("appId", appId));
  }
  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentPolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json effectivePolicy(std::string_view appId) const {
    return run("CrowdyStudioAgentEffectivePolicy", one("appId", appId));
  }
  void effectivePolicyAsync(std::string_view appId,
                            graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentEffectivePolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json usage(std::string_view appId,
                      const graphql::JVal& window = graphql::JVal()) const {
    auto vars = window;
    vars["appId"] = appId;
    return run("CrowdyStudioAgentUsage", vars);
  }
  void usageAsync(std::string_view appId, const graphql::JVal& window,
                  graphql::GraphQLCallback cb) const {
    auto vars = window;
    vars["appId"] = appId;
    runAsync("CrowdyStudioAgentUsage", vars, std::move(cb));
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentSetPolicy", input, std::move(cb));
  }

  // Operator roots.
  graphql::Json platformPolicy() const {
    return run("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal());
  }
  void platformPolicyAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal(),
                    std::move(cb));
  }
  graphql::Json setPlatformPolicy(const graphql::JVal& input) const {
    return runInput("CpSetCrowdyStudioAgentPlatformPolicy", input);
  }
  void setPlatformPolicyAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    runInputAsync("CpSetCrowdyStudioAgentPlatformPolicy", input,
                         std::move(cb));
  }
  graphql::Json setOperatorAppKill(const graphql::JVal& input) const {
    return runInput("CpSetCrowdyStudioAgentAppKill", input);
  }
  void setOperatorAppKillAsync(const graphql::JVal& input,
                               graphql::GraphQLCallback cb) const {
    runInputAsync("CpSetCrowdyStudioAgentAppKill", input, std::move(cb));
  }

 private:
  static graphql::JVal one(std::string_view key, std::string_view value) {
    graphql::JVal vars;
    vars[key] = value;
    return vars;
  }
  static graphql::JVal inputVars(const graphql::JVal& input) {
    graphql::JVal vars;
    vars["input"] = input;
    return vars;
  }
  graphql::Json run(std::string_view operation,
                     const graphql::JVal& vars) const {
    auto data = api_->request(gen::crowdyStudioAgent::documentFor(operation),
                               vars, operation);
    graphql::Json result;
    data.forEachMember([&](std::string_view, graphql::Json value) {
      result = value;
    });
    return result;
  }
  void runAsync(std::string_view operation, const graphql::JVal& vars,
                 graphql::GraphQLCallback cb) const {
    unwrapAsync(api_, gen::crowdyStudioAgent::documentFor(operation),
                operation, vars, std::move(cb));
  }
  graphql::Json runInput(std::string_view operation,
                          const graphql::JVal& input) const {
    return run(operation, inputVars(input));
  }
  void runInputAsync(std::string_view operation, const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runAsync(operation, inputVars(input), std::move(cb));
  }
  static void unwrapAsync(
      const std::shared_ptr<graphql::GraphQLClient>& client,
      std::string_view document, std::string_view operation,
      const graphql::JVal& vars, graphql::GraphQLCallback cb) {
    client->requestAsync(
        document, vars, operation,
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          if (outcome.ok() && outcome.data.isObject() &&
              outcome.data.size() == 1) {
            graphql::Json result;
            outcome.data.forEachMember(
                [&](std::string_view, graphql::Json value) { result = value; });
            outcome.data = result;
          }
          cb(std::move(outcome));
        });
  }

  std::shared_ptr<graphql::GraphQLClient> api_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
};

}  // namespace crowdy::domains
