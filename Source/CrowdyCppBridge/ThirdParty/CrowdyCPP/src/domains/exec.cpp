#include "crowdy/domains/exec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#include "crowdy/core/base64.hpp"
#include "crowdy/default_origin.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/estate.hpp"

namespace crowdy::domains {

// ---- statuses ----------------------------------------------------------------

std::string_view execStatusName(ExecStatus status) noexcept {
  switch (status) {
    case ExecStatus::Ok: return "Ok";
    case ExecStatus::AppError: return "AppError";
    case ExecStatus::Busy: return "Busy";
    case ExecStatus::Moved: return "Moved";
    case ExecStatus::NotFound: return "NotFound";
    case ExecStatus::DeadlineExceeded: return "DeadlineExceeded";
    case ExecStatus::Denied: return "Denied";
    case ExecStatus::RateLimited: return "RateLimited";
    case ExecStatus::Unavailable: return "Unavailable";
    case ExecStatus::Internal: return "Internal";
    case ExecStatus::Trapped: return "Trapped";
    case ExecStatus::BadRequest: return "BadRequest";
    case ExecStatus::Unknown: break;
  }
  return "Unknown";
}

ExecStatus execStatusFromWire(std::uint8_t value) noexcept {
  return value <= 11 ? static_cast<ExecStatus>(value) : ExecStatus::Unknown;
}

bool execStatusRetryable(ExecStatus status) noexcept {
  return status == ExecStatus::Busy || status == ExecStatus::Moved ||
         status == ExecStatus::Unavailable || status == ExecStatus::RateLimited;
}

bool ExecReply::rateLimited() const {
  constexpr std::string_view kRateLimited = "rate limited";
  return status == ExecStatus::RateLimited ||
         (status == ExecStatus::Busy && std::string_view(payload).substr(0, kRateLimited.size()) == kRateLimited);
}

std::optional<long> ExecReply::retryAfterMs() const {
  if (!rateLimited()) return std::nullopt;
  constexpr std::string_view kRetryIn = "retry in ";
  const std::string_view text(payload);
  const auto at = text.find(kRetryIn);
  if (at == std::string_view::npos) return std::nullopt;
  std::size_t i = at + kRetryIn.size();
  long ms = 0;
  bool digits = false;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    if (ms > 100000000L) return std::nullopt;
    ms = ms * 10 + (text[i] - '0');
    digits = true;
  }
  if (!digits || text.substr(i, 3) != " ms") return std::nullopt;
  return ms;
}

// ---- frames ------------------------------------------------------------------

namespace exec_wire {

namespace {

void putU16(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void putU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

bool putStr8(std::string& out, std::string_view s) {
  if (s.size() > 0xff) return false;
  out.push_back(static_cast<char>(s.size()));
  out.append(s);
  return true;
}

bool putStr16(std::string& out, std::string_view s) {
  if (s.size() > 0xffff) return false;
  putU16(out, static_cast<std::uint32_t>(s.size()));
  out.append(s);
  return true;
}

}  // namespace

Result<std::string> encode(const ClientFrame& f) {
  std::string out;
  using K = ClientFrame::Kind;
  if (f.kind == K::Ping) {
    out.push_back(static_cast<char>(0x04));
    putU32(out, f.rid);
    return out;
  }
  out.push_back(static_cast<char>(f.kind == K::Call ? 0x01 : f.kind == K::Subscribe ? 0x02 : 0x03));
  putU32(out, f.rid);
  if (!putStr8(out, f.nodeType) || !putStr16(out, f.key) || !putStr8(out, f.method)) {
    return Errc::InvalidArgument;
  }
  if (f.kind == K::Call) out.append(f.payload);
  return out;
}

Result<ServerFrame> decode(std::string_view b) {
  std::size_t at = 0;
  auto have = [&](std::size_t n) { return b.size() - at >= n; };
  auto u8 = [&]() { return static_cast<std::uint8_t>(b[at++]); };
  auto u16 = [&]() {
    const std::uint32_t v = static_cast<std::uint8_t>(b[at]) | (static_cast<std::uint8_t>(b[at + 1]) << 8);
    at += 2;
    return v;
  };
  auto u32 = [&]() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + i])) << (8 * i);
    at += 4;
    return v;
  };
  auto str = [&](std::size_t n, std::string& out) {
    if (!have(n)) return false;
    out.assign(b.substr(at, n));
    at += n;
    return true;
  };
  if (!have(1)) return Errc::Malformed;
  ServerFrame f;
  switch (u8()) {
    case 0x81:
      if (!have(5)) return Errc::Malformed;
      f.kind = ServerFrame::Kind::Reply;
      f.rid = u32();
      f.status = u8();
      f.payload.assign(b.substr(at));
      return f;
    case 0x82: {
      f.kind = ServerFrame::Kind::Push;
      if (!have(1) || !str(u8(), f.nodeType)) return Errc::Malformed;
      if (!have(2)) return Errc::Malformed;
      if (!str(u16(), f.key)) return Errc::Malformed;
      if (!have(1) || !str(u8(), f.topic)) return Errc::Malformed;
      f.payload.assign(b.substr(at));
      return f;
    }
    case 0x84:
      if (!have(4)) return Errc::Malformed;
      f.kind = ServerFrame::Kind::Pong;
      f.rid = u32();
      return f;
    default:
      return Errc::Malformed;
  }
}

}  // namespace exec_wire

namespace {

/// `s` as a query-string value: everything outside RFC 3986's unreserved set percent-encoded,
/// as CrowdyJS's `encodeURIComponent` does for the connect token.
std::string queryValue(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

// ---- where a connect token may go ----

std::string lowercase(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/// The scheme of an absolute URL, lowercased, or empty when `url` is not one.
std::string schemeOf(std::string_view url) {
  const std::size_t sep = url.find("://");
  if (sep == std::string_view::npos || sep == 0) return {};
  for (unsigned char c : url.substr(0, sep)) {
    if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return {};
  }
  return lowercase(url.substr(0, sep));
}

/// Whether the authority of an absolute URL carries userinfo (`user:pass@`).
bool hasUserinfo(std::string_view url) {
  std::string_view rest = url.substr(url.find("://") + 3);
  return rest.substr(0, rest.find_first_of("/?#")).find('@') != std::string_view::npos;
}

bool isIpLiteral(std::string_view host) {
  if (!host.empty() && host.front() == '[') return true;
  int parts = 0;
  std::size_t digits = 0;
  for (char c : host) {
    if (c == '.') {
      if (digits == 0) return false;
      ++parts;
      digits = 0;
    } else if (c >= '0' && c <= '9' && digits < 3) {
      ++digits;
    } else {
      return false;
    }
  }
  return parts == 3 && digits > 0;
}

bool isLoopback(std::string_view host) {
  constexpr std::string_view suffix = ".localhost";
  return host == "localhost" || host == "[::1]" ||
         (host.size() > suffix.size() && host.substr(host.size() - suffix.size()) == suffix) ||
         (isIpLiteral(host) && host.substr(0, 4) == "127.");
}

/// One host, or two DNS names graphql::isSameEstate puts on one estate. Never two IPs.
bool sameEstate(std::string_view urlA, const std::string& hostA, std::string_view urlB,
                const std::string& hostB) {
  if (hostA == hostB) return true;
  if (isIpLiteral(hostA) || isIpLiteral(hostB)) return false;
  return graphql::isSameEstate(urlA, urlB);
}

}  // namespace

std::optional<std::string> execGatewayRefusal(std::string_view gameApiUrl, std::string_view gatewayUrl) {
  const std::string scheme = schemeOf(gatewayUrl);
  const auto gateway = graphql::estateHostname(gatewayUrl);
  if (scheme.empty() || !gateway) return "it is not an absolute URL";
  if (scheme != "ws" && scheme != "wss") return scheme + ": is not a WebSocket scheme";
  if (hasUserinfo(gatewayUrl)) return "it carries credentials";
  const std::string apiScheme = schemeOf(gameApiUrl);
  const auto api = graphql::estateHostname(gameApiUrl);
  if (apiScheme.empty() || !api) return "the game API URL " + std::string(gameApiUrl) + " is not absolute";
  if (apiScheme == "https" && scheme != "wss") return "a game API on https: hands out wss: gateways only";
  if (sameEstate(gameApiUrl, *api, gatewayUrl, *gateway)) return std::nullopt;
  const auto tier = graphql::estateHostname(kDefaultHttpOrigin);
  if (tier && sameEstate(kDefaultHttpOrigin, *tier, gatewayUrl, *gateway)) return std::nullopt;
  if (isLoopback(*api) && isLoopback(*gateway)) return std::nullopt;
  const std::string estates = tier && *tier != *api ? *api + " and " + *tier : *api;
  return *gateway + " is outside the estate of " + estates;
}

// ---- SHA-256 (FIPS 180-4), for deploy digests ------------------------------------

std::string execSha256Hex(std::string_view bytes) {
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  std::string msg(bytes);
  const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + 4 * i);
      w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto v = h;
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (int i = 0; i < 8; ++i) h[i] += v[i];
  }
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::uint32_t word : h) {
    for (int i = 28; i >= 0; i -= 4) out.push_back(digits[(word >> i) & 0xf]);
  }
  return out;
}

// ---- the connection ----------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using Kind = exec_wire::ClientFrame::Kind;

class ExecConnection::Impl : public std::enable_shared_from_this<Impl> {
 public:
  Impl(std::shared_ptr<graphql::IWebSocketTransport> transport,
       std::shared_ptr<graphql::Dispatcher> dispatcher, ExecDial dial, ExecConnectOptions options)
      : transport_(std::move(transport)),
        dispatcher_(std::move(dispatcher)),
        dial_(std::move(dial)),
        opts_(std::move(options)),
        backoffMs_(opts_.initialReconnectDelayMs) {}

  ~Impl() { stopTimer(); }

  // A request not yet answered: sent on connection `gen`, or waiting for one (gen 0).
  struct Pending {
    exec_wire::ClientFrame frame;
    ExecReplyCallback done;
    Clock::time_point deadline;
    std::uint64_t gen = 0;
    bool retried = false;
    bool internal = false;  // a subscription renewal: nobody waits on it
  };

  void connect(std::function<void(Status)> done) {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard lock(mu_);
      if (closed_) {
        if (done) run.push_back([done] { done(Errc::Closed); });
      } else if (ws_ && open_) {
        if (done) run.push_back([done] { done(Errc::Ok); });
      } else {
        if (done) waiters_.push_back(std::move(done));
        if (!dialing_ && !ws_) beginDialLocked(run);
      }
    }
    deliver(std::move(run));
  }

  void request(exec_wire::ClientFrame frame, ExecReplyCallback done, bool internal = false) {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard lock(mu_);
      if (closed_) {
        if (done) run.push_back([done] { done(ExecReply{ExecStatus::Unavailable, "the connection is closed"}); });
      } else {
        const std::uint32_t rid = nextRid();
        frame.rid = rid;
        Pending p{std::move(frame), std::move(done), Clock::now() + std::chrono::milliseconds(opts_.callTimeoutMs)};
        p.internal = internal;
        pending_.emplace(rid, std::move(p));
        if (ws_ && open_) {
          sendLocked(rid);
        } else if (!dialing_ && !ws_) {
          beginDialLocked(run);
        }
        ensureTimerLocked();
      }
    }
    deliver(std::move(run));
    flushOut();
  }

  std::uint64_t subscribe(std::string nodeType, std::string key, std::string topic,
                          ExecPushHandler onPush, ExecReplyCallback done) {
    const std::string k = subKey(nodeType, key, topic);
    bool first = false;
    std::uint64_t handle = 0;
    {
      std::lock_guard lock(mu_);
      handle = ++nextHandle_;
      auto& sub = subs_[k];
      first = sub.handlers.empty();
      sub.nodeType = nodeType;
      sub.key = key;
      sub.topic = topic;
      sub.handlers.emplace(handle, std::move(onPush));
      handles_.emplace(handle, k);
    }
    if (first) {
      exec_wire::ClientFrame f{Kind::Subscribe, 0, std::move(nodeType), std::move(key), std::move(topic), {}};
      auto weak = weak_from_this();
      request(std::move(f), [weak, handle, done](ExecReply r) {
        if (!r.ok()) {
          if (auto self = weak.lock()) self->dropHandler(handle, false);
        }
        if (done) done(std::move(r));
      });
    } else if (done) {
      deliver({[done] { done(ExecReply{ExecStatus::Ok, {}}); }});
    }
    return handle;
  }

  void unsubscribe(std::uint64_t handle) { dropHandler(handle, true); }

  void onReconnect(std::function<void(std::string)> listener) {
    std::lock_guard lock(mu_);
    reconnectListeners_.push_back(std::move(listener));
  }

  std::string host() const {
    std::lock_guard lock(mu_);
    return endpoint_.host;
  }

  bool connected() const {
    std::lock_guard lock(mu_);
    return ws_ && open_;
  }

  std::optional<ExecReply> lastFailure() const {
    std::lock_guard lock(mu_);
    return lastFailure_;
  }

  void pinGateway(std::function<std::string()> gameApiUrl) {
    std::lock_guard lock(mu_);
    gatewayPin_ = std::move(gameApiUrl);
  }

  void close() {
    std::vector<std::function<void()>> run;
    std::shared_ptr<graphql::IWebSocketConnection> ws;
    {
      std::lock_guard lock(mu_);
      if (closed_) return;
      closed_ = true;
      ws = std::move(ws_);
      open_ = false;
      failAllLocked(run, "the connection is closed");
      for (auto& w : waiters_) run.push_back([w] { w(Errc::Closed); });
      waiters_.clear();
    }
    if (ws) ws->close(1000, "client closed");
    cv_.notify_all();
    deliver(std::move(run));
    stopTimer();
  }

  /// Dial a known endpoint rather than asking for one (ExecConnection::open).
  void useFixedEndpoint(ExecEndpoint e) {
    dial_ = [e](std::function<void(Result<ExecEndpoint>)> found) { found(e); };
  }

 private:
  struct Sub {
    std::string nodeType, key, topic;
    std::map<std::uint64_t, ExecPushHandler> handlers;
  };

  static std::string subKey(std::string_view t, std::string_view k, std::string_view topic) {
    std::string s(t);
    s.push_back('\0');
    s.append(k);
    s.push_back('\0');
    s.append(topic);
    return s;
  }

  std::uint32_t nextRid() {
    const std::uint32_t rid = rid_;
    rid_ = rid_ == 0xffffffffu ? 1 : rid_ + 1;
    return rid;
  }

  void post(std::vector<std::function<void()>>& run, std::function<void()> fn) { run.push_back(std::move(fn)); }

  void deliver(std::vector<std::function<void()>> run) {
    for (auto& fn : run) {
      if (dispatcher_) {
        dispatcher_->post(std::move(fn));
      } else {
        fn();
      }
    }
  }

  void sendLocked(std::uint32_t rid) {
    auto it = pending_.find(rid);
    if (it == pending_.end()) return;
    it->second.gen = gen_;
    Result<std::string> bytes = exec_wire::encode(it->second.frame);
    if (!bytes.ok()) {
      auto done = std::move(it->second.done);
      pending_.erase(it);
      if (done) outbox_.push_back([done] { done(ExecReply{ExecStatus::BadRequest, "a name is longer than its length prefix"}); });
      return;
    }
    toSend_.push_back(std::move(bytes.value()));
  }

  /// Sends what sendLocked queued and delivers what it failed. Called after every
  /// critical section that may have queued, never while holding the lock.
  void flushOut() {
    std::vector<std::string> frames;
    std::vector<std::function<void()>> run;
    std::shared_ptr<graphql::IWebSocketConnection> ws;
    {
      std::lock_guard lock(mu_);
      frames.swap(toSend_);
      run.swap(outbox_);
      ws = ws_;
    }
    if (ws) {
      for (auto& b : frames) ws->send(graphql::WebSocketFrame{graphql::WebSocketFrameKind::Binary, std::move(b)});
    }
    deliver(std::move(run));
  }

  void beginDialLocked(std::vector<std::function<void()>>& run) {
    dialing_ = true;
    auto weak = weak_from_this();
    auto dial = dial_;
    // Dial outside the lock: the dialer may answer synchronously.
    run.push_back([weak, dial] {
      dial([weak](Result<ExecEndpoint> found) {
        if (auto self = weak.lock()) self->onEndpoint(std::move(found));
      });
    });
  }

  void onEndpoint(Result<ExecEndpoint> found) {
    // The Game API's URL is read outside the lock: the client's endpoint has a lock of its own.
    std::optional<std::string> refused;
    if (found.ok()) {
      std::function<std::string()> pin;
      {
        std::lock_guard lock(mu_);
        pin = gatewayPin_;
      }
      if (pin) {
        if (auto why = execGatewayRefusal(pin(), found.value().gatewayUrl)) {
          refused = "refusing the gateway " + found.value().gatewayUrl + ": " + *why;
        }
      }
    }
    std::vector<std::function<void()>> run;
    std::shared_ptr<graphql::IWebSocketConnection> ws;
    std::uint64_t gen = 0;
    {
      std::lock_guard lock(mu_);
      if (closed_) return;
      if (!found.ok()) {
        dialing_ = false;
        attemptFailedLocked(run, found.error(), "no host for this app");
      } else if (refused) {
        dialing_ = false;
        attemptFailedLocked(run, Errc::NotConnected, *refused);
      } else {
        endpoint_ = found.value();
        std::string url = endpoint_.gatewayUrl;
        while (!url.empty() && url.back() == '/') url.pop_back();
        graphql::WebSocketConnectRequest req;
        req.url = url + "/v1/connect?token=" + queryValue(endpoint_.token);
        req.subprotocol.clear();
        req.connectTimeoutMs = opts_.openTimeoutMs;
        ws = transport_ ? transport_->createConnection(req) : nullptr;
        if (!ws) {
          dialing_ = false;
          attemptFailedLocked(run, Errc::NotConnected, "no WebSocket transport");
        } else {
          gen = ++gen_;
          ws_ = ws;
          open_ = false;
        }
      }
    }
    deliver(std::move(run));
    if (ws) {
      auto weak = weak_from_this();
      ws->start([weak, gen](graphql::WebSocketEvent ev) {
        if (auto self = weak.lock()) self->onEvent(gen, std::move(ev));
      });
    }
  }

  void onEvent(std::uint64_t gen, graphql::WebSocketEvent ev) {
    switch (ev.kind) {
      case graphql::WebSocketEventKind::Open: return onOpen(gen);
      case graphql::WebSocketEventKind::Frame:
        if (ev.frame.kind == graphql::WebSocketFrameKind::Binary) onFrame(gen, ev.frame.payload);
        return;
      case graphql::WebSocketEventKind::Close:
        // 4401 is how a gateway before ck-exec 0.10.0 refused a token: it upgraded, then closed.
        return onLost(gen, ev.close.code == 4401 ? ExecStatus::Denied : ExecStatus::Unavailable,
                      ev.close.reason.empty() ? "the gateway closed the connection" : ev.close.reason);
      case graphql::WebSocketEventKind::Error:
        if (ev.error.httpStatus > 0) return onRefused(gen, ev.error.httpStatus, ev.error.httpBody);
        return onLost(gen, ExecStatus::Unavailable, ev.error.message);
    }
  }

  /// The gateway answered the upgrade `status` (401: a refused connect token; 429: the player's
  /// or the gateway's session cap), with `body` as the reason when the transport could read it.
  void onRefused(std::uint64_t gen, int status, const std::string& body) {
    std::string reason;
    for (char c : body) {
      const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
      if (space && (reason.empty() || reason.back() == ' ')) continue;
      reason.push_back(space ? ' ' : c);
      if (reason.size() >= kRefusalReasonMaxChars) break;
    }
    while (!reason.empty() && reason.back() == ' ') reason.pop_back();
    onLost(gen, status == 401 ? ExecStatus::Denied : ExecStatus::Unavailable,
           "the gateway refused the connection (HTTP " + std::to_string(status) + (reason.empty() ? "" : ": " + reason) +
               ")");
  }

  static constexpr std::size_t kRefusalReasonMaxChars = 500;

  void onOpen(std::uint64_t gen) {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard lock(mu_);
      if (gen != gen_ || closed_) return;
      open_ = true;
      dialing_ = false;
      lastFailure_.reset();
      const bool again = everOpened_;
      everOpened_ = true;
      backoffMs_ = opts_.initialReconnectDelayMs;
      for (auto& w : waiters_) run.push_back([w] { w(Errc::Ok); });
      waiters_.clear();
      if (again) {
        // Renew every subscription on the new connection.
        for (auto& [k, sub] : subs_) {
          const std::uint32_t rid = nextRid();
          Pending p{exec_wire::ClientFrame{Kind::Subscribe, rid, sub.nodeType, sub.key, sub.topic, {}}, {},
                    Clock::now() + std::chrono::milliseconds(opts_.callTimeoutMs)};
          p.internal = true;
          pending_.emplace(rid, std::move(p));
        }
        const std::string host = endpoint_.host;
        for (auto& l : reconnectListeners_) run.push_back([l, host] { l(host); });
      }
      std::vector<std::uint32_t> waiting;
      for (auto& [rid, p] : pending_) {
        if (p.gen == 0) waiting.push_back(rid);
      }
      for (auto rid : waiting) sendLocked(rid);
    }
    flushOut();
    deliver(std::move(run));
  }

  void onFrame(std::uint64_t gen, const std::string& bytes) {
    Result<exec_wire::ServerFrame> decoded = exec_wire::decode(bytes);
    if (!decoded.ok()) return;
    exec_wire::ServerFrame& f = decoded.value();
    std::vector<std::function<void()>> run;
    std::shared_ptr<graphql::IWebSocketConnection> redialFrom;
    {
      std::lock_guard lock(mu_);
      if (closed_) return;
      if (f.kind == exec_wire::ServerFrame::Kind::Push) {
        auto it = subs_.find(subKey(f.nodeType, f.key, f.topic));
        if (it == subs_.end()) return;
        auto push = std::make_shared<ExecPush>(ExecPush{f.nodeType, f.key, f.topic, f.payload});
        for (auto& [h, handler] : it->second.handlers) {
          run.push_back([handler, push] { handler(*push); });
        }
      } else {
        auto it = pending_.find(f.rid);
        if (it == pending_.end()) return;
        Pending& p = it->second;
        const ExecStatus status =
            f.kind == exec_wire::ServerFrame::Kind::Pong ? ExecStatus::Ok : execStatusFromWire(f.status);
        if (status == ExecStatus::Moved && p.frame.kind == Kind::Call && !p.retried && opts_.reconnect) {
          // The instance moved. Once per call: leave this host for the one the Game API
          // picks now (unless an earlier Moved already did), and send the call again there.
          p.retried = true;
          p.gen = 0;
          if (gen == gen_ && ws_) {
            redialFrom = ws_;
            loseLocked(run, ExecStatus::Unavailable, "the connection moved to another host", true);
          } else if (ws_ && open_) {
            sendLocked(f.rid);
          }
        } else {
          auto done = std::move(p.done);
          pending_.erase(it);
          if (done) {
            ExecReply reply{status, std::move(f.payload)};
            run.push_back([done, reply = std::move(reply)]() mutable { done(std::move(reply)); });
          }
        }
      }
    }
    if (redialFrom) redialFrom->close(1000, "moved");
    flushOut();
    deliver(std::move(run));
  }

  void onLost(std::uint64_t gen, ExecStatus status, std::string why) {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard lock(mu_);
      if (gen != gen_ || closed_) return;
      loseLocked(run, status, why, false);
    }
    deliver(std::move(run));
  }

  /// The current connection is gone (or being left): requeue or fail what was in
  /// flight on it, and reconnect. A later event from it is ignored.
  void loseLocked(std::vector<std::function<void()>>& run, ExecStatus status, const std::string& why,
                  bool immediately) {
    {
      const bool wasOpen = open_;
      ws_.reset();
      open_ = false;
      dialing_ = false;
      ++gen_;
      if (!immediately) lastFailure_ = ExecReply{status, why};
      // Calls in flight on the lost connection go again once (a call caught by
      // a lost connection is retried, like one answered Moved); the rest fail.
      for (auto it = pending_.begin(); it != pending_.end();) {
        Pending& p = it->second;
        if (p.gen == 0) {
          ++it;
          continue;
        }
        if (p.internal) {
          it = pending_.erase(it);
          continue;
        }
        if (opts_.reconnect && !p.retried && p.frame.kind == Kind::Call) {
          p.retried = true;
          p.gen = 0;
          ++it;
          continue;
        }
        auto done = std::move(p.done);
        it = pending_.erase(it);
        if (done) run.push_back([done, status, why] { done(ExecReply{status, why}); });
      }
      if (!everOpened_ && !wasOpen) {
        // The first connection never opened: fail the waiters rather than retry forever.
        const Errc code = status == ExecStatus::Denied ? Errc::Rejected : Errc::NotConnected;
        for (auto& w : waiters_) run.push_back([w, code] { w(code); });
        waiters_.clear();
        failAllLocked(run, why, status);
      } else if (opts_.reconnect) {
        scheduleReconnectLocked(immediately ? 0 : backoffMs_);
      } else {
        failAllLocked(run, why, status);
      }
    }
  }

  void attemptFailedLocked(std::vector<std::function<void()>>& run, Errc code, const std::string& why) {
    lastFailure_ = ExecReply{ExecStatus::Unavailable, why};
    if (everOpened_ && opts_.reconnect) {
      scheduleReconnectLocked(backoffMs_);
      backoffMs_ = std::min(backoffMs_ * 2, opts_.maxReconnectDelayMs);
      return;
    }
    for (auto& w : waiters_) run.push_back([w, code] { w(code); });
    waiters_.clear();
    failAllLocked(run, why);
  }

  void failAllLocked(std::vector<std::function<void()>>& run, const std::string& why,
                     ExecStatus status = ExecStatus::Unavailable) {
    for (auto& [rid, p] : pending_) {
      if (p.done) {
        auto done = std::move(p.done);
        run.push_back([done, status, why] { done(ExecReply{status, why}); });
      }
    }
    pending_.clear();
  }

  void scheduleReconnectLocked(long delayMs) {
    reconnectAt_ = Clock::now() + std::chrono::milliseconds(delayMs);
    reconnectDue_ = true;
    ensureTimerLocked();
    cv_.notify_all();
  }

  void dropHandler(std::uint64_t handle, bool tellGateway) {
    std::optional<exec_wire::ClientFrame> unsub;
    {
      std::lock_guard lock(mu_);
      auto h = handles_.find(handle);
      if (h == handles_.end()) return;
      const std::string k = h->second;
      handles_.erase(h);
      auto it = subs_.find(k);
      if (it == subs_.end()) return;
      it->second.handlers.erase(handle);
      if (!it->second.handlers.empty()) return;
      if (tellGateway && ws_ && open_) {
        unsub = exec_wire::ClientFrame{Kind::Unsubscribe, 0, it->second.nodeType, it->second.key, it->second.topic, {}};
      }
      subs_.erase(it);
    }
    if (unsub) request(std::move(*unsub), {}, true);
  }

  // ---- timer: call deadlines and reconnect backoff ----

  void ensureTimerLocked() {
    if (timerStarted_ || closed_) return;
    timerStarted_ = true;
    auto weak = weak_from_this();
    timer_ = std::thread([weak] {
      while (auto self = weak.lock()) {
        if (!self->timerTick()) return;
      }
    });
  }

  /// One wait and its consequences. False once the connection is closed.
  bool timerTick() {
    std::vector<std::function<void()>> run;
    {
      std::unique_lock lock(mu_);
      if (closed_) return false;
      auto next = Clock::now() + std::chrono::milliseconds(250);
      for (auto& [rid, p] : pending_) next = std::min(next, p.deadline);
      if (reconnectDue_) next = std::min(next, reconnectAt_);
      cv_.wait_until(lock, next);
      if (closed_) return false;
      const auto now = Clock::now();
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.deadline > now) {
          ++it;
          continue;
        }
        auto done = std::move(it->second.done);
        const long ms = opts_.callTimeoutMs;
        it = pending_.erase(it);
        if (done) run.push_back([done, ms] { done(ExecReply{ExecStatus::DeadlineExceeded, "no reply in " + std::to_string(ms) + " ms"}); });
      }
      if (reconnectDue_ && reconnectAt_ <= now && !ws_ && !dialing_) {
        reconnectDue_ = false;
        beginDialLocked(run);
      }
    }
    deliver(std::move(run));
    return true;
  }

  void stopTimer() {
    cv_.notify_all();
    if (timer_.joinable()) {
      if (timer_.get_id() == std::this_thread::get_id()) {
        timer_.detach();
      } else {
        timer_.join();
      }
    }
  }

  std::shared_ptr<graphql::IWebSocketTransport> transport_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
  ExecDial dial_;
  ExecConnectOptions opts_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread timer_;
  bool timerStarted_ = false;

  std::shared_ptr<graphql::IWebSocketConnection> ws_;
  std::uint64_t gen_ = 0;
  bool open_ = false;
  bool dialing_ = false;
  bool closed_ = false;
  bool everOpened_ = false;
  ExecEndpoint endpoint_;
  std::function<std::string()> gatewayPin_;
  std::optional<ExecReply> lastFailure_;
  long backoffMs_;
  bool reconnectDue_ = false;
  Clock::time_point reconnectAt_{};

  std::uint32_t rid_ = 1;
  std::map<std::uint32_t, Pending> pending_;
  std::vector<std::string> toSend_;
  std::vector<std::function<void()>> outbox_;
  std::vector<std::function<void(Status)>> waiters_;
  std::map<std::string, Sub> subs_;
  std::map<std::uint64_t, std::string> handles_;
  std::uint64_t nextHandle_ = 0;
  std::vector<std::function<void(std::string)>> reconnectListeners_;
};

ExecConnection::ExecConnection(std::shared_ptr<graphql::IWebSocketTransport> transport,
                               std::shared_ptr<graphql::Dispatcher> dispatcher, ExecDial dial,
                               ExecConnectOptions options)
    : impl_(std::make_shared<Impl>(std::move(transport), std::move(dispatcher), std::move(dial),
                                   std::move(options))) {}

ExecConnection::~ExecConnection() { impl_->close(); }

std::shared_ptr<ExecConnection> ExecConnection::open(std::shared_ptr<graphql::IWebSocketTransport> transport,
                                                     std::shared_ptr<graphql::Dispatcher> dispatcher,
                                                     ExecEndpoint endpoint, ExecConnectOptions options) {
  options.reconnect = false;
  auto c = std::make_shared<ExecConnection>(std::move(transport), std::move(dispatcher), ExecDial{},
                                            std::move(options));
  c->impl_->useFixedEndpoint(std::move(endpoint));
  c->connect();
  return c;
}

void ExecConnection::connect(std::function<void(Status)> done) { impl_->connect(std::move(done)); }

void ExecConnection::callRaw(std::string nodeType, std::string key, std::string method, std::string payload,
                             ExecReplyCallback done) {
  impl_->request({Kind::Call, 0, std::move(nodeType), std::move(key), std::move(method), std::move(payload)},
                 std::move(done));
}

void ExecConnection::call(std::string nodeType, std::string key, std::string method, const graphql::JVal& args,
                          ExecReplyCallback done) {
  std::string payload = graphql::Json::parse(args.dump()).toMsgpack();
  callRaw(std::move(nodeType), std::move(key), std::move(method), std::move(payload), std::move(done));
}

std::uint64_t ExecConnection::subscribe(std::string nodeType, std::string key, std::string topic,
                                        ExecPushHandler onPush, ExecReplyCallback done) {
  return impl_->subscribe(std::move(nodeType), std::move(key), std::move(topic), std::move(onPush),
                          std::move(done));
}

void ExecConnection::unsubscribe(std::uint64_t handle) { impl_->unsubscribe(handle); }

void ExecConnection::ping(ExecReplyCallback done) { impl_->request({Kind::Ping, 0, {}, {}, {}, {}}, std::move(done)); }

void ExecConnection::onReconnect(std::function<void(std::string)> listener) {
  impl_->onReconnect(std::move(listener));
}

std::string ExecConnection::host() const { return impl_->host(); }

bool ExecConnection::connected() const { return impl_->connected(); }

std::optional<ExecReply> ExecConnection::lastFailure() const { return impl_->lastFailure(); }

void ExecConnection::pinGateway(std::function<std::string()> gameApiUrl) { impl_->pinGateway(std::move(gameApiUrl)); }

void ExecConnection::close() { impl_->close(); }

// ---- the domain -------------------------------------------------------------------

ExecAPI::ExecAPI(std::shared_ptr<graphql::GraphQLClient> gql, std::shared_ptr<graphql::IWebSocketTransport> transport)
    : DomainBase(std::move(gql)), transport_(std::move(transport)) {}

namespace {

graphql::JVal connectVariables(const std::string& appId, const std::string& nodeType, const std::string& key) {
  graphql::JVal vars;
  vars["appId"] = graphql::JVal(appId);
  if (!nodeType.empty()) {
    vars["nodeType"] = graphql::JVal(nodeType);
    vars["key"] = graphql::JVal(key);
  }
  return vars;
}

Result<ExecEndpoint> endpointFrom(const graphql::Json& c) {
  if (!c.isObject()) return Errc::Rejected;
  ExecEndpoint e{c["gatewayUrl"].asString(), c["token"].asString(), c["host"].asString()};
  if (e.gatewayUrl.empty() || e.token.empty()) return Errc::Malformed;
  return e;
}

}  // namespace

Result<ExecEndpoint> ExecAPI::endpoint(std::string appId, std::string nodeType, std::string key) const {
  return endpointFrom(exec(gen::exec::kExecConnectIsolatedDocument, "execConnect",
                           connectVariables(appId, nodeType, key), gen::exec::kExecConnectOperationName));
}

void ExecAPI::endpointAsync(std::string appId, std::string nodeType, std::string key,
                            std::function<void(Result<ExecEndpoint>)> done) const {
  execAsync(gen::exec::kExecConnectIsolatedDocument, "execConnect", connectVariables(appId, nodeType, key),
            gen::exec::kExecConnectOperationName, [done = std::move(done)](graphql::GraphQLOutcome out) {
              if (!out.ok()) return done(out.status.ok() ? Errc::Rejected : out.status.code);
              done(endpointFrom(out.data));
            });
}

ExecDial ExecAPI::dialer(std::string appId, std::string nodeType, std::string key, bool developer) const {
  // Everything the dialer needs is copied in, so a connection may reconnect long
  // after the call that made it returned.
  auto gql = gql_;
  return [gql, appId = std::move(appId), nodeType = std::move(nodeType), key = std::move(key),
          developer](std::function<void(Result<ExecEndpoint>)> found) {
    const auto doc = developer ? gen::exec::kExecConnectAsDeveloperIsolatedDocument : gen::exec::kExecConnectIsolatedDocument;
    const auto op = developer ? gen::exec::kExecConnectAsDeveloperOperationName : gen::exec::kExecConnectOperationName;
    const char* field = developer ? "execConnectAsDeveloper" : "execConnect";
    gql->requestAsync(doc, connectVariables(appId, nodeType, key), op,
                      [found = std::move(found), field](graphql::GraphQLOutcome out) {
                        if (!out.ok()) return found(out.status.ok() ? Errc::Rejected : out.status.code);
                        found(endpointFrom(out.data[field]));
                      });
  };
}

std::shared_ptr<ExecConnection> ExecAPI::pinned(ExecDial dial, ExecConnectOptions options) const {
  auto conn = std::make_shared<ExecConnection>(transport_, gql_->dispatcher(), std::move(dial), std::move(options));
  std::weak_ptr<graphql::GraphQLClient> gql = gql_;
  conn->pinGateway([gql] {
    auto client = gql.lock();
    return client ? client->endpoint() : std::string(kDefaultHttpOrigin);
  });
  return conn;
}

std::shared_ptr<ExecConnection> ExecAPI::connect(std::string appId, ExecConnectOptions options) const {
  auto dial = dialer(std::move(appId), options.nodeType, options.key);
  auto conn = pinned(std::move(dial), std::move(options));
  conn->connect();
  return conn;
}

void ExecAPI::connectAsync(std::string appId, ExecConnectOptions options,
                           std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const {
  auto dial = dialer(std::move(appId), options.nodeType, options.key);
  auto conn = pinned(std::move(dial), std::move(options));
  conn->connect([conn, done = std::move(done)](Status s) {
    if (!s.ok()) return done(s.code);
    done(conn);
  });
}

Result<ExecEndpoint> ExecAPI::developerEndpoint(std::string appId, std::string nodeType, std::string key) const {
  return endpointFrom(exec(gen::exec::kExecConnectAsDeveloperIsolatedDocument, "execConnectAsDeveloper",
                           connectVariables(appId, nodeType, key), gen::exec::kExecConnectAsDeveloperOperationName));
}

void ExecAPI::developerEndpointAsync(std::string appId, std::string nodeType, std::string key,
                                     std::function<void(Result<ExecEndpoint>)> done) const {
  execAsync(gen::exec::kExecConnectAsDeveloperIsolatedDocument, "execConnectAsDeveloper",
            connectVariables(appId, nodeType, key), gen::exec::kExecConnectAsDeveloperOperationName,
            [done = std::move(done)](graphql::GraphQLOutcome out) {
              if (!out.ok()) return done(out.status.ok() ? Errc::Rejected : out.status.code);
              done(endpointFrom(out.data));
            });
}

std::shared_ptr<ExecConnection> ExecAPI::connectAsDeveloper(std::string appId, ExecConnectOptions options) const {
  auto dial = dialer(std::move(appId), options.nodeType, options.key, true);
  auto conn = pinned(std::move(dial), std::move(options));
  conn->connect();
  return conn;
}

void ExecAPI::connectAsDeveloperAsync(std::string appId, ExecConnectOptions options,
                                      std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const {
  auto dial = dialer(std::move(appId), options.nodeType, options.key, true);
  auto conn = pinned(std::move(dial), std::move(options));
  conn->connect([conn, done = std::move(done)](Status s) {
    if (!s.ok()) return done(s.code);
    done(conn);
  });
}

namespace {

graphql::JVal appVariables(const std::string& appId) {
  graphql::JVal vars;
  vars["appId"] = graphql::JVal(appId);
  return vars;
}

graphql::JVal logsVariables(const std::string& appId, const ExecLogsQuery& q) {
  graphql::JVal vars = appVariables(appId);
  if (!q.nodeType.empty()) vars["nodeType"] = graphql::JVal(q.nodeType);
  if (!q.key.empty()) vars["key"] = graphql::JVal(q.key);
  if (q.maxLevel >= 0) vars["maxLevel"] = graphql::JVal(q.maxLevel);
  if (!q.before.empty()) vars["before"] = graphql::JVal(q.before);
  if (q.limit >= 0) vars["limit"] = graphql::JVal(q.limit);
  if (!q.flow.empty()) vars["flow"] = graphql::JVal(q.flow);
  return vars;
}

graphql::JVal endpointStatsVariables(const std::string& appId, const std::string& nodeType, int sinceMinutes) {
  graphql::JVal vars = appVariables(appId);
  if (!nodeType.empty()) vars["nodeType"] = graphql::JVal(nodeType);
  if (sinceMinutes >= 0) vars["sinceMinutes"] = graphql::JVal(sinceMinutes);
  return vars;
}

graphql::JVal activateVariables(const std::string& appId, int version) {
  graphql::JVal vars = appVariables(appId);
  vars["version"] = graphql::JVal(version);
  return vars;
}

graphql::JVal enabledVariables(const std::string& appId, bool enabled, const std::string& nodeType) {
  graphql::JVal vars = appVariables(appId);
  vars["enabled"] = graphql::JVal(enabled);
  if (!nodeType.empty()) vars["nodeType"] = graphql::JVal(nodeType);
  return vars;
}

}  // namespace

graphql::Json ExecAPI::logs(std::string appId, const ExecLogsQuery& query) const {
  return exec(gen::exec::kExecLogsIsolatedDocument, "execLogs", logsVariables(appId, query),
              gen::exec::kExecLogsOperationName);
}

void ExecAPI::logsAsync(std::string appId, const ExecLogsQuery& query, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecLogsIsolatedDocument, "execLogs", logsVariables(appId, query),
            gen::exec::kExecLogsOperationName, std::move(done));
}

graphql::Json ExecAPI::instances(std::string appId) const {
  return exec(gen::exec::kExecInstancesIsolatedDocument, "execInstances", appVariables(appId),
              gen::exec::kExecInstancesOperationName);
}

void ExecAPI::instancesAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecInstancesIsolatedDocument, "execInstances", appVariables(appId),
            gen::exec::kExecInstancesOperationName, std::move(done));
}

graphql::Json ExecAPI::versions(std::string appId) const {
  return exec(gen::exec::kExecVersionsIsolatedDocument, "execVersions", appVariables(appId),
              gen::exec::kExecVersionsOperationName);
}

void ExecAPI::versionsAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecVersionsIsolatedDocument, "execVersions", appVariables(appId),
            gen::exec::kExecVersionsOperationName, std::move(done));
}

graphql::Json ExecAPI::endpointStats(std::string appId, std::string nodeType, int sinceMinutes) const {
  return exec(gen::exec::kExecEndpointStatsIsolatedDocument, "execEndpointStats",
              endpointStatsVariables(appId, nodeType, sinceMinutes), gen::exec::kExecEndpointStatsOperationName);
}

void ExecAPI::endpointStatsAsync(std::string appId, std::string nodeType, int sinceMinutes,
                                 graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecEndpointStatsIsolatedDocument, "execEndpointStats",
            endpointStatsVariables(appId, nodeType, sinceMinutes), gen::exec::kExecEndpointStatsOperationName,
            std::move(done));
}

graphql::Json ExecAPI::status(std::string appId) const {
  return exec(gen::exec::kExecAppStatusIsolatedDocument, "execAppStatus", appVariables(appId),
              gen::exec::kExecAppStatusOperationName);
}

void ExecAPI::statusAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecAppStatusIsolatedDocument, "execAppStatus", appVariables(appId),
            gen::exec::kExecAppStatusOperationName, std::move(done));
}

graphql::Json ExecAPI::activateVersion(std::string appId, int version) const {
  return exec(gen::exec::kExecActivateVersionIsolatedDocument, "execActivateVersion",
              activateVariables(appId, version), gen::exec::kExecActivateVersionOperationName);
}

void ExecAPI::activateVersionAsync(std::string appId, int version, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecActivateVersionIsolatedDocument, "execActivateVersion",
            activateVariables(appId, version), gen::exec::kExecActivateVersionOperationName, std::move(done));
}

graphql::Json ExecAPI::setEnabled(std::string appId, bool enabled, std::string nodeType) const {
  return exec(gen::exec::kExecSetEnabledIsolatedDocument, "execSetEnabled",
              enabledVariables(appId, enabled, nodeType), gen::exec::kExecSetEnabledOperationName);
}

void ExecAPI::setEnabledAsync(std::string appId, bool enabled, std::string nodeType,
                              graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecSetEnabledIsolatedDocument, "execSetEnabled", enabledVariables(appId, enabled, nodeType),
            gen::exec::kExecSetEnabledOperationName, std::move(done));
}

graphql::Json ExecAPI::restartType(std::string appId, std::string nodeType) const {
  graphql::JVal vars = appVariables(appId);
  vars["nodeType"] = graphql::JVal(nodeType);
  return exec(gen::exec::kExecRestartTypeIsolatedDocument, "execRestartType", vars,
              gen::exec::kExecRestartTypeOperationName);
}

void ExecAPI::restartTypeAsync(std::string appId, std::string nodeType, graphql::GraphQLCallback done) const {
  graphql::JVal vars = appVariables(appId);
  vars["nodeType"] = graphql::JVal(nodeType);
  execAsync(gen::exec::kExecRestartTypeIsolatedDocument, "execRestartType", vars,
            gen::exec::kExecRestartTypeOperationName, std::move(done));
}

graphql::Json ExecAPI::deploy(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                             std::string buildId) const {
  return exec(gen::exec::kExecDeployIsolatedDocument, "execDeploy",
              deployVariables(std::move(appId), std::move(root), types, buildId), gen::exec::kExecDeployOperationName);
}

void ExecAPI::deployAsync(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                          graphql::GraphQLCallback done) const {
  deployAsync(std::move(appId), std::move(root), types, {}, std::move(done));
}

void ExecAPI::deployAsync(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                          std::string buildId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecDeployIsolatedDocument, "execDeploy",
            deployVariables(std::move(appId), std::move(root), types, buildId), gen::exec::kExecDeployOperationName,
            std::move(done));
}

namespace {

graphql::JVal buildVariables(const std::string& appId, const std::vector<ExecCrate>& crates) {
  graphql::JArray list;
  for (const auto& c : crates) {
    graphql::JArray files;
    for (const auto& [path, content] : c.files) {
      files.push_back(graphql::JVal::object({{"path", graphql::JVal(path)}, {"content", graphql::JVal(content)}}));
    }
    list.push_back(graphql::JVal::object({{"name", graphql::JVal(c.name)}, {"files", graphql::JVal(std::move(files))}}));
  }
  graphql::JVal vars;
  vars["input"]["appId"] = graphql::JVal(appId);
  vars["input"]["crates"] = graphql::JVal(std::move(list));
  return vars;
}

graphql::JVal buildStatusVariables(const std::string& appId, const std::string& buildId) {
  graphql::JVal vars = appVariables(appId);
  vars["buildId"] = graphql::JVal(buildId);
  return vars;
}

}  // namespace

graphql::Json ExecAPI::starters(std::string appId) const {
  return exec(gen::exec::kExecStartersIsolatedDocument, "execStarters", appVariables(appId),
              gen::exec::kExecStartersOperationName);
}

void ExecAPI::startersAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecStartersIsolatedDocument, "execStarters", appVariables(appId),
            gen::exec::kExecStartersOperationName, std::move(done));
}

graphql::Json ExecAPI::build(std::string appId, const std::vector<ExecCrate>& crates) const {
  return exec(gen::exec::kExecBuildIsolatedDocument, "execBuild", buildVariables(appId, crates),
              gen::exec::kExecBuildOperationName);
}

void ExecAPI::buildAsync(std::string appId, const std::vector<ExecCrate>& crates,
                         graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecBuildIsolatedDocument, "execBuild", buildVariables(appId, crates),
            gen::exec::kExecBuildOperationName, std::move(done));
}

graphql::Json ExecAPI::buildStatus(std::string appId, std::string buildId) const {
  return exec(gen::exec::kExecBuildStatusIsolatedDocument, "execBuildStatus", buildStatusVariables(appId, buildId),
              gen::exec::kExecBuildStatusOperationName);
}

void ExecAPI::buildStatusAsync(std::string appId, std::string buildId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecBuildStatusIsolatedDocument, "execBuildStatus", buildStatusVariables(appId, buildId),
            gen::exec::kExecBuildStatusOperationName, std::move(done));
}

graphql::Json ExecAPI::waitForBuild(std::string appId, std::string buildId, int intervalMs, int timeoutMs) const {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    auto b = buildStatus(appId, buildId);
    if (!b.isObject()) return b;
    const auto status = b["status"].asString();
    if (status == "succeeded" || status == "failed" || std::chrono::steady_clock::now() >= until) return b;
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
}

// ---- mods ----

namespace {

graphql::JVal modVariables(const std::string& appId, const std::string& gridId, const std::string& name) {
  graphql::JVal vars = appVariables(appId);
  vars["gridId"] = graphql::JVal(gridId);
  vars["name"] = graphql::JVal(name);
  return vars;
}

graphql::JVal modBuildVariables(const std::string& appId, const ExecCrate& c) {
  graphql::JArray files;
  for (const auto& [path, content] : c.files) {
    files.push_back(graphql::JVal::object({{"path", graphql::JVal(path)}, {"content", graphql::JVal(content)}}));
  }
  graphql::JVal vars = appVariables(appId);
  vars["crate"] = graphql::JVal::object({{"name", graphql::JVal(c.name)}, {"files", graphql::JVal(std::move(files))}});
  return vars;
}

graphql::JVal modLogsVariables(const std::string& appId, const std::string& gridId, const std::string& name,
                               const ExecLogsQuery& q) {
  graphql::JVal vars = modVariables(appId, gridId, name);
  if (q.maxLevel >= 0) vars["maxLevel"] = graphql::JVal(q.maxLevel);
  if (!q.before.empty()) vars["before"] = graphql::JVal(q.before);
  if (q.limit >= 0) vars["limit"] = graphql::JVal(q.limit);
  return vars;
}

graphql::JVal modPublishVariables(const std::string& appId, const std::string& gridId, const std::string& name,
                                  const std::string& title, const std::string& description) {
  graphql::JVal vars = modVariables(appId, gridId, name);
  vars["title"] = graphql::JVal(title);
  if (!description.empty()) vars["description"] = graphql::JVal(description);
  return vars;
}

graphql::JVal modListingVariables(const std::string& appId, const std::string& listingId) {
  graphql::JVal vars = appVariables(appId);
  vars["listingId"] = graphql::JVal(listingId);
  return vars;
}

graphql::JVal appModsVariables(const std::string& appId, const std::string& gridId, const std::string& ownerId) {
  graphql::JVal vars = appVariables(appId);
  if (!gridId.empty()) vars["gridId"] = graphql::JVal(gridId);
  if (!ownerId.empty()) vars["ownerId"] = graphql::JVal(ownerId);
  return vars;
}

graphql::JVal modSwitchVariables(const std::string& appId, gen::ExecModScope scope, bool off, const std::string& target,
                                 const std::string& reason) {
  graphql::JVal vars = appVariables(appId);
  vars["scope"] = graphql::JVal(std::string(gen::toString(scope)));
  vars["off"] = graphql::JVal(off);
  if (!target.empty()) vars["target"] = graphql::JVal(target);
  if (!reason.empty()) vars["reason"] = graphql::JVal(reason);
  return vars;
}

}  // namespace

std::string execModType(std::string_view name) { return "mod:" + std::string(name); }

graphql::Json ExecAPI::modStarter(std::string appId) const {
  return exec(gen::exec::kExecModStarterIsolatedDocument, "execModStarter", appVariables(appId),
              gen::exec::kExecModStarterOperationName);
}

void ExecAPI::modStarterAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModStarterIsolatedDocument, "execModStarter", appVariables(appId),
            gen::exec::kExecModStarterOperationName, std::move(done));
}

graphql::Json ExecAPI::modBuild(std::string appId, const ExecCrate& crate) const {
  return exec(gen::exec::kExecModBuildIsolatedDocument, "execModBuild", modBuildVariables(appId, crate),
              gen::exec::kExecModBuildOperationName);
}

void ExecAPI::modBuildAsync(std::string appId, const ExecCrate& crate, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModBuildIsolatedDocument, "execModBuild", modBuildVariables(appId, crate),
            gen::exec::kExecModBuildOperationName, std::move(done));
}

graphql::Json ExecAPI::modBuildStatus(std::string appId, std::string buildId) const {
  return exec(gen::exec::kExecModBuildStatusIsolatedDocument, "execModBuildStatus",
              buildStatusVariables(appId, buildId), gen::exec::kExecModBuildStatusOperationName);
}

void ExecAPI::modBuildStatusAsync(std::string appId, std::string buildId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModBuildStatusIsolatedDocument, "execModBuildStatus",
            buildStatusVariables(appId, buildId), gen::exec::kExecModBuildStatusOperationName, std::move(done));
}

graphql::Json ExecAPI::waitForModBuild(std::string appId, std::string buildId, int intervalMs, int timeoutMs) const {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    auto b = modBuildStatus(appId, buildId);
    if (!b.isObject()) return b;
    const auto status = b["status"].asString();
    if (status == "succeeded" || status == "failed" || std::chrono::steady_clock::now() >= until) return b;
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
}

graphql::Json ExecAPI::modDeploy(std::string appId, std::string gridId, std::string name, std::string buildId) const {
  auto vars = modVariables(appId, gridId, name);
  vars["buildId"] = graphql::JVal(buildId);
  return exec(gen::exec::kExecModDeployIsolatedDocument, "execModDeploy", std::move(vars),
              gen::exec::kExecModDeployOperationName);
}

void ExecAPI::modDeployAsync(std::string appId, std::string gridId, std::string name, std::string buildId,
                             graphql::GraphQLCallback done) const {
  auto vars = modVariables(appId, gridId, name);
  vars["buildId"] = graphql::JVal(buildId);
  execAsync(gen::exec::kExecModDeployIsolatedDocument, "execModDeploy", std::move(vars),
            gen::exec::kExecModDeployOperationName, std::move(done));
}

graphql::Json ExecAPI::modSetEnabled(std::string appId, std::string gridId, std::string name, bool enabled) const {
  auto vars = modVariables(appId, gridId, name);
  vars["enabled"] = graphql::JVal(enabled);
  return exec(gen::exec::kExecModSetEnabledIsolatedDocument, "execModSetEnabled", std::move(vars),
              gen::exec::kExecModSetEnabledOperationName);
}

void ExecAPI::modSetEnabledAsync(std::string appId, std::string gridId, std::string name, bool enabled,
                                 graphql::GraphQLCallback done) const {
  auto vars = modVariables(appId, gridId, name);
  vars["enabled"] = graphql::JVal(enabled);
  execAsync(gen::exec::kExecModSetEnabledIsolatedDocument, "execModSetEnabled", std::move(vars),
            gen::exec::kExecModSetEnabledOperationName, std::move(done));
}

graphql::Json ExecAPI::modDelete(std::string appId, std::string gridId, std::string name) const {
  return exec(gen::exec::kExecModDeleteIsolatedDocument, "execModDelete", modVariables(appId, gridId, name),
              gen::exec::kExecModDeleteOperationName);
}

void ExecAPI::modDeleteAsync(std::string appId, std::string gridId, std::string name,
                             graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModDeleteIsolatedDocument, "execModDelete", modVariables(appId, gridId, name),
            gen::exec::kExecModDeleteOperationName, std::move(done));
}

graphql::Json ExecAPI::mods(std::string appId, std::string gridId) const {
  auto vars = appVariables(appId);
  vars["gridId"] = graphql::JVal(gridId);
  return exec(gen::exec::kExecModsIsolatedDocument, "execMods", std::move(vars), gen::exec::kExecModsOperationName);
}

void ExecAPI::modsAsync(std::string appId, std::string gridId, graphql::GraphQLCallback done) const {
  auto vars = appVariables(appId);
  vars["gridId"] = graphql::JVal(gridId);
  execAsync(gen::exec::kExecModsIsolatedDocument, "execMods", std::move(vars), gen::exec::kExecModsOperationName,
            std::move(done));
}

graphql::Json ExecAPI::myMods(std::string appId) const {
  return exec(gen::exec::kExecMyModsIsolatedDocument, "execMyMods", appVariables(appId),
              gen::exec::kExecMyModsOperationName);
}

void ExecAPI::myModsAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecMyModsIsolatedDocument, "execMyMods", appVariables(appId),
            gen::exec::kExecMyModsOperationName, std::move(done));
}

graphql::Json ExecAPI::modLogs(std::string appId, std::string gridId, std::string name,
                               const ExecLogsQuery& query) const {
  return exec(gen::exec::kExecModLogsIsolatedDocument, "execModLogs", modLogsVariables(appId, gridId, name, query),
              gen::exec::kExecModLogsOperationName);
}

void ExecAPI::modLogsAsync(std::string appId, std::string gridId, std::string name, const ExecLogsQuery& query,
                           graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModLogsIsolatedDocument, "execModLogs", modLogsVariables(appId, gridId, name, query),
            gen::exec::kExecModLogsOperationName, std::move(done));
}

graphql::Json ExecAPI::modPublish(std::string appId, std::string gridId, std::string name, std::string title,
                                  std::string description) const {
  return exec(gen::exec::kExecModPublishIsolatedDocument, "execModPublish",
              modPublishVariables(appId, gridId, name, title, description), gen::exec::kExecModPublishOperationName);
}

void ExecAPI::modPublishAsync(std::string appId, std::string gridId, std::string name, std::string title,
                              std::string description, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModPublishIsolatedDocument, "execModPublish",
            modPublishVariables(appId, gridId, name, title, description), gen::exec::kExecModPublishOperationName,
            std::move(done));
}

graphql::Json ExecAPI::modListings(std::string appId) const {
  return exec(gen::exec::kExecModListingsIsolatedDocument, "execModListings", appVariables(appId),
              gen::exec::kExecModListingsOperationName);
}

void ExecAPI::modListingsAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModListingsIsolatedDocument, "execModListings", appVariables(appId),
            gen::exec::kExecModListingsOperationName, std::move(done));
}

graphql::Json ExecAPI::modUnpublish(std::string appId, std::string listingId) const {
  return exec(gen::exec::kExecModUnpublishIsolatedDocument, "execModUnpublish", modListingVariables(appId, listingId),
              gen::exec::kExecModUnpublishOperationName);
}

void ExecAPI::modUnpublishAsync(std::string appId, std::string listingId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModUnpublishIsolatedDocument, "execModUnpublish", modListingVariables(appId, listingId),
            gen::exec::kExecModUnpublishOperationName, std::move(done));
}

graphql::Json ExecAPI::modInstall(std::string appId, std::string gridId, std::string name,
                                  std::string listingId) const {
  auto vars = modVariables(appId, gridId, name);
  vars["listingId"] = graphql::JVal(listingId);
  return exec(gen::exec::kExecModInstallIsolatedDocument, "execModInstall", std::move(vars),
              gen::exec::kExecModInstallOperationName);
}

void ExecAPI::modInstallAsync(std::string appId, std::string gridId, std::string name, std::string listingId,
                              graphql::GraphQLCallback done) const {
  auto vars = modVariables(appId, gridId, name);
  vars["listingId"] = graphql::JVal(listingId);
  execAsync(gen::exec::kExecModInstallIsolatedDocument, "execModInstall", std::move(vars),
            gen::exec::kExecModInstallOperationName, std::move(done));
}

graphql::Json ExecAPI::appMods(std::string appId, std::string gridId, std::string ownerId) const {
  return exec(gen::exec::kExecAppModsIsolatedDocument, "execAppMods", appModsVariables(appId, gridId, ownerId),
              gen::exec::kExecAppModsOperationName);
}

void ExecAPI::appModsAsync(std::string appId, std::string gridId, std::string ownerId,
                           graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecAppModsIsolatedDocument, "execAppMods", appModsVariables(appId, gridId, ownerId),
            gen::exec::kExecAppModsOperationName, std::move(done));
}

graphql::Json ExecAPI::modSwitches(std::string appId) const {
  return exec(gen::exec::kExecModSwitchesIsolatedDocument, "execModSwitches", appVariables(appId),
              gen::exec::kExecModSwitchesOperationName);
}

void ExecAPI::modSwitchesAsync(std::string appId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModSwitchesIsolatedDocument, "execModSwitches", appVariables(appId),
            gen::exec::kExecModSwitchesOperationName, std::move(done));
}

graphql::Json ExecAPI::modSetSwitch(std::string appId, gen::ExecModScope scope, bool off, std::string target,
                                    std::string reason) const {
  return exec(gen::exec::kExecModSetSwitchIsolatedDocument, "execModSetSwitch",
              modSwitchVariables(appId, scope, off, target, reason), gen::exec::kExecModSetSwitchOperationName);
}

void ExecAPI::modSetSwitchAsync(std::string appId, gen::ExecModScope scope, bool off, std::string target,
                                std::string reason, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModSetSwitchIsolatedDocument, "execModSetSwitch",
            modSwitchVariables(appId, scope, off, target, reason), gen::exec::kExecModSetSwitchOperationName,
            std::move(done));
}

// ---- CLIENT halves ----

namespace {

graphql::JVal modIdVariables(const std::string& appId, const std::string& modId) {
  graphql::JVal vars = appVariables(appId);
  vars["modId"] = graphql::JVal(modId);
  return vars;
}

graphql::JVal consentVariables(const std::string& appId, const std::string& modId, const std::string& capabilityHash) {
  graphql::JVal vars = modIdVariables(appId, modId);
  vars["capabilityHash"] = graphql::JVal(capabilityHash);
  return vars;
}

graphql::JVal authorVariables(const std::string& appId, const std::string& gridId, const std::string& authorId) {
  graphql::JVal vars = appVariables(appId);
  vars["gridId"] = graphql::JVal(gridId);
  vars["authorId"] = graphql::JVal(authorId);
  return vars;
}

graphql::JVal trustVariables(const std::string& appId, const std::string& gridId, const std::string& authorId,
                             const std::string& capabilityHash) {
  graphql::JVal vars = authorVariables(appId, gridId, authorId);
  vars["capabilityHash"] = graphql::JVal(capabilityHash);
  return vars;
}

graphql::JVal gridVariables(const std::string& appId, const std::string& gridId) {
  graphql::JVal vars = appVariables(appId);
  vars["gridId"] = graphql::JVal(gridId);
  return vars;
}

void appendStrings(const graphql::Json& list, std::vector<std::string>& into) {
  if (!list.isArray()) return;
  list.forEach([&](graphql::Json v) {
    if (v.isString()) into.push_back(v.asString());
  });
}

std::string asciiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

/// Fills `out` from an `execModClientArtifact` result, checked in CrowdyJS's order: the ABI, the
/// capability summary, then the bytes against their digest. Returns why it is refused, or "".
std::string checkClientArtifact(const graphql::Json& a, ExecModClientArtifactBytes& out) {
  if (!a.isObject() || !a["wasmBase64"].isString() || !a["digest"].isString() || !a["abiVersion"].isNumber()) {
    return "execModClientArtifact returned no CLIENT module";
  }
  out.modId = a["modId"].asString();
  const std::int64_t abi = a["abiVersion"].asInt64(-1);
  if (abi != kExecClientAbiVersion) {
    return "CLIENT half of mod " + out.modId + " is built for CLIENT ABI " + a["abiVersion"].dump() +
           "; this SDK runs ABI " + std::to_string(kExecClientAbiVersion);
  }
  out.capabilitySummaryJson = a["capabilitySummaryJson"].asString();
  auto summary = parseExecClientCapabilitySummary(out.capabilitySummaryJson);
  if (!summary) {
    return "CLIENT half of mod " + out.modId +
           ": its capability summary does not parse, so nothing bounds its host calls";
  }
  auto bytes = core::base64Decode(a["wasmBase64"].asStringView());
  if (!bytes) return "CLIENT half of mod " + out.modId + ": its module is not base64";
  out.digest = asciiLower(a["digest"].asStringView());
  const std::string actual =
      execSha256Hex(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (actual != out.digest) {
    return "CLIENT half of mod " + out.modId + ": the module's SHA-256 is " + actual + ", not the digest " +
           out.digest + " it was served with";
  }
  out.fuelPerDispatch = a["fuelPerDispatch"].asBigIntString();
  if (out.fuelPerDispatch.empty()) {
    return "CLIENT half of mod " + out.modId + ": its fuel per dispatch is not an integer";
  }
  out.name = a["name"].asString();
  out.gridId = a["gridId"].asBigIntString();
  out.clientVersion = static_cast<int>(a["clientVersion"].asInt64());
  out.bytes = std::move(*bytes);
  out.sizeBytes = static_cast<int>(a["sizeBytes"].asInt64());
  out.tickIntervalMs = static_cast<int>(a["tickIntervalMs"].asInt64());
  out.capabilitySummary = std::move(*summary);
  out.capabilityHash = a["capabilityHash"].asString();
  out.abiVersion = static_cast<int>(abi);
  return {};
}

}  // namespace

std::optional<ExecClientCapabilitySummary> parseExecClientCapabilitySummary(std::string_view json) {
  const graphql::Json doc = graphql::Json::parse(json);
  if (!doc.isObject() || !doc["hostFunctions"].isArray()) return std::nullopt;
  ExecClientCapabilitySummary summary;
  bool named = true;
  doc["hostFunctions"].forEach([&](graphql::Json v) {
    if (v.isString()) {
      summary.hostFunctions.push_back(v.asString());
    } else {
      named = false;
    }
  });
  if (!named) return std::nullopt;
  summary.version = static_cast<int>(doc["version"].asInt64());
  summary.target = doc["target"].asString();
  appendStrings(doc["imports"], summary.imports);
  appendStrings(doc["capabilityGroups"], summary.capabilityGroups);
  appendStrings(doc["presentationHooks"], summary.presentationHooks);
  appendStrings(doc["exportedFunctions"], summary.exportedFunctions);
  return summary;
}

graphql::Json ExecAPI::modClientBuild(std::string appId, const ExecCrate& crate) const {
  return exec(gen::exec::kExecModClientBuildIsolatedDocument, "execModClientBuild", modBuildVariables(appId, crate),
              gen::exec::kExecModClientBuildOperationName);
}

void ExecAPI::modClientBuildAsync(std::string appId, const ExecCrate& crate, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModClientBuildIsolatedDocument, "execModClientBuild", modBuildVariables(appId, crate),
            gen::exec::kExecModClientBuildOperationName, std::move(done));
}

graphql::Json ExecAPI::modClientDeploy(std::string appId, std::string gridId, std::string name,
                                       std::string buildId) const {
  auto vars = modVariables(appId, gridId, name);
  vars["buildId"] = graphql::JVal(buildId);
  return exec(gen::exec::kExecModClientDeployIsolatedDocument, "execModClientDeploy", std::move(vars),
              gen::exec::kExecModClientDeployOperationName);
}

void ExecAPI::modClientDeployAsync(std::string appId, std::string gridId, std::string name, std::string buildId,
                                   graphql::GraphQLCallback done) const {
  auto vars = modVariables(appId, gridId, name);
  vars["buildId"] = graphql::JVal(buildId);
  execAsync(gen::exec::kExecModClientDeployIsolatedDocument, "execModClientDeploy", std::move(vars),
            gen::exec::kExecModClientDeployOperationName, std::move(done));
}

graphql::Json ExecAPI::modClientDelete(std::string appId, std::string gridId, std::string name) const {
  return exec(gen::exec::kExecModClientDeleteIsolatedDocument, "execModClientDelete",
              modVariables(appId, gridId, name), gen::exec::kExecModClientDeleteOperationName);
}

void ExecAPI::modClientDeleteAsync(std::string appId, std::string gridId, std::string name,
                                   graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModClientDeleteIsolatedDocument, "execModClientDelete",
            modVariables(appId, gridId, name), gen::exec::kExecModClientDeleteOperationName, std::move(done));
}

graphql::Json ExecAPI::gridClientMods(std::string appId, std::string gridId) const {
  return exec(gen::exec::kExecGridClientModsIsolatedDocument, "execGridClientMods", gridVariables(appId, gridId),
              gen::exec::kExecGridClientModsOperationName);
}

void ExecAPI::gridClientModsAsync(std::string appId, std::string gridId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecGridClientModsIsolatedDocument, "execGridClientMods", gridVariables(appId, gridId),
            gen::exec::kExecGridClientModsOperationName, std::move(done));
}

graphql::Json ExecAPI::consentClientMod(std::string appId, std::string modId, std::string capabilityHash) const {
  return exec(gen::exec::kExecConsentClientModIsolatedDocument, "execConsentClientMod",
              consentVariables(appId, modId, capabilityHash), gen::exec::kExecConsentClientModOperationName);
}

void ExecAPI::consentClientModAsync(std::string appId, std::string modId, std::string capabilityHash,
                                    graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecConsentClientModIsolatedDocument, "execConsentClientMod",
            consentVariables(appId, modId, capabilityHash), gen::exec::kExecConsentClientModOperationName,
            std::move(done));
}

graphql::Json ExecAPI::trustAuthor(std::string appId, std::string gridId, std::string authorId,
                                   std::string capabilityHash) const {
  return exec(gen::exec::kExecTrustAuthorIsolatedDocument, "execTrustAuthor",
              trustVariables(appId, gridId, authorId, capabilityHash), gen::exec::kExecTrustAuthorOperationName);
}

void ExecAPI::trustAuthorAsync(std::string appId, std::string gridId, std::string authorId, std::string capabilityHash,
                               graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecTrustAuthorIsolatedDocument, "execTrustAuthor",
            trustVariables(appId, gridId, authorId, capabilityHash), gen::exec::kExecTrustAuthorOperationName,
            std::move(done));
}

graphql::Json ExecAPI::revokeClientModConsent(std::string appId, std::string modId) const {
  return exec(gen::exec::kExecRevokeClientModConsentIsolatedDocument, "execRevokeClientModConsent",
              modIdVariables(appId, modId), gen::exec::kExecRevokeClientModConsentOperationName);
}

void ExecAPI::revokeClientModConsentAsync(std::string appId, std::string modId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecRevokeClientModConsentIsolatedDocument, "execRevokeClientModConsent",
            modIdVariables(appId, modId), gen::exec::kExecRevokeClientModConsentOperationName, std::move(done));
}

graphql::Json ExecAPI::revokeAuthorTrust(std::string appId, std::string gridId, std::string authorId) const {
  return exec(gen::exec::kExecRevokeAuthorTrustIsolatedDocument, "execRevokeAuthorTrust",
              authorVariables(appId, gridId, authorId), gen::exec::kExecRevokeAuthorTrustOperationName);
}

void ExecAPI::revokeAuthorTrustAsync(std::string appId, std::string gridId, std::string authorId,
                                     graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecRevokeAuthorTrustIsolatedDocument, "execRevokeAuthorTrust",
            authorVariables(appId, gridId, authorId), gen::exec::kExecRevokeAuthorTrustOperationName, std::move(done));
}

graphql::Json ExecAPI::modClientArtifact(std::string appId, std::string modId) const {
  return exec(gen::exec::kExecModClientArtifactIsolatedDocument, "execModClientArtifact",
              modIdVariables(appId, modId), gen::exec::kExecModClientArtifactOperationName);
}

void ExecAPI::modClientArtifactAsync(std::string appId, std::string modId, graphql::GraphQLCallback done) const {
  execAsync(gen::exec::kExecModClientArtifactIsolatedDocument, "execModClientArtifact",
            modIdVariables(appId, modId), gen::exec::kExecModClientArtifactOperationName, std::move(done));
}

ExecModClientArtifactBytes ExecAPI::modClientArtifactBytes(std::string appId, std::string modId) const {
  ExecModClientArtifactBytes out;
  const std::string refusal = checkClientArtifact(modClientArtifact(std::move(appId), std::move(modId)), out);
  if (refusal.empty()) return out;
#ifndef CROWDY_NO_EXCEPTIONS
  throw graphql::CrowdyProtocolError(refusal);
#else
  return {};
#endif
}

void ExecAPI::modClientArtifactBytesAsync(std::string appId, std::string modId,
                                          ExecModClientArtifactBytesCallback done) const {
  modClientArtifactAsync(std::move(appId), std::move(modId), [done = std::move(done)](graphql::GraphQLOutcome out) {
    ExecModClientArtifactBytes decoded;
    if (out.ok()) {
      std::string refusal = checkClientArtifact(out.data, decoded);
      if (!refusal.empty()) {
        decoded = {};
        out.status = Errc::Malformed;
        out.kind = graphql::GraphQLErrorKind::Protocol;
        out.errorMessage = std::move(refusal);
      }
    }
    done(std::move(out), std::move(decoded));
  });
}

graphql::JVal ExecAPI::deployVariables(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                                       const std::string& buildId) {
  graphql::JVal manifest;
  manifest["root"] = graphql::JVal(root);
  graphql::JObject typeMap;
  graphql::JArray artifacts;
  std::set<std::string> seen;
  for (const auto& t : types) {
    graphql::JVal spec = t.extra.isObject() ? t.extra : graphql::JVal(graphql::JObject{});
    spec["kind"] = graphql::JVal(t.kind);
    if (!t.parent.empty()) spec["parent"] = graphql::JVal(t.parent);
    if (t.client) spec["client"] = graphql::JVal(true);
    if (!t.calls.empty()) {
      graphql::JArray calls;
      for (const auto& c : t.calls) calls.emplace_back(c);
      spec["calls"] = graphql::JVal(std::move(calls));
    }
    // A module of the build: the platform holds it, and fills in its digest.
    if (t.wasm.empty() && !t.crate.empty()) {
      spec["crate"] = graphql::JVal(t.crate);
      typeMap.emplace(t.name, std::move(spec));
      continue;
    }
    const std::string digest = execSha256Hex(t.wasm);
    spec["digest"] = graphql::JVal(digest);
    typeMap.emplace(t.name, std::move(spec));
    if (seen.insert(digest).second) {
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(t.wasm.data());
      artifacts.push_back(graphql::JVal::object(
          {{"digest", graphql::JVal(digest)},
           {"wasmBase64", graphql::JVal(core::base64Encode(Bytes(bytes, t.wasm.size())))}}));
    }
  }
  manifest["types"] = graphql::JVal(std::move(typeMap));
  graphql::JVal vars;
  vars["input"]["appId"] = graphql::JVal(appId);
  vars["input"]["manifestJson"] = graphql::JVal(manifest.dump());
  vars["input"]["artifacts"] = graphql::JVal(std::move(artifacts));
  if (!buildId.empty()) vars["input"]["buildId"] = graphql::JVal(buildId);
  return vars;
}

}  // namespace crowdy::domains
