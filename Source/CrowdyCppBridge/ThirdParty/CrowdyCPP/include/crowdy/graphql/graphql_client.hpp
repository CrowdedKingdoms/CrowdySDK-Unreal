#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/core/result.hpp"
#include "crowdy/graphql/async_scope.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/datacenter_redirect.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"

/// GraphQL-over-HTTP client. One per CrowdyClient, sharing that client's
/// AuthState so HTTP auth never drifts.
namespace crowdy::graphql {

struct GraphQLClientConfig {
  std::string endpoint;  ///< full URL of the /graphql endpoint
  long timeoutMs = 60000;
};

/// Which failure a GraphQLOutcome represents (None when it succeeded). The
/// blocking request() maps each of these onto the matching CrowdyError; the
/// async path leaves them as data so callers branch on the code.
enum class GraphQLErrorKind { None, Http, GraphQL, Protocol, Network, Timeout };

/// Non-throwing result of an operation, the async twin of request(). ok() means
/// the server returned data with no GraphQL errors; otherwise `kind` and the
/// matching fields describe the failure.
struct GraphQLOutcome {
  Status status;                            ///< Ok, or an Errc describing the failure
  GraphQLErrorKind kind = GraphQLErrorKind::None;
  Json data;                                ///< the `data` value when ok()
  std::vector<GraphQLErrorDetail> errors;   ///< server GraphQL errors (kind == GraphQL)
  int httpStatus = 0;                       ///< HTTP status code of the response
  std::string errorMessage;                 ///< protocol/network/timeout detail
  std::string body;                         ///< raw response body (kind == Http)

  bool ok() const { return status.ok() && kind == GraphQLErrorKind::None; }
};

using GraphQLCallback = std::function<void(GraphQLOutcome)>;

class GraphQLClient {
 public:
  GraphQLClient(GraphQLClientConfig config, std::shared_ptr<IHttpTransport> transport,
                std::shared_ptr<AuthState> auth)
      : config_(std::move(config)),
        endpoint_(config_.endpoint),
        transport_(std::move(transport)),
        auth_(std::move(auth)) {}
  ~GraphQLClient() { close(); }

  GraphQLClient(const GraphQLClient&) = delete;
  GraphQLClient& operator=(const GraphQLClient&) = delete;

  /// Execute an operation and return the `data` value. Blocking.
#ifndef CROWDY_NO_EXCEPTIONS
  /// Throws CrowdyHttpError / CrowdyGraphQLError / CrowdyNetworkError /
  /// CrowdyTimeoutError / CrowdyProtocolError. Prefer requestAsync in engines.
#else
  /// Exception-disabled compatibility path: returns an invalid Json on any
  /// HTTP/GraphQL/protocol/transport failure. Prefer requestAsync when the
  /// typed failure details are required.
#endif
  Json request(std::string_view document, const JVal& variables = JVal(),
               std::string_view operationName = {});

  /// Execute an operation without blocking or throwing. `cb` is invoked once
  /// with the outcome. When an async transport is set the request runs on it;
  /// otherwise it falls back to the synchronous transport inline. When a
  /// Dispatcher is set the callback is delivered from Dispatcher::drain(),
  /// otherwise inline on whatever thread the transport completed on.
  void requestAsync(std::string_view document, const JVal& variables,
                    std::string_view operationName, GraphQLCallback cb);

  /// Handle a WRONG_DATACENTER redirect. The handler MOVES the client (this
  /// endpoint, and typically the WebSocket and UDP session with it) and returns
  /// whether it did. Declining is safe: the caller then sees the server's
  /// original error. A retry is earned by a `true`, or by the client already
  /// sitting somewhere other than where the failed request was sent - the case
  /// where a concurrent request's redirect moved the client first, which the
  /// handler reports as "no move" only because the target is already current.
  ///
  /// Set by CrowdyClient. Set it yourself only when driving a bare
  /// GraphQLClient, and move every transport together if you do — an HTTP
  /// client that follows a redirect while its subscriptions stay behind is
  /// querying one datacenter and playing in another.
  void setWrongDatacenterHandler(
      std::function<bool(const DatacenterMove&)> handler) {
    std::lock_guard lock(endpointMutex_);
    wrongDatacenterHandler_ = std::move(handler);
  }

  /// Inject the engine's async HTTP transport (FHttpModule, etc.).
  void setAsyncTransport(std::shared_ptr<IAsyncHttpTransport> transport) {
    asyncTransport_ = std::move(transport);
  }
  /// Route async callbacks through this dispatcher so they fire on drain().
  void setDispatcher(std::shared_ptr<Dispatcher> dispatcher) {
    dispatcher_ = std::move(dispatcher);
  }

  /// Terminally suppress callbacks from in-flight requests. The underlying
  /// transport may still finish its platform request, but its retained
  /// completion cannot invoke code owned by this client.
  void close() { asyncScope_->close(); }

  /// By value, not by reference: the endpoint MOVES now (datacenter redirect,
  /// re-discovery), and a reference into it would dangle the moment it did.
  std::string endpoint() const {
    std::lock_guard lock(endpointMutex_);
    return endpoint_;
  }

  /// Point this client at a different origin. Requests already in flight keep
  /// the URL they were built with; everything after this call uses the new one.
  ///
  /// Returns false when `endpoint` is empty or already current, so a caller can
  /// tell "moved" from "no move happened" — the datacenter-redirect retry needs
  /// that distinction, because retrying against the same URL is how a redirect
  /// loop starts.
  bool setEndpoint(std::string endpoint) {
    if (endpoint.empty()) return false;
    std::lock_guard lock(endpointMutex_);
    if (endpoint == endpoint_) return false;
    endpoint_ = std::move(endpoint);
    return true;
  }

  AuthState& auth() { return *auth_; }
  std::shared_ptr<AuthState> sharedAuthState() const { return auth_; }
  std::shared_ptr<Dispatcher> dispatcher() const { return dispatcher_; }

 private:
  HttpRequest buildHttpRequest(std::string_view document, const JVal& variables,
                               std::string_view operationName) const;
  HttpOutcome sendInline(const HttpRequest& request);
  /// Offer a failed outcome to the wrong-datacenter handler. True when the
  /// request is worth re-issuing: either this outcome's redirect moved the
  /// endpoint, or another request's redirect had already moved it away from
  /// `requestUrl` (the URL this request was actually sent to) while this one
  /// was in flight. Retrying against an unchanged URL stays refused - that is
  /// how a redirect loop starts.
  bool applyDatacenterRedirect(const GraphQLOutcome& outcome,
                               std::string_view requestUrl);
  /// `retryAuthorization` is set on the datacenter-redirect retry only: the
  /// retry re-issues the SAME operation, so it carries the Authorization the
  /// original attempt carried (empty = the attempt sent none) rather than
  /// whatever the shared AuthState holds by delivery time - engines that
  /// multiplex token planes over one client switch that state per call.
  void requestAsyncAttempt(std::string document, const JVal& variables,
                           std::string operationName, GraphQLCallback cb,
                           bool isRetry,
                           std::optional<std::string> retryAuthorization =
                               std::nullopt);

  GraphQLClientConfig config_;
  mutable std::mutex endpointMutex_;
  std::string endpoint_;
  std::function<bool(const DatacenterMove&)> wrongDatacenterHandler_;
  std::shared_ptr<IHttpTransport> transport_;
  std::shared_ptr<AuthState> auth_;
  std::shared_ptr<IAsyncHttpTransport> asyncTransport_;
  std::shared_ptr<Dispatcher> dispatcher_;
  std::shared_ptr<AsyncScope> asyncScope_ = std::make_shared<AsyncScope>();
};

}  // namespace crowdy::graphql
