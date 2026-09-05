#include "crowdy/graphql/subscription_client.hpp"

#include "crowdy/graphql/graphql_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <exception>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <utility>

#include "error_extensions.hpp"

namespace crowdy::graphql {

namespace {

using Clock = std::chrono::steady_clock;

bool validUtf8(std::string_view input) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
  std::size_t i = 0;
  while (i < input.size()) {
    const unsigned char first = bytes[i++];
    if (first <= 0x7fU) continue;

    std::size_t continuation = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation = 1;
      value = static_cast<std::uint32_t>(first & 0x1fU);
      minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation = 2;
      value = static_cast<std::uint32_t>(first & 0x0fU);
      minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation = 3;
      value = static_cast<std::uint32_t>(first & 0x07U);
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation > input.size() - i) return false;
    for (std::size_t n = 0; n < continuation; ++n) {
      const unsigned char next = bytes[i++];
      if ((next & 0xc0U) != 0x80U) return false;
      value = (value << 6U) | static_cast<std::uint32_t>(next & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
      return false;
    }
  }
  return true;
}

std::string uppercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return result;
}

std::vector<GraphQLErrorDetail> parseGraphQLErrors(Json value) {
  if (value.isObject() && value["errors"].isArray()) value = value["errors"];

  std::vector<GraphQLErrorDetail> errors;
  auto append = [&errors](Json item) {
    if (!item.isObject()) return;
    errors.push_back(detail::readGraphQLError(item, "GraphQL subscription error"));
  };

  if (value.isArray()) {
    value.forEach(append);
  } else {
    append(value);
  }
  return errors;
}

GraphQLSubscriptionError classifyGraphQLError(
    std::vector<GraphQLErrorDetail> errors) {
  GraphQLSubscriptionError result;
  result.status = Errc::Rejected;
  result.kind = GraphQLSubscriptionErrorKind::GraphQL;
  result.code = errors.empty() || errors.front().code.empty()
                    ? "GRAPHQL_SUBSCRIPTION_ERROR"
                    : errors.front().code;
  result.message = errors.empty() ? "GraphQL subscription failed"
                                  : errors.front().message;
  result.errors = std::move(errors);
  result.terminal = true;
  result.retryable = false;

  const std::string code = uppercase(result.code);
  if (code == "UNAUTHENTICATED" || code == "AUTH_REQUIRED" ||
      code == "TOKEN_EXPIRED" || code == "INVALID_TOKEN" ||
      code == "AUTHENTICATION_ERROR") {
    result.status = Errc::TokenExpired;
    result.kind = GraphQLSubscriptionErrorKind::Authentication;
  } else if (code == "FORBIDDEN" || code == "UNAUTHORIZED") {
    result.kind = GraphQLSubscriptionErrorKind::Authorization;
  } else if (code == "APP_ID_REQUIRED" || code == "APP_SCOPE_REQUIRED" ||
             code == "APP_SCOPE_MISMATCH" ||
             code == "APP_TOKEN_SCOPE_MISMATCH") {
    result.kind = GraphQLSubscriptionErrorKind::AppScope;
  } else if (code == "CLIENT_EPOCH_STALE" ||
             code == "AGENT_CLIENT_EPOCH_STALE" ||
             code.ends_with("_CLIENT_EPOCH_STALE")) {
    result.kind = GraphQLSubscriptionErrorKind::StaleClientEpoch;
  }
  return result;
}

GraphQLSubscriptionError protocolError(GraphQLSubscriptionErrorKind kind,
                                       Status status, std::string code,
                                       std::string message) {
  GraphQLSubscriptionError error;
  error.status = status;
  error.kind = kind;
  error.code = std::move(code);
  error.message = std::move(message);
  error.terminal = true;
  error.retryable = false;
  return error;
}

std::string reasonCode(std::string_view reason) {
  const std::string upper = uppercase(reason);
  constexpr std::string_view candidates[] = {
      "AUTH_REQUIRED",           "UNAUTHENTICATED",
      "TOKEN_EXPIRED",           "APP_ID_REQUIRED",
      "APP_SCOPE_REQUIRED",      "APP_SCOPE_MISMATCH",
      "APP_TOKEN_SCOPE_MISMATCH", "AGENT_CLIENT_EPOCH_STALE",
      "CLIENT_EPOCH_STALE",
  };
  for (const std::string_view candidate : candidates) {
    if (upper.find(candidate) != std::string::npos) return std::string(candidate);
  }
  return {};
}

GraphQLSubscriptionError mapClose(const WebSocketCloseInfo& close) {
  const std::string stableReasonCode = reasonCode(close.reason);
  if (!stableReasonCode.empty()) {
    GraphQLErrorDetail detail;
    detail.code = stableReasonCode;
    detail.message = close.reason.empty() ? stableReasonCode : close.reason;
    auto result = classifyGraphQLError({std::move(detail)});
    result.closeCode = close.code;
    return result;
  }

  GraphQLSubscriptionError result;
  result.status = Errc::SocketError;
  result.kind = GraphQLSubscriptionErrorKind::Transport;
  result.code = "WEBSOCKET_CLOSED";
  result.message = close.reason.empty()
                       ? "WebSocket closed with code " + std::to_string(close.code)
                       : close.reason;
  result.closeCode = close.code;
  result.retryable = true;
  result.terminal = false;

  switch (close.code) {
    case 4401:
      result.status = Errc::TokenExpired;
      result.kind = GraphQLSubscriptionErrorKind::Authentication;
      result.code = "UNAUTHENTICATED";
      result.retryable = false;
      result.terminal = true;
      break;
    case 4403:
    case 1008:
      result.status = Errc::Rejected;
      result.kind = GraphQLSubscriptionErrorKind::Authorization;
      result.code = "FORBIDDEN";
      result.retryable = false;
      result.terminal = true;
      break;
    case 4408:
      result.status = Errc::Timeout;
      result.kind = GraphQLSubscriptionErrorKind::Timeout;
      result.code = "ACKNOWLEDGEMENT_TIMEOUT";
      break;
    case 1007:
      result.status = Errc::Malformed;
      result.kind = GraphQLSubscriptionErrorKind::InvalidUtf8;
      result.code = "INVALID_UTF8";
      result.retryable = false;
      result.terminal = true;
      break;
    case 1009:
      result.status = Errc::BufferTooSmall;
      result.kind = GraphQLSubscriptionErrorKind::FrameTooLarge;
      result.code = "FRAME_TOO_LARGE";
      result.retryable = false;
      result.terminal = true;
      break;
    case 1002:
    case 1003:
    case 4400:
    case 4406:
    case 4409:
    case 4429:
      result.status = Errc::Malformed;
      result.kind = GraphQLSubscriptionErrorKind::Protocol;
      result.code = "WEBSOCKET_PROTOCOL_ERROR";
      result.retryable = false;
      result.terminal = true;
      break;
    default:
      break;
  }
  return result;
}

GraphQLSubscriptionError mapTransportError(const WebSocketError& source) {
  GraphQLSubscriptionError result;
  result.status = source.status;
  result.kind = GraphQLSubscriptionErrorKind::Transport;
  result.code = "WEBSOCKET_TRANSPORT_ERROR";
  result.message = source.message.empty() ? "WebSocket transport failed"
                                          : source.message;
  result.retryable = source.retryable;
  result.terminal = !source.retryable;
  switch (source.kind) {
    case WebSocketErrorKind::Unavailable:
      result.kind = GraphQLSubscriptionErrorKind::TransportUnavailable;
      result.code = "WEBSOCKET_TRANSPORT_UNAVAILABLE";
      break;
    case WebSocketErrorKind::Timeout:
      result.kind = GraphQLSubscriptionErrorKind::Timeout;
      result.code = "WEBSOCKET_TIMEOUT";
      break;
    case WebSocketErrorKind::Protocol:
      result.kind = GraphQLSubscriptionErrorKind::Protocol;
      result.code = "WEBSOCKET_PROTOCOL_ERROR";
      break;
    case WebSocketErrorKind::FrameTooLarge:
      result.kind = GraphQLSubscriptionErrorKind::FrameTooLarge;
      result.code = "FRAME_TOO_LARGE";
      result.retryable = false;
      result.terminal = true;
      break;
    case WebSocketErrorKind::InvalidUtf8:
      result.kind = GraphQLSubscriptionErrorKind::InvalidUtf8;
      result.code = "INVALID_UTF8";
      result.retryable = false;
      result.terminal = true;
      break;
    case WebSocketErrorKind::Cancelled:
      result.status = Errc::Closed;
      result.code = "WEBSOCKET_CANCELLED";
      result.retryable = false;
      result.terminal = true;
      break;
    case WebSocketErrorKind::Resolve:
    case WebSocketErrorKind::Tls:
    case WebSocketErrorKind::Connection:
    case WebSocketErrorKind::Io:
      break;
  }
  return result;
}

std::shared_ptr<Dispatcher> ensureDispatcher(GraphQLClient& client) {
  auto dispatcher = client.dispatcher();
  if (!dispatcher) {
    dispatcher = std::make_shared<Dispatcher>();
    client.setDispatcher(dispatcher);
  }
  return dispatcher;
}

}  // namespace

Result<std::string> normalizeGraphQLWebSocketUrl(
    std::string_view endpoint, GraphQLWebSocketEndpointKind kind) {
  if (endpoint.empty()) return Errc::InvalidArgument;
  for (const unsigned char c : endpoint) {
    if (c <= 0x20U || c == 0x7fU) return Errc::InvalidArgument;
  }
  if (endpoint.find('#') != std::string_view::npos) return Errc::InvalidArgument;

  const std::size_t schemeEnd = endpoint.find("://");
  if (schemeEnd == std::string_view::npos) return Errc::InvalidArgument;
  const std::string scheme = uppercase(endpoint.substr(0, schemeEnd));
  std::string wsScheme;
  if (scheme == "HTTP" || scheme == "WS") {
    wsScheme = "ws";
  } else if (scheme == "HTTPS" || scheme == "WSS") {
    wsScheme = "wss";
  } else {
    return Errc::InvalidArgument;
  }

  std::string_view remainder = endpoint.substr(schemeEnd + 3);
  const std::size_t queryAt = remainder.find('?');
  const std::string_view address = remainder.substr(0, queryAt);
  const std::string_view query =
      queryAt == std::string_view::npos ? std::string_view{} : remainder.substr(queryAt);
  const std::size_t pathAt = address.find('/');
  const std::string_view authority = address.substr(0, pathAt);
  if (authority.empty()) return Errc::InvalidArgument;

  if (kind == GraphQLWebSocketEndpointKind::Complete) {
    return wsScheme + "://" + std::string(address) + std::string(query);
  }

  std::string path =
      pathAt == std::string_view::npos ? std::string{} : std::string(address.substr(pathAt));
  while (!path.empty() && path.back() == '/') path.pop_back();
  constexpr std::string_view suffix = "/graphql";
  while (path.size() >= suffix.size() &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
    path.erase(path.size() - suffix.size());
    while (!path.empty() && path.back() == '/') path.pop_back();
  }
  path += suffix;

  return wsScheme + "://" + std::string(authority) + path + std::string(query);
}

bool isTerminalSubscriptionErrorCode(std::string_view code) {
  const std::string normalized = uppercase(code);
  return normalized == "UNAUTHENTICATED" || normalized == "AUTH_REQUIRED" ||
         normalized == "TOKEN_EXPIRED" || normalized == "INVALID_TOKEN" ||
         normalized == "AUTHENTICATION_ERROR" || normalized == "FORBIDDEN" ||
         normalized == "UNAUTHORIZED" || normalized == "APP_ID_REQUIRED" ||
         normalized == "APP_SCOPE_REQUIRED" ||
         normalized == "APP_SCOPE_MISMATCH" ||
         normalized == "APP_TOKEN_SCOPE_MISMATCH" ||
         normalized == "CLIENT_EPOCH_STALE" ||
         normalized == "AGENT_CLIENT_EPOCH_STALE" ||
         normalized.ends_with("_CLIENT_EPOCH_STALE");
}

struct SubscriptionHandle::Control {
  std::atomic<bool> active{true};
  std::atomic<bool> cancelled{false};
  std::recursive_mutex deliveryMutex;
  std::function<void()> cancel;
};

SubscriptionHandle::~SubscriptionHandle() { cancel(); }

SubscriptionHandle::SubscriptionHandle(SubscriptionHandle&& other) noexcept
    : control_(std::move(other.control_)) {}

SubscriptionHandle& SubscriptionHandle::operator=(SubscriptionHandle&& other) noexcept {
  if (this != &other) {
    cancel();
    control_ = std::move(other.control_);
  }
  return *this;
}

void SubscriptionHandle::cancel() {
  if (!control_) return;
  {
    // Synchronize with callback execution so no callback can begin after this
    // method returns. Recursive locking permits cancellation from a callback.
    std::lock_guard deliveryLock(control_->deliveryMutex);
    control_->cancelled.store(true, std::memory_order_release);
  }
  if (!control_->active.exchange(false, std::memory_order_acq_rel)) return;
  if (control_->cancel) control_->cancel();
}

bool SubscriptionHandle::active() const {
  return control_ && control_->active.load();
}

class GraphQLSubscriptionClient::Impl final
    : public std::enable_shared_from_this<GraphQLSubscriptionClient::Impl> {
 public:
  Impl(GraphQLSubscriptionClientConfig config,
       std::shared_ptr<IWebSocketTransport> transport,
       std::shared_ptr<AuthState> auth,
       std::shared_ptr<Dispatcher> dispatcher)
      : config_(std::move(config)),
        transport_(std::move(transport)),
        auth_(auth ? std::move(auth) : std::make_shared<AuthState>()),
        dispatcher_(dispatcher ? std::move(dispatcher)
                               : std::make_shared<Dispatcher>()),
        random_(config_.options.reconnectRandomSeed == 0
                    ? std::random_device{}()
                    : config_.options.reconnectRandomSeed) {
    Result<std::string> normalized =
        normalizeGraphQLWebSocketUrl(config_.endpoint, config_.endpointKind);
    if (normalized.ok()) endpoint_ = std::move(normalized.value());
  }

  ~Impl() { shutdown(); }

  void initialize() {
    auth_->onChange([weak = weak_from_this()](const std::string&) {
      if (auto self = weak.lock()) self->credentialsChanged();
    });
  }

  SubscriptionHandle subscribe(GraphQLSubscriptionRequest request,
                               GraphQLSubscriptionCallbacks callbacks) {
    auto operation = std::make_shared<Operation>();
    operation->request = std::move(request);
    operation->callbacks = std::move(callbacks);
    operation->control = std::make_shared<SubscriptionHandle::Control>();

    bool connect = false;
    bool sendNow = false;
    std::uint64_t generation = 0;
    GraphQLSubscriptionError immediate;
    bool hasImmediateError = false;
    {
      std::lock_guard lock(mutex_);
      operation->id = std::to_string(nextOperationId_++);
      operation->control->cancel =
          [weak = weak_from_this(), id = operation->id] {
            if (auto self = weak.lock()) self->cancelOperation(id);
          };

      if (shutdown_) {
        immediate = protocolError(GraphQLSubscriptionErrorKind::Transport,
                                  Errc::Closed, "CLIENT_CLOSED",
                                  "GraphQL subscription client is closed");
        hasImmediateError = true;
      } else if (operation->request.document.empty()) {
        immediate = protocolError(GraphQLSubscriptionErrorKind::Protocol,
                                  Errc::InvalidArgument, "EMPTY_DOCUMENT",
                                  "GraphQL subscription document is empty");
        hasImmediateError = true;
      } else if (endpoint_.empty()) {
        immediate = protocolError(GraphQLSubscriptionErrorKind::Protocol,
                                  Errc::InvalidArgument, "INVALID_WEBSOCKET_URL",
                                  "GraphQL WebSocket endpoint is invalid");
        hasImmediateError = true;
      } else if (config_.options.maxFrameBytes == 0) {
        immediate = protocolError(GraphQLSubscriptionErrorKind::FrameTooLarge,
                                  Errc::InvalidArgument, "INVALID_FRAME_LIMIT",
                                  "WebSocket frame limit must be greater than zero");
        hasImmediateError = true;
      } else if (!transport_) {
        immediate = protocolError(
            GraphQLSubscriptionErrorKind::TransportUnavailable,
            Errc::NotConnected, "WEBSOCKET_TRANSPORT_UNAVAILABLE",
            "No default WebSocket transport is available; inject IWebSocketTransport");
        hasImmediateError = true;
      } else {
        operations_.emplace(operation->id, operation);
        if (state_ == ConnectionState::Idle) {
          connect = true;
        } else if (state_ == ConnectionState::Ready) {
          operation->subscribedOnce = true;
          sendNow = true;
          generation = generation_;
        }
      }

      if (hasImmediateError) operation->control->active.store(false);
    }

    if (hasImmediateError) {
      postError(operation, std::move(immediate));
    } else {
      ensureTimerThread();
      if (connect) startConnection(0);
      if (sendNow) sendSubscribe(operation, generation);
    }
    return SubscriptionHandle(operation->control);
  }

  void shutdown() {
    std::lock_guard shutdownLock(shutdownMutex_);
    std::vector<std::shared_ptr<Operation>> operations;
    std::shared_ptr<IWebSocketConnection> connection;
    bool didShutdown = false;
    {
      std::lock_guard lock(mutex_);
      if (!shutdown_) {
        shutdown_ = true;
        didShutdown = true;
        ++generation_;
        state_ = ConnectionState::Idle;
        connection = std::move(connection_);
        for (auto& [id, operation] : operations_) {
          (void)id;
          operation->control->cancelled.store(true);
          operation->control->active.store(false);
          operations.push_back(std::move(operation));
        }
        operations_.clear();
      }
    }
    if (didShutdown) {
      timerCv_.notify_all();
      if (connection) connection->close(1000, "client closed");
    }
    if (timerThread_.joinable() &&
        timerThread_.get_id() != std::this_thread::get_id()) {
      timerThread_.join();
    }
  }

  std::size_t poll() { return dispatcher_->drain(); }

  std::string endpoint() const {
    std::lock_guard lock(mutex_);
    return endpoint_;
  }

  /// Move the socket to a different origin and reconnect onto it immediately,
  /// replaying every live subscription. Same shape as credentialsChanged():
  /// bump the generation so in-flight frames from the old socket are ignored,
  /// then let the timer thread reconnect with no backoff, because a move is a
  /// deliberate action rather than a failure to wait out.
  /// Ask `handler` for a better endpoint once `afterFailures` consecutive
  /// reconnects have failed. Zero disables it.
  void setRepeatedFailureHandler(int afterFailures, std::function<void()> handler) {
    std::lock_guard lock(mutex_);
    rediscoverAfterFailures_ = afterFailures;
    onRepeatedFailure_ = std::move(handler);
  }

  bool setEndpoint(std::string_view next) {
    Result<std::string> normalized =
        normalizeGraphQLWebSocketUrl(next, config_.endpointKind);
    if (!normalized.ok() || normalized.value().empty()) return false;

    std::shared_ptr<IWebSocketConnection> connection;
    bool reconnect = false;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || normalized.value() == endpoint_) return false;
      endpoint_ = normalized.value();
      // With nothing subscribed there is no socket to move: the new endpoint is
      // simply where the next subscribe() will connect.
      if (!operations_.empty()) {
        connection = std::move(connection_);
        ++generation_;
        currentReconnectAttempt_ = 1;
        state_ = ConnectionState::Backoff;
        reconnectDeadline_ = Clock::now();
        reconnect = true;
      }
    }
    if (reconnect) {
      ensureTimerThread();
      timerCv_.notify_all();
    }
    if (connection) connection->close(1000, "endpoint moved");
    return true;
  }

  std::shared_ptr<Dispatcher> dispatcher() const { return dispatcher_; }
  AuthState& auth() { return *auth_; }

 private:
  enum class ConnectionState {
    Idle,
    Connecting,
    AwaitingAcknowledgement,
    Ready,
    Backoff,
  };

  struct Operation {
    std::string id;
    GraphQLSubscriptionRequest request;
    GraphQLSubscriptionCallbacks callbacks;
    std::shared_ptr<SubscriptionHandle::Control> control;
    bool subscribedOnce = false;
  };

  void ensureTimerThread() {
    std::lock_guard lock(mutex_);
    if (timerStarted_ || shutdown_) return;
    timerStarted_ = true;
    timerThread_ = std::thread([this] { timerLoop(); });
  }

  void timerLoop() {
    std::unique_lock lock(mutex_);
    while (!shutdown_) {
      if (state_ != ConnectionState::AwaitingAcknowledgement &&
          state_ != ConnectionState::Backoff) {
        timerCv_.wait(lock, [this] {
          return shutdown_ ||
                 state_ == ConnectionState::AwaitingAcknowledgement ||
                 state_ == ConnectionState::Backoff;
        });
        continue;
      }

      const ConnectionState observedState = state_;
      const Clock::time_point deadline =
          observedState == ConnectionState::AwaitingAcknowledgement
              ? acknowledgementDeadline_
              : reconnectDeadline_;
      timerCv_.wait_until(lock, deadline);
      if (shutdown_ || state_ != observedState ||
          Clock::now() < deadline) {
        continue;
      }

      if (observedState == ConnectionState::AwaitingAcknowledgement) {
        const std::uint64_t generation = generation_;
        auto connection = connection_;
        lock.unlock();
        if (connection) connection->close(4408, "connection acknowledgement timeout");
        WebSocketError error;
        error.kind = WebSocketErrorKind::Timeout;
        error.status = Errc::Timeout;
        error.message = "Timed out waiting for connection_ack";
        error.retryable = true;
        handleConnectionFailure(generation, mapTransportError(error));
        lock.lock();
      } else {
        const std::size_t attempt = currentReconnectAttempt_;
        lock.unlock();
        startConnection(attempt);
        lock.lock();
      }
    }
  }

  void startConnection(std::size_t attempt) {
    std::uint64_t generation = 0;
    WebSocketConnectRequest request;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || operations_.empty()) return;
      if (state_ != ConnectionState::Idle &&
          state_ != ConnectionState::Backoff) {
        return;
      }
      state_ = ConnectionState::Connecting;
      generation = ++generation_;
      connectingAttempt_ = attempt;
      request.url = endpoint_;
      request.connectTimeoutMs = config_.options.connectTimeoutMs;
      request.maxFrameBytes = config_.options.maxFrameBytes;
    }
    timerCv_.notify_all();

    std::shared_ptr<IWebSocketConnection> connection;
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      connection = transport_->createConnection(request);
    } catch (const std::exception& error) {
      WebSocketError transportError;
      transportError.kind = WebSocketErrorKind::Connection;
      transportError.status = Errc::SocketError;
      transportError.message =
          std::string("WebSocket connection creation failed: ") + error.what();
      handleConnectionFailure(generation, mapTransportError(transportError));
      return;
    }
#else
    connection = transport_->createConnection(request);
#endif
    if (!connection) {
      WebSocketError transportError;
      transportError.kind = WebSocketErrorKind::Unavailable;
      transportError.status = Errc::NotConnected;
      transportError.message = "WebSocket transport could not create a connection";
      handleConnectionFailure(generation, mapTransportError(transportError));
      return;
    }

    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_ ||
          state_ != ConnectionState::Connecting) {
        connection->close(1000, "stale connection");
        return;
      }
      connection_ = connection;
    }

    auto weak = weak_from_this();
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      connection->start([weak, generation](WebSocketEvent event) mutable {
        if (auto self = weak.lock()) {
          self->handleEvent(generation, std::move(event));
        }
      });
    } catch (const std::exception& error) {
      WebSocketError transportError;
      transportError.kind = WebSocketErrorKind::Connection;
      transportError.status = Errc::SocketError;
      transportError.message =
          std::string("WebSocket transport start failed: ") + error.what();
      handleConnectionFailure(generation, mapTransportError(transportError));
    }
#else
    connection->start([weak, generation](WebSocketEvent event) mutable {
      if (auto self = weak.lock()) self->handleEvent(generation, std::move(event));
    });
#endif
  }

  void handleEvent(std::uint64_t generation, WebSocketEvent event) {
    std::lock_guard eventLock(eventMutex_);
    switch (event.kind) {
      case WebSocketEventKind::Open:
        handleOpen(generation);
        return;
      case WebSocketEventKind::Frame:
        handleFrame(generation, std::move(event.frame));
        return;
      case WebSocketEventKind::Close:
        handleConnectionFailure(generation, mapClose(event.close));
        return;
      case WebSocketEventKind::Error:
        handleConnectionFailure(generation, mapTransportError(event.error));
        return;
    }
  }

  bool generationIsCurrent(std::uint64_t generation) const {
    std::lock_guard lock(mutex_);
    return !shutdown_ && generation == generation_;
  }

  void handleOpen(std::uint64_t generation) {
    std::shared_ptr<IWebSocketConnection> connection;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_ ||
          state_ != ConnectionState::Connecting) {
        return;
      }
      state_ = ConnectionState::AwaitingAcknowledgement;
      acknowledgementDeadline_ =
          Clock::now() +
          std::chrono::milliseconds(
              std::max<long>(0, config_.options.acknowledgementTimeoutMs));
      connection = connection_;
    }
    timerCv_.notify_all();

    JVal message;
    message["type"] = "connection_init";
    const std::string token = auth_->token();
    if (!token.empty()) {
      JVal payload;
      payload["Authorization"] = "Bearer " + token;
      message["payload"] = std::move(payload);
    }
    sendConnectionFrame(generation, connection, message.dump());
  }

  void handleFrame(std::uint64_t generation, WebSocketFrame frame) {
    if (!generationIsCurrent(generation)) return;
    if (frame.payload.size() > config_.options.maxFrameBytes) {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::FrameTooLarge,
                        Errc::BufferTooSmall, "FRAME_TOO_LARGE",
                        "WebSocket message exceeded the configured frame limit"),
          1009);
      return;
    }

    if (frame.kind == WebSocketFrameKind::Ping) {
      std::shared_ptr<IWebSocketConnection> connection;
      {
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        connection = connection_;
      }
      if (connection) {
        const Status status =
            connection->send({WebSocketFrameKind::Pong, std::move(frame.payload)});
        if (!status.ok()) {
          WebSocketError error;
          error.kind = WebSocketErrorKind::Io;
          error.status = status;
          error.message = "Failed to send WebSocket pong";
          handleConnectionFailure(generation, mapTransportError(error));
        }
      }
      return;
    }
    if (frame.kind == WebSocketFrameKind::Pong) return;
    if (frame.kind != WebSocketFrameKind::Text) {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "BINARY_FRAME",
                        "graphql-transport-ws requires text JSON messages"),
          1003);
      return;
    }
    if (!validUtf8(frame.payload)) {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::InvalidUtf8,
                        Errc::Malformed, "INVALID_UTF8",
                        "WebSocket text message is not valid UTF-8"),
          1007);
      return;
    }

    const Json message = Json::parse(frame.payload);
    if (!message.isObject() || !message["type"].isString()) {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "INVALID_PROTOCOL_MESSAGE",
                        "WebSocket message is not a graphql-transport-ws object"),
          4400);
      return;
    }

    const std::string type = message["type"].asString();
    if (type == "connection_ack") {
      handleAcknowledgement(generation);
    } else if (type == "ping") {
      std::string pong = R"({"type":"pong")";
      const Json payload = message["payload"];
      if (payload.ok()) pong += R"(,"payload":)" + payload.dump();
      pong += '}';
      std::shared_ptr<IWebSocketConnection> connection;
      {
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        connection = connection_;
      }
      sendConnectionFrame(generation, connection, std::move(pong));
    } else if (type == "pong") {
      return;
    } else if (type == "next") {
      handleNext(generation, message["id"].asString(), message["payload"]);
    } else if (type == "error") {
      handleOperationError(generation, message["id"].asString(),
                           message["payload"]);
    } else if (type == "complete") {
      handleComplete(generation, message["id"].asString());
    } else {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "UNKNOWN_PROTOCOL_MESSAGE",
                        "Unknown graphql-transport-ws message type: " + type),
          4400);
    }
  }

  void handleAcknowledgement(std::uint64_t generation) {
    std::vector<std::shared_ptr<Operation>> operations;
    std::size_t reconnectAttempt = 0;
    bool unexpected = false;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_) return;
      if (state_ != ConnectionState::AwaitingAcknowledgement) {
        unexpected = true;
      } else {
        state_ = ConnectionState::Ready;
        reconnectAttempt = connectingAttempt_;
        currentReconnectAttempt_ = 0;
        for (auto& [id, operation] : operations_) {
          (void)id;
          operations.push_back(operation);
        }
      }
    }
    if (unexpected) {
      failConnection(
          generation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "UNEXPECTED_CONNECTION_ACK",
                        "Received connection_ack outside the handshake"),
          4400);
      return;
    }
    timerCv_.notify_all();

    for (const auto& operation : operations) {
      if (!operation->control->active.load()) continue;
      const bool replay = operation->subscribedOnce;
      operation->subscribedOnce = true;
      if (replay && operation->callbacks.onReconnect) {
        postReconnect(operation,
                      GraphQLReconnectInfo{reconnectAttempt, operation->id});
      }
      sendSubscribe(operation, generation);
    }
  }

  void handleNext(std::uint64_t generation, const std::string& id,
                  Json payload) {
    std::shared_ptr<Operation> operation;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_ ||
          state_ != ConnectionState::Ready) {
        return;
      }
      const auto found = operations_.find(id);
      if (found == operations_.end()) return;
      operation = found->second;
    }
    if (!payload.isObject()) {
      finishOperationError(
          operation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "INVALID_NEXT_PAYLOAD",
                        "GraphQL subscription next payload is not an object"));
      return;
    }

    GraphQLSubscriptionOutcome outcome;
    outcome.payload = payload;
    outcome.data = payload["data"];
    outcome.errors = parseGraphQLErrors(payload["errors"]);
    if (!outcome.errors.empty()) outcome.status = Errc::Rejected;
    if (!outcome.data.ok() && outcome.errors.empty()) {
      finishOperationError(
          operation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "EMPTY_NEXT_PAYLOAD",
                        "GraphQL subscription next payload has no data or errors"));
      return;
    }
    postNext(operation, std::move(outcome));
  }

  void handleOperationError(std::uint64_t generation, const std::string& id,
                            Json payload) {
    std::shared_ptr<Operation> operation;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_) return;
      const auto found = operations_.find(id);
      if (found == operations_.end()) return;
      operation = found->second;
    }
    auto errors = parseGraphQLErrors(payload);
    if (errors.empty()) {
      finishOperationError(
          operation,
          protocolError(GraphQLSubscriptionErrorKind::Protocol, Errc::Malformed,
                        "INVALID_ERROR_PAYLOAD",
                        "GraphQL subscription error payload is malformed"));
      return;
    }
    finishOperationError(operation, classifyGraphQLError(std::move(errors)));
  }

  void handleComplete(std::uint64_t generation, const std::string& id) {
    std::shared_ptr<Operation> operation;
    std::shared_ptr<IWebSocketConnection> closeConnection;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_) return;
      const auto found = operations_.find(id);
      if (found == operations_.end()) return;
      operation = found->second;
      operations_.erase(found);
      operation->control->active.store(false);
      if (operations_.empty()) closeConnection = becomeIdleLocked();
    }
    timerCv_.notify_all();
    if (closeConnection) closeConnection->close(1000, "all subscriptions complete");
    postComplete(operation);
  }

  void sendSubscribe(const std::shared_ptr<Operation>& operation,
                     std::uint64_t generation) {
    if (!operation->control->active.load()) return;
    JVal payload;
    payload["query"] = operation->request.document;
    if (!operation->request.variables.isNull()) {
      payload["variables"] = operation->request.variables;
    }
    if (!operation->request.operationName.empty()) {
      payload["operationName"] = operation->request.operationName;
    }
    JVal message;
    message["id"] = operation->id;
    message["type"] = "subscribe";
    message["payload"] = std::move(payload);
    std::string text = message.dump();
    if (text.size() > config_.options.maxFrameBytes) {
      finishOperationError(
          operation,
          protocolError(GraphQLSubscriptionErrorKind::FrameTooLarge,
                        Errc::BufferTooSmall, "FRAME_TOO_LARGE",
                        "GraphQL subscribe message exceeded the frame limit"));
      return;
    }
    if (!validUtf8(text)) {
      finishOperationError(
          operation,
          protocolError(GraphQLSubscriptionErrorKind::InvalidUtf8,
                        Errc::Malformed, "INVALID_UTF8",
                        "GraphQL subscribe message is not valid UTF-8"));
      return;
    }

    std::shared_ptr<IWebSocketConnection> connection;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_ ||
          state_ != ConnectionState::Ready ||
          operations_.find(operation->id) == operations_.end()) {
        return;
      }
      connection = connection_;
    }
    sendConnectionFrame(generation, connection, std::move(text));
  }

  void sendConnectionFrame(std::uint64_t generation,
                           const std::shared_ptr<IWebSocketConnection>& connection,
                           std::string text) {
    if (!connection || !generationIsCurrent(generation)) return;
    if (text.size() > config_.options.maxFrameBytes || !validUtf8(text)) {
      failConnection(
          generation,
          protocolError(
              text.size() > config_.options.maxFrameBytes
                  ? GraphQLSubscriptionErrorKind::FrameTooLarge
                  : GraphQLSubscriptionErrorKind::InvalidUtf8,
              text.size() > config_.options.maxFrameBytes ? Errc::BufferTooSmall
                                                          : Errc::Malformed,
              text.size() > config_.options.maxFrameBytes ? "FRAME_TOO_LARGE"
                                                          : "INVALID_UTF8",
              "Outbound GraphQL WebSocket message violates frame limits"),
          text.size() > config_.options.maxFrameBytes ? 1009 : 1007);
      return;
    }
    const Status status =
        connection->send({WebSocketFrameKind::Text, std::move(text)});
    if (!status.ok()) {
      WebSocketError error;
      error.kind = WebSocketErrorKind::Io;
      error.status = status;
      error.message = "Failed to send GraphQL WebSocket message";
      error.retryable = true;
      handleConnectionFailure(generation, mapTransportError(error));
    }
  }

  void cancelOperation(const std::string& id) {
    std::shared_ptr<IWebSocketConnection> connection;
    std::shared_ptr<IWebSocketConnection> closeConnection;
    bool sendComplete = false;
    {
      std::lock_guard lock(mutex_);
      const auto found = operations_.find(id);
      if (found == operations_.end()) return;
      operations_.erase(found);
      if (state_ == ConnectionState::Ready && connection_) {
        connection = connection_;
        sendComplete = true;
      }
      if (operations_.empty()) closeConnection = becomeIdleLocked();
    }
    timerCv_.notify_all();

    if (sendComplete && connection) {
      JVal message;
      message["id"] = id;
      message["type"] = "complete";
      const std::string text = message.dump();
      if (text.size() <= config_.options.maxFrameBytes) {
        (void)connection->send({WebSocketFrameKind::Text, text});
      }
    }
    if (closeConnection) closeConnection->close(1000, "subscription cancelled");
  }

  void finishOperationError(const std::shared_ptr<Operation>& operation,
                            GraphQLSubscriptionError error) {
    std::shared_ptr<IWebSocketConnection> closeConnection;
    {
      std::lock_guard lock(mutex_);
      const auto found = operations_.find(operation->id);
      if (found == operations_.end() || found->second != operation) return;
      operations_.erase(found);
      operation->control->active.store(false);
      if (operations_.empty()) closeConnection = becomeIdleLocked();
    }
    timerCv_.notify_all();
    if (closeConnection) closeConnection->close(1000, "subscription failed");
    postError(operation, std::move(error));
  }

  std::shared_ptr<IWebSocketConnection> becomeIdleLocked() {
    ++generation_;
    state_ = ConnectionState::Idle;
    currentReconnectAttempt_ = 0;
    connectingAttempt_ = 0;
    return std::move(connection_);
  }

  void failConnection(std::uint64_t generation,
                      GraphQLSubscriptionError error,
                      std::uint16_t closeCode) {
    error.closeCode = closeCode;
    failAll(generation, std::move(error), closeCode);
  }

  void handleConnectionFailure(std::uint64_t generation,
                               GraphQLSubscriptionError error) {
    if (error.terminal || !error.retryable) {
      failAll(generation, std::move(error));
      return;
    }

    std::vector<std::shared_ptr<Operation>> exhausted;
    std::shared_ptr<IWebSocketConnection> connection;
    GraphQLSubscriptionError finalError;
    bool terminal = false;
    std::function<void()> rediscover;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_) return;
      connection = std::move(connection_);
      ++generation_;
      if (operations_.empty()) {
        state_ = ConnectionState::Idle;
        currentReconnectAttempt_ = 0;
      } else if (config_.options.maxReconnectAttempts == 0) {
        terminal = true;
        finalError = std::move(error);
      } else if (currentReconnectAttempt_ >=
                 config_.options.maxReconnectAttempts) {
        terminal = true;
        finalError = protocolError(
            GraphQLSubscriptionErrorKind::ReconnectExhausted,
            error.status, "RECONNECT_EXHAUSTED",
            "GraphQL WebSocket reconnect limit reached: " + error.message);
        finalError.closeCode = error.closeCode;
      } else {
        ++currentReconnectAttempt_;
        state_ = ConnectionState::Backoff;
        reconnectDeadline_ =
            Clock::now() +
            std::chrono::milliseconds(reconnectDelayLocked(currentReconnectAttempt_));
        // A single blip is what backoff is for. Repeated failures against the
        // same URL are the signal that the URL itself is the problem, and no
        // number of further retries can fix that.
        if (rediscoverAfterFailures_ > 0 &&
            currentReconnectAttempt_ >=
                static_cast<std::size_t>(rediscoverAfterFailures_) &&
            onRepeatedFailure_) {
          rediscover = onRepeatedFailure_;
        }
      }

      if (terminal) {
        state_ = ConnectionState::Idle;
        currentReconnectAttempt_ = 0;
        for (auto& [id, operation] : operations_) {
          (void)id;
          operation->control->active.store(false);
          exhausted.push_back(std::move(operation));
        }
        operations_.clear();
      }
    }
    timerCv_.notify_all();
    if (connection) connection->close(1001, "reconnecting");
    // Outside the lock: re-discovery makes a network call and may move this very
    // client's endpoint, which would deadlock against mutex_.
    if (rediscover) rediscover();
    for (const auto& operation : exhausted) postError(operation, finalError);
  }

  long reconnectDelayLocked(std::size_t attempt) {
    const long initial = std::max<long>(0, config_.options.initialReconnectDelayMs);
    const long maximum =
        std::max<long>(initial, config_.options.maxReconnectDelayMs);
    long base = initial;
    for (std::size_t n = 1; n < attempt && base < maximum; ++n) {
      if (base > maximum / 2) {
        base = maximum;
      } else {
        base *= 2;
      }
    }
    base = std::min(base, maximum);

    const double jitter =
        std::clamp(config_.options.reconnectJitter, 0.0, 1.0);
    if (base == 0 || jitter == 0.0) return base;
    std::uniform_real_distribution<double> distribution(-jitter, jitter);
    const double varied =
        static_cast<double>(base) * (1.0 + distribution(random_));
    return std::clamp<long>(
        static_cast<long>(std::llround(varied)), 0, maximum);
  }

  void failAll(std::uint64_t generation, GraphQLSubscriptionError error,
               std::uint16_t closeCode = 1000) {
    std::vector<std::shared_ptr<Operation>> operations;
    std::shared_ptr<IWebSocketConnection> connection;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation != generation_) return;
      ++generation_;
      state_ = ConnectionState::Idle;
      currentReconnectAttempt_ = 0;
      connection = std::move(connection_);
      for (auto& [id, operation] : operations_) {
        (void)id;
        operation->control->active.store(false);
        operations.push_back(std::move(operation));
      }
      operations_.clear();
    }
    timerCv_.notify_all();
    if (connection) connection->close(closeCode, error.code);
    for (const auto& operation : operations) postError(operation, error);
  }

  void credentialsChanged() {
    std::shared_ptr<IWebSocketConnection> connection;
    bool reconnect = false;
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || operations_.empty()) return;
      connection = std::move(connection_);
      ++generation_;
      currentReconnectAttempt_ = 1;
      state_ = ConnectionState::Backoff;
      reconnectDeadline_ = Clock::now();
      reconnect = true;
    }
    if (reconnect) {
      ensureTimerThread();
      timerCv_.notify_all();
    }
    if (connection) connection->close(1000, "credentials changed");
  }

  void postNext(const std::shared_ptr<Operation>& operation,
                GraphQLSubscriptionOutcome outcome) {
    if (!operation->callbacks.onNext) return;
    dispatcher_->post([operation, outcome = std::move(outcome)]() mutable {
      std::lock_guard deliveryLock(operation->control->deliveryMutex);
      if (!operation->control->cancelled.load(std::memory_order_acquire) &&
          operation->callbacks.onNext) {
        operation->callbacks.onNext(std::move(outcome));
      }
    });
  }

  void postError(const std::shared_ptr<Operation>& operation,
                 GraphQLSubscriptionError error) {
    if (!operation->callbacks.onError) return;
    dispatcher_->post([operation, error = std::move(error)]() mutable {
      std::lock_guard deliveryLock(operation->control->deliveryMutex);
      if (!operation->control->cancelled.load(std::memory_order_acquire) &&
          operation->callbacks.onError) {
        operation->callbacks.onError(std::move(error));
      }
    });
  }

  void postComplete(const std::shared_ptr<Operation>& operation) {
    if (!operation->callbacks.onComplete) return;
    dispatcher_->post([operation] {
      std::lock_guard deliveryLock(operation->control->deliveryMutex);
      if (!operation->control->cancelled.load(std::memory_order_acquire) &&
          operation->callbacks.onComplete) {
        operation->callbacks.onComplete();
      }
    });
  }

  void postReconnect(const std::shared_ptr<Operation>& operation,
                     GraphQLReconnectInfo info) {
    dispatcher_->post([operation, info = std::move(info)]() mutable {
      std::lock_guard deliveryLock(operation->control->deliveryMutex);
      if (!operation->control->cancelled.load(std::memory_order_acquire) &&
          operation->callbacks.onReconnect) {
        operation->callbacks.onReconnect(std::move(info));
      }
    });
  }

  GraphQLSubscriptionClientConfig config_;
  std::shared_ptr<IWebSocketTransport> transport_;
  std::shared_ptr<AuthState> auth_;
  std::shared_ptr<Dispatcher> dispatcher_;
  std::string endpoint_;

  mutable std::mutex mutex_;
  std::mutex shutdownMutex_;
  std::recursive_mutex eventMutex_;
  std::condition_variable timerCv_;
  std::thread timerThread_;
  bool timerStarted_ = false;
  bool shutdown_ = false;
  ConnectionState state_ = ConnectionState::Idle;
  std::uint64_t generation_ = 0;
  std::uint64_t nextOperationId_ = 1;
  std::size_t currentReconnectAttempt_ = 0;
  int rediscoverAfterFailures_ = 0;
  std::function<void()> onRepeatedFailure_;
  std::size_t connectingAttempt_ = 0;
  Clock::time_point acknowledgementDeadline_{};
  Clock::time_point reconnectDeadline_{};
  std::shared_ptr<IWebSocketConnection> connection_;
  std::unordered_map<std::string, std::shared_ptr<Operation>> operations_;
  std::mt19937 random_;
};

GraphQLSubscriptionClient::GraphQLSubscriptionClient(
    GraphQLSubscriptionClientConfig config,
    std::shared_ptr<IWebSocketTransport> transport,
    std::shared_ptr<AuthState> auth,
    std::shared_ptr<Dispatcher> dispatcher)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(transport),
                                   std::move(auth), std::move(dispatcher))) {
  impl_->initialize();
}

GraphQLSubscriptionClient::GraphQLSubscriptionClient(
    GraphQLSubscriptionClientConfig config,
    std::shared_ptr<IWebSocketTransport> transport,
    GraphQLClient& httpClient)
    : GraphQLSubscriptionClient(std::move(config), std::move(transport),
                                httpClient.sharedAuthState(),
                                ensureDispatcher(httpClient)) {}

GraphQLSubscriptionClient::~GraphQLSubscriptionClient() {
  if (impl_) impl_->shutdown();
}

GraphQLSubscriptionClient::GraphQLSubscriptionClient(
    GraphQLSubscriptionClient&&) noexcept = default;

GraphQLSubscriptionClient& GraphQLSubscriptionClient::operator=(
    GraphQLSubscriptionClient&&) noexcept = default;

SubscriptionHandle GraphQLSubscriptionClient::subscribe(
    GraphQLSubscriptionRequest request,
    GraphQLSubscriptionCallbacks callbacks) {
  return impl_->subscribe(std::move(request), std::move(callbacks));
}

SubscriptionHandle GraphQLSubscriptionClient::subscribe(
    std::string_view document, const JVal& variables,
    std::string_view operationName, GraphQLSubscriptionCallbacks callbacks) {
  GraphQLSubscriptionRequest request;
  request.document = std::string(document);
  request.variables = variables;
  request.operationName = std::string(operationName);
  return subscribe(std::move(request), std::move(callbacks));
}

void GraphQLSubscriptionClient::close() {
  if (impl_) impl_->shutdown();
}

std::size_t GraphQLSubscriptionClient::poll() {
  return impl_ ? impl_->poll() : 0;
}

std::string GraphQLSubscriptionClient::endpoint() const {
  return impl_ ? impl_->endpoint() : std::string();
}

bool GraphQLSubscriptionClient::setEndpoint(std::string_view endpoint) {
  return impl_ ? impl_->setEndpoint(endpoint) : false;
}

void GraphQLSubscriptionClient::setRepeatedFailureHandler(
    int afterFailures, std::function<void()> handler) {
  if (impl_) impl_->setRepeatedFailureHandler(afterFailures, std::move(handler));
}

std::shared_ptr<Dispatcher> GraphQLSubscriptionClient::dispatcher() const {
  return impl_ ? impl_->dispatcher() : nullptr;
}

AuthState& GraphQLSubscriptionClient::auth() { return impl_->auth(); }

}  // namespace crowdy::graphql
