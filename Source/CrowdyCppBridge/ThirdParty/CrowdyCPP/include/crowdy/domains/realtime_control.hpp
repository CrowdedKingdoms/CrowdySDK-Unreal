#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/generated/operations.hpp"
#include "crowdy/graphql/subscription_client.hpp"

/// The realtime CONTROL stream: lifecycle events about the API instance a client
/// is connected to, as opposed to anything about gameplay.
///
/// Native clients had no access to this at all. CrowdyCPP speaks UDP directly
/// rather than through the browser's UDP proxy, so it never subscribed to
/// udpNotifications — and SERVER_DRAINING, the only advance warning that an
/// instance is about to stop serving, arrives on that subscription and nowhere
/// else. Under direct connect that means a native client learned its instance
/// was going away by watching it stop answering.
namespace crowdy::domains {

/// extensions-free codes carried by RealtimeConnectionEvent.code.
///
/// Branch on these, never on `message`: the message is written for a human
/// reading a log and the server is free to reword it.
inline constexpr std::string_view kRealtimeServerDraining = "SERVER_DRAINING";
inline constexpr std::string_view kRealtimeAuthRequired = "AUTH_REQUIRED";
inline constexpr std::string_view kRealtimeAppIdRequired = "APP_ID_REQUIRED";
inline constexpr std::string_view kRealtimeUdpProxyFailed =
    "UDP_PROXY_CONNECTION_FAILED";

/// One control frame.
struct RealtimeConnectionEvent {
  /// "failed", or "draining" for the one advisory case.
  std::string status;
  std::string code;
  std::string message;
  bool retryable = true;

  /// This instance is being taken out of service, but the stream KEEPS WORKING.
  /// Re-discover and move before it stops, rather than waiting for it to.
  bool draining() const { return code == kRealtimeServerDraining; }

  /// The subscription ends immediately after emitting this, so the cause must be
  /// fixed and the subscription reopened. Draining is the exception: it arrives
  /// mid-stream on a healthy subscription and does not end it.
  bool terminal() const { return !draining() && status == "failed"; }
};

class RealtimeControlAPI {
 public:
  explicit RealtimeControlAPI(
      std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions)
      : subscriptions_(std::move(subscriptions)) {}

  /// Watch the control stream. Requires an APP-SCOPED token, or the server
  /// answers APP_ID_REQUIRED: game tokens are app-agnostic and one socket is
  /// shared across apps, so the server cannot infer which app is meant.
  ///
  /// The handle stays live until cancelled. Every non-control member of the
  /// union is ignored, so a client that also has a UDP proxy session open does
  /// not see its gameplay traffic here.
  graphql::SubscriptionHandle watch(
      std::function<void(RealtimeConnectionEvent)> onEvent) {
    graphql::GraphQLSubscriptionRequest request;
    request.document =
        std::string(gen::realtime::kRealtimeControlEventsIsolatedDocument);
    request.operationName =
        std::string(gen::realtime::kRealtimeControlEventsOperationName);

    graphql::GraphQLSubscriptionCallbacks callbacks;
    callbacks.onNext = [onEvent = std::move(onEvent)](
                           graphql::GraphQLSubscriptionOutcome outcome) {
      if (!outcome.ok() || !onEvent) return;
      const graphql::Json payload =
          outcome.data["udpNotifications"].isObject()
              ? outcome.data["udpNotifications"]
              : outcome.data;
      if (payload["__typename"].asString() != "RealtimeConnectionEvent") return;
      RealtimeConnectionEvent event;
      event.status = payload["status"].asString();
      event.code = payload["code"].asString();
      event.message = payload["message"].asString();
      // Absent means retryable, matching the server's contract that only an
      // explicit false forecloses a retry.
      event.retryable = !payload["retryable"].isBool() ||
                        payload["retryable"].asBool();
      onEvent(std::move(event));
    };
    return subscriptions_->subscribe(std::move(request), std::move(callbacks));
  }

 private:
  std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions_;
};

}  // namespace crowdy::domains
