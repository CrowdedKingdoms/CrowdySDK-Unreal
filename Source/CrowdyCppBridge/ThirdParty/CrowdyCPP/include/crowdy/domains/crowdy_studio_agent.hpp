#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

namespace crowdy::domains {

/// The `action` `crowdyStudioLiveModding` checks: the agent's draft test or its
/// live deploy.
enum class CrowdyStudioLiveModdingAction { DraftTest, DeployLive };

inline constexpr std::string_view toString(CrowdyStudioLiveModdingAction action) {
  return action == CrowdyStudioLiveModdingAction::DraftTest ? "draft_test"
                                                            : "deploy_live";
}

/// A model's rate card, in micro-USD (1 USD = 1,000,000) per million tokens.
struct CrowdyStudioModelPricing {
  std::int64_t input = 0;
  std::int64_t output = 0;
  std::int64_t reasoning = 0;
  std::int64_t cachedInput = 0;
};

/// One model of `GET /v1/model/models`: the app policy's allowlist intersected with
/// the models that carry an active rate card, in the platform's order.
struct CrowdyStudioModel {
  std::string id;
  /// May equal `id`.
  std::string name;
  /// Unset when the platform sends null.
  std::optional<std::int64_t> contextWindow;
  std::vector<std::string> inputModalities;
  CrowdyStudioModelPricing pricingMicrousdPerMillion;
};

/// The endpoint's OpenAI-style refusal, `{"error":{"message","type","code","retryable"}}`.
struct CrowdyStudioModelError {
  std::string code;
  std::string message;
  /// `crowded_kingdoms_error` for the platform's own codes.
  std::string type;
  /// The body's `retryable`; when it sends none, true only for a 5xx.
  bool retryable = false;
};

/// The model list, or why there is none. `status` is Ok with `models`; `Rejected` with
/// `error` when the API refused (`httpStatus` says how); otherwise the transport's
/// failure (`Timeout`, `Cancelled`, `ResponseTooLarge`, `SocketError`) or `Malformed`,
/// with `errorMessage`.
struct CrowdyStudioModelList {
  Status status;
  int httpStatus = 0;
  std::vector<CrowdyStudioModel> models;
  std::optional<CrowdyStudioModelError> error;
  std::string errorMessage;

  bool ok() const { return status.ok(); }
};

/// The most models a list keeps; an app's allowlist is far shorter.
inline constexpr std::size_t kCrowdyStudioModelListMax = 1024;

/// Read a `GET /v1/model/models` answer, for an engine that sends it on its own stack.
CrowdyStudioModelList crowdyStudioModelListFrom(const graphql::HttpOutcome& http);

/// Exact GraphQL surface for `crowdy.studio-agent/1` after the orchestrator
/// retirement: app policy, sanitized usage, provider-data consent, the metered
/// model usage read model, the live-modding check, and the REST model list. All
/// go to the one API origin, gated by permission. The agent's
/// session/run/lease/tool operations are gone from the API (the agent runs in a
/// Studio dock, the browser's or a game's, against the REST `/v1/model`
/// endpoint), so this class no longer has them. It does not expose an arbitrary
/// document executor, provider client, UDP authority, or tool bridge.
class CrowdyStudioAgentAPI {
 public:
  CrowdyStudioAgentAPI(std::shared_ptr<graphql::GraphQLClient> api,
                       std::shared_ptr<graphql::Dispatcher> dispatcher)
      : api_(std::move(api)), dispatcher_(std::move(dispatcher)) {}

  std::shared_ptr<graphql::Dispatcher> dispatcher() const { return dispatcher_; }

  // The app's Allow Live Modding switch. Only this call checks it (the mod
  // build, deploy and switch-on calls do not), so a dock asks it before the
  // agent's draft test or live deploy. True when the player may; a refusal is
  // a GraphQL error whose code says why (AGENT_SCOPE_DENIED,
  // AGENT_PERMISSION_DENIED, AGENT_DISABLED, AGENT_OPERATOR_KILLED,
  // AGENT_PROVIDER_POLICY_UNSATISFIED). Async: `outcome.data` is the Boolean.
  bool liveModding(std::string_view appId,
                   CrowdyStudioLiveModdingAction action) const {
    return liveModding(appId, toString(action));
  }
  bool liveModding(std::string_view appId, std::string_view action) const {
    return run("CrowdyStudioLiveModding", liveModdingVars(appId, action))
        .asBool();
  }
  void liveModdingAsync(std::string_view appId,
                        CrowdyStudioLiveModdingAction action,
                        graphql::GraphQLCallback cb) const {
    liveModdingAsync(appId, toString(action), std::move(cb));
  }
  void liveModdingAsync(std::string_view appId, std::string_view action,
                        graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioLiveModding", liveModdingVars(appId, action),
             std::move(cb));
  }

  // The models this player may use in this app, which a dock lists before its
  // harness starts: `GET {origin}/v1/model/models?appId=<id>` with this
  // client's bearer, on its transports, timeout and response limit.
  CrowdyStudioModelList models(std::string_view appId,
                               const graphql::RequestOptions& options = {}) const {
    return crowdyStudioModelListFrom(api_->getOutcome(modelsPath(appId), options));
  }
  void modelsAsync(std::string_view appId,
                   std::function<void(CrowdyStudioModelList)> cb) const {
    modelsAsync(appId, graphql::RequestOptions{}, std::move(cb));
  }
  void modelsAsync(std::string_view appId, const graphql::RequestOptions& options,
                   std::function<void(CrowdyStudioModelList)> cb) const {
    api_->getAsync(modelsPath(appId), options,
                   [cb = std::move(cb)](graphql::HttpOutcome http) mutable {
                     cb(crowdyStudioModelListFrom(http));
                   });
  }

  // Provider-data consent and metered model usage: the GraphQL companions of
  // the REST endpoint the agent spends tokens through.
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

 private:
  static graphql::JVal one(std::string_view key, std::string_view value) {
    graphql::JVal vars;
    vars[key] = value;
    return vars;
  }
  static std::string modelsPath(std::string_view appId) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string path = "/v1/model/models?appId=";
    for (const unsigned char c : appId) {
      const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
                         c == '.' || c == '~';
      if (plain) {
        path.push_back(static_cast<char>(c));
      } else {
        path.push_back('%');
        path.push_back(kHex[c >> 4]);
        path.push_back(kHex[c & 0xF]);
      }
    }
    return path;
  }
  static graphql::JVal liveModdingVars(std::string_view appId,
                                       std::string_view action) {
    graphql::JVal vars = one("appId", appId);
    vars["action"] = action;
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
