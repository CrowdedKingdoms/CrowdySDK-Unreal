#pragma once

#include <memory>
#include <stdexcept>
#include <utility>

#include "crowdy/agent/controller.hpp"

namespace crowdy::agent {

/**
 * Lifetime-safe construction path for the production Agentic Studio client.
 * The typed HTTP transport and GraphQL-WS event adapter outlive the controller
 * and share CrowdyClient's Dispatcher.
 */
class CrowdyStudioAgentControllerRuntime {
 public:
  CrowdyStudioAgentControllerRuntime(
      domains::CrowdyStudioAgentAPI& api,
      graphql::GraphQLSubscriptionClient& subscriptions,
      CrowdyStudioAgentControllerOptions options)
      : transport_(api, subscriptions),
        controller_(bind(std::move(options), transport_, api)) {}
  /// Durable HTTP polling fallback for native builds without an injected or
  /// compatible default WebSocket transport.
  CrowdyStudioAgentControllerRuntime(
      domains::CrowdyStudioAgentAPI& api,
      CrowdyStudioAgentControllerOptions options)
      : transport_(api),
        controller_(bindPolling(std::move(options), transport_, api)) {}

  CrowdyStudioAgentControllerRuntime(
      std::shared_ptr<domains::CrowdyStudioAgentAPI> api,
      std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions,
      CrowdyStudioAgentControllerOptions options)
      : apiOwner_(std::move(api)),
        subscriptionsOwner_(std::move(subscriptions)),
        transport_(requireApi(apiOwner_),
                   requireSubscriptions(subscriptionsOwner_)),
        controller_(bind(std::move(options), transport_,
                         requireApi(apiOwner_))) {}
  CrowdyStudioAgentControllerRuntime(
      std::shared_ptr<domains::CrowdyStudioAgentAPI> api,
      CrowdyStudioAgentControllerOptions options)
      : apiOwner_(std::move(api)),
        transport_(requireApi(apiOwner_)),
        controller_(bindPolling(std::move(options), transport_,
                                requireApi(apiOwner_))) {}

  CrowdyStudioAgentController& controller() { return controller_; }
  const CrowdyStudioAgentController& controller() const {
    return controller_;
  }
  std::size_t poll() { return controller_.poll(); }

 private:
  static domains::CrowdyStudioAgentAPI& requireApi(
      const std::shared_ptr<domains::CrowdyStudioAgentAPI>& api) {
    if (!api) {
      throw std::invalid_argument(
          "Crowdy Studio agent runtime requires its API");
    }
    return *api;
  }

  static graphql::GraphQLSubscriptionClient& requireSubscriptions(
      const std::shared_ptr<graphql::GraphQLSubscriptionClient>&
          subscriptions) {
    if (!subscriptions) {
      throw std::invalid_argument(
          "Crowdy Studio agent runtime requires subscriptions");
    }
    return *subscriptions;
  }

  static CrowdyStudioAgentControllerOptions bind(
      CrowdyStudioAgentControllerOptions options,
      CrowdyStudioAgentGraphQLTransport& transport,
      domains::CrowdyStudioAgentAPI& api) {
    options.transport = &transport;
    options.subscriptionAdapter = &transport;
    if (!options.dispatcher) options.dispatcher = api.dispatcher();
    return options;
  }
  static CrowdyStudioAgentControllerOptions bindPolling(
      CrowdyStudioAgentControllerOptions options,
      CrowdyStudioAgentGraphQLTransport& transport,
      domains::CrowdyStudioAgentAPI& api) {
    options.transport = &transport;
    options.subscriptionAdapter = nullptr;
    if (!options.dispatcher) options.dispatcher = api.dispatcher();
    return options;
  }

  std::shared_ptr<domains::CrowdyStudioAgentAPI> apiOwner_;
  std::shared_ptr<graphql::GraphQLSubscriptionClient>
      subscriptionsOwner_;
  CrowdyStudioAgentGraphQLTransport transport_;
  CrowdyStudioAgentController controller_;
};

}  // namespace crowdy::agent
