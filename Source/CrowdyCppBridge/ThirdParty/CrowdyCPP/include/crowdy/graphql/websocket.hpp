#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "crowdy/core/result.hpp"

/// Replaceable, engine-neutral WebSocket transport primitives. Implementations
/// own their I/O model; callbacks may move between arbitrary transport threads,
/// but one connection must preserve wire order and invoke them serially.
/// GraphQLSubscriptionClient protects protocol state and routes every user
/// callback through Dispatcher.
namespace crowdy::graphql {

inline constexpr std::size_t kDefaultWebSocketFrameLimit = 1024U * 1024U;

enum class WebSocketFrameKind {
  Text,
  Binary,
  Ping,
  Pong,
};

struct WebSocketFrame {
  WebSocketFrameKind kind = WebSocketFrameKind::Text;
  std::string payload;
};

enum class WebSocketErrorKind {
  Unavailable,
  Resolve,
  Tls,
  Connection,
  Timeout,
  Io,
  Protocol,
  FrameTooLarge,
  InvalidUtf8,
  Cancelled,
};

struct WebSocketError {
  WebSocketErrorKind kind = WebSocketErrorKind::Io;
  Status status = Errc::SocketError;
  std::string message;
  /// Whether opening a fresh connection without changing the request may work.
  bool retryable = true;
};

struct WebSocketCloseInfo {
  std::uint16_t code = 1006;
  std::string reason;
  bool clean = false;
};

enum class WebSocketEventKind {
  Open,
  Frame,
  Close,
  Error,
};

struct WebSocketEvent {
  WebSocketEventKind kind = WebSocketEventKind::Error;
  WebSocketFrame frame;
  WebSocketCloseInfo close;
  WebSocketError error;
};

using WebSocketEventCallback = std::function<void(WebSocketEvent)>;

struct WebSocketConnectRequest {
  std::string url;
  std::string subprotocol = "graphql-transport-ws";
  long connectTimeoutMs = 10000;
  /// Hard limit for one reassembled inbound or outbound WebSocket message.
  std::size_t maxFrameBytes = kDefaultWebSocketFrameLimit;
};

class IWebSocketConnection {
 public:
  virtual ~IWebSocketConnection() = default;

  /// Start I/O and install the event callback. Called exactly once. The
  /// callback may run synchronously before start() returns and later from any
  /// transport thread.
  virtual void start(WebSocketEventCallback callback) = 0;

  /// Queue or send one frame. Thread-safe and non-blocking for engine
  /// transports. A later send failure is reported through the event callback.
  virtual Status send(WebSocketFrame frame) = 0;

  /// Begin an idempotent close handshake. Thread-safe and non-blocking.
  virtual void close(std::uint16_t code = 1000, std::string_view reason = {}) = 0;
};

class IWebSocketTransport {
 public:
  virtual ~IWebSocketTransport() = default;

  /// Create a dormant connection. No callbacks or I/O occur until start().
  /// Returning null reports that the connection could not be created.
  virtual std::shared_ptr<IWebSocketConnection> createConnection(
      const WebSocketConnectRequest& request) = 0;
};

/// Optional default transport backed by libcurl's WebSocket API. Returns null
/// when CrowdyCPP was built without a compatible libcurl; injected transports
/// remain fully supported in that build.
std::shared_ptr<IWebSocketTransport> makeCurlWebSocketTransport();
bool curlWebSocketTransportAvailable() noexcept;

}  // namespace crowdy::graphql
