#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/core/result.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/graphql/websocket.hpp"

/// Generic GraphQL subscriptions over the graphql-transport-ws protocol.
/// This layer intentionally exposes documents and JSON rather than
/// domain-specific event reduction so engines and higher-level controllers can
/// build their own durable replay semantics.
namespace crowdy::graphql {

class GraphQLClient;

struct GraphQLSubscriptionOptions {
  long connectTimeoutMs = 10000;
  long acknowledgementTimeoutMs = 10000;
  long initialReconnectDelayMs = 250;
  long maxReconnectDelayMs = 10000;
  std::size_t maxReconnectAttempts = 8;
  double reconnectJitter = 0.20;
  std::size_t maxFrameBytes = kDefaultWebSocketFrameLimit;
  /// Zero selects an implementation-defined seed. Tests may set a stable seed.
  std::uint32_t reconnectRandomSeed = 0;
};

enum class GraphQLWebSocketEndpointKind {
  /// An API base URL whose path must end in exactly one `/graphql`.
  ApiBase,
  /// An explicitly complete endpoint. Its path, trailing slash, and query are
  /// preserved while HTTP(S) schemes are converted to WS(S).
  Complete,
};

struct GraphQLSubscriptionClientConfig {
  /// HTTP(S) or WS(S) API base or explicitly complete endpoint.
  std::string endpoint;
  GraphQLSubscriptionOptions options;
  GraphQLWebSocketEndpointKind endpointKind =
      GraphQLWebSocketEndpointKind::ApiBase;
};

struct GraphQLSubscriptionRequest {
  std::string document;
  JVal variables;
  std::string operationName;
};

/// A successful `next` payload. GraphQL may legally return partial data and
/// errors together; callers can inspect both without losing either.
struct GraphQLSubscriptionOutcome {
  Status status = Errc::Ok;
  Json data;
  std::vector<GraphQLErrorDetail> errors;
  Json payload;

  bool ok() const { return status.ok() && errors.empty(); }
};

enum class GraphQLSubscriptionErrorKind {
  TransportUnavailable,
  Transport,
  Timeout,
  Protocol,
  FrameTooLarge,
  InvalidUtf8,
  GraphQL,
  Authentication,
  Authorization,
  AppScope,
  StaleClientEpoch,
  ReconnectExhausted,
};

struct GraphQLSubscriptionError {
  Status status = Errc::SocketError;
  GraphQLSubscriptionErrorKind kind = GraphQLSubscriptionErrorKind::Transport;
  /// Stable GraphQL extension code or local protocol code.
  std::string code;
  std::string message;
  std::vector<GraphQLErrorDetail> errors;
  std::uint16_t closeCode = 0;
  bool retryable = false;
  bool terminal = true;
};

struct GraphQLReconnectInfo {
  /// One-based attempt that established this replacement connection.
  std::size_t attempt = 0;
  /// Stable protocol operation id that was replayed.
  std::string operationId;
};

struct GraphQLSubscriptionCallbacks {
  std::function<void(GraphQLSubscriptionOutcome)> onNext;
  std::function<void(GraphQLSubscriptionError)> onError;
  std::function<void()> onComplete;
  /// Fired immediately before the original subscribe operation is replayed
  /// after a reconnect. Use it to start a durable gap-fill query when needed.
  std::function<void(GraphQLReconnectInfo)> onReconnect;
};

class SubscriptionHandle {
 public:
  SubscriptionHandle() = default;
  ~SubscriptionHandle();

  SubscriptionHandle(const SubscriptionHandle&) = delete;
  SubscriptionHandle& operator=(const SubscriptionHandle&) = delete;
  SubscriptionHandle(SubscriptionHandle&& other) noexcept;
  SubscriptionHandle& operator=(SubscriptionHandle&& other) noexcept;

  /// Idempotently send `complete` (when connected) and suppress callbacks that
  /// were queued but not yet drained.
  void cancel();
  bool active() const;
  explicit operator bool() const { return active(); }

 private:
  struct Control;
  explicit SubscriptionHandle(std::shared_ptr<Control> control)
      : control_(std::move(control)) {}

  std::shared_ptr<Control> control_;
  friend class GraphQLSubscriptionClient;
};

class GraphQLSubscriptionClient {
 public:
  GraphQLSubscriptionClient(GraphQLSubscriptionClientConfig config,
                            std::shared_ptr<IWebSocketTransport> transport,
                            std::shared_ptr<AuthState> auth,
                            std::shared_ptr<Dispatcher> dispatcher);
  /// Reuse an HTTP GraphQL client's AuthState and Dispatcher. If the HTTP
  /// client has no Dispatcher yet, one is installed on it.
  GraphQLSubscriptionClient(GraphQLSubscriptionClientConfig config,
                            std::shared_ptr<IWebSocketTransport> transport,
                            GraphQLClient& httpClient);
  ~GraphQLSubscriptionClient();

  GraphQLSubscriptionClient(const GraphQLSubscriptionClient&) = delete;
  GraphQLSubscriptionClient& operator=(const GraphQLSubscriptionClient&) = delete;
  GraphQLSubscriptionClient(GraphQLSubscriptionClient&&) noexcept;
  GraphQLSubscriptionClient& operator=(GraphQLSubscriptionClient&&) noexcept;

  SubscriptionHandle subscribe(GraphQLSubscriptionRequest request,
                               GraphQLSubscriptionCallbacks callbacks);
  SubscriptionHandle subscribe(std::string_view document, const JVal& variables,
                               std::string_view operationName,
                               GraphQLSubscriptionCallbacks callbacks);

  /// Cancel all operations and close the connection. Idempotent.
  void close();

  /// Standalone completion pump. CrowdyClient users call CrowdyClient::poll(),
  /// which drains the same shared Dispatcher for HTTP and subscriptions.
  std::size_t poll();

  /// By value: the endpoint moves (datacenter redirect, re-discovery), so a
  /// reference into it would dangle the moment it did.
  std::string endpoint() const;

  /// Move the socket to a different origin, reconnecting immediately and
  /// replaying every live subscription. False when the URL will not normalize
  /// or is already current.
  bool setEndpoint(std::string_view endpoint);

  /// Call `handler` once `afterFailures` consecutive reconnects have failed, so
  /// a caller can go find a different endpoint. Zero disables it. Invoked
  /// outside the internal lock, so the handler may move this client.
  void setRepeatedFailureHandler(int afterFailures, std::function<void()> handler);

  std::shared_ptr<Dispatcher> dispatcher() const;
  AuthState& auth();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

/// Convert http/https to ws/wss and preserve ws/wss. API bases remove trailing
/// duplicate GraphQL segments and finish with exactly one `/graphql`; complete
/// endpoints preserve their explicitly configured path, query, and slash.
Result<std::string> normalizeGraphQLWebSocketUrl(
    std::string_view endpoint,
    GraphQLWebSocketEndpointKind kind =
        GraphQLWebSocketEndpointKind::ApiBase);

/// Stable terminal-code classification used by reconnect policy. Exposed so
/// higher-level generic controllers can make the same decision for partial
/// `next` payloads.
bool isTerminalSubscriptionErrorCode(std::string_view code);

}  // namespace crowdy::graphql
