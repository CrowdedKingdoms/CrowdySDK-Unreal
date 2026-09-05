#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/core/result.hpp"

/// Pluggable HTTP transport. The default implementation (curlTransport())
/// wraps libcurl; engine wrappers substitute their own HTTP stack (e.g. an
/// engine HTTP module) by implementing IHttpTransport.
///
/// Transports must be safe to call from multiple threads.
namespace crowdy::graphql {

struct HttpRequest {
  std::string url;
  std::string method = "POST";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  long timeoutMs = 60000;
};

struct HttpResponse {
  int status = 0;
  std::string body;
};

/// Result of an HTTP round trip. status.ok() means the request completed and
/// `response` holds whatever the server returned (any HTTP status code); a
/// non-ok status is a transport-level failure (DNS, TLS, connection, timeout)
/// with detail in `errorMessage`.
struct HttpOutcome {
  Status status;
  HttpResponse response;
  std::string errorMessage;
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
#ifndef CROWDY_NO_EXCEPTIONS
  /// Throws CrowdyNetworkError / CrowdyTimeoutError on transport failure.
  /// Non-2xx statuses are returned, not thrown (the GraphQL client decides).
#else
  /// Compatibility method for existing injected transports. Implementations
  /// compiled without exceptions must return an empty response on failure and
  /// should override sendOutcome() to preserve a typed transport status.
#endif
  virtual HttpResponse send(const HttpRequest& request) = 0;

  /// Non-throwing transport entry point used internally by GraphQLClient.
  /// Existing exception-enabled transports only overriding send() remain
  /// source-compatible: the default adapter catches and translates failures.
  /// A transport compiled with exceptions disabled should override this method
  /// to report failures instead of relying on send().
  virtual HttpOutcome sendOutcome(const HttpRequest& request) noexcept;
};

/// Default libcurl transport. Maintains a per-instance cookie jar so sticky
/// load-balancer cookies persist across requests. Only available when built
/// with CROWDY_WITH_CURL (the default); returns null otherwise.
std::shared_ptr<IHttpTransport> makeCurlTransport();

/// Async, non-blocking, non-throwing HTTP transport. This is the interface a
/// game engine implements over its own async HTTP stack (Unreal FHttpModule,
/// Unity UnityWebRequest, Godot HTTPRequest) so API calls never block a frame
/// and never throw. sendAsync starts the request and invokes `cb` exactly once
/// when it finishes; the callback may run on any thread, so route it through a
/// Dispatcher to land on the game thread.
class IAsyncHttpTransport {
 public:
  virtual ~IAsyncHttpTransport() = default;
  virtual void sendAsync(const HttpRequest& request,
                         std::function<void(HttpOutcome)> cb) = 0;
};

/// Adapts a synchronous IHttpTransport to the async interface by running the
/// blocking send inline and invoking the callback before returning. Useful as
/// a fallback and for tests; a real engine transport avoids the block.
std::shared_ptr<IAsyncHttpTransport> makeInlineAsyncTransport(
    std::shared_ptr<IHttpTransport> sync);

/// Serial worker-thread adapter for native clients that need nonblocking
/// callback pumps but do not inject an engine HTTP stack. Requests execute
/// away from the game thread; GraphQLClient still delivers completions through
/// Dispatcher/poll().
std::shared_ptr<IAsyncHttpTransport> makeThreadedAsyncTransport(
    std::shared_ptr<IHttpTransport> sync);

}  // namespace crowdy::graphql
