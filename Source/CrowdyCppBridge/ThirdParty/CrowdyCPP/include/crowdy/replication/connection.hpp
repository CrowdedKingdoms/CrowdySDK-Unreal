#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "crowdy/core/clock.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/logger.hpp"
#include "crowdy/core/spsc.hpp"
#include "crowdy/replication/session_provider.hpp"
#include "crowdy/replication/types.hpp"
#include "crowdy/replication/udp_socket.hpp"

/// Native UDP replication client — the high-performance replacement for
/// CrowdyJS's GraphQL UDP proxy path. Implements the public Replication API
/// (https://docs.crowdedkingdoms.com/replication-api/intro):
///
///  - connect(): server assignment (installs the session server-side via the
///    Game API), socket setup, session-ready wait
///  - signed spatial sends + channel publish + heartbeats (zero steady-state
///    allocation; payloads are written straight into a pooled send buffer)
///  - receive path: bundle unpack, HMAC verification, typed dispatch through
///    a lock-free ring to the poll() thread
///  - lifecycle: proactive token refresh, verified COMMAND_RECONNECT
///    reassignment, TOKEN_EXPIRED recovery, optional silent-drop watchdog
///
/// Threading: by default the client owns one network thread and you call
/// poll() from your game thread. With Config::manualPump the SDK spawns no
/// threads: call pump() from your network thread (or tick) and poll() from
/// the game thread. Callbacks always fire on the poll() caller.
namespace crowdy::replication {

class Connection {
 public:
  Connection(Config config, std::shared_ptr<ISessionProvider> provider,
             const core::ICrypto& crypto,
             const core::IClock& clock = core::systemClock(),
             const core::ILogger& logger = core::defaultLogger());
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /// Assign a server and open the socket. Blocking (one HTTP round trip).
  /// After it returns the state is Connecting until the session-ready wait
  /// elapses, then Connected (status callback fires from poll()).
  Status connect();

  /// Stop threads, close the socket, state -> Closed.
  void disconnect();

  ConnState state() const { return state_.load(std::memory_order_acquire); }

  /// Replace the handler set (call before connect() or between polls).
  void setHandlers(Handlers handlers);

  /// Rotate token material after an external refresh (the client also
  /// refreshes proactively through the session provider).
  void setToken(const TokenInfo& token);

  /// Ask for a fresh server assignment on the next poll, as COMMAND_RECONNECT
  /// and the silence watchdog do.
  ///
  /// Needed because assignment is fetched through the API client, so moving that
  /// client to another datacenter without re-assigning would leave the UDP
  /// session on a Buddy in the datacenter the app is NOT served from — gameplay
  /// writes crossing a WAN, which reads as "the system is slow" rather than as a
  /// routing error.
  void requestReassignment() {
    reconnectRequested_.store(true, std::memory_order_release);
  }

  /// Stable lifecycle snapshot used by CrowdyClient's gameplay-token
  /// rotation. Handlers are copied so reconnecting the same native endpoint
  /// cannot lose or duplicate subscriptions.
  struct Snapshot {
    Config config;
    Handlers handlers;
    ConnState state = ConnState::Idle;
    Assignment endpoint;
  };
  Snapshot snapshot() const;

  // ----- Sends (thread-safe; never throw; return the sequence number) -------

  Result<std::uint8_t> sendActorUpdate(const SpatialSend& p) {
    return sendLongSpatial(wire::MessageType::ActorUpdateRequest, p);
  }
  Result<std::uint8_t> sendVoxelUpdate(const wire::ChunkCoord& chunk, const core::ActorUuid& uuid,
                                       std::int16_t x, std::int16_t y, std::int16_t z,
                                       std::int16_t voxelType, Bytes voxelState,
                                       std::uint8_t distance = 8,
                                       wire::DecayRate decay = wire::DecayRate::None);
  Result<std::uint8_t> sendAudio(const SpatialSend& p) {
    return sendLongSpatial(wire::MessageType::ClientAudioPacket, p);
  }
  Result<std::uint8_t> sendText(const SpatialSend& p) {
    return sendLongSpatial(wire::MessageType::ClientTextPacket, p);
  }
  Result<std::uint8_t> sendClientEvent(const wire::ChunkCoord& chunk, const core::ActorUuid& uuid,
                                       std::uint16_t eventType, Bytes state,
                                       std::uint8_t distance = 8,
                                       wire::DecayRate decay = wire::DecayRate::None);
  Result<std::uint8_t> sendGenericSpatial(const SpatialSend& p) {
    return sendLongSpatial(wire::MessageType::GenericSpatial1, p);
  }
  /// Direct actor-to-actor message: chunk + uuid address the TARGET actor.
  /// Fire-and-forget (no echo); embed sender identity in the payload if the
  /// recipient needs it.
  Result<std::uint8_t> sendSingleActorMessage(const wire::ChunkCoord& targetChunk,
                                              const core::ActorUuid& targetUuid, Bytes payload);
  /// Publish to a channel (requires membership + send_messages).
  Result<std::uint8_t> sendChannelMessage(std::int64_t channelId, const core::ActorUuid& uuid,
                                          Bytes payload);
  /// Idle keep-alive for your own actor (no fan-out). Send every ~2 s while
  /// idle so presence never lapses.
  Result<std::uint8_t> sendHeartbeat(const wire::ChunkCoord& chunk, const core::ActorUuid& uuid);

  // ----- AndWait correlation (the CrowdyJS *AndWait analog) -------------------

  /// Outcome of a waitForSequence.
  struct WaitOutcome {
    /// True when the send's self-echo notification arrived.
    bool acknowledged = false;
    /// Set when the server answered with a GENERIC_ERROR_MESSAGE instead.
    std::optional<wire::ErrorCode> error;
    /// Server epoch millis from the echo (when acknowledged).
    std::int64_t serverEpochMs = 0;
  };

  /// Wait (pumping/polling internally; handlers still fire normally) until a
  /// long-spatial notification with our `uuid` + `sequence` arrives (echo),
  /// a GENERIC_ERROR_MESSAGE with that sequence arrives, or the timeout
  /// elapses. Actor/voxel sends echo to the sender; audio/text/event sends
  /// only error-correlate — for those, treat a timeout as accepted.
  WaitOutcome waitForSequence(std::uint8_t sequence, const core::ActorUuid& uuid, int timeoutMs);

  /// Send an actor update and wait for its echo or error.
  WaitOutcome sendActorUpdateAndWait(const SpatialSend& p, int timeoutMs = 5000);
  /// Send a voxel update and wait for its echo or error.
  WaitOutcome sendVoxelUpdateAndWait(const wire::ChunkCoord& chunk, const core::ActorUuid& uuid,
                                     std::int16_t x, std::int16_t y, std::int16_t z,
                                     std::int16_t voxelType, Bytes voxelState,
                                     std::uint8_t distance = 8,
                                     wire::DecayRate decay = wire::DecayRate::None,
                                     int timeoutMs = 5000);
  /// Send a text packet and wait for a correlated error (no echo exists; a
  /// clean timeout means accepted).
  WaitOutcome sendTextAndWait(const SpatialSend& p, int timeoutMs = 1500);

  // ----- Receive-side --------------------------------------------------------

  /// Drain pending notifications and dispatch them to the handlers on the
  /// calling thread. Returns the number of events dispatched.
  std::size_t poll(std::size_t maxEvents = SIZE_MAX);

  /// Manual-pump mode only: perform network work (receive datagrams, run
  /// lifecycle housekeeping) without blocking longer than timeoutMs.
  /// Returns the number of datagrams processed.
  std::size_t pump(int timeoutMs = 0);

  // ----- Introspection --------------------------------------------------------
  /// Cumulative traffic/health counters since connect(). All counters only
  /// grow (no built-in rate windows, keeping the hot paths allocation-free):
  /// callers wanting rates should snapshot stats() periodically and diff.
  /// Byte counters measure real wire bytes (full datagrams, framing + HMAC
  /// included); per-type counters are indexed by the raw wire::MessageType
  /// opcode (e.g. messagesSentByType[static_cast<std::uint8_t>(
  /// wire::MessageType::ActorUpdateRequest)]).
  ///
  /// THESE ARE A LOCAL DIAGNOSTIC, NOT YOUR BILL. Do not reconcile them against
  /// an invoice; they will not agree, and they are not meant to:
  ///
  ///   - Billing counts EGRESS ONLY. `bytesSent` here is traffic leaving the
  ///     CLIENT, which is ingress to the platform and is not counted toward
  ///     Aggregate Data Volume at all. The billed figure corresponds more nearly
  ///     to `bytesReceived`.
  ///   - The platform measures at ITS network interface, so its count includes
  ///     the IP and UDP headers of each datagram. This counts the datagram
  ///     payload, which excludes them.
  ///   - A datagram lost in flight is counted here and not there, or vice versa,
  ///     depending on which side of the loss you are standing on.
  ///
  /// Use these for rate limiting, backpressure tuning and diagnosing a chatty
  /// client. For what you are actually charged, read the usage API
  /// (admin::appUsageSummary) or the account's billing pages.
  struct Stats {
    std::uint64_t datagramsSent = 0;
    std::uint64_t datagramsReceived = 0;
    std::uint64_t messagesSent = 0;
    std::uint64_t messagesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    /// Sends the kernel could not accept because its buffer was full. The
    /// datagram was not transmitted and the socket is healthy: this counter
    /// rising means the client is outrunning the send buffer, not that sends
    /// are failing. Callers that requeue should expect it to move under load.
    std::uint64_t sendsDeferred = 0;
    /// Sends that failed for a genuine socket fault. Unlike sendsDeferred,
    /// this one rising is a problem.
    std::uint64_t sendsFailed = 0;
    std::uint64_t hmacFailures = 0;
    std::uint64_t malformed = 0;
    std::uint64_t ringDropped = 0;
    std::uint64_t reconnects = 0;
    std::int64_t lastServerEpochMs = 0;
    /// Messages sent, by wire opcode (actor/voxel/audio/text/event/channel/
    /// heartbeat requests).
    std::array<std::uint64_t, 256> messagesSentByType{};
    /// Messages received, by wire opcode (notifications, errors, channel
    /// deliveries) after unbundling.
    std::array<std::uint64_t, 256> messagesReceivedByType{};
  };
  Stats stats() const;

  /// Current assigned endpoint. Prefer assignmentSnapshot() when reading
  /// concurrently with reconnect housekeeping.
  const Assignment& assignment() const { return assignment_; }
  Assignment assignmentSnapshot() const {
    std::lock_guard lock(assignmentMutex_);
    return assignment_;
  }

 private:
  // One parsed inbound message, copied off the receive buffer for the ring.
  // Assignment copies only the used bytes, not the whole buffer.
  struct Event {
    enum class Kind : std::uint8_t { Spatial, Channel, Error, Status } kind = Kind::Spatial;
    ConnState statusValue = ConnState::Idle;
    std::uint16_t len = 0;
    // Raw wire bytes of a single (unbundled) message.
    std::uint8_t data[wire::kMaxDatagramSize];

    Event() = default;
    Event(const Event& other) { *this = other; }
    Event& operator=(const Event& other) {
      kind = other.kind;
      statusValue = other.statusValue;
      len = other.len;
      if (len > 0) std::memcpy(data, other.data, len);
      return *this;
    }
    Event(Event&& other) noexcept { *this = other; }
    Event& operator=(Event&& other) noexcept { return *this = static_cast<const Event&>(other); }
  };

  // Everything the current token owns, read under one lock. Signing needs all
  // three together, and taking them separately could straddle a rotation.
  struct Credentials {
    wire::Token64 token;
    std::shared_ptr<const core::IMac> mac;
    std::int64_t gameTokenId = 0;
  };
  Credentials credentials() const;

  Result<std::uint8_t> sendLongSpatial(wire::MessageType type, const SpatialSend& p);
  Status transmit(const std::uint8_t* data, std::size_t len);
  void netLoop();
  std::size_t receiveBatch(int timeoutMs);
  void handleDatagram(Bytes datagram);
  void housekeeping();
  void setState(ConnState next);
  Status doAssign();
  // Claimed before the datagram is encoded, so a send that is deferred or
  // fails still consumes a sequence number. That is harmless and deliberate:
  // sequences are correlation, not idempotency, they wrap at 256, and rolling
  // the counter back would hand the same number to a concurrent sender.
  std::uint8_t nextSequence() { return sequence_.fetch_add(1, std::memory_order_relaxed); }
  wire::Token64 tokenSnapshot() const;

  Config config_;
  std::shared_ptr<ISessionProvider> provider_;
  const core::ICrypto& crypto_;
  const core::IClock& clock_;
  const core::ILogger& logger_;

  UdpSocket socket_;
  mutable std::mutex assignmentMutex_;
  Assignment assignment_;

  mutable std::mutex tokenMutex_;
  TokenInfo token_;
  wire::Token64 token64_{};
  /// Pre-keyed MAC for token64_, rebuilt on rotation. Null when the injected
  /// crypto provider offers no pre-keyed form, which is not an error.
  std::shared_ptr<const core::IMac> mac_;

  Handlers handlers_;
  mutable std::mutex handlersMutex_;

  core::SpscRing<Event> ring_;
  std::atomic<ConnState> state_{ConnState::Idle};
  std::atomic<std::uint8_t> sequence_{0};
  std::atomic<bool> running_{false};
  std::thread netThread_;

  std::int64_t readyAtMs_ = 0;
  std::int64_t lastRecvMs_ = 0;
  std::int64_t lastSendMs_ = 0;
  std::atomic<bool> reconnectRequested_{false};

  // One in-flight AndWait at a time (poll-thread only).
  struct PendingWait {
    bool active = false;
    std::uint8_t sequence = 0;
    core::ActorUuid uuid{};
    WaitOutcome outcome;
    bool done = false;
  };
  PendingWait pendingWait_;

  mutable std::mutex statsMutex_;
  Stats stats_;
};

/// Owns Connections and bridges them to a CrowdyClient (or a custom session
/// provider). One Connection per app.
class ReplicationClient {
 public:
  struct ConnectResult {
    std::shared_ptr<Connection> connection;
    Status status;

    bool ok() const { return status.ok() && connection != nullptr; }
  };

  ReplicationClient(std::shared_ptr<ISessionProvider> provider,
                    const core::ICrypto& crypto)
      : provider_(std::move(provider)), crypto_(crypto) {}

  /// Create and connect a Connection while preserving the initial assignment
  /// or socket failure. The initial token is required for UDP signing.
  ConnectResult connectWithStatus(const Config& config,
                                  Handlers handlers = {});

  /// Compatibility convenience. Prefer connectWithStatus() when startup
  /// failures must be reported directly; this still leaves the Connection in
  /// Failed state when the initial connect did not succeed.
  std::shared_ptr<Connection> connect(const Config& config, Handlers handlers = {});

  /// Most recently created connection, when still alive. CrowdyClient uses
  /// this to rotate an active gameplay token without owning user connection
  /// handles or introducing the browser UDP-proxy lifecycle.
  std::shared_ptr<Connection> activeConnection() const {
    std::lock_guard lock(connectionMutex_);
    return activeConnection_.lock();
  }

 private:
  std::shared_ptr<ISessionProvider> provider_;
  const core::ICrypto& crypto_;
  mutable std::mutex connectionMutex_;
  std::weak_ptr<Connection> activeConnection_;
};

}  // namespace crowdy::replication
